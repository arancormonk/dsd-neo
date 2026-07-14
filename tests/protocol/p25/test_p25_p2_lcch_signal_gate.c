// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify that LCCH MAC_SIGNAL does not flip P25p2 per-slot audio gates.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Forward declaration for the MAC VPDU handler under test
void process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]);

// Minimal stubs referenced by linked objects

// Alias/embedded helpers referenced in p25p2_vpdu.c (unused in this test path)
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    // Pretend we are on an LCCH-bearing channel; choose slot 0
    state.p2_is_lcch = 1;
    state.currentslot = 0;
    // Open gates for both logical slots
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_p2_audio_allowed[1] = 1;

    // Build a MAC buffer with SIGNAL opcode; use FACCH type (0)
    unsigned long long int MAC[24];
    DSD_MEMSET(MAC, 0, sizeof(MAC));
    MAC[1] = 0x00; // SIGNAL
    MAC[2] = 0x00; // standard MFID

    process_MAC_VPDU(&opts, &state, 0 /*FACCH*/, MAC);

    // Gates must remain unchanged by MAC_SIGNAL when on LCCH
    rc |= expect_eq("gate slot0", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("gate slot1", state.p25_p2_audio_allowed[1], 1);

    // Repeat on slot 1 and SACCH path to cover inverted mapping
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.p2_is_lcch = 1;
    state.currentslot = 1;
    state.p25_p2_audio_allowed[0] = 1;
    state.p25_p2_audio_allowed[1] = 1;
    process_MAC_VPDU(&opts, &state, 1 /*SACCH*/, MAC);
    rc |= expect_eq("gate slot0 (SACCH)", state.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("gate slot1 (SACCH)", state.p25_p2_audio_allowed[1], 1);

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
