# SPDX-License-Identifier: GPL-3.0-or-later
#
# DECODE_IQ_M17_VOICE_OUTPUT (issue #658): replays the M17 fixture with its decoded Codec2 voice on stdout (-o -) in
# each output format and checks that the voice follows the stream's format, as every other mode's does:
#   - -fz (s16 mono) carries whole 160-sample frames, and at least one;
#   - -fz -y (f32 mono) is twice its bytes, -fa (s16 stereo) twice, -fa -y (f32 stereo) four times; AUTO starts on M17's
#     own profile, so -fa decodes the frames -fz does;
#   - the fixed gain changes it: -g 1 and -g 50 differ.
# Each run must exit 0, show EXPECTED, and leave no sanitizer report.
#
# Expected -D inputs: DSD_BIN, FIXTURE, EXPECTED, WORK_DIR.
foreach(_var DSD_BIN FIXTURE EXPECTED WORK_DIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "iq_m17_voice_output_check: missing -D${_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(_m17_output_fail text)
    file(REMOVE_RECURSE "${WORK_DIR}")
    message(FATAL_ERROR "iq_m17_voice_output_check: ${text}")
endfunction()

# Runs one format; sets <name>_SIZE and <name>_MD5 in the caller.
function(_m17_output_run name mode)
    separate_arguments(_mode_args UNIX_COMMAND "${mode}")
    set(_pcm "${WORK_DIR}/${name}.pcm")
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${FIXTURE}"
            -o -
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_FILE "${_pcm}"
        ERROR_VARIABLE _err
        TIMEOUT 110
    )
    if(
        "${_err}"
            MATCHES
            "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        _m17_output_fail("${mode}: sanitizer report detected:\n${_err}")
    endif()
    if(NOT "${_rc}" STREQUAL "0")
        _m17_output_fail("${mode}: dsd-neo exited with '${_rc}' (expected 0)\n${_err}")
    endif()
    if(NOT "${_err}" MATCHES "${EXPECTED}")
        _m17_output_fail("${mode}: expected payload /${EXPECTED}/ not found\n${_err}")
    endif()
    file(SIZE "${_pcm}" _size)
    file(MD5 "${_pcm}" _md5)
    set(${name}_SIZE "${_size}" PARENT_SCOPE)
    set(${name}_MD5 "${_md5}" PARENT_SCOPE)
endfunction()

_m17_output_run(s16_mono "-fz")
_m17_output_run(f32_mono "-fz -y")
_m17_output_run(s16_stereo "-fa")
_m17_output_run(f32_stereo "-fa -y")
_m17_output_run(gain_1 "-fz -g 1")
_m17_output_run(gain_50 "-fz -g 50")

math(EXPR _frame_rem "${s16_mono_SIZE} % 320")
if(s16_mono_SIZE LESS 320 OR NOT _frame_rem EQUAL 0)
    _m17_output_fail(
        "-fz wrote ${s16_mono_SIZE} bytes; expected whole 160-sample s16 frames, at least one"
    )
endif()
math(EXPR _want_f32_mono "${s16_mono_SIZE} * 2")
math(EXPR _want_s16_stereo "${s16_mono_SIZE} * 2")
math(EXPR _want_f32_stereo "${s16_mono_SIZE} * 4")
if(NOT f32_mono_SIZE EQUAL _want_f32_mono)
    _m17_output_fail(
        "-fz -y wrote ${f32_mono_SIZE} bytes; expected ${_want_f32_mono} (f32 mono)"
    )
endif()
if(NOT s16_stereo_SIZE EQUAL _want_s16_stereo)
    _m17_output_fail(
        "-fa wrote ${s16_stereo_SIZE} bytes; expected ${_want_s16_stereo} (s16 stereo)"
    )
endif()
if(NOT f32_stereo_SIZE EQUAL _want_f32_stereo)
    _m17_output_fail(
        "-fa -y wrote ${f32_stereo_SIZE} bytes; expected ${_want_f32_stereo} (f32 stereo)"
    )
endif()
if(
    NOT gain_1_SIZE EQUAL s16_mono_SIZE
    OR NOT gain_50_SIZE EQUAL s16_mono_SIZE
    OR gain_1_MD5 STREQUAL gain_50_MD5
)
    _m17_output_fail(
        "-g 1 and -g 50 must write -fz's ${s16_mono_SIZE} bytes, differently: got ${gain_1_SIZE} (${gain_1_MD5}) and ${gain_50_SIZE} (${gain_50_MD5})"
    )
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
