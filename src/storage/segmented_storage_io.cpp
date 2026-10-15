#include "minidb/storage.h"

#include "file_sync.h"
#include "segmented_storage_internal.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

// Segment-level record I/O: reading stored objects, appending records, rotating
// to a new segment, and maintaining the checksummed per-segment object counter.
namespace minidb {

std::shared_ptr<const Object> SegmentedStorage::object_at(const ObjectLocation& location) const {
    if (location.segment_index >= segment_files_.size()) {
        throw std::runtime_error("Object location references a missing segment.");
    }
    const auto segment_path = directory_ / segment_files_[location.segment_index];
    std::ifstream input(segment_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open database segment: " + segment_path.string());
    }
    input.seekg(static_cast<std::streamoff>(location.payload_offset), std::ios::beg);
    if (!input) {
        throw std::runtime_error("Could not seek to a stored object: " + segment_path.string());
    }
    std::string payload(static_cast<std::size_t>(location.payload_size), '\0');
    input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!input) {
        throw std::runtime_error("Could not read a stored object: " + segment_path.string());
    }
    auto object = detail::parse_stored_object(payload, segment_path);
    return std::make_shared<const Object>(std::move(object));
}

void SegmentedStorage::append_record(char operation, std::uint64_t id, const Object* object,
                                     std::uint64_t next_id) {
    std::string record;
    record.push_back(operation);
    detail::append_u64(record, id);
    std::size_t payload_size = 0;
    if (object != nullptr) {
        const auto json = object->dump();
        payload_size = json.size();
        detail::append_u64(record, static_cast<std::uint64_t>(json.size()));
        record.append(json);
    }
    detail::append_record_checksum(record);
    const auto created_objects =
        object != nullptr && id == next_id_ && next_id > next_id_ ? 1U : 0U;
    const auto record_start = append_payload(std::move(record), next_id, true, created_objects);
    const auto segment_index = segment_files_.size() - 1;
    if (object != nullptr) {
        record_store(segment_index,
                     record_start + detail::record_length_size + 1 + 2 * sizeof(std::uint64_t),
                     payload_size, id, *object);
    } else {
        record_erase(id);
    }
}

