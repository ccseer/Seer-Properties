cmake_minimum_required(VERSION 3.21)
project(image_histogram LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Test)

get_filename_component(QT_BIN_DIR "${Qt6Core_DIR}/../../../bin" ABSOLUTE)

include(CTest)
enable_testing()

add_library(image_histogram_core STATIC
    src/histogram.cpp
    src/histogramrenderer.cpp
    src/imageanalysis.cpp
    src/scoperenderer.cpp
    src/histogramhelper.cpp
)
target_include_directories(image_histogram_core PUBLIC src)
target_link_libraries(image_histogram_core PUBLIC Qt6::Core Qt6::Gui)

add_executable(image_histogram
    src/main.cpp
)
target_link_libraries(image_histogram PRIVATE image_histogram_core)

add_executable(image_histogram_test
    test/histogram_test.cpp
)
target_link_libraries(image_histogram_test PRIVATE image_histogram_core Qt6::Test)
add_test(NAME image_histogram_test COMMAND image_histogram_test)
set_tests_properties(image_histogram_test PROPERTIES
    ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}"
)

add_executable(image_histogram_renderer_test
    test/histogramrenderer_test.cpp
)
target_link_libraries(image_histogram_renderer_test PRIVATE image_histogram_core Qt6::Test)
add_test(NAME image_histogram_renderer_test COMMAND image_histogram_renderer_test)
set_tests_properties(image_histogram_renderer_test PROPERTIES
    ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}"
)

add_executable(image_histogram_output_test
    test/output_contract_test.cpp
)
target_link_libraries(image_histogram_output_test PRIVATE image_histogram_core Qt6::Test)
add_test(NAME image_histogram_output_test COMMAND image_histogram_output_test)
set_tests_properties(image_histogram_output_test PROPERTIES
    ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}"
)

add_executable(image_histogram_analysis_test
    test/imageanalysis_test.cpp
)
target_link_libraries(image_histogram_analysis_test PRIVATE image_histogram_core Qt6::Test)
add_test(NAME image_histogram_analysis_test COMMAND image_histogram_analysis_test)
set_tests_properties(image_histogram_analysis_test PROPERTIES
    ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}"
)

add_executable(image_histogram_scope_renderer_test
    test/scoperenderer_test.cpp
)
target_link_libraries(image_histogram_scope_renderer_test PRIVATE image_histogram_core Qt6::Test)
add_test(NAME image_histogram_scope_renderer_test COMMAND image_histogram_scope_renderer_test)
set_tests_properties(image_histogram_scope_renderer_test PROPERTIES
    ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}"
)

add_executable(image_histogram_manifest_test
    test/manifest_test.cpp
)
target_include_directories(image_histogram_manifest_test PRIVATE src)
target_link_libraries(image_histogram_manifest_test PRIVATE Qt6::Core Qt6::Gui)

# Stages plugin.json, the helper and PACKAGE_README.md (as README.md) so the
# manifest test and the installed package both cover the shipped layout.
set(IMAGE_HISTOGRAM_MANIFEST_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/manifest-stage)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/PackageStaging.cmake)
seer_property_package_staging(
    PACKAGE_ID io.1218.seer.image-histogram
    PACKAGE_NAME "Image Scopes"
    COMMAND_NAME image_histogram.exe
    COMMAND_TARGET image_histogram
    TEST_TARGET image_histogram_manifest_test
    STAGE_TARGET image_histogram_manifest_stage
    PACKAGE_TARGET image_histogram_package
    STAGING_DIR ${IMAGE_HISTOGRAM_MANIFEST_STAGING_DIR}
)
set_tests_properties(image_histogram_manifest_test PROPERTIES
    ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}"
)
