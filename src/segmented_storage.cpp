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
constexpr std::string_view manifest_format = "MINIDB-MANIFEST-1";
constexpr char store_operation = 'S';
constexpr char erase_operation = 'E';
constexpr std::uint64_t header_size = segment_signature.size();
constexpr std::uint64_t record_length_size = sizeof(std::uint64_t);
constexpr std::uint64_t record_prefix_size = 1 + sizeof(std::uint64_t);
constexpr std::uint64_t minimum_object_record_size =
    header_size + record_length_size + record_prefix_size + sizeof(std::uint64_t) + 2;

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

SegmentedStorage::SegmentedStorage(std::filesystem::path directory, std::string database_name,
                                   std::uint64_t maximum_segment_size)
    : directory_(std::move(directory)), database_name_(std::move(database_name)),
      maximum_segment_size_(maximum_segment_size) {
    if (directory_.empty()) {
        throw std::invalid_argument("Database directory cannot be empty.");
    }
    const std::filesystem::path name_path{database_name_};
    if (database_name_.empty() || name_path.has_parent_path() || database_name_ == "." ||
        database_name_ == "..") {
        throw std::invalid_argument("Database name must be a file name, not a path.");
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
    bool initialize = !std::filesystem::exists(manifest_path_);
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
        if (!manifest.is_object() || manifest.value("format", std::string{}) != manifest_format ||
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
            if (is_segment_filename(filename, database_name_) &&
                std::find(segment_files_.begin(), segment_files_.end(), filename) ==
                    segment_files_.end()) {
                throw std::runtime_error(
                    "Database directory contains a segment not listed in the manifest.");
            }
        }
    }

    auto recovered_next_id = next_id_;
    for (std::size_t index = 0; index < segment_files_.size(); ++index) {
        const auto segment_path = directory_ / segment_files_[index];
        if (!std::filesystem::exists(segment_path)) {
            if (index + 1 != segment_files_.size()) {
                throw std::runtime_error("A database segment listed in the manifest is missing.");
            }
            create_segment(segment_files_[index]);
        }
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
                auto object = Object::parse(std::string_view{record}.substr(offset));
                if (!object.is_object()) {
                    throw std::runtime_error("Database record does not contain a JSON object.");
                }
                objects.insert_or_assign(id, std::move(object));
            } else if (operation == erase_operation) {
                if (offset != record.size()) {
                    throw std::runtime_error("Database deletion record contains trailing data.");
                }
                objects.erase(id);
            } else {
                throw std::runtime_error("Database segment contains an unknown operation.");
            }
        }
        if (input.bad()) {
            throw std::runtime_error("Failed to read database segment: " + segment_path.string());
        }
    }

    if (recovered_next_id != next_id_) {
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
    if (id > next_id_ || next_id < next_id_ || (id == next_id_ && next_id != id + 1)) {
        throw std::invalid_argument("Object ID and next-ID metadata are inconsistent.");
    }
    append_record(store_operation, id, &object, next_id);
}

void SegmentedStorage::erase(std::uint64_t id) {
    append_record(erase_operation, id, nullptr, next_id_);
}

void SegmentedStorage::append_record(char operation, std::uint64_t id, const Object* object,
                                     std::uint64_t next_id) {
    if (segment_files_.empty()) {
        throw std::logic_error("Database storage must be loaded before writing records.");
    }

    std::string record;
    record.push_back(operation);
    append_u64(record, id);
    if (object != nullptr) {
        const auto json = object->dump();
        append_u64(record, static_cast<std::uint64_t>(json.size()));
        record.append(json);
    }
    if (record.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("Database record is too large to write.");
    }

    auto segment_path = directory_ / segment_files_.back();
    if (!std::filesystem::exists(segment_path)) {
        create_segment(segment_files_.back());
    }
    auto segment_size = std::filesystem::file_size(segment_path);
    const auto bytes_to_write = record_length_size + record.size();
    if (bytes_to_write > maximum_segment_size_ - header_size) {
        throw std::length_error("Object is too large to fit in a database segment.");
    }
    if (bytes_to_write > maximum_segment_size_ - segment_size) {
        std::ostringstream next_name;
        next_name << database_name_ << ".segment-" << std::setw(8) << std::setfill('0')
                  << segment_files_.size() << ".minidb";
        segment_files_.push_back(next_name.str());
        try {
            save_manifest();
        } catch (...) {
            segment_files_.pop_back();
            throw;
        }
        create_segment(segment_files_.back());
        segment_path = directory_ / segment_files_.back();
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

    if (next_id != next_id_) {
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
    if (std::filesystem::exists(segment_path)) {
        if (!std::filesystem::is_regular_file(segment_path) ||
            std::filesystem::file_size(segment_path) != header_size) {
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
