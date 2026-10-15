#include "minidb/bplus_tree.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace minidb {

namespace {

constexpr char tree_magic[8] = {'M', 'I', 'N', 'I', 'D', 'X', '0', '2'};
constexpr char internal_page = 1;
constexpr char leaf_page = 2;

constexpr std::size_t header_page_size = 8;
constexpr std::size_t header_next_page = 12;
constexpr std::size_t header_root_page = 20;
constexpr std::size_t header_height = 28;
constexpr std::size_t header_entry_count = 32;
// A checksum over the header fields detects a torn or bit-flipped header. The
// index is rebuilt from the object log, so an invalid header only triggers a
// rebuild rather than data loss.
constexpr std::size_t header_checksum_offset = 40;

std::uint64_t tree_header_checksum(const std::vector<char>& header) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < header_checksum_offset; ++index) {
        hash ^= static_cast<unsigned char>(header[index]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void put_u16(std::vector<char>& buffer, std::size_t offset, std::uint16_t value) {
    buffer[offset] = static_cast<char>((value >> 8) & 0xff);
    buffer[offset + 1] = static_cast<char>(value & 0xff);
}

void put_u32(std::vector<char>& buffer, std::size_t offset, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        buffer[offset + shift / 8] = static_cast<char>((value >> (24 - shift)) & 0xff);
    }
}

void put_u64(std::vector<char>& buffer, std::size_t offset, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        buffer[offset + shift / 8] = static_cast<char>((value >> (56 - shift)) & 0xff);
    }
}

std::uint16_t read_u16(const std::vector<char>& buffer, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<unsigned char>(buffer[offset])) << 8) |
        static_cast<std::uint16_t>(static_cast<unsigned char>(buffer[offset + 1])));
}

std::uint32_t read_u32(const std::vector<char>& buffer, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        value = (value << 8) | static_cast<unsigned char>(buffer[offset + shift / 8]);
    }
    return value;
}

std::uint64_t read_u64(const std::vector<char>& buffer, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        value = (value << 8) | static_cast<unsigned char>(buffer[offset + shift / 8]);
    }
    return value;
}

} // namespace

BPlusTree::BPlusTree(std::filesystem::path path) : path_(std::move(path)) {
    if (!std::filesystem::exists(path_)) {
        std::ofstream create(path_, std::ios::binary | std::ios::trunc);
        if (!create) {
            throw std::runtime_error("Could not create B+ tree file: " + path_.string());
        }
    }
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) {
        throw std::runtime_error("Could not open B+ tree file: " + path_.string());
    }
    load();
}

BPlusTree::~BPlusTree() {
    if (file_.is_open()) {
        try {
            write_header();
        } catch (...) {
        }
        file_.close();
    }
}

void BPlusTree::load() {
    std::error_code error;
    const auto size = std::filesystem::file_size(path_, error);
    if (error) {
        throw std::system_error(error, "Could not inspect B+ tree file: " + path_.string());
    }
    if (size < node_size) {
        write_header();
        return;
    }
    std::vector<char> header(node_size, 0);
    file_.clear();
    file_.seekg(0, std::ios::beg);
    file_.read(header.data(), static_cast<std::streamsize>(node_size));
    if (!file_) {
        throw std::runtime_error("Could not read B+ tree header: " + path_.string());
    }
    if (std::memcmp(header.data(), tree_magic, sizeof(tree_magic)) != 0 ||
        read_u32(header, header_page_size) != node_size) {
        throw std::runtime_error("Invalid B+ tree file: " + path_.string());
    }
    if (read_u64(header, header_checksum_offset) != tree_header_checksum(header)) {
        throw std::runtime_error("Corrupt B+ tree header: " + path_.string());
    }
    next_page_ = read_u64(header, header_next_page);
    root_page_ = read_u64(header, header_root_page);
    height_ = read_u32(header, header_height);
    entry_count_ = read_u64(header, header_entry_count);
}

void BPlusTree::write_header() {
    std::vector<char> header(node_size, 0);
    std::memcpy(header.data(), tree_magic, sizeof(tree_magic));
    put_u32(header, header_page_size, static_cast<std::uint32_t>(node_size));
    put_u64(header, header_next_page, next_page_);
    put_u64(header, header_root_page, root_page_);
    put_u32(header, header_height, height_);
    put_u64(header, header_entry_count, entry_count_);
    put_u64(header, header_checksum_offset, tree_header_checksum(header));
    write_page(0, header);
}

