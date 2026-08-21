# Locate an installed Dokany 2.x SDK on Windows.
#
# Exposes:
#   Dokany_FOUND
#   Dokany_INCLUDE_DIR
#   Dokany_LIBRARY
#   Dokany::Dokany
#
# The official installer currently places development files under a versioned
# Program Files/Dokan/Dokan Library-* directory. DOKANY_ROOT can override that
# location for CI or custom installations.

set(DOKANY_ROOT "" CACHE PATH "Root of an installed Dokany 2.x SDK")

set(_dokany_roots)
if(DOKANY_ROOT)
    list(APPEND _dokany_roots "${DOKANY_ROOT}")
endif()

if(WIN32)
    file(GLOB _dokany_program_files_roots LIST_DIRECTORIES true
        "$ENV{ProgramFiles}/Dokan/Dokan Library-*"
    )
    if(_dokany_program_files_roots)
        list(SORT _dokany_program_files_roots COMPARE NATURAL ORDER DESCENDING)
        list(APPEND _dokany_roots ${_dokany_program_files_roots})
    endif()
endif()

find_path(Dokany_INCLUDE_DIR
    NAMES dokan/dokan.h dokan.h
    HINTS ${_dokany_roots}
    PATH_SUFFIXES include include/dokan
)

find_library(Dokany_LIBRARY
    NAMES dokan2
    HINTS ${_dokany_roots}
    PATH_SUFFIXES lib x64/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Dokany
    REQUIRED_VARS Dokany_INCLUDE_DIR Dokany_LIBRARY
)

if(Dokany_FOUND AND NOT TARGET Dokany::Dokany)
    add_library(Dokany::Dokany UNKNOWN IMPORTED)
    set_target_properties(Dokany::Dokany PROPERTIES
        IMPORTED_LOCATION "${Dokany_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Dokany_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(Dokany_INCLUDE_DIR Dokany_LIBRARY)
