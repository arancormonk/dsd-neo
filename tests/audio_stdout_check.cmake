# SPDX-License-Identifier: GPL-3.0-or-later
#
# The -o - audio output end to end (issue #659), on an I/Q fixture replay.
#
#   CLOSED_PIPE  A reader that goes away (`| head -c 1000`; here `| true`, which leaves before the first block, so the
#                case never depends on how much a pipe buffers) costs stdout audio one logged error, never the
#                process: dsd-neo exits 0, logs exactly one stdout failure, and its -w WAV matches a full run's.
#   MONITOR      The analog monitor (-fA, -fM) writes its 48 kHz mono s16le to stdout: at least MIN_BYTES, whole
#                samples.
#   MONITOR_8    The -8 source monitor during digital decoding is not written to stdout: it warns once at startup and
#                stdout carries what the same run without -8 writes.
#
# Expected -D inputs: DSD_BIN, CHECK, MODE, FIXTURE, WORK_DIR; MIN_BYTES for MONITOR.
foreach(
    _var
    DSD_BIN
    CHECK
    MODE
    FIXTURE
    WORK_DIR
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "audio_stdout_check: missing -D${_var}")
    endif()
endforeach()

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(_sanitizer_regex
    "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
)

function(_audio_stdout_require_clean label rc err)
    if("${err}" MATCHES "${_sanitizer_regex}")
        message(
            FATAL_ERROR
            "audio_stdout_check: ${label}: sanitizer report:\n${err}"
        )
    endif()
    if(NOT "${rc}" STREQUAL "0")
        message(
            FATAL_ERROR
            "audio_stdout_check: ${label}: dsd-neo exited with '${rc}' (expected 0)\n${err}"
        )
    endif()
endfunction()

# Count the matches of a regex in a string.
function(_audio_stdout_count out_var regex text)
    string(REGEX MATCHALL "${regex}" _hits "${text}")
    list(LENGTH _hits _count)
    set(${out_var} ${_count} PARENT_SCOPE)
endfunction()

set(_stdout_failure "Audio output \\(stdout\\): write failed")

if(CHECK STREQUAL "CLOSED_PIPE")
    find_program(_true true REQUIRED)
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${FIXTURE}"
            -o - -w "${WORK_DIR}/full.wav"
        OUTPUT_FILE "${WORK_DIR}/full.raw"
        ERROR_VARIABLE _full_err
        RESULT_VARIABLE _full_rc
        TIMEOUT 110
    )
    _audio_stdout_require_clean("full run" "${_full_rc}" "${_full_err}")
    file(SIZE "${WORK_DIR}/full.raw" _full_bytes)
    if(_full_bytes LESS 4000)
        message(
            FATAL_ERROR
            "audio_stdout_check: the full run wrote ${_full_bytes} bytes; the pipe case needs audio to write"
        )
    endif()

    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${FIXTURE}"
            -o - -w "${WORK_DIR}/pipe.wav"
        COMMAND "${_true}"
        OUTPUT_QUIET
        ERROR_VARIABLE _pipe_err
        RESULTS_VARIABLE _pipe_rcs
        TIMEOUT 110
    )
    list(GET _pipe_rcs 0 _pipe_rc)
    _audio_stdout_require_clean("closed pipe" "${_pipe_rc}" "${_pipe_err}")
    _audio_stdout_count(_failures "${_stdout_failure}" "${_pipe_err}")
    if(NOT _failures EQUAL 1)
        message(
            FATAL_ERROR
            "audio_stdout_check: closed pipe: ${_failures} stdout failure lines, want exactly 1\n${_pipe_err}"
        )
    endif()

    file(SHA256 "${WORK_DIR}/full.wav" _full_wav)
    file(SHA256 "${WORK_DIR}/pipe.wav" _pipe_wav)
    file(SIZE "${WORK_DIR}/full.wav" _full_wav_bytes)
    file(SIZE "${WORK_DIR}/pipe.wav" _pipe_wav_bytes)
    if(NOT _full_wav STREQUAL _pipe_wav)
        message(
            FATAL_ERROR
            "audio_stdout_check: closed pipe: the -w WAV (${_pipe_wav_bytes} bytes) differs from the full run's (${_full_wav_bytes} bytes)"
        )
    endif()
elseif(CHECK STREQUAL "MONITOR")
    if(NOT DEFINED MIN_BYTES)
        message(FATAL_ERROR "audio_stdout_check: MONITOR needs -DMIN_BYTES")
    endif()
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${FIXTURE}"
            -o -
        OUTPUT_FILE "${WORK_DIR}/monitor.raw"
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc
        TIMEOUT 110
    )
    _audio_stdout_require_clean("monitor" "${_rc}" "${_err}")
    file(SIZE "${WORK_DIR}/monitor.raw" _bytes)
    math(EXPR _odd "${_bytes} % 2")
    if(_bytes LESS MIN_BYTES OR NOT _odd EQUAL 0)
        message(
            FATAL_ERROR
            "audio_stdout_check: monitor: stdout carried ${_bytes} bytes, want at least ${MIN_BYTES} in whole s16 samples\n${_err}"
        )
    endif()
elseif(CHECK STREQUAL "MONITOR_8")
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${FIXTURE}"
            -o -
        OUTPUT_FILE "${WORK_DIR}/digital.raw"
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc
        TIMEOUT 110
    )
    _audio_stdout_require_clean("digital" "${_rc}" "${_err}")
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_mode_args} -8 --iq-replay
            "${FIXTURE}" -o -
        OUTPUT_FILE "${WORK_DIR}/digital_8.raw"
        ERROR_VARIABLE _err8
        RESULT_VARIABLE _rc8
        TIMEOUT 110
    )
    _audio_stdout_require_clean("digital -8" "${_rc8}" "${_err8}")
    _audio_stdout_count(_warnings "-8 source monitoring is not written to stdout" "${_err8}")
    if(NOT _warnings EQUAL 1)
        message(
            FATAL_ERROR
            "audio_stdout_check: -8: ${_warnings} warnings, want exactly 1\n${_err8}"
        )
    endif()
    file(SHA256 "${WORK_DIR}/digital.raw" _digital)
    file(SHA256 "${WORK_DIR}/digital_8.raw" _digital_8)
    if(NOT _digital STREQUAL _digital_8)
        file(SIZE "${WORK_DIR}/digital.raw" _a)
        file(SIZE "${WORK_DIR}/digital_8.raw" _b)
        message(
            FATAL_ERROR
            "audio_stdout_check: -8 changed what stdout carries (${_a} bytes without, ${_b} with)"
        )
    endif()
else()
    message(FATAL_ERROR "audio_stdout_check: unknown CHECK '${CHECK}'")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
