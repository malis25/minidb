#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace minidb {

inline constexpr int maximum_object_nesting_depth = 128;

// A document is a JSON object. Its fields can contain strings, integers,
// floating-point numbers, booleans, nulls, arrays, and nested objects.
using Object = nlohmann::json;
using ObjectSchema = nlohmann::json;

[[nodiscard]] inline bool validate_object_schema(const ObjectSchema& schema,
                                                 std::string* error = nullptr) {
    const auto invalid = [&](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    if (!schema.is_object() || !schema.contains("type") || schema["type"] != "object" ||
        !schema.contains("properties") || !schema["properties"].is_object()) {
        return invalid("Schema must define an object type and object properties.");
    }

    const auto validate_node = [&](const auto& self, const Object& node,
                                   const std::string& path) -> bool {
        if (!node.is_object() || !node.contains("type") || !node["type"].is_string()) {
            return invalid("Schema at '" + path + "' must define a string type.");
        }
        const auto type = node["type"].get<std::string>();
        if (type == "object") {
            if (!node.contains("properties") || !node["properties"].is_object()) {
                return invalid("Object schema at '" + path + "' must define properties.");
            }
            if (node.contains("additionalProperties") &&
                !node["additionalProperties"].is_boolean()) {
                return invalid("additionalProperties at '" + path + "' must be boolean.");
            }
            if (node.contains("required")) {
                if (!node["required"].is_array()) {
                    return invalid("Schema required at '" + path + "' must be an array.");
                }
                for (const auto& required : node["required"]) {
                    if (!required.is_string() ||
                        !node["properties"].contains(required.get<std::string>())) {
                        return invalid("Schema required fields at '" + path +
                                       "' must name declared properties.");
                    }
                }
            }
            for (auto property = node["properties"].begin(); property != node["properties"].end();
                 ++property) {
                if (!self(self, property.value(), path + "." + property.key())) {
                    return false;
                }
            }
            return true;
        }
        if (type == "array") {
            if (!node.contains("items") || !self(self, node["items"], path + "[]")) {
                return invalid("Array schema at '" + path + "' must define a valid items schema.");
            }
            return true;
        }
        if (type == "string" || type == "integer" || type == "number" || type == "boolean" ||
            type == "null") {
            return true;
        }
        return invalid("Unsupported schema type at '" + path + "': " + type);
    };
    return validate_node(validate_node, schema, "$");
}

[[nodiscard]] inline bool is_strict_object_schema(const ObjectSchema& schema) {
    const auto strict_node = [&](const auto& self, const Object& node) -> bool {
        if (node["type"] == "object") {
            if (node.value("additionalProperties", false)) {
                return false;
            }
            for (const auto& [name, property] : node["properties"].items()) {
                static_cast<void>(name);
                if (!self(self, property)) {
                    return false;
                }
            }
        } else if (node["type"] == "array") {
            return self(self, node["items"]);
        }
        return true;
    };
    return validate_object_schema(schema) && strict_node(strict_node, schema);
}

[[nodiscard]] inline bool validate_object_against_schema(const Object& object,
                                                         const ObjectSchema& schema,
                                                         std::string* error = nullptr) {
    if (!object.is_object()) {
        if (error != nullptr) {
            *error = "A database object must be a JSON object.";
        }
        return false;
    }
    if (!validate_object_schema(schema, error)) {
        return false;
    }
    const auto validate_value = [&](const auto& self, const Object& value, const Object& rule,
                                    const std::string& path) -> bool {
        const auto type = rule["type"].get<std::string>();
        const bool matches =
            (type == "object" && value.is_object()) || (type == "array" && value.is_array()) ||
            (type == "string" && value.is_string()) ||
            (type == "integer" && value.is_number_integer()) ||
            (type == "number" && value.is_number()) || (type == "boolean" && value.is_boolean()) ||
            (type == "null" && value.is_null());
        if (!matches) {
            if (error != nullptr) {
                *error = "Value at '" + path + "' must be of type " + type + ".";
            }
            return false;
        }
        if (type == "object") {
            const auto& properties = rule["properties"];
            const bool allow_additional = rule.value("additionalProperties", false);
            if (rule.contains("required")) {
                for (const auto& required : rule["required"]) {
                    const auto& name = required.get_ref<const std::string&>();
                    if (!value.contains(name)) {
                        if (error != nullptr) {
                            *error = "Required property is missing: " + path + "." + name;
                        }
                        return false;
                    }
                }
            }
            for (auto property = value.begin(); property != value.end(); ++property) {
                if (!properties.contains(property.key())) {
                    if (allow_additional) {
                        continue;
                    }
                    if (error != nullptr) {
                        *error =
                            "Property is not allowed by the schema: " + path + "." + property.key();
                    }
                    return false;
                }
                if (!self(self, property.value(), properties[property.key()],
                          path + "." + property.key())) {
                    return false;
                }
            }
            return true;
        }
        if (type == "array") {
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (!self(self, value[index], rule["items"],
                          path + "[" + std::to_string(index) + "]")) {
                    return false;
                }
            }
        }
        return true;
    };
    return validate_value(validate_value, object, schema, "$");
}

[[nodiscard]] inline bool is_within_object_nesting_depth(const Object& object) {
    std::vector<std::pair<const Object*, int>> pending{{&object, 0}};
    while (!pending.empty()) {
        const auto [value, depth] = pending.back();
        pending.pop_back();
        if (depth > maximum_object_nesting_depth) {
            return false;
        }
        if (value->is_array() || value->is_object()) {
            for (const auto& child : *value) {
                pending.emplace_back(&child, depth + 1);
            }
        }
    }
    return true;
}

} // namespace minidb
