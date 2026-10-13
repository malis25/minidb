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
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace minidb {

namespace {

constexpr std::array<char, 8> legacy_segment_signature{'M', 'I', 'N', 'I', 'D', 'B', '2', '\0'};
constexpr std::array<char, 8> segment_signature{'M', 'I', 'N', 'I', 'D', 'B', '3', '\0'};
constexpr std::string_view legacy_manifest_format = "MINIDB-MANIFEST-1";
constexpr std::string_view batch_manifest_format = "MINIDB-MANIFEST-2";
constexpr std::string_view manifest_format = "MINIDB-MANIFEST-3";
constexpr char store_operation = 'S';
constexpr char erase_operation = 'E';
constexpr char batch_operation = 'T';
constexpr std::uint64_t legacy_header_size = legacy_segment_signature.size();
constexpr std::uint64_t header_slot_size = 3 * sizeof(std::uint64_t);
constexpr std::uint64_t header_size = segment_signature.size() + 2 * header_slot_size;
constexpr std::uint64_t header_checksum_salt = 0x4d494e4944423300ULL;
constexpr std::uint64_t legacy_default_objects_per_segment = 10000;
constexpr std::uint64_t record_length_size = sizeof(std::uint64_t);
constexpr std::uint64_t record_prefix_size = 1 + sizeof(std::uint64_t);

struct SegmentHeader {
    bool current_format = false;
    std::uint64_t generation = 0;
    std::uint64_t object_count = 0;
    std::uint64_t bytes = 0;
};

std::uint64_t header_checksum(std::uint64_t generation, std::uint64_t object_count) {
    return header_checksum_salt ^ generation ^ object_count;
}

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

bool read_header_slot(std::istream& input, std::uint64_t& generation, std::uint64_t& object_count) {
    try {
        generation = read_u64(input);
        object_count = read_u64(input);
        const auto checksum = read_u64(input);
        return checksum == header_checksum(generation, object_count);
    } catch (const std::runtime_error&) {
        return false;
    }
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
                                   std::uint64_t maximum_objects_per_segment, ObjectSchema schema)
    : directory_(std::move(directory)), database_name_(std::move(database_name)),
      maximum_objects_per_segment_(maximum_objects_per_segment), schema_(std::move(schema)) {
    if (directory_.empty()) {
        throw std::invalid_argument("Database directory cannot be empty.");
    }
    if (!is_valid_database_name(database_name_)) {
        throw std::invalid_argument("Invalid database name.");
    }
    if (maximum_objects_per_segment_ == 0) {
        maximum_objects_per_segment_ = legacy_default_objects_per_segment;
    }
    if (!schema_.is_null()) {
        std::string schema_error;
        if (!validate_object_schema(schema_, &schema_error)) {
            throw std::invalid_argument("Invalid object schema: " + schema_error);
        }
    }
    manifest_path_ = directory_ / (database_name_ + ".manifest.json");
}

const std::filesystem::path& SegmentedStorage::manifest_path() const noexcept {
    return manifest_path_;
}

std::uint64_t SegmentedStorage::maximum_objects_per_segment() const noexcept {
    return maximum_objects_per_segment_;
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
        if (schema_.is_null()) {
            throw std::invalid_argument("New databases require an object schema.");
        }
        segment_files_ = {first_segment};
        segment_object_counts_ = {0};
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
        if ((manifest_version != legacy_manifest_format &&
             manifest_version != batch_manifest_format && manifest_version != manifest_format) ||
            !manifest.contains("next_id") || !manifest["next_id"].is_number_unsigned() ||
            !manifest.contains("segments") || !manifest["segments"].is_array() ||
            manifest["segments"].empty()) {
            throw std::runtime_error("Database manifest has an unsupported or invalid format.");
        }
        if (manifest_version == manifest_format) {
            if (!manifest.contains("maximum_objects_per_segment") ||
                !manifest["maximum_objects_per_segment"].is_number_unsigned() ||
                manifest["maximum_objects_per_segment"] == 0 || !manifest.contains("schema") ||
                !manifest["schema"].is_object()) {
                throw std::runtime_error("Database manifest is missing schema or segment limits.");
            }
            maximum_objects_per_segment_ =
                manifest["maximum_objects_per_segment"].get<std::uint64_t>();
            schema_ = manifest["schema"];
            std::string schema_error;
            if (!validate_object_schema(schema_, &schema_error)) {
                throw std::runtime_error("Database manifest contains an invalid schema: " +
                                         schema_error);
            }
        } else {
            maximum_objects_per_segment_ = legacy_default_objects_per_segment;
            schema_ = {{"type", "object"},
                       {"properties", Object::object()},
                       {"additionalProperties", true}};
            manifest_needs_save = true;
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
                        is_recoverable_empty_segment =
                            orphan && signature == segment_signature &&
                            std::filesystem::file_size(orphan_path) == header_size;
                    }
                    if (!is_recoverable_empty_segment) {
                        throw std::runtime_error(
                            "Database directory contains an unexpected segment file.");
                    }
                    segment_files_.push_back(filename);
                    segment_object_counts_.push_back(0);
                    manifest_needs_save = true;
                }
            }
        }
    }

    auto recovered_next_id = next_id_;
    std::unordered_set<std::uint64_t> stored_ids;
    segment_object_counts_.resize(segment_files_.size(), 0);
    std::vector<bool> legacy_segments(segment_files_.size(), false);
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

        std::ifstream input(segment_path, std::ios::binary);
        std::array<char, segment_signature.size()> signature{};
        input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        if (!input || (signature != segment_signature && signature != legacy_segment_signature)) {
            throw std::runtime_error("Invalid database segment: " + segment_path.string());
        }

        std::uint64_t valid_size = legacy_header_size;
        if (signature == segment_signature) {
            legacy_segments[index] = false;
            std::uint64_t first_generation = 0;
            std::uint64_t first_count = 0;
            const bool first_valid = read_header_slot(input, first_generation, first_count);
            std::uint64_t second_generation = 0;
            std::uint64_t second_count = 0;
            const bool second_valid = read_header_slot(input, second_generation, second_count);
            if (!first_valid && !second_valid) {
                throw std::runtime_error("Database segment header counters are corrupt: " +
                                         segment_path.string());
            }
            const auto selected =
                !second_valid || (first_valid && first_generation >= second_generation)
                    ? std::pair{first_generation, first_count}
                    : std::pair{second_generation, second_count};
            segment_object_counts_[index] = selected.second;
            valid_size = header_size;
        } else {
            legacy_segments[index] = true;
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(valid_size), std::ios::beg);
        if (!input) {
            throw std::runtime_error("Could not seek to segment records: " + segment_path.string());
        }
        const auto recorded_object_count = segment_object_counts_[index];
        std::uint64_t recovered_object_count = 0;
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
                if (stored_ids.insert(id).second) {
                    ++recovered_object_count;
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
                        if (stored_ids.insert(mutation_id).second) {
                            ++recovered_object_count;
                        }
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
        segment_object_counts_[index] = recovered_object_count;
        input.close();
        if (legacy_segments[index]) {
            if (recovered_object_count > maximum_objects_per_segment_) {
                maximum_objects_per_segment_ = recovered_object_count;
            }
            migrate_legacy_segment(segment_files_[index], recovered_object_count);
            manifest_needs_save = true;
        } else if (recorded_object_count != recovered_object_count) {
            update_segment_object_count(segment_path, recovered_object_count);
        }
        if (segment_object_counts_[index] > maximum_objects_per_segment_) {
            throw std::runtime_error("Database segment exceeds its configured object-count limit.");
        }
    }

    if (recovered_next_id != next_id_ || manifest_needs_save) {
        next_id_ = recovered_next_id;
        save_manifest();
    }

    StorageSnapshot snapshot;
    snapshot.next_id = next_id_;
    snapshot.schema = schema_;
    snapshot.maximum_objects_per_segment = maximum_objects_per_segment_;
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
    const auto created_objects = static_cast<std::uint64_t>(
        std::count_if(mutations.begin(), mutations.end(), [&](const auto& mutation) {
            return mutation.type == StorageMutationType::store && mutation.id >= next_id_;
        }));
    append_payload(std::move(record), next_id, true, created_objects);
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
    const auto created_objects =
        object != nullptr && id == next_id_ && next_id > next_id_ ? 1U : 0U;
    append_payload(std::move(record), next_id, true, created_objects);
}

