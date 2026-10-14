#include "minidb/database.h"
#include "minidb/storage.h"

#include <array>
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
        minidb::StorageSnapshot snapshot;
        snapshot.objects = {{{0, {{"name", "before"}}}}};
        snapshot.next_id = 1;
        return snapshot;
    }

    void store(std::uint64_t, const minidb::Object&, std::uint64_t) override {
        throw std::runtime_error("Test storage failure.");
    }

    void erase(std::uint64_t) override {
        throw std::runtime_error("Test storage failure.");
    }

    void apply_batch(const std::vector<minidb::StorageMutation>&, std::uint64_t) override {
        throw std::runtime_error("Test storage failure.");
    }

    void update_schema(const minidb::ObjectSchema&) override {
        throw std::runtime_error("Test storage failure.");
    }
};

constexpr std::uint64_t small_segment_size = 1;

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

TEST(DatabaseTests, UsesStableIdCursorForObjectPages) {
    minidb::Database database;
    for (int index = 0; index < 5; ++index) {
        static_cast<void>(database.create({{"room", "general"}}));
    }

    const auto first = database.ids_after(std::nullopt, 2);
    ASSERT_EQ(first.ids, (std::vector<std::uint64_t>{0, 1}));
    EXPECT_TRUE(first.has_more);

    const auto next = database.ids_after(first.ids.back(), 2);
    EXPECT_EQ(next.ids, (std::vector<std::uint64_t>{2, 3}));
    EXPECT_TRUE(next.has_more);

    const auto final = database.ids_after(next.ids.back(), 2);
    EXPECT_EQ(final.ids, (std::vector<std::uint64_t>{4}));
    EXPECT_FALSE(final.has_more);
}

TEST(DatabaseIndexTests, UsesStableIdCursorForMatchingObjectPages) {
    minidb::Database database;
    for (int index = 0; index < 5; ++index) {
        static_cast<void>(database.create({{"room", "general"}}));
    }

    const auto first = database.find_after("room", "general", std::nullopt, 2);
    ASSERT_EQ(first.matches.size(), 2);
    EXPECT_EQ(first.matches[0].id, 0);
    EXPECT_EQ(first.matches[1].id, 1);
    EXPECT_EQ(first.total, 5);
    EXPECT_TRUE(first.has_more);

    const auto next = database.find_after("room", "general", first.matches.back().id, 2);
    ASSERT_EQ(next.matches.size(), 2);
    EXPECT_EQ(next.matches[0].id, 2);
    EXPECT_EQ(next.matches[1].id, 3);
    EXPECT_TRUE(next.has_more);
}