void BPlusTree::clear() {
    file_.close();
    {
        std::ofstream truncate(path_, std::ios::binary | std::ios::trunc);
        if (!truncate) {
            throw std::runtime_error("Could not reset B+ tree file: " + path_.string());
        }
    }
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) {
        throw std::runtime_error("Could not reopen B+ tree file: " + path_.string());
    }
    root_page_ = 0;
    next_page_ = 1;
    height_ = 0;
    entry_count_ = 0;
    write_header();
}

void BPlusTree::read_page(std::uint64_t page_id, std::vector<char>& buffer) {
    buffer.assign(node_size, 0);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(page_id * node_size), std::ios::beg);
    file_.read(buffer.data(), static_cast<std::streamsize>(node_size));
    if (!file_) {
        throw std::runtime_error("Could not read B+ tree page " + std::to_string(page_id) +
                                 ": " + path_.string());
    }
}

void BPlusTree::write_page(std::uint64_t page_id, const std::vector<char>& buffer) {
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(page_id * node_size), std::ios::beg);
    file_.write(buffer.data(), static_cast<std::streamsize>(node_size));
    file_.flush();
    if (!file_) {
        throw std::runtime_error("Could not write B+ tree page " + std::to_string(page_id) +
                                 ": " + path_.string());
    }
}

BPlusTree::InternalNode BPlusTree::decode_internal(const std::vector<char>& page) const {
    if (page.size() < node_size || page[0] != internal_page) {
        throw std::runtime_error("Corrupt B+ tree internal page: " + path_.string());
    }
    InternalNode node;
    const auto count = read_u16(page, 1);
    std::size_t offset = 3;
    node.children.reserve(static_cast<std::size_t>(count) + 1);
    node.keys.reserve(count);
    if (offset + 8 > page.size()) {
        throw std::runtime_error("Truncated B+ tree internal page: " + path_.string());
    }
    node.children.push_back(read_u64(page, offset));
    offset += 8;
    for (std::uint16_t index = 0; index < count; ++index) {
        if (offset + 2 > page.size()) {
            throw std::runtime_error("Truncated B+ tree internal page: " + path_.string());
        }
        const auto key_length = read_u16(page, offset);
        offset += 2;
        if (offset + key_length + 8 > page.size()) {
            throw std::runtime_error("Corrupt B+ tree internal page: " + path_.string());
        }
        node.keys.emplace_back(page.data() + offset, key_length);
        offset += key_length;
        node.children.push_back(read_u64(page, offset));
        offset += 8;
    }
    return node;
}

std::vector<BPlusTree::LeafEntry> BPlusTree::decode_leaf(const std::vector<char>& page,
                                                         std::uint64_t& next_leaf) const {
    if (page.size() < node_size || page[0] != leaf_page) {
        throw std::runtime_error("Corrupt B+ tree leaf page: " + path_.string());
    }
    const auto count = read_u16(page, 1);
    if (11 > page.size()) {
        throw std::runtime_error("Truncated B+ tree leaf page: " + path_.string());
    }
    next_leaf = read_u64(page, 3);
    std::size_t offset = 11;
    std::vector<LeafEntry> entries;
    entries.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        if (offset + 5 > page.size()) {
            throw std::runtime_error("Truncated B+ tree leaf page: " + path_.string());
        }
        LeafEntry entry;
        entry.deleted = page[offset] != 0;
        offset += 1;
        const auto key_length = read_u16(page, offset);
        offset += 2;
        const auto value_length = read_u16(page, offset);
        offset += 2;
        if (offset + key_length + value_length > page.size()) {
            throw std::runtime_error("Corrupt B+ tree leaf page: " + path_.string());
        }
        entry.key.assign(page.data() + offset, key_length);
        offset += key_length;
        entry.value.assign(page.data() + offset, value_length);
        offset += value_length;
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::size_t BPlusTree::find_child(const InternalNode& node, std::string_view key) {
    const auto position = std::upper_bound(
        node.keys.begin(), node.keys.end(), key,
        [](std::string_view value, const std::string& element) { return value < element; });
    auto index = static_cast<std::size_t>(position - node.keys.begin());
    if (index >= node.children.size()) {
        index = node.children.size() - 1;
    }
    return index;
}

std::size_t BPlusTree::leaf_position(const std::vector<LeafEntry>& entries, std::string_view key) {
    const auto position = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const LeafEntry& entry, std::string_view value) { return entry.key < value; });
    return static_cast<std::size_t>(position - entries.begin());
}

std::uint64_t BPlusTree::allocate_page() {
    return next_page_++;
}

