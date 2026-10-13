#include "request_logging.h" // Optional request logging configuration.

#include "server_options.h"

#include <httplib.h>

#include <iostream>
#include <syncstream>

void configure_request_logging(httplib::Server& server, const ServerOptions& options) {
    if (!options.log_requests) {
        return;
    }

    server.set_logger([log_details = options.log_details](const httplib::Request& request,
                                                          const httplib::Response& response) {
        // Each request is handled on a worker thread. osyncstream keeps the entire
        // multi-line log record together instead of interleaving concurrent output.
        std::osyncstream log{std::cout};
        log << "[HTTP] " << request.method << ' ' << request.path << " -> " << response.status
            << '\n';
        if (log_details && request.path != "/health") {
            log << "  Request body:\n"
                << request.body << "\n  Response body:\n"
                << response.body << '\n';
        }
        // Flush so redirected output and interactive terminals see each request
        // promptly, even while the server remains running.
        log << std::flush;
    });
}
