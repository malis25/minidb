#pragma once

#include "minidb/bplus_tree.h"
#include "minidb/object.h"

#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minidb {

[[nodiscard]] bool is_valid_database_name(std::string_view name) noexcept;

struct StoredObject {
    std::uint64_t id = 0;
    Object object;
};

struct StorageSnapshot {
    std::vector<StoredObject> objects;
    std::vector<std::uint64_t> object_ids;
    std::uint64_t next_id = 0;
    ObjectSchema schema;
    std::uint64_t maximum_objects_per_segment = 0;
    std::vector<std::string> indexed_fields;
    bool disk_backed = false;
};

enum class StorageMutationType { store, erase };

struct StorageMutation {
    StorageMutationType type = StorageMutationType::store;
    std::uint64_t id = 0;
    Object object;
};

// Database uses this boundary without depending on filenames or segment layout.
// The index/read methods let a storage backend serve lookups and objects straight
// from disk so a database can keep a bounded working set in memory.
class ObjectStorage {
  public:
    virtual ~ObjectStorage() = default;

    [[nodiscard]] virtual StorageSnapshot load() = 0;
    virtual void store(std::uint64_t id, const Object& object, std::uint64_t next_id) = 0;
    virtual void erase(std::uint64_t id) = 0;
    virtual void apply_batch(const std::vector<StorageMutation>& mutations,
                             std::uint64_t next_id) = 0;
    virtual void update_schema(const ObjectSchema& schema) = 0;

    [[nodiscard]] virtual bool is_disk_backed() const noexcept {
        return false;
    }

    [[nodiscard]] virtual std::shared_ptr<const Object> read(std::uint64_t id) {
        static_cast<void>(id);
        return nullptr;
    }

    [[nodiscard]] virtual std::vector<std::uint64_t> query_index(std::string_view field,
                                                                 std::string_view encoded_value) {
        static_cast<void>(field);
        static_cast<void>(encoded_value);
        return {};
    }

    virtual void create_field_index(std::string_view field) {
        static_cast<void>(field);
        throw std::logic_error("This storage backend does not support field indexes.");
    }

    virtual void drop_field_index(std::string_view field) {
        static_cast<void>(field);
        throw std::logic_error("This storage backend does not support field indexes.");
    }

    [[nodiscard]] virtual std::vector<std::string> indexed_fields() const {
        return {};
    }
};

// The current backend stores metadata in one manifest and records in segments
// bounded by the number of newly created objects. Other backends can implement
// ObjectStorage later.
class SegmentedStorage final : public ObjectStorage {
  public:
    SegmentedStorage(std::filesystem::path directory, std::string database_name,
                     std::uint64_t maximum_objects_per_segment = 0,
                     ObjectSchema schema = {{"type", "object"},
                                            {"properties", Object::object()},
                                            {"additionalProperties", true}},
                     std::vector<std::string> indexed_fields = {},
                     std::uint64_t cache_bytes = 0);

    [[nodiscard]] StorageSnapshot load() override;
    void store(std::uint64_t id, const Object& object, std::uint64_t next_id) override;
    void erase(std::uint64_t id) override;
    void apply_batch(const std::vector<StorageMutation>& mutations, std::uint64_t next_id) override;
    void update_schema(const ObjectSchema& schema) override;

    [[nodiscard]] bool is_disk_backed() const noexcept override;
    [[nodiscard]] std::shared_ptr<const Object> read(std::uint64_t id) override;
    [[nodiscard]] std::vector<std::uint64_t> query_index(std::string_view field,
                                                         std::string_view encoded_value) override;
    void create_field_index(std::string_view field) override;
    void drop_field_index(std::string_view field) override;
    [[nodiscard]] std::vector<std::string> indexed_fields() const override;

    [[nodiscard]] const std::filesystem::path& manifest_path() const noexcept;
    [[nodiscard]] std::uint64_t maximum_objects_per_segment() const noexcept;
    [[nodiscard]] std::uint64_t cache_bytes() const noexcept;

  private:
    struct ObjectLocation {
        std::size_t segment_index = 0;
        std::uint64_t payload_offset = 0;
        std::uint64_t payload_size = 0;
    };

    void append_record(char operation, std::uint64_t id, const Object* object,
                       std::uint64_t next_id);
    std::uint64_t append_payload(std::string record, std::uint64_t next_id, bool update_next_id,
                                 std::uint64_t created_objects);
    void save_manifest();
    void create_segment(const std::string& filename);
    void update_segment_object_count(const std::filesystem::path& path, std::uint64_t object_count);

    void open_index_trees();
    void build_index_tree(const std::string& field);
    void insert_into_indexes(std::uint64_t id, const Object& object);
    void remove_from_indexes(std::uint64_t id, const Object& object);
    [[nodiscard]] std::shared_ptr<const Object> object_at(const ObjectLocation& location) const;
    [[nodiscard]] std::filesystem::path index_path(std::string_view field) const;
    void cache_object(std::uint64_t id, std::shared_ptr<const Object> object);
    void drop_cached(std::uint64_t id);
    void record_store(std::size_t segment_index, std::uint64_t payload_offset,
                      std::uint64_t payload_size, std::uint64_t id, const Object& object);
    void record_erase(std::uint64_t id);

    std::filesystem::path directory_;
    std::filesystem::path manifest_path_;
    std::string database_name_;
    std::uint64_t maximum_objects_per_segment_;
    std::uint64_t next_id_ = 0;
    ObjectSchema schema_;
    std::vector<std::string> segment_files_;
    std::vector<std::uint64_t> segment_object_counts_;

    std::vector<std::string> indexed_fields_;
    std::unordered_map<std::string, std::unique_ptr<BPlusTree>> index_trees_;
    std::unordered_map<std::uint64_t, ObjectLocation> object_locations_;
    bool disk_backed_ = false;
    std::uint64_t cache_bytes_ = 0;
    std::uint64_t cached_bytes_ = 0;
    std::list<std::pair<std::uint64_t, std::shared_ptr<const Object>>> cache_order_;
    std::unordered_map<std::uint64_t,
                       std::list<std::pair<std::uint64_t, std::shared_ptr<const Object>>>::iterator>
        cache_index_;
};

} // namespace minidb
