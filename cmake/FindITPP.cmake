# - Try to find ITPP
# Once done this will define
#
#  ITPP_FOUND - System has ITPP
#  ITPP_INCLUDE_DIR - The ITPP include directory
#  ITPP_LIBRARY - The library needed to use ITPP
#

find_path(ITPP_INCLUDE_DIR itpp/itcomm.h)

set(ITPP_NAMES ${ITPP_NAMES} itpp libitpp)
find_library(ITPP_LIBRARY NAMES ${ITPP_NAMES})

# On MinGW, CMake may prefer import libraries (`*.dll.a`) over static archives
# (`*.a`). When both exist, prefer the static archive to avoid runtime DLL
# dependencies for "static" builds (e.g., vcpkg *-mingw-static-* triplets).
if(MINGW AND ITPP_LIBRARY AND ITPP_LIBRARY MATCHES "\\.dll\\.a$")
    string(REGEX REPLACE "\\.dll\\.a$" ".a" _itpp_static_candidate "${ITPP_LIBRARY}")
    if(EXISTS "${_itpp_static_candidate}")
        set(ITPP_LIBRARY "${_itpp_static_candidate}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ITPP DEFAULT_MSG ITPP_LIBRARY ITPP_INCLUDE_DIR)
