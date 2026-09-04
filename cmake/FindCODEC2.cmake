# - Try to find CODEC2
# Once done this will define

if(NOT CODEC2_FOUND)
    # pkg-config answers for the build host, so its hints are actively wrong when
    # cross compiling: an Android configure otherwise picks up /usr/include/codec2
    # and /usr/lib from the x86-64 host and feeds them to an arm64 search. Only
    # the toolchain's CMAKE_FIND_ROOT_PATH_MODE_* settings keep that from being
    # used. Skip it and let find_path/find_library search the sysroot alone.
    if(NOT CMAKE_CROSSCOMPILING)
        find_package(PkgConfig)
        pkg_check_modules(CODEC2_PKG codec2)
    endif()

    find_path(
        CODEC2_INCLUDE_DIR
        NAMES codec2/codec2.h codec2.h
        HINTS ${CODEC2_PKG_INCLUDE_DIRS}
    )

    find_library(
        CODEC2_LIBRARY
        NAMES codec2/codec2 codec2
        HINTS ${CODEC2_PKG_LIBRARY_DIRS}
    )

    set(CODEC2_LIBRARIES ${CODEC2_LIBRARY})
    set(CODEC2_INCLUDE_DIRS ${CODEC2_INCLUDE_DIR})

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(
        CODEC2
        DEFAULT_MSG
        CODEC2_LIBRARY
        CODEC2_INCLUDE_DIR
    )
    mark_as_advanced(CODEC2_INCLUDE_DIR CODEC2_LIBRARY)
endif(NOT CODEC2_FOUND)
