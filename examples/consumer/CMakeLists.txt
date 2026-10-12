# A project that consumes bc-file-system the way another repository would: through FetchContent, linking the
# core by its namespaced name, with BUILD_TESTING unset and -Werror off. A CI leg of its own.
#
# To build it against the checkout it sits in, with no download (what CI does, so the leg runs before the
# repository has a URL):
#
#   cmake -S examples/consumer -B build/consumer -DFETCHCONTENT_SOURCE_DIR_BC_FILE_SYSTEM=<path to this checkout>
#   cmake --build build/consumer
cmake_minimum_required(VERSION 3.25)
project(bc_file_system_consumer LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(bc_file_system
    # Pin a commit: until 1.0.0 the interface may change in any release.
    GIT_REPOSITORY <the Forgejo URL, filled at repo-scaffolded-and-hosted>
    GIT_TAG        <the pinned commit, filled at repo-scaffolded-and-hosted>
)
FetchContent_MakeAvailable(bc_file_system)

add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE bc::file_system_core)
