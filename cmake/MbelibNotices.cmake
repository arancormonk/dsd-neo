# SPDX-License-Identifier: GPL-3.0-or-later
# Use the selected library's installed notices, including its component assessment.
# A vcpkg config is in share/mbe-neo; a normal install uses lib[/arch]/cmake/mbe-neo.
if(NOT DEFINED DSD_MBELIB_NOTICE_DIR)
    set(DSD_MBELIB_NOTICE_DIR
        ""
        CACHE PATH
        "Installed share/doc/mbelib-neo directory"
    )
endif()
if(NOT DSD_MBELIB_NOTICE_DIR)
    set(_mbe_search "${mbe-neo_DIR}")
    foreach(_level RANGE 0 5)
        if(EXISTS "${_mbe_search}/share/doc/mbelib-neo/legal/components.json")
            set(DSD_MBELIB_NOTICE_DIR "${_mbe_search}/share/doc/mbelib-neo")
            break()
        endif()
        get_filename_component(_mbe_search "${_mbe_search}" DIRECTORY)
    endforeach()
endif()
set(_mbe_required_notices
    LICENSE
    GPL-2.0-or-later.txt
    ISC.txt
    LicenseRef-PFFFT.txt
    LicenseRef-ETSI-EN-300-395-2-data.txt
    upstream-mbelib-attribution.md
    legal/components.json
    legal/status.md
    legal/patents.md
)
# mbelib 2.6.12 embeds pffft-neo and installs its notices under pffft-neo/;
# earlier 2.6 releases install the PFFFT fork's notices under pffft/. The
# installed component inventory says which one this mbelib embeds, so a stale
# subtree an in-place upgrade left behind cannot stand in for the other.
set(_mbe_components "${DSD_MBELIB_NOTICE_DIR}/legal/components.json")
set(_mbe_components_text "")
if(EXISTS "${_mbe_components}")
    file(READ "${_mbe_components}" _mbe_components_text)
endif()
# The notices must be those of the mbelib this build links, not of another
# installation's documentation (an override, or a prefix search falling back).
# Every 2.6 inventory names its release.
if(DEFINED mbe-neo_VERSION)
    set(_mbe_notice_release "")
    if(
        _mbe_components_text
            MATCHES
            "\"release\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]*)\""
    )
        set(_mbe_notice_release "${CMAKE_MATCH_1}")
    endif()
    if(NOT _mbe_notice_release VERSION_EQUAL mbe-neo_VERSION)
        message(
            FATAL_ERROR
            "mbelib-neo notices in ${DSD_MBELIB_NOTICE_DIR} are for release ${_mbe_notice_release}, but this build links mbelib-neo ${mbe-neo_VERSION}; set DSD_MBELIB_NOTICE_DIR to the share/doc/mbelib-neo directory of the linked installation."
        )
    endif()
endif()
if(_mbe_components_text MATCHES "\"id\"[ \t\r\n]*:[ \t\r\n]*\"pffft-neo\"")
    set(DSD_MBELIB_UNSELECTED_FFT pffft)
    list(
        APPEND _mbe_required_notices
        pffft-neo/LICENSE
        pffft-neo/README.md
        pffft-neo/third_party/pffft/LICENSE.txt
        pffft-neo/third_party/pffft/LicenseRef-PFFFT.txt
    )
elseif(_mbe_components_text MATCHES "\"id\"[ \t\r\n]*:[ \t\r\n]*\"pffft\"")
    set(DSD_MBELIB_UNSELECTED_FFT pffft-neo)
    list(APPEND _mbe_required_notices pffft/LICENSE.txt pffft/PFFFT-NOTICES.txt)
else()
    message(
        FATAL_ERROR
        "mbelib-neo's ${_mbe_components} names neither the pffft-neo nor the pffft component; install mbelib-neo 2.6+ including its documentation, or set DSD_MBELIB_NOTICE_DIR to that installation's share/doc/mbelib-neo directory."
    )
endif()
foreach(_notice IN LISTS _mbe_required_notices)
    if(NOT EXISTS "${DSD_MBELIB_NOTICE_DIR}/${_notice}")
        message(
            FATAL_ERROR
            "Missing mbelib-neo notice ${_notice}; install mbelib-neo 2.6+ including its documentation, or set DSD_MBELIB_NOTICE_DIR to that installation's share/doc/mbelib-neo directory."
        )
    endif()
endforeach()
# The other FFT's notice tree, which an in-place upgrade or downgrade can leave
# beside the selected one, stays out of packages. Packages copy the notice
# folder's top-level entries by name, without that one; a path regex would
# depend on the host's path spelling (file(COPY) and install() lowercase it on
# macOS and Windows before matching).
file(
    GLOB _mbe_notice_entries
    LIST_DIRECTORIES true
    RELATIVE "${DSD_MBELIB_NOTICE_DIR}"
    "${DSD_MBELIB_NOTICE_DIR}/*"
)
list(REMOVE_ITEM _mbe_notice_entries "${DSD_MBELIB_UNSELECTED_FFT}")
set(DSD_MBELIB_NOTICE_DIRS)
set(DSD_MBELIB_NOTICE_FILES)
foreach(_entry IN LISTS _mbe_notice_entries)
    if(IS_DIRECTORY "${DSD_MBELIB_NOTICE_DIR}/${_entry}")
        list(APPEND DSD_MBELIB_NOTICE_DIRS "${DSD_MBELIB_NOTICE_DIR}/${_entry}")
    else()
        list(
            APPEND DSD_MBELIB_NOTICE_FILES
            "${DSD_MBELIB_NOTICE_DIR}/${_entry}"
        )
    endif()
endforeach()
# The folder this build packages, for the release checks (tools/check_release_notices.py).
set(DSD_MBELIB_SELECTED_NOTICE_DIR
    "${DSD_MBELIB_NOTICE_DIR}"
    CACHE INTERNAL
    "The mbelib-neo notice folder this build packages"
)