TEST(DatabaseTests, RejectsObjectsBeyondMaximumNestingDepth) {
    minidb::Object nested = 0;
    for (int depth = 0; depth < minidb::maximum_object_nesting_depth - 1; ++depth) {
        nested = minidb::Object::array({std::move(nested)});
    }
    minidb::Database database;
    EXPECT_NO_THROW(static_cast<void>(database.create({{"value", nested}})));

    nested = minidb::Object::array({std::move(nested)});
    EXPECT_THROW(static_cast<void>(database.create({{"value", nested}})), std::invalid_argument);
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

TEST(DatabaseTests, AppliesBatchCreatesReplacementsAndDeletesTogether) {
    minidb::Database database;
    const auto first = database.create({{"name", "Ada"}, {"room", "general"}});
    const auto second = database.create({{"name", "Grace"}, {"room", "general"}});

    const auto created_ids = database.apply_batch(
        {{minidb::ObjectMutation::Type::create, 0, {{"name", "Alan"}, {"room", "random"}}},
         {minidb::ObjectMutation::Type::replace, first, {{"name", "Ada"}, {"room", "random"}}},
         {minidb::ObjectMutation::Type::erase, second, {}}});

    EXPECT_EQ(created_ids, (std::vector<std::uint64_t>{2}));
    EXPECT_EQ(database.next_id(), 3);
    EXPECT_FALSE(database.contains(second));
    EXPECT_EQ(database.find("room", "general").size(), 0);
    EXPECT_EQ(database.find("room", "random").size(), 2);
}

TEST(DatabaseTests, ValidatesEntireBatchBeforePersistingOrPublishingChanges) {
    minidb::Database database;
    const auto id = database.create({{"name", "Ada"}});

    EXPECT_THROW(static_cast<void>(database.apply_batch(
                     {{minidb::ObjectMutation::Type::create, 0, {{"name", "Grace"}}},
                      {minidb::ObjectMutation::Type::replace, 999, {{"name", "Missing"}}}})),
                 std::out_of_range);

    EXPECT_EQ(database.size(), 1);
    EXPECT_EQ(database.next_id(), 1);
    EXPECT_EQ(database.get(id)->at("name"), "Ada");
}

TEST(SegmentedStorageTests, CreatesCountBoundedSegmentsAndRecordsHeaderCounts) {
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
    TemporaryDirectory directory;
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

TEST(SegmentedStorageTests, ReplaysBatchAsOneAtomicStorageRecord) {
    TemporaryDirectory directory;
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

TEST(SegmentedStorageTests, RejectsLegacyManifestAndSegmentFormats) {
    TemporaryDirectory directory;
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
        manifest["format"] = "MINIDB-MANIFEST-4";
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

TEST(SegmentedStorageTests, RejectsUnsafeAndPlatformReservedDatabaseNames) {
    TemporaryDirectory directory;

    for (const auto* name :
         {"../users", "users/data", "users:stream", "con", "nul", "com1", "lpt9"}) {
        EXPECT_THROW(minidb::SegmentedStorage(directory.path(), name, small_segment_size),
                     std::invalid_argument)
            << name;
    }
}

TEST(SegmentedStorageTests, RejectsWritesBeforeLoading) {
    TemporaryDirectory directory;
    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);

    EXPECT_THROW(storage.store(0, {{"name", "Ada"}}, 1), std::logic_error);
    EXPECT_THROW(storage.erase(0), std::logic_error);
}

TEST(SegmentedStorageTests, RejectsMissingActiveSegmentInsteadOfSilentlyRecreatingIt) {
    TemporaryDirectory directory;
    minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
    static_cast<void>(storage.load());
    ASSERT_TRUE(std::filesystem::remove(directory.path() / "users.segment-00000000.minidb"));

    EXPECT_THROW(storage.store(0, {{"name", "Ada"}}, 1), std::runtime_error);
}

TEST(SegmentedStorageTests, RejectsMissingSegmentThatMayContainCommittedRecords) {
    TemporaryDirectory directory;
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
    TemporaryDirectory directory;
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
        orphan.write("MINIDB3\0", 8);
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
    TemporaryDirectory directory;
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

namespace {

minidb::ObjectSchema default_object_schema() {
    return {{"type", "object"},
            {"properties", minidb::Object::object()},
            {"additionalProperties", true}};
}

std::unique_ptr<minidb::SegmentedStorage>
make_disk_storage(const std::filesystem::path& directory, std::vector<std::string> indexed_fields,
                  std::uint64_t cache_bytes) {
    return std::make_unique<minidb::SegmentedStorage>(
        directory, "users", 10000, default_object_schema(), std::move(indexed_fields), cache_bytes);
}

} // namespace

TEST(DiskBackedStorageTests, ServesFieldIndexesAndObjectsFromDisk) {
    TemporaryDirectory directory;
    {
        minidb::Database database{make_disk_storage(directory.path(), {"email"}, 64)};
        EXPECT_TRUE(database.disk_backed());
        EXPECT_EQ(database.create({{"email", "ada@example.test"}, {"name", "Ada"}}), 0);
        EXPECT_EQ(database.create({{"email", "grace@example.test"}, {"name", "Grace"}}), 1);
        EXPECT_EQ(database.create({{"email", "ada@example.test"}, {"name", "Ada Two"}}), 2);

        const auto matches = database.find("email", "ada@example.test");
        ASSERT_EQ(matches.size(), 2U);
        EXPECT_EQ(matches.front().id, 0U);
        EXPECT_EQ(matches.back().id, 2U);
        EXPECT_TRUE(database.find("email", "missing@example.test").empty());

        ASSERT_TRUE(database.get(1));
        EXPECT_EQ(database.get(1)->at("name"), "Grace");

        EXPECT_TRUE(database.remove(0));
        EXPECT_EQ(database.find("email", "ada@example.test").size(), 1U);
    }

    // Reopen without naming indexes: the manifest is authoritative.
    minidb::Database restored{make_disk_storage(directory.path(), {}, 0)};
    EXPECT_TRUE(restored.disk_backed());
    EXPECT_EQ(restored.size(), 2U);
    ASSERT_EQ(restored.find("email", "grace@example.test").size(), 1U);
    EXPECT_EQ(restored.find("email", "grace@example.test").front().id, 1U);
    EXPECT_EQ(restored.find("email", "ada@example.test").size(), 1U);
}

TEST(DiskBackedStorageTests, ScansNonIndexedFields) {
    TemporaryDirectory directory;
    minidb::Database database{make_disk_storage(directory.path(), {"email"}, 0)};
    static_cast<void>(database.create({{"email", "a@example.test"}, {"city", "Ankara"}}));
    static_cast<void>(database.create({{"email", "b@example.test"}, {"city", "Izmir"}}));

    const auto matches = database.find("city", "Izmir");
    ASSERT_EQ(matches.size(), 1U);
    EXPECT_EQ(matches.front().id, 1U);
}

TEST(DiskBackedStorageTests, VerifiesCandidatesByReadingStoredObjects) {
    TemporaryDirectory directory;
    minidb::Database database{make_disk_storage(directory.path(), {"tag"}, 8)};
    static_cast<void>(database.create({{"tag", "one"}, {"n", 1}}));
    static_cast<void>(database.create({{"tag", "two"}, {"n", 2}}));
    static_cast<void>(database.create({{"tag", 3}, {"n", 3}}));

    EXPECT_EQ(database.find("tag", "one").size(), 1U);
    EXPECT_EQ(database.find("tag", "two").size(), 1U);
    EXPECT_EQ(database.find("tag", 3).size(), 1U);
    EXPECT_TRUE(database.find("tag", "three").empty());
}

TEST(DiskBackedStorageTests, CreatesIndexesAtRuntime) {
    TemporaryDirectory directory;
    minidb::Database database{make_disk_storage(directory.path(), {}, 0)};
    EXPECT_FALSE(database.disk_backed());
    static_cast<void>(database.create({{"email", "ada@example.test"}}));
    static_cast<void>(database.create({{"email", "grace@example.test"}}));
    EXPECT_EQ(database.find("email", "grace@example.test").size(), 1U);

    database.create_index("email");
    EXPECT_TRUE(database.disk_backed());
    ASSERT_EQ(database.indexed_fields().size(), 1U);
    EXPECT_EQ(database.indexed_fields().front(), "email");
    ASSERT_EQ(database.find("email", "grace@example.test").size(), 1U);
    EXPECT_EQ(database.find("email", "grace@example.test").front().id, 1U);

    database.drop_index("email");
    EXPECT_TRUE(database.indexed_fields().empty());
    EXPECT_EQ(database.find("email", "ada@example.test").size(), 1U);
}

TEST(DiskBackedStorageTests, ReplacesDropStaleIndexEntriesAfterReopen) {
    TemporaryDirectory directory;
    {
        minidb::Database database{make_disk_storage(directory.path(), {"email"}, 0)};
        static_cast<void>(database.create({{"email", "old@example.test"}}));
        database.set(0, {{"email", "new@example.test"}});
    }

    auto reopened = make_disk_storage(directory.path(), {}, 0);
    static_cast<void>(reopened->load());
    const auto old_value = minidb::Object{{"email", "old@example.test"}}.at("email").dump();
    const auto new_value = minidb::Object{{"email", "new@example.test"}}.at("email").dump();
    EXPECT_TRUE(reopened->query_index("email", old_value).empty());
    ASSERT_EQ(reopened->query_index("email", new_value).size(), 1U);
    EXPECT_EQ(reopened->query_index("email", new_value).front(), 0U);
}

// The manifest is only rewritten when a reserved block of IDs is exhausted, so
// creating N objects must cause far fewer than N writes.
TEST(SegmentedStorageTests, BatchesManifestWritesAcrossReservedIdBlocks) {
    TemporaryDirectory directory;
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
    TemporaryDirectory directory;
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
    TemporaryDirectory directory;
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

