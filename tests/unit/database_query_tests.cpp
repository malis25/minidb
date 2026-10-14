#include "minidb/database.h"
#include "minidb/storage.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using test_support::make_disk_storage;
using test_support::TemporaryDirectory;

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

TEST(DiskBackedStorageTests, ServesFieldIndexesAndObjectsFromDisk) {
    TemporaryDirectory directory("minidb-segment-tests-");
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
    TemporaryDirectory directory("minidb-segment-tests-");
    minidb::Database database{make_disk_storage(directory.path(), {"email"}, 0)};
    static_cast<void>(database.create({{"email", "a@example.test"}, {"city", "Ankara"}}));
    static_cast<void>(database.create({{"email", "b@example.test"}, {"city", "Izmir"}}));

    const auto matches = database.find("city", "Izmir");
    ASSERT_EQ(matches.size(), 1U);
    EXPECT_EQ(matches.front().id, 1U);
}

TEST(DiskBackedStorageTests, VerifiesCandidatesByReadingStoredObjects) {
    TemporaryDirectory directory("minidb-segment-tests-");
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
    TemporaryDirectory directory("minidb-segment-tests-");
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
    TemporaryDirectory directory("minidb-segment-tests-");
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
