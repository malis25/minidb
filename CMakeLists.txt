cmake_minimum_required(VERSION 3.21)

project(minidb VERSION 0.1.0 LANGUAGES CXX)

# Avoid compiler-specific C++ extensions so the project stays portable C++20.
set(CMAKE_CXX_EXTENSIONS OFF)

# CTest provides the standard test command; FetchContent downloads pinned dependencies.
include(CTest)
include(FetchContent)

# Configure cpp-httplib as a dependency of the application, not as an installable
# product of this project. TLS is outside the current loopback-only learning server.
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cpp_httplib)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# The core library has no main() function. Unit tests and the application can link
# the same database implementation without duplicating or embedding entry points.
add_library(minidb_core
    src/core/database.cpp
    src/core/database_query.cpp
    src/index/bplus_tree.cpp
    src/storage/file_sync.cpp
    src/storage/segmented_storage.cpp
    src/storage/segmented_storage_io.cpp
    src/storage/segmented_storage_index.cpp
    src/storage/segmented_storage_manifest.cpp
)
add_library(minidb::core ALIAS minidb_core)

target_compile_features(minidb_core PUBLIC cxx_std_20)
target_include_directories(minidb_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(minidb_core PUBLIC nlohmann_json::nlohmann_json)
minidb_set_compiler_warnings(minidb_core)

add_subdirectory(src/server)

# The benchmark harness is optional: it links the server module and is not part
# of the shipped application. MINIDB_BUILD_BENCHMARKS=OFF skips it entirely.
option(MINIDB_BUILD_BENCHMARKS "Build the MiniDB benchmark harness" ON)
if(MINIDB_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

add_executable(minidb
    app/main.cpp
    app/version.cpp
)
target_compile_definitions(minidb PRIVATE MINIDB_VERSION="${PROJECT_VERSION}")
target_link_libraries(minidb PRIVATE minidb::server)
minidb_set_compiler_warnings(minidb)

# BUILD_TESTING comes from CTest. Turning it off removes GoogleTest from configure
# and build entirely; test code is never linked into the minidb application.
if(BUILD_TESTING)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # We use GoogleTest, not GoogleMock, so skip building the unused mock library.
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()
