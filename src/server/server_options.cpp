#include "server_options.h" // Command-line configuration for the HTTP service.

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace {

int parse_port(std::string_view value) {
    int port = 0;
    // from_chars is locale-independent and checking its end pointer rejects
    // values with a valid numeric prefix followed by unrelated characters.
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
    if (error != std::errc{} || end != value.data() + value.size() || port < 1 || port > 65535) {
        throw std::runtime_error("Port must be a number between 1 and 65535.");
    }
    return port;
}

void print_help() {
    std::cout << "Usage: minidb [--port PORT] --db-dir DIRECTORY "
                 "[--log-requests | --log-details]\n"
                 "  --port PORT       Port to listen on (default: 8080)\n"
                 "  --db-dir DIRECTORY Root directory for all databases\n"
                 "  --log-requests    Log request method, path, and response status\n"
                 "  --log-details     Also log request and response bodies\n"
                 "HTTP API listens on 127.0.0.1 only.\n";
}

} // namespace

std::optional<ServerOptions> parse_server_options(int argc, char* argv[]) {
    ServerOptions options;

    // argv[0] is the executable name. Each option that takes a value consumes
    // the immediately following argument, keeping the command line predictable.
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            print_help();
            return std::nullopt;
        }
        if (argument == "--port" && index + 1 < argc) {
            options.port = parse_port(argv[++index]);
        } else if (argument == "--db-dir" && index + 1 < argc) {
            options.database_directory = argv[++index];
        } else if (argument == "--log-requests") {
            options.log_requests = true;
        } else if (argument == "--log-details") {
            options.log_requests = true;
            options.log_details = true;
        } else {
            throw std::runtime_error("Unknown or incomplete argument: " + std::string(argument));
        }
    }

    if (options.database_directory.empty()) {
        throw std::runtime_error("Required argument missing: --db-dir DIRECTORY.");
    }
    return options;
}
