#include "database_api.h"

#include "http_util.h"

#include <httplib.h>

namespace {

void register_health_route(httplib::Server& server) {
    server.Get("/health", [](const httplib::Request&, httplib::Response& response) {
        send_json(response, {{"status", "ok"}}, httplib::StatusCode::OK_200);
    });
}

} // namespace

DatabaseApi::DatabaseApi(DatabaseCatalog& catalog) : catalog_(catalog) {}

void DatabaseApi::register_routes(httplib::Server& server) {
    // Registration order matters: cpp-httplib returns the first route whose
    // pattern matches, and literal sub-paths like `/schema` must be registered
    // before the catch-all `/databases/:name/:id` object route.
    register_health_route(server);
    register_database_routes(server);
    register_schema_routes(server);
    register_index_routes(server);
    register_object_routes(server);
}
