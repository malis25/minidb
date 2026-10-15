#include "minidb/storage.h"

#include "segmented_storage_internal.h"

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

namespace minidb {

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
    // Reject Windows reserved device names so a database directory is always
    // addressable on every supported platform.
    if (name == "con" || name == "prn" || name == "aux" || name == "nul") {
        return false;
    }
    return !((name.size() == 4 && name.starts_with("com")) ||
             (name.size() == 4 && name.starts_with("lpt"))) ||
           (name.back() < '1' || name.back() > '9');
}

SegmentedStorage::SegmentedStorage(std::filesystem::path directory, std::string database_name,
                                   std::uint64_t maximum_objects_per_segment, ObjectSchema schema,
                                   std::vector<std::string> indexed_fields,
                                   std::uint64_t cache_bytes, std::uint64_t next_id_reservation)
    : directory_(std::move(directory)), database_name_(std::move(database_name)),
      maximum_objects_per_segment_(maximum_objects_per_segment),
      next_id_reservation_size_(next_id_reservation), schema_(std::move(schema)),
      indexed_fields_(std::move(indexed_fields)), cache_bytes_(cache_bytes) {
    if (directory_.empty()) {
        throw std::invalid_argument("Database directory cannot be empty.");
    }
    if (!is_valid_database_name(database_name_)) {
        throw std::invalid_argument("Invalid database name.");
    }
    if (next_id_reservation_size_ == 0) {
        throw std::invalid_argument("The next-ID reservation must be at least one.");
    }
    detail::require_valid_indexed_fields(indexed_fields_);
    if (maximum_objects_per_segment_ == 0) {
        maximum_objects_per_segment_ = detail::default_objects_per_segment;
    }
    if (!schema_.is_null()) {
        std::string schema_error;
        if (!validate_object_schema(schema_, &schema_error)) {
            throw std::invalid_argument("Invalid object schema: " + schema_error);
        }
    }
    manifest_path_ = directory_ / (database_name_ + ".manifest.json");
}

SegmentedStorage::~SegmentedStorage() {
    // Best effort: a graceful shutdown collapses a partially used reservation so
    // the next open does not skip IDs. Errors are ignored because a destructor
    // must not throw, and recovery can always rebuild next_id from the segments.
    try {
        collapse_next_id_reservation();
    } catch (...) {
    }
}

bool SegmentedStorage::is_disk_backed() const noexcept {
    return disk_backed_;
}

std::uint64_t SegmentedStorage::cache_bytes() const noexcept {
    return cache_bytes_;
}

std::uint64_t SegmentedStorage::next_id_reservation() const noexcept {
    return next_id_reservation_size_;
}

