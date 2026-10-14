#include "http_test_support.h"
#include "server_options.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

using test_support::TemporaryDirectory;

TEST_F(DatabaseApiTests, HealthEndpointReturnsStatus) {
    const auto response = client_->Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, httplib::StatusCode::OK_200);
    EXPECT_EQ(nlohmann::json::parse(response->body), (nlohmann::json{{"status", "ok"}}));
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

TEST(DatabaseCatalogTests, RejectsConcurrentUseOfTheSameDatabaseDirectory) {
    TemporaryDirectory temporary_directory("minidb-http-tests-");
    DatabaseCatalog first(temporary_directory.path());

    EXPECT_THROW(DatabaseCatalog second(temporary_directory.path()), std::system_error);
}

TEST(DatabaseCatalogTests, RestoresPersistedDatabaseAfterCatalogRestart) {
    TemporaryDirectory temporary_directory("minidb-http-tests-");
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
