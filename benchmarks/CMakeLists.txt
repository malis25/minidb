# The benchmark harness links the real server so it can start the HTTP API on an
# ephemeral loopback port in-process and measure it without spawning a second
# executable or hard-coding a port.
add_executable(minidb_bench benchmark_main.cpp)
target_link_libraries(minidb_bench
    PRIVATE
        minidb::server
        httplib::httplib
        nlohmann_json::nlohmann_json
)
minidb_set_compiler_warnings(minidb_bench)

# A core-only write benchmark that sweeps the next-ID reservation block size.
# It links the database library directly so it can vary a constructor parameter
# that the HTTP API does not expose.
add_executable(minidb_write_bench reservation_benchmark_main.cpp)
target_link_libraries(minidb_write_bench
    PRIVATE
        minidb::core
        nlohmann_json::nlohmann_json
)
minidb_set_compiler_warnings(minidb_write_bench)
