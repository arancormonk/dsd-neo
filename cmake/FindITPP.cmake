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

# On MinGW, prefer the import library (`*.dll.a`) for dynamic linking.
if(MINGW AND ITPP_LIBRARY)
    if(ITPP_LIBRARY MATCHES "\\.a$" AND NOT ITPP_LIBRARY MATCHES "\\.dll\\.a$")
        string(REGEX REPLACE "\\.a$" ".dll.a" _itpp_shared_candidate "${ITPP_LIBRARY}")
        if(EXISTS "${_itpp_shared_candidate}")
            set(ITPP_LIBRARY "${_itpp_shared_candidate}")
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ITPP DEFAULT_MSG ITPP_LIBRARY ITPP_INCLUDE_DIR)
