#pragma once

#include <nlohmann/json.hpp>

#include <utility>
#include <vector>

namespace minidb {

inline constexpr int maximum_object_nesting_depth = 128;

// A document is a JSON object. Its fields can contain strings, integers,
// floating-point numbers, booleans, nulls, arrays, and nested objects.
using Object = nlohmann::json;

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
