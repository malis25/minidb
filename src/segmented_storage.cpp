#include "minidb/storage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace minidb {

namespace {

constexpr std::array<char, 8> segment_signature{'M', 'I', 'N', 'I', 'D', 'B', '2', '\0'};
constexpr std::string_view legacy_manifest_format = "MINIDB-MANIFEST-1";
constexpr std::string_view manifest_format = "MINIDB-MANIFEST-2";
constexpr char store_operation = 'S';
constexpr char erase_operation = 'E';
constexpr char batch_operation = 'T';
constexpr std::uint64_t header_size = segment_signature.size();
constexpr std::uint64_t record_length_size = sizeof(std::uint64_t);
constexpr std::uint64_t record_prefix_size = 1 + sizeof(std::uint64_t);
constexpr std::uint64_t minimum_object_record_size =
    header_size + record_length_size + record_prefix_size + sizeof(std::uint64_t) + 2;

bool path_exists_without_following_symlinks(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return false;
    }
    if (error) {
        throw std::system_error(error, "Could not inspect database path: " + path.string());
    }
    return status.type() != std::filesystem::file_type::not_found;
}

void require_regular_file(const std::filesystem::path& path, std::string_view description) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        throw std::system_error(error, "Could not inspect " + std::string(description) + ": " +
                                           path.string());
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error(std::string(description) +
                                 " is not a regular file: " + path.string());
    }
}

bool is_segment_filename(std::string_view filename, std::string_view database_name) {
    const auto prefix = std::string(database_name) + ".segment-";
    constexpr std::string_view suffix = ".minidb";
    return filename.starts_with(prefix) && filename.ends_with(suffix);
}

void append_u64(std::string& output, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

std::uint64_t read_u64(std::string_view bytes, std::size_t& offset) {
    if (bytes.size() - offset < sizeof(std::uint64_t)) {
        throw std::runtime_error("Database record is truncated.");
    }
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset++])) << shift;
    }
    return value;
}

void write_u64(std::ostream& output, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output.put(static_cast<char>((value >> shift) & 0xff));
    }
}

std::uint64_t read_u64(std::istream& input) {
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        const auto byte = input.get();
        if (byte == std::char_traits<char>::eof()) {
            throw std::runtime_error("Database record length is truncated.");
        }
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte)) << shift;
    }
    return value;
}

void replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Could not replace metadata file: " + destination.string());
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

bool is_valid_database_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 63 || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    const bool valid_characters =
        std::all_of(name.begin() + 1, name.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '-' || character == '_';
        });
    if (!valid_characters) {
        return false;
    }
    if (name == "con" || name == "prn" || name == "aux" || name == "nul") {
        return false;
    }
    return !((name.size() == 4 && name.starts_with("com")) ||
             (name.size() == 4 && name.starts_with("lpt"))) ||
           (name.back() < '1' || name.back() > '9');
}

SegmentedStorage::SegmentedStorage(std::filesystem::path directory, std::string database_name,
                                   std::uint64_t maximum_segment_size)
    : directory_(std::move(directory)), database_name_(std::move(database_name)),
      maximum_segment_size_(maximum_segment_size) {
    if (directory_.empty()) {
        throw std::invalid_argument("Database directory cannot be empty.");
    }
    if (!is_valid_database_name(database_name_)) {
        throw std::invalid_argument("Invalid database name.");
    }
    if (maximum_segment_size_ < minimum_object_record_size) {
        throw std::invalid_argument("Maximum file size is too small for a database object.");
    }
    manifest_path_ = directory_ / (database_name_ + ".manifest.json");
}

const std::filesystem::path& SegmentedStorage::manifest_path() const noexcept {
    return manifest_path_;
}