std::uint64_t SegmentedStorage::manifest_writes() const noexcept {
    return manifest_writes_;
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
    const bool manifest_exists = detail::path_exists_without_following_symlinks(manifest_path_);
    if (manifest_exists) {
        detail::require_regular_file(manifest_path_, "Database manifest");
    }
    const bool initialize = !manifest_exists;
    bool manifest_needs_save = false;
    bool has_existing_segments = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (detail::is_segment_filename(entry.path().filename().string(), database_name_)) {
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
        next_id_reservation_ = 0;
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
        if (manifest_version != detail::manifest_format ||
            !manifest.contains("next_id") || !manifest["next_id"].is_number_unsigned() ||
            !manifest.contains("segments") || !manifest["segments"].is_array() ||
            manifest["segments"].empty()) {
            throw std::runtime_error("Database manifest has an unsupported or invalid format.");
        }
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
        if (manifest.contains("schema_migration_pending")) {
            if (!manifest["schema_migration_pending"].is_boolean()) {
                throw std::runtime_error(
                    "Database manifest contains an invalid schema migration flag.");
            }
            schema_migration_pending_ = manifest["schema_migration_pending"].get<bool>();
        }
        next_id_ = manifest["next_id"].get<std::uint64_t>();
        // A manifest written by a run that reserved ID blocks may record a higher
        // bound than the last used ID. Honour the larger value so reserved IDs are
        // never handed out twice after an unclean stop.
        if (manifest.contains("next_id_reservation") &&
            manifest["next_id_reservation"].is_number_unsigned()) {
            next_id_ =
                std::max(next_id_, manifest["next_id_reservation"].get<std::uint64_t>());
        }
        if (manifest.contains("indexed_fields")) {
            if (!manifest["indexed_fields"].is_array()) {
                throw std::runtime_error("Database manifest contains an invalid index list.");
            }
            indexed_fields_ = manifest["indexed_fields"].get<std::vector<std::string>>();
        }
        if (manifest.contains("cache_bytes") && manifest["cache_bytes"].is_number_unsigned()) {
            cache_bytes_ = manifest["cache_bytes"].get<std::uint64_t>();
        }
        detail::require_valid_indexed_fields(indexed_fields_);
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
            if (detail::is_segment_filename(filename, database_name_)) {
                detail::require_regular_file(entry.path(), "Database segment");
                if (std::find(segment_files_.begin(), segment_files_.end(), filename) ==
                    segment_files_.end()) {
                    std::ostringstream expected_name;
                    expected_name << database_name_ << ".segment-" << std::setw(8)
                                  << std::setfill('0') << segment_files_.size() << ".minidb";
                    const auto orphan_path = entry.path();
                    bool is_recoverable_empty_segment =
                        filename == expected_name.str() &&
                        std::filesystem::file_size(orphan_path) == detail::header_size;
                    if (is_recoverable_empty_segment) {
                        std::ifstream orphan(orphan_path, std::ios::binary);
                        std::array<char, detail::segment_signature.size()> signature{};
                        orphan.read(signature.data(),
                                    static_cast<std::streamsize>(signature.size()));
                        is_recoverable_empty_segment =
                            orphan && signature == detail::segment_signature &&
                            std::filesystem::file_size(orphan_path) == detail::header_size;
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

    disk_backed_ = !indexed_fields_.empty() || cache_bytes_ > 0;
    open_index_trees();
    object_locations_.clear();
    cache_order_.clear();
    cache_index_.clear();
    cached_bytes_ = 0;

    auto recovered_next_id = next_id_;
    std::unordered_set<std::uint64_t> stored_ids;
    segment_object_counts_.resize(segment_files_.size(), 0);
    for (std::size_t index = 0; index < segment_files_.size(); ++index) {
        const auto segment_path = directory_ / segment_files_[index];
        if (!detail::path_exists_without_following_symlinks(segment_path)) {
            if (index + 1 != segment_files_.size() || segment_files_.size() != 1 || next_id_ != 0) {
                throw std::runtime_error(
                    "A database segment listed in the manifest is missing; refusing to "
                    "reinitialize potentially lost data.");
            }
            create_segment(segment_files_[index]);
        }
        detail::require_regular_file(segment_path, "Database segment");
        const auto segment_size = std::filesystem::file_size(segment_path);

        std::ifstream input(segment_path, std::ios::binary);
        std::array<char, detail::segment_signature.size()> signature{};
        input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        if (!input || signature != detail::segment_signature) {
            throw std::runtime_error("Invalid database segment: " + segment_path.string());
        }

        std::uint64_t first_generation = 0;
        std::uint64_t first_count = 0;
        const bool first_valid = detail::read_header_slot(input, first_generation, first_count);
        std::uint64_t second_generation = 0;
        std::uint64_t second_count = 0;
        const bool second_valid = detail::read_header_slot(input, second_generation, second_count);
        if (!first_valid && !second_valid) {
            throw std::runtime_error("Database segment header counters are corrupt: " +
                                     segment_path.string());
        }
        const auto selected =
            !second_valid || (first_valid && first_generation >= second_generation)
                ? std::pair{first_generation, first_count}
                : std::pair{second_generation, second_count};
        segment_object_counts_[index] = selected.second;
        std::uint64_t valid_size = detail::header_size;
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
            if (remaining < detail::record_length_size) {
                if (index + 1 != segment_files_.size()) {
                    throw std::runtime_error(
                        "An incomplete record was found in a non-final database segment.");
                }
                input.close();
                std::filesystem::resize_file(segment_path, record_start);
                break;
            }
            const auto record_size = detail::read_u64(input);
            valid_size += detail::record_length_size;
            if (record_size > segment_size - valid_size) {
                if (index + 1 != segment_files_.size()) {
                    throw std::runtime_error(
                        "An incomplete record was found in a non-final database segment.");
                }
                input.close();
                std::filesystem::resize_file(segment_path, record_start);
                break;
            }
            if (record_size < detail::record_prefix_size ||
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
            const auto id = detail::read_u64(record, offset);
            if (id == std::numeric_limits<std::uint64_t>::max()) {
                recovered_next_id = id;
            } else {
                recovered_next_id = std::max(recovered_next_id, id + 1);
            }
            if (operation == detail::store_operation) {
                const auto object_size = detail::read_u64(record, offset);
                if (object_size != record.size() - offset) {
                    throw std::runtime_error("Database object record has an invalid payload size.");
                }
                const auto payload_offset = record_start + detail::record_length_size + offset;
                auto object = detail::parse_stored_object(std::string_view{record}.substr(offset),
                                                          segment_path);
                if (stored_ids.insert(id).second) {
                    ++recovered_object_count;
                }
                if (disk_backed_) {
                    // record_store evicts the previous version's index entries, so a
                    // replaced object never leaves stale keys behind in the tree.
                    record_store(index, payload_offset, object_size, id, object);
                } else {
                    object_locations_[id] = ObjectLocation{index, payload_offset, object_size};
                    objects.insert_or_assign(id, std::move(object));
                }
            } else if (operation == detail::erase_operation) {
                if (offset != record.size()) {
                    throw std::runtime_error("Database deletion record contains trailing data.");
                }
                if (disk_backed_) {
                    record_erase(id);
                } else {
                    object_locations_.erase(id);
                }
                objects.erase(id);
            } else if (operation == detail::batch_operation) {
                const auto mutation_count = detail::read_u64(record, offset);
                if (mutation_count == 0 ||
                    mutation_count >
                        static_cast<std::uint64_t>(record.size() - offset) /
                            detail::record_prefix_size) {
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
                    const auto mutation_id = detail::read_u64(record, offset);
                    if (mutation_operation == detail::store_operation) {
                        const auto object_size = detail::read_u64(record, offset);
                        if (object_size > record.size() - offset) {
                            throw std::runtime_error("Database batch object is truncated.");
                        }
                        const auto payload_offset = record_start + detail::record_length_size + offset;
                        auto object = detail::parse_stored_object(
                            std::string_view{record}.substr(
                                offset, static_cast<std::size_t>(object_size)),
                            segment_path);
                        offset += static_cast<std::size_t>(object_size);
                        if (stored_ids.insert(mutation_id).second) {
                            ++recovered_object_count;
                        }
                        if (disk_backed_) {
                            record_store(index, payload_offset, object_size, mutation_id, object);
                            mutations.push_back({StorageMutationType::store, mutation_id, {}});
                        } else {
                            object_locations_[mutation_id] =
                                ObjectLocation{index, payload_offset, object_size};
                            mutations.push_back(
                                {StorageMutationType::store, mutation_id, std::move(object)});
                        }
                        if (mutation_id == std::numeric_limits<std::uint64_t>::max()) {
                            recovered_next_id = mutation_id;
                        } else {
                            recovered_next_id = std::max(recovered_next_id, mutation_id + 1);
                        }
                    } else if (mutation_operation == detail::erase_operation) {
                        if (disk_backed_) {
                            record_erase(mutation_id);
                        } else {
                            object_locations_.erase(mutation_id);
                        }
                        mutations.push_back({StorageMutationType::erase, mutation_id, {}});
                    } else {
                        throw std::runtime_error(
                            "Database batch record contains an unknown operation.");
                    }
                }
                if (offset != record.size()) {
                    throw std::runtime_error("Database batch record contains trailing data.");
                }
                if (!disk_backed_) {
                    for (auto& mutation : mutations) {
                        if (mutation.type == StorageMutationType::store) {
                            objects.insert_or_assign(mutation.id, std::move(mutation.object));
                        } else {
                            objects.erase(mutation.id);
                        }
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
        if (recorded_object_count != recovered_object_count) {
            update_segment_object_count(segment_path, recovered_object_count);
        }
        if (segment_object_counts_[index] > maximum_objects_per_segment_) {
            throw std::runtime_error("Database segment exceeds its configured object-count limit.");
        }
    }

    if (recovered_next_id != next_id_ || manifest_needs_save) {
        next_id_ = recovered_next_id;
        next_id_reservation_ = next_id_;
        save_manifest();
    } else {
        next_id_reservation_ = next_id_;
    }
    loaded_ = true;

    StorageSnapshot snapshot;
    snapshot.next_id = next_id_;
    snapshot.schema = schema_;
    snapshot.maximum_objects_per_segment = maximum_objects_per_segment_;
    snapshot.indexed_fields = indexed_fields_;
    snapshot.disk_backed = disk_backed_;
    snapshot.schema_migration_pending = schema_migration_pending_;
    if (disk_backed_) {
        snapshot.object_ids.reserve(object_locations_.size());
        for (const auto& [id, location] : object_locations_) {
            static_cast<void>(location);
            snapshot.object_ids.push_back(id);
        }
    } else {
        snapshot.objects.reserve(objects.size());
        for (auto& [id, object] : objects) {
            snapshot.objects.push_back({id, std::move(object)});
        }
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
    append_record(detail::store_operation, id, &object, next_id);
}

void SegmentedStorage::erase(std::uint64_t id) {
    append_record(detail::erase_operation, id, nullptr, next_id_);
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
            append_record(detail::store_operation, mutation.id, &mutation.object, next_id);
        } else {
            append_record(detail::erase_operation, mutation.id, nullptr, next_id_);
        }
        return;
    }

    std::string record;
    record.push_back(detail::batch_operation);
    detail::append_u64(record, 0);
    detail::append_u64(record, static_cast<std::uint64_t>(mutations.size()));
    std::vector<std::size_t> payload_offsets(mutations.size(), 0);
    std::vector<std::size_t> payload_sizes(mutations.size(), 0);
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto& mutation = mutations[index];
        if (mutation.type == StorageMutationType::store) {
            if (!mutation.object.is_object() || !is_within_object_nesting_depth(mutation.object) ||
                mutation.id >= next_id) {
                throw std::invalid_argument("Batch contains an invalid stored object or ID.");
            }
            const auto json = mutation.object.dump();
            record.push_back(detail::store_operation);
            detail::append_u64(record, mutation.id);
            detail::append_u64(record, static_cast<std::uint64_t>(json.size()));
            payload_offsets[index] = record.size();
            payload_sizes[index] = json.size();
            record.append(json);
        } else {
            record.push_back(detail::erase_operation);
            detail::append_u64(record, mutation.id);
        }
    }
    const auto created_objects = static_cast<std::uint64_t>(
        std::count_if(mutations.begin(), mutations.end(), [&](const auto& mutation) {
            return mutation.type == StorageMutationType::store && mutation.id >= next_id_;
        }));
    const auto record_start = append_payload(std::move(record), next_id, true, created_objects);
    const auto segment_index = segment_files_.size() - 1;
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto& mutation = mutations[index];
        if (mutation.type == StorageMutationType::store) {
            record_store(segment_index,
                         record_start + detail::record_length_size +
                             static_cast<std::uint64_t>(payload_offsets[index]),
                         static_cast<std::uint64_t>(payload_sizes[index]), mutation.id,
                         mutation.object);
        } else {
            record_erase(mutation.id);
        }
    }
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

void SegmentedStorage::set_schema_migration_pending(bool pending) {
    if (schema_migration_pending_ == pending) {
        return;
    }
    const auto previous = schema_migration_pending_;
    schema_migration_pending_ = pending;
    try {
        save_manifest();
    } catch (...) {
        schema_migration_pending_ = previous;
        throw;
    }
}

void SegmentedStorage::close() {
    // Collapse any partially used ID reservation so a clean restart stays
    // contiguous. Best effort: releasing the files below matters more than the
    // manifest, which recovery can rebuild by scanning the segments.
    try {
        collapse_next_id_reservation();
    } catch (...) {
    }
    closed_ = true;
    // Dropping the trees closes their files. Leaving them open would prevent the
    // database directory from being renamed or removed on Windows.
    index_trees_.clear();
    cache_order_.clear();
    cache_index_.clear();
    cached_bytes_ = 0;
}

} // namespace minidb
