#include "database_api.h"
#include "minidb/storage.h"
#include "server_options.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

const nlohmann::json test_database_schema = {
    {"type", "object"},
    {"properties",
     {{"name", {{"type", "string"}}},
      {"username", {{"type", "string"}}},
      {"email", {{"type", "string"}}},
      {"text", {{"type", "string"}}},
      {"group", {{"type", "string"}}},
      {"room", {{"type", "string"}}},
      {"active", {{"type", "boolean"}}},
      {"friends", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
      {"value", {{"type", "object"}, {"properties", nlohmann::json::object()}}}}},
    {"required", nlohmann::json::array()}};

nlohmann::json database_creation(std::string name, std::uint64_t limit = 10000) {
    return {{"name", std::move(name)},
            {"schema", test_database_schema},
            {"maximum_objects_per_segment", limit}};
}

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
    DatabaseApiTests() : catalog_(temporary_directory_.path()), api_(catalog_) {
        if (!catalog_.create("users", test_database_schema, 10000)) {
            throw std::runtime_error("Could not create the initial HTTP test database.");
        }
    }

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
    DatabaseCatalog catalog_;
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

TEST_F(DatabaseApiTests, RejectsCrossOriginStateChangingRequests) {
    const httplib::Headers cross_origin{{"Origin", "http://attacker.example"}};
    const auto rejected_create =
        client_->Post("/databases/users", cross_origin, R"({"name":"injected"})", "text/plain");
    ASSERT_TRUE(rejected_create) << httplib::to_string(rejected_create.error());
    EXPECT_EQ(rejected_create->status, httplib::StatusCode::Forbidden_403);

    const httplib::Headers same_origin{{"Origin", "http://127.0.0.1:" + std::to_string(port_)}};
    const auto created = client_->Post("/databases", same_origin,
                                       database_creation("origin-test").dump(), "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, httplib::StatusCode::Created_201);
    const auto ada = client_->Post("/databases/users", R"({"name":"Ada"})", "application/json");
    ASSERT_TRUE(ada);

    client_->set_default_headers(cross_origin);
    const auto rejected_update =
        client_->Put("/databases/users/0", R"({"name":"changed"})", "application/json");
    client_->set_default_headers({});
    ASSERT_TRUE(rejected_update) << httplib::to_string(rejected_update.error());
    EXPECT_EQ(rejected_update->status, httplib::StatusCode::Forbidden_403);

    const auto rejected_object_delete = client_->Delete("/databases/users/0", cross_origin);
    ASSERT_TRUE(rejected_object_delete);
    EXPECT_EQ(rejected_object_delete->status, httplib::StatusCode::Forbidden_403);

    const auto rejected_database_delete = client_->Delete("/databases/users", cross_origin);
    ASSERT_TRUE(rejected_database_delete);
    EXPECT_EQ(rejected_database_delete->status, httplib::StatusCode::Forbidden_403);

    const auto object = client_->Get("/databases/users/0");
    ASSERT_TRUE(object);
    EXPECT_EQ(nlohmann::json::parse(object->body)["object"]["name"], "Ada");
    const auto databases = client_->Get("/databases");
    ASSERT_TRUE(databases);
    EXPECT_EQ(nlohmann::json::parse(databases->body)["databases"],
              nlohmann::json::array({"origin-test", "users"}));
}

TEST_F(DatabaseApiTests, RequiresJsonContentTypeAndLimitsNestingDepth) {
    const auto plain_text = client_->Post("/databases/users", R"({"name":"Ada"})", "text/plain");
    ASSERT_TRUE(plain_text);
    EXPECT_EQ(plain_text->status, httplib::StatusCode::UnsupportedMediaType_415);

    const auto missing_content_type = client_->Post("/databases/users", R"({"name":"Ada"})", "");
    ASSERT_TRUE(missing_content_type);
    EXPECT_EQ(missing_content_type->status, httplib::StatusCode::UnsupportedMediaType_415);

    const auto valid_json =
        client_->Post("/databases/users", R"({"name":"Ada"})", "Application/JSON; charset=utf-8");
    ASSERT_TRUE(valid_json);
    ASSERT_EQ(valid_json->status, httplib::StatusCode::Created_201);

    std::string deeply_nested = R"({"value":)";
    deeply_nested.append(130, '[');
    deeply_nested += "0";
    deeply_nested.append(130, ']');
    deeply_nested += '}';
    const auto too_deep = client_->Post("/databases/users", deeply_nested, "application/json");
    ASSERT_TRUE(too_deep);
    EXPECT_EQ(too_deep->status, httplib::StatusCode::BadRequest_400);

    const auto ids = client_->Get("/databases/users");
    ASSERT_TRUE(ids);
    EXPECT_EQ(nlohmann::json::parse(ids->body)["ids"], nlohmann::json::array({0}));
}

TEST_F(DatabaseApiTests, CreatesListsAndIsolatesDatabases) {
    const auto created =
        client_->Post("/databases", database_creation("messages").dump(), "application/json");
    ASSERT_TRUE(created);
    EXPECT_EQ(created->status, httplib::StatusCode::Created_201);
    ASSERT_TRUE(
        client_->Post("/databases", database_creation("archived").dump(), "application/json"));
    const auto archived_named_database = client_->Get("/databases/archived/info");
    ASSERT_TRUE(archived_named_database);
    EXPECT_EQ(archived_named_database->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(archived_named_database->body)["name"], "archived");
    const auto archived_database_ids = client_->Get("/databases/archived");
    ASSERT_TRUE(archived_database_ids);
    EXPECT_EQ(nlohmann::json::parse(archived_database_ids->body)["ids"], nlohmann::json::array());
    const auto obsolete_objects_route = client_->Get("/databases/archived/objects");
    ASSERT_TRUE(obsolete_objects_route);
    EXPECT_EQ(obsolete_objects_route->status, httplib::StatusCode::BadRequest_400);

    const auto message =
        client_->Post("/databases/messages", R"({"text":"hello"})", "application/json");
    ASSERT_TRUE(message);
    EXPECT_EQ(nlohmann::json::parse(message->body)["id"], 0);

    const auto user = client_->Post("/databases/users", R"({"name":"Ada"})", "application/json");
    ASSERT_TRUE(user);
    EXPECT_EQ(nlohmann::json::parse(user->body)["id"], 0);

    const auto users = client_->Get("/databases/users");
    const auto messages = client_->Get("/databases/messages");
    ASSERT_TRUE(users);
    ASSERT_TRUE(messages);
    EXPECT_EQ(nlohmann::json::parse(users->body)["ids"], (nlohmann::json::array({0})));
    EXPECT_EQ(nlohmann::json::parse(messages->body)["ids"], (nlohmann::json::array({0})));

    const auto database_list = client_->Get("/databases");
    ASSERT_TRUE(database_list);
    EXPECT_EQ(nlohmann::json::parse(database_list->body)["databases"],
              (nlohmann::json::array({"archived", "messages", "users"})));
    const auto invalid_state = client_->Get("/databases?state=unknown");
    ASSERT_TRUE(invalid_state);
    EXPECT_EQ(invalid_state->status, httplib::StatusCode::BadRequest_400);
}

TEST_F(DatabaseApiTests, EnforcesAndPersistsStrictDatabaseSchemaAndObjectLimit) {
    const nlohmann::json schema = {
        {"type", "object"},
        {"properties",
         {{"username", {{"type", "string"}}},
          {"friends", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
        {"required", nlohmann::json::array({"username"})}};
    const auto created = client_->Post(
        "/databases",
        nlohmann::json{{"name", "profiles"}, {"schema", schema}, {"maximum_objects_per_segment", 1}}
            .dump(),
        "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, httplib::StatusCode::Created_201);

    for (const auto* invalid :
         {R"({"friends":[1]})", R"({"username":4})", R"({"username":"ada","admin":true})",
          R"({"username":"ada","friends":["bad"]})"}) {
        const auto rejected = client_->Post("/databases/profiles", invalid, "application/json");
        ASSERT_TRUE(rejected);
        EXPECT_EQ(rejected->status, httplib::StatusCode::BadRequest_400) << invalid;
    }
    const auto valid = client_->Post("/databases/profiles",
                                     R"({"username":"ada","friends":[12,18]})", "application/json");
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->status, httplib::StatusCode::Created_201);

    const auto schema_response = client_->Get("/databases/profiles/schema");
    ASSERT_TRUE(schema_response);
    const auto schema_body = nlohmann::json::parse(schema_response->body);
    EXPECT_EQ(schema_body["maximum_objects_per_segment"], 1);
    EXPECT_EQ(schema_body["schema"], schema);

    const auto second =
        client_->Post("/databases/profiles", R"({"username":"grace"})", "application/json");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, httplib::StatusCode::Created_201);
    EXPECT_EQ(nlohmann::json::parse(client_->Get("/databases/profiles/info")->body)["object_count"],
              2);
    EXPECT_EQ(
        std::distance(std::filesystem::directory_iterator(temporary_directory_.path() / "profiles"),
                      std::filesystem::directory_iterator{}),
        3);
}

TEST_F(DatabaseApiTests, SchemaUpdateAdaptsExistingObjectsToAddedFields) {
    ASSERT_TRUE(client_->Post("/databases/users", R"({"name":"Ada"})", "application/json"));

    nlohmann::json schema = test_database_schema;
    // `nickname` has a default, `verified` does not, and `username` becomes
    // required: adaptation fills them with the default or null.
    schema["properties"]["nickname"] = {{"type", "string"}, {"default", "ada"}};
    schema["properties"]["verified"] = {{"type", "boolean"}};
    schema["required"] = nlohmann::json::array({"username"});

    const auto updated = client_->Put("/databases/users/schema", schema.dump(), "application/json");
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->status, httplib::StatusCode::OK_200);
    const auto status = nlohmann::json::parse(updated->body)["migration"];
    EXPECT_FALSE(status["pending"]);
    EXPECT_EQ(status["total"], 1);

    const auto object = client_->Get("/databases/users/0");
    ASSERT_TRUE(object);
    const auto stored = nlohmann::json::parse(object->body)["object"];
    EXPECT_EQ(stored.at("name"), "Ada");
    EXPECT_EQ(stored.at("nickname"), "ada"); // filled from the property default
    ASSERT_TRUE(stored.contains("username"));
    EXPECT_TRUE(stored.at("username").is_null()); // required but no default: null
    ASSERT_TRUE(stored.contains("verified"));
    EXPECT_TRUE(stored.at("verified").is_null());

    // The new schema is enforced for writes made after the change.
    const auto rejected =
        client_->Post("/databases/users", R"({"extra":true})", "application/json");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, httplib::StatusCode::BadRequest_400);
}

TEST_F(DatabaseApiTests, SchemaUpdateRemovesUndeclaredFields) {
    ASSERT_TRUE(
        client_->Post("/databases/users", R"({"name":"Ada","username":"ada"})", "application/json"));
    const nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}}}}},
        {"required", nlohmann::json::array()}};

    const auto updated = client_->Put("/databases/users/schema", schema.dump(), "application/json");
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->status, httplib::StatusCode::OK_200);

    const auto object = client_->Get("/databases/users/0");
    ASSERT_TRUE(object);
    const auto stored = nlohmann::json::parse(object->body)["object"];
    EXPECT_EQ(stored.at("name"), "Ada");
    EXPECT_FALSE(stored.contains("username"));
}