StorageSnapshot SegmentedStorage::load() {
    std::filesystem::create_directories(directory_);
    std::unordered_map<std::uint64_t, Object> objects;
    const bool manifest_exists = path_exists_without_following_symlinks(manifest_path_);
    if (manifest_exists) {
        require_regular_file(manifest_path_, "Database manifest");
    }
    const bool initialize = !manifest_exists;
    bool manifest_needs_save = false;
    bool has_existing_segments = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (is_segment_filename(entry.path().filename().string(), database_name_)) {
            has_existing_segments = true;
            break;
        }
    }

    if (initialize) {
        const auto first_segment = database_name_ + ".segment-00000000.minidb";
        if (has_existing_segments) {
            throw std::runtime_error(
                "Database manifest is missing while segment files already exist.");
        }
        next_id_ = 0;
        segment_files_ = {first_segment};
        save_manifest();
        create_segment(first_segment);
    } else {
        std::ifstream manifest_input(manifest_path_, std::ios::binary);
        if (!manifest_input) {
            throw std::runtime_error("Could not open database manifest: " +
                                     manifest_path_.string());
        }
        nlohmann::json manifest;
        try {
            manifest_input >> manifest;
        } catch (const nlohmann::json::exception& error) {
            throw std::runtime_error("Invalid database manifest: " + std::string(error.what()));
        }
        if (!manifest.is_object()) {
            throw std::runtime_error("Database manifest has an unsupported or invalid format.");
        }
        const auto manifest_version = manifest.value("format", std::string{});
        if ((manifest_version != legacy_manifest_format && manifest_version != manifest_format) ||
            !manifest.contains("next_id") || !manifest["next_id"].is_number_unsigned() ||
            !manifest.contains("maximum_segment_size") ||
            !manifest["maximum_segment_size"].is_number_unsigned() ||
            manifest["maximum_segment_size"].get<std::uint64_t>() != maximum_segment_size_ ||
            !manifest.contains("segments") || !manifest["segments"].is_array() ||
            manifest["segments"].empty()) {
            throw std::runtime_error("Database manifest has an unsupported or invalid format.");
        }
        next_id_ = manifest["next_id"].get<std::uint64_t>();
        segment_files_.clear();
        segment_files_.reserve(manifest["segments"].size());
        for (std::size_t index = 0; index < manifest["segments"].size(); ++index) {
            if (!manifest["segments"][index].is_string()) {
                throw std::runtime_error("Database manifest contains an invalid segment name.");
            }
            std::ostringstream expected_name;
            expected_name << database_name_ << ".segment-" << std::setw(8) << std::setfill('0')
                          << index << ".minidb";
            const auto name = manifest["segments"][index].get<std::string>();
            if (name != expected_name.str()) {
                throw std::runtime_error("Database manifest contains an unexpected segment name.");
            }
            segment_files_.push_back(name);
        }

        for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
            const auto filename = entry.path().filename().string();
            if (is_segment_filename(filename, database_name_)) {
                require_regular_file(entry.path(), "Database segment");
                if (std::find(segment_files_.begin(), segment_files_.end(), filename) ==
                    segment_files_.end()) {
                    std::ostringstream expected_name;
                    expected_name << database_name_ << ".segment-" << std::setw(8)
                                  << std::setfill('0') << segment_files_.size() << ".minidb";
                    const auto orphan_path = entry.path();
                    bool is_recoverable_empty_segment =
                        filename == expected_name.str() &&
                        std::filesystem::file_size(orphan_path) == header_size;
                    if (is_recoverable_empty_segment) {
                        std::ifstream orphan(orphan_path, std::ios::binary);
                        std::array<char, segment_signature.size()> signature{};
                        orphan.read(signature.data(),
                                    static_cast<std::streamsize>(signature.size()));
                        is_recoverable_empty_segment = orphan && signature == segment_signature;
                    }
                    if (!is_recoverable_empty_segment) {
                        throw std::runtime_error(
                            "Database directory contains an unexpected segment file.");
                    }
                    segment_files_.push_back(filename);
                    manifest_needs_save = true;
                }
            }
        }
    }

    auto recovered_next_id = next_id_;
    for (std::size_t index = 0; index < segment_files_.size(); ++index) {
        const auto segment_path = directory_ / segment_files_[index];
        if (!path_exists_without_following_symlinks(segment_path)) {
            if (index + 1 != segment_files_.size() || segment_files_.size() != 1 || next_id_ != 0) {
                throw std::runtime_error(
                    "A database segment listed in the manifest is missing; refusing to "
                    "reinitialize potentially lost data.");
            }
            create_segment(segment_files_[index]);
        }
        require_regular_file(segment_path, "Database segment");
        const auto segment_size = std::filesystem::file_size(segment_path);
        if (segment_size > maximum_segment_size_) {
            throw std::runtime_error("Database segment exceeds the configured maximum size.");
        }

        std::ifstream input(segment_path, std::ios::binary);
        std::array<char, segment_signature.size()> signature{};
        input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        if (!input || signature != segment_signature) {
            throw std::runtime_error("Invalid database segment: " + segment_path.string());
        }

        std::uint64_t valid_size = header_size;
        while (valid_size < segment_size) {
            const auto record_start = valid_size;
            const auto remaining = segment_size - record_start;
            if (remaining < record_length_size) {
                if (index + 1 != segment_files_.size()) {
                    throw std::runtime_error(
                        "An incomplete record was found in a non-final database segment.");
                }
                input.close();
                std::filesystem::resize_file(segment_path, record_start);
                break;
            }
            const auto record_size = read_u64(input);
            valid_size += record_length_size;
            if (record_size > segment_size - valid_size) {
                if (index + 1 != segment_files_.size()) {
                    throw std::runtime_error(
                        "An incomplete record was found in a non-final database segment.");
                }
                input.close();
                std::filesystem::resize_file(segment_path, record_start);
                break;
            }
            if (record_size < record_prefix_size ||
                record_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
                record_size >
                    static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
                throw std::runtime_error("Database segment contains an invalid record size.");
            }

            std::string record(static_cast<std::size_t>(record_size), '\0');
            input.read(record.data(), static_cast<std::streamsize>(record.size()));
            if (!input) {
                throw std::runtime_error("Database segment record is truncated.");
            }
            valid_size += record_size;

            const auto operation = record.front();
            std::size_t offset = 1;
            const auto id = read_u64(record, offset);
            if (id == std::numeric_limits<std::uint64_t>::max()) {
                recovered_next_id = id;
            } else {
                recovered_next_id = std::max(recovered_next_id, id + 1);
            }
            if (operation == store_operation) {
                const auto object_size = read_u64(record, offset);
                if (object_size != record.size() - offset) {
                    throw std::runtime_error("Database object record has an invalid payload size.");
                }
                auto object = Object::parse(
                    std::string_view{record}.substr(offset),
                    [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                        if (depth > maximum_object_nesting_depth) {
                            throw std::length_error(
                                "Stored JSON nesting exceeds the maximum depth of 128.");
                        }
                        return true;
                    });
                if (!object.is_object()) {
                    throw std::runtime_error("Database record does not contain a JSON object.");
                }
                objects.insert_or_assign(id, std::move(object));
            } else if (operation == erase_operation) {
                if (offset != record.size()) {
                    throw std::runtime_error("Database deletion record contains trailing data.");
                }
                objects.erase(id);
            } else if (operation == batch_operation) {
                const auto mutation_count = read_u64(record, offset);
                if (mutation_count == 0 ||
                    mutation_count >
                        static_cast<std::uint64_t>(record.size() - offset) / record_prefix_size) {
                    throw std::runtime_error(
                        "Database batch record has an invalid operation count.");
                }
                std::vector<StorageMutation> mutations;
                mutations.reserve(static_cast<std::size_t>(mutation_count));
                for (std::uint64_t mutation_index = 0; mutation_index < mutation_count;
                     ++mutation_index) {
                    if (offset == record.size()) {
                        throw std::runtime_error("Database batch record is truncated.");
                    }
                    const auto mutation_operation = record[offset++];
                    const auto mutation_id = read_u64(record, offset);
                    if (mutation_operation == store_operation) {
                        const auto object_size = read_u64(record, offset);
                        if (object_size > record.size() - offset) {
                            throw std::runtime_error("Database batch object is truncated.");
                        }
                        auto object = Object::parse(
                            std::string_view{record}.substr(offset,
                                                            static_cast<std::size_t>(object_size)),
                            [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                                if (depth > maximum_object_nesting_depth) {
                                    throw std::length_error(
                                        "Stored JSON nesting exceeds the maximum depth of 128.");
                                }
                                return true;
                            });
                        if (!object.is_object()) {
                            throw std::runtime_error(
                                "Database batch record does not contain a JSON object.");
                        }
                        offset += static_cast<std::size_t>(object_size);
                        mutations.push_back(
                            {StorageMutationType::store, mutation_id, std::move(object)});
                        if (mutation_id == std::numeric_limits<std::uint64_t>::max()) {
                            recovered_next_id = mutation_id;
                        } else {
                            recovered_next_id = std::max(recovered_next_id, mutation_id + 1);
                        }
                    } else if (mutation_operation == erase_operation) {
                        mutations.push_back({StorageMutationType::erase, mutation_id, {}});
                    } else {
                        throw std::runtime_error(
                            "Database batch record contains an unknown operation.");
                    }
                }
                if (offset != record.size()) {
                    throw std::runtime_error("Database batch record contains trailing data.");
                }
                for (auto& mutation : mutations) {
                    if (mutation.type == StorageMutationType::store) {
                        objects.insert_or_assign(mutation.id, std::move(mutation.object));
                    } else {
                        objects.erase(mutation.id);
                    }
                }
            } else {
                throw std::runtime_error("Database segment contains an unknown operation.");
            }
        }
        if (input.bad()) {
            throw std::runtime_error("Failed to read database segment: " + segment_path.string());
        }
    }

    if (recovered_next_id != next_id_ || manifest_needs_save) {
        next_id_ = recovered_next_id;
        save_manifest();
    }

    StorageSnapshot snapshot;
    snapshot.next_id = next_id_;
    snapshot.objects.reserve(objects.size());
    for (auto& [id, object] : objects) {
        snapshot.objects.push_back({id, std::move(object)});
    }
    return snapshot;
}