void SegmentedStorage::append_payload(std::string record, std::uint64_t next_id,
                                      bool update_next_id, std::uint64_t created_objects) {
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
    if (segment_size < header_size) {
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
    if (created_objects > maximum_objects_per_segment_) {
        throw std::length_error("Batch creates more objects than fit in one segment.");
    }
    if (created_objects > 0 &&
        segment_object_counts_.back() > maximum_objects_per_segment_ - created_objects) {
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
        segment_object_counts_.push_back(0);
        try {
            save_manifest();
        } catch (...) {
            segment_files_.pop_back();
            segment_object_counts_.pop_back();
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

    const auto previous_object_count = segment_object_counts_.back();
    if (created_objects > 0) {
        try {
            update_segment_object_count(segment_path, previous_object_count + created_objects);
            segment_object_counts_.back() = previous_object_count + created_objects;
        } catch (...) {
            std::error_code rollback_error;
            std::filesystem::resize_file(segment_path, segment_size, rollback_error);
            if (rollback_error) {
                throw std::system_error(rollback_error,
                                        "Could not roll back a record after header failure");
            }
            throw;
        }
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
            if (created_objects > 0) {
                update_segment_object_count(segment_path, previous_object_count);
                segment_object_counts_.back() = previous_object_count;
            }
            throw;
        }
    }
}

void SegmentedStorage::save_manifest() {
    const nlohmann::json manifest = {
        {"format", manifest_format},
        {"next_id", next_id_},
        {"maximum_objects_per_segment", maximum_objects_per_segment_},
        {"schema", schema_},
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

void SegmentedStorage::update_schema(const ObjectSchema& schema) {
    std::string error;
    if (!validate_object_schema(schema, &error)) {
        throw std::invalid_argument("Invalid object schema: " + error);
    }
    const auto previous_schema = schema_;
    schema_ = schema;
    try {
        save_manifest();
    } catch (...) {
        schema_ = previous_schema;
        throw;
    }
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
    write_u64(output, 0);
    write_u64(output, 0);
    write_u64(output, header_checksum(0, 0));
    write_u64(output, 0);
    write_u64(output, 0);
    write_u64(output, header_checksum(0, 0));
    output.close();
    if (!output) {
        throw std::runtime_error("Could not initialize database segment: " + segment_path.string());
    }
}

void SegmentedStorage::update_segment_object_count(const std::filesystem::path& path,
                                                   std::uint64_t object_count) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        throw std::runtime_error("Could not open database segment header: " + path.string());
    }
    std::array<char, segment_signature.size()> signature{};
    file.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    if (!file || signature != segment_signature) {
        throw std::runtime_error("Invalid database segment header: " + path.string());
    }
    std::uint64_t first_generation = 0;
    std::uint64_t first_count = 0;
    const bool first_valid = read_header_slot(file, first_generation, first_count);
    std::uint64_t second_generation = 0;
    std::uint64_t second_count = 0;
    const bool second_valid = read_header_slot(file, second_generation, second_count);
    if (!first_valid && !second_valid) {
        throw std::runtime_error("Database segment header counters are corrupt: " + path.string());
    }
    const bool first_is_latest =
        first_valid && (!second_valid || first_generation >= second_generation);
    const auto generation = (first_is_latest ? first_generation : second_generation) + 1;
    const auto slot = generation % 2;
    file.clear();
    file.seekp(static_cast<std::streamoff>(segment_signature.size() + slot * header_slot_size),
               std::ios::beg);
    write_u64(file, generation);
    write_u64(file, object_count);
    write_u64(file, header_checksum(generation, object_count));
    file.flush();
    if (!file) {
        throw std::runtime_error("Could not update database segment object count: " +
                                 path.string());
    }
}

void SegmentedStorage::migrate_legacy_segment(const std::string& filename,
                                              std::uint64_t object_count) {
    const auto path = directory_ / filename;
    auto temporary = path;
    temporary += ".v3.tmp";
    if (path_exists_without_following_symlinks(temporary)) {
        require_regular_file(temporary, "Temporary upgraded segment");
        std::filesystem::remove(temporary);
    }
    std::ifstream source(path, std::ios::binary);
    if (!source) {
        throw std::runtime_error("Could not open legacy segment for migration: " + path.string());
    }
    source.seekg(static_cast<std::streamoff>(legacy_header_size), std::ios::beg);
    std::ofstream destination(temporary, std::ios::binary | std::ios::trunc);
    if (!destination) {
        throw std::runtime_error("Could not create upgraded segment: " + temporary.string());
    }
    destination.write(segment_signature.data(),
                      static_cast<std::streamsize>(segment_signature.size()));
    write_u64(destination, 0);
    write_u64(destination, object_count);
    write_u64(destination, header_checksum(0, object_count));
    write_u64(destination, 0);
    write_u64(destination, object_count);
    write_u64(destination, header_checksum(0, object_count));
    destination << source.rdbuf();
    destination.close();
    source.close();
    if (!destination || source.bad()) {
        throw std::runtime_error("Could not finish migrating segment: " + path.string());
    }
    replace_file(temporary, path);
}

} // namespace minidb
