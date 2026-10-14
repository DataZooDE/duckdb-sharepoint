cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME sharepoint)

find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# picojson stores parsed numbers as int64_t only when PICOJSON_USE_INT64 is
# defined BEFORE picojson.h is first included; otherwise every number is a
# double. Graph returns file sizes and token expiries as integers, so define
# it for the whole project rather than per-file -- a future #include cannot
# then silently reintroduce the double/int split.
add_compile_definitions(PICOJSON_USE_INT64)

# The vcpkg picojson port (>=1.3.0) installs the header at
# include/picojson/picojson.h, but the sources -- like the sibling gdrive --
# spell it `#include <picojson.h>`. Add the subdirectory to the include path so
# the bare form resolves regardless of the vcpkg layout, rather than rewriting
# every #include.
find_path(PICOJSON_INCLUDE_DIR NAMES picojson.h PATH_SUFFIXES picojson)
if(NOT PICOJSON_INCLUDE_DIR)
  message(FATAL_ERROR "picojson.h not found -- is the picojson vcpkg dependency installed?")
endif()
include_directories(${PICOJSON_INCLUDE_DIR})

# DuckDB ships cpp-httplib (namespace duckdb_httplib_openssl) under its
# third_party tree. Use it directly rather than adding a separate vcpkg dep,
# matching the sibling ../duckdb-gdrive. Pure-logic targets (the Catch2 binary)
# deliberately do NOT get this include path.
include_directories(./duckdb/third_party/httplib)

# Anonymous usage telemetry and the feedback banner, shared with the sibling
# DataZoo extensions (gdrive, erpl, anofox-*). Native only: telemetry uses
# duckdb_httplib_openssl::SSLClient, which is not portable to wasm. Opt-out:
# `SET sharepoint_telemetry_enabled = false` or DATAZOO_DISABLE_TELEMETRY=1.
if(NOT EMSCRIPTEN)
    set(DATAZOO_BANNER_TELEMETRY ON CACHE BOOL "" FORCE)
    set(DATAZOO_BANNER_TELEMETRY_INCLUDE_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/posthog-telemetry/include CACHE PATH "" FORCE)
endif()
add_subdirectory(datazoo-banner)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/datazoo-banner/include)

if(NOT EMSCRIPTEN)
    include_directories(posthog-telemetry/include)
    add_subdirectory(posthog-telemetry)
endif()

# ---------------------------------------------------------------------------
# datazoo-oauth2 -- the shared, provider-agnostic OAuth2 stack the sibling
# duckdb-gdrive uses (PKCE, the loopback redirect server, browser launch).
# PROVIDER authorization_code builds on it; see docs/hld.md section 5.
#
# Pinned by commit, not tracked by branch: each consumer upgrades on its own
# schedule. Native only, like posthog-telemetry: the loopback server and the
# browser launcher have no meaning under emscripten.
# ---------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
    add_subdirectory(datazoo-oauth2)
endif()

# ---------------------------------------------------------------------------
# Pure-logic sources: no `#include "duckdb.hpp"`, no I/O. Compiled into BOTH
# the extension and the Catch2 unit-test binary. Because they do no I/O there
# is nothing to mock.
# ---------------------------------------------------------------------------
file(GLOB PURE_SOURCES CONFIGURE_DEPENDS
    src/sharepoint_version.cpp
    src/sharepoint_uri.cpp
    src/sharepoint_errors.cpp
    src/sharepoint_glob.cpp
    src/sharepoint_upload.cpp
    src/sharepoint_blockcache.cpp
    src/sharepoint_rawhttp.cpp
    src/sharepoint_identity.cpp
    src/sharepoint_counters.cpp
    src/sharepoint_token_policy.cpp
    src/sharepoint_delegated.cpp
)

# ---------------------------------------------------------------------------
# DuckDB-coupled sources. Everything here is exercised by live SQL tests
# against a real Microsoft Graph tenant, never by a fake.
# ---------------------------------------------------------------------------
file(GLOB DUCKDB_SOURCES CONFIGURE_DEPENDS
    src/sharepoint_extension.cpp
    src/sharepoint_secret.cpp
    src/sharepoint_auth.cpp
    src/sharepoint_delegated_flow.cpp
    src/sharepoint_client.cpp
    src/sharepoint_preauth.cpp
    src/sharepoint_stats.cpp
    src/sharepoint_filesystem.cpp
    src/sharepoint_vfs_functions.cpp
)

set(EXTENSION_SOURCES ${PURE_SOURCES} ${DUCKDB_SOURCES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)

target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
if(NOT EMSCRIPTEN)
  target_link_libraries(${EXTENSION_NAME} posthog_telemetry datazoo_oauth2)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} posthog_telemetry datazoo_oauth2)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# ---------------------------------------------------------------------------
# Catch2 unit-test binary -- pure logic only, no DuckDB linkage.
# Skipped on MSVC by default (static/dynamic CRT mismatch with vcpkg Catch2),
# exactly as in the sibling gdrive build. The pure layer (URI parsing, glob
# matching, error classification, token-endpoint construction) has no
# platform-dependent behaviour, so Linux + macOS coverage is real.
# ---------------------------------------------------------------------------
if(MSVC)
  set(SHAREPOINT_BUILD_UNIT_TESTS_DEFAULT OFF)
else()
  set(SHAREPOINT_BUILD_UNIT_TESTS_DEFAULT ON)
endif()
option(SHAREPOINT_BUILD_UNIT_TESTS "Build the Catch2 pure-logic test binary"
       ${SHAREPOINT_BUILD_UNIT_TESTS_DEFAULT})

find_package(Catch2 3 CONFIG QUIET)
if(Catch2_FOUND AND NOT EMSCRIPTEN AND SHAREPOINT_BUILD_UNIT_TESTS)
  file(GLOB SHAREPOINT_UNIT_TEST_SOURCES CONFIGURE_DEPENDS test/cpp/test_*.cpp)
  if(SHAREPOINT_UNIT_TEST_SOURCES)
    add_executable(sharepoint_unit_tests
      ${PURE_SOURCES}
      ${SHAREPOINT_UNIT_TEST_SOURCES}
    )
    target_include_directories(sharepoint_unit_tests PRIVATE src/include)
    target_compile_features(sharepoint_unit_tests PRIVATE cxx_std_17)
    target_link_libraries(sharepoint_unit_tests PRIVATE
      Catch2::Catch2WithMain
      OpenSSL::SSL
      OpenSSL::Crypto
    )
    set_target_properties(sharepoint_unit_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
    )
    if(MSVC)
      set_target_properties(sharepoint_unit_tests PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
      )
    endif()
  endif()
endif()
