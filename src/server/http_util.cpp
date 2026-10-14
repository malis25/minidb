#include "http_util.h"

#include <httplib.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

constexpr std::size_t default_page_size = 100;
constexpr std::size_t maximum_page_size = 1000;

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

} // namespace

bool parse_id(std::string_view text, std::uint64_t& id) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
    return error == std::errc{} && end == text.data() + text.size();
}

void send_json(httplib::Response& response, const nlohmann::json& body, int status) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& response, const std::string& message, int status) {
    send_json(response, {{"error", message}}, status);
}

nlohmann::json migration_status_json(const minidb::Database::SchemaMigrationStatus& status) {
    return {{"pending", status.pending},
            {"total", status.total},
            {"processed", status.processed},
            {"remaining", status.total - status.processed}};
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
