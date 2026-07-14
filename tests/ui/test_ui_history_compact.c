// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <dsd-neo/app_control/history.h>

int
main(void) {
    char out[256];

    const char* canonical = "2026-01-21 02:05:13 DMR TGT: 00000014; SRC: 00300010; CC: 01; Group; TXI;";
    size_t n = dsd_app_frontend_history_compact_event_text(out, sizeof out, canonical, 1);
    assert(n == strlen(out));
    assert(strcmp(out, "02:05:13 DMR TGT: 00000014; SRC: 00300010; CC: 01; Group; TXI;") == 0);

    n = dsd_app_frontend_history_compact_event_text(out, sizeof out, canonical, 2);
    assert(n == strlen(out));
    assert(strcmp(out, canonical) == 0);

    const char* noncanonical = "DMR TGT: 00000014; SRC: 00300010; CC: 01;";
    n = dsd_app_frontend_history_compact_event_text(out, sizeof out, noncanonical, 1);
    assert(n == strlen(out));
    assert(strcmp(out, noncanonical) == 0);

    n = dsd_app_frontend_history_compact_event_text(out, sizeof out, NULL, 1);
    assert(n == 0);
    assert(out[0] == '\0');

    char tiny[5];
    n = dsd_app_frontend_history_compact_event_text(tiny, sizeof tiny, canonical, 1);
    assert(n == 4);
    assert(strcmp(tiny, "02:0") == 0);

    time_t newer = dsd_app_frontend_history_event_sort_time("2026-06-07 19:20:15 P25p2 TGT: 00050002;", (time_t)1);
    time_t older =
        dsd_app_frontend_history_event_sort_time("2026-06-07 19:20:07 P25p1 TGT: 00021001;", (time_t)2000000000);
    assert(newer > older);
    assert(dsd_app_frontend_history_event_sort_time(noncanonical, (time_t)1234) == (time_t)1234);

    printf("UI_HISTORY_COMPACT: OK\n");
    return 0;
}
