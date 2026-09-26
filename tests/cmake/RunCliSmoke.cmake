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
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-width-rate-refused-rtl")
    # The same refusal for an RTL-SDR spec, whose DSP bandwidth the spec's
    # sixth field sets. It happens before the input looks for a device, so it
    # needs no dongle and no RTL-SDR support in the build: a run that went on
    # would enumerate devices (or report none, or report the input unsupported)
    # before any refusal, and on a host with a dongle would open the stream.
    set(_args
        --frontend
        none
        -fA
        -i
        rtl:0:851.375M:0:0:24
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
    set(_forbid_output_regex
        "radio stream|RTL Input:|device[(]s[)]|No supported devices|RTL-SDR input"
    )
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-default-low-rate")
    # Issue #525: an existing low-rate -fA command line keeps starting. With
    # no width given, the unset default is DSP-limited below a 20 kHz DSP rate
    # and never refused for it, so startup goes on to open the rtl_tcp stream
    # (and fails there only because nothing listens on port 1). The negative
    # control for nfm-width-rate-refused: the stream failure, not a refusal.
    set(_args
        --frontend
        none
        -fA
        -i
        rtltcp:127.0.0.1:1:851.375M:0:0:12
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex "Failed to open radio stream")
    set(_forbid_output_regex "does not fit|cannot be filtered")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-width-fits-low-rate")
    # An explicit width the 16 kHz DSP rate filters (12.5 kHz of its 13.2 kHz
    # maximum) passes the pre-open check the same way.
    set(_args
        --frontend
        none
        -fA
        -i
        rtltcp:127.0.0.1:1:851.375M:0:0:16
        --nfm-bandwidth-hz
        12500
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex "Failed to open radio stream")
    set(_forbid_output_regex "does not fit|cannot be filtered")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "am-width-rate-refused")
    # Issue #524: an AM width the rtl_tcp input's 16 kHz DSP bandwidth cannot
    # filter is refused before the device opens, with the validator's
    # actionable text, and startup fails. No server is needed: nothing listens
    # on port 1, and a run that went on to open the stream would print the
    # stream failure instead of the refusal.
    set(_args
        --frontend
        none
        -fM
        -i
        rtltcp:127.0.0.1:1:118.1M:0:0:16
        --am-bandwidth-hz
        20000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "AM bandwidth 20 kHz does not fit the 16 kHz DSP rate [(]the largest width it fits is 13[.]2 kHz[)]; set the RTL DSP bandwidth to 24 or 48 kHz"
    )
    set(_forbid_output_regex "radio stream")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "am-width-rate-refused-rtl")
    # The same refusal for an RTL-SDR spec, whose DSP bandwidth the spec's
    # sixth field sets. It happens before the input looks for a device, so it
    # needs no dongle and no RTL-SDR support in the build: a run that went on
    # would enumerate devices (or report none, or report the input unsupported)
    # before any refusal, and on a host with a dongle would open the stream.
    set(_args
        --frontend
        none
        -fM
        -i
        rtl:0:118.1M:22:0:16
        --am-bandwidth-hz
        20000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "AM bandwidth 20 kHz does not fit the 16 kHz DSP rate [(]the largest width it fits is 13[.]2 kHz[)]; set the RTL DSP bandwidth to 24 or 48 kHz"
    )
    set(_forbid_output_regex
        "radio stream|RTL Input:|device[(]s[)]|No supported devices|RTL-SDR input"
    )
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "am-default-rate-refused")
    # The unset AM default (6 kHz) is held to the rate like an explicit width:
    # a 6 kHz DSP bandwidth fits at most 4.2 kHz, so -fM alone is refused.
    set(_args
        --frontend
        none
        -fM
        -i
        rtltcp:127.0.0.1:1:118.1M:0:0:6
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "AM bandwidth 6 kHz does not fit the 6 kHz DSP rate [(]the largest width it fits is 4[.]2 kHz[)]; set the RTL DSP bandwidth to 8, 12, 16, 24 or 48 kHz"
    )
    set(_forbid_output_regex "radio stream")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "am-width-fits-low-rate")
    # An AM width the 16 kHz DSP rate filters (10 kHz of its 13.2 kHz maximum)
    # passes the pre-open check: startup goes on to open the rtl_tcp stream
    # and fails there only because nothing listens on port 1.
    set(_args
        --frontend
        none
        -fM
        -i
        rtltcp:127.0.0.1:1:118.1M:0:0:16
        --am-bandwidth-hz
        10000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex "Failed to open radio stream")
    set(_forbid_output_regex "does not fit|cannot be filtered")
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
