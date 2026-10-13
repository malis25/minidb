#include "database_api.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t default_page_size = 100;
constexpr std::size_t maximum_page_size = 1000;

struct Page {
    std::size_t offset = 0;
    std::size_t limit = default_page_size;
    std::optional<std::uint64_t> after_id;
};

void send_json(httplib::Response& response, const nlohmann::json& body, int status) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& response, const std::string& message, int status) {
    send_json(response, {{"error", message}}, status);
}

bool has_json_content_type(const httplib::Request& request) {
    if (!request.has_header("Content-Type") ||
        request.get_header_value_count("Content-Type") != 1) {
        return false;
    }
    const auto content_type = request.get_header_value("Content-Type");
    const auto parameter_start = content_type.find(';');
    auto media_type = std::string_view{content_type}.substr(0, parameter_start);
    while (!media_type.empty() && (media_type.front() == ' ' || media_type.front() == '\t')) {
        media_type.remove_prefix(1);
    }
    while (!media_type.empty() && (media_type.back() == ' ' || media_type.back() == '\t')) {
        media_type.remove_suffix(1);
    }
    constexpr std::string_view json_type = "application/json";
    if (media_type.size() != json_type.size()) {
        return false;
    }
    for (std::size_t index = 0; index < media_type.size(); ++index) {
        auto character = media_type[index];
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        if (character != json_type[index]) {
            return false;
        }
    }
    return true;
}

bool parse_json_body(const httplib::Request& request, nlohmann::json& body,
                     httplib::Response& response) {
    if (!has_json_content_type(request)) {
        send_error(response, "Content-Type must be application/json.",
                   httplib::StatusCode::UnsupportedMediaType_415);
        return false;
    }
    try {
        body = nlohmann::json::parse(
            request.body, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                if (depth > minidb::maximum_object_nesting_depth) {
                    throw std::length_error("JSON nesting exceeds the maximum depth of 128.");
                }
                return true;
            });
    } catch (const nlohmann::json::parse_error& error) {
        send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
        return false;
    } catch (const std::length_error& error) {
        send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
        return false;
    }
    return true;
}

bool parse_unsigned(std::string_view text, std::size_t& value) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool parse_id(std::string_view text, std::uint64_t& id) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
    return error == std::errc{} && end == text.data() + text.size();
}

bool parse_page(const httplib::Request& request, Page& page, httplib::Response& response) {
    if (request.has_param("after_id")) {
        std::uint64_t after_id = 0;
        if (request.has_param("offset") ||
            !parse_id(request.get_param_value("after_id"), after_id)) {
            send_error(response,
                       "after_id must be an unsigned 64-bit integer and cannot be "
                       "combined with offset.",
                       httplib::StatusCode::BadRequest_400);
            return false;
        }
        page.after_id = after_id;
    }
    if (request.has_param("offset") &&
        !parse_unsigned(request.get_param_value("offset"), page.offset)) {
        send_error(response, "offset must be a non-negative integer.",
                   httplib::StatusCode::BadRequest_400);
        return false;
    }
    if (request.has_param("limit") &&
        !parse_unsigned(request.get_param_value("limit"), page.limit)) {
        send_error(response, "limit must be a positive integer.",
                   httplib::StatusCode::BadRequest_400);
        return false;
    }
    if (page.limit == 0 || page.limit > maximum_page_size) {
        send_error(response, "limit must be between 1 and 1000.",
                   httplib::StatusCode::BadRequest_400);
        return false;
    }
    return true;
}

bool parse_object(const httplib::Request& request, nlohmann::json& object,
                  httplib::Response& response) {
    if (!parse_json_body(request, object, response)) {
        return false;
    }
    if (!object.is_object()) {
        send_error(response, "Request body must be a JSON object.",
                   httplib::StatusCode::BadRequest_400);
        return false;
    }
    return true;
}

