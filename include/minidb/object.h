#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace minidb {

// JSON objects may nest this deep; deeper documents are rejected before they are
// stored so that decoding cannot exhaust the stack.
inline constexpr int maximum_object_nesting_depth = 128;

// A document is a JSON object. Its fields can contain strings, integers,
// floating-point numbers, booleans, nulls, arrays, and nested objects.
using Object = nlohmann::json;
using ObjectSchema = nlohmann::json;

// Returns true when the document nests no deeper than
// `maximum_object_nesting_depth`. The traversal is iterative so a hostile
// document cannot overflow the stack.
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

// Schema validation and adaptation are a separate concern from the document type
// itself, so they live in their own header. Including it here keeps `object.h`
// the single umbrella for the object model.
#include "minidb/object_schema.h"
