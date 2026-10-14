#include "minidb/database.h"

#include <gtest/gtest.h>

#include <string>

TEST(SchemaMigrationTests, AdaptsValuesToSchemaWithDefaultsAndNull) {
    const minidb::ObjectSchema schema = {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"nickname", {{"type", "string"}, {"default", "ada"}}},
          {"profile",
           {{"type", "object"},
            {"properties", {{"city", {{"type", "string"}, {"default", "London"}}}}}}},
          {"friends", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
        {"required", nlohmann::json::array()}};
    minidb::Object value = {
        {"name", "Ada"}, {"legacy", true}, {"profile", minidb::Object::object()}};

    ASSERT_TRUE(minidb::adapt_value_to_schema(value, schema));
    EXPECT_EQ(value.at("name"), "Ada");
    EXPECT_EQ(value.at("nickname"), "ada"); // property default
    EXPECT_EQ(value.at("profile").at("city"), "London"); // nested, recursive
    ASSERT_TRUE(value.contains("friends"));
    EXPECT_TRUE(value.at("friends").is_null()); // no default: null
    EXPECT_FALSE(value.contains("legacy"));     // undeclared: removed

    std::string error;
    EXPECT_TRUE(minidb::validate_object_against_schema(value, schema, &error)) << error;
}

TEST(SchemaMigrationTests, AcceptsNullForDeclaredTypesButRejectsInvalidDefaults) {
    const minidb::ObjectSchema schema = {
        {"type", "object"},
        {"properties", {{"age", {{"type", "integer"}}}}},
        {"required", nlohmann::json::array({"age"})}};
    std::string error;
    // A required field may still be present with the explicit "unset" value.
    EXPECT_TRUE(minidb::validate_object_against_schema({{"age", nullptr}}, schema, &error)) << error;
    EXPECT_FALSE(minidb::validate_object_against_schema({{"age", "old"}}, schema, &error));

    const minidb::ObjectSchema invalid_default = {
        {"type", "object"},
        {"properties", {{"age", {{"type", "integer"}, {"default", "old"}}}}},
        {"required", nlohmann::json::array()}};
    EXPECT_FALSE(minidb::validate_schema_defaults(invalid_default, &error));
}
