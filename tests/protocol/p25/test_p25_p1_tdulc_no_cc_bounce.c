// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression: P25p1 TDULC must not force an immediate return to the control channel.
 *
 * Some systems use TDULC to carry mid-call link control updates (e.g., LCW 0x44).
 * Returning to CC on every TDULC causes VC bouncing and missed audio.
 *
 * This test:
 *  - Puts the unified P25 trunk SM into TUNED via a synthetic group grant
 *  - Invokes processTDULC() while forcing TDULC FEC failure (no LCW dispatch)
 *  - Asserts that return_to_cc() is not called (i.e., no immediate CC bounce)
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
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

void processTDULC(dsd_opts* opts, dsd_state* state);

// Canonical trunk-tuning hooks keep the test hermetic.
static int g_return_to_cc_called = 0;

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
    (void)request_id;
    g_return_to_cc_called++;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
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

// Minimal utility used by TDULC path (MSB-first)
// LCW path external helpers (not exercised by this test; provide no-op stubs for link)
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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

// FEC stubs: force Reed-Solomon failure so processTDULC does not dispatch LCW
int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_golay_24_12(char* dodeca, char* parity, int* fixed_errors) {
    (void)dodeca;
    (void)parity;
    if (fixed_errors) {
        *fixed_errors = 0;
    }
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_reedsolomon_24_12_13(char* data, char* parity) {
    (void)data;
    (void)parity;
    return 1; // irrecoverable
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_reedsolomon_24_12_13_soft(char* data, char* parity, const int* erasures, int n_erasures) {
    (void)data;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 1;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    if (out_soft) {
        out_soft->reliability = 255;
        out_soft->llr[0] = -255;
        out_soft->llr[1] = -255;
    }
    return 0;
}

// Canonical TDULC dibit-reader fixture (all zeros).
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
read_dibit_update_soft_data(dsd_opts* opts, dsd_state* state, char* buffer, unsigned int count, int* status_count,
                            P25P1SoftDibit* soft_dibits, int* soft_dibit_index) {
    (void)opts;
    (void)state;
    (void)status_count;
    (void)soft_dibits;
    (void)soft_dibit_index;
    DSD_MEMSET(buffer, 0, count);
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

    static dsd_opts opts;
    static dsd_state state;
    install_trunk_tuning_hooks();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    // Enable trunking and allow group-call tuning
    opts.trunk_enable = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    opts.verbose = 0;

    // Seed a known CC to allow the SM to initialize in ON_CC
    state.p25_cc_freq = 851000000;

    // Minimal IDEN mapping so the synthetic grant produces a non-zero VC frequency
    int iden = 1;
    // Populate new dual-array
    state.p25_iden_fdma[iden].base_freq = 851000000L / 5L;
    state.p25_iden_fdma[iden].chan_type = 1;
    state.p25_iden_fdma[iden].chan_spac = 100;
    state.p25_iden_fdma[iden].populated = 1;
    state.p25_iden_fdma[iden].trust = 2;
    state.p25_chan_tdma_explicit[iden] = 1; // FDMA known

    // Initialize SM and tune to a VC via a group grant
    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
    int channel = (iden << 12) | 0x000A;
    p25_sm_event(p25_sm_get_ctx(), &opts, &state,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = channel,
                                   .tg = 1234,
                                   .src = 5678,
                                   .svc_bits = 0,
                                   .is_group = 1});
    rc |= expect_eq_int("tuned after grant", opts.trunk_is_tuned, 1);

    // TDULC should not immediately bounce back to CC
    g_return_to_cc_called = 0;
    processTDULC(&opts, &state);
    rc |= expect_eq_int("return_to_cc not called", g_return_to_cc_called, 0);
    rc |= expect_eq_int("still tuned after TDULC", opts.trunk_is_tuned, 1);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
