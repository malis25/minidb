#include "database_api.h"
#include "minidb/storage.h"
#include "server_options.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        std::random_device random;
        for (int attempt = 0; attempt < 10; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("minidb-http-tests-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(random()));
            if (std::filesystem::create_directory(path_)) {
                return;
            }
        }
        throw std::runtime_error("Could not create a unique HTTP test directory.");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

class DatabaseApiTests : public ::testing::Test {
  protected:
    DatabaseApiTests()
        : database_(std::make_unique<minidb::SegmentedStorage>(temporary_directory_.path(),
                                                               "http-test", 1024 * 1024)),
          api_(database_) {}

    void SetUp() override {
        api_.register_routes(server_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        ASSERT_GT(port_, 0);
        server_thread_ = std::thread([this] { server_.listen_after_bind(); });

        client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
        client_->set_connection_timeout(0, 500000);
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (client_->Get("/health")) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        FAIL() << "HTTP test server did not become ready.";
    }

    void TearDown() override {
        server_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    TemporaryDirectory temporary_directory_;
    minidb::Database database_;
    DatabaseApi api_;
    httplib::Server server_;
    int port_ = 0;
    std::thread server_thread_;
    std::unique_ptr<httplib::Client> client_;
};

} // namespace

TEST_F(DatabaseApiTests, HealthEndpointReturnsStatus) {
    const auto response = client_->Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(response->body), (nlohmann::json{{"status", "ok"}}));
}

TEST_F(DatabaseApiTests, CreatesSequentialNumericIdsAndReadsObjects) {
    const auto first =
        client_->Post("/objects", R"({"username":"ada","friends":[12,18]})", "application/json");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->status, httplib::StatusCode::Created_201);
    EXPECT_EQ(
        nlohmann::json::parse(first->body),
        (nlohmann::json{{"id", 0}, {"object", {{"username", "ada"}, {"friends", {12, 18}}}}}));

    const auto second = client_->Post("/objects", R"({"username":"grace"})", "application/json");
    ASSERT_TRUE(second);
    EXPECT_EQ(nlohmann::json::parse(second->body)["id"], 1);

    const auto fetched = client_->Get("/objects/0");
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(fetched->body)["object"]["username"], "ada");
}

TEST_F(DatabaseApiTests, ReplacesAndDeletesByNumericId) {
    const auto created = client_->Post("/objects", R"({"username":"ada"})", "application/json");
    ASSERT_TRUE(created);

    const auto updated = client_->Put("/objects/0", R"({"username":"grace"})", "application/json");
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(updated->body)["object"]["username"], "grace");

    const auto deleted = client_->Delete("/objects/0");
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->status, httplib::StatusCode::OK_200);

    const auto missing = client_->Get("/objects/0");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, httplib::StatusCode::NotFound_404);
}

TEST_F(DatabaseApiTests, RejectsInvalidObjectsAndIds) {
    const auto invalid_json = client_->Post("/objects", "{broken", "application/json");
    ASSERT_TRUE(invalid_json);
    EXPECT_EQ(invalid_json->status, httplib::StatusCode::BadRequest_400);

    const auto scalar = client_->Post("/objects", R"("not an object")", "application/json");
    ASSERT_TRUE(scalar);
    EXPECT_EQ(scalar->status, httplib::StatusCode::BadRequest_400);

    const auto invalid_id = client_->Get("/objects/not-a-number");
    ASSERT_TRUE(invalid_id);
    EXPECT_EQ(invalid_id->status, httplib::StatusCode::BadRequest_400);

    const auto missing_update =
        client_->Put("/objects/99", R"({"username":"ada"})", "application/json");
    ASSERT_TRUE(missing_update);
    EXPECT_EQ(missing_update->status, httplib::StatusCode::NotFound_404);
}

TEST_F(DatabaseApiTests, ListsNumericIdsInStableOrder) {
    ASSERT_TRUE(client_->Post("/objects", R"({"name":"zero"})", "application/json"));
    ASSERT_TRUE(client_->Post("/objects", R"({"name":"one"})", "application/json"));
    ASSERT_TRUE(client_->Post("/objects", R"({"name":"two"})", "application/json"));
    ASSERT_TRUE(client_->Delete("/objects/1"));

    const auto response = client_->Get("/objects");
    ASSERT_TRUE(response);
    const auto body = nlohmann::json::parse(response->body);
    EXPECT_EQ(body["count"], 2);
    EXPECT_EQ(body["ids"], (nlohmann::json::array({0, 2})));
}

