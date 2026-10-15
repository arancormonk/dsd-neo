# SPDX-License-Identifier: GPL-3.0-or-later
#
# dsd_test_count_regex_matches(<out_var> <regex> <text>) sets <out_var> to the number of non-overlapping matches of
# <regex> in <text>.
#
# string(REGEX MATCHALL) returns the matches as a list, and a list does not keep them apart: CMake splits a list at no
# semicolon inside square brackets, so matches holding one bracket of a pair (a payload line "[TETRA NDB ... AACH
# 2/1/9]" matched from "AACH") run together and count as one, and a match holding a semicolon counts as two. Each match
# is replaced with a marker that holds neither and that the text does not already hold, and the markers are counted;
# the newlines around each marker keep one from forming across the text next to it. Tests: REGEX_MATCH_COUNT.
function(dsd_test_count_regex_matches out_var regex text)
    set(_marker "<dsd-test-regex-match>")
    set(_tries 0)
    string(FIND "${text}" "${_marker}" _at)
    while(NOT _at EQUAL -1)
        math(EXPR _tries "${_tries} + 1")
        set(_marker "<dsd-test-regex-match-${_tries}>")
        string(FIND "${text}" "${_marker}" _at)
    endwhile()
    string(REGEX REPLACE "${regex}" "\n${_marker}\n" _marked "${text}")
    string(REGEX MATCHALL "${_marker}" _markers "${_marked}")
    list(LENGTH _markers _count)
    set(${out_var} ${_count} PARENT_SCOPE)
endfunction()
