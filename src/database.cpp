#include "minidb/database.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace minidb {

namespace {

std::string indexed_value(const Object& value) {
    return value.dump();
}

std::vector<std::uint64_t> ordered_page(std::vector<std::uint64_t> ids, std::size_t offset,
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

} // namespace

Database::Database(std::unique_ptr<ObjectStorage> storage) : storage_(std::move(storage)) {
    if (!storage_) {
        throw std::invalid_argument("Database storage cannot be null.");
    }

    auto snapshot = storage_->load();
    next_id_ = snapshot.next_id;
    if (!snapshot.schema.is_null()) {
        schema_ = std::move(snapshot.schema);
        std::string schema_error;
        if (!validate_object_schema(schema_, &schema_error)) {
            throw std::runtime_error("Storage returned an invalid object schema: " + schema_error);
        }
        maximum_objects_per_segment_ = snapshot.maximum_objects_per_segment;
    }
    objects_.reserve(snapshot.objects.size());
    for (auto& stored : snapshot.objects) {
        if (!stored.object.is_object()) {
            throw std::runtime_error("Storage returned an invalid database object.");
        }
        std::string validation_error;
        if (!validate_object_against_schema(stored.object, schema_, &validation_error)) {
            throw std::runtime_error("Stored object does not match the database schema: " +
                                     validation_error);
        }
        if (stored.id >= next_id_ && stored.id != std::numeric_limits<std::uint64_t>::max()) {
            next_id_ = stored.id + 1;
        } else if (stored.id == std::numeric_limits<std::uint64_t>::max()) {
            next_id_ = stored.id;
        }
        const auto [entry, inserted] =
            objects_.emplace(stored.id, std::make_shared<const Object>(std::move(stored.object)));
        if (!inserted) {
            throw std::runtime_error("Storage returned duplicate object IDs.");
        }
        ordered_ids_.insert(entry->first);
        add_to_index(entry->first, *entry->second);
    }
}

std::uint64_t Database::create(Object object) {
    const auto ids = apply_batch({{ObjectMutation::Type::create, 0, std::move(object)}});
    return ids.front();
}

void Database::set(std::uint64_t id, Object object) {
    static_cast<void>(apply_batch({{ObjectMutation::Type::replace, id, std::move(object)}}));
}

std::vector<std::uint64_t> Database::apply_batch(const std::vector<ObjectMutation>& mutations) {
    if (mutations.empty()) {
        throw std::invalid_argument("A database batch must contain at least one operation.");
    }

    struct PreparedMutation {
        ObjectMutation::Type type;
        std::uint64_t id;
        ObjectPtr object;
    };
    std::vector<PreparedMutation> prepared;
    std::vector<std::uint64_t> created_ids;
    std::vector<StorageMutation> storage_mutations;
    std::unordered_set<std::uint64_t> touched_ids;
    prepared.reserve(mutations.size());
    created_ids.reserve(mutations.size());
    storage_mutations.reserve(mutations.size());
    touched_ids.reserve(mutations.size());
    auto following_id = next_id_;

    for (const auto& mutation : mutations) {
        switch (mutation.type) {
        case ObjectMutation::Type::create: {
            std::string validation_error;
            if (!validate_object_against_schema(mutation.object, schema_, &validation_error)) {
                throw std::invalid_argument(validation_error);
            }
            if (!is_within_object_nesting_depth(mutation.object)) {
                throw std::invalid_argument("JSON nesting exceeds the maximum depth of 128.");
            }
            if (following_id == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("The database has exhausted its uint64 object IDs.");
            }
            const auto id = following_id++;
            auto object = std::make_shared<const Object>(mutation.object);
            prepared.push_back({mutation.type, id, object});
            storage_mutations.push_back({StorageMutationType::store, id, *object});
            created_ids.push_back(id);
            break;
        }
        case ObjectMutation::Type::replace: {
            std::string validation_error;
            if (!validate_object_against_schema(mutation.object, schema_, &validation_error)) {
                throw std::invalid_argument(validation_error);
            }
            if (!is_within_object_nesting_depth(mutation.object)) {
                throw std::invalid_argument("JSON nesting exceeds the maximum depth of 128.");
            }
            if (!touched_ids.insert(mutation.id).second) {
                throw std::invalid_argument(
                    "A batch cannot mutate the same object more than once.");
            }
            if (!objects_.contains(mutation.id)) {
                throw std::out_of_range("Object ID does not exist.");
            }
            auto object = std::make_shared<const Object>(mutation.object);
            prepared.push_back({mutation.type, mutation.id, object});
            storage_mutations.push_back({StorageMutationType::store, mutation.id, *object});
            break;
        }
        case ObjectMutation::Type::erase:
            if (!touched_ids.insert(mutation.id).second) {
                throw std::invalid_argument(
                    "A batch cannot mutate the same object more than once.");
            }
            if (!objects_.contains(mutation.id)) {
                throw std::out_of_range("Object ID does not exist.");
            }
            prepared.push_back({mutation.type, mutation.id, nullptr});
            storage_mutations.push_back({StorageMutationType::erase, mutation.id, {}});
            break;
        default:
            throw std::invalid_argument("Batch contains an unknown operation type.");
        }
    }

    if (storage_) {
        storage_->apply_batch(storage_mutations, following_id);
    }

    for (const auto& mutation : prepared) {
        const auto existing = objects_.find(mutation.id);
        if (mutation.type == ObjectMutation::Type::create) {
            const auto [entry, inserted] = objects_.emplace(mutation.id, mutation.object);
            if (!inserted) {
                throw std::logic_error("The next database ID is already in use.");
            }
            ordered_ids_.insert(entry->first);
            add_to_index(entry->first, *entry->second);
        } else if (mutation.type == ObjectMutation::Type::replace) {
            remove_from_index(mutation.id, *existing->second);
            existing->second = mutation.object;
            add_to_index(mutation.id, *existing->second);
        } else {
            remove_from_index(mutation.id, *existing->second);
            ordered_ids_.erase(mutation.id);
            objects_.erase(existing);
        }
    }
    next_id_ = following_id;
    return created_ids;
}

void Database::update_schema(ObjectSchema schema) {
    std::string schema_error;
    if (!validate_object_schema(schema, &schema_error)) {
        throw std::invalid_argument(schema_error);
    }
    for (const auto& [id, object] : objects_) {
        static_cast<void>(id);
        if (!validate_object_against_schema(*object, schema, &schema_error)) {
            throw std::invalid_argument("Existing objects do not satisfy the new schema: " +
                                        schema_error);
        }
    }
    if (storage_) {
        storage_->update_schema(schema);
    }
    schema_ = std::move(schema);
}

const ObjectSchema& Database::schema() const noexcept {
    return schema_;
}

std::uint64_t Database::maximum_objects_per_segment() const noexcept {
    return maximum_objects_per_segment_;
}

std::shared_ptr<const Object> Database::get(std::uint64_t id) const {
    const auto object = objects_.find(id);
    return object == objects_.end() ? nullptr : object->second;
}

bool Database::remove(std::uint64_t id) {
    const auto object = objects_.find(id);
    if (object == objects_.end()) {
        return false;
    }
    static_cast<void>(apply_batch({{ObjectMutation::Type::erase, id, {}}}));
    return true;
}

bool Database::contains(std::uint64_t id) const {
    return objects_.find(id) != objects_.end();
}

std::size_t Database::size() const noexcept {
    return objects_.size();
}

std::vector<std::uint64_t> Database::ids() const {
    std::vector<std::uint64_t> result;
    result.reserve(objects_.size());
    for (const auto& [id, object] : objects_) {
        static_cast<void>(object);
        result.push_back(id);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::uint64_t> Database::ids(std::size_t offset, std::size_t limit) const {
    if (limit == 0 || offset >= objects_.size()) {
        return {};
    }
    std::vector<std::uint64_t> all_ids;
    all_ids.reserve(objects_.size());
    for (const auto& [id, object] : objects_) {
        static_cast<void>(object);
        all_ids.push_back(id);
    }
    return ordered_page(std::move(all_ids), offset, limit);
}

IdPage Database::ids_after(std::optional<std::uint64_t> after_id, std::size_t limit) const {
    IdPage result;
    result.total = objects_.size();
    if (limit == 0) {
        return result;
    }
    auto id = after_id ? ordered_ids_.upper_bound(*after_id) : ordered_ids_.begin();
    result.ids.reserve(std::min(limit, ordered_ids_.size()));
    while (id != ordered_ids_.end() && result.ids.size() < limit) {
        result.ids.push_back(*id++);
    }
    result.has_more = id != ordered_ids_.end();
    return result;
}

std::vector<ObjectMatch> Database::find(std::string_view field, const Object& value) const {
    return find(field, value, 0, std::numeric_limits<std::size_t>::max()).matches;
}

ObjectMatchPage Database::find(std::string_view field, const Object& value, std::size_t offset,
                               std::size_t limit) const {
    const auto field_entry = field_index_.find(field);
    if (field_entry == field_index_.end()) {
        return {};
    }
    const auto matching_ids = field_entry->second.find(indexed_value(value));
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
    auto selected_ids = ordered_page(std::move(matching_id_list), offset, limit);
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
    const auto field_entry = field_index_.find(field);
    if (field_entry == field_index_.end()) {
        return {};
    }
    const auto matching_ids = field_entry->second.find(indexed_value(value));
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

std::uint64_t Database::next_id() const noexcept {
    return next_id_;
}

void Database::add_to_index(std::uint64_t id, const Object& object) {
    for (const auto& [field, value] : object.items()) {
        field_index_[field][indexed_value(value)].insert(id);
    }
}

void Database::remove_from_index(std::uint64_t id, const Object& object) {
    for (const auto& [field, value] : object.items()) {
        const auto field_entry = field_index_.find(field);
        if (field_entry == field_index_.end()) {
            continue;
        }
        const auto value_entry = field_entry->second.find(indexed_value(value));
        if (value_entry == field_entry->second.end()) {
            continue;
        }
        value_entry->second.erase(id);
        if (value_entry->second.empty()) {
            field_entry->second.erase(value_entry);
        }
        if (field_entry->second.empty()) {
            field_index_.erase(field_entry);
        }
    }
}

} // namespace minidb
