# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DECODE_IQ_SPEECH_LEVEL_* case (issue #518): replays an I/Q fixture through dsd-neo with its voice on stdout
# (-o -) and measures the whole output's active speech level (ITU-T P.56, tests/core/speech_parity_tool.c `level`).
# The level must lie inside [MIN, MAX]. With REFERENCE, the fixture is replayed a second time with those arguments too
# (the fixed reference gain, -g 1), and the first run's level must have moved from that one toward -24 dBFS. As
# iq_decode_check.cmake does, each run must exit 0 and raise no sanitizer report.
#
# Expected -D inputs: DSD_BIN, TOOL_BIN, MODE, FIXTURE, WORK_DIR, CHANNELS, MIN, MAX. Optional: REFERENCE.
foreach(
    _var
    DSD_BIN
    TOOL_BIN
    MODE
    FIXTURE
    WORK_DIR
    CHANNELS
    MIN
    MAX
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "speech_level_check: missing -D${_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Every failure removes the work folder first: a test leaves no files behind, passing or not.
function(_level_fail text)
    file(REMOVE_RECURSE "${WORK_DIR}")
    message(FATAL_ERROR "speech_level_check: ${text}")
endfunction()

function(_level_replay out args)
    separate_arguments(_args UNIX_COMMAND "${args}")
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_args} --iq-replay "${FIXTURE}" -o -
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_FILE "${WORK_DIR}/${out}"
        ERROR_VARIABLE _err
        TIMEOUT 110
    )
    if(
        "${_err}"
            MATCHES
            "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        _level_fail("${args}: sanitizer report detected:\n${_err}")
    endif()
    if(NOT "${_rc}" STREQUAL "0")
        _level_fail("${args}: dsd-neo exited with '${_rc}' (expected 0)\n${_err}")
    endif()
endfunction()

_level_replay(voice.raw "${MODE}")
set(_toward)
if(DEFINED REFERENCE)
    _level_replay(reference.raw "${MODE} ${REFERENCE}")
    set(_toward --toward reference.raw)
endif()

execute_process(
    COMMAND
        "${TOOL_BIN}" level --min-dbfs ${MIN} --max-dbfs ${MAX} ${_toward}
        voice.raw 8000 ${CHANNELS}
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
)
if(NOT "${_rc}" STREQUAL "0")
    _level_fail("${_out}${_err}")
endif()
message(STATUS "${_err}")
file(REMOVE_RECURSE "${WORK_DIR}")
