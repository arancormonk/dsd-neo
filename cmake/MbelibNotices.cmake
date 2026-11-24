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
if(_mbe_components_text MATCHES "\"id\"[ \t\r\n]*:[ \t\r\n]*\"pffft-neo\"")
    list(
        APPEND _mbe_required_notices
        pffft-neo/LICENSE
        pffft-neo/README.md
        pffft-neo/third_party/pffft/LICENSE.txt
        pffft-neo/third_party/pffft/LicenseRef-PFFFT.txt
    )
elseif(_mbe_components_text MATCHES "\"id\"[ \t\r\n]*:[ \t\r\n]*\"pffft\"")
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
# The folder this build packages, for the release checks (tools/check_release_notices.py).
set(DSD_MBELIB_SELECTED_NOTICE_DIR
    "${DSD_MBELIB_NOTICE_DIR}"
    CACHE INTERNAL
    "The mbelib-neo notice folder this build packages"
)
