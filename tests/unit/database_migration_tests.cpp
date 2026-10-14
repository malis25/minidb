#include "minidb/database.h"
#include "minidb/storage.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using test_support::make_disk_storage;
using test_support::small_segment_size;
using test_support::TemporaryDirectory;

TEST(SchemaMigrationTests, AdaptsExistingObjectsInResumableChunks) {
    minidb::Database database;
    for (int index = 0; index < 5; ++index) {
        EXPECT_EQ(database.create({{"name", "user" + std::to_string(index)}}),
                  static_cast<std::uint64_t>(index));
    }
    const minidb::ObjectSchema schema = {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}}}, {"active", {{"type", "boolean"}, {"default", true}}}}},
        {"required", nlohmann::json::array()}};

    database.begin_schema_migration(schema);
    const auto started = database.schema_migration_status();
    EXPECT_TRUE(started.pending);
    EXPECT_EQ(started.total, 5);
    // A second change must wait for the first to finish.
    EXPECT_THROW(database.begin_schema_migration(schema), std::logic_error);

    EXPECT_GT(database.migrate_schema_chunk(2), 0u);
    EXPECT_TRUE(database.schema_migration_status().pending);
    while (database.schema_migration_status().pending) {
        static_cast<void>(database.migrate_schema_chunk(2));
    }
    for (const auto id : database.ids()) {
        const auto object = database.get(id);
        ASSERT_TRUE(object);
        EXPECT_EQ(object->at("active"), true);
        EXPECT_TRUE(object->contains("name"));
    }
}

TEST(SchemaMigrationTests, CompletesAnInterruptedMigrationOnReopen) {
    TemporaryDirectory directory("minidb-segment-tests-");
    const minidb::ObjectSchema schema = {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}}}, {"active", {{"type", "boolean"}, {"default", true}}}}},
        {"required", nlohmann::json::array()}};
    {
        // Simulate a crash: the new schema and the pending flag are persisted,
        // but the existing object was never adapted.
        minidb::SegmentedStorage storage(directory.path(), "users", small_segment_size);
        static_cast<void>(storage.load());
        storage.store(0, {{"name", "Ada"}}, 1);
        storage.set_schema_migration_pending(true);
        storage.update_schema(schema);
        storage.close();
    }

    minidb::Database restored{
        std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", small_segment_size)};
    const auto object = restored.get(0);
    ASSERT_TRUE(object);
    EXPECT_EQ(object->at("name"), "Ada");
    EXPECT_EQ(object->at("active"), true);
    EXPECT_FALSE(restored.schema_migration_status().pending);

    // The completed state is durable: reopening finds nothing left to adapt.
    minidb::Database reopened{
        std::make_unique<minidb::SegmentedStorage>(directory.path(), "users", small_segment_size)};
    EXPECT_FALSE(reopened.schema_migration_status().pending);
    EXPECT_EQ(reopened.get(0)->at("active"), true);
}

TEST(SchemaMigrationTests, MigratesDiskBackedObjectsAndKeepsIndexesUsable) {
    TemporaryDirectory directory("minidb-segment-tests-");
    const minidb::ObjectSchema migrated = {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}}}, {"active", {{"type", "boolean"}, {"default", true}}}}},
        {"required", nlohmann::json::array()}};
    {
        minidb::Database database{make_disk_storage(directory.path(), {"name"}, 0)};
        ASSERT_TRUE(database.disk_backed());
        EXPECT_EQ(database.create({{"name", "Ada"}}), 0);
        EXPECT_EQ(database.create({{"name", "Grace"}}), 1);

        database.begin_schema_migration(migrated);
        while (database.schema_migration_status().pending) {
            static_cast<void>(database.migrate_schema_chunk(1));
        }
        const auto matches = database.find("name", "Ada");
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_EQ(matches.front().object->at("active"), true);
    }

    minidb::Database restored{make_disk_storage(directory.path(), {"name"}, 0)};
    EXPECT_FALSE(restored.schema_migration_status().pending);
    EXPECT_EQ(restored.get(0)->at("active"), true);
    EXPECT_EQ(restored.get(1)->at("active"), true);
    EXPECT_EQ(restored.find("name", "Grace").size(), 1u);
}
