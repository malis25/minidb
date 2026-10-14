#pragma once

namespace httplib {
class Request;
class Response;
} // namespace httplib

// Rejects cross-origin state-changing requests. The server is loopback-only, so
// a mutation is allowed only when its Origin (if present) names localhost or
// 127.0.0.1 and matches the Host header exactly. Returns true and writes a 403
// when the request is rejected.
bool reject_cross_origin_mutation(const httplib::Request& request, httplib::Response& response);
