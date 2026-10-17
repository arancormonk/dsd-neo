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
    pffft/LICENSE.txt
    pffft/PFFFT-NOTICES.txt
    legal/components.json
    legal/status.md
    legal/patents.md
)
foreach(_notice IN LISTS _mbe_required_notices)
    if(NOT EXISTS "${DSD_MBELIB_NOTICE_DIR}/${_notice}")
        message(
            FATAL_ERROR
            "Missing mbelib-neo notice ${_notice}; install mbelib-neo 2.6+ including its documentation, or set DSD_MBELIB_NOTICE_DIR to that installation's share/doc/mbelib-neo directory."
        )
    endif()
endforeach()