TEST_F(DatabaseApiTests, SchemaUpdateRejectsInvalidFieldDefault) {
    nlohmann::json schema = test_database_schema;
    schema["properties"]["age"] = {{"type", "integer"}, {"default", "not-a-number"}};

    const auto rejected = client_->Put("/databases/users/schema", schema.dump(), "application/json");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, httplib::StatusCode::BadRequest_400);
    EXPECT_EQ(nlohmann::json::parse(client_->Get("/databases/users/schema")->body)["schema"],
              test_database_schema);
}

TEST_F(DatabaseApiTests, SchemaMigrationRunsInTheBackgroundWithoutBlockingRequests) {
    // Populate more objects than fit in one migration chunk, so the change must
    // be completed by the background worker rather than during the request.
    nlohmann::json operations = nlohmann::json::array();
    for (int index = 0; index < 600; ++index) {
        operations.push_back(
            {{"type", "create"}, {"object", {{"name", "user" + std::to_string(index)}}}});
    }
    const auto seeded = client_->Post("/databases/users/batch",
                                      nlohmann::json{{"operations", operations}}.dump(),
                                      "application/json");
    ASSERT_TRUE(seeded);
    ASSERT_EQ(seeded->status, httplib::StatusCode::OK_200);

    nlohmann::json schema = test_database_schema;
    schema["properties"]["active"] = {{"type", "boolean"}, {"default", true}};
    const auto updated = client_->Put("/databases/users/schema", schema.dump(), "application/json");
    ASSERT_TRUE(updated);
    // The schema is installed immediately and the response reports the work that
    // is still running, instead of holding the request for the whole migration.
    EXPECT_EQ(updated->status, httplib::StatusCode::Accepted_202);
    EXPECT_TRUE(nlohmann::json::parse(updated->body)["migration"]["pending"]);

    // The database stays available while the migration runs.
    const auto during = client_->Get("/databases/users/0");
    ASSERT_TRUE(during);
    EXPECT_EQ(during->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(during->body)["object"]["name"], "user0");

    bool pending = true;
    for (int attempt = 0; attempt < 500; ++attempt) {
        const auto info = client_->Get("/databases/users/info");
        ASSERT_TRUE(info);
        pending = nlohmann::json::parse(info->body)["migration"]["pending"];
        if (!pending) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(pending) << "schema migration did not finish";

    // Every object was adapted, including ones the first chunk did not reach.
    for (int index = 0; index < 600; index += 97) {
        const auto object = client_->Get("/databases/users/" + std::to_string(index));
        ASSERT_TRUE(object);
        ASSERT_EQ(object->status, httplib::StatusCode::OK_200);
        const auto stored = nlohmann::json::parse(object->body)["object"];
        ASSERT_TRUE(stored.contains("active"));
        EXPECT_EQ(stored.at("active"), true);
    }
}

TEST_F(DatabaseApiTests, ArchivesRestoresAndPurgesDatabasesOnRequest) {
    ASSERT_TRUE(client_->Post("/databases/users", R"({"name":"Ada"})", "application/json"));
    const auto archived = client_->Delete("/databases/users");
    ASSERT_TRUE(archived);
    EXPECT_EQ(archived->status, httplib::StatusCode::OK_200);
    EXPECT_FALSE(std::filesystem::exists(temporary_directory_.path() / "users"));
    EXPECT_TRUE(std::filesystem::exists(temporary_directory_.path() / ".archive" / "users"));

    const auto archived_list = client_->Get("/databases?state=archived");
    ASSERT_TRUE(archived_list);
    EXPECT_EQ(nlohmann::json::parse(archived_list->body)["databases"],
              (nlohmann::json::array({"users"})));

    const auto restored = client_->Post("/databases/users/restore", "", "application/json");
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->status, httplib::StatusCode::OK_200);
    const auto object = client_->Get("/databases/users/0");
    ASSERT_TRUE(object);
    EXPECT_EQ(nlohmann::json::parse(object->body)["object"]["name"], "Ada");

    const auto purged = client_->Delete("/databases/users?purge=true");
    ASSERT_TRUE(purged);
    EXPECT_EQ(purged->status, httplib::StatusCode::OK_200);
    EXPECT_FALSE(std::filesystem::exists(temporary_directory_.path() / "users"));
}

