#include "minidb/database.h"
#include "minidb/storage.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using test_support::small_segment_size;
using test_support::TemporaryDirectory;

TEST(SegmentedStorageTests, RejectsLegacyManifestAndSegmentFormats) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::Database database{
            std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", 10)};
        EXPECT_EQ(database.create({{"name", "Ada"}}), 0);
    }

    const auto manifest_path = directory.path() / "users.manifest.json";
    {
        std::ifstream input(manifest_path);
        auto manifest = nlohmann::json::parse(input);
        manifest["format"] = "MINIDB-MANIFEST-2";
        std::ofstream output(manifest_path, std::ios::binary | std::ios::trunc);
        output << manifest.dump(2) << '\n';
    }

    minidb::SegmentedStorage storage_legacy_manifest(directory.path(), "users", 10);
    EXPECT_THROW(static_cast<void>(storage_legacy_manifest.load()), std::runtime_error);

    {
        std::ifstream input(manifest_path);
        auto manifest = nlohmann::json::parse(input);
        manifest["format"] = "MINIDB-MANIFEST-5";
        std::ofstream output(manifest_path, std::ios::binary | std::ios::trunc);
        output << manifest.dump(2) << '\n';
    }

    const auto segment_path = directory.path() / "users.segment-00000000.minidb";
    std::vector<char> segment_bytes;
    {
        std::ifstream input(segment_path, std::ios::binary);
        segment_bytes.assign(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>());
    }
    ASSERT_GE(segment_bytes.size(), 56);
    {
        std::ofstream output(segment_path, std::ios::binary | std::ios::trunc);
        output.write("MINIDB2\0", 8);
        output.write(segment_bytes.data() + 8,
                     static_cast<std::streamsize>(segment_bytes.size() - 8));
    }

    minidb::SegmentedStorage storage_legacy_segment(directory.path(), "users", 10);
    EXPECT_THROW(static_cast<void>(storage_legacy_segment.load()), std::runtime_error);
}

TEST(SegmentedStorageTests, RejectsUnsafeAndPlatformReservedDatabaseNames) {
    TemporaryDirectory directory("minidb-segment-tests-");

    for (const auto* name :
         {"../users", "users/data", "users:stream", "con", "nul", "com1", "lpt9"}) {
        EXPECT_THROW(minidb::SegmentedStorage(directory.path(), name, small_segment_size),
                     std::invalid_argument)
            << name;
    }
}

TEST(SegmentedStorageTests, RejectsWritesBeforeLoading) {
    TemporaryDirectory directory("minidb-segment-tests-");
    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);

    EXPECT_THROW(storage.store(0, {{"name", "Ada"}}, 1), std::logic_error);
    EXPECT_THROW(storage.erase(0), std::logic_error);
}

TEST(SegmentedStorageTests, RejectsMissingActiveSegmentInsteadOfSilentlyRecreatingIt) {
    TemporaryDirectory directory("minidb-segment-tests-");
    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
    static_cast<void>(storage.load());
    ASSERT_TRUE(std::filesystem::remove(directory.path() / "users.segment-00000000.minidb"));

    EXPECT_THROW(storage.store(0, {{"name", "Ada"}}, 1), std::runtime_error);
}

TEST(SegmentedStorageTests, RejectsMissingSegmentThatMayContainCommittedRecords) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::Database database{std::make_unique<minidb::SegmentedStorage>(
            directory.path(), "users", small_segment_size)};
        ASSERT_EQ(database.create({{"name", "Ada"}}), 0);
    }
    ASSERT_TRUE(std::filesystem::remove(directory.path() / "users.segment-00000000.minidb"));

    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
    EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}

TEST(SegmentedStorageTests, RecoversAnEmptySegmentCreatedBeforeManifestCommit) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
        static_cast<void>(storage.load());
    }
    const auto orphan_path = directory.path() / "users.segment-00000001.minidb";
    {
        std::ofstream orphan(orphan_path, std::ios::binary);
        const auto write_u64 = [&](std::uint64_t value) {
            for (unsigned int shift = 0; shift < 64; shift += 8) {
                orphan.put(static_cast<char>((value >> shift) & 0xff));
            }
        };
        orphan.write("MINIDB4\0", 8);
        write_u64(0);
        write_u64(0);
        write_u64(0x4d494e4944423300ULL);
        write_u64(0);
        write_u64(0);
        write_u64(0x4d494e4944423300ULL);
    }

    minidb::Database restored{
        std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", small_segment_size)};
    ASSERT_EQ(restored.create({{"name", "Ada"}}), 0);
    EXPECT_GT(std::filesystem::file_size(orphan_path), 8);
    std::ifstream manifest_input(directory.path() / "users.manifest.json");
    nlohmann::json manifest;
    manifest_input >> manifest;
    EXPECT_EQ(manifest["segments"].size(), 2);
}

TEST(SegmentedStorageTests, RejectsSegmentSymlinks) {
    TemporaryDirectory directory("minidb-segment-tests-");
    const auto segment_path = directory.path() / "users.segment-00000000.minidb";
    const auto target_path = directory.path() / "outside.minidb";
    {
        minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
        static_cast<void>(storage.load());
    }
    std::filesystem::remove(segment_path);
    {
        std::ofstream target(target_path, std::ios::binary);
        target.write("MINIDB2\0", 8);
    }
    std::error_code symlink_error;
    std::filesystem::create_symlink(target_path, segment_path, symlink_error);
    if (symlink_error) {
        GTEST_SKIP() << "Creating a file symlink is not permitted: " << symlink_error.message();
    }

    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
    EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}

TEST(SegmentedStorageTests, RejectsMissingManifestWhenSegmentsRemain) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
        const auto snapshot = storage.load();
        EXPECT_EQ(snapshot.objects.size(), 0);
    }
    const bool manifest_removed = std::filesystem::remove(directory.path() / "users.manifest.json");
    EXPECT_TRUE(manifest_removed);

    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
    EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}

TEST(SegmentedStorageTests, RejectsMissingManifestWhenOnlyALaterSegmentRemains) {
    TemporaryDirectory directory("minidb-segment-tests-");
    {
        minidb::Database database{std::make_unique<minidb::SegmentedStorage>(
            directory.path(), "users", small_segment_size)};
        EXPECT_EQ(database.create({{"username", "ada"}}), 0);
        EXPECT_EQ(database.create({{"username", "grace"}}), 1);
    }
    const bool manifest_removed = std::filesystem::remove(directory.path() / "users.manifest.json");
    const bool first_segment_removed =
        std::filesystem::remove(directory.path() / "users.segment-00000000.minidb");
    EXPECT_TRUE(manifest_removed);
    EXPECT_TRUE(first_segment_removed);

    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
    EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}
