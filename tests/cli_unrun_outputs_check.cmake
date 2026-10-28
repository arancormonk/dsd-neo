# SPDX-License-Identifier: GPL-3.0-or-later
#
# A dsd-neo run that stops before decoding leaves no per-call recording behind (#782 gate r2 H3). The -P option opens
# each slot's WAV under a TEMP_ name while the options are parsed; only the engine's cleanup closed it, and a recording
# with no audio is removed then. MODE names how the run stops before the engine: PARSE_ERROR (an unknown option after
# -P) or AIRSPY_LIST (--airspy-list, which lists and exits).
#
# Expected -D inputs: DSD_BIN, MODE, WORK_DIR.
foreach(_var DSD_BIN MODE WORK_DIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "cli_unrun_outputs_check: missing -D${_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/wav")

if(MODE STREQUAL "PARSE_ERROR")
    set(_args
        -i
        -
        -7
        "${WORK_DIR}/wav"
        -P
        --no-such-option
    )
    set(_want_rc_nonzero TRUE)
elseif(MODE STREQUAL "AIRSPY_LIST")
    set(_args
        -i
        -
        -7
        "${WORK_DIR}/wav"
        -P
        --airspy-list
    )
    set(_want_rc_nonzero FALSE)
else()
    message(FATAL_ERROR "cli_unrun_outputs_check: unknown MODE ${MODE}")
endif()

execute_process(
    COMMAND "${DSD_BIN}" ${_args}
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT 60
)
if(
    "${_err}"
        MATCHES
        "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
)
    message(
        FATAL_ERROR
        "cli_unrun_outputs_check ${MODE}: sanitizer report\n${_err}"
    )
endif()
if(NOT "${_rc}" MATCHES "^[0-9]+$")
    message(
        FATAL_ERROR
        "cli_unrun_outputs_check ${MODE}: dsd-neo did not exit normally: ${_rc}\n${_err}"
    )
endif()
if(_want_rc_nonzero AND _rc EQUAL 0)
    message(
        FATAL_ERROR
        "cli_unrun_outputs_check ${MODE}: the unknown option was accepted\n${_err}"
    )
endif()
if(NOT "${_err}${_out}" MATCHES "Per Call Wav File Enabled")
    message(
        FATAL_ERROR
        "cli_unrun_outputs_check ${MODE}: -P was not parsed before the run stopped\n${_err}"
    )
endif()

file(GLOB _left RELATIVE "${WORK_DIR}/wav" "${WORK_DIR}/wav/*")
if(_left)
    message(
        FATAL_ERROR
        "cli_unrun_outputs_check ${MODE}: recordings left behind: ${_left}"
    )
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
