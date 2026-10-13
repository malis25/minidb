#include "minidb/database.h"
#include "minidb/storage.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        std::random_device random;
        for (int attempt = 0; attempt < 10; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("minidb-segment-tests-" +
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

class FailingStorage final : public minidb::ObjectStorage {
  public:
    [[nodiscard]] minidb::StorageSnapshot load() override {
        return {{{0, {{"name", "before"}}}}, 1};
    }

    void store(std::uint64_t, const minidb::Object&, std::uint64_t) override {
        throw std::runtime_error("Test storage failure.");
    }

    void erase(std::uint64_t) override {
        throw std::runtime_error("Test storage failure.");
    }
};

constexpr std::uint64_t small_segment_size = 80;

} // namespace

TEST(DatabaseTests, StartsIdSequenceAtZero) {
    minidb::Database database;

    EXPECT_EQ(database.create({{"name", "Ada"}}), 0);
    EXPECT_EQ(database.create({{"name", "Grace"}}), 1);
    EXPECT_EQ(database.next_id(), 2);
}

TEST(DatabaseTests, StoresTypedAndNestedObjectProperties) {
    minidb::Database database;
    const minidb::Object user = {
        {"username", "ada"},
        {"age", 36},
        {"score", 99.5},
        {"friends", {12, 18}},
        {"profile", {{"email", "ada@example.test"}, {"active", true}}},
    };

    const auto id = database.create(user);

    const auto stored = database.get(id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(*stored, user);
    EXPECT_TRUE(stored->at("age").is_number_integer());
    EXPECT_TRUE(stored->at("score").is_number_float());
    EXPECT_TRUE(stored->at("friends").is_array());
    EXPECT_TRUE(stored->at("profile").is_object());
}

TEST(DatabaseTests, ReplacesAndRemovesUsingNumericId) {
    minidb::Database database;
    const auto id = database.create({{"id", "application-id"}, {"name", "Ada"}});

    database.set(id, {{"id", "application-id"}, {"name", "Grace"}});
    ASSERT_TRUE(database.get(id));
    EXPECT_EQ(database.get(id)->at("id"), "application-id");
    EXPECT_EQ(database.get(id)->at("name"), "Grace");
    EXPECT_TRUE(database.remove(id));
    EXPECT_FALSE(database.remove(id));
    EXPECT_EQ(database.next_id(), 1);
}

TEST(DatabaseTests, RejectsNonObjectsAndUpdatesToMissingIds) {
    minidb::Database database;

    EXPECT_THROW(static_cast<void>(database.create("not an object")), std::invalid_argument);
    EXPECT_THROW(database.set(0, {{"name", "Ada"}}), std::out_of_range);
    EXPECT_EQ(database.size(), 0);
}

TEST(DatabaseTests, ListsNumericIdsInStableOrder) {
    minidb::Database database;
    EXPECT_EQ(database.create({{"name", "zero"}}), 0);
    EXPECT_EQ(database.create({{"name", "one"}}), 1);
    EXPECT_EQ(database.create({{"name", "two"}}), 2);
    EXPECT_TRUE(database.remove(1));

    EXPECT_EQ(database.ids(), (std::vector<std::uint64_t>{0, 2}));
}

TEST(DatabaseIndexTests, ReturnsEveryObjectMatchingAFieldValue) {
    minidb::Database database;
    static_cast<void>(database.create({{"email", "same@example.test"}, {"username", "ada"}}));
    static_cast<void>(database.create({{"email", "other@example.test"}, {"username", "grace"}}));
    static_cast<void>(database.create({{"email", "same@example.test"}, {"username", "lovelace"}}));

    const auto matches = database.find("email", "same@example.test");

    ASSERT_EQ(matches.size(), 2);
    EXPECT_EQ(matches[0].id, 0);
    EXPECT_EQ(matches[0].object->at("username"), "ada");
    EXPECT_EQ(matches[1].id, 2);
    EXPECT_EQ(matches[1].object->at("username"), "lovelace");
}

TEST(DatabaseIndexTests, PreservesJsonTypesWhenMatchingValues) {
    minidb::Database database;
    static_cast<void>(database.create({{"score", 1}}));
    static_cast<void>(database.create({{"score", 1.0}}));
    static_cast<void>(database.create({{"score", "1"}}));

    EXPECT_EQ(database.find("score", 1).size(), 1);
    EXPECT_EQ(database.find("score", 1.0).size(), 1);
    EXPECT_EQ(database.find("score", "1").size(), 1);
}

TEST(DatabaseIndexTests, UpdatesIndexesWhenAnObjectChanges) {
    minidb::Database database;
    const auto id = database.create({{"email", "old@example.test"}, {"active", true}});

    database.set(id, {{"email", "new@example.test"}, {"active", false}});

    EXPECT_TRUE(database.find("email", "old@example.test").empty());
    EXPECT_EQ(database.find("email", "new@example.test").front().id, id);
    EXPECT_TRUE(database.find("active", true).empty());
    EXPECT_EQ(database.find("active", false).front().id, id);
}

TEST(DatabaseIndexTests, RemovesIndexesWhenAnObjectIsDeleted) {
    minidb::Database database;
    const auto id = database.create({{"username", "ada"}});

    ASSERT_TRUE(database.remove(id));

    EXPECT_TRUE(database.find("username", "ada").empty());
}

TEST(DatabaseTests, StorageFailureDoesNotPublishOrReplaceAnObject) {
    minidb::Database database{std::make_unique<FailingStorage>()};

    EXPECT_THROW(database.set(0, {{"name", "after"}}), std::runtime_error);
    EXPECT_THROW(static_cast<void>(database.create({{"name", "Grace"}})), std::runtime_error);
    EXPECT_EQ(database.get(0)->at("name"), "before");
    EXPECT_EQ(database.size(), 1);
    EXPECT_EQ(database.next_id(), 1);
}

TEST(SegmentedStorageTests, CreatesManifestAndRollsOverAtConfiguredLimit) {
    TemporaryDirectory directory;
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
        EXPECT_LE(std::filesystem::file_size(segment), small_segment_size);
    }
}

TEST(SegmentedStorageTests, PersistsIdsIndexesUpdatesAndDeletesAcrossRestart) {
    TemporaryDirectory directory;
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

TEST(SegmentedStorageTests, RecoversNextIdFromACommittedRecord) {
    TemporaryDirectory directory;
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
    TemporaryDirectory directory;
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

TEST(SegmentedStorageTests, RejectsAFileLimitTooSmallForAnObject) {
    TemporaryDirectory directory;

    EXPECT_THROW(minidb::SegmentedStorage(directory.path(), "users", 1), std::invalid_argument);
}

TEST(SegmentedStorageTests, RejectsWritesBeforeLoading) {
    TemporaryDirectory directory;
    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);

    EXPECT_THROW(storage.store(0, {{"name", "Ada"}}, 1), std::logic_error);
    EXPECT_THROW(storage.erase(0), std::logic_error);
}

TEST(SegmentedStorageTests, RejectsMissingManifestWhenSegmentsRemain) {
    TemporaryDirectory directory;
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
    TemporaryDirectory directory;
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
