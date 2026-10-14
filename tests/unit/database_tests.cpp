#include "minidb/database.h"
#include "minidb/storage.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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
