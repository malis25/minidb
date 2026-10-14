#include "minidb/storage.h"

#include "segmented_storage_internal.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Field indexes and the bounded object cache. The B+ tree stores hash-then-ID
// keys so a value lookup is a contiguous scan; the cache keeps recently read
// objects in memory without retaining the whole database.
namespace minidb {

std::vector<std::string> SegmentedStorage::indexed_fields() const {
    return indexed_fields_;
}

std::filesystem::path SegmentedStorage::index_path(std::string_view field) const {
    return directory_ / (database_name_ + ".index-" + detail::index_file_component(field) + ".tree");
}

void SegmentedStorage::insert_into_indexes(std::uint64_t id, const Object& object) {
    if (index_trees_.empty()) {
        return;
    }
    for (const auto& field : indexed_fields_) {
        const auto entry = object.find(field);
        if (entry == object.end()) {
            continue;
        }
        const auto tree = index_trees_.find(field);
        if (tree == index_trees_.end()) {
            continue;
        }
        tree->second->put(detail::make_index_key(entry->dump(), id), std::string_view{});
    }
}

void SegmentedStorage::remove_from_indexes(std::uint64_t id, const Object& object) {
    if (index_trees_.empty()) {
        return;
    }
    for (const auto& field : indexed_fields_) {
        const auto entry = object.find(field);
        if (entry == object.end()) {
            continue;
        }
        const auto tree = index_trees_.find(field);
        if (tree == index_trees_.end()) {
            continue;
        }
        static_cast<void>(tree->second->remove(detail::make_index_key(entry->dump(), id)));
    }
}

void SegmentedStorage::drop_cached(std::uint64_t id) {
    const auto cached = cache_index_.find(id);
    if (cached == cache_index_.end()) {
        return;
    }
    cached_bytes_ -= cached->second->second->dump().size();
    cache_order_.erase(cached->second);
    cache_index_.erase(cached);
}

void SegmentedStorage::cache_object(std::uint64_t id, std::shared_ptr<const Object> object) {
    if (cache_bytes_ == 0 || !object) {
        return;
    }
    const auto object_size = object->dump().size();
    if (object_size > cache_bytes_) {
        return;
    }
    drop_cached(id);
    while (cached_bytes_ + object_size > cache_bytes_ && !cache_order_.empty()) {
        const auto oldest = cache_order_.begin();
        cached_bytes_ -= oldest->second->dump().size();
        cache_index_.erase(oldest->first);
        cache_order_.erase(oldest);
    }
    cache_order_.push_back({id, std::move(object)});
    cache_index_[id] = std::prev(cache_order_.end());
    cached_bytes_ += object_size;
}

std::shared_ptr<const Object> SegmentedStorage::read(std::uint64_t id) {
    const auto cached = cache_index_.find(id);
    if (cached != cache_index_.end()) {
        cache_order_.splice(cache_order_.end(), cache_order_, cached->second);
        return cached->second->second;
    }
    const auto location = object_locations_.find(id);
    if (location == object_locations_.end()) {
        return nullptr;
    }
    auto object = object_at(location->second);
    cache_object(id, object);
    return object;
}

std::vector<std::uint64_t> SegmentedStorage::query_index(std::string_view field,
                                                         std::string_view encoded_value) {
    const auto tree = index_trees_.find(std::string(field));
    if (tree == index_trees_.end()) {
        return {};
    }
    const auto prefix = detail::make_index_prefix(encoded_value);
    std::vector<std::uint64_t> ids;
    auto cursor = tree->second->seek(prefix);
    while (cursor.valid()) {
        const auto key = cursor.key();
        if (key.size() < 16 || key.substr(0, 8) != prefix) {
            break;
        }
        ids.push_back(detail::read_index_id(key));
        cursor.next();
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void SegmentedStorage::build_index_tree(const std::string& field) {
    auto tree = std::make_unique<BPlusTree>(index_path(field));
    tree->clear();
    std::vector<std::uint64_t> ids;
    ids.reserve(object_locations_.size());
    for (const auto& [id, location] : object_locations_) {
        static_cast<void>(location);
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    for (const auto id : ids) {
        const auto location = object_locations_.find(id);
        if (location != object_locations_.end()) {
            const auto object = object_at(location->second);
            const auto entry = object->find(field);
            if (entry != object->end()) {
                tree->put(detail::make_index_key(entry->dump(), id), std::string_view{});
            }
        }
    }
    index_trees_[field] = std::move(tree);
}

void SegmentedStorage::open_index_trees() {
    detail::require_valid_indexed_fields(indexed_fields_);
    index_trees_.clear();
    for (const auto& field : indexed_fields_) {
        auto tree = std::make_unique<BPlusTree>(index_path(field));
        tree->clear();
        index_trees_[field] = std::move(tree);
    }
}

void SegmentedStorage::create_field_index(std::string_view field) {
    if (field.empty()) {
        throw std::invalid_argument("Indexed field names cannot be empty.");
    }
    const auto existing =
        std::find_if(indexed_fields_.begin(), indexed_fields_.end(),
                     [&](const std::string& name) { return std::string_view{name} == field; });
    if (existing != indexed_fields_.end()) {
        return;
    }
    indexed_fields_.push_back(std::string(field));
    build_index_tree(std::string(field));
    disk_backed_ = true;
    save_manifest();
}

void SegmentedStorage::drop_field_index(std::string_view field) {
    const auto entry =
        std::find_if(indexed_fields_.begin(), indexed_fields_.end(),
                     [&](const std::string& name) { return std::string_view{name} == field; });
    if (entry == indexed_fields_.end()) {
        return;
    }
    indexed_fields_.erase(entry);
    const auto tree = index_trees_.find(std::string(field));
    if (tree != index_trees_.end()) {
        const auto path = index_path(field);
        index_trees_.erase(tree);
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    save_manifest();
}

void SegmentedStorage::record_store(std::size_t segment_index, std::uint64_t payload_offset,
                                    std::uint64_t payload_size, std::uint64_t id,
                                    const Object& object) {
    const auto previous = object_locations_.find(id);
    if (previous != object_locations_.end()) {
        remove_from_indexes(id, *object_at(previous->second));
    }
    drop_cached(id);
    object_locations_[id] = {segment_index, payload_offset, payload_size};
    insert_into_indexes(id, object);
}

void SegmentedStorage::record_erase(std::uint64_t id) {
    const auto location = object_locations_.find(id);
    if (location != object_locations_.end()) {
        remove_from_indexes(id, *object_at(location->second));
        object_locations_.erase(location);
    }
    drop_cached(id);
}

} // namespace minidb
