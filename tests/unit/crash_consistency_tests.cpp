// Crash-consistency and corruption-recovery tests.
//
// These tests materialise the on-disk state that a crash at each critical write
// stage would leave behind (a torn trailing record, a torn record in a sealed
// segment, zeroed header counters, a stale manifest reservation, a corrupt
// index file, a truncated manifest) and then reopen the database. Where a real
// process termination matters, `minidb_crash_fixture` is executed as a child
// process that writes records and then calls _Exit, so no destructor or stream
// flush runs before the state is inspected.

#include "minidb/database.h"
#include "minidb/storage.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using test_support::default_object_schema;
using test_support::make_disk_storage;
using test_support::small_segment_size;
using test_support::TemporaryDirectory;

#ifndef MINIDB_CRASH_FIXTURE
#define MINIDB_CRASH_FIXTURE ""
#endif

namespace {

std::filesystem::path user_segment(const std::filesystem::path& directory) {
    return directory / "users.segment-00000000.minidb";
}

std::filesystem::path user_manifest(const std::filesystem::path& directory) {
    return directory / "users.manifest.json";
}

void append_u64_le(std::string& out, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

// Mirrors detail::record_checksum so the tests can build well-formed records.
std::uint64_t record_checksum(std::string_view body) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : body) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void write_u64_le(std::ostream& out, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        out.put(static_cast<char>((value >> shift) & 0xff));
    }
}

std::string make_store_record(std::uint64_t id, const std::string& payload) {
    std::string record;
    record.push_back('S');
    append_u64_le(record, id);
    append_u64_le(record, static_cast<std::uint64_t>(payload.size()));
    record += payload;
    append_u64_le(record, record_checksum(record));
    return record;
}

// Appends one length-prefixed record to a segment, bypassing SegmentedStorage so
// a test can leave a partial or malformed tail exactly as a crash would.
void append_raw_record(const std::filesystem::path& segment, const std::string& record) {
    std::ofstream out(segment, std::ios::binary | std::ios::app);
    ASSERT_TRUE(out);
    write_u64_le(out, static_cast<std::uint64_t>(record.size()));
    out.write(record.data(), static_cast<std::streamsize>(record.size()));
}

// Appends only a length prefix and a truncated body, modelling a torn write.
void append_torn_record(const std::filesystem::path& segment, std::uint64_t claimed_size,
                        const std::string& partial_body) {
    std::ofstream out(segment, std::ios::binary | std::ios::app);
    ASSERT_TRUE(out);
    write_u64_le(out, claimed_size);
    out.write(partial_body.data(), static_cast<std::streamsize>(partial_body.size()));
}

std::filesystem::path find_index_file(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.find(".index-") != std::string::npos && name.ends_with(".tree")) {
            return entry.path();
        }
    }
    return {};
}

nlohmann::json read_manifest(const std::filesystem::path& path) {
    std::ifstream input(path);
    nlohmann::json manifest;
    input >> manifest;
    return manifest;
}

void write_manifest(const std::filesystem::path& path, const nlohmann::json& manifest) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << manifest.dump(2) << '\n';
}

std::size_t object_count(const minidb::StorageSnapshot& snapshot) {
    return snapshot.objects.size();
}

} // namespace

// A crash after the last complete record leaves a partial length prefix. Recovery
// must keep every committed record and discard only the torn tail.
TEST(CrashConsistencyTests, TruncatesATornTrailingRecordAndKeepsCommittedObjects) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", 10000);
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "a"}}, 1);
        storage.store(1, {{"name", "b"}}, 2);
    }
    append_torn_record(user_segment(directory.path()), 100, "short");

    minidb::SegmentedStorage storage(directory.path(), "users", 10000);
    const auto snapshot = storage.load();
    EXPECT_EQ(object_count(snapshot), 2U);
    EXPECT_EQ(snapshot.next_id, 2U);
    // The torn tail is gone, so the segment is a clean collection of records.
    storage.store(2, {{"name", "c"}}, 3);
}

// A torn record in a sealed (non-final) segment means a record was lost from the
// middle of the log; recovery must refuse rather than silently drop data.
TEST(CrashConsistencyTests, RejectsATornRecordInANonFinalSegment) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "a"}}, 1);
        storage.store(1, {{"name", "b"}}, 2);
    }
    append_torn_record(directory.path() / "users.segment-00000000.minidb", 100, "short");

    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
    EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}

