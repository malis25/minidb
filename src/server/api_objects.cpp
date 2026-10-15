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

namespace {

const char* const allowed_ops[] = {"eq", "ne", "neq", "gt", "gte", "lt", "lte",
                                     "contains", "starts_with", "ends_with",
                                     "exists", "is_null", "in"};

bool op_allowed(const std::string& op) {
    for (const auto* candidate : allowed_ops) {
        if (op == candidate) return true;
    }
    return false;
}

bool parse_filter_node(const nlohmann::json& json, minidb::FilterNode& node,
                       httplib::Response& response) {
    if (!json.is_object()) {
        send_error(response, "Query filter must be a JSON object.",
                   httplib::StatusCode::BadRequest_400);
        return false;
    }
    if (json.contains("and") || json.contains("or")) {
        node.composite = true;
        node.combine = json.contains("and") ? std::string("and") : std::string("or");
        const auto& children = json.contains("and") ? json["and"] : json["or"];
        if (!children.is_array() || children.empty()) {
            send_error(response, "Query combinator must be a non-empty array.",
                       httplib::StatusCode::BadRequest_400);
            return false;
        }
        for (const auto& child : children) {
            minidb::FilterNode child_node;
            if (!parse_filter_node(child, child_node, response)) return false;
            node.children.push_back(std::move(child_node));
        }
        return true;
    }

    node.composite = false;
    if (!json.contains("field") || !json["field"].is_string() ||
        json["field"].get_ref<const std::string&>().empty()) {
        send_error(response, "Query requires a non-empty string field.",
                   httplib::StatusCode::BadRequest_400);
        return false;
    }
    node.field = json["field"].get<std::string>();
    node.op = "eq";
    if (json.contains("op")) {
        if (!json["op"].is_string()) {
            send_error(response, "Query operator must be a string.",
                       httplib::StatusCode::BadRequest_400);
            return false;
        }
        node.op = json["op"].get<std::string>();
    }
    if (!op_allowed(node.op)) {
        send_error(response, "Unsupported query operator: " + node.op,
                   httplib::StatusCode::BadRequest_400);
        return false;
    }
    if (json.contains("value")) {
        node.value = json["value"];
    } else if (json.contains("equals")) {
        node.value = json["equals"];
    } else if (node.op != "exists" && node.op != "is_null") {
        send_error(response, "Query requires a value (value or equals).",
                   httplib::StatusCode::BadRequest_400);
        return false;
    } else {
        node.value = nullptr;
    }
    return true;
}

} // namespace

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
        if (!parse_page(request, page, response)) {
            return;
        }
        nlohmann::json body;
        if (!parse_json_body(request, body, response)) {
            return;
        }
        minidb::FilterNode filter;
        if (body.contains("filter")) {
            if (!parse_filter_node(body["filter"], filter, response)) return;
        } else if (body.contains("and") || body.contains("or") || body.contains("field")) {
            if (!parse_filter_node(body, filter, response)) return;
        } else {
            // Backwards-compatible shortcut: {field, equals} still works.
            std::string field;
            nlohmann::json value;
            if (!parse_query(request, field, value, response)) return;
            filter.composite = false;
            filter.field = std::move(field);
            filter.op = "eq";
            filter.value = std::move(value);
        }

        const auto database = find_database(catalog_, name, response);
        if (!database) {
            return;
        }
        std::lock_guard lock(database->mutex);
        if (!is_active(database, name, response)) {
            return;
        }
        minidb::ObjectMatchPage matches;
        if (page.after_id) {
            matches = database->database.query(filter, page.after_id, page.limit);
            send_json(response, cursor_matches(matches, page), httplib::StatusCode::OK_200);
        } else {
            matches = database->database.query(filter, page.offset, page.limit);
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
