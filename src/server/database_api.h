#pragma once

#include "minidb/database.h"

#include <mutex>

namespace httplib {
class Server;
}

// Connect HTTP routes to the object database. The API serializes route access;
// persistence remains the responsibility of Database's storage backend.
class DatabaseApi {
  public:
    explicit DatabaseApi(minidb::Database& database);

    void register_routes(httplib::Server& server);

  private:
    minidb::Database& database_;
    std::mutex database_mutex_;
};
