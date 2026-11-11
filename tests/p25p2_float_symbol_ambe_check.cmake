# SPDX-License-Identifier: GPL-3.0-or-later
#
# DECODE_P25P2_FLOAT_SYMBOL_AMBE_ERRORS: a P25 Phase 2 voice channel replayed from a float symbol file counts its AMBE
# errors. A P2 sync puts the session on QPSK (rf_mod 1), whose fixed CQPSK reliability levels a float symbol file's
# symbols (scaled by 10000 on the way in) are nowhere near; measured there, every reliability read 0, so mbelib-neo's
# reliability-weighted error counts read 0 on every frame.
#
# The generator (CONVERT_BIN, dsd-neo_test_p25p2_voice_symbol_file) writes, in <WORK_DIR>, symbols.raw and capture.bin:
# the same dibits as a float symbol file and as a soft symbol capture at full confidence. Replayed with -f2 -X and a
# frame log, the two must decode the same number of AMBE frames, at least MIN_FRAMES, and the float replay must count
# errors in exactly the frames the full-confidence replay does (the frames' voice bits are random, so Golay finds
# errors in most). A soft capture (-c) the float replay writes must then replay to the same AMBE frames, counts
# included: the capture records the reliabilities the replay measured.
#
# Expected -D inputs: DSD_BIN, CONVERT_BIN, WORK_DIR, MIN_FRAMES.
foreach(_var DSD_BIN CONVERT_BIN WORK_DIR MIN_FRAMES)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "p25p2_float_symbol_ambe_check: missing -D${_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Every failure removes the work folder first: a test leaves no files behind, passing or not.
function(_ambe_fail text)
    file(REMOVE_RECURSE "${WORK_DIR}")
    message(FATAL_ERROR "p25p2_float_symbol_ambe_check: ${text}")
endfunction()

execute_process(
    COMMAND "${CONVERT_BIN}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT 60
)
if(NOT "${_rc}" STREQUAL "0")
    _ambe_fail("generator exited with '${_rc}'\n${_out}\n${_err}")
endif()

# Replays <input> with a frame log; <out_var> gets its AMBE frame lines without their timestamps.
function(_ambe_replay leg input extra out_var)
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none -f2 -X BEE00164161 -i "${input}"
            --frame-log "${leg}.frames" -o null ${extra}
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
        _ambe_fail("${leg}: sanitizer report detected:\n${_all}")
    endif()
    if(NOT "${_rc}" STREQUAL "0")
        _ambe_fail("${leg}: exited with '${_rc}' (expected 0)\n${_all}")
    endif()
    file(
        STRINGS "${WORK_DIR}/${leg}.frames"
        _lines
        REGEX "FRAME AMBE slot=[12] data=[0-9A-F]+ err="
    )
    set(_frames "")
    foreach(_line IN LISTS _lines)
        string(REGEX MATCH "FRAME AMBE .*" _frame "${_line}")
        # CMake does not split a list at a semicolon between square brackets: the counts' brackets become parentheses.
        string(REPLACE "[" "(" _frame "${_frame}")
        string(REPLACE "]" ")" _frame "${_frame}")
        list(APPEND _frames "${_frame}")
    endforeach()
    list(LENGTH _frames _count)
    message(
        STATUS
        "p25p2_float_symbol_ambe_check: ${leg}: ${_count} AMBE frames"
    )
    set(${out_var} "${_frames}" PARENT_SCOPE)
endfunction()

# 1 for each frame with an error counted, 0 for each without.
function(_ambe_error_flags frames out_var)
    set(_flags "")
    foreach(_frame IN LISTS frames)
        if("${_frame}" MATCHES "err=\\(0\\) \\(0\\)$")
            string(APPEND _flags "0")
        else()
            string(APPEND _flags "1")
        endif()
    endforeach()
    set(${out_var} "${_flags}" PARENT_SCOPE)
endfunction()

_ambe_replay(float symbols.raw "-c;float_capture.bin" _float)
_ambe_replay(full capture.bin "" _full)

list(LENGTH _float _float_count)
list(LENGTH _full _full_count)
if(_full_count LESS MIN_FRAMES)
    _ambe_fail(
        "the full-confidence replay decoded ${_full_count} AMBE frames, expected at least ${MIN_FRAMES}"
    )
endif()
if(NOT _float_count EQUAL _full_count)
    _ambe_fail(
        "the float symbol replay decoded ${_float_count} AMBE frames, the full-confidence replay ${_full_count}"
    )
endif()
_ambe_error_flags("${_float}" _float_flags)
_ambe_error_flags("${_full}" _full_flags)
string(REGEX MATCHALL "1" _full_errored "${_full_flags}")
list(LENGTH _full_errored _full_errored_count)
if(_full_errored_count LESS MIN_FRAMES)
    _ambe_fail(
        "the full-confidence replay counted errors in ${_full_errored_count} frames, expected at least ${MIN_FRAMES}"
    )
endif()
if(NOT "${_float_flags}" STREQUAL "${_full_flags}")
    string(REGEX MATCHALL "1" _float_errored "${_float_flags}")
    list(LENGTH _float_errored _float_errored_count)
    _ambe_fail(
        "the float symbol replay counted errors in ${_float_errored_count} frames, not the ${_full_errored_count} the full-confidence replay did:\n${_float_flags}\n${_full_flags}"
    )
endif()

_ambe_replay(recaptured float_capture.bin "" _recaptured)
if(NOT "${_recaptured}" STREQUAL "${_float}")
    _ambe_fail(
        "the float symbol replay's own capture replayed to other AMBE frames:\n${_recaptured}\n--- the float replay:\n${_float}"
    )
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