TEST_F(DatabaseApiTests, ArchivesRestoresAndPurgesDiskBackedDatabases) {
    // Regression test: a disk-backed database keeps its index files open, and an
    // open file inside the directory used to make the archive rename fail on
    // Windows (HTTP 500).
    auto creation = database_creation("indexed");
    creation["indexed_fields"] = {"username"};
    const auto created = client_->Post("/databases", creation.dump(), "application/json");
    ASSERT_TRUE(created) << httplib::to_string(created.error());
    ASSERT_EQ(created->status, httplib::StatusCode::Created_201);

    const auto ada =
        client_->Post("/databases/indexed", R"({"username":"ada"})", "application/json");
    ASSERT_TRUE(ada);
    ASSERT_EQ(ada->status, httplib::StatusCode::Created_201);

    const auto archived = client_->Delete("/databases/indexed");
    ASSERT_TRUE(archived);
    EXPECT_EQ(archived->status, httplib::StatusCode::OK_200);
    EXPECT_FALSE(std::filesystem::exists(temporary_directory_.path() / "indexed"));
    EXPECT_TRUE(std::filesystem::exists(temporary_directory_.path() / ".archive" / "indexed"));

    const auto restored = client_->Post("/databases/indexed/restore", "", "application/json");
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->status, httplib::StatusCode::OK_200);

    const auto query = client_->Post("/databases/indexed/query?limit=10",
                                     R"({"field":"username","equals":"ada"})", "application/json");
    ASSERT_TRUE(query);
    ASSERT_EQ(query->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(query->body)["count"], 1);

    const auto purged = client_->Delete("/databases/indexed?purge=true");
    ASSERT_TRUE(purged);
    EXPECT_EQ(purged->status, httplib::StatusCode::OK_200);
    EXPECT_FALSE(std::filesystem::exists(temporary_directory_.path() / "indexed"));
}

