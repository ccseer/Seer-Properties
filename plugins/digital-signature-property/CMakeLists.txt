cmake_minimum_required(VERSION 3.21)
project(digital_signature_property LANGUAGES CXX)

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

add_library(digital_signature_core STATIC
    src/signaturequery.cpp
    src/signaturehelper.cpp
)
target_include_directories(digital_signature_core PUBLIC src)
target_include_directories(digital_signature_core PUBLIC ${NLOHMANN_INCLUDE_DIR})
target_include_directories(digital_signature_core PUBLIC ${PLUGIN_COMMON_INCLUDE_DIR})
# wintrust and crypt32 are the Windows trust/certificate APIs this plugin uses.
# JSON is produced through the single-header nlohmann/json vendored under
# plugins/third_party, so the package has no runtime dependency of its own.
target_link_libraries(digital_signature_core PRIVATE wintrust crypt32)

add_executable(digital_signature
    src/main.cpp
)
target_link_libraries(digital_signature PRIVATE digital_signature_core)

add_executable(digital_signature_query_test
    test/signaturequery_test.cpp
)
target_link_libraries(digital_signature_query_test PRIVATE digital_signature_core)
add_test(NAME digital_signature_query_test COMMAND digital_signature_query_test)

add_executable(digital_signature_output_test
    test/output_contract_test.cpp
)
target_link_libraries(digital_signature_output_test PRIVATE digital_signature_core)
add_test(NAME digital_signature_output_test COMMAND digital_signature_output_test)

set(DIGITAL_SIGNATURE_MANIFEST_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/manifest-stage)
add_custom_command(
    OUTPUT
        ${DIGITAL_SIGNATURE_MANIFEST_STAGING_DIR}/plugin.json
        ${DIGITAL_SIGNATURE_MANIFEST_STAGING_DIR}/digital_signature.exe
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIGITAL_SIGNATURE_MANIFEST_STAGING_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json
        ${DIGITAL_SIGNATURE_MANIFEST_STAGING_DIR}/plugin.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:digital_signature>
        ${DIGITAL_SIGNATURE_MANIFEST_STAGING_DIR}/digital_signature.exe
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json
        digital_signature
    VERBATIM
)
add_custom_target(digital_signature_manifest_stage
    DEPENDS
        ${DIGITAL_SIGNATURE_MANIFEST_STAGING_DIR}/plugin.json
        ${DIGITAL_SIGNATURE_MANIFEST_STAGING_DIR}/digital_signature.exe
)

add_executable(digital_signature_manifest_test test/manifest_test.cpp)
target_include_directories(digital_signature_manifest_test PRIVATE src)
target_include_directories(digital_signature_manifest_test PRIVATE ${NLOHMANN_INCLUDE_DIR})
add_dependencies(digital_signature_manifest_test digital_signature_manifest_stage)
add_test(NAME digital_signature_manifest_test
         COMMAND digital_signature_manifest_test ${DIGITAL_SIGNATURE_MANIFEST_STAGING_DIR})

install(TARGETS digital_signature RUNTIME DESTINATION .)
install(FILES plugin.json DESTINATION .)
