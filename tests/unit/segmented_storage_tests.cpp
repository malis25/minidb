#include "minidb/database.h"
#include "minidb/storage.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using test_support::default_object_schema;
using test_support::small_segment_size;
using test_support::TemporaryDirectory;

TEST(SegmentedStorageTests, CreatesCountBoundedSegmentsAndRecordsHeaderCounts) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::Database database{std::make_unique<minidb::SegmentedStorage>(
            directory.path(), "users", small_segment_size)};
        EXPECT_EQ(database.create({{"username", "ada"}}), 0);
        EXPECT_EQ(database.create({{"username", "grace"}}), 1);
        EXPECT_EQ(database.create({{"username", "alan"}}), 2);
    }

    const auto manifest_file = directory.path() / "users.manifest.json";
    ASSERT_TRUE(std::filesystem::is_regular_file(manifest_file));
    std::ifstream manifest_input(manifest_file);
    const auto manifest = nlohmann::json::parse(manifest_input);
    ASSERT_EQ(manifest["segments"].size(), 3);
    EXPECT_EQ(manifest["next_id"], 3);
    for (int index = 0; index < 3; ++index) {
        std::ostringstream filename;
        filename << "users.segment-" << std::setw(8) << std::setfill('0') << index << ".minidb";
        const auto segment = directory.path() / filename.str();
        ASSERT_TRUE(std::filesystem::is_regular_file(segment));
        std::ifstream header(segment, std::ios::binary);
        std::array<char, 8> signature{};
        header.read(signature.data(), static_cast<std::streamsize>(signature.size()));
        EXPECT_EQ(std::string(signature.data(), signature.size()), std::string("MINIDB3\0", 8));
        header.seekg(40, std::ios::beg);
        std::uint64_t stored_count = 0;
        for (unsigned int shift = 0; shift < 64; shift += 8) {
            const auto byte = header.get();
            stored_count |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte)) << shift;
        }
        EXPECT_EQ(stored_count, 1);
    }
}

TEST(SegmentedStorageTests, UpdatesAndDeletesDoNotConsumeObjectCreationQuota) {
    TemporaryDirectory directory("minidb-segment-tests-");
    minidb::Database database{
        std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", 1)};

    const auto first_id = database.create({{"name", "Ada"}});
    database.set(first_id, {{"name", "Grace"}});
    EXPECT_TRUE(database.remove(first_id));
    const auto second_id = database.create({{"name", "Alan"}});

    EXPECT_EQ(first_id, 0);
    EXPECT_EQ(second_id, 1);
    std::ifstream manifest_input(directory.path() / "users.manifest.json");
    const auto manifest = nlohmann::json::parse(manifest_input);
    EXPECT_EQ(manifest["segments"].size(), 2);
}

TEST(SegmentedStorageTests, PersistsIdsIndexesUpdatesAndDeletesAcrossRestart) {
    TemporaryDirectory directory("minidb-segment-tests-");
    const auto make_storage = [&] {
        return std::make_unique<minidb::SegmentedStorage>(directory.path(), "users",
                                                          small_segment_size);
    };
    {
        minidb::Database database{make_storage()};
        const auto ada_id = database.create({{"email", "same@example.test"}, {"name", "Ada"}});
        const auto grace_id = database.create({{"email", "same@example.test"}, {"name", "Grace"}});
        database.set(grace_id, {{"email", "grace@example.test"}, {"name", "Grace"}});
        EXPECT_TRUE(database.remove(ada_id));
        EXPECT_EQ(grace_id, 1);
    }

    minidb::Database restored{make_storage()};
    EXPECT_EQ(restored.next_id(), 2);
    ASSERT_EQ(restored.size(), 1);
    EXPECT_TRUE(restored.find("email", "same@example.test").empty());
    EXPECT_EQ(restored.find("email", "grace@example.test").front().id, 1);
    EXPECT_EQ(restored.create({{"name", "new"}}), 2);
}

TEST(SegmentedStorageTests, ReplaysBatchAsOneAtomicStorageRecord) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::Database database{
            std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", 1024)};
        const auto ada_id = database.create({{"name", "Ada"}, {"room", "general"}});
        const auto grace_id = database.create({{"name", "Grace"}, {"room", "general"}});
        const auto segment = directory.path() / "users.segment-00000000.minidb";
        const auto before_batch = std::filesystem::file_size(segment);

        static_cast<void>(database.apply_batch(
            {{minidb::ObjectMutation::Type::create, 0, {{"name", "Alan"}, {"room", "random"}}},
             {minidb::ObjectMutation::Type::replace, ada_id, {{"name", "Ada"}, {"room", "random"}}},
             {minidb::ObjectMutation::Type::erase, grace_id, {}}}));

        EXPECT_GT(std::filesystem::file_size(segment), before_batch);
    }

    const auto manifest_path = directory.path() / "users.manifest.json";
    minidb::Database restored{
        std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", 1024)};
    EXPECT_EQ(restored.next_id(), 3);
    EXPECT_FALSE(restored.contains(1));
    EXPECT_EQ(restored.get(0)->at("room"), "random");
    EXPECT_EQ(restored.get(2)->at("room"), "random");
    std::ifstream manifest_input(manifest_path);
    const auto manifest = nlohmann::json::parse(manifest_input);
    EXPECT_EQ(manifest["format"], "MINIDB-MANIFEST-4");
}

TEST(SegmentedStorageTests, RecoversNextIdFromACommittedRecord) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::Database database{std::make_unique<minidb::SegmentedStorage>(
            directory.path(), "users", small_segment_size)};
        EXPECT_EQ(database.create({{"name", "Ada"}}), 0);
    }

    const auto manifest_path = directory.path() / "users.manifest.json";
    std::ifstream manifest_input(manifest_path);
    auto manifest = nlohmann::json::parse(manifest_input);
    manifest_input.close();
    manifest["next_id"] = 0;
    {
        std::ofstream manifest_output(manifest_path, std::ios::binary | std::ios::trunc);
        manifest_output << manifest.dump(2) << '\n';
    }

    minidb::Database restored{
        std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", small_segment_size)};
    EXPECT_EQ(restored.next_id(), 1);
    EXPECT_EQ(restored.create({{"name", "Grace"}}), 1);
}

