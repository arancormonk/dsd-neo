# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs dsd-neo once per named mode and checks the exit code with the output
# together: CTest's PASS_REGULAR_EXPRESSION alone ignores the exit code.
#
# Expected -D inputs: DSD_NEO_CLI, DSD_NEO_CLI_SMOKE_MODE.

if(NOT DEFINED DSD_NEO_CLI)
    message(FATAL_ERROR "DSD_NEO_CLI is required")
endif()

if(NOT DEFINED DSD_NEO_CLI_SMOKE_MODE)
    message(FATAL_ERROR "DSD_NEO_CLI_SMOKE_MODE is required")
endif()

if(DSD_NEO_CLI_SMOKE_MODE STREQUAL "help")
    set(_args "-h")
    set(_want_rc 0)
    set(_want_stdout_regex "Usage: dsd-neo \\[options\\].*Decoder options:")
    set(_want_stderr_regex "")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "invalid-option")
    set(_args "--definitely-not-an-option")
    set(_want_rc 1)
    set(_want_stdout_regex "Usage: dsd-neo \\[options\\]")
    # glibc getopt reports "invalid option", musl reports "unrecognized option".
    set(_want_stderr_regex "invalid option|unrecognized option")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-width-rate-refused")
    # Issue #525: an explicit NFM width the rtl_tcp input's 24 kHz DSP
    # bandwidth cannot filter is refused before the device opens, with the
    # validator's actionable text, and startup fails. No server is needed:
    # nothing listens on port 1, and a run that went on to open the stream
    # would print the stream failure instead of the refusal.
    set(_args
        --frontend
        none
        -fA
        -i
        rtltcp:127.0.0.1:1:851.375M:0:0:24
        --nfm-bandwidth-hz
        25000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate [(]the largest width it fits is 20[.]4 kHz[)]; set the RTL DSP bandwidth to 48 kHz"
    )
    set(_forbid_output_regex "radio stream")
else()
    message(
        FATAL_ERROR
        "unknown DSD_NEO_CLI_SMOKE_MODE: ${DSD_NEO_CLI_SMOKE_MODE}"
    )
endif()

execute_process(
    COMMAND "${DSD_NEO_CLI}" ${_args}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 25
)

set(_combined "${_stdout}\n${_stderr}")

if(NOT _rc EQUAL _want_rc)
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} rc=${_want_rc}, got ${_rc}\n${_combined}"
    )
endif()

if(_want_stdout_regex AND NOT _stdout MATCHES "${_want_stdout_regex}")
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} stdout to match ${_want_stdout_regex}\n${_combined}"
    )
endif()

if(_want_stderr_regex AND NOT _stderr MATCHES "${_want_stderr_regex}")
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} stderr to match ${_want_stderr_regex}\n${_combined}"
    )
endif()

if(_want_output_regex AND NOT _combined MATCHES "${_want_output_regex}")
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} output to match ${_want_output_regex}\n${_combined}"
    )
endif()

if(_forbid_output_regex AND _combined MATCHES "${_forbid_output_regex}")
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} output not to match ${_forbid_output_regex}\n${_combined}"
    )
endif()
