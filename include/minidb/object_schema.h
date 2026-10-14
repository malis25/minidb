#pragma once

#include "minidb/object.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace minidb {

// Validates the shape of a schema document: every node must declare a supported
// type, objects must declare properties, `required` must name declared
// properties, and arrays must declare an items schema. On failure writes a
// human-readable reason to `error` when it is non-null.
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

// A strict schema rejects undeclared properties at every object level. The
// server only creates databases with strict schemas, so stored objects are
// self-describing rather than silently carrying unknown fields.
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

// A JSON value matches a declared schema type. `null` is the explicit "unset"
// value and satisfies every declared type, so a value that must be present can
// still be represented before its field has a meaningful value.
[[nodiscard]] inline bool json_value_matches_type(const Object& value, std::string_view type) {
    if (value.is_null()) {
        return true;
    }
    return (type == "object" && value.is_object()) || (type == "array" && value.is_array()) ||
           (type == "string" && value.is_string()) ||
           (type == "integer" && value.is_number_integer()) ||
           (type == "number" && value.is_number()) || (type == "boolean" && value.is_boolean()) ||
           (type == "null" && value.is_null());
}

// Validates one JSON value against a schema node. This is shared by whole-object
// validation and by `default` validation so that a declared default must itself
// be a valid instance of the property schema it belongs to.
[[nodiscard]] inline bool validate_value_against_rule(const Object& value, const Object& rule,
                                                      const std::string& path,
                                                      std::string* error = nullptr) {
    const auto type = rule["type"].get<std::string>();
    if (!json_value_matches_type(value, type)) {
        if (error != nullptr) {
            *error = "Value at '" + path + "' must be of type " + type + ".";
        }
        return false;
    }
    if (value.is_null()) {
        return true;
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
                    *error = "Property is not allowed by the schema: " + path + "." + property.key();
                }
                return false;
            }
            if (!validate_value_against_rule(property.value(), properties[property.key()],
                                             path + "." + property.key(), error)) {
                return false;
            }
        }
        return true;
    }
    if (type == "array") {
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (!validate_value_against_rule(value[index], rule["items"],
                                             path + "[" + std::to_string(index) + "]", error)) {
                return false;
            }
        }
    }
    return true;
}

// Validates a complete document: it must be a JSON object and must satisfy every
// rule in the schema.
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
    return validate_value_against_rule(object, schema, "$", error);
}

// Rejects schemas whose `default` values are not valid instances of the node
// that declares them. Schema adaptation copies defaults into existing objects,
// so an invalid default would corrupt the database.
[[nodiscard]] inline bool validate_schema_defaults(const ObjectSchema& schema,
                                                   std::string* error = nullptr) {
    const auto walk = [&](const auto& self, const Object& node, const std::string& path) -> bool {
        if (node.contains("default") &&
            !validate_value_against_rule(node["default"], node, path + ".default", error)) {
            return false;
        }
        if (node["type"] == "object") {
            for (auto property = node["properties"].begin(); property != node["properties"].end();
                 ++property) {
                if (!self(self, property.value(), path + "." + property.key())) {
                    return false;
                }
            }
        } else if (node["type"] == "array") {
            if (!self(self, node["items"], path + "[]")) {
                return false;
            }
        }
        return true;
    };
    return validate_object_schema(schema, error) && walk(walk, schema, "$");
}

// The value a newly declared property receives when an object is adapted to a
// schema: the property's `default` when present, otherwise `null`.
[[nodiscard]] inline Object schema_default_or_null(const Object& rule) {
    if (rule.is_object() && rule.contains("default")) {
        return rule["default"];
    }
    return Object();
}

// Adapts `value` in place so it satisfies `rule`:
//   * a `null` value is left untouched (null already satisfies every type);
//   * object properties the schema no longer declares are removed;
//   * missing object properties are filled from the property's `default` or null;
//   * a value that cannot satisfy its rule is replaced with the rule's default
//     or null.
// Returns true when the value satisfies the rule afterwards. The transformation
// always succeeds so a schema migration never leaves a database half-adapted.
[[nodiscard]] inline bool adapt_value_to_schema(Object& value, const Object& rule) {
    if (value.is_null()) {
        return true;
    }
    const auto type = rule["type"].get<std::string>();
    if (type == "object") {
        if (!value.is_object()) {
            return false;
        }
        const auto& properties = rule["properties"];
        for (auto property = value.begin(); property != value.end();) {
            if (properties.contains(property.key())) {
                ++property;
            } else {
                property = value.erase(property);
            }
        }
        for (auto property = properties.begin(); property != properties.end(); ++property) {
            const auto& name = property.key();
            const auto& property_rule = property.value();
            if (value.contains(name)) {
                if (!adapt_value_to_schema(value[name], property_rule)) {
                    value[name] = schema_default_or_null(property_rule);
                }
            } else {
                value[name] = schema_default_or_null(property_rule);
            }
        }
        return true;
    }
    if (type == "array") {
        if (!value.is_array()) {
            return false;
        }
        for (auto& element : value) {
            if (!adapt_value_to_schema(element, rule["items"])) {
                element = schema_default_or_null(rule["items"]);
            }
        }
        return true;
    }
    return json_value_matches_type(value, type);
}

} // namespace minidb
