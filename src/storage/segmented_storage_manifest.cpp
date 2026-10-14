#include "minidb/storage.h"

#include "segmented_storage_internal.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

// Manifest persistence: the manifest is the single source of truth for schema,
// segment list, index list, and the next-ID reservation, so it is rewritten with
// a temporary file plus an atomic replace.
namespace minidb {

namespace {

void replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& destination) {
#ifdef _WIN32
    // A virus scanner, search indexer, or backup agent can briefly hold the
    // destination open, which makes MoveFileExW fail even though the replacement
    // is valid. Retry those transient sharing errors before giving up.
    constexpr int maximum_attempts = 50;
    for (int attempt = 0; attempt < maximum_attempts; ++attempt) {
        if (MoveFileExW(temporary.c_str(), destination.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return;
        }
        const DWORD error = GetLastError();
        const bool transient = error == ERROR_ACCESS_DENIED ||
                               error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
        if (!transient || attempt + 1 == maximum_attempts) {
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "Could not replace metadata file: " + destination.string());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        throw std::system_error(error, "Could not replace metadata file: " + destination.string());
    }
#endif
}

} // namespace

void SegmentedStorage::save_manifest() {
    const nlohmann::json manifest = {
        {"format", detail::manifest_format},
        {"next_id", next_id_},
        {"next_id_reservation", next_id_reservation_},
        {"maximum_objects_per_segment", maximum_objects_per_segment_},
        {"schema", schema_},
        {"segments", segment_files_},
        {"indexed_fields", indexed_fields_},
        {"cache_bytes", cache_bytes_},
        {"schema_migration_pending", schema_migration_pending_},
    };
    auto temporary_path = manifest_path_;
    temporary_path += ".tmp";
    if (detail::path_exists_without_following_symlinks(temporary_path)) {
        detail::require_regular_file(temporary_path, "Temporary database manifest");
    }
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not write database manifest: " + temporary_path.string());
    }
    output << manifest.dump(2) << '\n';
    output.close();
    if (!output) {
        throw std::runtime_error("Could not finish writing database manifest: " +
                                 temporary_path.string());
    }
    replace_file(temporary_path, manifest_path_);
    ++manifest_writes_;
}

void SegmentedStorage::collapse_next_id_reservation() {
    if (!loaded_ || closed_ || next_id_reservation_ == next_id_) {
        return;
    }
    next_id_reservation_ = next_id_;
    save_manifest();
}

} // namespace minidb
