// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 LCW → Trunk SM dispatch tests.
 *
 * Verifies that an explicit Group Voice Channel Update (format 0x44) reaches
 * the canonical state machine with the expected grant fields under
 * retune-allowed policy, and is ignored when retune is disabled.
 */

#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static dsd_trunk_tune_result
test_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    return freq > 0 ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

void p25_test_invoke_lcw(const unsigned char* lcw_bits, int len, int enable_retune, long cc_freq, long lastsrc,
                         long tuner_freq);

// Alias helpers referenced by the LCW path.
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)lc_bits;
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

static void
set_bits_msb(uint8_t* bits, int start, int width, unsigned value) {
    for (int i = 0; i < width; i++) {
        int bit = (value >> (width - 1 - i)) & 1;
        bits[start + i] = (uint8_t)bit;
    }
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    install_trunk_tuning_hooks();

    // Build LCW bits for format 0x44 (Group Voice Channel Update – Explicit)
    // Layout (bit indices):
    //   [0..7]  format (0x44), with bit0=PF=0, bit1=SF=1
    //   [8..15] MFID (0)
    //   [16..23] SVC options
    //   [24..39] Group ID
    //   [40..55] CHAN-T (iden:4 | chan:12)
    //   [56..71] CHAN-R (unused here)
    uint8_t lcw[72];
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    const int svc = 0x00;  // unencrypted
    const int tg = 0x1234; // talkgroup
    const int ch = 0x100A; // iden=1, chan=0x00A (10)
    set_bits_msb(lcw, 0, 8, 0x44);
    set_bits_msb(lcw, 8, 8, 0x00);
    set_bits_msb(lcw, 16, 8, (unsigned)svc);
    set_bits_msb(lcw, 24, 16, (unsigned)tg);
    set_bits_msb(lcw, 40, 16, (unsigned)ch);
    // CHAN-R left zero

    // Subcase A: retune disabled → no SM dispatch
    p25_test_invoke_lcw(lcw, 72, /*enable_retune*/ 0, /*cc*/ 851000000, /*lastsrc*/ 0, /*tuner_freq*/ 0);
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    rc |= expect_eq_int("no-dispatch when disabled", (int)ctx->grant_count, 0);

    // Subcase B: retune enabled and CC set → expect dispatch with exact fields
    p25_test_invoke_lcw(lcw, 72, /*enable_retune*/ 1, /*cc*/ 851000000, /*lastsrc*/ 0, /*tuner_freq*/ 0);
    ctx = p25_sm_get_ctx();
    rc |= expect_eq_int("dispatch called", (int)ctx->grant_count, 1);
    rc |= expect_eq_int("channel", ctx->vc_channel, ch);
    rc |= expect_eq_int("svc", ctx->slots[0].svc_bits, svc);
    rc |= expect_eq_int("tg", ctx->vc_tg, tg);
    // source may be 0 unless prior LCW set it
    rc |= expect_eq_int("src default", ctx->vc_src, 0);

    // Subcase C: LCW traffic frames must not infer a CC from live tuner metadata.
    p25_test_invoke_lcw(lcw, 72, /*enable_retune*/ 1, /*cc*/ 0, /*lastsrc*/ 0, /*tuner_freq*/ 851012500);
    ctx = p25_sm_get_ctx();
    rc |= expect_eq_int("no dispatch from tuner-only lcw", (int)ctx->grant_count, 0);

    // Gating cases are covered in a separate test without overriding
    // the canonical grant handler so the implementation's gating logic runs.

    // Format 0x42 reports calls in progress on other channels. It is display-only
    // in the LCW path and must not dispatch a traffic-channel grant.
    {
        uint8_t lcw42[72];
        DSD_MEMSET(lcw42, 0, sizeof(lcw42));
        set_bits_msb(lcw42, 0, 8, 0x42);
        set_bits_msb(lcw42, 8, 16, (unsigned)ch);
        set_bits_msb(lcw42, 24, 16, (unsigned)tg);

        p25_test_invoke_lcw(lcw42, 72, /*enable_retune*/ 0, /*cc*/ 851000000, /*lastsrc*/ 777777,
                            /*tuner_freq*/ 0);
        ctx = p25_sm_get_ctx();
        rc |= expect_eq_int("0x42 no dispatch", (int)ctx->grant_count, 0);
    }

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
