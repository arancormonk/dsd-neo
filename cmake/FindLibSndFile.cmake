# - Try to find Libsndfile
# Once done this will define
#
#  LIBSNDFILE_FOUND - System has LIBSNDFILE
#  LIBSNDFILE_INCLUDE_DIR - The SNDFILE include directory
#  LIBSNDFILE_LIBRARY - The library needed to use SNDFILE
#  LIBSNDFILE_LIBRARIES - Libraries needed to link SNDFILE
#

# Prefer libsndfile's own CONFIG package when it is installed. A static
# libsndfile pulls in FLAC/Ogg/Vorbis/Opus/mpg123, and only the imported target
# carries those transitive dependencies; the bare library path below leaves them
# unresolved at link time (vcpkg and cross sysroots hit this, shared system
# builds do not).
if(NOT LIBSNDFILE_LIBRARY)
    find_package(SndFile CONFIG QUIET)
    if(SndFile_FOUND AND TARGET SndFile::sndfile)
        set(LIBSNDFILE_LIBRARY SndFile::sndfile)
        get_target_property(
            _LIBSNDFILE_CONFIG_INCLUDES
            SndFile::sndfile
            INTERFACE_INCLUDE_DIRECTORIES
        )
        if(_LIBSNDFILE_CONFIG_INCLUDES)
            list(GET _LIBSNDFILE_CONFIG_INCLUDES 0 LIBSNDFILE_INCLUDE_DIR)
        endif()
        unset(_LIBSNDFILE_CONFIG_INCLUDES)
    endif()
endif()

if(NOT LIBSNDFILE_LIBRARY)
    # Hints let cross sysroots and vcpkg prefixes resolve without hand-editing.
    set(_LIBSNDFILE_HINTS
        ${LibSndFile_ROOT}
        $ENV{LibSndFile_ROOT}
        ${LIBSNDFILE_ROOT}
        $ENV{LIBSNDFILE_ROOT}
    )

    find_path(
        LIBSNDFILE_INCLUDE_DIR
        NAMES sndfile.h
        HINTS ${_LIBSNDFILE_HINTS}
        PATH_SUFFIXES include
    )

    set(LIBSNDFILE_NAMES ${LIBSNDFILE_NAMES} sndfile libsndfile)
    find_library(
        LIBSNDFILE_LIBRARY
        NAMES ${LIBSNDFILE_NAMES}
        HINTS ${_LIBSNDFILE_HINTS}
        PATH_SUFFIXES lib lib64
    )

    unset(_LIBSNDFILE_HINTS)
endif()

set(LIBSNDFILE_LIBRARIES ${LIBSNDFILE_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    LibSndFile
    DEFAULT_MSG
    LIBSNDFILE_LIBRARY
    LIBSNDFILE_INCLUDE_DIR
)
