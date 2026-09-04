// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/ui/ncurses_internal.h>
#include <stdio.h>

int
main(void) {
    assert(ui_burst_is_active_call(16) == 1);
    assert(ui_burst_is_active_call(20) == 1);
    assert(ui_burst_is_active_call(21) == 1);
    assert(ui_burst_is_active_call(22) == 1);
    assert(ui_burst_is_active_call(26) == 1);
    assert(ui_burst_is_active_call(27) == 1);

    assert(ui_burst_is_active_p25_call(16) == 0);
    assert(ui_burst_is_active_p25_call(20) == 1);
    assert(ui_burst_is_active_p25_call(21) == 1);
    assert(ui_burst_is_active_p25_call(22) == 1);
    assert(ui_burst_is_active_p25_call(26) == 1);
    assert(ui_burst_is_active_p25_call(27) == 1);

    assert(ui_burst_has_p25_crypto_metadata(20) == 1);
    assert(ui_burst_has_p25_crypto_metadata(21) == 1);
    assert(ui_burst_has_p25_crypto_metadata(22) == 1);
    assert(ui_burst_has_p25_crypto_metadata(25) == 1);
    assert(ui_burst_has_p25_crypto_metadata(26) == 1);
    assert(ui_burst_has_p25_crypto_metadata(27) == 1);

    assert(ui_burst_is_active_call(0) == 0);
    assert(ui_burst_is_active_call(19) == 0);
    assert(ui_burst_is_active_call(23) == 0);
    assert(ui_burst_is_active_call(24) == 0);
    assert(ui_burst_is_active_call(25) == 0);
    assert(ui_burst_is_active_call(28) == 0);
    assert(ui_burst_is_active_call(29) == 0);
    assert(ui_burst_is_active_call(30) == 0);
    assert(ui_burst_is_active_call(31) == 0);

    assert(ui_burst_is_active_p25_call(23) == 0);
    assert(ui_burst_is_active_p25_call(24) == 0);
    assert(ui_burst_is_active_p25_call(28) == 0);
    assert(ui_burst_is_active_p25_call(29) == 0);
    assert(ui_burst_is_active_p25_call(30) == 0);
    assert(ui_burst_is_active_p25_call(31) == 0);

    assert(ui_burst_has_p25_crypto_metadata(0) == 0);
    assert(ui_burst_has_p25_crypto_metadata(16) == 0);
    assert(ui_burst_has_p25_crypto_metadata(19) == 0);
    assert(ui_burst_has_p25_crypto_metadata(23) == 0);
    assert(ui_burst_has_p25_crypto_metadata(24) == 0);
    assert(ui_burst_has_p25_crypto_metadata(28) == 0);
    assert(ui_burst_has_p25_crypto_metadata(29) == 0);
    assert(ui_burst_has_p25_crypto_metadata(30) == 0);
    assert(ui_burst_has_p25_crypto_metadata(31) == 0);

    /* A lockout-suppressed companion (no ACTIVE canonical call, crypto
       suppressed) with a call-claiming burst hint renders idle. */
    assert(ui_p25p2_lockout_suppressed_slot(1, 0, 1, 20) == 1); /* MAC_PTT repeat */
    assert(ui_p25p2_lockout_suppressed_slot(1, 0, 1, 21) == 1); /* MAC_ACTIVE repeat */
    assert(ui_p25p2_lockout_suppressed_slot(1, 0, 1, 22) == 1); /* MAC_HANGTIME */

    /* Follow mode, an ACTIVE canonical call, an unsuppressed classification,
       or a hint with no call/crypto claim all render as-is. */
    assert(ui_p25p2_lockout_suppressed_slot(0, 0, 1, 21) == 0); /* lockout off */
    assert(ui_p25p2_lockout_suppressed_slot(1, 1, 1, 21) == 0); /* call active */
    assert(ui_p25p2_lockout_suppressed_slot(1, 0, 0, 21) == 0); /* crypto clear/unknown */
    assert(ui_p25p2_lockout_suppressed_slot(1, 0, 1, 23) == 0); /* PTT END */
    assert(ui_p25p2_lockout_suppressed_slot(1, 0, 1, 24) == 0); /* already idle */

    printf("UI_BURST_ACTIVE_CALL: OK\n");
    return 0;
}
