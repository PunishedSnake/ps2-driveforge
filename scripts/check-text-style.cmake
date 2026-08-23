if(NOT DEFINED PS2DF_SOURCE_DIR)
    get_filename_component(PS2DF_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# Keep the pass intentionally limited to project-authored docs and C/C++ source.
# Generated output and third-party trees do not need our punctuation preferences
# imposed on them. We have enough maintenance work without linting strangers.
file(GLOB ROOT_DOCS
    "${PS2DF_SOURCE_DIR}/*.md"
)
file(GLOB_RECURSE PROJECT_DOCS
    "${PS2DF_SOURCE_DIR}/docs/*.md"
)
file(GLOB_RECURSE PROJECT_CODE
    "${PS2DF_SOURCE_DIR}/include/*.h"
    "${PS2DF_SOURCE_DIR}/include/*.hpp"
    "${PS2DF_SOURCE_DIR}/src/*.h"
    "${PS2DF_SOURCE_DIR}/src/*.hpp"
    "${PS2DF_SOURCE_DIR}/src/*.c"
    "${PS2DF_SOURCE_DIR}/src/*.cpp"
    "${PS2DF_SOURCE_DIR}/tests/*.cpp"
)
set(TEXT_FILES ${ROOT_DOCS} ${PROJECT_DOCS} ${PROJECT_CODE})

# Two pre-Frieren frontends still contain user-visible em dashes. Keep this as a
# path-exact legacy debt list rather than weakening the repository-wide rule.
# When either frontend is next edited substantially, remove its entry and clean
# the strings in the same change. New files never get grandfathered here merely
# because punctuation managed to become a build problem.
set(LEGACY_EM_DASH_ALLOWLIST
    "${PS2DF_SOURCE_DIR}/src/gui/windows_main.cpp"
    "${PS2DF_SOURCE_DIR}/src/winui/MainWindow.xaml.cpp"
)

set(FAILURES "")
foreach(FILE_PATH IN LISTS TEXT_FILES)
    file(READ "${FILE_PATH}" CONTENT)
    string(FIND "${CONTENT}" "—" EM_DASH_POS)
    if(NOT EM_DASH_POS EQUAL -1)
        list(FIND LEGACY_EM_DASH_ALLOWLIST "${FILE_PATH}" LEGACY_INDEX)
        if(LEGACY_INDEX EQUAL -1)
            list(APPEND FAILURES "${FILE_PATH}: contains an em dash; use a normal hyphen")
        endif()
    endif()
endforeach()

# These were the old Mutation Journal API/file names. Rescue Capsule is a real
# FHDB term now, so allowing the identifiers back into code would recreate the
# exact ambiguity the rename removed. Documentation may mention the retired name
# when explaining the rule; executable code may not resurrect it.
foreach(FILE_PATH IN LISTS PROJECT_CODE)
    file(READ "${FILE_PATH}" CONTENT)
    string(FIND "${CONTENT}" "RecoveryCapsule" OLD_TYPE_POS)
    string(FIND "${CONTENT}" "recovery_capsule" OLD_FILE_POS)
    if(NOT OLD_TYPE_POS EQUAL -1 OR NOT OLD_FILE_POS EQUAL -1)
        list(APPEND FAILURES "${FILE_PATH}: contains retired RecoveryCapsule/recovery_capsule terminology")
    endif()
endforeach()

if(FAILURES)
    list(JOIN FAILURES "\n  " FAILURE_TEXT)
    message(FATAL_ERROR
        "DriveForge text-style/recovery terminology check failed:\n  ${FAILURE_TEXT}\n"
        "FHDB Rescue Capsule means PS2HBRC v1; DriveForge transaction recovery means Mutation Journal.")
endif()

message(STATUS "DriveForge documentation/source terminology check passed")