bool parse_batch(const httplib::Request& request, std::vector<minidb::ObjectMutation>& mutations,
                 httplib::Response& response) {
    nlohmann::json body;
    if (!parse_json_body(request, body, response)) {
        return false;
    }
    if (!body.is_object() || !body.contains("operations") || !body["operations"].is_array() ||
        body["operations"].empty() || body["operations"].size() > 1000) {
        send_error(response, "Batch requires between 1 and 1000 operations.",
                   httplib::StatusCode::BadRequest_400);
        return false;
    }

    mutations.reserve(body["operations"].size());
    for (const auto& operation : body["operations"]) {
        if (!operation.is_object() || !operation.contains("type") ||
            !operation["type"].is_string()) {
            send_error(response, "Each batch operation requires a string type.",
                       httplib::StatusCode::BadRequest_400);
            return false;
        }
        const auto type = operation["type"].get<std::string>();
        if (type == "create" || type == "replace") {
            if (!operation.contains("object") || !operation["object"].is_object()) {
                send_error(response, "Create and replace operations require an object.",
                           httplib::StatusCode::BadRequest_400);
                return false;
            }
            if (type == "create") {
                mutations.push_back({minidb::ObjectMutation::Type::create, 0, operation["object"]});
                continue;
            }
        } else if (type != "delete") {
            send_error(response, "Batch operation type must be create, replace, or delete.",
                       httplib::StatusCode::BadRequest_400);
            return false;
        }

        if (!operation.contains("id") || !operation["id"].is_number_unsigned()) {
            send_error(response, "Replace and delete operations require an unsigned numeric id.",
                       httplib::StatusCode::BadRequest_400);
            return false;
        }
        const auto id = operation["id"].get<std::uint64_t>();
        if (type == "replace") {
            mutations.push_back({minidb::ObjectMutation::Type::replace, id, operation["object"]});
        } else {
            mutations.push_back({minidb::ObjectMutation::Type::erase, id, {}});
        }
    }
    return true;
}

