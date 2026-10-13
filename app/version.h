#pragma once

#include <string_view>

namespace minidb {

// Application version, compiled into the executable by CMake.
[[nodiscard]] std::string_view version() noexcept;

} // namespace minidb