TEST(SegmentedStorageTests, TruncatesAnIncompleteFinalRecord) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::Database database{std::make_unique<minidb::SegmentedStorage>(
            directory.path(), "users", small_segment_size)};
        EXPECT_EQ(database.create({{"name", "Ada"}}), 0);
    }

    const auto segment_path = directory.path() / "users.segment-00000000.minidb";
    const auto complete_size = std::filesystem::file_size(segment_path);
    {
        std::ofstream segment_output(segment_path, std::ios::binary | std::ios::app);
        segment_output.write("\x01\x02", 2);
    }

    minidb::Database restored{
        std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", small_segment_size)};
    EXPECT_EQ(restored.get(0)->at("name"), "Ada");
    EXPECT_EQ(std::filesystem::file_size(segment_path), complete_size);
}

// The manifest is only rewritten when a reserved block of IDs is exhausted, so
// creating N objects must cause far fewer than N writes.
TEST(SegmentedStorageTests, BatchesManifestWritesAcrossReservedIdBlocks) {
    TemporaryDirectory directory("minidb-segment-tests-");
    constexpr std::uint64_t reservation = 64;
    constexpr std::uint64_t object_count = 1000;
    minidb::SegmentedStorage storage{directory.path(), "users", 100000,
                                     default_object_schema(), std::vector<std::string>{}, 0, reservation};
    static_cast<void>(storage.load());
    const auto baseline = storage.manifest_writes();

    for (std::uint64_t index = 0; index < object_count; ++index) {
        storage.store(index, {{"name", "object"}}, index + 1);
    }

    const auto writes = storage.manifest_writes() - baseline;
    // One write starts the first block, then one more per full block.
    EXPECT_LE(writes, object_count / reservation + 2);
    EXPECT_GE(writes, 1U);
    EXPECT_LT(writes, object_count / 4);
}

// After an unclean stop the manifest may only record the reservation bound. The
// backend must resume above that bound instead of reusing reserved IDs.
TEST(SegmentedStorageTests, HonoursReservedIdsAfterAnUncleanStop) {
    TemporaryDirectory directory("minidb-segment-tests-");
    nlohmann::json crashed_manifest;
    constexpr std::uint64_t reservation = 8;
    {
        minidb::SegmentedStorage storage{directory.path(), "users", 100000,
                                         default_object_schema(), std::vector<std::string>{}, 0, reservation};
        static_cast<void>(storage.load());
        for (std::uint64_t index = 0; index < 3; ++index) {
            storage.store(index, {{"name", "object"}}, index + 1);
        }
        std::ifstream manifest_input(directory.path() / "users.manifest.json");
        crashed_manifest = nlohmann::json::parse(manifest_input);
    }
    // The destructor collapsed the reservation; restore the crash-time view.
    // The exact next_id lags behind because it is only checkpointed when a block
    // is reserved, so recovery has to rely on the reservation bound.
    EXPECT_LT(crashed_manifest["next_id"].get<std::uint64_t>(), 3U);
    const auto reservation_bound = crashed_manifest["next_id_reservation"].get<std::uint64_t>();
    EXPECT_GE(reservation_bound, 3U);
    {
        std::ofstream manifest_output(directory.path() / "users.manifest.json",
                                      std::ios::binary | std::ios::trunc);
        manifest_output << crashed_manifest.dump(2) << '\n';
    }

    minidb::SegmentedStorage reopened{directory.path(), "users", 100000,
                                      default_object_schema(), std::vector<std::string>{}, 0, reservation};
    const auto snapshot = reopened.load();
    EXPECT_GE(snapshot.next_id, reservation_bound);
    // Reserved-but-unused IDs are skipped, so an old ID can no longer be stored.
    EXPECT_THROW(reopened.store(3, {{"name", "reused"}}, 4), std::invalid_argument);
}

// A graceful restart collapses the reservation, so IDs stay contiguous.
TEST(SegmentedStorageTests, KeepsIdsContiguousAcrossAGracefulRestart) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::Database database{std::make_unique<minidb::SegmentedStorage>(
            directory.path(), "users", 100000, default_object_schema(), std::vector<std::string>{}, 0, 32)};
        EXPECT_EQ(database.create({{"name", "Ada"}}), 0);
        EXPECT_EQ(database.create({{"name", "Grace"}}), 1);
    }

    minidb::Database restored{std::make_unique<minidb::SegmentedStorage>(
        directory.path(), "users", 100000, default_object_schema(), std::vector<std::string>{}, 0, 32)};
    EXPECT_EQ(restored.next_id(), 2);
    EXPECT_EQ(restored.create({{"name", "Alan"}}), 2);
}
