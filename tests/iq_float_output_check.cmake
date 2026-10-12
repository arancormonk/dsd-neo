# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DECODE_IQ_FLOAT_OUTPUT_* case (issue #652): replays an I/Q fixture through dsd-neo twice, once with 16-bit
# output and once with float output (-y), each writing the digital voice stream to stdout (-o -) and a static WAV
# (-w), and hands both to the comparator (tests/core/float_output_compare.c). Float output must be the 16-bit output
# written as f32 (every sample s16 / 32768, so none is NaN or past full scale) and record the same static WAV. As
# iq_decode_check.cmake does, each run must exit 0, print the expected payload and raise no sanitizer report.
#
# Expected -D inputs: DSD_BIN, COMPARE_BIN, MODE, FIXTURE, EXPECTED, WORK_DIR, CHANNELS.
# Optional: BOTH_EARS (true: the stereo stream carries the same audio in both channels), SILENT (true: neither run
# plays anything, and the static WAVs are empty or contain only silence and still match), SINK ("null": both runs use -o null, so both streams are empty and the static WAVs are compared).
foreach(
    _var
    DSD_BIN
    COMPARE_BIN
    MODE
    FIXTURE
    EXPECTED
    WORK_DIR
    CHANNELS
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "iq_float_output_check: missing -D${_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Every failure removes the work folder first: a test leaves no files behind, passing or not.
function(_float_fail text)
    file(REMOVE_RECURSE "${WORK_DIR}")
    message(FATAL_ERROR "iq_float_output_check: ${text}")
endfunction()

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")
set(_sink -)
if(DEFINED SINK AND "${SINK}" STREQUAL "null")
    set(_sink null)
endif()

foreach(_leg s16 f32)
    set(_format_args)
    if("${_leg}" STREQUAL "f32")
        set(_format_args -y)
    endif()
    # Each leg writes fresh files: the static WAV opens read-write.
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_mode_args} ${_format_args}
            --iq-replay "${FIXTURE}" -o ${_sink} -w "${WORK_DIR}/${_leg}.wav"
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_FILE "${WORK_DIR}/${_leg}.raw"
        ERROR_VARIABLE _err
        TIMEOUT 110
    )
    if(
        "${_err}"
            MATCHES
            "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        _float_fail("${_leg}: sanitizer report detected:\n${_err}")
    endif()
    if(NOT "${_rc}" STREQUAL "0")
        _float_fail("${_leg}: dsd-neo exited with '${_rc}' (expected 0)\n${_err}")
    endif()
    if(NOT "${_err}" MATCHES "${EXPECTED}")
        _float_fail("${_leg}: expected payload /${EXPECTED}/ not found in output\n${_err}")
    endif()
endforeach()

set(_compare_args)
if(BOTH_EARS)
    list(APPEND _compare_args --both-ears)
endif()
# A muted call may record silence; with -o null nothing reaches stdout, and only the static WAVs carry the audio.
if(SILENT)
    list(APPEND _compare_args --silent --silent-wav)
elseif("${_sink}" STREQUAL "null")
    list(APPEND _compare_args --silent)
endif()
execute_process(
    COMMAND "${COMPARE_BIN}" ${CHANNELS} ${_compare_args}
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _cmp_rc
    OUTPUT_VARIABLE _cmp_out
    ERROR_VARIABLE _cmp_err
)
if(NOT "${_cmp_rc}" STREQUAL "0")
    _float_fail("${_cmp_out}${_cmp_err}")
endif()
message(STATUS "${_cmp_err}")
file(REMOVE_RECURSE "${WORK_DIR}")
