cmake_minimum_required(VERSION 3.16)

project(scribe C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)

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
    src/journal/event_writer.c
    src/journal/journal.c
    src/journal/journal_builder.c
    src/json/json_scan.c
    src/json/json_write.c
    src/phi/phi_vault.c
    src/phi/tokenise.c
    src/projection/balance_projector.c
    src/projection/projection.c
    src/read_store/sqlite_store.c
    src/util/run_id.c
    src/stitch/coverage/coverage_stitcher.c
    src/stitch/claims/aggregate_stitcher.c
    src/stitch/claims/snapshot.c
    src/x12/mappers/x12_mapper_270_271.c
    src/x12/mappers/x12_mapper_834.c
    src/x12/mappers/x12_mapper_835.c
    src/x12/mappers/x12_mapper_837.c
    src/x12/x12_parser.c
    src/x12/x12_reader.c
)

target_include_directories(scribe_core PUBLIC
    src
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
target_link_libraries(scribe_core PUBLIC SQLite::SQLite3 yyjson)
scribe_enable_warnings(scribe_core)

add_executable(scribe src/main.c)
target_link_libraries(scribe PRIVATE scribe_core)
scribe_enable_warnings(scribe)

enable_testing()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test-output)

add_executable(test_parser tests/test_parser.c)
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

add_executable(test_837_mapper tests/test_837_mapper.c)
target_link_libraries(test_837_mapper PRIVATE scribe_core)
target_compile_definitions(test_837_mapper PRIVATE
    TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output"
)
scribe_enable_warnings(test_837_mapper)

add_test(NAME test_837_mapper COMMAND test_837_mapper)
