# The C core. The public header is include/bc_file_system/bc_file_system.h; platform.h and platform.c are private
# and are the only files that name the operating system; engine.h is the engine's private face (the observer's
# hooks, the probed fast paths) for the C-only test; path.c is the path module, the freestanding tier, with its
# private face path.h; resolve.c is the resolution layer (a location to a parent descriptor and a leaf name,
# with containment) behind resolve.h, over the cleanup idiom of owned.h; calls.c is every gated system call,
# one loop each, and the errno table, behind calls.h. scripts/check_build_parity.py holds the .c list below
# equal to src/*.c.
add_library(bc_file_system_core STATIC
    src/bc_file_system.c
    src/calls.c
    src/calls.h
    src/engine.h
    src/owned.h
    src/path.c
    src/path.h
    src/platform.c
    src/platform.h
    src/resolve.c
    src/resolve.h
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

# The path module is the freestanding tier: it may include nothing but <stddef.h> and <stdint.h> and call no
# library function. That is proved on every build, not stated: src/path.c is compiled twice more, as part of
# `all`, so a hosted include or a libc call fails the build here rather than in a consumer's wasm32 or kernel
# build. The objects are a compile proof, never linked, so they take the warning set and not the build posture
# (a sanitizer runtime does not exist for a bare wasm32 object).
set(path_freestanding_warnings
    -Wall -Wextra -Wpedantic
    ${BC_FILE_SYSTEM_C_WARNINGS}
)
if(PROJECT_IS_TOP_LEVEL)
    list(APPEND path_freestanding_warnings -Werror)
endif()

# The host compiler under -ffreestanding: every compiler accepts the flag.
add_library(bc_file_system_path_freestanding_host OBJECT src/path.c)
target_include_directories(bc_file_system_path_freestanding_host
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(bc_file_system_path_freestanding_host PRIVATE
    -ffreestanding
    ${path_freestanding_warnings}
    "$<$<C_COMPILER_ID:GNU>:-Wcast-align=strict>"
    "$<$<C_COMPILER_ID:Clang,AppleClang>:-Wcast-align>"
    "$<$<C_COMPILER_ID:Clang,AppleClang>:${BC_FILE_SYSTEM_CLANG_ONLY_C_WARNINGS}>"
)

# A bare wasm32 object, where there is no libc at all. GCC has no wasm32 back end and some clangs are built
# without it, so the object is built only where the compiler accepts the triple. Both the probe and the compile
# invoke the compiler directly rather than through a CMake target, because on a Darwin host CMake adds `-arch
# <host>` to every compile it drives and clang refuses that option for a wasm32 target. The probe's answer is
# recorded in the cache so scripts/verify.sh can name what was proved.
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/wasm32_probe.c" CONTENT "int bc_file_system_wasm32_probe(void);\n")
execute_process(
    COMMAND ${CMAKE_C_COMPILER} --target=wasm32 -ffreestanding -nostdlib -std=gnu2x -fsyntax-only
            "${CMAKE_CURRENT_BINARY_DIR}/wasm32_probe.c"
    RESULT_VARIABLE wasm32_probe_status
    OUTPUT_QUIET
    ERROR_QUIET
)
if(wasm32_probe_status EQUAL 0)
    set(BC_FILE_SYSTEM_C_COMPILER_ACCEPTS_WASM32 1 CACHE INTERNAL "the C compiler compiles for bare wasm32")
    set(wasm32_object "${CMAKE_CURRENT_BINARY_DIR}/path_freestanding_wasm32.o")
    add_custom_command(
        OUTPUT "${wasm32_object}"
        COMMAND ${CMAKE_C_COMPILER} --target=wasm32 -ffreestanding -nostdlib -std=gnu2x
                -I "${CMAKE_CURRENT_SOURCE_DIR}/include" -I "${CMAKE_CURRENT_SOURCE_DIR}/src"
                ${path_freestanding_warnings} -Wcast-align ${BC_FILE_SYSTEM_CLANG_ONLY_C_WARNINGS}
                -c "${CMAKE_CURRENT_SOURCE_DIR}/src/path.c" -o "${wasm32_object}"
        DEPENDS src/path.c src/path.h include/bc_file_system/bc_file_system.h
        COMMENT "Compiling the path module for bare wasm32 (the freestanding proof)"
        VERBATIM
    )
    add_custom_target(bc_file_system_path_freestanding_wasm32 ALL DEPENDS "${wasm32_object}")
    message(STATUS "bc-file-system: the path module's wasm32 freestanding check is built")
else()
    set(BC_FILE_SYSTEM_C_COMPILER_ACCEPTS_WASM32 0 CACHE INTERNAL "the C compiler compiles for bare wasm32")
    message(STATUS "bc-file-system: the C compiler has no wasm32 target; the path module's wasm32 freestanding check is not built")
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
