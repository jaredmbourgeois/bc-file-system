# The C core. The public header is include/bc_file_system/bc_file_system.h; platform.h is private and is the only
# file that names the operating system. scripts/check_build_parity.py holds the .c list below equal to src/*.c.
add_library(bc_file_system_core STATIC
    src/bc_file_system.c
    src/platform.h
)
add_library(bc::file_system_core ALIAS bc_file_system_core)

target_include_directories(bc_file_system_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
bc_file_system_set_compile_options(bc_file_system_core)
bc_file_system_add_c_header_check(bc_file_system_core bc_file_system/bc_file_system.h)
