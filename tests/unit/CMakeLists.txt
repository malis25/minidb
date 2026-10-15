# One test executable keeps the unit-test setup simple as the library grows.
add_executable(minidb_unit_tests
    bplus_tree_tests.cpp
    crash_consistency_tests.cpp
    database_tests.cpp
    database_query_tests.cpp
    database_schema_tests.cpp
    database_migration_tests.cpp
    segmented_storage_tests.cpp
    segmented_storage_recovery_tests.cpp
    version_tests.cpp
    ${PROJECT_SOURCE_DIR}/app/version.cpp
)
target_include_directories(minidb_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/app)
target_compile_definitions(minidb_unit_tests PRIVATE MINIDB_VERSION="${PROJECT_VERSION}")
target_link_libraries(minidb_unit_tests PRIVATE minidb::core GTest::gtest_main)
minidb_set_compiler_warnings(minidb_unit_tests)

# A separate helper process lets a test terminate abruptly (std::_Exit) mid-write
# and then reopen the directory, exercising crash consistency end to end.
add_executable(minidb_crash_fixture crash_fixture_main.cpp)
target_link_libraries(minidb_crash_fixture PRIVATE minidb::core)
minidb_set_compiler_warnings(minidb_crash_fixture)
target_compile_definitions(minidb_unit_tests PRIVATE
    MINIDB_CRASH_FIXTURE="$<TARGET_FILE:minidb_crash_fixture>")
add_dependencies(minidb_unit_tests minidb_crash_fixture)

include(GoogleTest)
# Discover individual TEST/TEST_F cases so CTest can run/filter them separately.
gtest_discover_tests(minidb_unit_tests)

add_executable(minidb_http_tests
    http_database_tests.cpp
    http_object_tests.cpp
    http_schema_tests.cpp
    http_security_tests.cpp
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
