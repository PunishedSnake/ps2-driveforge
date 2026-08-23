cmake_minimum_required(VERSION 3.24)
project(ps2_driveforge_magicgate_tests LANGUAGES CXX)

enable_testing()

add_executable(ps2-driveforge-magicgate-tests ../magicgate_tests.cpp)
target_include_directories(ps2-driveforge-magicgate-tests PRIVATE ../../include)
target_compile_features(ps2-driveforge-magicgate-tests PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(ps2-driveforge-magicgate-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(ps2-driveforge-magicgate-tests PRIVATE
        -Wall -Wextra -Wpedantic
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(ps2-driveforge-magicgate-tests PRIVATE
        -fsanitize=address,undefined
    )
endif()

add_test(NAME ps2-driveforge-magicgate-tests COMMAND ps2-driveforge-magicgate-tests)