void SegmentedStorage::store(std::uint64_t id, const Object& object, std::uint64_t next_id) {
    if (!object.is_object()) {
        throw std::invalid_argument("A database object must be a JSON object.");
    }
    if (!is_within_object_nesting_depth(object)) {
        throw std::invalid_argument("JSON nesting exceeds the maximum depth of 128.");
    }
    if (id > next_id_ || next_id < next_id_ || (id == next_id_ && next_id != id + 1)) {
        throw std::invalid_argument("Object ID and next-ID metadata are inconsistent.");
    }
    append_record(store_operation, id, &object, next_id);
}

void SegmentedStorage::erase(std::uint64_t id) {
    append_record(erase_operation, id, nullptr, next_id_);
}

void SegmentedStorage::apply_batch(const std::vector<StorageMutation>& mutations,
                                   std::uint64_t next_id) {
    if (mutations.empty()) {
        throw std::invalid_argument("A storage batch must contain at least one operation.");
    }
    if (next_id < next_id_) {
        throw std::invalid_argument("Batch next-ID metadata cannot move backwards.");
    }
    if (mutations.size() == 1) {
        const auto& mutation = mutations.front();
        if (mutation.type == StorageMutationType::store) {
            if (!mutation.object.is_object() || !is_within_object_nesting_depth(mutation.object) ||
                mutation.id >= next_id) {
                throw std::invalid_argument("Batch contains an invalid stored object or ID.");
            }
            append_record(store_operation, mutation.id, &mutation.object, next_id);
        } else {
            append_record(erase_operation, mutation.id, nullptr, next_id_);
        }
        return;
    }

    save_manifest();
    std::string record;
    record.push_back(batch_operation);
    append_u64(record, 0);
    append_u64(record, static_cast<std::uint64_t>(mutations.size()));
    for (const auto& mutation : mutations) {
        if (mutation.type == StorageMutationType::store) {
            if (!mutation.object.is_object() || !is_within_object_nesting_depth(mutation.object) ||
                mutation.id >= next_id) {
                throw std::invalid_argument("Batch contains an invalid stored object or ID.");
            }
            const auto json = mutation.object.dump();
            record.push_back(store_operation);
            append_u64(record, mutation.id);
            append_u64(record, static_cast<std::uint64_t>(json.size()));
            record.append(json);
        } else {
            record.push_back(erase_operation);
            append_u64(record, mutation.id);
        }
    }
    append_payload(std::move(record), next_id, true);
}

