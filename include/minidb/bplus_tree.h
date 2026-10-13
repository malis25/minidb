#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minidb {

// A disk-resident B+ tree mapping arbitrary byte-string keys to byte-string
// values in sorted key order. Only the pages touched by a lookup are read, so
// memory use does not grow with the number of entries. Keys and values are
// compared as unsigned byte strings.
//
// Deletion marks an entry as a tombstone instead of rebalancing nodes. This
// keeps writes cheap and is safe here because the index is rebuilt from the
// object log when a database is opened.
class BPlusTree {
  private:
    struct LeafEntry {
        std::string key;
        std::string value;
        bool deleted = false;
    };

    struct InternalNode {
        std::vector<std::uint64_t> children;
        std::vector<std::string> keys;
    };

    struct Split {
        std::string separator;
        std::uint64_t right = 0;
    };

  public:
    static constexpr std::size_t node_size = 4096;
    static constexpr std::size_t max_key_size = 1000;
    static constexpr std::size_t max_value_size = 3000;

    explicit BPlusTree(std::filesystem::path path);
    ~BPlusTree();

    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    // Removes every entry and resets the tree to a single empty leaf.
    void clear();

    // Inserts or replaces the value for key.
    void put(std::string_view key, std::string_view value);

    // Copies the value for key into value and returns true when a live entry
    // exists.
    [[nodiscard]] bool get(std::string_view key, std::string& value);

    // Marks the entry for key as deleted. Returns true when a live entry existed.
    [[nodiscard]] bool remove(std::string_view key);

    [[nodiscard]] std::uint64_t live_entries() const noexcept { return entry_count_; }

    // Forward cursor over live entries in ascending key order.
    class Cursor {
      public:
        [[nodiscard]] bool valid() const noexcept { return leaf_page_ != 0; }
        [[nodiscard]] std::string_view key() const;
        [[nodiscard]] std::string_view value() const;
        void next();

      private:
        friend class BPlusTree;
        BPlusTree* tree_ = nullptr;
        std::uint64_t leaf_page_ = 0;
        std::size_t index_ = 0;
        std::uint64_t next_leaf_ = 0;
        std::vector<LeafEntry> entries_;

        void load(std::uint64_t page_id);
        void skip_deleted();
    };

    // Cursor positioned at the first live entry with key >= the requested key.
    [[nodiscard]] Cursor seek(std::string_view key);

    // Cursor positioned at the first live entry.
    [[nodiscard]] Cursor begin();

  private:
    void load();
    void write_header();

    void read_page(std::uint64_t page_id, std::vector<char>& buffer);
    void write_page(std::uint64_t page_id, const std::vector<char>& buffer);

    [[nodiscard]] InternalNode decode_internal(const std::vector<char>& page) const;
    [[nodiscard]] std::vector<LeafEntry> decode_leaf(const std::vector<char>& page,
                                                     std::uint64_t& next_leaf) const;

    [[nodiscard]] std::optional<Split> insert_into_leaf(std::uint64_t page_id, std::string_view key,
                                                        std::string_view value);
    [[nodiscard]] std::optional<Split> insert_into_internal(std::uint64_t page_id,
                                                            std::size_t child_index,
                                                            std::string_view separator,
                                                            std::uint64_t right);

    [[nodiscard]] std::uint64_t descend(std::string_view key,
                                        std::vector<std::pair<std::uint64_t, std::size_t>>* path);
    [[nodiscard]] static std::size_t find_child(const InternalNode& node, std::string_view key);
    [[nodiscard]] static std::size_t leaf_position(const std::vector<LeafEntry>& entries,
                                                   std::string_view key);

    [[nodiscard]] std::uint64_t allocate_page();
    void write_leaf(std::uint64_t page_id, const std::vector<LeafEntry>& entries,
                    std::uint64_t next_leaf);
    void write_internal(std::uint64_t page_id, const InternalNode& node);

    std::filesystem::path path_;
    std::fstream file_;
    std::uint64_t root_page_ = 0;
    std::uint64_t next_page_ = 1;
    std::uint32_t height_ = 0;
    std::uint64_t entry_count_ = 0;
};

} // namespace minidb
