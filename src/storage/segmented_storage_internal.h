#pragma once

#include "minidb/object.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <ostream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

// Helpers shared by the SegmentedStorage translation units. Keeping the on-disk
// format in one place lets the segment, manifest, and index readers/writers live
// in separate files without duplicating layout constants or byte-level encoding.
namespace minidb::detail {

inline constexpr std::array<char, 8> segment_signature{'M', 'I', 'N', 'I', 'D', 'B', '3', '\0'};
inline constexpr std::string_view manifest_format = "MINIDB-MANIFEST-4";
inline constexpr char store_operation = 'S';
inline constexpr char erase_operation = 'E';
inline constexpr char batch_operation = 'T';
inline constexpr std::uint64_t header_slot_size = 3 * sizeof(std::uint64_t);
inline constexpr std::uint64_t header_size = segment_signature.size() + 2 * header_slot_size;
inline constexpr std::uint64_t header_checksum_salt = 0x4d494e4944423300ULL;
inline constexpr std::uint64_t default_objects_per_segment = 10000;
inline constexpr std::uint64_t record_length_size = sizeof(std::uint64_t);
inline constexpr std::uint64_t record_prefix_size = 1 + sizeof(std::uint64_t);

// Each segment header holds two checksummed slots written in alternation. The
// checksum lets recovery pick the newer intact slot after a torn write.
inline std::uint64_t header_checksum(std::uint64_t generation, std::uint64_t object_count) {
    return header_checksum_salt ^ generation ^ object_count;
}

// FNV-1a over a field's encoded value. Index keys are hash-then-ID so a lookup
// scans a contiguous run of keys instead of comparing full JSON payloads.
inline std::uint64_t field_value_hash(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Big-endian so the byte-wise ordering of a B+ tree key matches numeric order.
inline void append_big_endian(std::string& output, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<char>((value >> (56 - shift)) & 0xff));
    }
}

inline std::string make_index_key(std::string_view value, std::uint64_t id) {
    std::string key;
    key.reserve(16);
    append_big_endian(key, field_value_hash(value));
    append_big_endian(key, id);
    return key;
}

inline std::string make_index_prefix(std::string_view value) {
    std::string prefix;
    prefix.reserve(8);
    append_big_endian(prefix, field_value_hash(value));
    return prefix;
}

inline std::uint64_t read_index_id(std::string_view key) {
    std::uint64_t id = 0;
    for (std::size_t index = 8; index < 16; ++index) {
        id = (id << 8) | static_cast<unsigned char>(key[index]);
    }
    return id;
}

inline std::string index_file_component(std::string_view field) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << field_value_hash(field);
    return output.str();
}

inline void require_valid_indexed_fields(const std::vector<std::string>& fields) {
    std::unordered_set<std::string> seen;
    for (const auto& field : fields) {
        if (field.empty()) {
            throw std::invalid_argument("Indexed field names cannot be empty.");
        }
        if (!seen.insert(field).second) {
            throw std::invalid_argument("Indexed field names must be unique.");
        }
    }
}

// Path inspection never follows symlinks, so a symlinked manifest or segment
// cannot redirect reads or writes outside the database directory.
inline bool path_exists_without_following_symlinks(const std::filesystem::path& path) {
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

inline void require_regular_file(const std::filesystem::path& path, std::string_view description) {
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

inline bool is_segment_filename(std::string_view filename, std::string_view database_name) {
    const auto prefix = std::string(database_name) + ".segment-";
    constexpr std::string_view suffix = ".minidb";
    return filename.starts_with(prefix) && filename.ends_with(suffix);
}

// Records store integers little-endian, and records are length-prefixed so a
// torn write at the tail of the final segment can be detected and truncated.
inline void append_u64(std::string& output, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

inline std::uint64_t read_u64(std::string_view bytes, std::size_t& offset) {
    if (bytes.size() - offset < sizeof(std::uint64_t)) {
        throw std::runtime_error("Database record is truncated.");
    }
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset++])) << shift;
    }
    return value;
}

inline void write_u64(std::ostream& output, std::uint64_t value) {
    // Write the eight little-endian bytes in one call instead of eight put()
    // calls; stream insertion per byte dominates header and record writes.
    char bytes[sizeof(std::uint64_t)];
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        bytes[shift / 8] = static_cast<char>((value >> shift) & 0xff);
    }
    output.write(bytes, sizeof(bytes));
}

inline std::uint64_t read_u64(std::istream& input) {
    char bytes[sizeof(std::uint64_t)];
    if (!input.read(bytes, sizeof(bytes))) {
        throw std::runtime_error("Database record length is truncated.");
    }
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[shift / 8])) << shift;
    }
    return value;
}

// Reads one header slot and verifies its checksum. A false return means the slot
// is unreadable, which is expected for the never-written second slot.
inline bool read_header_slot(std::istream& input, std::uint64_t& generation,
                             std::uint64_t& object_count) {
    try {
        generation = read_u64(input);
        object_count = read_u64(input);
        const auto checksum = read_u64(input);
        return checksum == header_checksum(generation, object_count);
    } catch (const std::runtime_error&) {
        return false;
    }
}

// Parses a stored JSON payload, translating parser failures into a storage error
// that names the segment. A corrupt record must surface as an explicit failure,
// never as a silently skipped object.
inline Object parse_stored_object(std::string_view payload,
                                  const std::filesystem::path& segment_path) {
    try {
        auto object =
            Object::parse(payload, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                if (depth > maximum_object_nesting_depth) {
                    throw std::length_error("Stored JSON nesting exceeds the maximum depth of 128.");
                }
                return true;
            });
        if (!object.is_object()) {
            throw std::runtime_error("Stored record does not contain a JSON object.");
        }
        return object;
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("Database segment contains invalid JSON (" +
                                 std::string(error.what()) + "): " + segment_path.string());
    }
}

} // namespace minidb::detail
