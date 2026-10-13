#include "database_api.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <charconv>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

void send_json(httplib::Response& response, const nlohmann::json& body, int status) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& response, const std::string& message, int status) {
    send_json(response, {{"error", message}}, status);
}

bool parse_id(std::string_view text, std::uint64_t& id) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
    return error == std::errc{} && end == text.data() + text.size();
}

bool parse_object(const httplib::Request& request, nlohmann::json& object,
                  httplib::Response& response) {
    try {
        object = nlohmann::json::parse(request.body);
    } catch (const nlohmann::json::parse_error& error) {
        send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
        return false;
    }
    if (!object.is_object()) {
        send_error(response, "Request body must be a JSON object.",
                   httplib::StatusCode::BadRequest_400);
        return false;
    }
    return true;
}

bool parse_query(const httplib::Request& request, std::string& field, nlohmann::json& value,
                 httplib::Response& response) {
    nlohmann::json query;
    try {
        query = nlohmann::json::parse(request.body);
    } catch (const nlohmann::json::parse_error& error) {
        send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
        return false;
    }
    if (!query.is_object() || !query.contains("field") || !query["field"].is_string() ||
        query["field"].get_ref<const std::string&>().empty() || !query.contains("equals")) {
        send_error(response, "Query requires a non-empty string field and an equals value.",
                   httplib::StatusCode::BadRequest_400);
        return false;
    }
    field = query["field"].get<std::string>();
    value = std::move(query["equals"]);
    return true;
}

nlohmann::json format_matches(const std::vector<minidb::ObjectMatch>& matches) {
    auto results = nlohmann::json::array();
    for (const auto& match : matches) {
        results.push_back({{"id", match.id}, {"object", *match.object}});
    }
    return {{"count", matches.size()}, {"results", std::move(results)}};
}

} // namespace

DatabaseApi::DatabaseApi(minidb::Database& database) : database_(database) {}

void DatabaseApi::register_routes(httplib::Server& server) {
    server.Get("/health", [](const httplib::Request&, httplib::Response& response) {
        send_json(response, {{"status", "ok"}}, httplib::StatusCode::OK_200);
    });

    server.Get("/objects", [this](const httplib::Request&, httplib::Response& response) {
        std::lock_guard lock(database_mutex_);
        const auto ids = database_.ids();
        send_json(response, {{"ids", ids}, {"count", ids.size()}}, httplib::StatusCode::OK_200);
    });

    server.Post("/objects/query",
                [this](const httplib::Request& request, httplib::Response& response) {
                    std::string field;
                    nlohmann::json value;
                    if (!parse_query(request, field, value, response)) {
                        return;
                    }
                    std::lock_guard lock(database_mutex_);
                    send_json(response, format_matches(database_.find(field, value)),
                              httplib::StatusCode::OK_200);
                });

    server.Post("/objects", [this](const httplib::Request& request, httplib::Response& response) {
        nlohmann::json object;
        if (!parse_object(request, object, response)) {
            return;
        }
        std::lock_guard lock(database_mutex_);
        try {
            const auto id = database_.create(std::move(object));
            send_json(response, {{"id", id}, {"object", *database_.get(id)}},
                      httplib::StatusCode::Created_201);
        } catch (const std::exception& error) {
            send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
        }
    });

    server.Get(
        "/objects/:id", [this](const httplib::Request& request, httplib::Response& response) {
            std::uint64_t id = 0;
            if (!parse_id(request.path_params.at("id"), id)) {
                send_error(response, "Object ID must be an unsigned 64-bit integer.",
                           httplib::StatusCode::BadRequest_400);
                return;
            }
            std::lock_guard lock(database_mutex_);
            const auto object = database_.get(id);
            if (!object) {
                send_error(response, "Object not found: " + std::to_string(id),
                           httplib::StatusCode::NotFound_404);
                return;
            }
            send_json(response, {{"id", id}, {"object", *object}}, httplib::StatusCode::OK_200);
        });

    server.Put(
        "/objects/:id", [this](const httplib::Request& request, httplib::Response& response) {
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

            std::lock_guard lock(database_mutex_);
            if (!database_.contains(id)) {
                send_error(response, "Object not found: " + std::to_string(id),
                           httplib::StatusCode::NotFound_404);
                return;
            }
            try {
                database_.set(id, std::move(object));
                send_json(response, {{"id", id}, {"object", *database_.get(id)}},
                          httplib::StatusCode::OK_200);
            } catch (const std::exception& error) {
                send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
            }
        });

    server.Delete(
        "/objects/:id", [this](const httplib::Request& request, httplib::Response& response) {
            std::uint64_t id = 0;
            if (!parse_id(request.path_params.at("id"), id)) {
                send_error(response, "Object ID must be an unsigned 64-bit integer.",
                           httplib::StatusCode::BadRequest_400);
                return;
            }
            std::lock_guard lock(database_mutex_);
            try {
                if (!database_.remove(id)) {
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
