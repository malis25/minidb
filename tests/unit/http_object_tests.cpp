#include "http_test_support.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

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