// A truncated final record whose payload is invalid JSON must fail loudly instead
// of being dropped or accepted.
TEST(CrashConsistencyTests, RejectsACommittedRecordWithCorruptPayload) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", 10000);
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "a"}}, 1);
    }
    append_raw_record(user_segment(directory.path()), make_store_record(1, "{"));

    minidb::SegmentedStorage storage(directory.path(), "users", 10000);
    EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}

// An in-place bit flip inside a committed record keeps the length prefix intact,
// so only the per-record checksum can detect it; recovery must refuse.
TEST(CrashConsistencyTests, RejectsABitFlipInsideACommittedRecord) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", 10000);
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "Ada"}}, 1);
    }
    const auto segment = user_segment(directory.path());
    std::vector<char> bytes;
    {
        std::ifstream input(segment, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    ASSERT_GT(bytes.size(), 82U);
    // Offset 82 is inside the stored JSON payload (header 56 + length 8 + op 1 +
    // id 8 + payload size 8 = 81), so the record length stays valid.
    bytes[82] = static_cast<char>(bytes[82] ^ 0x20);
    {
        std::ofstream output(segment, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    minidb::SegmentedStorage storage(directory.path(), "users", 10000);
    EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}

// Zeroing both header count slots models a torn header update that destroyed both
// copies; the object count cannot be trusted and the database must refuse.
TEST(CrashConsistencyTests, RejectsSegmentWithCorruptHeaderCounters) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", 10000);
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "a"}}, 1);
    }
    const auto segment = user_segment(directory.path());
    std::vector<char> bytes;
    {
        std::ifstream input(segment, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    ASSERT_GE(bytes.size(), 56U);
    std::fill(bytes.begin() + 8, bytes.begin() + 56, 0);
    {
        std::ofstream output(segment, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    minidb::SegmentedStorage storage(directory.path(), "users", 10000);
    EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}

// A manifest whose recorded next_id is behind the segment log must not cause an
// ID to be reused: recovery scans the log and continues past the highest ID.
TEST(CrashConsistencyTests, RecoversAStaleManifestWithoutReusingIds) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", 10000);
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "a"}}, 1);
        storage.store(1, {{"name", "b"}}, 2);
        storage.store(2, {{"name", "c"}}, 3);
    }
    const auto manifest_path = user_manifest(directory.path());
    auto manifest = read_manifest(manifest_path);
    manifest["next_id"] = 0;
    manifest.erase("next_id_reservation");
    write_manifest(manifest_path, manifest);

    minidb::SegmentedStorage storage(directory.path(), "users", 10000);
    const auto snapshot = storage.load();
    EXPECT_EQ(object_count(snapshot), 3U);
    EXPECT_EQ(snapshot.next_id, 3U);
    storage.store(snapshot.next_id, {{"name", "d"}}, snapshot.next_id + 1);
}

// A crash between writing the manifest temp file and the atomic replace leaves a
// stale temp file behind; the next open must not be confused by it.
TEST(CrashConsistencyTests, ToleratesALeftoverTemporaryManifest) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", 10000);
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "a"}}, 1);
    }
    {
        std::ofstream temp(user_manifest(directory.path()).string() + ".tmp",
                           std::ios::binary | std::ios::trunc);
        temp << "{ this is not a complete manifest";
    }

    minidb::SegmentedStorage storage(directory.path(), "users", 10000);
    const auto snapshot = storage.load();
    EXPECT_EQ(object_count(snapshot), 1U);
}

// A manifest truncated mid-write is not a valid manifest; opening must fail
// rather than start from an empty database and lose the segments.
TEST(CrashConsistencyTests, RejectsATruncatedManifest) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", 10000);
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "a"}}, 1);
    }
    const auto manifest_path = user_manifest(directory.path());
    const auto size = std::filesystem::file_size(manifest_path);
    std::filesystem::resize_file(manifest_path, size / 2);

    minidb::SegmentedStorage storage(directory.path(), "users", 10000);
    EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}

// Create/update/delete durability: the surviving object set after a reopen must
// reflect every committed mutation, including deletes and replacements.
TEST(CrashConsistencyTests, PersistsUpdatesAndDeletesAcrossReopen) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::Database database{
            std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", 10000)};
        for (int index = 0; index < 5; ++index) {
            ASSERT_EQ(database.create({{"n", index}}), static_cast<std::uint64_t>(index));
        }
        database.set(1, {{"n", 100}});
        ASSERT_TRUE(database.remove(3));
    }

    minidb::Database database{
        std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", 10000)};
    EXPECT_EQ(database.size(), 4U);
    EXPECT_FALSE(database.contains(3));
    ASSERT_TRUE(database.get(1) != nullptr);
    EXPECT_EQ((*database.get(1))["n"], 100);
    EXPECT_EQ(database.next_id(), 5U);
}

