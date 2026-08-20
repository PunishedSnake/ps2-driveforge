cmake_minimum_required(VERSION 3.24)

project(ps2_driveforge VERSION 0.2.0 LANGUAGES CXX)

set(PS2DF_CODENAME "Bocchi" CACHE STRING "Release codename")
option(PS2DF_BUILD_TESTS "Build unit tests" ON)
option(PS2DF_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ps2hdd/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ps2hdd/version.hpp
    @ONLY
)

add_library(ps2driveforge_core
    src/core/file_block_device.cpp
    src/core/apa.cpp
    src/core/apa_volume.cpp
    src/core/pfs.cpp
)

target_include_directories(ps2driveforge_core
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_features(ps2driveforge_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ps2driveforge_core PRIVATE /W4 /permissive-)
    if(PS2DF_WARNINGS_AS_ERRORS)
        target_compile_options(ps2driveforge_core PRIVATE /WX)
    endif()
else()
    target_compile_options(ps2driveforge_core PRIVATE -Wall -Wextra -Wpedantic)
    if(PS2DF_WARNINGS_AS_ERRORS)
        target_compile_options(ps2driveforge_core PRIVATE -Werror)
    endif()
endif()

if(WIN32)
    target_sources(ps2driveforge_core PRIVATE src/platform/windows/physical_drive.cpp)
    target_compile_definitions(ps2driveforge_core PUBLIC PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE=1)
endif()

add_executable(ps2-driveforge-inspect src/cli/main.cpp)
target_link_libraries(ps2-driveforge-inspect PRIVATE ps2driveforge_core)
target_compile_features(ps2-driveforge-inspect PRIVATE cxx_std_20)

if(PS2DF_BUILD_TESTS)
    enable_testing()
    add_executable(ps2-driveforge-tests tests/apa_tests.cpp)
    target_link_libraries(ps2-driveforge-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-tests COMMAND ps2-driveforge-tests)
endif()
