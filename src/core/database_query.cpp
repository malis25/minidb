#include "minidb/database.h"

#include "database_internal.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
// Evaluates a FilterNode against a single object. A missing field is present
// with value null, so unset fields compare predictably.
bool match_field(const minidb::Object& object, const minidb::FilterNode& leaf) {
    const auto entry = object.find(leaf.field);
    const bool present = entry != object.end();
    const bool is_null = !present || entry->is_null();

    if (leaf.op == "exists") return present;
    if (leaf.op == "is_null") return is_null;

    const auto field_value = present ? *entry : minidb::Object(nullptr);

    // `in`: field value must be in the supplied array.
    if (leaf.op == "in") {
        if (!leaf.value.is_array()) return false;
        for (const auto& candidate : leaf.value) {
            if (candidate == field_value) return true;
        }
        return false;
    }

    // ordered / string comparisons
    const bool both_numbers = field_value.is_number() && leaf.value.is_number();
    if (leaf.op == "eq") return field_value == leaf.value;
    if (leaf.op == "ne" || leaf.op == "neq") return !(field_value == leaf.value);

    if (leaf.op == "contains") {
        if (!field_value.is_string() || !leaf.value.is_string()) return false;
        return field_value.get<std::string>().find(leaf.value.get<std::string>()) != std::string::npos;
    }
    if (leaf.op == "starts_with") {
        if (!field_value.is_string() || !leaf.value.is_string()) return false;
        const auto& s = field_value.get<std::string>();
        const auto& p = leaf.value.get<std::string>();
        return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
    }
    if (leaf.op == "ends_with") {
        if (!field_value.is_string() || !leaf.value.is_string()) return false;
        const auto& s = field_value.get<std::string>();
        const auto& p = leaf.value.get<std::string>();
        return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
    }

    // ordered comparisons
    if (leaf.op == "gt" || leaf.op == "gte" || leaf.op == "lt" || leaf.op == "lte") {
        int cmp = 0;
        if (both_numbers) {
            double a = field_value.get<double>();
            double b = leaf.value.get<double>();
            cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
        } else if (field_value.is_string() && leaf.value.is_string()) {
            cmp = field_value.get<std::string>().compare(leaf.value.get<std::string>());
        } else {
            // incomparable types: no match for ordered operators
            return false;
        }
        if (leaf.op == "gt") return cmp > 0;
        if (leaf.op == "gte") return cmp >= 0;
        if (leaf.op == "lt") return cmp < 0;
        if (leaf.op == "lte") return cmp <= 0;
    }
    throw std::invalid_argument("Unsupported query operator: " + leaf.op);
}

bool evaluate_filter(const minidb::FilterNode& filter, const minidb::Object& object) {
    if (filter.composite) {
        if (filter.children.empty()) return true;
        if (filter.combine == "or") {
            for (const auto& c : filter.children) if (evaluate_filter(c, object)) return true;
            return false;
        }
        for (const auto& c : filter.children) if (!evaluate_filter(c, object)) return false;
        return true;
    }
    // leaf with empty field name => match all
    if (filter.field.empty()) return true;
    return match_field(object, filter);
}
} // namespace

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

ObjectMatchPage Database::query(const FilterNode& filter, std::size_t offset,
                                std::size_t limit) const {
    std::vector<std::uint64_t> matching_ids;
    if (disk_backed()) {
        for (const auto id : ordered_ids_) {
            const auto object = storage_->read(id);
            if (object == nullptr) continue;
            if (evaluate_filter(filter, *object)) matching_ids.push_back(id);
        }
    } else {
        for (const auto& [id, object] : objects_) {
            static_cast<void>(id);
            if (object != nullptr && evaluate_filter(filter, *object)) matching_ids.push_back(id);
        }
    }
    std::sort(matching_ids.begin(), matching_ids.end());
    ObjectMatchPage result;
    result.total = matching_ids.size();
    if (limit == 0 || offset >= result.total) {
        return result;
    }
    const auto end = offset + std::min(limit, matching_ids.size() - offset);
    result.matches.reserve(end - offset);
    for (std::size_t index = offset; index < end; ++index) {
        const auto id = matching_ids[index];
        std::shared_ptr<const Object> object;
        if (disk_backed()) object = storage_->read(id);
        else {
            const auto it = objects_.find(id);
            if (it != objects_.end()) object = it->second;
        }
        if (object != nullptr) result.matches.push_back({id, object});
    }
    return result;
}

ObjectMatchPage Database::query(const FilterNode& filter,
                                std::optional<std::uint64_t> after_id,
                                std::size_t limit) const {
    std::vector<std::uint64_t> matching_ids;
    if (disk_backed()) {
        for (const auto id : ordered_ids_) {
            const auto object = storage_->read(id);
            if (object == nullptr) continue;
            if (evaluate_filter(filter, *object)) matching_ids.push_back(id);
        }
    } else {
        for (const auto& [id, object] : objects_) {
            static_cast<void>(id);
            if (object != nullptr && evaluate_filter(filter, *object)) matching_ids.push_back(id);
        }
    }
    std::sort(matching_ids.begin(), matching_ids.end());
    ObjectMatchPage result;
    result.total = matching_ids.size();
    if (limit == 0) {
        return result;
    }
    auto it = after_id ? std::upper_bound(matching_ids.begin(), matching_ids.end(), *after_id)
                       : matching_ids.begin();
    result.matches.reserve(std::min(limit, matching_ids.size()));
    while (it != matching_ids.end() && result.matches.size() < limit) {
        const auto id = *it++;
        std::shared_ptr<const Object> object;
        if (disk_backed()) object = storage_->read(id);
        else {
            const auto it2 = objects_.find(id);
            if (it2 != objects_.end()) object = it2->second;
        }
        if (object != nullptr) result.matches.push_back({id, object});
    }
    result.has_more = it != matching_ids.end();
    return result;
}

} // namespace minidb
