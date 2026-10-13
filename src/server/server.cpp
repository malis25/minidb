#include "server.h" // Server startup module.
#include "database_api.h"
#include "database_catalog.h"
#include "request_logging.h"
#include "server_options.h"

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

        DatabaseCatalog catalog{options.database_directory};

        httplib::Server server;
        server.set_payload_max_length(maximum_request_size);
        configure_socket_options(server);

        // These separate objects keep responsibilities isolated:
        // Routes manage the catalog and its databases, while this function owns
        // startup, request logging, and the listening socket.
        DatabaseApi database_api{catalog};
        database_api.register_routes(server);
        configure_request_logging(server, options);

        constexpr auto listen_address = "127.0.0.1";
        if (!server.bind_to_port(listen_address, options.port)) {
            throw std::runtime_error("Port " + std::to_string(options.port) +
                                     " is unavailable on " + listen_address + ".");
        }

        std::cout << "MiniDB HTTP API listening on http://" << listen_address << ':' << options.port
                  << "\nDatabase root: " << options.database_directory.string() << std::endl;
        if (!server.listen_after_bind()) {
            throw std::runtime_error("HTTP server stopped unexpectedly.");
        }
    } catch (const std::exception& error) {
        std::cerr << "minidb: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
