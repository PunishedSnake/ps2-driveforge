cmake_minimum_required(VERSION 3.24)
project(ps2_driveforge_magicgate_tests LANGUAGES CXX)

enable_testing()

function(configure_magicgate_target target)
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

function(add_magicgate_test target source)
    add_executable(${target} ${source})
    configure_magicgate_target(${target})
endfunction()

add_magicgate_test(ps2-driveforge-magicgate-tests ../magicgate_tests.cpp)
add_magicgate_test(ps2-driveforge-magicgate-sign-tests ../magicgate_sign_tests.cpp)
add_magicgate_test(ps2-driveforge-magicgate-service-tests ../magicgate_service_tests.cpp)

# Provider staging is header-only apart from the shared SHA-256 primitive. Keep
# it in the fast cross-platform security gate together with strict local manifest
# parsing and immutable-plan validation so trust-boundary regressions do not
# wait for a full Windows package.
add_executable(ps2-driveforge-bootstrap-provider-tests
    ../bootstrap_provider_tests.cpp
    ../../src/core/sha256.cpp
)
configure_magicgate_target(ps2-driveforge-bootstrap-provider-tests)

add_executable(ps2-driveforge-bootstrap-manifest-tests
    ../bootstrap_manifest_tests.cpp
    ../../src/core/sha256.cpp
)
configure_magicgate_target(ps2-driveforge-bootstrap-manifest-tests)

add_executable(ps2-driveforge-bootstrap-frozen-tests
    ../bootstrap_frozen_tests.cpp
    ../../src/core/sha256.cpp
)
configure_magicgate_target(ps2-driveforge-bootstrap-frozen-tests)