// A corrupt index file must not prevent the database from opening: the index is
// derived data and is rebuilt from the segment log, leaving objects untouched.
TEST(CrashConsistencyTests, RebuildsACorruptIndexWithoutLosingObjects) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::Database database{make_disk_storage(directory.path(), {"name"}, 0)};
        ASSERT_EQ(database.create({{"name", "ada"}}), 0U);
        ASSERT_EQ(database.create({{"name", "grace"}}), 1U);
        ASSERT_EQ(database.create({{"name", "ada"}}), 2U);
    }
    const auto index = find_index_file(directory.path());
    ASSERT_FALSE(index.empty());
    {
        std::ofstream output(index, std::ios::binary | std::ios::trunc);
        output.write("BROKEN!!", 8);
    }

    minidb::Database database{make_disk_storage(directory.path(), {"name"}, 0)};
    EXPECT_EQ(database.size(), 3U);
    const auto matches = database.find("name", minidb::Object("ada"));
    EXPECT_EQ(matches.size(), 2U);
    // The rebuilt index must accept new writes.
    EXPECT_EQ(database.create({{"name", "ada"}}), 3U);
    EXPECT_EQ(database.find("name", minidb::Object("ada")).size(), 3U);
}

// A truncated index file (shorter than one page) is treated as an empty index and
// rebuilt from the log.
TEST(CrashConsistencyTests, RebuildsATruncatedIndexFile) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::Database database{make_disk_storage(directory.path(), {"name"}, 0)};
        ASSERT_EQ(database.create({{"name", "ada"}}), 0U);
    }
    const auto index = find_index_file(directory.path());
    ASSERT_FALSE(index.empty());
    std::filesystem::resize_file(index, 3);

    minidb::Database database{make_disk_storage(directory.path(), {"name"}, 0)};
    EXPECT_EQ(database.size(), 1U);
    EXPECT_EQ(database.find("name", minidb::Object("ada")).size(), 1U);
}

// Storage opened without the durability barrier must still use the same
// crash-consistent on-disk format and recover cleanly on reopen; it only skips
// the fsync, so a process crash is safe even though a power loss may lose the
// most recent acknowledged writes.
TEST(CrashConsistencyTests, NonDurableModeKeepsTheOnDiskFormatConsistent) {
    TemporaryDirectory directory("minidb-crash-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", 10000,
                                         default_object_schema(), {}, 0, 128, /*durable=*/false);
        EXPECT_FALSE(storage.durable());
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "a"}}, 1);
        storage.store(1, {{"name", "b"}}, 2);
    }

    minidb::SegmentedStorage storage(directory.path(), "users", 10000);
    EXPECT_TRUE(storage.durable());
    const auto snapshot = storage.load();
    EXPECT_EQ(object_count(snapshot), 2U);
    EXPECT_EQ(snapshot.next_id, 2U);
}

// An abrupt process termination (no destructors, no graceful close) must not lose
// records that store() already returned successfully for, and must not reuse IDs.
TEST(CrashConsistencyTests, AckedCreatesSurviveAnAbruptTermination) {
    ASSERT_FALSE(std::string(MINIDB_CRASH_FIXTURE).empty())
        << "MINIDB_CRASH_FIXTURE was not configured by the build";
    TemporaryDirectory directory("minidb-crash-");
    std::string fixture = MINIDB_CRASH_FIXTURE;
#ifdef _WIN32
    // cmd.exe rejects forward slashes in a quoted executable path.
    std::replace(fixture.begin(), fixture.end(), '/', '\\');
#endif
    std::string command = "\"" + fixture + "\" \"" + directory.path().string() + "\" 5 1";
#ifdef _WIN32
    // cmd.exe /c strips the outer quotes around a quoted command, so add one more
    // layer: cmd unwraps the extra pair and runs the quoted executable untouched.
    command = "\"" + command + "\"";
#endif
    const int status = std::system(command.c_str());
    ASSERT_EQ(status, 0) << "crash fixture failed; command: " << command;

    minidb::SegmentedStorage storage(directory.path(), "users", 100000, default_object_schema(),
                                     {}, 0, 1);
    const auto snapshot = storage.load();
    EXPECT_EQ(object_count(snapshot), 5U);
    EXPECT_GE(snapshot.next_id, 5U);
    // The next ID handed out must be fresh, never a reused one.
    const auto fresh = snapshot.next_id;
    storage.store(fresh, {{"name", "after"}}, fresh + 1);
}
