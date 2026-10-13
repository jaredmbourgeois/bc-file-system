# The hand-run benchmark of the C++ face against std::filesystem, <fstream> and POSIX (design doc, "Benchmark").
# Added by the root only when this is the top-level project and BC_FILE_SYSTEM_BUILD_BENCHMARKS is on, which the
# `bench` preset sets (Release, no posture): never a CI job, never a leg of scripts/verify.sh, never a consumer's
# target. The Rust program is the `std_comparison` bench target of crates/bc-file-system, run with `cargo bench`.
#
#   cmake --preset bench && cmake --build --preset bench
#   caffeinate -i build/bench/bench/bc_file_system_benchmark "$PWD/build/bench/bench/bench-temporary" \
#       20 1000 3 20 4096 1048576 67108864 1000 16 400 1 8 50 200
#
# The program makes one directory under the root it is handed and removes it when it ends; the root itself is
# made here, inside the build tree, so the run touches no shared directory.
set(BC_FILE_SYSTEM_BENCHMARK_TEMPORARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/bench-temporary")
file(MAKE_DIRECTORY "${BC_FILE_SYSTEM_BENCHMARK_TEMPORARY_ROOT}")

add_executable(bc_file_system_benchmark
    cpp/benchmark.cpp
    cpp/harness.cpp
    cpp/harness.hpp
)
target_link_libraries(bc_file_system_benchmark PRIVATE bc::file_system)
bc_file_system_set_compile_options(bc_file_system_benchmark)
