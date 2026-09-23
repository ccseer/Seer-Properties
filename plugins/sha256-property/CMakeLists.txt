cmake_minimum_required(VERSION 3.21)
project(sha256_property LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
enable_testing()

add_library(sha256_core STATIC src/sha256.cpp)
target_include_directories(sha256_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(sha256_core PUBLIC bcrypt)

add_executable(sha256_property src/main.cpp)
target_link_libraries(sha256_property PRIVATE sha256_core shell32)

add_executable(sha256_property_test test/sha256_test.cpp)
target_link_libraries(sha256_property_test PRIVATE sha256_core)
add_test(NAME sha256_property_test COMMAND sha256_property_test)

add_executable(sha256_property_manifest_test test/manifest_test.cpp)

# Stages plugin.json, the helper and PACKAGE_README.md (as README.md) so the
# manifest test and the installed package both cover the shipped layout.
set(SHA256_PROPERTY_MANIFEST_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/manifest-stage)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/PackageStaging.cmake)
seer_property_package_staging(
    PACKAGE_ID io.1218.seer.sha256-property
    PACKAGE_NAME "SHA-256"
    COMMAND_NAME sha256_property.exe
    COMMAND_TARGET sha256_property
    TEST_TARGET sha256_property_manifest_test
    STAGE_TARGET sha256_property_manifest_stage
    PACKAGE_TARGET sha256_property_package
    STAGING_DIR ${SHA256_PROPERTY_MANIFEST_STAGING_DIR}
)
