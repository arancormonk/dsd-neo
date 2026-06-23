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
    set(_want_stderr_regex "invalid option")
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
)

set(_combined "${_stdout}\n${_stderr}")

if(NOT _rc EQUAL _want_rc)
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} rc=${_want_rc}, got ${_rc}\n${_combined}"
    )
endif()

if(NOT _stdout MATCHES "${_want_stdout_regex}")
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
