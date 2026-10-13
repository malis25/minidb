#include "minidb/database.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace minidb {

namespace {

std::string indexed_value(const Object& value) {
    return value.dump();
}

} // namespace

Database::Database(std::unique_ptr<ObjectStorage> storage) : storage_(std::move(storage)) {
    if (!storage_) {
        throw std::invalid_argument("Database storage cannot be null.");
    }

    auto snapshot = storage_->load();
    next_id_ = snapshot.next_id;
    objects_.reserve(snapshot.objects.size());
    for (auto& stored : snapshot.objects) {
        if (!stored.object.is_object()) {
            throw std::runtime_error("Storage returned an invalid database object.");
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
        add_to_index(entry->first, *entry->second);
    }
}

std::uint64_t Database::create(Object object) {
    if (!object.is_object()) {
        throw std::invalid_argument("A database object must be a JSON object.");
    }
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("The database has exhausted its uint64 object IDs.");
    }

    const auto id = next_id_;
    const auto following_id = id + 1;
    auto stored_object = std::make_shared<const Object>(std::move(object));
    if (storage_) {
        storage_->store(id, *stored_object, following_id);
    }

    const auto [entry, inserted] = objects_.emplace(id, stored_object);
    if (!inserted) {
        throw std::logic_error("The next database ID is already in use.");
    }
    add_to_index(id, *stored_object);
    next_id_ = following_id;
    return id;
}

void Database::set(std::uint64_t id, Object object) {
    if (!object.is_object()) {
        throw std::invalid_argument("A database object must be a JSON object.");
    }
    const auto existing = objects_.find(id);
    if (existing == objects_.end()) {
        throw std::out_of_range("Object ID does not exist.");
    }

    auto replacement = std::make_shared<const Object>(std::move(object));
    if (storage_) {
        storage_->store(id, *replacement, next_id_);
    }
    remove_from_index(id, *existing->second);
    existing->second = std::move(replacement);
    add_to_index(id, *existing->second);
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
    if (storage_) {
        storage_->erase(id);
    }
    remove_from_index(id, *object->second);
    objects_.erase(object);
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

std::vector<ObjectMatch> Database::find(std::string_view field, const Object& value) const {
    const auto field_entry = field_index_.find(field);
    if (field_entry == field_index_.end()) {
        return {};
    }
    const auto matching_ids = field_entry->second.find(indexed_value(value));
    if (matching_ids == field_entry->second.end()) {
        return {};
    }

    std::vector<std::uint64_t> ordered_ids(matching_ids->second.begin(),
                                           matching_ids->second.end());
    std::sort(ordered_ids.begin(), ordered_ids.end());
    std::vector<ObjectMatch> result;
    result.reserve(ordered_ids.size());
    for (const auto id : ordered_ids) {
        const auto object = objects_.find(id);
        if (object != objects_.end()) {
            result.push_back({id, object->second});
        }
    }
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
