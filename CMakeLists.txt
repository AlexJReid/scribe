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
    src/coverage_stitcher.c
    src/event_writer.c
    src/journal.c
    src/journal_builder.c
    src/json_scan.c
    src/json_write.c
    src/phi/phi_vault.c
    src/phi/tokenise.c
    src/projection/balance_projector.c
    src/projection/projection.c
    src/read_store/sqlite_store.c
    src/run_id.c
    src/stitch/claims/aggregate_stitcher.c
    src/stitch/claims/snapshot.c
    src/x12_mapper_270_271.c
    src/x12_mapper_834.c
    src/x12_mapper_835.c
    src/x12_mapper_837.c
    src/x12_parser.c
    src/x12_reader.c
)

target_include_directories(scribe_core PUBLIC src src/phi src/projection src/read_store)
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

add_test(NAME test_parser COMMAND test_parser)
