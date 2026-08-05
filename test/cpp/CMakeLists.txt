# Standalone build of the Catch2 pure-logic tests -- NO DuckDB, NO OpenSSL.
#
# The pure sources (URI parsing, glob matching, error classification, version,
# upload chunk plan, block cache) include no duckdb.hpp and do no I/O, so they
# compile and link against nothing but Catch2. Building them here, rather than
# through the full extension (`make unit_test`, which configures all of DuckDB),
# keeps the per-PR "Catch2 pure-logic tests" check to seconds instead of the
# platform-matrix build's tens of minutes. `make test` still exercises the same
# binary produced by the in-tree build; this is purely a faster CI path.
cmake_minimum_required(VERSION 3.16)
project(sharepoint_unit_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Catch2 3 CONFIG REQUIRED)

set(SP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(sharepoint_unit_tests
    ${SP_ROOT}/src/sharepoint_version.cpp
    ${SP_ROOT}/src/sharepoint_uri.cpp
    ${SP_ROOT}/src/sharepoint_errors.cpp
    ${SP_ROOT}/src/sharepoint_glob.cpp
    ${SP_ROOT}/src/sharepoint_upload.cpp
    ${SP_ROOT}/src/sharepoint_blockcache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_pure.cpp
)
target_include_directories(sharepoint_unit_tests PRIVATE ${SP_ROOT}/src/include)
target_link_libraries(sharepoint_unit_tests PRIVATE Catch2::Catch2WithMain)
