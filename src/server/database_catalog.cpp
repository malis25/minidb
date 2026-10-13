#include "database_catalog.h"

#include "minidb/storage.h"

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

class DatabaseDirectoryLock {
  public:
    explicit DatabaseDirectoryLock(const std::filesystem::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "Database directory is already in use or cannot be locked.");
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle_, &information) ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("Database lock path must be a regular file.");
        }
#else
        int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        descriptor_ = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
        if (descriptor_ < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "Could not open database directory lock.");
        }
        struct stat information {};
        if (::fstat(descriptor_, &information) != 0 || !S_ISREG(information.st_mode)) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("Database lock path must be a regular file.");
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const auto error = errno;
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::system_error(error, std::generic_category(),
                                    "Database directory is already in use.");
        }
#endif
    }

    ~DatabaseDirectoryLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
#endif
    }

    DatabaseDirectoryLock(const DatabaseDirectoryLock&) = delete;
    DatabaseDirectoryLock& operator=(const DatabaseDirectoryLock&) = delete;

  private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

namespace {

bool has_database_files(const std::filesystem::path& directory, const std::string& name) {
    if (std::filesystem::exists(directory / (name + ".manifest.json"))) {
        return true;
    }
    const auto prefix = name + ".segment-";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto filename = entry.path().filename().string();
        if (filename.starts_with(prefix) && filename.ends_with(".minidb")) {
            return true;
        }
    }
    return false;
}

bool is_real_directory(const std::filesystem::directory_entry& entry) {
    return std::filesystem::is_directory(entry.symlink_status());
}

} // namespace

ManagedDatabase::ManagedDatabase(std::unique_ptr<minidb::ObjectStorage> storage)
    : database(std::move(storage)) {}

DatabaseCatalog::DatabaseCatalog(std::filesystem::path root_directory,
                                 std::uint64_t default_cache_bytes)
    : root_directory_(std::move(root_directory)), archive_directory_(root_directory_ / ".archive"),
      default_cache_bytes_(default_cache_bytes) {
    if (root_directory_.empty()) {
        throw std::invalid_argument("Database root directory cannot be empty.");
    }
    std::filesystem::create_directories(root_directory_);
    directory_lock_ = std::make_unique<DatabaseDirectoryLock>(root_directory_ / ".minidb.lock");
    load_existing_databases();
}

DatabaseCatalog::~DatabaseCatalog() = default;

bool DatabaseCatalog::is_valid_name(const std::string& name) noexcept {
    return minidb::is_valid_database_name(name);
}

std::uint64_t DatabaseCatalog::default_cache_bytes() const noexcept {
    return default_cache_bytes_;
}

