cmake_minimum_required(VERSION 3.21)
project(git_info_property LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The helper has no Qt dependency, so it can link the static C runtime and ship
# as a single self-contained executable without a redistributable requirement.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
enable_testing()

set(NLOHMANN_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
# Header-only helpers shared with the other non-Qt packages.
set(PLUGIN_COMMON_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_library(git_info_core STATIC
    src/gitrunner.cpp
    src/gitdiscovery.cpp
    src/githelper.cpp
)
target_include_directories(git_info_core PUBLIC src)
target_include_directories(git_info_core PUBLIC ${NLOHMANN_INCLUDE_DIR})
target_include_directories(git_info_core PUBLIC ${PLUGIN_COMMON_INCLUDE_DIR})
# Only Win32 system libraries are needed. JSON is produced through the
# single-header nlohmann/json vendored under plugins/third_party, so the package
# has no runtime dependency of its own.

add_executable(git_info
    src/main.cpp
)
target_link_libraries(git_info PRIVATE git_info_core)

# Test double that emulates git.exe for deterministic tests.
add_executable(git_info_fake_git
    test/fakegit.cpp
)

add_executable(git_info_runner_test
    test/gitrunner_test.cpp
)
target_link_libraries(git_info_runner_test PRIVATE git_info_core)
target_compile_definitions(git_info_runner_test PRIVATE
    FAKE_GIT_PATH="$<TARGET_FILE:git_info_fake_git>")
add_test(NAME git_info_runner_test COMMAND git_info_runner_test)

add_executable(git_info_discovery_test
    test/gitdiscovery_test.cpp
)
target_link_libraries(git_info_discovery_test PRIVATE git_info_core)
add_test(NAME git_info_discovery_test COMMAND git_info_discovery_test)

add_executable(git_info_output_test
    test/output_contract_test.cpp
)
target_link_libraries(git_info_output_test PRIVATE git_info_core)
target_compile_definitions(git_info_output_test PRIVATE
    FAKE_GIT_PATH="$<TARGET_FILE:git_info_fake_git>")
add_test(NAME git_info_output_test COMMAND git_info_output_test)

# Integration test against the real Git found on PATH. It reports SKIP and
# succeeds when no Git installation is available, so it never makes the suite
# depend on a developer machine's PATH.
add_executable(git_info_real_git_test
    test/realgit_test.cpp
)
target_link_libraries(git_info_real_git_test PRIVATE git_info_core)
add_test(NAME git_info_real_git_test COMMAND git_info_real_git_test)

set(GIT_INFO_MANIFEST_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/manifest-stage)
add_custom_command(
    OUTPUT
        ${GIT_INFO_MANIFEST_STAGING_DIR}/plugin.json
        ${GIT_INFO_MANIFEST_STAGING_DIR}/git_info.exe
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GIT_INFO_MANIFEST_STAGING_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json
        ${GIT_INFO_MANIFEST_STAGING_DIR}/plugin.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:git_info>
        ${GIT_INFO_MANIFEST_STAGING_DIR}/git_info.exe
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json
        git_info
    VERBATIM
)
add_custom_target(git_info_manifest_stage
    DEPENDS
        ${GIT_INFO_MANIFEST_STAGING_DIR}/plugin.json
        ${GIT_INFO_MANIFEST_STAGING_DIR}/git_info.exe
)

add_executable(git_info_manifest_test test/manifest_test.cpp)
target_include_directories(git_info_manifest_test PRIVATE src)
target_include_directories(git_info_manifest_test PRIVATE ${NLOHMANN_INCLUDE_DIR})
add_dependencies(git_info_manifest_test git_info_manifest_stage)
add_test(NAME git_info_manifest_test
         COMMAND git_info_manifest_test ${GIT_INFO_MANIFEST_STAGING_DIR})

install(TARGETS git_info RUNTIME DESTINATION .)
install(FILES plugin.json DESTINATION .)
