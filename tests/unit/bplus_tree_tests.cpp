#include "minidb/bplus_tree.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
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
                    ("minidb-bplus-tests-" +
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

std::string key_for(int value) {
    const auto digits = std::to_string(value);
    return "key-" + std::string(6 - digits.size(), '0') + digits;
}

} // namespace

TEST(BPlusTreeTests, StoresAndReadsValues) {
    TemporaryDirectory directory;
    minidb::BPlusTree tree(directory.path() / "index.idx");

    tree.put("alpha", "one");
    tree.put("beta", "two");

    std::string value;
    ASSERT_TRUE(tree.get("alpha", value));
    EXPECT_EQ(value, "one");
    ASSERT_TRUE(tree.get("beta", value));
    EXPECT_EQ(value, "two");
    EXPECT_FALSE(tree.get("gamma", value));
    EXPECT_EQ(tree.live_entries(), 2U);
}

TEST(BPlusTreeTests, ReplacesAndRemovesEntries) {
    TemporaryDirectory directory;
    minidb::BPlusTree tree(directory.path() / "index.idx");

    tree.put("alpha", "one");
    tree.put("alpha", "updated");
    std::string value;
    ASSERT_TRUE(tree.get("alpha", value));
    EXPECT_EQ(value, "updated");
    EXPECT_EQ(tree.live_entries(), 1U);

    EXPECT_TRUE(tree.remove("alpha"));
    EXPECT_FALSE(tree.remove("alpha"));
    EXPECT_FALSE(tree.get("alpha", value));
    EXPECT_EQ(tree.live_entries(), 0U);
}

TEST(BPlusTreeTests, KeepsEveryEntryAcrossManySplits) {
    TemporaryDirectory directory;
    minidb::BPlusTree tree(directory.path() / "index.idx");

    constexpr int entry_count = 5000;
    for (int index = 0; index < entry_count; ++index) {
        tree.put(key_for(index), "value-" + std::to_string(index));
    }
    EXPECT_EQ(tree.live_entries(), static_cast<std::uint64_t>(entry_count));

    std::string value;
    for (int index = 0; index < entry_count; ++index) {
        ASSERT_TRUE(tree.get(key_for(index), value)) << index;
        EXPECT_EQ(value, "value-" + std::to_string(index));
    }

    for (int index = 0; index < entry_count; index += 3) {
        EXPECT_TRUE(tree.remove(key_for(index)));
    }
    for (int index = 0; index < entry_count; ++index) {
        const bool present = tree.get(key_for(index), value);
        EXPECT_EQ(present, index % 3 != 0) << index;
    }
}

TEST(BPlusTreeTests, WalksEntriesInSortedOrder) {
    TemporaryDirectory directory;
    minidb::BPlusTree tree(directory.path() / "index.idx");

    for (int index = 0; index < 2000; ++index) {
        tree.put(key_for(index), std::to_string(index));
    }

    int expected = 0;
    for (auto cursor = tree.begin(); cursor.valid(); cursor.next()) {
        EXPECT_EQ(cursor.key(), key_for(expected));
        ++expected;
    }
    EXPECT_EQ(expected, 2000);

    auto cursor = tree.seek(key_for(1500));
    ASSERT_TRUE(cursor.valid());
    EXPECT_EQ(cursor.key(), key_for(1500));
}

TEST(BPlusTreeTests, PersistsEntriesAcrossReopen) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "index.idx";
    {
        minidb::BPlusTree tree(path);
        for (int index = 0; index < 500; ++index) {
            tree.put(key_for(index), std::to_string(index));
        }
    }

    minidb::BPlusTree reopened(path);
    EXPECT_EQ(reopened.live_entries(), 500U);
    std::string value;
    ASSERT_TRUE(reopened.get(key_for(250), value));
    EXPECT_EQ(value, "250");
}

TEST(BPlusTreeTests, ClearsEveryEntry) {
    TemporaryDirectory directory;
    minidb::BPlusTree tree(directory.path() / "index.idx");
    tree.put("alpha", "one");
    tree.put("beta", "two");

    tree.clear();

    EXPECT_EQ(tree.live_entries(), 0U);
    std::string value;
    EXPECT_FALSE(tree.get("alpha", value));
    auto cursor = tree.begin();
    EXPECT_FALSE(cursor.valid());
}

namespace {

std::uint64_t read_u64_be(const std::vector<char>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + shift / 8]);
    }
    return value;
}

void overwrite_byte(const std::filesystem::path& path, std::uint64_t offset, char value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file);
    file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file.put(value);
}

} // namespace

// A header whose checksum no longer matches its fields is unusable.
TEST(BPlusTreeTests, RejectsACorruptHeaderChecksum) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "index.idx";
    {
        minidb::BPlusTree tree(path);
        tree.put("alpha", "one");
        tree.put("beta", "two");
    }
    // Flip the low byte of the entry-count field, leaving magic and page size
    // intact so only the checksum can detect the corruption.
    overwrite_byte(path, 33, static_cast<char>(0x7f));

    EXPECT_THROW(minidb::BPlusTree tree(path), std::runtime_error);
}

// A page whose type byte is invalid must raise an explicit error when read.
TEST(BPlusTreeTests, RejectsAnInvalidPageType) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "index.idx";
    std::uint64_t root_page = 0;
    {
        minidb::BPlusTree tree(path);
        tree.put("alpha", "one");
        tree.put("beta", "two");
    }
    {
        std::fstream file(path, std::ios::binary | std::ios::in);
        std::vector<char> header(minidb::BPlusTree::node_size, 0);
        file.read(header.data(), static_cast<std::streamsize>(header.size()));
        root_page = read_u64_be(header, 20);
    }
    ASSERT_NE(root_page, 0U);
    overwrite_byte(path, root_page * minidb::BPlusTree::node_size, static_cast<char>(0));

    minidb::BPlusTree tree(path);
    std::string value;
    EXPECT_THROW(static_cast<void>(tree.get("alpha", value)), std::runtime_error);
}

// A file shorter than one page has no header and is rebuilt as an empty tree.
TEST(BPlusTreeTests, TreatsATruncatedFileAsEmpty) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "index.idx";
    {
        minidb::BPlusTree tree(path);
        tree.put("alpha", "one");
    }
    std::filesystem::resize_file(path, 3);

    minidb::BPlusTree tree(path);
    EXPECT_EQ(tree.live_entries(), 0U);
    tree.put("fresh", "value");
    std::string value;
    ASSERT_TRUE(tree.get("fresh", value));
    EXPECT_EQ(value, "value");
}
