#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct ServerOptions {
    int port = 8080;
    std::filesystem::path database_directory;
    std::uint64_t maximum_file_size = 0;
    bool log_requests = false;
    bool log_details = false;
};

// Return no value after printing help; invalid or incomplete options throw
// std::runtime_error so the executable can report them consistently.
std::optional<ServerOptions> parse_server_options(int argc, char* argv[]);
