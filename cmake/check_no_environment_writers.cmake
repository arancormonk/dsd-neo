# SPDX-License-Identifier: GPL-3.0-or-later
#
# Refuse a shared library that imports a process-environment writer (#782).
#
# On Android's bionic, adding an environment variable can reallocate and free the environ array another thread's
# getenv() is walking, and Qt's thread reads it all the time (every localtime_r() reads TZ). The app's native library
# therefore writes no environment once Qt's main() runs: overrides live in <dsd-neo/platform/env.h>. Semgrep keeps the
# writers out of our sources; this check also covers vendored and statically linked dependencies, by refusing a
# library whose dynamic symbol table imports one: libc's setenv, unsetenv, putenv or clearenv, or Qt's qputenv or
# qunsetenv.
#
# Usage:
#   cmake -DNM=<llvm-nm> -DLIBRARY=<lib.so> -P check_no_environment_writers.cmake
#   cmake -DNM_OUTPUT_FILE=<file> -P check_no_environment_writers.cmake   (the parser alone, for its tests)

set(_writers setenv unsetenv putenv clearenv)

if(DEFINED NM_OUTPUT_FILE)
    file(READ "${NM_OUTPUT_FILE}" _symbols)
    set(_what "${NM_OUTPUT_FILE}")
else()
    if(NOT NM OR NOT LIBRARY)
        message(FATAL_ERROR "check_no_environment_writers: set NM and LIBRARY")
    endif()
    execute_process(
        COMMAND "${NM}" -D --undefined-only "${LIBRARY}"
        OUTPUT_VARIABLE _symbols
        ERROR_VARIABLE _nm_error
        RESULT_VARIABLE _nm_rc
    )
    if(NOT _nm_rc EQUAL 0)
        message(
            FATAL_ERROR
            "check_no_environment_writers: ${NM} failed on ${LIBRARY}: ${_nm_error}"
        )
    endif()
    set(_what "${LIBRARY}")
endif()

# One symbol per line, "U name" or "U name@VERSION" (llvm-nm), possibly with a leading address column.
string(REPLACE "\n" ";" _lines "${_symbols}")
set(_found "")
foreach(_line IN LISTS _lines)
    if(
        _line
            MATCHES
            "^[ \t0-9a-fA-F]*[Uw][ \t]+([A-Za-z_][A-Za-z0-9_]*)(@.*)?[ \t]*$"
    )
        set(_symbol "${CMAKE_MATCH_1}")
        list(FIND _writers "${_symbol}" _index)
        if(NOT _index EQUAL -1)
            list(APPEND _found "${_symbol}")
        elseif(_symbol MATCHES "^_Z7qputenv")
            # Qt's writers, imported from libQt6Core under their mangled names (qputenv(const char*, QByteArrayView)).
            list(APPEND _found "qputenv")
        elseif(_symbol MATCHES "^_Z9qunsetenv")
            list(APPEND _found "qunsetenv")
        endif()
    endif()
endforeach()

if(_found)
    list(REMOVE_DUPLICATES _found)
    list(JOIN _found ", " _names)
    # One unwrapped line for tooling and the host tests; the error below is reflowed.
    list(JOIN _found "," _compact)
    message(NOTICE "environment-writers=${_compact}")
    message(
        FATAL_ERROR
        "${_what} imports ${_names}: nothing in the app may write the process environment once Qt's main() runs "
        "(#782). Set an override with dsd_env_set() (<dsd-neo/platform/env.h>) instead."
    )
endif()