bool parse_query(const httplib::Request& request, std::string& field, nlohmann::json& value,
                 httplib::Response& response) {
    nlohmann::json query;
    if (!parse_json_body(request, query, response)) {
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

std::shared_ptr<ManagedDatabase> find_database(DatabaseCatalog& catalog, const std::string& name,
                                               httplib::Response& response) {
    if (!DatabaseCatalog::is_valid_name(name)) {
        send_error(response, "Invalid database name.", httplib::StatusCode::BadRequest_400);
        return nullptr;
    }
    auto database = catalog.find_active(name);
    if (!database) {
        send_error(response, "Database not found: " + name, httplib::StatusCode::NotFound_404);
    }
    return database;
}

bool is_active(const std::shared_ptr<ManagedDatabase>& database, const std::string& name,
               httplib::Response& response) {
    if (!database->active) {
        send_error(response, "Database not found: " + name, httplib::StatusCode::NotFound_404);
        return false;
    }
    return true;
}

nlohmann::json paged_ids(const std::vector<std::uint64_t>& ids, std::size_t total,
                         const Page& page) {
    const bool has_more = page.offset < total && ids.size() < total - page.offset;
    return {{"ids", ids},
            {"count", ids.size()},
            {"total", total},
            {"limit", page.limit},
            {"offset", page.offset},
            {"has_more", has_more},
            {"next_after_id",
             has_more && !ids.empty() ? nlohmann::json(ids.back()) : nlohmann::json(nullptr)}};
}

nlohmann::json cursor_ids(const minidb::IdPage& id_page, const Page& page) {
    const auto next_after_id = id_page.has_more && !id_page.ids.empty()
                                   ? nlohmann::json(id_page.ids.back())
                                   : nlohmann::json(nullptr);
    return {{"ids", id_page.ids},
            {"count", id_page.ids.size()},
            {"total", id_page.total},
            {"limit", page.limit},
            {"after_id", page.after_id ? nlohmann::json(*page.after_id) : nlohmann::json(nullptr)},
            {"next_after_id", next_after_id},
            {"has_more", id_page.has_more}};
}

nlohmann::json paged_matches(const minidb::ObjectMatchPage& page_matches, const Page& page) {
    auto results = nlohmann::json::array();
    for (const auto& match : page_matches.matches) {
        results.push_back({{"id", match.id}, {"object", *match.object}});
    }
    const bool has_more = page.offset < page_matches.total &&
                          page_matches.matches.size() < page_matches.total - page.offset;
    return {{"results", std::move(results)},
            {"count", page_matches.matches.size()},
            {"total", page_matches.total},
            {"limit", page.limit},
            {"offset", page.offset},
            {"has_more", has_more},
            {"next_after_id", has_more && !page_matches.matches.empty()
                                  ? nlohmann::json(page_matches.matches.back().id)
                                  : nlohmann::json(nullptr)}};
}

nlohmann::json cursor_matches(const minidb::ObjectMatchPage& page_matches, const Page& page) {
    auto response = paged_matches(page_matches, page);
    response["after_id"] = page.after_id ? nlohmann::json(*page.after_id) : nlohmann::json(nullptr);
    response["next_after_id"] = page_matches.has_more && !page_matches.matches.empty()
                                    ? nlohmann::json(page_matches.matches.back().id)
                                    : nlohmann::json(nullptr);
    response["has_more"] = page_matches.has_more;
    return response;
}

bool parse_purge(const httplib::Request& request, bool& purge, httplib::Response& response) {
    if (!request.has_param("purge")) {
        return true;
    }
    const auto value = request.get_param_value("purge");
    if (value == "true") {
        purge = true;
        return true;
    }
    if (value == "false") {
        purge = false;
        return true;
    }
    send_error(response, "purge must be either true or false.",
               httplib::StatusCode::BadRequest_400);
    return false;
}

bool equals_ascii_case_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto left_character = left[index];
        auto right_character = right[index];
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character = static_cast<char>(left_character - 'A' + 'a');
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character = static_cast<char>(right_character - 'A' + 'a');
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

bool is_loopback_authority(std::string_view authority) {
    const auto port_separator = authority.find(':');
    const auto hostname = authority.substr(0, port_separator);
    if (!equals_ascii_case_insensitive(hostname, "localhost") &&
        !equals_ascii_case_insensitive(hostname, "127.0.0.1")) {
        return false;
    }
    if (port_separator == std::string_view::npos) {
        return true;
    }
    const auto port = authority.substr(port_separator + 1);
    return !port.empty() && std::all_of(port.begin(), port.end(), [](char character) {
        return character >= '0' && character <= '9';
    });
}

bool is_allowed_mutation_origin(const httplib::Request& request) {
    if (!request.has_header("Origin")) {
        return true;
    }
    if (request.get_header_value_count("Origin") != 1 ||
        request.get_header_value_count("Host") != 1) {
        return false;
    }
    const auto origin_value = request.get_header_value("Origin");
    const std::string_view origin = origin_value;
    constexpr std::string_view http_origin_prefix = "http://";
    if (origin.size() <= http_origin_prefix.size() ||
        !equals_ascii_case_insensitive(origin.substr(0, http_origin_prefix.size()),
                                       http_origin_prefix)) {
        return false;
    }
    const auto authority = origin.substr(http_origin_prefix.size());
    const auto host_value = request.get_header_value("Host");
    const std::string_view host = host_value;
    return is_loopback_authority(authority) && equals_ascii_case_insensitive(authority, host);
}

bool reject_cross_origin_mutation(const httplib::Request& request, httplib::Response& response) {
    if (is_allowed_mutation_origin(request)) {
        return false;
    }
    send_error(response, "Cross-origin state-changing requests are not allowed.",
               httplib::StatusCode::Forbidden_403);
    return true;
}

} // namespace

DatabaseApi::DatabaseApi(DatabaseCatalog& catalog) : catalog_(catalog) {}

void DatabaseApi::register_routes(httplib::Server& server) {
    server.Get("/health", [](const httplib::Request&, httplib::Response& response) {
        send_json(response, {{"status", "ok"}}, httplib::StatusCode::OK_200);
    });

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
            if (!catalog_.create(name, schema, maximum_objects_per_segment)) {
                send_error(response, "Database already exists: " + name,
                           httplib::StatusCode::Conflict_409);
                return;
            }
            send_json(response, {{"name", name}, {"created", true}},
                      httplib::StatusCode::Created_201);
        } catch (const std::invalid_argument& error) {
            send_error(response, error.what(), httplib::StatusCode::BadRequest_400);
        } catch (const std::exception& error) {
            send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
        }
    });

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
             {"maximum_objects_per_segment", database->database.maximum_objects_per_segment()}},
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
        std::lock_guard lock(database->mutex);
        if (!is_active(database, name, response)) {
            return;
        }
        try {
            database->database.update_schema(std::move(schema));
            send_json(response,
                      {{"name", name}, {"schema", database->database.schema()}, {"updated", true}},
                      httplib::StatusCode::OK_200);
        } catch (const std::invalid_argument& error) {
            send_error(response, error.what(), httplib::StatusCode::Conflict_409);
        } catch (const std::exception& error) {
            send_error(response, error.what(), httplib::StatusCode::InternalServerError_500);
        }
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
                              {"next_id", database->database.next_id()}},
                             httplib::StatusCode::OK_200);
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
