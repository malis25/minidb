#include "database_api.h"

#include "http_util.h"
#include "origin_policy.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

// Object routes: listing IDs, querying, batched mutation, and single-object
// create/read/replace/delete.
void DatabaseApi::register_object_routes(httplib::Server& server) {
    server.Get(
        "/databases/:name", [this](const httplib::Request& request, httplib::Response& response) {
            const auto name = request.path_params.at("name");
            Page page;
            if (!parse_page(request, page, response)) {
                return;
            }
            const auto database = find_database(catalog_, name, response);
            if (!database) {
                return;
            }
            std::lock_guard lock(database->mutex);
            if (!is_active(database, name, response)) {
                return;
            }
            const auto total = database->database.size();
            if (page.after_id) {
                const auto ids = database->database.ids_after(page.after_id, page.limit);
                send_json(response, cursor_ids(ids, page), httplib::StatusCode::OK_200);
            } else {
                const auto ids = database->database.ids(page.offset, page.limit);
                send_json(response, paged_ids(ids, total, page), httplib::StatusCode::OK_200);
            }
        });

    server.Post("/databases/:name/query", [this](const httplib::Request& request,
                                                 httplib::Response& response) {
        const auto name = request.path_params.at("name");
        Page page;
        std::string field;
        nlohmann::json value;
        if (!parse_page(request, page, response) || !parse_query(request, field, value, response)) {
            return;
        }
        const auto database = find_database(catalog_, name, response);
        if (!database) {
            return;
        }
        std::lock_guard lock(database->mutex);
        if (!is_active(database, name, response)) {
            return;
        }
        if (page.after_id) {
            const auto matches =
                database->database.find_after(field, value, page.after_id, page.limit);
            send_json(response, cursor_matches(matches, page), httplib::StatusCode::OK_200);
        } else {
            const auto matches = database->database.find(field, value, page.offset, page.limit);
            send_json(response, paged_matches(matches, page), httplib::StatusCode::OK_200);
        }
    });

    server.Post("/databases/:name/batch", [this](const httplib::Request& request,
                                                 httplib::Response& response) {
        if (reject_cross_origin_mutation(request, response)) {
            return;
        }
        const auto name = request.path_params.at("name");
        std::vector<minidb::ObjectMutation> mutations;
        if (!parse_batch(request, mutations, response)) {
            return;
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
            const auto created_ids = database->database.apply_batch(mutations);
            send_json(response, {{"applied", mutations.size()}, {"created_ids", created_ids}},
                      httplib::StatusCode::OK_200);
        } catch (const std::invalid_argument& error) {
            send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
        } catch (const std::out_of_range& error) {
            send_error(response, error.what(), httplib::StatusCode::NotFound_404);
        } catch (const std::length_error& error) {
            send_error(response, error.what(), httplib::StatusCode::PayloadTooLarge_413);
        } catch (const std::overflow_error& error) {
            send_error(response, error.what(), httplib::StatusCode::Conflict_409);
        } catch (const std::exception& error) {
            send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
        }
    });

    server.Post(
        "/databases/:name", [this](const httplib::Request& request, httplib::Response& response) {
            if (reject_cross_origin_mutation(request, response)) {
                return;
            }
            const auto name = request.path_params.at("name");
            nlohmann::json object;
            if (!parse_object(request, object, response)) {
                return;
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
                const auto id = database->database.create(std::move(object));
                send_json(response, {{"id", id}, {"object", *database->database.get(id)}},
                          httplib::StatusCode::Created_201);
            } catch (const std::invalid_argument& error) {
                send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
            } catch (const std::exception& error) {
                send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
            }
        });

    server.Get("/databases/:name/:id", [this](const httplib::Request& request,
                                              httplib::Response& response) {
        const auto name = request.path_params.at("name");
        std::uint64_t id = 0;
        if (!parse_id(request.path_params.at("id"), id)) {
            send_error(response, "Object ID must be an unsigned 64-bit integer.",
                       httplib::StatusCode::BadRequest_400);
            return;
        }
        const auto database = find_database(catalog_, name, response);
        if (!database) {
            return;
        }
        std::lock_guard lock(database->mutex);
        if (!is_active(database, name, response)) {
            return;
        }
        const auto object = database->database.get(id);
        if (!object) {
            send_error(response, "Object not found: " + std::to_string(id),
                       httplib::StatusCode::NotFound_404);
            return;
        }
        send_json(response, {{"id", id}, {"object", *object}}, httplib::StatusCode::OK_200);
    });

    server.Put("/databases/:name/:id", [this](const httplib::Request& request,
                                              httplib::Response& response) {
        if (reject_cross_origin_mutation(request, response)) {
            return;
        }
        const auto name = request.path_params.at("name");
        std::uint64_t id = 0;
        if (!parse_id(request.path_params.at("id"), id)) {
            send_error(response, "Object ID must be an unsigned 64-bit integer.",
                       httplib::StatusCode::BadRequest_400);
            return;
        }
        nlohmann::json object;
        if (!parse_object(request, object, response)) {
            return;
        }
        const auto database = find_database(catalog_, name, response);
        if (!database) {
            return;
        }
        std::lock_guard lock(database->mutex);
        if (!is_active(database, name, response)) {
            return;
        }
        if (!database->database.contains(id)) {
            send_error(response, "Object not found: " + std::to_string(id),
                       httplib::StatusCode::NotFound_404);
            return;
        }
        try {
            database->database.set(id, std::move(object));
            send_json(response, {{"id", id}, {"object", *database->database.get(id)}},
                      httplib::StatusCode::OK_200);
        } catch (const std::exception& error) {
            send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
        }
    });

    server.Delete("/databases/:name/:id", [this](const httplib::Request& request,
                                                 httplib::Response& response) {
        if (reject_cross_origin_mutation(request, response)) {
            return;
        }
        const auto name = request.path_params.at("name");
        std::uint64_t id = 0;
        if (!parse_id(request.path_params.at("id"), id)) {
            send_error(response, "Object ID must be an unsigned 64-bit integer.",
                       httplib::StatusCode::BadRequest_400);
            return;
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
            if (!database->database.remove(id)) {
                send_error(response, "Object not found: " + std::to_string(id),
                           httplib::StatusCode::NotFound_404);
                return;
            }
            send_json(response, {{"id", id}, {"deleted", true}}, httplib::StatusCode::OK_200);
        } catch (const std::exception& error) {
            send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
        }
    });
}