TEST_F(DatabaseApiTests, RejectsInvalidDatabaseNamesAndDuplicateCreation) {
    const auto invalid =
        client_->Post("/databases", database_creation("../users").dump(), "application/json");
    ASSERT_TRUE(invalid);
    EXPECT_EQ(invalid->status, httplib::StatusCode::BadRequest_400);

    const auto reserved =
        client_->Post("/databases", database_creation("con").dump(), "application/json");
    ASSERT_TRUE(reserved);
    EXPECT_EQ(reserved->status, httplib::StatusCode::BadRequest_400);

    const auto duplicate =
        client_->Post("/databases", database_creation("users").dump(), "application/json");
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate->status, httplib::StatusCode::Conflict_409);
}

TEST_F(DatabaseApiTests, PaginatesIdsAndQueryResults) {
    for (const auto* name : {"Ada", "Grace", "Alan"}) {
        const auto response = client_->Post("/databases/users",
                                            nlohmann::json{{"name", name}, {"group", "all"}}.dump(),
                                            "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, httplib::StatusCode::Created_201);
    }

    const auto ids = client_->Get("/databases/users?limit=2&offset=1");
    ASSERT_TRUE(ids);
    const auto id_page = nlohmann::json::parse(ids->body);
    EXPECT_EQ(id_page["ids"], (nlohmann::json::array({1, 2})));
    EXPECT_EQ(id_page["count"], 2);
    EXPECT_EQ(id_page["total"], 3);
    EXPECT_EQ(id_page["offset"], 1);

    const auto matches = client_->Post("/databases/users/query?limit=1&offset=1",
                                       R"({"field":"group","equals":"all"})", "application/json");
    ASSERT_TRUE(matches);
    const auto match_page = nlohmann::json::parse(matches->body);
    EXPECT_EQ(match_page["results"][0]["id"], 1);
    EXPECT_EQ(match_page["count"], 1);
    EXPECT_EQ(match_page["total"], 3);

    const auto invalid_page = client_->Get("/databases/users?limit=0");
    ASSERT_TRUE(invalid_page);
    EXPECT_EQ(invalid_page->status, httplib::StatusCode::BadRequest_400);
}

TEST_F(DatabaseApiTests, PaginatesIdAndQueryResultsWithStableCursors) {
    for (int index = 0; index < 5; ++index) {
        const auto response =
            client_->Post("/databases/users", R"({"room":"general"})", "application/json");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, httplib::StatusCode::Created_201);
    }

    const auto first_ids = client_->Get("/databases/users?limit=2");
    ASSERT_TRUE(first_ids);
    auto first_id_page = nlohmann::json::parse(first_ids->body);
    EXPECT_EQ(first_id_page["ids"], nlohmann::json::array({0, 1}));
    EXPECT_TRUE(first_id_page["has_more"]);
    const auto next_ids =
        client_->Get("/databases/users?limit=2&after_id=" + first_id_page["next_after_id"].dump());
    ASSERT_TRUE(next_ids);
    const auto next_id_page = nlohmann::json::parse(next_ids->body);
    EXPECT_EQ(next_id_page["ids"], nlohmann::json::array({2, 3}));
    EXPECT_TRUE(next_id_page["has_more"]);

    const auto first_query =
        client_->Post("/databases/users/query?limit=2", R"({"field":"room","equals":"general"})",
                      "application/json");
    ASSERT_TRUE(first_query);
    const auto first_query_page = nlohmann::json::parse(first_query->body);
    EXPECT_EQ(first_query_page["results"][0]["id"], 0);
    EXPECT_TRUE(first_query_page["has_more"]);
    const auto next_query = client_->Post(
        "/databases/users/query?limit=2&after_id=" + first_query_page["next_after_id"].dump(),
        R"({"field":"room","equals":"general"})", "application/json");
    ASSERT_TRUE(next_query);
    const auto next_query_page = nlohmann::json::parse(next_query->body);
    EXPECT_EQ(next_query_page["results"][0]["id"], 2);
    EXPECT_EQ(next_query_page["results"][1]["id"], 3);

    const auto invalid = client_->Get("/databases/users?limit=2&offset=1&after_id=2");
    ASSERT_TRUE(invalid);
    EXPECT_EQ(invalid->status, httplib::StatusCode::BadRequest_400);
}

