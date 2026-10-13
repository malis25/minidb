#pragma once

#include <nlohmann/json.hpp>

namespace minidb {

// A document is a JSON object. Its fields can contain strings, integers,
// floating-point numbers, booleans, nulls, arrays, and nested objects.
using Object = nlohmann::json;

} // namespace minidb