void BPlusTree::write_leaf(std::uint64_t page_id, const std::vector<LeafEntry>& entries,
                           std::uint64_t next_leaf) {
    std::vector<char> page(node_size, 0);
    page[0] = leaf_page;
    put_u16(page, 1, static_cast<std::uint16_t>(entries.size()));
    put_u64(page, 3, next_leaf);
    std::size_t offset = 11;
    for (const auto& entry : entries) {
        if (offset + 5 + entry.key.size() + entry.value.size() > node_size) {
            throw std::length_error("B+ tree leaf page overflow.");
        }
        page[offset++] = entry.deleted ? 1 : 0;
        put_u16(page, offset, static_cast<std::uint16_t>(entry.key.size()));
        offset += 2;
        put_u16(page, offset, static_cast<std::uint16_t>(entry.value.size()));
        offset += 2;
        std::memcpy(page.data() + offset, entry.key.data(), entry.key.size());
        offset += entry.key.size();
        std::memcpy(page.data() + offset, entry.value.data(), entry.value.size());
        offset += entry.value.size();
    }
    write_page(page_id, page);
}

void BPlusTree::write_internal(std::uint64_t page_id, const InternalNode& node) {
    std::vector<char> page(node_size, 0);
    page[0] = internal_page;
    put_u16(page, 1, static_cast<std::uint16_t>(node.keys.size()));
    std::size_t offset = 3;
    put_u64(page, offset, node.children.front());
    offset += 8;
    for (std::size_t index = 0; index < node.keys.size(); ++index) {
        if (offset + 2 + node.keys[index].size() + 8 > node_size) {
            throw std::length_error("B+ tree internal page overflow.");
        }
        put_u16(page, offset, static_cast<std::uint16_t>(node.keys[index].size()));
        offset += 2;
        std::memcpy(page.data() + offset, node.keys[index].data(), node.keys[index].size());
        offset += node.keys[index].size();
        put_u64(page, offset, node.children[index + 1]);
        offset += 8;
    }
    write_page(page_id, page);
}

std::uint64_t BPlusTree::descend(std::string_view key,
                                 std::vector<std::pair<std::uint64_t, std::size_t>>* path) {
    std::uint64_t page_id = root_page_;
    auto level = height_;
    while (level > 1) {
        std::vector<char> buffer;
        read_page(page_id, buffer);
        const auto node = decode_internal(buffer);
        const auto child = find_child(node, key);
        if (path != nullptr) {
            path->push_back({page_id, child});
        }
        page_id = node.children[child];
        --level;
    }
    return page_id;
}

std::optional<BPlusTree::Split> BPlusTree::insert_into_leaf(std::uint64_t page_id,
                                                            std::string_view key,
                                                            std::string_view value) {
    std::vector<char> buffer;
    read_page(page_id, buffer);
    std::uint64_t next_leaf = 0;
    auto entries = decode_leaf(buffer, next_leaf);

    const auto position = leaf_position(entries, key);
    if (position < entries.size() && entries[position].key == key) {
        if (entries[position].deleted) {
            ++entry_count_;
        }
        entries[position].value.assign(value);
        entries[position].deleted = false;
        write_leaf(page_id, entries, next_leaf);
        return std::nullopt;
    }

    if (key.size() > max_key_size) {
        throw std::length_error("B+ tree key exceeds the maximum size of 1000 bytes.");
    }
    if (value.size() > max_value_size) {
        throw std::length_error("B+ tree value exceeds the maximum size of 3000 bytes.");
    }

    LeafEntry entry;
    entry.key.assign(key);
    entry.value.assign(value);
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(position), std::move(entry));
    ++entry_count_;

    const auto serialized_size = [](const std::vector<LeafEntry>& list) {
        std::size_t total = 11;
        for (const auto& item : list) {
            total += 5 + item.key.size() + item.value.size();
        }
        return total;
    };
    if (serialized_size(entries) <= node_size) {
        write_leaf(page_id, entries, next_leaf);
        return std::nullopt;
    }

    std::size_t split = 0;
    std::size_t running = 11;
    while (split < entries.size()) {
        const auto item_size = 5 + entries[split].key.size() + entries[split].value.size();
        if (split > 0 && running + item_size > node_size) {
            break;
        }
        running += item_size;
        ++split;
    }
    if (split == 0 || split >= entries.size()) {
        split = entries.size() / 2;
    }

    std::vector<LeafEntry> left(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(split));
    std::vector<LeafEntry> right(entries.begin() + static_cast<std::ptrdiff_t>(split), entries.end());
    const auto right_page = allocate_page();
    write_leaf(right_page, right, next_leaf);
    write_leaf(page_id, left, right_page);
    return Split{right.front().key, right_page};
}

