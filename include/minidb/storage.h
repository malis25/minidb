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

    // Releases any on-disk resources the backend holds open (for example index
    // files). Callers must do this before moving or deleting the database
    // directory, because some platforms refuse to rename a directory that still
    // has open files. The backend must not be used afterwards.
    virtual void close() {}

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

// IDs are reserved in blocks so that creating objects does not rewrite the
// manifest on every store. The manifest only needs to guarantee that an ID is
// never handed out twice, so it records an upper bound and the backend skips the
// unused remainder of a block after an unclean stop. A graceful close collapses
// the block back to the exact next ID, so clean restarts keep IDs contiguous.
//
// The value is the start of the measured throughput plateau: a write sweep
// (benchmarks/reservation_benchmark_main.cpp) shows Debug/Release throughput
// reaching ~98% of its asymptote by 128, with no meaningful gain beyond it. A
// smaller block writes the manifest more often; a larger one wastes more IDs
// after an unclean stop, so 128 balances the two.
inline constexpr std::uint64_t default_next_id_reservation = 128;

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
                     std::uint64_t cache_bytes = 0,
                     std::uint64_t next_id_reservation = default_next_id_reservation);

    ~SegmentedStorage() override;

    [[nodiscard]] StorageSnapshot load() override;
    void store(std::uint64_t id, const Object& object, std::uint64_t next_id) override;
    void erase(std::uint64_t id) override;
    void apply_batch(const std::vector<StorageMutation>& mutations, std::uint64_t next_id) override;
    void update_schema(const ObjectSchema& schema) override;
    void close() override;

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
    [[nodiscard]] std::uint64_t next_id_reservation() const noexcept;
    // Number of manifest writes performed so far; useful for tests and for
    // measuring how often the reservation has to be refreshed.
    [[nodiscard]] std::uint64_t manifest_writes() const noexcept;

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
    // Writes the manifest with the exact next ID and clears any unused reserved
    // block. Best-effort on shutdown so clean restarts do not skip IDs.
    void collapse_next_id_reservation();
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
    // Exclusive upper bound of IDs that are safe to hand out without another
    // manifest write. next_id_ <= next_id_reservation_ always holds.
    std::uint64_t next_id_reservation_size_ = default_next_id_reservation;
    std::uint64_t next_id_reservation_ = 0;
    std::uint64_t manifest_writes_ = 0;
    bool loaded_ = false;
    bool closed_ = false;
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
