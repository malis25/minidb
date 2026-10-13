# One test executable keeps the unit-test setup simple as the library grows.
add_executable(minidb_unit_tests
    database_tests.cpp
    version_tests.cpp
    ${PROJECT_SOURCE_DIR}/app/version.cpp
)
target_include_directories(minidb_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/app)
target_compile_definitions(minidb_unit_tests PRIVATE MINIDB_VERSION="${PROJECT_VERSION}")
target_link_libraries(minidb_unit_tests PRIVATE minidb::core GTest::gtest_main)
minidb_set_compiler_warnings(minidb_unit_tests)

include(GoogleTest)
# Discover individual TEST/TEST_F cases so CTest can run/filter them separately.
gtest_discover_tests(minidb_unit_tests)

add_executable(minidb_http_tests
    http_api_tests.cpp
)
target_link_libraries(minidb_http_tests
    PRIVATE
        minidb::server
        httplib::httplib
        nlohmann_json::nlohmann_json
        GTest::gtest_main
)
minidb_set_compiler_warnings(minidb_http_tests)
gtest_discover_tests(minidb_http_tests)