void SegmentedStorage::append_record(char operation, std::uint64_t id, const Object* object,
                                     std::uint64_t next_id) {
    std::string record;
    record.push_back(operation);
    append_u64(record, id);
    if (object != nullptr) {
        const auto json = object->dump();
        append_u64(record, static_cast<std::uint64_t>(json.size()));
        record.append(json);
    }
    append_payload(std::move(record), next_id, true);
}

void SegmentedStorage::append_payload(std::string record, std::uint64_t next_id,
                                      bool update_next_id) {
    if (segment_files_.empty()) {
        throw std::logic_error("Database storage must be loaded before writing records.");
    }
    if (record.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("Database record is too large to write.");
    }

    auto segment_path = directory_ / segment_files_.back();
    if (!path_exists_without_following_symlinks(segment_path)) {
        throw std::runtime_error("Active database segment is missing: " + segment_path.string());
    }
    require_regular_file(segment_path, "Active database segment");
    auto segment_size = std::filesystem::file_size(segment_path);
    if (segment_size < header_size || segment_size > maximum_segment_size_) {
        throw std::runtime_error("Active database segment has an invalid size: " +
                                 segment_path.string());
    }
    {
        std::ifstream input(segment_path, std::ios::binary);
        std::array<char, segment_signature.size()> signature{};
        input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        if (!input || signature != segment_signature) {
            throw std::runtime_error("Invalid database segment header: " + segment_path.string());
        }
    }
    if (record.size() > std::numeric_limits<std::uint64_t>::max() - record_length_size) {
        throw std::length_error("Database record is too large to write.");
    }
    const auto bytes_to_write = record_length_size + record.size();
    if (bytes_to_write > maximum_segment_size_ - header_size) {
        throw std::length_error("Object is too large to fit in a database segment.");
    }
    if (bytes_to_write > maximum_segment_size_ - segment_size) {
        std::ostringstream next_name;
        next_name << database_name_ << ".segment-" << std::setw(8) << std::setfill('0')
                  << segment_files_.size() << ".minidb";
        const auto next_segment_name = next_name.str();
        const auto next_segment_path = directory_ / next_segment_name;
        if (path_exists_without_following_symlinks(next_segment_path)) {
            throw std::runtime_error("Unexpected next database segment already exists: " +
                                     next_segment_path.string());
        }
        create_segment(next_segment_name);
        segment_files_.push_back(next_segment_name);
        try {
            save_manifest();
        } catch (...) {
            segment_files_.pop_back();
            std::error_code cleanup_error;
            std::filesystem::remove(next_segment_path, cleanup_error);
            if (cleanup_error) {
                throw std::system_error(cleanup_error,
                                        "Could not remove an uncommitted database segment");
            }
            throw;
        }
        segment_path = next_segment_path;
        segment_size = header_size;
    }

    std::ofstream output(segment_path, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("Could not open database segment for appending: " +
                                 segment_path.string());
    }
    write_u64(output, static_cast<std::uint64_t>(record.size()));
    output.write(record.data(), static_cast<std::streamsize>(record.size()));
    output.flush();
    output.close();
    if (!output) {
        std::error_code rollback_error;
        std::filesystem::resize_file(segment_path, segment_size, rollback_error);
        if (rollback_error) {
            throw std::system_error(rollback_error, "Could not roll back an incomplete record");
        }
        throw std::runtime_error("Could not append database record: " + segment_path.string());
    }

    if (update_next_id && next_id != next_id_) {
        const auto previous_next_id = next_id_;
        next_id_ = next_id;
        try {
            save_manifest();
        } catch (...) {
            next_id_ = previous_next_id;
            std::error_code rollback_error;
            std::filesystem::resize_file(segment_path, segment_size, rollback_error);
            if (rollback_error) {
                throw std::system_error(rollback_error,
                                        "Could not roll back a record after manifest failure");
            }
            throw;
        }
    }
}