TEST_F(DatabaseApiTests, AppliesDatabaseBatchAtomically) {
    const auto ada =
        client_->Post("/databases/users", R"({"name":"Ada","room":"general"})", "application/json");
    const auto grace = client_->Post("/databases/users", R"({"name":"Grace","room":"general"})",
                                     "application/json");
    ASSERT_TRUE(ada);
    ASSERT_TRUE(grace);

    const auto rejected = client_->Post(
        "/databases/users/batch",
        R"({"operations":[{"type":"create","object":{"name":"Alan"}},{"type":"delete","id":99}]})",
        "application/json");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, httplib::StatusCode::NotFound_404);
    EXPECT_EQ(nlohmann::json::parse(client_->Get("/databases/users")->body)["total"], 2);

    const auto applied = client_->Post(
        "/databases/users/batch",
        R"({"operations":[{"type":"create","object":{"name":"Alan","room":"random"}},{"type":"replace","id":0,"object":{"name":"Ada","room":"random"}},{"type":"delete","id":1}]})",
        "application/json");
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, httplib::StatusCode::OK_200);
    const auto result = nlohmann::json::parse(applied->body);
    EXPECT_EQ(result["applied"], 3);
    EXPECT_EQ(result["created_ids"], nlohmann::json::array({2}));
    EXPECT_EQ(nlohmann::json::parse(client_->Get("/databases/users/0")->body)["object"]["room"],
              "random");
    EXPECT_EQ(client_->Get("/databases/users/1")->status, httplib::StatusCode::NotFound_404);
    EXPECT_EQ(client_->Get("/databases/users/2")->status, httplib::StatusCode::OK_200);
}

