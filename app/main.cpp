#include "server.h"
#include "version.h"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    // Keep version reporting independent from server startup. This is useful in scripts
    // that need to inspect the binary without choosing a database location or opening a port.
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "minidb " << minidb::version() << '\n';
        return 0;
    }

    // The primary executable is the database server; options and --help stay
    // beside the server implementation rather than in this entry point.
    return minidb::server::run(argc, argv);
}
