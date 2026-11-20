# SPDX-License-Identifier: GPL-3.0-or-later
#
# A temp folder of its own for every test, checked when the test is done.
#
# dsd_neo_add_test_tmpdirs() gives each test registered so far in the calling
# directory the folder ${PROJECT_BINARY_DIR}/test-tmp/<TEST> through TMPDIR,
# TEMP, TMP and DSD_NEO_TEST_TMPDIR (the C helpers, Python's tempfile, Qt, pwsh
# and the JVM runner each read one of them), and two helper tests:
#
#   TMPDIR_SETUP__<TEST>  empties or makes the folder before the test;
#   TMPDIR_CHECK__<TEST>  after it, fails naming whatever the test left there,
#                         and removes the folder.
#
# They are tied to the test by fixtures, so `ctest -R`, `-L` and
# `--rerun-failed` bring them along: the setup and check are the test's own
# fixture's, and the test is the setup of a second fixture the check requires,
# so a rerun of a failed check runs the test again rather than looking at an old
# folder. Each test has its own folder, so two ctest runs in one build tree that
# run different tests leave each other's folders alone. A test that fails leaves
# its check Not Run; the folder is emptied by the test's next setup.
#
# Call it last in each directory that registers tests.

function(dsd_neo_add_test_tmpdirs)
    set(_guard "${PROJECT_SOURCE_DIR}/tests/cmake/TestTmpdirGuard.cmake")
    get_property(_tests DIRECTORY PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        if(_test MATCHES "^TMPDIR_(SETUP|CHECK)__")
            continue()
        endif()
        get_test_property(${_test} DISABLED _disabled)
        if(_disabled)
            continue()
        endif()
        set(_dir "${PROJECT_BINARY_DIR}/test-tmp/${_test}")
        # The value goes to the test as it is, with no shell to undo escapes:
        # cmake_path() gives the native separators and nothing else.
        cmake_path(NATIVE_PATH _dir _native)
        set_property(
            TEST ${_test}
            APPEND
            PROPERTY
                ENVIRONMENT
                    "TMPDIR=${_native}"
                    "TEMP=${_native}"
                    "TMP=${_native}"
                    "DSD_NEO_TEST_TMPDIR=${_native}"
        )
        set_property(
            TEST ${_test}
            APPEND
            PROPERTY FIXTURES_REQUIRED "tmpdir:${_test}"
        )
        set_property(
            TEST ${_test}
            APPEND
            PROPERTY FIXTURES_SETUP "tmpdir-check:${_test}"
        )
        add_test(
            NAME TMPDIR_SETUP__${_test}
            COMMAND ${CMAKE_COMMAND} -DMODE=setup "-DDIR=${_dir}" -P "${_guard}"
        )
        add_test(
            NAME TMPDIR_CHECK__${_test}
            COMMAND
                ${CMAKE_COMMAND} -DMODE=check "-DDIR=${_dir}"
                "-DTEST_NAME=${_test}" -P "${_guard}"
        )
        set_tests_properties(
            TMPDIR_SETUP__${_test}
            PROPERTIES FIXTURES_SETUP "tmpdir:${_test}"
        )
        set_tests_properties(
            TMPDIR_CHECK__${_test}
            PROPERTIES
                FIXTURES_CLEANUP "tmpdir:${_test}"
                FIXTURES_REQUIRED "tmpdir-check:${_test}"
        )
    endforeach()
endfunction()