TEST(DatabaseCatalogTests, RejectsConcurrentUseOfTheSameDatabaseDirectory) {
    TemporaryDirectory temporary_directory;
    DatabaseCatalog first(temporary_directory.path());

    EXPECT_THROW(DatabaseCatalog second(temporary_directory.path()), std::system_error);
}

TEST(DatabaseCatalogTests, RestoresPersistedDatabaseAfterCatalogRestart) {
    TemporaryDirectory temporary_directory;
    const auto create_catalog = [&] {
        return std::make_unique<DatabaseCatalog>(temporary_directory.path());
    };
    {
        auto catalog = create_catalog();
        ASSERT_TRUE(catalog->create("users", test_database_schema, 10000));
        const auto database = catalog->find_active("users");
        ASSERT_TRUE(database);
        {
            std::lock_guard lock(database->mutex);
            EXPECT_EQ(database->database.create({{"name", "Ada"}}), 0);
        }
        EXPECT_EQ(catalog->remove("users", false), DatabaseCatalog::RemoveResult::removed);
    }

    auto restored_catalog = create_catalog();
    EXPECT_TRUE(restored_catalog->active_names().empty());
    EXPECT_EQ(restored_catalog->archived_names(), (std::vector<std::string>{"users"}));
    EXPECT_EQ(restored_catalog->restore("users"), DatabaseCatalog::RestoreResult::restored);

    const auto restored_database = restored_catalog->find_active("users");
    ASSERT_TRUE(restored_database);
    std::lock_guard lock(restored_database->mutex);
    ASSERT_TRUE(restored_database->database.get(0));
    EXPECT_EQ(restored_database->database.get(0)->at("name"), "Ada");
}

TEST_F(DatabaseApiTests, CreatesSequentialNumericIdsAndReadsObjects) {
    const auto first = client_->Post("/databases/users", R"({"username":"ada","friends":[12,18]})",
                                     "application/json");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->status, httplib::StatusCode::Created_201);
    EXPECT_EQ(
        nlohmann::json::parse(first->body),
        (nlohmann::json{{"id", 0}, {"object", {{"username", "ada"}, {"friends", {12, 18}}}}}));

    const auto second =
        client_->Post("/databases/users", R"({"username":"grace"})", "application/json");
    ASSERT_TRUE(second);
    EXPECT_EQ(nlohmann::json::parse(second->body)["id"], 1);

    const auto fetched = client_->Get("/databases/users/0");
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(fetched->body)["object"]["username"], "ada");
}

TEST_F(DatabaseApiTests, ReplacesAndDeletesByNumericId) {
    const auto created =
        client_->Post("/databases/users", R"({"username":"ada"})", "application/json");
    ASSERT_TRUE(created);

    const auto updated =
        client_->Put("/databases/users/0", R"({"username":"grace"})", "application/json");
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(updated->body)["object"]["username"], "grace");

    const auto deleted = client_->Delete("/databases/users/0");
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->status, httplib::StatusCode::OK_200);

    const auto missing = client_->Get("/databases/users/0");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, httplib::StatusCode::NotFound_404);
}

