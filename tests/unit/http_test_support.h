#pragma once

#include "database_api.h"
#include "test_support.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

inline const nlohmann::json test_database_schema = {
    {"type", "object"},
    {"properties",
     {{"name", {{"type", "string"}}},
      {"username", {{"type", "string"}}},
      {"email", {{"type", "string"}}},
      {"text", {{"type", "string"}}},
      {"group", {{"type", "string"}}},
      {"room", {{"type", "string"}}},
      {"active", {{"type", "boolean"}}},
      {"friends", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
      {"value", {{"type", "object"}, {"properties", nlohmann::json::object()}}}}},
    {"required", nlohmann::json::array()}};

inline nlohmann::json database_creation(std::string name, std::uint64_t limit = 10000) {
    return {{"name", std::move(name)},
            {"schema", test_database_schema},
            {"maximum_objects_per_segment", limit}};
}

// Starts the real HTTP server in-process on a loopback port and exposes a client
// connected to it. Shared by every HTTP test translation unit.
class DatabaseApiTests : public ::testing::Test {
  protected:
    DatabaseApiTests() : catalog_(temporary_directory_.path()), api_(catalog_) {
        if (!catalog_.create("users", test_database_schema, 10000)) {
            throw std::runtime_error("Could not create the initial HTTP test database.");
        }
    }

    void SetUp() override {
        api_.register_routes(server_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        ASSERT_GT(port_, 0);
        server_thread_ = std::thread([this] { server_.listen_after_bind(); });

        client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
        client_->set_connection_timeout(0, 500000);
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (client_->Get("/health")) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        FAIL() << "HTTP test server did not become ready.";
    }

    void TearDown() override {
        server_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    test_support::TemporaryDirectory temporary_directory_{"minidb-http-tests-"};
    DatabaseCatalog catalog_;
    DatabaseApi api_;
    httplib::Server server_;
    int port_ = 0;
    std::thread server_thread_;
    std::unique_ptr<httplib::Client> client_;
};