std::optional<BPlusTree::Split> BPlusTree::insert_into_internal(std::uint64_t page_id,
                                                                std::size_t child_index,
                                                                std::string_view separator,
                                                                std::uint64_t right_child) {
    std::vector<char> buffer;
    read_page(page_id, buffer);
    auto node = decode_internal(buffer);

    node.keys.insert(node.keys.begin() + static_cast<std::ptrdiff_t>(child_index),
                     std::string(separator));
    node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(child_index + 1),
                         right_child);

    const auto serialized_size = [](const InternalNode& value) {
        std::size_t total = 11;
        for (const auto& item : value.keys) {
            total += 10 + item.size();
        }
        return total;
    };
    if (serialized_size(node) <= node_size) {
        write_internal(page_id, node);
        return std::nullopt;
    }

    const auto middle = node.keys.size() / 2;
    const auto promoted = node.keys[middle];
    InternalNode left;
    InternalNode right;
    left.children.assign(node.children.begin(),
                         node.children.begin() + static_cast<std::ptrdiff_t>(middle) + 1);
    left.keys.assign(node.keys.begin(), node.keys.begin() + static_cast<std::ptrdiff_t>(middle));
    right.children.assign(node.children.begin() + static_cast<std::ptrdiff_t>(middle) + 1,
                          node.children.end());
    right.keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(middle) + 1, node.keys.end());

    const auto right_page = allocate_page();
    write_internal(right_page, right);
    write_internal(page_id, left);
    return Split{promoted, right_page};
}

void BPlusTree::put(std::string_view key, std::string_view value) {
    if (root_page_ == 0) {
        root_page_ = allocate_page();
        height_ = 1;
        write_leaf(root_page_, {}, 0);
    }

    std::vector<std::pair<std::uint64_t, std::size_t>> path;
    const auto leaf = descend(key, &path);
    auto split = insert_into_leaf(leaf, key, value);

    while (split) {
        if (path.empty()) {
            const auto new_root = allocate_page();
            InternalNode root;
            root.children = {root_page_, split->right};
            root.keys = {split->separator};
            write_internal(new_root, root);
            root_page_ = new_root;
            ++height_;
            split.reset();
        } else {
            const auto [parent_page, child_index] = path.back();
            path.pop_back();
            split = insert_into_internal(parent_page, child_index, split->separator, split->right);
        }
    }
    write_header();
}

bool BPlusTree::get(std::string_view key, std::string& value) {
    if (root_page_ == 0) {
        return false;
    }
    const auto leaf = descend(key, nullptr);
    std::vector<char> buffer;
    read_page(leaf, buffer);
    std::uint64_t next_leaf = 0;
    auto entries = decode_leaf(buffer, next_leaf);
    const auto position = leaf_position(entries, key);
    if (position < entries.size() && entries[position].key == key && !entries[position].deleted) {
        value = entries[position].value;
        return true;
    }
    return false;
}

bool BPlusTree::remove(std::string_view key) {
    if (root_page_ == 0) {
        return false;
    }
    const auto leaf = descend(key, nullptr);
    std::vector<char> buffer;
    read_page(leaf, buffer);
    std::uint64_t next_leaf = 0;
    auto entries = decode_leaf(buffer, next_leaf);
    const auto position = leaf_position(entries, key);
    if (position < entries.size() && entries[position].key == key && !entries[position].deleted) {
        entries[position].deleted = true;
        --entry_count_;
        write_leaf(leaf, entries, next_leaf);
        write_header();
        return true;
    }
    return false;
}

void BPlusTree::Cursor::load(std::uint64_t page_id) {
    std::vector<char> page;
    tree_->read_page(page_id, page);
    entries_ = tree_->decode_leaf(page, next_leaf_);
    leaf_page_ = page_id;
    index_ = 0;
    skip_deleted();
}

void BPlusTree::Cursor::skip_deleted() {
    while (leaf_page_ != 0) {
        while (index_ < entries_.size() && entries_[index_].deleted) {
            ++index_;
        }
        if (index_ < entries_.size()) {
            return;
        }
        if (next_leaf_ == 0) {
            leaf_page_ = 0;
            return;
        }
        load(next_leaf_);
    }
}

std::string_view BPlusTree::Cursor::key() const {
    return entries_[index_].key;
}

std::string_view BPlusTree::Cursor::value() const {
    return entries_[index_].value;
}

void BPlusTree::Cursor::next() {
    if (leaf_page_ == 0) {
        return;
    }
    ++index_;
    skip_deleted();
}

BPlusTree::Cursor BPlusTree::seek(std::string_view key) {
    Cursor cursor;
    cursor.tree_ = this;
    if (root_page_ == 0) {
        return cursor;
    }
    const auto leaf = descend(key, nullptr);
    cursor.load(leaf);
    while (cursor.valid() && cursor.key() < key) {
        cursor.next();
    }
    return cursor;
}

BPlusTree::Cursor BPlusTree::begin() {
    return seek(std::string_view{});
}

} // namespace minidb
