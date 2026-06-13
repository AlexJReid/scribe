cmake_minimum_required(VERSION 3.16)

set(SCRIBE_FALLBACK_VERSION "0.0.0")
set(SCRIBE_DETECTED_VERSION "${SCRIBE_FALLBACK_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE SCRIBE_GIT_TAG_RESULT
        OUTPUT_VARIABLE SCRIBE_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(SCRIBE_GIT_TAG_RESULT EQUAL 0)
        string(REGEX REPLACE "^v" "" SCRIBE_GIT_TAG_VERSION "${SCRIBE_GIT_TAG}")
        if(SCRIBE_GIT_TAG_VERSION MATCHES "^[0-9]+(\\.[0-9]+){0,3}$")
            set(SCRIBE_DETECTED_VERSION "${SCRIBE_GIT_TAG_VERSION}")
        endif()
    endif()
endif()
set(SCRIBE_VERSION "${SCRIBE_DETECTED_VERSION}" CACHE STRING "scribe version")

project(scribe VERSION "${SCRIBE_VERSION}" LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_static)
    set(SCRIBE_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
    set(SCRIBE_ZSTD_TARGET zstd::libzstd_shared)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    set(SCRIBE_ZSTD_TARGET PkgConfig::ZSTD)
endif()

set(SCRIBE_BUILD_PLATFORM "${CMAKE_SYSTEM_NAME}")
set(SCRIBE_BUILD_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
set(SCRIBE_BUILD_COMPILER "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
set(SCRIBE_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(NOT SCRIBE_BUILD_TYPE)
    set(SCRIBE_BUILD_TYPE "multi-config/unspecified")
endif()
set(SCRIBE_BUILD_INFO "${SCRIBE_BUILD_PLATFORM}/${SCRIBE_BUILD_ARCH} ${SCRIBE_BUILD_COMPILER} ${SCRIBE_BUILD_TYPE}")
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(src/scribe_version.h.in generated/scribe_version.h @ONLY)

function(scribe_enable_warnings target)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
        )
    endif()
endfunction()

add_library(yyjson third_party/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC third_party/yyjson)

add_library(scribe_core
    src/db/sqlite_util.c
    src/journal/event_writer.c
    src/journal/journal.c
    src/journal/journal_builder.c
    src/json/json_write.c
    src/phi/phi_vault.c
    src/phi/tokenise.c
    src/projection/balance_projector.c
    src/read_store/delta_exporter.c
    src/read_store/sqlite_store.c
    src/util/grow_vec.c
    src/util/money.c
    src/util/run_id.c
    src/util/str_util.c
    src/stitch/coverage/coverage_stitcher.c
    src/stitch/claims/aggregate_stitcher.c
    src/stitch/claims/snapshot.c
    src/x12/mappers/x12_mapper_270_271.c
    src/x12/mappers/x12_mapper_834.c
    src/x12/mappers/x12_mapper_835.c
    src/x12/mappers/x12_mapper_837.c
    src/x12/mappers/x12_mapper_phi.c
    src/x12/mappers/x12_mapper_registry.c
    src/x12/x12_parser.c
    src/x12/x12_reader.c
)

target_include_directories(scribe_core PUBLIC
    src
    src/db
    src/journal
    src/json
    src/phi
    src/projection
    src/read_store
    src/stitch/claims
    src/stitch/coverage
    src/util
    src/x12
    src/x12/mappers
)
target_link_libraries(scribe_core PUBLIC SQLite::SQLite3 OpenSSL::Crypto yyjson ${SCRIBE_ZSTD_TARGET})
scribe_enable_warnings(scribe_core)

add_executable(scribe src/main.c)
target_include_directories(scribe PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(scribe PRIVATE scribe_core)
scribe_enable_warnings(scribe)

enable_testing()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test-output)

add_test(NAME scribe_version COMMAND scribe --version)
set_tests_properties(scribe_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^scribe [0-9][0-9]*(\\.[0-9][0-9]*)*"
)

add_executable(test_parser tests/test_parser.c tests/json_scan.c)
target_link_libraries(test_parser PRIVATE scribe_core)
target_compile_definitions(test_parser PRIVATE
    TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output"
)
scribe_enable_warnings(test_parser)
if(MSVC)
    target_link_options(test_parser PRIVATE /STACK:8388608)
endif()

add_test(NAME test_parser COMMAND test_parser)

add_executable(test_x12_parser tests/test_x12_parser.c)
target_link_libraries(test_x12_parser PRIVATE scribe_core)
target_compile_definitions(test_x12_parser PRIVATE
    TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
)
scribe_enable_warnings(test_x12_parser)

add_test(NAME test_x12_parser COMMAND test_x12_parser)

add_executable(test_core tests/test_core.c tests/json_scan.c)
target_link_libraries(test_core PRIVATE scribe_core)
target_compile_definitions(test_core PRIVATE
    TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output"
)
scribe_enable_warnings(test_core)

add_test(NAME test_core COMMAND test_core)

add_executable(test_store tests/test_store.c)
target_link_libraries(test_store PRIVATE scribe_core)
target_compile_definitions(test_store PRIVATE
    TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output"
)
scribe_enable_warnings(test_store)

add_test(NAME test_store COMMAND test_store)

add_executable(test_837_mapper tests/test_837_mapper.c)
target_link_libraries(test_837_mapper PRIVATE scribe_core)
target_compile_definitions(test_837_mapper PRIVATE
    TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output"
)
scribe_enable_warnings(test_837_mapper)

add_test(NAME test_837_mapper COMMAND test_837_mapper)
