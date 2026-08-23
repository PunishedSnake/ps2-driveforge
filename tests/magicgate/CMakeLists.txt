cmake_minimum_required(VERSION 3.24)
project(ps2_driveforge_magicgate_tests LANGUAGES CXX)

enable_testing()

function(add_magicgate_test target source)
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE ../../include)
    target_compile_features(${target} PRIVATE cxx_std_20)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
        )
    endif()

    add_test(NAME ${target} COMMAND ${target})
endfunction()

# Keep the crypto harness independent from the full DriveForge graph. A broken
# KELF byte invariant should take seconds to diagnose, not require WinUI,
# Dokany and half the storage stack to assemble for moral support.
add_magicgate_test(ps2-driveforge-magicgate-tests ../magicgate_tests.cpp)
add_magicgate_test(ps2-driveforge-magicgate-sign-tests ../magicgate_sign_tests.cpp)
add_magicgate_test(ps2-driveforge-magicgate-service-tests ../magicgate_service_tests.cpp)
