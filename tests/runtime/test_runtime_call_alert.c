// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/call_alert.h>
#include <stdint.h>
#include <stdio.h>

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    rc |= expect_int("disabled suppresses default mask",
                     dsd_call_alert_event_enabled(0, 0, DSD_CALL_ALERT_EVENT_VOICE_START), 0);
    rc |= expect_int("enabled zero mask means legacy all",
                     dsd_call_alert_event_enabled(1, 0, DSD_CALL_ALERT_EVENT_DATA), 1);
    rc |= expect_int(
        "start-only allows start",
        dsd_call_alert_event_enabled(1, DSD_CALL_ALERT_EVENT_VOICE_START, DSD_CALL_ALERT_EVENT_VOICE_START), 1);
    rc |= expect_int("start-only suppresses end",
                     dsd_call_alert_event_enabled(1, DSD_CALL_ALERT_EVENT_VOICE_START, DSD_CALL_ALERT_EVENT_VOICE_END),
                     0);
    rc |= expect_int("unknown bits are masked",
                     dsd_call_alert_normalize_events((uint8_t)(0x80 | DSD_CALL_ALERT_EVENT_DATA)),
                     DSD_CALL_ALERT_EVENT_DATA);

    if (rc == 0) {
        printf("RUNTIME_CALL_ALERT: OK\n");
    }
    return rc;
}
