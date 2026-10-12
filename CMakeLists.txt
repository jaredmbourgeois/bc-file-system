cmake_minimum_required(VERSION 3.25)

project(bc_file_system
    VERSION 0.1.0
    DESCRIPTION "A C23 file-system core with one header and C++, Swift and Rust faces"
    LANGUAGES C CXX
)

# The standards are set once, here. The core is C23 in its GNU dialect: strict C23 hides the POSIX declarations on
# musl and Emscripten. CMake spells it -std=gnu2x for AppleClang and GCC 14, gnu23 where the compiler knows the
# newer name. The faces and the tests are C++20 without extensions.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/helpers.cmake)

# Tests exist only for the project that owns the code. A consumer that fetched us never configures GoogleTest,
# never sees BUILD_TESTING, never builds a test.
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if(BUILD_TESTING)
        include(FetchContent)
        # GoogleTest v1.17.0, pinned by commit so every machine builds the suite against the same sources.
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        52eb8108c5bdec04579160ae17225d66034bd723
        )
        set(INSTALL_GTEST OFF CACHE BOOL "bc-file-system installs nothing" FORCE)
        FetchContent_MakeAvailable(googletest)
        include(GoogleTest)
    endif()
endif()

add_subdirectory(core)   # the C core: bc::file_system_core
add_subdirectory(cpp)    # the C++ face: bc::file_system, and the contract and binding suites
add_subdirectory(fuzz)   # the libFuzzer targets, behind BC_FILE_SYSTEM_BUILD_FUZZERS (the fuzz preset)
# No install(): consumers build from source through FetchContent and link the alias.
