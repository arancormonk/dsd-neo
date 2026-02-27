// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/ui/ui_history.h>

int
main(void) {
    char out[256];

    const char* canonical = "2026-01-21 02:05:13 DMR TGT: 00000014; SRC: 00300010; CC: 01; Group; TXI;";
    size_t n = ui_history_compact_event_text(out, sizeof out, canonical, 1);
    assert(n == strlen(out));
    assert(strcmp(out, "02:05:13 DMR TGT: 00000014; SRC: 00300010; CC: 01; Group; TXI;") == 0);

    n = ui_history_compact_event_text(out, sizeof out, canonical, 2);
    assert(n == strlen(out));
    assert(strcmp(out, canonical) == 0);

    const char* noncanonical = "DMR TGT: 00000014; SRC: 00300010; CC: 01;";
    n = ui_history_compact_event_text(out, sizeof out, noncanonical, 1);
    assert(n == strlen(out));
    assert(strcmp(out, noncanonical) == 0);

    n = ui_history_compact_event_text(out, sizeof out, NULL, 1);
    assert(n == 0);
    assert(out[0] == '\0');

    char tiny[5];
    n = ui_history_compact_event_text(tiny, sizeof tiny, canonical, 1);
    assert(n == 4);
    assert(strcmp(tiny, "02:0") == 0);

    printf("UI_HISTORY_COMPACT: OK\n");
    return 0;
}
