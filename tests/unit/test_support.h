#pragma once

#include "minidb/database.h"
#include "minidb/storage.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace test_support {

// Creates a uniquely named directory below the system temp directory and removes
// it (and everything inside) on destruction. The prefix keeps the different
// test executables distinguishable in the temp directory.
class TemporaryDirectory {
  public:
    explicit TemporaryDirectory(std::string prefix) {
        std::random_device random;
        for (int attempt = 0; attempt < 10; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    (prefix +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(random()));
            if (std::filesystem::create_directory(path_)) {
                return;
            }
        }
        throw std::runtime_error("Could not create a unique temporary test directory.");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

inline constexpr std::uint64_t small_segment_size = 1;

inline minidb::ObjectSchema default_object_schema() {
    return {{"type", "object"},
            {"properties", minidb::Object::object()},
            {"additionalProperties", true}};
}

inline std::unique_ptr<minidb::SegmentedStorage>
make_disk_storage(const std::filesystem::path& directory, std::vector<std::string> indexed_fields,
                  std::uint64_t cache_bytes) {
    return std::make_unique<minidb::SegmentedStorage>(
        directory, "users", 10000, default_object_schema(), std::move(indexed_fields), cache_bytes);
}

} // namespace test_support
