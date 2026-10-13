# The C++ face: a thin wrapper over the C header, its own target so the per-language flags stay well-defined under
# every generator and so a consumer can link the core alone.
add_library(bc_file_system STATIC
    include/bc_file_system/engine.hpp
    include/bc_file_system/error.hpp
    include/bc_file_system/file_system.hpp
    include/bc_file_system/location.hpp
    include/bc_file_system/options.hpp
    include/bc_file_system/path.hpp
    include/bc_file_system/required.hpp
    include/bc_file_system/result.hpp
    include/bc_file_system/status.hpp
    src/engine.cpp
    src/error.cpp
    src/file_system.cpp
    src/path.cpp
)
add_library(bc::file_system ALIAS bc_file_system)

target_include_directories(bc_file_system
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(bc_file_system PUBLIC bc::file_system_core)
bc_file_system_set_compile_options(bc_file_system)

if(BUILD_TESTING)
    # Every case creates its own directory under this root and removes it again. The root is handed to the tests
    # through a generated header (never a compile definition); they never ask the system where temporary files go.
    # A host build makes the root inside its build tree. A cross build's tests run on the device they were built
    # for, so the root is the directory on that device its preset names (BC_FILE_SYSTEM_DEVICE_TEST_ROOT, which
    # scripts/android_legs.sh creates there and pushes the test binaries into), and nothing is made on this machine.
    if(CMAKE_CROSSCOMPILING)
        if(NOT BC_FILE_SYSTEM_DEVICE_TEST_ROOT)
            message(FATAL_ERROR "A cross build with BUILD_TESTING needs BC_FILE_SYSTEM_DEVICE_TEST_ROOT, the directory on the device its tests run under (the android-* presets set it)")
        endif()
        set(BC_FILE_SYSTEM_TEST_TEMPORARY_ROOT "${BC_FILE_SYSTEM_DEVICE_TEST_ROOT}")
    else()
        set(BC_FILE_SYSTEM_TEST_TEMPORARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/test-temporary")
        file(MAKE_DIRECTORY "${BC_FILE_SYSTEM_TEST_TEMPORARY_ROOT}")
    endif()
    configure_file(tests/support/test_paths.hpp.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/test_paths.hpp" @ONLY
    )

    # The tests' own way to the file system (the one place the tests touch it without the library) and the
    # descriptor-balance fixture every suite runs under.
    add_library(bc_file_system_test_support STATIC
        tests/support/descriptor_balance.cpp
        tests/support/descriptor_balance.hpp
        tests/support/temp_directory.cpp
        tests/support/temp_directory.hpp
    )
    target_include_directories(bc_file_system_test_support
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
            "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
    target_link_libraries(bc_file_system_test_support PUBLIC GTest::gtest)
    bc_file_system_set_compile_options(bc_file_system_test_support)

    # The contract suite (the specification every face is held to, driving C through the C++ face), the C++
    # binding suite, the observer suite, the path-value suite and the path module's binding suite, in one binary.
    add_executable(bc_file_system_test
        tests/binding_test.cpp
        tests/contract_test.cpp
        tests/observer_test.cpp
        tests/path_test.cpp
        tests/path_values_test.cpp
    )
    target_link_libraries(bc_file_system_test PRIVATE
        bc::file_system
        bc_file_system_test_support
        GTest::gmock_main
    )
    bc_file_system_set_compile_options(bc_file_system_test)
    # gtest_discover_tests runs the binary at build time to list its cases, and ctest would run it after: neither
    # can run a binary built for another machine (an exec format error on this one), so a cross build registers
    # nothing and its device runs the binary instead.
    if(NOT CMAKE_CROSSCOMPILING)
        gtest_discover_tests(bc_file_system_test)
    endif()
endif()
