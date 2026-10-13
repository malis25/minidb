#pragma once

#include "minidb/database.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class DatabaseDirectoryLock;

struct ManagedDatabase {
    explicit ManagedDatabase(std::unique_ptr<minidb::ObjectStorage> storage);

    std::mutex mutex;
    bool active = true;
    minidb::Database database;
};

class DatabaseCatalog {
  public:
    enum class RemoveResult { removed, not_found, archive_exists };
    enum class RestoreResult { restored, not_found, already_active };

    explicit DatabaseCatalog(std::filesystem::path root_directory,
                             std::uint64_t default_cache_bytes = 0);
    ~DatabaseCatalog();

    DatabaseCatalog(const DatabaseCatalog&) = delete;
    DatabaseCatalog& operator=(const DatabaseCatalog&) = delete;

    [[nodiscard]] static bool is_valid_name(const std::string& name) noexcept;
    [[nodiscard]] std::uint64_t default_cache_bytes() const noexcept;
    [[nodiscard]] std::vector<std::string> active_names() const;
    [[nodiscard]] std::vector<std::string> archived_names() const;
    [[nodiscard]] std::shared_ptr<ManagedDatabase> find_active(const std::string& name) const;
    [[nodiscard]] bool create(const std::string& name, const minidb::ObjectSchema& schema,
                              std::uint64_t maximum_objects_per_segment,
                              std::vector<std::string> indexed_fields = {},
                              std::uint64_t cache_bytes = 0);
    [[nodiscard]] RemoveResult remove(const std::string& name, bool purge);
    [[nodiscard]] RestoreResult restore(const std::string& name);

  private:
    [[nodiscard]] std::shared_ptr<ManagedDatabase>
    open_database(const std::filesystem::path& directory, const std::string& name) const;
    void load_existing_databases();

    std::filesystem::path root_directory_;
    std::filesystem::path archive_directory_;
    std::unique_ptr<DatabaseDirectoryLock> directory_lock_;
    std::uint64_t default_cache_bytes_ = 0;
    mutable std::mutex catalog_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ManagedDatabase>> active_databases_;
    std::unordered_map<std::string, std::filesystem::path> archived_databases_;
};
