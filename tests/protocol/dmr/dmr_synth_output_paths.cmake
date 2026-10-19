# SPDX-License-Identifier: GPL-3.0-or-later
# Exercise the generator's actual file opens on both output options.
foreach(_var GEN_BIN WORK_DIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "dmr_synth_output_paths: missing -D${_var}")
    endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(run_generator case wav_name expect_name expected_error)
    file(MAKE_DIRECTORY "${WORK_DIR}/${case}")
    execute_process(
        COMMAND
            "${GEN_BIN}" -t 1 --lead 0 --tail 0 -o "${wav_name}" -e
            "${expect_name}"
        WORKING_DIRECTORY "${WORK_DIR}/${case}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        TIMEOUT 15
    )
    if("${expected_error}" STREQUAL "")
        if(NOT "${_rc}" STREQUAL "0")
            message(
                FATAL_ERROR
                "${case}: generator failed: ${_rc}\n${_out}\n${_err}"
            )
        endif()
    elseif(NOT "${_rc}" STREQUAL "1" OR NOT _err MATCHES "${expected_error}")
        message(
            FATAL_ERROR
            "${case}: expected '${expected_error}', got ${_rc}\n${_out}\n${_err}"
        )
    endif()
    if(
        "${_out}\n${_err}"
            MATCHES
            "AddressSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        message(FATAL_ERROR "${case}: sanitizer report:\n${_out}\n${_err}")
    endif()
endfunction()

set(_bad_name_error "output names must be single filenames")
file(WRITE "${WORK_DIR}/escape.txt" "outside sentinel")
set(_bad_names
    "../escape.txt"
    "..\\escape.txt"
    "sub/escape.txt"
    "sub\\escape.txt"
    "${WORK_DIR}/escape.txt"
    "/escape.txt"
    "\\escape.txt"
    "C:escape.txt"
    "C:/escape.txt"
    "C:\\escape.txt"
    "\\\\server\\share\\escape.txt"
    "escape.txt:stream"
    "."
    ".."
)
set(_i 0)
foreach(_name IN LISTS _bad_names)
    math(EXPR _i "${_i} + 1")
    run_generator("bad_wav_${_i}" "${_name}" expected.txt "${_bad_name_error}")
    run_generator("bad_expect_${_i}" signal.wav "${_name}" "${_bad_name_error}")
endforeach()
run_generator(empty_wav "" expected.txt "${_bad_name_error}")
run_generator(empty_expect signal.wav "" "${_bad_name_error}")
file(READ "${WORK_DIR}/escape.txt" _sentinel)
if(NOT _sentinel STREQUAL "outside sentinel")
    message(
        FATAL_ERROR
        "a rejected output path overwrote a file outside the working directory"
    )
endif()

# Ordinary names, including spaces, still produce both artifacts in the chosen directory.
run_generator(valid "signal sample.wav" "expected sample.txt" "")
file(READ "${WORK_DIR}/valid/signal sample.wav" _riff LIMIT 4 HEX)
file(READ "${WORK_DIR}/valid/expected sample.txt" _expect)
if(
    NOT _riff STREQUAL "52494646"
    OR NOT _expect MATCHES "# dmr_synth expectation v1"
)
    message(
        FATAL_ERROR
        "valid output names did not produce a WAV and its expectations"
    )
endif()

# Exclusive creation prevents overwriting regular files or following links out of the directory.
set(_exists_error "cannot create output file")
foreach(_option wav expect)
    set(_case "existing_${_option}")
    file(MAKE_DIRECTORY "${WORK_DIR}/${_case}")
    file(WRITE "${WORK_DIR}/${_case}/keep.txt" "existing sentinel")
    if(_option STREQUAL "wav")
        run_generator("${_case}" keep.txt expected.txt "${_exists_error}")
    else()
        run_generator("${_case}" signal.wav keep.txt "${_exists_error}")
    endif()
    file(READ "${WORK_DIR}/${_case}/keep.txt" _existing)
    if(NOT _existing STREQUAL "existing sentinel")
        message(FATAL_ERROR "${_case}: overwrote an existing file")
    endif()

    foreach(_kind existing dangling)
        set(_case "link_${_option}_${_kind}")
        file(MAKE_DIRECTORY "${WORK_DIR}/${_case}")
        if(_kind STREQUAL "existing")
            set(_target "${WORK_DIR}/escape.txt")
        else()
            set(_target "${WORK_DIR}/missing_${_option}.txt")
        endif()
        file(
            CREATE_LINK "${_target}" "${WORK_DIR}/${_case}/link.txt"
            SYMBOLIC
            RESULT _link_result
        )
        if("${_link_result}" STREQUAL "0")
            if(_option STREQUAL "wav")
                run_generator("${_case}" link.txt expected.txt "${_exists_error}")
            else()
                run_generator("${_case}" signal.wav link.txt "${_exists_error}")
            endif()
            if(_kind STREQUAL "dangling" AND EXISTS "${_target}")
                message(FATAL_ERROR "${_case}: followed a dangling symlink")
            endif()
        else()
            message(STATUS "${_case}: symlinks unavailable: ${_link_result}")
        endif()
    endforeach()
endforeach()
file(READ "${WORK_DIR}/escape.txt" _sentinel)
if(NOT _sentinel STREQUAL "outside sentinel")
    message(
        FATAL_ERROR
        "an output symlink overwrote a file outside the working directory"
    )
endif()
