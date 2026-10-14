#include "minidb/database.h"

#include "database_internal.h"

#include <algorithm>
#include <limits>

// Equality queries on a field. Disk-backed databases consult the field index
// when one exists and otherwise scan the object IDs, reading each object one at a
// time so the working set stays bounded. In-memory databases use the hash index.
namespace minidb {

std::vector<ObjectMatch> Database::find(std::string_view field, const Object& value) const {
    return find(field, value, 0, std::numeric_limits<std::size_t>::max()).matches;
}

ObjectMatchPage Database::find(std::string_view field, const Object& value, std::size_t offset,
                               std::size_t limit) const {
    if (disk_backed()) {
        const auto fields = storage_->indexed_fields();
        const bool indexed =
            std::any_of(fields.begin(), fields.end(), [&](const std::string& name) {
                return std::string_view{name} == field;
            });
        std::vector<std::uint64_t> matching_ids;
        const auto encoded = detail::indexed_value(value);
        if (indexed) {
            for (const auto id : storage_->query_index(field, encoded)) {
                const auto object = storage_->read(id);
                if (object == nullptr) {
                    continue;
                }
                const auto entry = object->find(std::string(field));
                if (entry != object->end() && entry->dump() == encoded) {
                    matching_ids.push_back(id);
                }
            }
        } else {
            for (const auto id : ordered_ids_) {
                const auto object = storage_->read(id);
                if (object == nullptr) {
                    continue;
                }
                const auto entry = object->find(std::string(field));
                if (entry != object->end() && entry->dump() == encoded) {
                    matching_ids.push_back(id);
                }
            }
        }
        ObjectMatchPage result;
        result.total = matching_ids.size();
        if (limit == 0 || offset >= result.total) {
            return result;
        }
        const auto end = offset + std::min(limit, matching_ids.size() - offset);
        result.matches.reserve(end - offset);
        for (std::size_t index = offset; index < end; ++index) {
            const auto object = storage_->read(matching_ids[index]);
            if (object != nullptr) {
                result.matches.push_back({matching_ids[index], object});
            }
        }
        return result;
    }
    const auto field_entry = field_index_.find(field);
    if (field_entry == field_index_.end()) {
        return {};
    }
    const auto matching_ids = field_entry->second.find(detail::indexed_value(value));
    if (matching_ids == field_entry->second.end()) {
        return {};
    }
    ObjectMatchPage result;
    result.total = matching_ids->second.size();
    if (limit == 0 || offset >= result.total) {
        return result;
    }

    std::vector<std::uint64_t> matching_id_list(matching_ids->second.begin(),
                                                matching_ids->second.end());
    auto selected_ids = detail::ordered_page(std::move(matching_id_list), offset, limit);
    result.matches.reserve(selected_ids.size());
    for (const auto id : selected_ids) {
        const auto object = objects_.find(id);
        if (object != objects_.end()) {
            result.matches.push_back({id, object->second});
        }
    }
    return result;
}

ObjectMatchPage Database::find_after(std::string_view field, const Object& value,
                                     std::optional<std::uint64_t> after_id,
                                     std::size_t limit) const {
    if (disk_backed()) {
        const auto fields = storage_->indexed_fields();
        const bool indexed =
            std::any_of(fields.begin(), fields.end(), [&](const std::string& name) {
                return std::string_view{name} == field;
            });
        std::vector<std::uint64_t> matching_ids;
        const auto encoded = detail::indexed_value(value);
        if (indexed) {
            for (const auto id : storage_->query_index(field, encoded)) {
                const auto object = storage_->read(id);
                if (object == nullptr) {
                    continue;
                }
                const auto entry = object->find(std::string(field));
                if (entry != object->end() && entry->dump() == encoded) {
                    matching_ids.push_back(id);
                }
            }
        } else {
            for (const auto id : ordered_ids_) {
                const auto object = storage_->read(id);
                if (object == nullptr) {
                    continue;
                }
                const auto entry = object->find(std::string(field));
                if (entry != object->end() && entry->dump() == encoded) {
                    matching_ids.push_back(id);
                }
            }
        }
        ObjectMatchPage result;
        result.total = matching_ids.size();
        if (limit == 0) {
            return result;
        }
        auto id = after_id ? std::upper_bound(matching_ids.begin(), matching_ids.end(), *after_id)
                           : matching_ids.begin();
        result.matches.reserve(std::min(limit, matching_ids.size()));
        while (id != matching_ids.end() && result.matches.size() < limit) {
            const auto object = storage_->read(*id);
            if (object != nullptr) {
                result.matches.push_back({*id, object});
            }
            ++id;
        }
        result.has_more = id != matching_ids.end();
        return result;
    }
    const auto field_entry = field_index_.find(field);
    if (field_entry == field_index_.end()) {
        return {};
    }
    const auto matching_ids = field_entry->second.find(detail::indexed_value(value));
    if (matching_ids == field_entry->second.end()) {
        return {};
    }
    ObjectMatchPage result;
    result.total = matching_ids->second.size();
    if (limit == 0) {
        return result;
    }
    auto id = after_id ? matching_ids->second.upper_bound(*after_id) : matching_ids->second.begin();
    result.matches.reserve(std::min(limit, matching_ids->second.size()));
    while (id != matching_ids->second.end() && result.matches.size() < limit) {
        const auto object = objects_.find(*id);
        if (object != objects_.end()) {
            result.matches.push_back({*id, object->second});
        }
        ++id;
    }
    result.has_more = id != matching_ids->second.end();
    return result;
}

} // namespace minidb
