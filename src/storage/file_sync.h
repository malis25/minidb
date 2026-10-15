#pragma once

#include <filesystem>

namespace minidb::detail {

// Flushes a file's buffered contents all the way to stable storage. A C++
// stream flush() (and even an OS write()) only moves bytes into the kernel page
// cache; a power loss or kernel panic can still discard them. This issues the
// platform durability primitive (FlushFileBuffers on Windows, fsync elsewhere)
// so a record that a caller has acknowledged cannot silently disappear.
//
// The file must not be held open for writing by another handle when this is
// called on Windows, because that would deny the temporary write access needed
// to flush it.
void sync_file(const std::filesystem::path& path);

// Flushes a directory entry so a freshly created or renamed file is reachable
// after a crash. On Windows this is a no-op: NTFS journals directory metadata,
// and flushing a directory handle requires privileges the process does not have.
void sync_directory(const std::filesystem::path& path);

} // namespace minidb::detail
