# Keep networking dependencies in the server module rather than the reusable
# key-value database library.
add_library(minidb_server
    server.cpp
    server_options.cpp
    database_catalog.cpp
    database_api.cpp
    api_databases.cpp
    api_schema.cpp
    api_indexes.cpp
    api_objects.cpp
    http_util.cpp
    origin_policy.cpp
    request_logging.cpp
)
add_library(minidb::server ALIAS minidb_server)

target_compile_features(minidb_server PUBLIC cxx_std_20)
target_include_directories(minidb_server
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(minidb_server
    PUBLIC
        minidb::core
    PRIVATE
        httplib::httplib
        nlohmann_json::nlohmann_json
)
minidb_set_compiler_warnings(minidb_server)