void SegmentedStorage::save_manifest() {
    const nlohmann::json manifest = {
        {"format", manifest_format},
        {"next_id", next_id_},
        {"maximum_segment_size", maximum_segment_size_},
        {"segments", segment_files_},
    };
    auto temporary_path = manifest_path_;
    temporary_path += ".tmp";
    if (path_exists_without_following_symlinks(temporary_path)) {
        require_regular_file(temporary_path, "Temporary database manifest");
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
}

void SegmentedStorage::create_segment(const std::string& filename) {
    const auto segment_path = directory_ / filename;
    if (path_exists_without_following_symlinks(segment_path)) {
        require_regular_file(segment_path, "Database segment");
        if (std::filesystem::file_size(segment_path) != header_size) {
            throw std::runtime_error("Unexpected data in new database segment: " +
                                     segment_path.string());
        }
        std::ifstream existing(segment_path, std::ios::binary);
        std::array<char, segment_signature.size()> signature{};
        existing.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        if (!existing || signature != segment_signature) {
            throw std::runtime_error("Invalid database segment header: " + segment_path.string());
        }
        return;
    }
    std::ofstream output(segment_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not create database segment: " + segment_path.string());
    }
    output.write(segment_signature.data(), static_cast<std::streamsize>(segment_signature.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("Could not initialize database segment: " + segment_path.string());
    }
}

} // namespace minidb
