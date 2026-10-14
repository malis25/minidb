#include "database_api.h"

#include "http_util.h"
#include "origin_policy.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <utility>

// Schema routes: reading and replacing the schema, and database info. Replacing
// a schema starts a background migration and reports its progress.
void DatabaseApi::register_schema_routes(httplib::Server& server) {
    server.Get("/databases/:name/schema", [this](const httplib::Request& request,
                                                 httplib::Response& response) {
        const auto name = request.path_params.at("name");
        const auto database = find_database(catalog_, name, response);
        if (!database) {
            return;
        }
        std::lock_guard lock(database->mutex);
        if (!is_active(database, name, response)) {
            return;
        }
        send_json(
            response,
            {{"name", name},
             {"schema", database->database.schema()},
             {"maximum_objects_per_segment", database->database.maximum_objects_per_segment()},
             {"migration", migration_status_json(database->database.schema_migration_status())}},
            httplib::StatusCode::OK_200);
    });

    server.Put("/databases/:name/schema", [this](const httplib::Request& request,
                                                 httplib::Response& response) {
        if (reject_cross_origin_mutation(request, response)) {
            return;
        }
        const auto name = request.path_params.at("name");
        nlohmann::json schema;
        if (!parse_json_body(request, schema, response)) {
            return;
        }
        std::string schema_error;
        if (!schema.is_object() || !minidb::validate_object_schema(schema, &schema_error) ||
            !minidb::is_strict_object_schema(schema)) {
            send_error(response,
                       schema_error.empty() ? "Schema must reject additional properties."
                                            : schema_error,
                       httplib::StatusCode::BadRequest_400);
            return;
        }
        const auto database = find_database(catalog_, name, response);
        if (!database) {
            return;
        }
        bool migration_pending = false;
        nlohmann::json updated_schema;
        nlohmann::json migration;
        {
            std::lock_guard lock(database->mutex);
            if (!is_active(database, name, response)) {
                return;
            }
            try {
                database->database.begin_schema_migration(std::move(schema));
                // Adapt the first chunk inline so small databases finish before
                // the response is sent and the caller sees a completed update.
                static_cast<void>(database->database.migrate_schema_chunk(128));
                const auto status = database->database.schema_migration_status();
                migration_pending = status.pending;
                updated_schema = database->database.schema();
                migration = migration_status_json(status);
            } catch (const std::invalid_argument& error) {
                send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
                return;
            } catch (const std::logic_error& error) {
                send_error(response, error.what(), httplib::StatusCode::Conflict_409);
                return;
            } catch (const std::exception& error) {
                send_error(response, error.what(),
                           httplib::StatusCode::InternalServerError_500);
                return;
            }
            // Start the worker while still holding `mutex`; it only takes that
            // same lock between chunks, and a running worker is always finished
            // when a new migration begins, so this cannot deadlock.
            if (migration_pending) {
                database->start_schema_migration();
            }
        }
        send_json(response,
                  {{"name", name},
                   {"schema", updated_schema},
                   {"updated", true},
                   {"migration", migration}},
                  migration_pending ? httplib::StatusCode::Accepted_202
                                    : httplib::StatusCode::OK_200);
    });

    server.Get("/databases/:name/info",
               [this](const httplib::Request& request, httplib::Response& response) {
                   const auto name = request.path_params.at("name");
                   const auto database = find_database(catalog_, name, response);
                   if (!database) {
                       return;
                   }
                   std::lock_guard lock(database->mutex);
                   if (!is_active(database, name, response)) {
                       return;
                   }
                   send_json(response,
                             {{"name", name},
                              {"object_count", database->database.size()},
                              {"next_id", database->database.next_id()},
                              {"disk_backed", database->database.disk_backed()},
                              {"indexed_fields", database->database.indexed_fields()},
                              {"migration",
                               migration_status_json(database->database.schema_migration_status())}},
                             httplib::StatusCode::OK_200);
               });
}
