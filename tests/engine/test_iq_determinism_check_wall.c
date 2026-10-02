// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief Stand-in replay host for the runner's wall-time check (issue #572).
 *
 * tests/cmake/IqDeterminismCheckWall.cmake runs tests/iq_determinism_check.cmake with this program as its host. It
 * takes the host arguments the runner passes and reads only "--fake-wall VALUE", which the test hands over through the
 * runner's MODE. It prints what a passing leg of a 1 s capture prints, a decoded "Src=901", the REPLAY STREAM line
 * (media_ms=1000.000000) and the REPLAY JITTER line, and then "REPLAY WALL: VALUE", or no REPLAY WALL line at all for
 * VALUE "omit". So the test sets exactly what the runner reads a realtime leg's wall time from.
 */

#include <stdio.h>
#include <string.h>

int
main(int argc, char** argv) {
    const char* wall = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--fake-wall") == 0) {
            wall = argv[i + 1];
        }
    }
    if (wall == NULL) {
        (void)fputs("iq determinism fake host: --fake-wall VALUE|omit is required\n", stderr);
        return 2;
    }
    (void)fputs("fake decode Src=901\n", stdout);
    (void)fputs("REPLAY STREAM: fsk_samples=48000 cqpsk_symbols=0 monitor_samples=0 generation_changes=0 "
                "media_ms=1000.000000\n",
                stderr);
    (void)fputs("REPLAY JITTER: jitter=off short_reads=off reads=1\n", stderr);
    if (strcmp(wall, "omit") != 0) {
        (void)fputs("REPLAY WALL: ", stderr);
        (void)fputs(wall, stderr);
        (void)fputs("\n", stderr);
    }
    return 0;
}
