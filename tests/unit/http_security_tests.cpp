#include "http_test_support.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>

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
