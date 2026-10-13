#pragma once

#include "minidb/object.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace minidb {

[[nodiscard]] bool is_valid_database_name(std::string_view name) noexcept;

struct StoredObject {
    std::uint64_t id = 0;
    Object object;
};

struct StorageSnapshot {
    std::vector<StoredObject> objects;
    std::uint64_t next_id = 0;
    ObjectSchema schema;
    std::uint64_t maximum_objects_per_segment = 0;
};

enum class StorageMutationType { store, erase };

struct StorageMutation {
    StorageMutationType type = StorageMutationType::store;
    std::uint64_t id = 0;
    Object object;
};

// Database uses this boundary without depending on filenames or segment layout.
class ObjectStorage {
  public:
    virtual ~ObjectStorage() = default;

    [[nodiscard]] virtual StorageSnapshot load() = 0;
    virtual void store(std::uint64_t id, const Object& object, std::uint64_t next_id) = 0;
    virtual void erase(std::uint64_t id) = 0;
    virtual void apply_batch(const std::vector<StorageMutation>& mutations,
                             std::uint64_t next_id) = 0;
    virtual void update_schema(const ObjectSchema& schema) = 0;
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
                                            {"additionalProperties", true}});

    [[nodiscard]] StorageSnapshot load() override;
    void store(std::uint64_t id, const Object& object, std::uint64_t next_id) override;
    void erase(std::uint64_t id) override;
    void apply_batch(const std::vector<StorageMutation>& mutations, std::uint64_t next_id) override;
    void update_schema(const ObjectSchema& schema) override;

    [[nodiscard]] const std::filesystem::path& manifest_path() const noexcept;
    [[nodiscard]] std::uint64_t maximum_objects_per_segment() const noexcept;

  private:
    void append_record(char operation, std::uint64_t id, const Object* object,
                       std::uint64_t next_id);
    void append_payload(std::string record, std::uint64_t next_id, bool update_next_id,
                        std::uint64_t created_objects);
    void save_manifest();
    void create_segment(const std::string& filename);
    void migrate_legacy_segment(const std::string& filename, std::uint64_t object_count);
    void update_segment_object_count(const std::filesystem::path& path, std::uint64_t object_count);

    std::filesystem::path directory_;
    std::filesystem::path manifest_path_;
    std::string database_name_;
    std::uint64_t maximum_objects_per_segment_;
    std::uint64_t next_id_ = 0;
    ObjectSchema schema_;
    std::vector<std::string> segment_files_;
    std::vector<std::uint64_t> segment_object_counts_;
};

} // namespace minidb
