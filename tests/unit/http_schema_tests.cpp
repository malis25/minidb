#include "http_test_support.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iterator>
#include <string>
#include <thread>

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