std::uint64_t SegmentedStorage::append_payload(std::string record, std::uint64_t next_id,
                                               bool update_next_id, std::uint64_t created_objects) {
    if (segment_files_.empty()) {
        throw std::logic_error("Database storage must be loaded before writing records.");
    }
    if (record.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("Database record is too large to write.");
    }

    auto segment_path = directory_ / segment_files_.back();
    if (!detail::path_exists_without_following_symlinks(segment_path)) {
        throw std::runtime_error("Active database segment is missing: " + segment_path.string());
    }
    detail::require_regular_file(segment_path, "Active database segment");
    auto segment_size = std::filesystem::file_size(segment_path);
    if (segment_size < detail::header_size) {
        throw std::runtime_error("Active database segment has an invalid size: " +
                                 segment_path.string());
    }
    if (record.size() > std::numeric_limits<std::uint64_t>::max() - detail::record_length_size) {
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
        if (detail::path_exists_without_following_symlinks(next_segment_path)) {
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
        segment_size = detail::header_size;
    }

    // One stream serves the signature check, the record append, and the object
    // count update. Opening the segment once per store instead of three times
    // removes the dominant cost for small-object workloads.
    std::fstream file(segment_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        throw std::runtime_error("Could not open database segment for appending: " +
                                 segment_path.string());
    }
    {
        std::array<char, detail::segment_signature.size()> signature{};
        file.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        if (!file || signature != detail::segment_signature) {
            throw std::runtime_error("Invalid database segment header: " + segment_path.string());
        }
    }
    file.clear();
    file.seekp(0, std::ios::end);
    detail::write_u64(file, static_cast<std::uint64_t>(record.size()));
    file.write(record.data(), static_cast<std::streamsize>(record.size()));
    // Flush the record before the header slot so a crash cannot leave the object
    // count ahead of the record it describes.
    file.flush();
    if (!file) {
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
            write_segment_object_count(file, segment_path, previous_object_count + created_objects);
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
    file.close();
    // Durability barrier: the record (and its counted header slot) must reach
    // stable storage before the next-ID reservation manifest is allowed to
    // advance, so recovery can never observe a reservation ahead of its data.
    if (durable_) {
        detail::sync_file(segment_path);
    }

    if (update_next_id && next_id != next_id_) {
        const auto previous_next_id = next_id_;
        const auto previous_reservation = next_id_reservation_;
        next_id_ = next_id;
        const bool reservation_advanced = next_id_ >= next_id_reservation_;
        if (reservation_advanced) {
            next_id_reservation_ = next_id_ + next_id_reservation_size_;
        }
        if (!reservation_advanced) {
            return segment_size;
        }
        try {
            save_manifest();
        } catch (...) {
            next_id_ = previous_next_id;
            next_id_reservation_ = previous_reservation;
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
    return segment_size;
}

void SegmentedStorage::create_segment(const std::string& filename) {
    const auto segment_path = directory_ / filename;
    if (detail::path_exists_without_following_symlinks(segment_path)) {
        detail::require_regular_file(segment_path, "Database segment");
        if (std::filesystem::file_size(segment_path) != detail::header_size) {
            throw std::runtime_error("Unexpected data in new database segment: " +
                                     segment_path.string());
        }
        std::ifstream existing(segment_path, std::ios::binary);
        std::array<char, detail::segment_signature.size()> signature{};
        existing.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        if (!existing || signature != detail::segment_signature) {
            throw std::runtime_error("Invalid database segment header: " + segment_path.string());
        }
        return;
    }
    std::ofstream output(segment_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not create database segment: " + segment_path.string());
    }
    output.write(detail::segment_signature.data(),
                 static_cast<std::streamsize>(detail::segment_signature.size()));
    detail::write_u64(output, 0);
    detail::write_u64(output, 0);
    detail::write_u64(output, detail::header_checksum(0, 0));
    detail::write_u64(output, 0);
    detail::write_u64(output, 0);
    detail::write_u64(output, detail::header_checksum(0, 0));
    output.close();
    if (!output) {
        throw std::runtime_error("Could not initialize database segment: " + segment_path.string());
    }
    // The new segment must exist on disk before the manifest lists it; otherwise
    // a crash could leave a manifest that references a missing segment.
    if (durable_) {
        detail::sync_file(segment_path);
    }
}

void SegmentedStorage::update_segment_object_count(const std::filesystem::path& path,
                                                   std::uint64_t object_count) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        throw std::runtime_error("Could not open database segment header: " + path.string());
    }
    std::array<char, detail::segment_signature.size()> signature{};
    file.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    if (!file || signature != detail::segment_signature) {
        throw std::runtime_error("Invalid database segment header: " + path.string());
    }
    write_segment_object_count(file, path, object_count);
}

void SegmentedStorage::write_segment_object_count(std::fstream& file,
                                                  const std::filesystem::path& path,
                                                  std::uint64_t object_count) {
    file.clear();
    file.seekg(static_cast<std::streamoff>(detail::segment_signature.size()), std::ios::beg);
    std::uint64_t first_generation = 0;
    std::uint64_t first_count = 0;
    const bool first_valid = detail::read_header_slot(file, first_generation, first_count);
    std::uint64_t second_generation = 0;
    std::uint64_t second_count = 0;
    const bool second_valid = detail::read_header_slot(file, second_generation, second_count);
    if (!first_valid && !second_valid) {
        throw std::runtime_error("Database segment header counters are corrupt: " + path.string());
    }
    const bool first_is_latest =
        first_valid && (!second_valid || first_generation >= second_generation);
    const auto generation = (first_is_latest ? first_generation : second_generation) + 1;
    const auto slot = generation % 2;
    file.clear();
    file.seekp(static_cast<std::streamoff>(detail::segment_signature.size() +
                                           slot * detail::header_slot_size),
               std::ios::beg);
    detail::write_u64(file, generation);
    detail::write_u64(file, object_count);
    detail::write_u64(file, detail::header_checksum(generation, object_count));
    file.flush();
    if (!file) {
        throw std::runtime_error("Could not update database segment object count: " +
                                 path.string());
    }
}

} // namespace minidb
