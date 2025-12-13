# - Try to find Libsndfile
# Once done this will define
#
#  LIBSNDFILE_FOUND - System has LIBSNDFILE
#  LIBSNDFILE_INCLUDE_DIR - The SNDFILE include directory
#  LIBSNDFILE_LIBRARY - The library needed to use SNDFILE
#  LIBSNDFILE_LIBRARIES - Libraries needed to link SNDFILE (including transitive deps for static builds)
#

find_path(LIBSNDFILE_INCLUDE_DIR sndfile.h)

set(LIBSNDFILE_NAMES ${LIBSNDFILE_NAMES} sndfile libsndfile)
find_library(LIBSNDFILE_LIBRARY NAMES ${LIBSNDFILE_NAMES})

# When libsndfile is a static archive, consumers must also link its private
# codec dependencies (FLAC/ogg/vorbis/opus/mpg123/etc). On MinGW CI we use a
# static vcpkg triplet, so wire in `pkg-config --static` to get the full link
# line when available.
if(LIBSNDFILE_LIBRARY)
    set(_dsd_libsndfile_prefer_static FALSE)
    if(DEFINED DSD_PREFER_STATIC_DEPS AND DSD_PREFER_STATIC_DEPS)
        set(_dsd_libsndfile_prefer_static TRUE)
    elseif(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "static")
        set(_dsd_libsndfile_prefer_static TRUE)
    endif()

    # On MinGW, prefer the import library (`*.dll.a`) when building dynamically.
    if(MINGW AND NOT _dsd_libsndfile_prefer_static AND LIBSNDFILE_LIBRARY MATCHES "\\.a$" AND NOT LIBSNDFILE_LIBRARY MATCHES "\\.dll\\.a$")
        string(REGEX REPLACE "\\.a$" ".dll.a" _libsndfile_dll_a_candidate "${LIBSNDFILE_LIBRARY}")
        if(EXISTS "${_libsndfile_dll_a_candidate}")
            set(LIBSNDFILE_LIBRARY "${_libsndfile_dll_a_candidate}")
        endif()
    endif()

    set(LIBSNDFILE_LIBRARIES ${LIBSNDFILE_LIBRARY})

    get_filename_component(_libsndfile_name "${LIBSNDFILE_LIBRARY}" NAME)
    set(_libsndfile_is_static FALSE)
    if(_libsndfile_name MATCHES "\\.a$" AND NOT _libsndfile_name MATCHES "\\.dll\\.a$")
        set(_libsndfile_is_static TRUE)
    elseif(_libsndfile_name MATCHES "\\.lib$")
        # On Windows, `.lib` can be either a static library or a DLL import
        # library. Only treat it as a static archive when we're explicitly
        # building with a static vcpkg triplet (or the project requests it),
        # otherwise we may incorrectly add `pkg-config --static` deps when
        # linking dynamically (x64-windows).
        if(_dsd_libsndfile_prefer_static)
            set(_libsndfile_is_static TRUE)
        endif()
    endif()

    if(_libsndfile_is_static)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            if(DEFINED PKG_CONFIG_ARGN)
                set(_libsndfile_pkg_config_argn_saved "${PKG_CONFIG_ARGN}")
                set(_libsndfile_pkg_config_argn_restore TRUE)
            else()
                set(_libsndfile_pkg_config_argn_restore FALSE)
            endif()

            set(PKG_CONFIG_ARGN --static)
            pkg_check_modules(LIBSNDFILE_PKG QUIET IMPORTED_TARGET sndfile)

            if(_libsndfile_pkg_config_argn_restore)
                set(PKG_CONFIG_ARGN "${_libsndfile_pkg_config_argn_saved}")
            else()
                unset(PKG_CONFIG_ARGN)
            endif()

            if(LIBSNDFILE_PKG_FOUND)
                set(LIBSNDFILE_LIBRARIES PkgConfig::LIBSNDFILE_PKG)
            endif()
        endif()
    endif()
endif()

if(NOT LIBSNDFILE_LIBRARIES)
    set(LIBSNDFILE_LIBRARIES ${LIBSNDFILE_LIBRARY})
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibSndFile DEFAULT_MSG LIBSNDFILE_LIBRARY
                                    LIBSNDFILE_INCLUDE_DIR)
