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
set(_dsd_itpp_prefer_static FALSE)
if(DEFINED DSD_PREFER_STATIC_DEPS AND DSD_PREFER_STATIC_DEPS)
    set(_dsd_itpp_prefer_static TRUE)
elseif(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "static")
    set(_dsd_itpp_prefer_static TRUE)
endif()

if(MINGW AND ITPP_LIBRARY)
    if(_dsd_itpp_prefer_static AND ITPP_LIBRARY MATCHES "\\.dll\\.a$")
        string(REGEX REPLACE "\\.dll\\.a$" ".a" _itpp_static_candidate "${ITPP_LIBRARY}")
        if(EXISTS "${_itpp_static_candidate}")
            set(ITPP_LIBRARY "${_itpp_static_candidate}")
        endif()
    elseif(NOT _dsd_itpp_prefer_static AND ITPP_LIBRARY MATCHES "\\.a$" AND NOT ITPP_LIBRARY MATCHES "\\.dll\\.a$")
        string(REGEX REPLACE "\\.a$" ".dll.a" _itpp_shared_candidate "${ITPP_LIBRARY}")
        if(EXISTS "${_itpp_shared_candidate}")
            set(ITPP_LIBRARY "${_itpp_shared_candidate}")
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ITPP DEFAULT_MSG ITPP_LIBRARY ITPP_INCLUDE_DIR)
