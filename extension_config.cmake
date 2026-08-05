# Included by DuckDB's build system. Specifies which extension to load.
#
# ---------------------------------------------------------------------------
# Force C++17 for the ENTIRE build.
#
# DuckDB caches CMAKE_CXX_STANDARD=11, and a plain `set(... CACHE ...)` in our
# own CMakeLists is a no-op against an already-populated cache -- so DuckDB
# compiles below C++17 by default. That breaks in two ways inherited from the
# sibling ../duckdb-gdrive build:
#   * Linux/GCC: a weak/strong symbol split on BufferedFileWriter when part of
#     the tree is C++17 and libduckdb_static stays C++11 -> multiple definition.
#   * Windows/MSVC: DuckDB's bundled fmt uses inline variables, rejected by
#     MSVC without /std:c++17.
# Forcing C++17 for every TU fixes both. Must land before DuckDB configures
# third_party -- this file is included well before those add_subdirectory()
# calls, so it does.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to enforce" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)

# The 1.4 LTS build compiles sqlite3_api_wrapper.cpp, which pulls in the
# Win-SDK headers; at C++17 on MSVC std::byte then clashes with the SDK's
# global `byte`. Disable std::byte for the LTS line -- we do not use it.
set(_sp_ddb_ver "$ENV{DUCKDB_GIT_VERSION}")
if(NOT _sp_ddb_ver)
  execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/duckdb" describe --tags
    OUTPUT_VARIABLE _sp_ddb_ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(_sp_ddb_ver MATCHES "^v?1\\.4\\.")
  add_compile_definitions(_HAS_STD_BYTE=0)
  message(STATUS "sharepoint: DuckDB ${_sp_ddb_ver} (1.4 LTS) -- defining _HAS_STD_BYTE=0")
endif()

# ---------------------------------------------------------------------------
# Extension version, computed here rather than left to DuckDB.
# duckdb_extension_generate_version() passes literal quotes to git and falls
# back to a commit hash on every build; compute it correctly and pass it.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --always --match v*.*.*
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE SHAREPOINT_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT SHAREPOINT_GIT_DESCRIBE)
    find_package(Git QUIET)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --match v*.*.*
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE SHAREPOINT_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
message(STATUS "sharepoint extension version: ${SHAREPOINT_GIT_DESCRIBE}")

# Extension from this repo.
duckdb_extension_load(sharepoint
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    EXTENSION_VERSION "${SHAREPOINT_GIT_DESCRIBE}"
)

# Needed by the live SQL tests: COPY ... TO 'sharepoint://...' (FORMAT parquet)
# and read_parquet over the fixture files.
duckdb_extension_load(parquet)
