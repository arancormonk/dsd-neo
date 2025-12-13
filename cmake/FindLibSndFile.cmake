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

set(LIBSNDFILE_LIBRARIES ${LIBSNDFILE_LIBRARY})

# When libsndfile is a static archive, consumers must also link its private
# codec dependencies (FLAC/ogg/vorbis/opus/mpg123/etc). On MinGW CI we use a
# static vcpkg triplet, so wire in `pkg-config --static` to get the full link
# line when available.
if(LIBSNDFILE_LIBRARY)
    get_filename_component(_libsndfile_name "${LIBSNDFILE_LIBRARY}" NAME)
    set(_libsndfile_is_static FALSE)
    if(_libsndfile_name MATCHES "\\.a$" AND NOT _libsndfile_name MATCHES "\\.dll\\.a$")
        set(_libsndfile_is_static TRUE)
    elseif(_libsndfile_name MATCHES "\\.lib$")
        set(_libsndfile_is_static TRUE)
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

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibSndFile DEFAULT_MSG LIBSNDFILE_LIBRARY
                                    LIBSNDFILE_INCLUDE_DIR)
