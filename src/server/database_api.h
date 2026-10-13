#pragma once

#include "database_catalog.h"

namespace httplib {
class Server;
}

// HTTP routes use a catalog for database lifecycle and a per-database lock for
// object operations, allowing independent databases to serve requests in parallel.
class DatabaseApi {
  public:
    explicit DatabaseApi(DatabaseCatalog& catalog);

    void register_routes(httplib::Server& server);

  private:
    DatabaseCatalog& catalog_;
};
