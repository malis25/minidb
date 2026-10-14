#pragma once

#include "database_catalog.h"

namespace httplib {
class Server;
}

// HTTP routes use a catalog for database lifecycle and a per-database lock for
// object operations, allowing independent databases to serve requests in
// parallel. Route registration is split by resource so each file stays focused.
class DatabaseApi {
  public:
    explicit DatabaseApi(DatabaseCatalog& catalog);

    void register_routes(httplib::Server& server);

  private:
    void register_database_routes(httplib::Server& server);
    void register_schema_routes(httplib::Server& server);
    void register_index_routes(httplib::Server& server);
    void register_object_routes(httplib::Server& server);

    DatabaseCatalog& catalog_;
};
