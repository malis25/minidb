#include "database_api.h"

#include "http_util.h"
#include "origin_policy.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <vector>

// Database lifecycle routes: listing, creation, deletion, and restore.
void DatabaseApi::register_database_routes(httplib::Server& server) {
    server.Get("/databases", [this](const httplib::Request& request, httplib::Response& response) {
        const auto state = request.has_param("state") ? request.get_param_value("state") : "active";
        if (state != "active" && state != "archived") {
            send_error(response, "state must be either active or archived.",
                       httplib::StatusCode::BadRequest_400);
            return;
        }
        const auto names =
            state == "archived" ? catalog_.archived_names() : catalog_.active_names();
        send_json(response, {{"databases", names}, {"count", names.size()}},
                  httplib::StatusCode::OK_200);
    });

    server.Post("/databases", [this](const httplib::Request& request, httplib::Response& response) {
        if (reject_cross_origin_mutation(request, response)) {
            return;
        }
        nlohmann::json body;
        if (!parse_object(request, body, response)) {
            return;
        }
        if (!body.contains("name") || !body["name"].is_string()) {
            send_error(response, "Request body requires a string name.",
                       httplib::StatusCode::BadRequest_400);
            return;
        }
        if (!body.contains("schema") || !body["schema"].is_object() ||
            !body.contains("maximum_objects_per_segment") ||
            !body["maximum_objects_per_segment"].is_number_unsigned() ||
            body["maximum_objects_per_segment"] == 0) {
            send_error(response,
                       "Request body requires schema and a positive "
                       "maximum_objects_per_segment.",
                       httplib::StatusCode::BadRequest_400);
            return;
        }
        const auto name = body["name"].get<std::string>();
        if (!DatabaseCatalog::is_valid_name(name)) {
            send_error(response, "Invalid database name.", httplib::StatusCode::BadRequest_400);
            return;
        }
        std::vector<std::string> indexed_fields;
        if (body.contains("indexed_fields")) {
            if (!body["indexed_fields"].is_array()) {
                send_error(response, "indexed_fields must be an array of field names.",
                           httplib::StatusCode::BadRequest_400);
                return;
            }
            for (const auto& field : body["indexed_fields"]) {
                if (!field.is_string() || field.get_ref<const std::string&>().empty()) {
                    send_error(response, "indexed_fields must contain non-empty strings.",
                               httplib::StatusCode::BadRequest_400);
                    return;
                }
                indexed_fields.push_back(field.get<std::string>());
            }
        }
        std::uint64_t cache_bytes = catalog_.default_cache_bytes();
        if (body.contains("cache_bytes")) {
            if (!body["cache_bytes"].is_number_unsigned()) {
                send_error(response, "cache_bytes must be a non-negative integer.",
                           httplib::StatusCode::BadRequest_400);
                return;
            }
            cache_bytes = body["cache_bytes"].get<std::uint64_t>();
        }
        try {
            const auto schema = body["schema"];
            std::string schema_error;
            if (!minidb::validate_object_schema(schema, &schema_error) ||
                !minidb::is_strict_object_schema(schema)) {
                send_error(response,
                           schema_error.empty() ? "Schema must reject additional properties."
                                                : schema_error,
                           httplib::StatusCode::BadRequest_400);
                return;
            }
            const auto maximum_objects_per_segment =
                body["maximum_objects_per_segment"].get<std::uint64_t>();
            if (!catalog_.create(name, schema, maximum_objects_per_segment, indexed_fields,
                                 cache_bytes)) {
                send_error(response, "Database already exists: " + name,
                           httplib::StatusCode::Conflict_409);
                return;
            }
            send_json(response,
                      {{"name", name},
                       {"created", true},
                       {"indexed_fields", indexed_fields},
                       {"disk_backed", !indexed_fields.empty() || cache_bytes > 0}},
                      httplib::StatusCode::Created_201);
        } catch (const std::invalid_argument& error) {
            send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
        } catch (const std::exception& error) {
            send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
        }
    });

    server.Delete(
        "/databases/:name", [this](const httplib::Request& request, httplib::Response& response) {
            if (reject_cross_origin_mutation(request, response)) {
                return;
            }
            const auto name = request.path_params.at("name");
            if (!DatabaseCatalog::is_valid_name(name)) {
                send_error(response, "Invalid database name.", httplib::StatusCode::BadRequest_400);
                return;
            }
            bool purge = false;
            if (!parse_purge(request, purge, response)) {
                return;
            }
            try {
                switch (catalog_.remove(name, purge)) {
                case DatabaseCatalog::RemoveResult::removed:
                    send_json(response, {{"name", name}, {"deleted", true}, {"purged", purge}},
                              httplib::StatusCode::OK_200);
                    return;
                case DatabaseCatalog::RemoveResult::not_found:
                    send_error(response, "Database not found: " + name,
                               httplib::StatusCode::NotFound_404);
                    return;
                case DatabaseCatalog::RemoveResult::archive_exists:
                    send_error(response, "An archived database already exists: " + name,
                               httplib::StatusCode::Conflict_409);
                    return;
                }
            } catch (const std::exception& error) {
                send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
            }
        });

    server.Post("/databases/:name/restore", [this](const httplib::Request& request,
                                                   httplib::Response& response) {
        if (reject_cross_origin_mutation(request, response)) {
            return;
        }
        static_cast<void>(request);
        const auto name = request.path_params.at("name");
        if (!DatabaseCatalog::is_valid_name(name)) {
            send_error(response, "Invalid database name.", httplib::StatusCode::BadRequest_400);
            return;
        }
        try {
            switch (catalog_.restore(name)) {
            case DatabaseCatalog::RestoreResult::restored:
                send_json(response, {{"name", name}, {"restored", true}},
                          httplib::StatusCode::OK_200);
                return;
            case DatabaseCatalog::RestoreResult::not_found:
                send_error(response, "Archived database not found: " + name,
                           httplib::StatusCode::NotFound_404);
                return;
            case DatabaseCatalog::RestoreResult::already_active:
                send_error(response, "Database is already active: " + name,
                           httplib::StatusCode::Conflict_409);
                return;
            }
        } catch (const std::exception& error) {
            send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
        }
    });
}
