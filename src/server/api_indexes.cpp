#include "database_api.h"

#include "http_util.h"
#include "origin_policy.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

// Index routes: reporting which fields are indexed and replacing that set.
void DatabaseApi::register_index_routes(httplib::Server& server) {
    server.Get("/databases/:name/indexes", [this](const httplib::Request& request,
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
        send_json(response,
                  {{"name", name}, {"indexed_fields", database->database.indexed_fields()}},
                  httplib::StatusCode::OK_200);
    });

    server.Put("/databases/:name/indexes", [this](const httplib::Request& request,
                                                  httplib::Response& response) {
        if (reject_cross_origin_mutation(request, response)) {
            return;
        }
        const auto name = request.path_params.at("name");
        nlohmann::json body;
        if (!parse_json_body(request, body, response)) {
            return;
        }
        if (!body.is_object() || !body.contains("indexed_fields") ||
            !body["indexed_fields"].is_array()) {
            send_error(response, "Request body requires an indexed_fields array.",
                       httplib::StatusCode::BadRequest_400);
            return;
        }
        std::vector<std::string> desired;
        for (const auto& field : body["indexed_fields"]) {
            if (!field.is_string() || field.get_ref<const std::string&>().empty()) {
                send_error(response, "indexed_fields must contain non-empty strings.",
                           httplib::StatusCode::BadRequest_400);
                return;
            }
            desired.push_back(field.get<std::string>());
        }
        const auto database = find_database(catalog_, name, response);
        if (!database) {
            return;
        }
        std::lock_guard lock(database->mutex);
        if (!is_active(database, name, response)) {
            return;
        }
        try {
            const auto current = database->database.indexed_fields();
            for (const auto& field : current) {
                if (std::find(desired.begin(), desired.end(), field) == desired.end()) {
                    database->database.drop_index(field);
                }
            }
            for (const auto& field : desired) {
                if (std::find(current.begin(), current.end(), field) == current.end()) {
                    database->database.create_index(field);
                }
            }
            send_json(response,
                      {{"name", name},
                       {"indexed_fields", database->database.indexed_fields()},
                       {"updated", true}},
                      httplib::StatusCode::OK_200);
        } catch (const std::invalid_argument& error) {
            send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
        } catch (const std::exception& error) {
            send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
        }
    });
}
