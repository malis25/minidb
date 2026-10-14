#include "origin_policy.h"

#include "http_util.h"

#include <httplib.h>

#include <algorithm>
#include <string_view>

namespace {

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

} // namespace

bool reject_cross_origin_mutation(const httplib::Request& request, httplib::Response& response) {
    if (is_allowed_mutation_origin(request)) {
        return false;
    }
    send_error(response, "Cross-origin state-changing requests are not allowed.",
               httplib::StatusCode::Forbidden_403);
    return true;
}
