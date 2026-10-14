#pragma once

#include "minidb/database.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class DatabaseDirectoryLock;

struct ManagedDatabase {
    explicit ManagedDatabase(std::unique_ptr<minidb::ObjectStorage> storage);
    ~ManagedDatabase();

    ManagedDatabase(const ManagedDatabase&) = delete;
    ManagedDatabase& operator=(const ManagedDatabase&) = delete;

    // Adapts existing objects to a newly installed schema in the background.
    // The worker holds `mutex` only for one bounded chunk at a time, so other
    // requests keep making progress while a large database migrates. The caller
    // must hold `mutex`; starting is only meaningfully possible while no other
    // migration is pending.
    void start_schema_migration();

    // Signals the worker to stop. The caller must hold `mutex`; the worker
    // observes the flag at its next checkpoint and never touches storage again.
    void signal_schema_migration_stop();

    // Waits for the worker to exit. The caller must NOT hold `mutex`.
    void join_schema_migration();

    // Convenience for destruction: signal and join without requiring the caller
    // to manage the locks.
    void stop_schema_migration();

    std::mutex mutex;
    bool active = true;
    minidb::Database database;

  private:
    void run_schema_migration();

    // `migration_thread_` is only touched under `mutex` or after the database is
    // inaccessible, so start/signal/join are serialized without their own lock.
    std::thread migration_thread_;
    bool migration_stop_ = false;
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