TEST_F(DatabaseApiTests, RejectsInvalidObjectsAndIds) {
    const auto invalid_json = client_->Post("/databases/users", "{broken", "application/json");
    ASSERT_TRUE(invalid_json);
    EXPECT_EQ(invalid_json->status, httplib::StatusCode::BadRequest_400);

    const auto scalar = client_->Post("/databases/users", R"("not an object")", "application/json");
    ASSERT_TRUE(scalar);
    EXPECT_EQ(scalar->status, httplib::StatusCode::BadRequest_400);

    const auto invalid_id = client_->Get("/databases/users/not-a-number");
    ASSERT_TRUE(invalid_id);
    EXPECT_EQ(invalid_id->status, httplib::StatusCode::BadRequest_400);

    const auto missing_update =
        client_->Put("/databases/users/99", R"({"username":"ada"})", "application/json");
    ASSERT_TRUE(missing_update);
    EXPECT_EQ(missing_update->status, httplib::StatusCode::NotFound_404);
}

TEST_F(DatabaseApiTests, ListsNumericIdsInStableOrder) {
    ASSERT_TRUE(client_->Post("/databases/users", R"({"name":"zero"})", "application/json"));
    ASSERT_TRUE(client_->Post("/databases/users", R"({"name":"one"})", "application/json"));
    ASSERT_TRUE(client_->Post("/databases/users", R"({"name":"two"})", "application/json"));
    ASSERT_TRUE(client_->Delete("/databases/users/1"));

    const auto response = client_->Get("/databases/users");
    ASSERT_TRUE(response);
    const auto body = nlohmann::json::parse(response->body);
    EXPECT_EQ(body["count"], 2);
    EXPECT_EQ(body["ids"], (nlohmann::json::array({0, 2})));
}

TEST_F(DatabaseApiTests, FindsEveryObjectMatchingTypedPropertyValue) {
    ASSERT_TRUE(client_->Post("/databases/users", R"({"email":"same@example.test","active":true})",
                              "application/json"));
    ASSERT_TRUE(client_->Post("/databases/users", R"({"email":"other@example.test","active":true})",
                              "application/json"));
    ASSERT_TRUE(client_->Post("/databases/users", R"({"email":"same@example.test","active":false})",
                              "application/json"));

    const auto response =
        client_->Post("/databases/users/query?limit=10",
                      R"({"field":"email","equals":"same@example.test"})", "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, httplib::StatusCode::OK_200);
    const auto body = nlohmann::json::parse(response->body);
    ASSERT_EQ(body["count"], 2);
    EXPECT_EQ(body["results"][0]["id"], 0);
    EXPECT_EQ(body["results"][1]["id"], 2);

    const auto bool_response = client_->Post(
        "/databases/users/query", R"({"field":"active","equals":true})", "application/json");
    ASSERT_TRUE(bool_response);
    EXPECT_EQ(nlohmann::json::parse(bool_response->body)["count"], 2);
}

TEST_F(DatabaseApiTests, QueryResultsFollowUpdatesAndDeletes) {
    ASSERT_TRUE(client_->Post("/databases/users", R"({"username":"ada"})", "application/json"));
    ASSERT_TRUE(client_->Put("/databases/users/0", R"({"username":"grace"})", "application/json"));

    const auto old_value = client_->Post(
        "/databases/users/query", R"({"field":"username","equals":"ada"})", "application/json");
    ASSERT_TRUE(old_value);
    EXPECT_EQ(nlohmann::json::parse(old_value->body)["count"], 0);

    const auto new_value = client_->Post(
        "/databases/users/query", R"({"field":"username","equals":"grace"})", "application/json");
    ASSERT_TRUE(new_value);
    EXPECT_EQ(nlohmann::json::parse(new_value->body)["count"], 1);

    ASSERT_TRUE(client_->Delete("/databases/users/0"));
    const auto after_delete = client_->Post(
        "/databases/users/query", R"({"field":"username","equals":"grace"})", "application/json");
    ASSERT_TRUE(after_delete);
    EXPECT_EQ(nlohmann::json::parse(after_delete->body)["count"], 0);
}

TEST_F(DatabaseApiTests, RejectsMalformedPropertyQueries) {
    const auto missing_field =
        client_->Post("/databases/users/query", R"({"equals":"ada"})", "application/json");
    ASSERT_TRUE(missing_field);
    EXPECT_EQ(missing_field->status, httplib::StatusCode::BadRequest_400);

    const auto malformed_json =
        client_->Post("/databases/users/query", "{broken", "application/json");
    ASSERT_TRUE(malformed_json);
    EXPECT_EQ(malformed_json->status, httplib::StatusCode::BadRequest_400);
}

TEST_F(DatabaseApiTests, CreatesIndexedDatabasesAndServesIndexEndpoints) {
    auto creation = database_creation("indexed");
    creation["indexed_fields"] = {"username"};
    creation["cache_bytes"] = 4096;
    const auto created =
        client_->Post("/databases", creation.dump(), "application/json");
    ASSERT_TRUE(created) << httplib::to_string(created.error());
    ASSERT_EQ(created->status, httplib::StatusCode::Created_201);
    const auto created_body = nlohmann::json::parse(created->body);
    EXPECT_EQ(created_body["indexed_fields"], (nlohmann::json{"username"}));
    EXPECT_TRUE(created_body["disk_backed"]);

    const auto info = client_->Get("/databases/indexed/info");
    ASSERT_TRUE(info);
    ASSERT_EQ(info->status, httplib::StatusCode::OK_200);
    const auto info_body = nlohmann::json::parse(info->body);
    EXPECT_TRUE(info_body["disk_backed"]);
    EXPECT_EQ(info_body["indexed_fields"], (nlohmann::json{"username"}));

    const auto initial = client_->Get("/databases/indexed/indexes");
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(initial->body)["indexed_fields"],
              (nlohmann::json{"username"}));

    const auto ada = client_->Post("/databases/indexed", R"({"username":"ada"})",
                                   "application/json");
    ASSERT_TRUE(ada) << httplib::to_string(ada.error());
    EXPECT_EQ(ada->status, httplib::StatusCode::Created_201);

    const auto matches = client_->Post("/databases/indexed/query", R"({"field":"username","equals":"ada"})",
                                       "application/json");
    ASSERT_TRUE(matches);
    ASSERT_EQ(matches->status, httplib::StatusCode::OK_200);
    const auto matches_body = nlohmann::json::parse(matches->body);
    ASSERT_EQ(matches_body["total"], 1);
    EXPECT_EQ(matches_body["results"][0]["object"]["username"], "ada");

    const auto updated = client_->Put("/databases/indexed/indexes",
                                      R"({"indexed_fields":["username","email"]})",
                                      "application/json");
    ASSERT_TRUE(updated);
    ASSERT_EQ(updated->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(updated->body)["indexed_fields"],
              (nlohmann::json{"username", "email"}));

    const auto cleared = client_->Put("/databases/indexed/indexes", R"({"indexed_fields":[]})",
                                      "application/json");
    ASSERT_TRUE(cleared);
    ASSERT_EQ(cleared->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(cleared->body)["indexed_fields"], nlohmann::json::array());
}

