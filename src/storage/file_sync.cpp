#include "file_sync.h"

#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <chrono>
#include <thread>
#endif

namespace minidb::detail {

#ifdef _WIN32

namespace {

bool is_transient_sharing_error(unsigned long error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
           error == ERROR_LOCK_VIOLATION;
}

} // namespace

void sync_file(const std::filesystem::path& path) {
    // A virus scanner, search indexer, or backup agent can briefly hold the file
    // open, which denies the write access FlushFileBuffers needs. Retry those
    // transient sharing errors before treating the sync as failed.
    constexpr int maximum_attempts = 50;
    for (int attempt = 0; attempt < maximum_attempts; ++attempt) {
        const HANDLE handle =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (!is_transient_sharing_error(error) || attempt + 1 == maximum_attempts) {
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "Could not open file for syncing: " + path.string());
            }
        } else {
            if (FlushFileBuffers(handle) != 0) {
                CloseHandle(handle);
                return;
            }
            const auto error = GetLastError();
            CloseHandle(handle);
            if (!is_transient_sharing_error(error) || attempt + 1 == maximum_attempts) {
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "Could not flush file to disk: " + path.string());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void sync_directory(const std::filesystem::path& path) {
    // NTFS journals directory metadata; a portable directory flush would need
    // backup privileges, so there is nothing to do here.
    static_cast<void>(path);
}

#else

void sync_file(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "Could not open file for syncing: " + path.string());
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw std::system_error(error, std::generic_category(),
                                "Could not flush file to disk: " + path.string());
    }
    ::close(descriptor);
}

void sync_directory(const std::filesystem::path& path) {
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "Could not open directory for syncing: " + path.string());
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw std::system_error(error, std::generic_category(),
                                "Could not flush directory to disk: " + path.string());
    }
    ::close(descriptor);
}

#endif

} // namespace minidb::detail
