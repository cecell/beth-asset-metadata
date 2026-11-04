cmake_minimum_required(VERSION 3.21)
project(asset_metadata VERSION 0.4.0 LANGUAGES CXX)

option(ASSET_METADATA_BUILD_TOOLS "Build the assetinfo command line program" ${PROJECT_IS_TOP_LEVEL})
option(ASSET_METADATA_BUILD_TESTS "Build and register the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(ASSET_METADATA_TOOL_BSA "Let assetinfo scan inside BSA archives (fetches bsa-reader)" ON)

add_library(asset_metadata STATIC
    src/dds.cpp
    src/tga.cpp
    src/nif.cpp
    src/nif_textures.cpp
    src/audio.cpp
    src/file.cpp)
add_library(asset::metadata ALIAS asset_metadata)
target_include_directories(asset_metadata PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(asset_metadata PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(asset_metadata PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(asset_metadata PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ASSET_METADATA_BUILD_TOOLS)
    add_executable(assetinfo tools/assetinfo.cpp)
    target_link_libraries(assetinfo PRIVATE asset::metadata)
    if(ASSET_METADATA_TOOL_BSA)
        include(FetchContent)
        FetchContent_Declare(bsa_reader
            GIT_REPOSITORY https://github.com/cecell/bsa-reader.git
            GIT_TAG v0.1.1)
        FetchContent_MakeAvailable(bsa_reader)
        target_link_libraries(assetinfo PRIVATE bsa::reader)
        target_compile_definitions(assetinfo PRIVATE ASSETINFO_HAS_BSA)
    endif()
endif()

if(ASSET_METADATA_BUILD_TESTS)
    enable_testing()
    add_executable(asset_metadata_tests tests/test_metadata.cpp)
    target_link_libraries(asset_metadata_tests PRIVATE asset::metadata)
    add_test(NAME unit COMMAND asset_metadata_tests)
endif()

include(GNUInstallDirs)
install(TARGETS asset_metadata EXPORT asset_metadataTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT asset_metadataTargets NAMESPACE asset:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/asset_metadata)
