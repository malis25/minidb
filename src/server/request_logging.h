#pragma once

namespace httplib {
class Server;
}

struct ServerOptions;

// Attach the configured console logger. No logger is installed when logging
// was not explicitly requested on the command line.
void configure_request_logging(httplib::Server& server, const ServerOptions& options);