TEST_F(DatabaseApiTests, FindsEveryObjectMatchingTypedPropertyValue) {
    ASSERT_TRUE(client_->Post("/objects", R"({"email":"same@example.test","active":true})",
                              "application/json"));
    ASSERT_TRUE(client_->Post("/objects", R"({"email":"other@example.test","active":true})",
                              "application/json"));
    ASSERT_TRUE(client_->Post("/objects", R"({"email":"same@example.test","active":false})",
                              "application/json"));

    const auto response = client_->Post(
        "/objects/query", R"({"field":"email","equals":"same@example.test"})", "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, httplib::StatusCode::OK_200);
    const auto body = nlohmann::json::parse(response->body);
    ASSERT_EQ(body["count"], 2);
    EXPECT_EQ(body["results"][0]["id"], 0);
    EXPECT_EQ(body["results"][1]["id"], 2);

    const auto bool_response =
        client_->Post("/objects/query", R"({"field":"active","equals":true})", "application/json");
    ASSERT_TRUE(bool_response);
    EXPECT_EQ(nlohmann::json::parse(bool_response->body)["count"], 2);
}

TEST_F(DatabaseApiTests, QueryResultsFollowUpdatesAndDeletes) {
    ASSERT_TRUE(client_->Post("/objects", R"({"username":"ada"})", "application/json"));
    ASSERT_TRUE(client_->Put("/objects/0", R"({"username":"grace"})", "application/json"));

    const auto old_value = client_->Post("/objects/query", R"({"field":"username","equals":"ada"})",
                                         "application/json");
    ASSERT_TRUE(old_value);
    EXPECT_EQ(nlohmann::json::parse(old_value->body)["count"], 0);

    const auto new_value = client_->Post(
        "/objects/query", R"({"field":"username","equals":"grace"})", "application/json");
    ASSERT_TRUE(new_value);
    EXPECT_EQ(nlohmann::json::parse(new_value->body)["count"], 1);

    ASSERT_TRUE(client_->Delete("/objects/0"));
    const auto after_delete = client_->Post(
        "/objects/query", R"({"field":"username","equals":"grace"})", "application/json");
    ASSERT_TRUE(after_delete);
    EXPECT_EQ(nlohmann::json::parse(after_delete->body)["count"], 0);
}

TEST_F(DatabaseApiTests, RejectsMalformedPropertyQueries) {
    const auto missing_field =
        client_->Post("/objects/query", R"({"equals":"ada"})", "application/json");
    ASSERT_TRUE(missing_field);
    EXPECT_EQ(missing_field->status, httplib::StatusCode::BadRequest_400);

    const auto malformed_json = client_->Post("/objects/query", "{broken", "application/json");
    ASSERT_TRUE(malformed_json);
    EXPECT_EQ(malformed_json->status, httplib::StatusCode::BadRequest_400);
}

TEST(ServerOptionsTests, ParsesRequiredMaximumFileSize) {
    char program[] = "minidb";
    char directory_flag[] = "--db-dir";
    char directory[] = "data";
    char file_flag[] = "--db-file";
    char filename[] = "users";
    char size_flag[] = "--max-file-size";
    char size[] = "1048576";
    char* arguments[] = {program, directory_flag, directory, file_flag, filename, size_flag, size};

    const auto options = parse_server_options(static_cast<int>(std::size(arguments)), arguments);

    ASSERT_TRUE(options);
    EXPECT_EQ(options->maximum_file_size, 1048576);
}

TEST(ServerOptionsTests, RejectsMissingOrInvalidMaximumFileSize) {
    char program[] = "minidb";
    char directory_flag[] = "--db-dir";
    char directory[] = "data";
    char file_flag[] = "--db-file";
    char filename[] = "users";
    char* incomplete[] = {program, directory_flag, directory, file_flag, filename};
    EXPECT_THROW(parse_server_options(static_cast<int>(std::size(incomplete)), incomplete),
                 std::runtime_error);

    char size_flag[] = "--max-file-size";
    char invalid_size[] = "0";
    char* invalid[] = {program,  directory_flag, directory,   file_flag,
                       filename, size_flag,      invalid_size};
    EXPECT_THROW(parse_server_options(static_cast<int>(std::size(invalid)), invalid),
                 std::runtime_error);
}
