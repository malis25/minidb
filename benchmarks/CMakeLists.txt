add_executable(minidb_bench scenarios/http_benchmark_main.cpp common/benchmark_harness.cpp)
target_link_libraries(minidb_bench
    PRIVATE
        minidb::server
        httplib::httplib
        nlohmann_json::nlohmann_json
)
minidb_set_compiler_warnings(minidb_bench)

add_executable(minidb_write_bench scenarios/reservation_benchmark_main.cpp)
target_link_libraries(minidb_write_bench
    PRIVATE
        minidb::core
        nlohmann_json::nlohmann_json
)
minidb_set_compiler_warnings(minidb_write_bench)

add_executable(minidb_concurrency_bench scenarios/concurrency_benchmark_main.cpp common/benchmark_harness.cpp)
target_link_libraries(minidb_concurrency_bench
    PRIVATE
        minidb::server
        httplib::httplib
        nlohmann_json::nlohmann_json
)
minidb_set_compiler_warnings(minidb_concurrency_bench)
