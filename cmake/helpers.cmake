# The compile-options helpers every target in this tree calls. A leaf CMakeLists.txt says what a target is (a
# library, its sources, its dependencies) and defers how it is compiled to the functions here, so the warning set,
# the toolchain-knob list and the build posture each live in one place. Always function(), never macro()
# (AGENTS.md, "No macros").

# The C-only warnings, as a list variable so the generator expression below stays one argument. Never
# -Wdeclaration-after-statement: declaring at first use is the readable form.
set(BC_FILE_SYSTEM_C_WARNINGS
    -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
    -Wbad-function-cast -Wnested-externs
    -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wcast-qual
    -Wnull-dereference -Wdouble-promotion -Wswitch-enum -Wimplicit-fallthrough
    -Werror=vla
)
# Warnings only clang knows: GCC 14 has neither, so they ride the clang branch of the helper.
set(BC_FILE_SYSTEM_CLANG_ONLY_C_WARNINGS
    -Wmissing-variable-declarations -Wassign-enum
)

# Knobs the whole tree sets. One list, applied PRIVATE by bc_file_system_set_compile_options. NDEBUG is not
# listed: CMake's Release and RelWithDebInfo flag sets already define it. _GNU_SOURCE and _FILE_OFFSET_BITS are not
# knobs of the build either: the one translation unit that needs them defines them at its top, guarded by the
# platform name, so every build system (CMake, SwiftPM, cargo's cc) compiles the same source the same way. The
# guard (scripts/check_house_rules.py, TOOLCHAIN_KNOBS) holds this list, _FORTIFY_SOURCE from the hardened posture
# below, and those two translation-unit knobs.
set(BC_FILE_SYSTEM_TOOLCHAIN_KNOBS
    $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
)

# Hardening flags and sanitizers blind each other (-ftrivial-auto-var-init=zero hides what MSan exists to find,
# _FORTIFY_SOURCE bypasses ASan's interceptors, -fsanitize-trap turns a report into SIGILL), so the posture is one
# cache value with four settings, chosen by a preset, and a leaf never touches it.
set(BC_FILE_SYSTEM_BUILD_POSTURE "none" CACHE STRING "none | hardened | sanitized | memory-sanitized")
set_property(CACHE BC_FILE_SYSTEM_BUILD_POSTURE PROPERTY STRINGS none hardened sanitized memory-sanitized)

function(bc_file_system_apply_build_posture target)
    if(BC_FILE_SYSTEM_BUILD_POSTURE STREQUAL "none")
        return()
    elseif(BC_FILE_SYSTEM_BUILD_POSTURE STREQUAL "hardened")
        # Production posture. _FORTIFY_SOURCE needs optimization to do anything, so the hardened preset
        # configures RelWithDebInfo.
        target_compile_definitions(${target} PRIVATE _FORTIFY_SOURCE=2)
        target_compile_options(${target} PRIVATE
            -fstack-protector-strong -ftrivial-auto-var-init=zero
            -fstrict-flex-arrays=3 -fsanitize=bounds -fsanitize-trap=bounds
        )
    elseif(BC_FILE_SYSTEM_BUILD_POSTURE STREQUAL "sanitized")
        # CI posture: ASan + UBSan everywhere, clang's integer checks where they exist.
        set(sanitizers address,undefined)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            string(APPEND sanitizers ",integer")
            # The integer group's unsigned-shift-base check fires inside libc++'s std::hash (murmur rotates
            # with a shift that discards bits by design), which GoogleTest reaches on every run; the other
            # integer checks stay on.
            target_compile_options(${target} PRIVATE -fno-sanitize=unsigned-shift-base)
        endif()
        target_compile_options(${target} PRIVATE -fsanitize=${sanitizers} -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=${sanitizers})
    elseif(BC_FILE_SYSTEM_BUILD_POSTURE STREQUAL "memory-sanitized")
        # MSan cannot share a binary with ASan, and only Linux clang ships it.
        target_compile_options(${target} PRIVATE -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=memory)
    else()
        message(FATAL_ERROR "BC_FILE_SYSTEM_BUILD_POSTURE is '${BC_FILE_SYSTEM_BUILD_POSTURE}'; expected none, hardened, sanitized or memory-sanitized")
    endif()
endfunction()

# Apply to every compiled target: each non-INTERFACE add_library, each add_executable, each test executable.
# The root sets each language's standard once and this function owns everything else.
function(bc_file_system_set_compile_options target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic
        "$<$<COMPILE_LANGUAGE:C>:${BC_FILE_SYSTEM_C_WARNINGS}>"
        # GCC's strict cast-alignment check has no clang spelling; clang takes the plain form.
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:GNU>>:-Wcast-align=strict>"
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:Clang,AppleClang>>:-Wcast-align>"
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:Clang,AppleClang>>:${BC_FILE_SYSTEM_CLANG_ONLY_C_WARNINGS}>"
    )
    # Warnings fail the build only for the project that owns the code. A consumer that builds us through
    # FetchContent with a newer compiler must not break on a warning we have not seen yet.
    if(PROJECT_IS_TOP_LEVEL)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE
            # Quiet known STL false-positives on this compiler.
            -Wno-maybe-uninitialized
            -Wno-missing-field-initializers
        )
    endif()
    target_compile_definitions(${target} PRIVATE ${BC_FILE_SYSTEM_TOOLCHAIN_KNOBS})
    bc_file_system_apply_build_posture(${target})
endfunction()

# The public header is parsed by four different front ends, and a construct one of them rejects is otherwise found
# by that consumer's build, one repository away. This compiles the header the four ways its consumers will: strict
# C11 under -pedantic-errors (the common-subset floor); gnu11 the way Swift's importer parses a C module
# (Objective-C on Apple, C elsewhere); C++20 under -pedantic-errors; and C++23. Each mode is an OBJECT library over
# a generated translation unit that includes the header and nothing else, so an error names the header line, and
# each is part of `all`.
function(bc_file_system_add_c_header_check lib_target header)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${lib_target}_header_check")
    # file(CONFIGURE) rewrites only when the content changes, so a reconfigure does not dirty the four objects.
    file(CONFIGURE OUTPUT "${generated}/include.c" CONTENT "#include <${header}>\n")
    file(CONFIGURE OUTPUT "${generated}/include.cpp" CONTENT "#include <${header}>\n")

    add_library(${lib_target}_header_c11 OBJECT "${generated}/include.c")
    set_target_properties(${lib_target}_header_c11 PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(${lib_target}_header_c11 PRIVATE -pedantic-errors)

    add_library(${lib_target}_header_gnu11 OBJECT "${generated}/include.c")
    set_target_properties(${lib_target}_header_gnu11 PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    if(APPLE)
        target_compile_options(${lib_target}_header_gnu11 PRIVATE -x objective-c)
    endif()

    add_library(${lib_target}_header_cxx20 OBJECT "${generated}/include.cpp")
    set_target_properties(${lib_target}_header_cxx20 PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF)
    target_compile_options(${lib_target}_header_cxx20 PRIVATE -pedantic-errors)

    add_library(${lib_target}_header_cxx23 OBJECT "${generated}/include.cpp")
    set_target_properties(${lib_target}_header_cxx23 PROPERTIES CXX_STANDARD 23 CXX_EXTENSIONS OFF)

    foreach(mode c11 gnu11 cxx20 cxx23)
        target_link_libraries(${lib_target}_header_${mode} PRIVATE ${lib_target})
        bc_file_system_set_compile_options(${lib_target}_header_${mode})
    endforeach()
endfunction()
