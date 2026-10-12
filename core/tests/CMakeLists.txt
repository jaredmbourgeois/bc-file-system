# The C-only test of the path module: a `.c` main over tables of cases, registered with add_test (the non-gtest
# form), so the module is proven through the public header alone, with no face in between. The contract and
# binding suites under cpp/tests/ drive the same functions through the C++ face.
add_executable(bc_file_system_path_test
    path_test.c
)
target_link_libraries(bc_file_system_path_test PRIVATE bc::file_system_core)
bc_file_system_set_compile_options(bc_file_system_path_test)
add_test(NAME bc_file_system_path_test COMMAND bc_file_system_path_test)
