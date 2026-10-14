#pragma once

#include "minidb/object.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Small helpers shared by the Database translation units. They are pure so they
// do not need access to Database's private state.
namespace minidb::detail {

// Objects are compared and indexed by the canonical text of a field's value.
inline std::string indexed_value(const Object& value) {
    return value.dump();
}

// Returns a page of `limit` IDs starting at `offset`, sorted ascending. It uses
// a partial selection so a large ID set is not fully sorted when only a small
// page is requested.
inline std::vector<std::uint64_t> ordered_page(std::vector<std::uint64_t> ids, std::size_t offset,
                                               std::size_t limit) {
    if (limit == 0 || offset >= ids.size()) {
        return {};
    }
    const auto end = offset + std::min(limit, ids.size() - offset);
    if (end < ids.size()) {
        std::nth_element(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(end), ids.end());
    }
    ids.resize(end);
    std::sort(ids.begin(), ids.end());
    return std::vector<std::uint64_t>(ids.begin() + static_cast<std::ptrdiff_t>(offset), ids.end());
}

} // namespace minidb::detail
