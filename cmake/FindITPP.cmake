# - Try to find ITPP
# Once done this will define
#
#  ITPP_FOUND - System has ITPP
#  ITPP_INCLUDE_DIR - The ITPP include directory
#  ITPP_LIBRARY - The library needed to use ITPP (may contain debug/optimized keywords)
#

find_path(ITPP_INCLUDE_DIR itpp/itcomm.h)

set(ITPP_NAMES itpp libitpp)
set(ITPP_NAMES_DEBUG itpp_debug libitpp_debug itppd libitppd itpp_d libitpp_d itpp libitpp)

# Find release library
find_library(ITPP_LIBRARY_RELEASE NAMES ${ITPP_NAMES})

# Compute vcpkg-style debug directory hint from the release library location
# vcpkg layout: <prefix>/lib/itpp.lib (release) and <prefix>/debug/lib/itpp.lib (debug)
set(_itpp_debug_hints "")
if(ITPP_LIBRARY_RELEASE)
    get_filename_component(_itpp_lib_dir "${ITPP_LIBRARY_RELEASE}" DIRECTORY)
    get_filename_component(_itpp_prefix "${_itpp_lib_dir}" DIRECTORY)
    if(EXISTS "${_itpp_prefix}/debug/lib")
        list(APPEND _itpp_debug_hints "${_itpp_prefix}/debug/lib")
    endif()
endif()

# Find debug library (vcpkg places these in a debug/ subdirectory with same name)
find_library(ITPP_LIBRARY_DEBUG NAMES ${ITPP_NAMES_DEBUG}
    HINTS ${_itpp_debug_hints}
    PATH_SUFFIXES debug/lib
)

# On MinGW, prefer the import library (`*.dll.a`) for dynamic linking.
if(MINGW)
    if(ITPP_LIBRARY_RELEASE AND ITPP_LIBRARY_RELEASE MATCHES "\\.a$" AND NOT ITPP_LIBRARY_RELEASE MATCHES "\\.dll\\.a$")
        string(REGEX REPLACE "\\.a$" ".dll.a" _itpp_shared_candidate "${ITPP_LIBRARY_RELEASE}")
        if(EXISTS "${_itpp_shared_candidate}")
            set(ITPP_LIBRARY_RELEASE "${_itpp_shared_candidate}")
        endif()
    endif()
    if(ITPP_LIBRARY_DEBUG AND ITPP_LIBRARY_DEBUG MATCHES "\\.a$" AND NOT ITPP_LIBRARY_DEBUG MATCHES "\\.dll\\.a$")
        string(REGEX REPLACE "\\.a$" ".dll.a" _itpp_shared_candidate "${ITPP_LIBRARY_DEBUG}")
        if(EXISTS "${_itpp_shared_candidate}")
            set(ITPP_LIBRARY_DEBUG "${_itpp_shared_candidate}")
        endif()
    endif()
endif()

# Build ITPP_LIBRARY with debug/optimized keywords for multi-config generators
if(ITPP_LIBRARY_DEBUG AND ITPP_LIBRARY_RELEASE)
    set(ITPP_LIBRARY optimized ${ITPP_LIBRARY_RELEASE} debug ${ITPP_LIBRARY_DEBUG})
elseif(ITPP_LIBRARY_RELEASE)
    set(ITPP_LIBRARY ${ITPP_LIBRARY_RELEASE})
elseif(ITPP_LIBRARY_DEBUG)
    set(ITPP_LIBRARY ${ITPP_LIBRARY_DEBUG})
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ITPP DEFAULT_MSG ITPP_LIBRARY ITPP_INCLUDE_DIR)

mark_as_advanced(ITPP_INCLUDE_DIR ITPP_LIBRARY ITPP_LIBRARY_RELEASE ITPP_LIBRARY_DEBUG)
