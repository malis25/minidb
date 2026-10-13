#include "version.h"

namespace minidb {

std::string_view version() noexcept {
    // MINIDB_VERSION is generated from the CMake project version.
    return MINIDB_VERSION;
}

} // namespace minidb
