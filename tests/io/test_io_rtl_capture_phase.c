// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <limits.h>
#include <stdio.h>

#include "rtl_capture_phase.h"

static int
expect_eq(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
test_reconnect_pending_discard_phase(void) {
    int rc = 0;
    unsigned int carry = 0;
    int phase = 2;
    int remaining = 0;
    size_t pending = 7;

    remaining = rtl_capture_restart_u8_stream_with_pending(&phase, remaining, &carry, &pending);
    rc |= expect_eq("reconnect drops buffered bytes from the old stream", (int)pending, 0);
    rc |= expect_eq("reconnect resets capture phase for the fresh stream", phase, 0);
    rc |= expect_eq("reconnect keeps empty mute span empty", remaining, 0);
    rc |= expect_eq("reconnect clears fragmented carry with pending bytes", (int)carry, 0);

    phase = 3;
    pending = 1;
    remaining = rtl_capture_restart_u8_stream_with_pending(&phase, remaining, NULL, &pending);
    rc |= expect_eq("reconnect resets phase even when only one stale byte was buffered", phase, 0);
    rc |= expect_eq("reconnect drops lone buffered byte from the old stream", (int)pending, 0);
    rc |= expect_eq("reconnect still leaves empty mute span unchanged", remaining, 0);

    return rc;
}

static int
test_pending_tail_drop_advances_phase(void) {
    int rc = 0;

    rc |= expect_eq("tail drop keeps phase when dropping a lone raw byte", rtl_capture_phase_advance_u8_bytes(1, 1), 1);
    rc |= expect_eq("tail drop advances by the lost buffered iq pairs", rtl_capture_phase_advance_u8_bytes(3, 5), 1);

    return rc;
}

static int
test_fragmented_muted_discard_phase(void) {
    int rc = 0;
    unsigned int carry = 0;
    int phase = 2;
    int remaining = rtl_capture_align_u8_iq_bytes(3);

    rc |= expect_eq("mute request rounds odd span to whole pair", remaining, 4);

    phase = rtl_capture_phase_advance_u8_bytes_fragmented(phase, 3, &carry);
    remaining -= 3;
    rc |= expect_eq("first odd fragment advances one full sample", phase, 3);
    rc |= expect_eq("first odd fragment leaves byte carry", (int)carry, 1);
    rc |= expect_eq("remaining muted bytes can be odd mid-stream", remaining, 1);

    phase = rtl_capture_phase_advance_u8_bytes_fragmented(phase, 1, &carry);
    remaining -= 1;
    rc |= expect_eq("second fragment completes the held sample", phase, 0);
    rc |= expect_eq("completed mute clears byte carry", (int)carry, 0);
    rc |= expect_eq("mute window drains exactly", remaining, 0);

    return rc;
}

static int
test_reconnect_realigns_fragmented_mute(void) {
    int rc = 0;
    unsigned int carry = 0;
    int phase = 2;
    int remaining = rtl_capture_align_u8_iq_bytes(3);

    phase = rtl_capture_phase_advance_u8_bytes_fragmented(phase, 3, &carry);
    remaining -= 3;
    rc |= expect_eq("pre-reconnect fragmented mute leaves odd remainder", remaining, 1);
    rc |= expect_eq("pre-reconnect fragmented mute leaves byte carry", (int)carry, 1);

    remaining = rtl_capture_restart_fragmented_u8_bytes(remaining, &carry);
    rc |= expect_eq("reconnect realigns remaining mute span", remaining, 2);
    rc |= expect_eq("reconnect clears muted-byte carry", (int)carry, 0);

    phase = rtl_capture_phase_advance_u8_bytes_fragmented(phase, (size_t)remaining, &carry);
    remaining -= 2;
    rc |= expect_eq("realigned reconnect mute advances one fresh iq pair", phase, 0);
    rc |= expect_eq("realigned reconnect mute clears byte carry", (int)carry, 0);
    rc |= expect_eq("realigned reconnect mute drains exactly", remaining, 0);

    return rc;
}

static int
test_stream_restart_resets_phase_and_carry(void) {
    int rc = 0;
    unsigned int carry = 1;
    int phase = 3;
    int remaining = 1;

    remaining = rtl_capture_restart_u8_stream(&phase, remaining, &carry);
    rc |= expect_eq("stream restart resets phase to zero", phase, 0);
    rc |= expect_eq("stream restart realigns odd mute remainder", remaining, 2);
    rc |= expect_eq("stream restart clears fragmented mute carry", (int)carry, 0);

    phase = 2;
    carry = 0;
    remaining = 4;
    remaining = rtl_capture_restart_u8_stream(&phase, remaining, &carry);
    rc |= expect_eq("stream restart clears non-zero phase without leftover mute carry", phase, 0);
    rc |= expect_eq("aligned mute span stays unchanged across restart", remaining, 4);
    rc |= expect_eq("aligned restart keeps carry cleared", (int)carry, 0);

    return rc;
}

static int
test_stream_restart_discards_buffered_bytes(void) {
    int rc = 0;
    unsigned int carry = 1;
    int phase = 3;
    int remaining = 5;
    size_t pending = 7;

    remaining = rtl_capture_restart_u8_stream_with_pending(&phase, remaining, &carry, &pending);
    rc |= expect_eq("stream restart drops buffered bytes from old stream", (int)pending, 0);
    rc |= expect_eq("stream restart still resets phase when dropping pending bytes", phase, 0);
    rc |= expect_eq("stream restart still realigns odd mute remainder when dropping pending bytes", remaining, 6);
    rc |= expect_eq("stream restart clears fragmented mute carry when dropping pending bytes", (int)carry, 0);

    pending = 0;
    remaining = rtl_capture_restart_u8_stream_with_pending(NULL, 0, NULL, &pending);
    rc |= expect_eq("stream restart tolerates empty pending buffer", (int)pending, 0);
    rc |= expect_eq("stream restart leaves empty mute count unchanged", remaining, 0);

    return rc;
}

static int
test_processing_byte_carry(void) {
    int rc = 0;
    struct rtl_capture_u8_byte_carry carry = {0};
    unsigned char pair[2] = {0, 0};
    const unsigned char chunk1[3] = {10, 11, 20};
    const unsigned char chunk2[3] = {21, 30, 31};

    rc |= expect_eq("odd processing chunk exposes one complete pair",
                    (int)rtl_capture_u8_byte_carry_ready_bytes(3, &carry), 2);
    rc |= expect_eq("prefix consume without stored byte does nothing",
                    (int)rtl_capture_u8_byte_carry_consume_prefix(chunk1, 3, &carry, pair), 0);

    rtl_capture_u8_byte_carry_save(&carry, chunk1[2]);
    rc |= expect_eq("stored byte plus one new byte forms a pair", (int)rtl_capture_u8_byte_carry_ready_bytes(1, &carry),
                    2);
    rc |= expect_eq("prefix consume uses one new byte",
                    (int)rtl_capture_u8_byte_carry_consume_prefix(chunk2, 3, &carry, pair), 1);
    rc |= expect_eq("prefix consume keeps the carried byte order", (int)pair[0], 20);
    rc |= expect_eq("prefix consume appends the next raw byte", (int)pair[1], 21);
    rc |= expect_eq("prefix consume clears the stored byte", (int)carry.valid, 0);

    return rc;
}

static int
test_drop_alignment_retain_tail(void) {
    int rc = 0;
    struct rtl_capture_u8_byte_carry carry = {0};
    const unsigned char first[3] = {10, 11, 12};
    const unsigned char second[2] = {13, 14};
    const unsigned char third[1] = {15};

    rc |= expect_eq("odd dropped span keeps one trailing byte buffered",
                    (int)rtl_capture_u8_byte_carry_drop_aligned(first, 3, &carry), 2);
    rc |= expect_eq("odd dropped span keeps the final raw byte", (int)carry.byte, 12);
    rc |= expect_eq("odd dropped span marks carry valid", (int)carry.valid, 1);

    rc |= expect_eq("drop with a carried byte still drops only full pairs",
                    (int)rtl_capture_u8_byte_carry_drop_aligned(second, 2, &carry), 2);
    rc |= expect_eq("drop with a carried byte retains the new tail", (int)carry.byte, 14);
    rc |= expect_eq("drop with a carried byte keeps carry valid", (int)carry.valid, 1);

    rc |= expect_eq("one more byte completes the retained pair",
                    (int)rtl_capture_u8_byte_carry_drop_aligned(third, 1, &carry), 2);
    rc |= expect_eq("completed dropped pair clears carry", (int)carry.valid, 0);

    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= expect_eq("negative clears mute", rtl_capture_align_u8_iq_bytes(-7), 0);
    rc |= expect_eq("zero stays zero", rtl_capture_align_u8_iq_bytes(0), 0);
    rc |= expect_eq("even byte count unchanged", rtl_capture_align_u8_iq_bytes(8), 8);
    rc |= expect_eq("single byte rounds to pair", rtl_capture_align_u8_iq_bytes(1), 2);
    rc |= expect_eq("odd byte count rounds up", rtl_capture_align_u8_iq_bytes(5), 6);
    rc |= expect_eq("overflow saturates to even", rtl_capture_align_u8_iq_bytes(INT_MAX), INT_MAX - 1);
    rc |= test_reconnect_pending_discard_phase();
    rc |= test_pending_tail_drop_advances_phase();
    rc |= test_fragmented_muted_discard_phase();
    rc |= test_reconnect_realigns_fragmented_mute();
    rc |= test_stream_restart_resets_phase_and_carry();
    rc |= test_stream_restart_discards_buffered_bytes();
    rc |= test_processing_byte_carry();
    rc |= test_drop_alignment_retain_tail();

    return rc ? 1 : 0;
}
