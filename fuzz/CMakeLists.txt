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

# One target per fuzzer, each a C file whose only entry point is libFuzzer's. The path fuzzer touches no disk;
# the two engine fuzzers each make a root of their own under this directory, handed to them through a generated
# header (never a compile definition), and read the engine's own name marks from core/src/names.h.
set(BC_FILE_SYSTEM_FUZZ_TEMPORARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/fuzz-temporary")
file(MAKE_DIRECTORY "${BC_FILE_SYSTEM_FUZZ_TEMPORARY_ROOT}")
configure_file(fuzz_paths.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/fuzz_paths.h" @ONLY)

function(bc_file_system_add_fuzzer target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE bc::file_system_core)
    bc_file_system_set_compile_options(${target})
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endfunction()

bc_file_system_add_fuzzer(bc_file_system_path_fuzz path_fuzz.c)

bc_file_system_add_fuzzer(bc_file_system_abi_arguments_fuzz abi_arguments_fuzz.c)
target_include_directories(bc_file_system_abi_arguments_fuzz PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/src
)

bc_file_system_add_fuzzer(bc_file_system_operation_sequence_fuzz operation_sequence_fuzz.c)
target_include_directories(bc_file_system_operation_sequence_fuzz PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/src
)
