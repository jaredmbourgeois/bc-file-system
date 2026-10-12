# The C-only tests, each a `.c` main over tables of cases, registered with add_test (the non-gtest form). The
# path test proves the path module through the public header alone, with no face in between; the engine test
# drives the observer's hooks and the platform probes through the core's private headers (engine.h, platform.h),
# which no face can reach; the resolve test drives the resolution layer (resolve.h) over a tree it plants under
# the root handed to it on its command line, the one place the C tests touch the file system. The contract and
# binding suites under cpp/tests/ drive the same core through the C++ face.
add_executable(bc_file_system_path_test
    path_test.c
)
target_link_libraries(bc_file_system_path_test PRIVATE bc::file_system_core)
bc_file_system_set_compile_options(bc_file_system_path_test)
add_test(NAME bc_file_system_path_test COMMAND bc_file_system_path_test)

add_executable(bc_file_system_engine_test
    engine_test.c
)
target_link_libraries(bc_file_system_engine_test PRIVATE bc::file_system_core)
target_include_directories(bc_file_system_engine_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
bc_file_system_set_compile_options(bc_file_system_engine_test)
add_test(NAME bc_file_system_engine_test COMMAND bc_file_system_engine_test)

# The resolve test plants its tree under this root (never asking the system where temporary files go) and
# removes it when it ends; the root is handed in as the one argument, never read from the environment.
set(BC_FILE_SYSTEM_RESOLVE_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/test-temporary")
file(MAKE_DIRECTORY "${BC_FILE_SYSTEM_RESOLVE_TEST_ROOT}")
add_executable(bc_file_system_resolve_test
    resolve_test.c
)
target_link_libraries(bc_file_system_resolve_test PRIVATE bc::file_system_core)
target_include_directories(bc_file_system_resolve_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
bc_file_system_set_compile_options(bc_file_system_resolve_test)
add_test(NAME bc_file_system_resolve_test COMMAND bc_file_system_resolve_test "${BC_FILE_SYSTEM_RESOLVE_TEST_ROOT}")
