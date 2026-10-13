#pragma once

#include "minidb/object.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace minidb {

struct StoredObject {
    std::uint64_t id = 0;
    Object object;
};

struct StorageSnapshot {
    std::vector<StoredObject> objects;
    std::uint64_t next_id = 0;
};

// Database uses this boundary without depending on filenames or segment layout.
class ObjectStorage {
  public:
    virtual ~ObjectStorage() = default;

    [[nodiscard]] virtual StorageSnapshot load() = 0;
    virtual void store(std::uint64_t id, const Object& object, std::uint64_t next_id) = 0;
    virtual void erase(std::uint64_t id) = 0;
};

// The current backend stores metadata in one manifest and records in bounded
// segment files. Other backends can implement ObjectStorage later.
class SegmentedStorage final : public ObjectStorage {
  public:
    SegmentedStorage(std::filesystem::path directory, std::string database_name,
                     std::uint64_t maximum_segment_size);

    [[nodiscard]] StorageSnapshot load() override;
    void store(std::uint64_t id, const Object& object, std::uint64_t next_id) override;
    void erase(std::uint64_t id) override;

    [[nodiscard]] const std::filesystem::path& manifest_path() const noexcept;

  private:
    void append_record(char operation, std::uint64_t id, const Object* object,
                       std::uint64_t next_id);
    void save_manifest();
    void create_segment(const std::string& filename);

    std::filesystem::path directory_;
    std::filesystem::path manifest_path_;
    std::string database_name_;
    std::uint64_t maximum_segment_size_;
    std::uint64_t next_id_ = 0;
    std::vector<std::string> segment_files_;
};

} // namespace minidb
