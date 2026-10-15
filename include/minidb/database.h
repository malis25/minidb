#pragma once

#include "minidb/object.h"
#include "minidb/storage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minidb {

// A composable query filter used by `Database::query`. Leaves match a single
// field against a value with an operator; composite nodes combine children with
// `and`/`or`. `null` values in either side are handled by the comparison
// operators, so unset fields can be queried explicitly.
struct FilterNode {
    bool composite = false;
    std::string combine = "and"; // "and" / "or"
    std::vector<FilterNode> children;

    std::string field;
    std::string op = "eq"; // eq, ne, gt, gte, lt, lte, contains, starts_with, ends_with, exists, is_null, in
    Object value = nullptr;
};

struct ObjectMatch {
    std::uint64_t id = 0;
    std::shared_ptr<const Object> object;
};

struct ObjectMatchPage {
    std::vector<ObjectMatch> matches;
    std::size_t total = 0;
    bool has_more = false;
};

struct IdPage {
    std::vector<std::uint64_t> ids;
    std::size_t total = 0;
    bool has_more = false;
};

struct ObjectMutation {
    enum class Type { create, replace, erase };

    Type type = Type::create;
    std::uint64_t id = 0;
    Object object;
};

// Objects use database-generated uint64 IDs. A hash index provides average
// O(1) ID lookups, and a second hash index supports equality queries on fields.
// The class is not internally synchronized; shared users provide their own lock.
class Database {
  public:
    Database() = default;
    explicit Database(std::unique_ptr<ObjectStorage> storage);

    // Allocate the next sequential ID, starting at zero, and create an object.
    [[nodiscard]] std::uint64_t create(Object object);

    // Replace an existing object without changing its ID.
    void set(std::uint64_t id, Object object);

    // Apply a set of creates/replacements/deletes as one persistent operation.
    [[nodiscard]] std::vector<std::uint64_t>
    apply_batch(const std::vector<ObjectMutation>& mutations);

    // Replaces the schema only when every existing object already satisfies it;
    // otherwise it throws std::invalid_argument. Use begin_schema_migration() to
    // change the schema for existing objects instead of rejecting the change.
    void update_schema(ObjectSchema schema);

    // Replaces the schema and marks every existing object for adaptation to it.
    // New writes are validated against the new schema immediately, while
    // existing objects are adapted in bounded chunks by migrate_schema_chunk()
    // so the database stays responsive. `schema` must be a valid strict schema
    // whose `default` values are valid instances of their properties.
    void begin_schema_migration(ObjectSchema schema);

    // Adapts at most `limit` not-yet-adapted objects and persists the changes as
    // one batch. Returns the number of objects still awaiting adaptation (zero
    // when the migration is complete). Callers hold the database lock and call
    // this repeatedly, releasing the lock between calls, until it returns zero.
    [[nodiscard]] std::size_t migrate_schema_chunk(std::size_t limit);

    struct SchemaMigrationStatus {
        bool pending = false;
        std::size_t total = 0;
        std::size_t processed = 0;
    };
    [[nodiscard]] SchemaMigrationStatus schema_migration_status() const noexcept;

    [[nodiscard]] const ObjectSchema& schema() const noexcept;
    [[nodiscard]] std::uint64_t maximum_objects_per_segment() const noexcept;

    // On-disk field indexes. These are no-ops for purely in-memory databases.
    [[nodiscard]] std::vector<std::string> indexed_fields() const;
    void create_index(std::string_view field);
    void drop_index(std::string_view field);
    [[nodiscard]] bool disk_backed() const noexcept;

    // Releases storage resources (open index files). The database must not be
    // used afterwards.
    void close();

    [[nodiscard]] std::shared_ptr<const Object> get(std::uint64_t id) const;
    [[nodiscard]] bool remove(std::uint64_t id);
    [[nodiscard]] bool contains(std::uint64_t id) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::vector<std::uint64_t> ids() const;
    [[nodiscard]] std::vector<std::uint64_t> ids(std::size_t offset, std::size_t limit) const;
    [[nodiscard]] IdPage ids_after(std::optional<std::uint64_t> after_id, std::size_t limit) const;

    // Equality match on a top-level field. JSON type participates in equality.
    [[nodiscard]] std::vector<ObjectMatch> find(std::string_view field, const Object& value) const;
    [[nodiscard]] ObjectMatchPage find(std::string_view field, const Object& value,
                                       std::size_t offset, std::size_t limit) const;
    [[nodiscard]] ObjectMatchPage find_after(std::string_view field, const Object& value,
                                             std::optional<std::uint64_t> after_id,
                                             std::size_t limit) const;
    // Filtered query with comparison/boolean operators. Empty leaves are treated
    // as "match everything"; returns a page whose `total` reflects the full
    // matching set. Indexes are not consulted: the predicate runs on each
    // object, which keeps indexing out of the query semantics.
    [[nodiscard]] ObjectMatchPage query(const FilterNode& filter, std::size_t offset,
                                        std::size_t limit) const;
    [[nodiscard]] ObjectMatchPage query(const FilterNode& filter,
                                       std::optional<std::uint64_t> after_id,
                                       std::size_t limit) const;
    [[nodiscard]] std::uint64_t next_id() const noexcept;

  private:
    struct ValueHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    using ObjectPtr = std::shared_ptr<const Object>;
    using ValueIds =
        std::unordered_map<std::string, std::set<std::uint64_t>, ValueHash, std::equal_to<>>;
    using FieldIndex = std::unordered_map<std::string, ValueIds, ValueHash, std::equal_to<>>;

    void add_to_index(std::uint64_t id, const Object& object);
    void remove_from_index(std::uint64_t id, const Object& object);

    std::unordered_map<std::uint64_t, ObjectPtr> objects_;
    std::set<std::uint64_t> ordered_ids_;
    FieldIndex field_index_;
    std::unique_ptr<ObjectStorage> storage_;
    ObjectSchema schema_ = {
        {"type", "object"}, {"properties", Object::object()}, {"additionalProperties", true}};
    std::uint64_t maximum_objects_per_segment_ = 0;
    std::uint64_t next_id_ = 0;

    // Schema-migration bookkeeping. The cursor walks ids in ascending order up
    // to the highest id that existed when the migration began, so objects
    // created afterwards (which already satisfy the new schema) are skipped.
    std::optional<std::uint64_t> migration_cursor_;
    std::uint64_t migration_limit_id_ = 0;
    std::size_t migration_total_ = 0;
    std::size_t migration_processed_ = 0;
    bool migration_pending_ = false;
};

} // namespace minidb