TEST_F(DatabaseApiTests, RejectsMalformedIndexRequests) {
    const auto missing = client_->Put("/databases/users/indexes", R"({})", "application/json");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, httplib::StatusCode::BadRequest_400);

    const auto not_array = client_->Put("/databases/users/indexes", R"({"indexed_fields":"name"})",
                                        "application/json");
    ASSERT_TRUE(not_array);
    EXPECT_EQ(not_array->status, httplib::StatusCode::BadRequest_400);

    const auto empty_field =
        client_->Put("/databases/users/indexes", R"({"indexed_fields":[""]})", "application/json");
    ASSERT_TRUE(empty_field);
    EXPECT_EQ(empty_field->status, httplib::StatusCode::BadRequest_400);

    auto bad_creation = database_creation("bad");
    bad_creation["indexed_fields"] = "name";
    const auto bad_create =
        client_->Post("/databases", bad_creation.dump(), "application/json");
    ASSERT_TRUE(bad_create);
    EXPECT_EQ(bad_create->status, httplib::StatusCode::BadRequest_400);
}

TEST(ServerOptionsTests, ParsesDatabaseDirectoryWithoutStorageSizingOption) {
    char program[] = "minidb";
    char directory_flag[] = "--db-dir";
    char directory[] = "data";
    char* arguments[] = {program, directory_flag, directory};

    const auto options = parse_server_options(static_cast<int>(std::size(arguments)), arguments);

    ASSERT_TRUE(options);
    EXPECT_EQ(options->database_directory, "data");
}

TEST(ServerOptionsTests, RejectsMissingDirectoryAndRemovedByteLimitOption) {
    char program[] = "minidb";
    char directory_flag[] = "--db-dir";
    char directory[] = "data";
    char* incomplete[] = {program};
    EXPECT_THROW(parse_server_options(static_cast<int>(std::size(incomplete)), incomplete),
                 std::runtime_error);

    char size_flag[] = "--max-file-size";
    char size[] = "1024";
    char* invalid[] = {program, directory_flag, directory, size_flag, size};
    EXPECT_THROW(parse_server_options(static_cast<int>(std::size(invalid)), invalid),
                 std::runtime_error);
}
