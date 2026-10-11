# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DECODE_IQ_MBE_CAPTURE_* case (issue #661): replays an I/Q fixture through dsd-neo with MBE capture into
# <WORK_DIR>/caps, given without a trailing separator, and checks the capture files. As iq_decode_check.cmake does, it
# first requires exit code 0, the expected payload regex and no sanitizer report, so a run that never decoded cannot
# pass a check for missing files.
#
# Every file in caps/ must carry the 4-byte header of its extension and hold whole records, at least one: a capture is
# created by its first frame. Nothing named caps* may appear beside the folder.
#
# Expected -D inputs: DSD_BIN, MODE, FIXTURE, EXPECTED, WORK_DIR.
# Optional: EXT (".amb", ".imb" or ".dmb": every file's), RECORD (record bytes, 8 or 12), FILES (exact file count),
# MIN_FILES, MIN_FRAMES (records over all files).
foreach(_var DSD_BIN MODE FIXTURE EXPECTED WORK_DIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "iq_mbe_capture_check: missing -D${_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")

execute_process(
    COMMAND
        "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${FIXTURE}" -o
        null -d "${WORK_DIR}/caps"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT 110
)
set(_all "${_out}\n${_err}")
if(
    "${_all}"
        MATCHES
        "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
)
    message(
        FATAL_ERROR
        "iq_mbe_capture_check: sanitizer report detected:\n${_all}"
    )
endif()
if(NOT "${_rc}" STREQUAL "0")
    message(
        FATAL_ERROR
        "iq_mbe_capture_check: dsd-neo exited with '${_rc}' (expected 0)\n${_all}"
    )
endif()
if(NOT "${_all}" MATCHES "${EXPECTED}")
    message(
        FATAL_ERROR
        "iq_mbe_capture_check: expected payload /${EXPECTED}/ not found in output\n${_all}"
    )
endif()

# A folder without its trailing separator still holds the captures: nothing lands beside it.
file(
    GLOB _beside
    LIST_DIRECTORIES true
    RELATIVE "${WORK_DIR}"
    "${WORK_DIR}/caps*"
)
list(REMOVE_ITEM _beside "caps")
if(_beside)
    message(
        FATAL_ERROR
        "iq_mbe_capture_check: captures written beside the folder: ${_beside}"
    )
endif()

file(GLOB _files LIST_DIRECTORIES false "${WORK_DIR}/caps/*")
list(LENGTH _files _count)
set(_frames 0)
foreach(_file IN LISTS _files)
    file(SIZE "${_file}" _size)
    # Hex: a text read of binary bytes can run past LIMIT.
    file(READ "${_file}" _header LIMIT 4 HEX)
    get_filename_component(_ext "${_file}" LAST_EXT)
    string(HEX "${_ext}" _ext_hex)
    if(NOT "${_header}" STREQUAL "${_ext_hex}")
        message(
            FATAL_ERROR
            "iq_mbe_capture_check: ${_file}: header ${_header} does not name its extension ${_ext}"
        )
    endif()
    if(DEFINED EXT AND NOT "${EXT}" STREQUAL "" AND NOT "${_ext}" STREQUAL "${EXT}")
        message(
            FATAL_ERROR
            "iq_mbe_capture_check: ${_file}: expected a ${EXT} capture"
        )
    endif()
    set(_record 8)
    if("${_ext}" STREQUAL ".imb")
        set(_record 12)
    endif()
    if(DEFINED RECORD AND NOT "${RECORD}" STREQUAL "")
        set(_record ${RECORD})
    endif()
    math(EXPR _body "${_size} - 4")
    math(EXPR _rest "${_body} % ${_record}")
    if(_body LESS_EQUAL 0 OR NOT _rest EQUAL 0)
        message(
            FATAL_ERROR
            "iq_mbe_capture_check: ${_file}: ${_size} bytes is not a header and whole ${_record}-byte records"
        )
    endif()
    math(EXPR _frames "${_frames} + ${_body} / ${_record}")
endforeach()

if(DEFINED FILES AND NOT "${FILES}" STREQUAL "" AND NOT _count EQUAL FILES)
    message(
        FATAL_ERROR
        "iq_mbe_capture_check: ${_count} capture files (expected ${FILES}): ${_files}\n${_all}"
    )
endif()
if(DEFINED MIN_FILES AND NOT "${MIN_FILES}" STREQUAL "" AND _count LESS MIN_FILES)
    message(
        FATAL_ERROR
        "iq_mbe_capture_check: ${_count} capture files (expected at least ${MIN_FILES})\n${_all}"
    )
endif()
if(
    DEFINED MIN_FRAMES
    AND NOT "${MIN_FRAMES}" STREQUAL ""
    AND _frames LESS MIN_FRAMES
)
    message(
        FATAL_ERROR
        "iq_mbe_capture_check: ${_frames} captured frames (expected at least ${MIN_FRAMES})\n${_all}"
    )
endif()
message(STATUS "iq_mbe_capture_check: ${_count} files, ${_frames} frames")
file(REMOVE_RECURSE "${WORK_DIR}")
