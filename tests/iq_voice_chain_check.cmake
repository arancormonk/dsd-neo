# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DECODE_IQ_VOICE_CHAIN_* case (issue #653): replays an I/Q fixture through dsd-neo with per-call WAVs (-P)
# under integer output to stdout, float output (-y) to stdout and the null output, and checks that the digital voice
# chain gave all of them the same audio:
#
#   - each run leaves at least one per-call WAV, and the three runs leave byte-identical WAVs (their names carry a
#     random nonce, so the files are compared as a sorted list of content hashes);
#   - float output carries as many samples as integer output, and turning the digital high-pass off (-v 0x1) changes
#     what float output plays;
#   - with PLAYBACK_MATCH, a mono preset's single call: the integer stdout is the per-call WAV's sample data.
#
# As iq_decode_check.cmake does, every run must exit 0, print the expected payload regex and report no sanitizer error,
# so a run that never decoded cannot pass.
#
# Expected -D inputs: DSD_BIN, MODE, FIXTURE, EXPECTED, WORK_DIR. Optional: PLAYBACK_MATCH (ON/OFF).
foreach(
    _var
    DSD_BIN
    MODE
    FIXTURE
    EXPECTED
    WORK_DIR
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "iq_voice_chain_check: missing -D${_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Every failure removes the work folder first: a test leaves no files behind, passing or not.
function(_chain_fail text)
    file(REMOVE_RECURSE "${WORK_DIR}")
    message(FATAL_ERROR "iq_voice_chain_check: ${text}")
endfunction()

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")

# One replay: @p name names its folder; @p ARGN are the output options. Its stdout lands in <name>.raw.
function(_chain_run name)
    file(MAKE_DIRECTORY "${WORK_DIR}/${name}")
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_mode_args} ${ARGN} --iq-replay
            "${FIXTURE}"
        WORKING_DIRECTORY "${WORK_DIR}/${name}"
        RESULT_VARIABLE _rc
        OUTPUT_FILE "${WORK_DIR}/${name}.raw"
        ERROR_VARIABLE _err
        TIMEOUT 110
    )
    if(
        "${_err}"
            MATCHES
            "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        _chain_fail("${name}: sanitizer report detected:\n${_err}")
    endif()
    if(NOT "${_rc}" STREQUAL "0")
        _chain_fail("${name}: dsd-neo exited with '${_rc}' (expected 0)\n${_err}")
    endif()
    if(NOT "${_err}" MATCHES "${EXPECTED}")
        _chain_fail(
            "${name}: expected payload /${EXPECTED}/ not found in output\n${_err}"
        )
    endif()
endfunction()

# The sorted content hashes of the per-call WAVs a run left, in @p out_var.
function(_chain_wav_hashes name out_var)
    file(GLOB _wavs LIST_DIRECTORIES false "${WORK_DIR}/${name}/calls/*.wav")
    set(_hashes "")
    foreach(_wav IN LISTS _wavs)
        file(SHA256 "${_wav}" _hash)
        list(APPEND _hashes "${_hash}")
    endforeach()
    list(SORT _hashes)
    set(${out_var} "${_hashes}" PARENT_SCOPE)
endfunction()

_chain_run(short -o - -7 ./calls -P)
_chain_run(float -y -o - -7 ./calls -P)
_chain_run(null -o null -7 ./calls -P)
_chain_run(float_nohpf -y -v 0x1 -o -)

_chain_wav_hashes(short _short_wavs)
_chain_wav_hashes(float _float_wavs)
_chain_wav_hashes(null _null_wavs)
if("${_short_wavs}" STREQUAL "")
    _chain_fail("the integer run left no per-call WAV")
endif()
if(NOT "${_float_wavs}" STREQUAL "${_short_wavs}")
    _chain_fail(
        "float output's per-call WAVs differ from integer output's:\n  integer ${_short_wavs}\n  float   ${_float_wavs}"
    )
endif()
if(NOT "${_null_wavs}" STREQUAL "${_short_wavs}")
    _chain_fail(
        "-o null's per-call WAVs differ from integer output's:\n  integer ${_short_wavs}\n  null    ${_null_wavs}"
    )
endif()

file(SIZE "${WORK_DIR}/short.raw" _short_bytes)
file(SIZE "${WORK_DIR}/float.raw" _float_bytes)
math(EXPR _float_want "${_short_bytes} * 2")
if(_short_bytes EQUAL 0 OR NOT _float_bytes EQUAL _float_want)
    _chain_fail(
        "float output wrote ${_float_bytes} bytes for integer output's ${_short_bytes}: not the same samples"
    )
endif()
file(SHA256 "${WORK_DIR}/float.raw" _float_hash)
file(SHA256 "${WORK_DIR}/float_nohpf.raw" _float_nohpf_hash)
if("${_float_hash}" STREQUAL "${_float_nohpf_hash}")
    _chain_fail("the digital high-pass (-v 0x8) has no effect on float output")
endif()

if(PLAYBACK_MATCH)
    file(GLOB _wavs LIST_DIRECTORIES false "${WORK_DIR}/short/calls/*.wav")
    list(LENGTH _wavs _wav_count)
    if(NOT _wav_count EQUAL 1)
        _chain_fail("PLAYBACK_MATCH needs one call; the run left ${_wav_count}")
    endif()
    # Hex, so binary bytes survive; the canonical WAV header is 44 bytes.
    file(READ "${_wavs}" _wav_hex OFFSET 44 HEX)
    file(READ "${WORK_DIR}/short.raw" _played_hex HEX)
    if(NOT "${_wav_hex}" STREQUAL "${_played_hex}")
        _chain_fail("the per-call WAV does not hold what integer output played")
    endif()
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
