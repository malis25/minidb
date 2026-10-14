#pragma once

#include "database_catalog.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace httplib {
class Request;
class Response;
} // namespace httplib

// Pagination parameters shared by the list and query endpoints. `offset` and
// `after_id` are mutually exclusive; `after_id` selects the cursor form.
struct Page {
    std::size_t offset = 0;
    std::size_t limit = 100;
    std::optional<std::uint64_t> after_id;
};

// Response helpers.
void send_json(httplib::Response& response, const nlohmann::json& body, int status);
void send_error(httplib::Response& response, const std::string& message, int status);
nlohmann::json migration_status_json(const minidb::Database::SchemaMigrationStatus& status);

// Request parsing helpers. Each returns false after writing an error response.
bool parse_json_body(const httplib::Request& request, nlohmann::json& body,
                     httplib::Response& response);
bool parse_page(const httplib::Request& request, Page& page, httplib::Response& response);
bool parse_object(const httplib::Request& request, nlohmann::json& object,
                  httplib::Response& response);
bool parse_batch(const httplib::Request& request, std::vector<minidb::ObjectMutation>& mutations,
                 httplib::Response& response);
bool parse_query(const httplib::Request& request, std::string& field, nlohmann::json& value,
                 httplib::Response& response);
bool parse_purge(const httplib::Request& request, bool& purge, httplib::Response& response);
bool parse_id(std::string_view text, std::uint64_t& id);

// Catalog lookup shared by every per-database route; writes a 404 when missing.
std::shared_ptr<ManagedDatabase> find_database(DatabaseCatalog& catalog, const std::string& name,
                                               httplib::Response& response);
bool is_active(const std::shared_ptr<ManagedDatabase>& database, const std::string& name,
               httplib::Response& response);

// Paged response bodies.
nlohmann::json paged_ids(const std::vector<std::uint64_t>& ids, std::size_t total, const Page& page);
nlohmann::json cursor_ids(const minidb::IdPage& id_page, const Page& page);
nlohmann::json paged_matches(const minidb::ObjectMatchPage& page_matches, const Page& page);
nlohmann::json cursor_matches(const minidb::ObjectMatchPage& page_matches, const Page& page);
