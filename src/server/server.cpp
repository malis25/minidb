#include "server.h" // Server startup module.
#include "database_api.h"
#include "request_logging.h"
#include "server_options.h"

#include "minidb/database.h"
#include "minidb/storage.h"

#include <httplib.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace {

constexpr std::size_t maximum_request_size = 1024 * 1024;

void configure_socket_options(httplib::Server& server) {
    server.set_socket_options([](socket_t socket) {
#ifdef _WIN32
        // Windows SO_REUSEADDR may allow another process to bind an active port.
        // Exclusive use makes binding this listener a reliable availability check.
        const int exclusive = 1;
        if (setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive),
                       sizeof(exclusive)) == SOCKET_ERROR) {
            throw std::system_error(WSAGetLastError(), std::system_category(),
                                    "Could not enable exclusive port binding.");
        }
#else
        static_cast<void>(socket);
#endif
    });
}

} // namespace

int minidb::server::run(int argc, char* argv[]) {
    try {
        const auto parsed_options = parse_server_options(argc, argv);
        if (!parsed_options) {
            return 0;
        }
        const auto& options = *parsed_options;

        // Resolve all storage paths from the explicit CLI directory and filename.
        // Creating the parent directory is convenient, but no default location is used.
        std::filesystem::create_directories(options.database_directory);
        // The manifest describes the segment set and next numeric object ID.
        auto storage = std::make_unique<minidb::SegmentedStorage>(
            options.database_directory, options.database_filename, options.maximum_file_size);
        const auto manifest_file = storage->manifest_path();
        minidb::Database database{std::move(storage)};

        httplib::Server server;
        server.set_payload_max_length(maximum_request_size);
        configure_socket_options(server);

        // These separate objects keep responsibilities isolated:
        // routes operate on Database, logging formats console records, and this
        // function owns startup, storage loading, and the listening socket.
        DatabaseApi database_api{database};
        database_api.register_routes(server);
        configure_request_logging(server, options);

        constexpr auto listen_address = "127.0.0.1";
        if (!server.bind_to_port(listen_address, options.port)) {
            throw std::runtime_error("Port " + std::to_string(options.port) +
                                     " is unavailable on " + listen_address + ".");
        }

        std::cout << "MiniDB HTTP API listening on http://" << listen_address << ':' << options.port
                  << "\nDatabase manifest: " << manifest_file.string()
                  << "\nMaximum segment size: " << options.maximum_file_size << " bytes"
                  << std::endl;
        if (!server.listen_after_bind()) {
            throw std::runtime_error("HTTP server stopped unexpectedly.");
        }
    } catch (const std::exception& error) {
        std::cerr << "minidb: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
