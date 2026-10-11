# Standalone build of the Catch2 pure-logic tests -- NO DuckDB, NO OpenSSL,
# NO vcpkg.
#
# The pure sources (URI parsing, glob matching, error classification, version,
# upload chunk plan, block cache) include no duckdb.hpp and do no I/O, so they
# compile and link against nothing but Catch2. Building them here, rather than
# through the full extension (`make unit_test`, which configures all of DuckDB),
# keeps the per-PR "Catch2 pure-logic tests" check to seconds instead of the
# platform-matrix build's tens of minutes. `make test` still exercises the same
# tests via the in-tree build; this is purely a faster, dependency-light CI path.
#
# Catch2 comes from the system if present, else is fetched -- so CI needs only a
# compiler and CMake, with nothing to install.
cmake_minimum_required(VERSION 3.16)
project(sharepoint_unit_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG 05e10dfccc28c7f973727c54f850237d07d5e10f # v3.5.2 (by commit: tags can be moved)
    )
    FetchContent_MakeAvailable(Catch2)
endif()

set(SP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(sharepoint_unit_tests
    ${SP_ROOT}/src/sharepoint_version.cpp
    ${SP_ROOT}/src/sharepoint_uri.cpp
    ${SP_ROOT}/src/sharepoint_errors.cpp
    ${SP_ROOT}/src/sharepoint_glob.cpp
    ${SP_ROOT}/src/sharepoint_upload.cpp
    ${SP_ROOT}/src/sharepoint_blockcache.cpp
    ${SP_ROOT}/src/sharepoint_rawhttp.cpp
    ${SP_ROOT}/src/sharepoint_identity.cpp
    ${SP_ROOT}/src/sharepoint_counters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_pure.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_glob.cpp
)
target_include_directories(sharepoint_unit_tests PRIVATE ${SP_ROOT}/src/include)
target_link_libraries(sharepoint_unit_tests PRIVATE Catch2::Catch2WithMain)
