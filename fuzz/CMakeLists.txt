# The libFuzzer targets, off by default and on in the `fuzz` preset. libFuzzer is compiler-rt's, so these build
# with clang only (Homebrew LLVM on a Mac: Apple's clang ships no libFuzzer; clang-19 with libclang-rt-dev on
# the Linux runner). Asking for them on a compiler that cannot link them is an error, never a silent skip.
option(BC_FILE_SYSTEM_BUILD_FUZZERS "Build the libFuzzer targets (clang with libFuzzer; the fuzz preset)" OFF)
if(NOT BC_FILE_SYSTEM_BUILD_FUZZERS)
    return()
endif()

include(CheckCCompilerFlag)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer)
check_c_compiler_flag(-fsanitize=fuzzer BC_FILE_SYSTEM_C_COMPILER_ACCEPTS_FUZZER)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(NOT BC_FILE_SYSTEM_C_COMPILER_ACCEPTS_FUZZER)
    message(FATAL_ERROR "BC_FILE_SYSTEM_BUILD_FUZZERS is ON but ${CMAKE_C_COMPILER} cannot build with -fsanitize=fuzzer; use a clang with compiler-rt's libFuzzer (Homebrew LLVM on a Mac)")
endif()

# One target per fuzzer, each a C file whose only entry point is libFuzzer's; the ABI-argument and the
# operation-sequence fuzzers join this list at stage `locks-links-condemn`.
add_executable(bc_file_system_path_fuzz
    path_fuzz.c
)
target_link_libraries(bc_file_system_path_fuzz PRIVATE bc::file_system_core)
bc_file_system_set_compile_options(bc_file_system_path_fuzz)
target_compile_options(bc_file_system_path_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(bc_file_system_path_fuzz PRIVATE -fsanitize=fuzzer)