std::vector<std::string> DatabaseCatalog::active_names() const {
    std::lock_guard lock(catalog_mutex_);
    std::vector<std::string> names;
    names.reserve(active_databases_.size());
    for (const auto& [name, database] : active_databases_) {
        static_cast<void>(database);
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> DatabaseCatalog::archived_names() const {
    std::lock_guard lock(catalog_mutex_);
    std::vector<std::string> names;
    names.reserve(archived_databases_.size());
    for (const auto& [name, directory] : archived_databases_) {
        static_cast<void>(directory);
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::shared_ptr<ManagedDatabase> DatabaseCatalog::find_active(const std::string& name) const {
    std::lock_guard lock(catalog_mutex_);
    const auto database = active_databases_.find(name);
    return database == active_databases_.end() ? nullptr : database->second;
}

bool DatabaseCatalog::create(const std::string& name, const minidb::ObjectSchema& schema,
                             std::uint64_t maximum_objects_per_segment,
                             std::vector<std::string> indexed_fields,
                             std::uint64_t cache_bytes) {
    if (!is_valid_name(name)) {
        throw std::invalid_argument("Database name must start with a lowercase letter and contain "
                                    "only lowercase letters, digits, '-' or '_'.");
    }
    std::string schema_error;
    if (!minidb::validate_object_schema(schema, &schema_error) ||
        !minidb::is_strict_object_schema(schema)) {
        throw std::invalid_argument(
            "Invalid strict database object schema: " +
            (schema_error.empty() ? "additional properties are allowed." : schema_error));
    }
    if (maximum_objects_per_segment == 0) {
        throw std::invalid_argument("maximum_objects_per_segment must be positive.");
    }

    std::lock_guard lock(catalog_mutex_);
    if (active_databases_.contains(name) || archived_databases_.contains(name) ||
        std::filesystem::exists(archive_directory_ / name)) {
        return false;
    }

    const auto directory = root_directory_ / name;
    if (!std::filesystem::create_directory(directory)) {
        return false;
    }
    try {
        auto storage = std::make_unique<minidb::SegmentedStorage>(
            directory, name, maximum_objects_per_segment, schema, std::move(indexed_fields),
            cache_bytes);
        auto database = std::make_shared<ManagedDatabase>(std::move(storage));
        const auto [entry, inserted] = active_databases_.emplace(name, std::move(database));
        static_cast<void>(entry);
        if (!inserted) {
            throw std::logic_error("Database was registered while holding the catalog lock.");
        }
    } catch (const std::exception& error) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory, cleanup_error);
        if (cleanup_error) {
            throw std::runtime_error(
                std::string(error.what()) +
                "; failed to clean up incomplete database directory: " + cleanup_error.message());
        }
        throw;
    }
    return true;
}

DatabaseCatalog::RemoveResult DatabaseCatalog::remove(const std::string& name, bool purge) {
    std::lock_guard catalog_lock(catalog_mutex_);
    const auto database = active_databases_.find(name);
    if (database == active_databases_.end()) {
        return RemoveResult::not_found;
    }

    const auto active_directory = root_directory_ / name;
    const auto managed_database = database->second;
    std::lock_guard database_lock(managed_database->mutex);
    if (purge) {
        std::error_code error;
        std::filesystem::remove_all(active_directory, error);
        if (error) {
            throw std::system_error(error, "Could not permanently remove database: " + name);
        }
    } else {
        const auto archive_directory = archive_directory_ / name;
        if (archived_databases_.contains(name) || std::filesystem::exists(archive_directory)) {
            return RemoveResult::archive_exists;
        }
        std::filesystem::create_directories(archive_directory_);
        const auto [archive_entry, inserted] = archived_databases_.emplace(name, archive_directory);
        if (!inserted) {
            return RemoveResult::archive_exists;
        }
        std::error_code error;
        std::filesystem::rename(active_directory, archive_directory, error);
        if (error) {
            archived_databases_.erase(archive_entry);
            throw std::system_error(error, "Could not archive database: " + name);
        }
    }
    managed_database->active = false;
    active_databases_.erase(database);
    return RemoveResult::removed;
}

DatabaseCatalog::RestoreResult DatabaseCatalog::restore(const std::string& name) {
    std::lock_guard lock(catalog_mutex_);
    if (active_databases_.contains(name)) {
        return RestoreResult::already_active;
    }
    const auto archived = archived_databases_.find(name);
    if (archived == archived_databases_.end()) {
        return RestoreResult::not_found;
    }

    const auto active_directory = root_directory_ / name;
    std::error_code move_error;
    std::filesystem::rename(archived->second, active_directory, move_error);
    if (move_error) {
        throw std::system_error(move_error, "Could not restore database: " + name);
    }
    try {
        auto database = open_database(active_directory, name);
        const auto [active, inserted] = active_databases_.emplace(name, std::move(database));
        static_cast<void>(active);
        if (!inserted) {
            throw std::logic_error("Database was registered while holding the catalog lock.");
        }
    } catch (const std::exception& error) {
        std::error_code rollback_error;
        std::filesystem::rename(active_directory, archived->second, rollback_error);
        if (rollback_error) {
            throw std::runtime_error(
                std::string(error.what()) +
                "; could not return failed restore to archive: " + rollback_error.message());
        }
        throw;
    }
    archived_databases_.erase(archived);
    return RestoreResult::restored;
}

std::shared_ptr<ManagedDatabase>
DatabaseCatalog::open_database(const std::filesystem::path& directory,
                               const std::string& name) const {
    auto storage = std::make_unique<minidb::SegmentedStorage>(directory, name);
    return std::make_shared<ManagedDatabase>(std::move(storage));
}

void DatabaseCatalog::load_existing_databases() {
    for (const auto& entry : std::filesystem::directory_iterator(root_directory_)) {
        const auto name = entry.path().filename().string();
        if (name == ".archive" || !is_real_directory(entry)) {
            continue;
        }
        if (!is_valid_name(name)) {
            if (std::filesystem::exists(entry.path() / (name + ".manifest.json"))) {
                throw std::runtime_error("Invalid database directory name: " + name);
            }
            continue;
        }
        if (has_database_files(entry.path(), name)) {
            active_databases_.emplace(name, open_database(entry.path(), name));
        }
    }

    if (!std::filesystem::exists(archive_directory_)) {
        return;
    }
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(archive_directory_))) {
        throw std::runtime_error("Database archive path is not a directory.");
    }
    for (const auto& entry : std::filesystem::directory_iterator(archive_directory_)) {
        const auto name = entry.path().filename().string();
        if (!is_real_directory(entry)) {
            continue;
        }
        if (!is_valid_name(name)) {
            if (std::filesystem::exists(entry.path() / (name + ".manifest.json"))) {
                throw std::runtime_error("Invalid archived database directory name: " + name);
            }
            continue;
        }
        if (active_databases_.contains(name)) {
            throw std::runtime_error("Database exists as both active and archived: " + name);
        }
        if (has_database_files(entry.path(), name)) {
            archived_databases_.emplace(name, entry.path());
        }
    }
}
