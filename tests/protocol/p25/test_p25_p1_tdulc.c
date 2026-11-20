// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 TDULC parser test → LCW retune (format 0x44).
 *
 * Feeds deterministic 6×12-bit data words through the canonical dibit reader to form an LCW
 * with format 0x44, service=0x00, TG=0x4567, CHAN-T=0x100A. Stubs FEC and
 * analog readers to bypass error correction. Asserts the canonical trunk SM
 * accepts a group grant when LCW retune is enabled and CC is known.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

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

void processTDULC(dsd_opts* opts, dsd_state* state);
void processTDU(dsd_opts* opts, dsd_state* state);

static int g_rs_hard_result = 0;
static int g_rs_soft_result = 1;
static int g_rs_soft_called = 0;
/* Whether the first of the TDULC's trailing reads adopts a retune its capture recorded, where the carrier boundary moves
   the carrier count (dsd_engine_carrier_boundary(), issue #575). The words before them come through the scripted
   reader, so this stub's first read is the first trailing symbol. */
static int g_boundary_in_tail = 0;
/* Whether a Signal search's yield (or a stalled live read) interrupts the first trailing read: getSymbol() records the
   interruption there and stands erasures in for every symbol after it. */
static int g_interrupt_in_tail = 0;

// Alias helpers referenced by LCW path
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

/* What the GPS decoders were handed (issue #800): the report's provenance, the link control's soft-decode mark while
   it was dispatched, and the L3Harris body. */
static int g_gps_calls = 0;
static dsd_gps_report_ctx g_gps_ctx;
static uint8_t g_gps_lcw_soft = 0;
static uint8_t g_gps_body[DSD_HARRIS_GPS_BODY_OCTETS];
/* The epoch of the valid closing call the context carried (it lives on processTDULC()'s stack); 0 for none. */
static uint64_t g_gps_closing_epoch = 0;

static void
note_gps_call(const dsd_state* state, const dsd_gps_report_ctx* ctx) {
    g_gps_calls++;
    DSD_MEMSET(&g_gps_ctx, 0, sizeof(g_gps_ctx));
    g_gps_closing_epoch = 0;
    if (ctx) {
        g_gps_ctx = *ctx;
        if (ctx->closing != NULL && ctx->closing->valid) {
            g_gps_closing_epoch = ctx->closing->epoch;
        }
        g_gps_ctx.closing = NULL;
    }
    g_gps_lcw_soft = state->p25_p1_lcw_soft;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[], const dsd_gps_report_ctx* ctx) {
    (void)opts;
    (void)lc_bits;
    note_gps_call(state, ctx);
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* body, const dsd_gps_report_ctx* ctx) {
    (void)opts;
    note_gps_call(state, ctx);
    DSD_MEMCPY(g_gps_body, body, sizeof(g_gps_body));
    return 0;
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

// Minimal utility used by p25_lcw (MSB-first)
// FEC stubs (bypass corrections)
int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_golay_24_12(char* dodeca, char* parity, int* fixed_errors) {
    (void)dodeca;
    (void)parity;
    if (fixed_errors) {
        *fixed_errors = 0;
    }
    return 0; // no irrecoverable errors
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_reedsolomon_24_12_13(char* data, char* parity) {
    (void)data;
    (void)parity;
    return g_rs_hard_result;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
check_and_fix_reedsolomon_24_12_13_soft(char* data, char* parity, const int* erasures, int n_erasures) {
    (void)data;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25p1_rs_24_12_13_soft_reliability(char* data, const char* parity, const uint8_t* data_reliab,
                                   const uint8_t* parity_reliab) {
    (void)data;
    (void)parity;
    (void)data_reliab;
    (void)parity_reliab;
    g_rs_soft_called++;
    return g_rs_soft_result;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    if (g_boundary_in_tail && state != NULL) {
        state->carrier_seq++;
        g_boundary_in_tail = 0;
    }
    if (g_interrupt_in_tail && state != NULL) {
        state->input_interrupted = 1;
        g_interrupt_in_tail = 0;
    }
    if (out_soft) {
        out_soft->reliability = 255;
        out_soft->llr[0] = -255;
        out_soft->llr[1] = -255;
    }
    return 0;
}

// Scripted 12-bit words fed into the canonical reader in TDULC data-word order.
static char g_words[6][12];
static int g_word_index = 0;
static int g_read_parity_next = 0;

// Helper: write MSB-first bits of an 8- or 16-bit value into an array
static void
bits_from_u16(uint16_t v, int nbits, char* out_bits) {
    for (int i = 0; i < nbits; i++) {
        out_bits[i] = (char)((v >> (nbits - 1 - i)) & 1);
    }
}

// Build the 6×12-bit data words for LCW format 0x44, MFID=0x00, SVC, TG, CHAN-T, CHAN-R.
static void
build_lcw_words(uint8_t lc_format, uint8_t mfid, uint8_t svc, uint16_t group1, uint16_t channelt, uint16_t channelr) {
    // Prepare bit arrays
    char fmt8[8], mf8[8], sv8[8], tg16[16], ct16[16], cr16[16];
    bits_from_u16(lc_format, 8, fmt8);
    bits_from_u16(mfid, 8, mf8);
    bits_from_u16(svc, 8, sv8);
    bits_from_u16(group1, 16, tg16);
    bits_from_u16(channelt, 16, ct16);
    bits_from_u16(channelr, 16, cr16);

    // Clear all words
    DSD_MEMSET(g_words, 0, sizeof(g_words));

    // Map into dodeca_data[5..0] per TDULC packing
    // data[5]
    for (int i = 0; i < 8; i++) {
        g_words[0][i] = fmt8[i]; // lcformat[0..7]
    }
    for (int i = 0; i < 4; i++) {
        g_words[0][8 + i] = mf8[i]; // mfid bits 0..3
    }
    // data[4]
    for (int i = 0; i < 4; i++) {
        g_words[1][i] = mf8[4 + i]; // mfid bits 4..7
    }
    for (int i = 0; i < 8; i++) {
        g_words[1][4 + i] = sv8[i]; // svc 8 bits
    }
    // data[3]
    for (int i = 0; i < 12; i++) {
        g_words[2][i] = tg16[i]; // group bits [0..11]
    }
    // data[2]
    for (int i = 0; i < 4; i++) {
        g_words[3][i] = tg16[12 + i]; // group bits [12..15]
    }
    for (int i = 0; i < 8; i++) {
        g_words[3][4 + i] = ct16[i]; // channelt bits [0..7]
    }
    // data[1]
    for (int i = 0; i < 8; i++) {
        g_words[4][i] = ct16[8 + i]; // channelt bits [8..15]
    }
    for (int i = 0; i < 4; i++) {
        g_words[4][8 + i] = cr16[i]; // channelr bits [0..3]
    }
    // data[0]
    for (int i = 0; i < 12; i++) {
        g_words[5][i] = cr16[4 + i]; // channelr bits [4..15]
    }

    // Shift the assembled data words into read order: index 0..5 should be data[5]..data[0]
    char ordered[6][12];
    DSD_MEMCPY(ordered[0], g_words[0], 12); // data[5]
    DSD_MEMCPY(ordered[1], g_words[1], 12); // data[4]
    DSD_MEMCPY(ordered[2], g_words[2], 12); // data[3]
    DSD_MEMCPY(ordered[3], g_words[3], 12); // data[2]
    DSD_MEMCPY(ordered[4], g_words[4], 12); // data[1]
    DSD_MEMCPY(ordered[5], g_words[5], 12); // data[0]
    DSD_MEMCPY(g_words, ordered, sizeof(ordered));

    g_word_index = 0;
    g_read_parity_next = 0;
}

// Canonical reader fixture used by TDULC.
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
read_dibit_update_soft_data(dsd_opts* opts, dsd_state* state, char* buffer, unsigned int count, int* status_count,
                            P25P1SoftDibit* soft_dibits, int* soft_dibit_index) {
    (void)opts;
    (void)state;
    (void)status_count;
    (void)soft_dibits;
    (void)soft_dibit_index;
    if (count != 12) {
        DSD_MEMSET(buffer, 0, count);
        return;
    }
    if (g_read_parity_next) {
        DSD_MEMSET(buffer, 0, 12);
        g_read_parity_next = 0;
        return;
    }
    if (g_word_index < 6) {
        DSD_MEMCPY(buffer, g_words[g_word_index], 12);
        g_word_index++;
    } else {
        DSD_MEMSET(buffer, 0, 12);
    }
    g_read_parity_next = 1;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_u64(const char* tag, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", tag, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "%s: condition failed\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_eq_float(const char* tag, float got, float want) {
    const float delta = got - want;
    const float abs_delta = (delta < 0.0f) ? -delta : delta;
    if (!(abs_delta <= 0.0001f)) {
        DSD_FPRINTF(stderr, "%s: got %.3f want %.3f\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
seed_active_call(dsd_state* state, uint64_t target, uint64_t source, uint16_t service_options) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
        .service_options = service_options,
        .emergency = (uint8_t)((service_options & 0x80U) != 0U),
        .priority = (uint8_t)(service_options & 0x07U),
        .has_service_metadata = 1U,
        .observed_m = 1.0,
    };
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0;
}

static int
test_voice_user_metadata_does_not_restart_call(void) {
    static const struct {
        uint8_t format;
        uint8_t mfid;
        const char* name;
    } cases[] = {
        {0x00, 0x00, "group voice user"},
        {0x03, 0x00, "unit voice user"},
        {0x4A, 0x00, "extended unit voice user"},
        {0x00, 0x90, "MFID90 regroup voice user"},
    };

    static dsd_opts opts;
    static dsd_state state;
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    int rc = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char label[96];
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        opts.trunk_hangtime = 2.0F;
        state.p25_cc_freq = 851000000;
        state.p25_iden_fdma[1].base_freq = 851000000 / 5;
        state.p25_iden_fdma[1].chan_type = 1;
        state.p25_iden_fdma[1].chan_spac = 100;
        state.p25_iden_fdma[1].trust = 2;
        state.p25_iden_fdma[1].populated = 1;
        state.p25_chan_tdma_explicit[1] = 1;

        p25_sm_init_ctx(ctx, &opts, &state);
        p25_sm_event_t grant = p25_sm_ev_group_grant(0x100A, 852125000, 0x1234, 0x010203, 0);
        p25_sm_event(ctx, &opts, &state, &grant);
        DSD_SNPRINTF(label, sizeof(label), "%s fixture tuned", cases[i].name);
        rc |= expect_eq_int(label, ctx->state, P25_SM_TUNED);
        DSD_SNPRINTF(label, sizeof(label), "%s fixture active", cases[i].name);
        rc |= expect_eq_int(label, p25_sm_emit_active_call(&opts, &state, 0, 0x1234, 0, 0x010203, 1, 0), 1);
        DSD_SNPRINTF(label, sizeof(label), "%s fixture voice active", cases[i].name);
        rc |= expect_eq_int(label, ctx->slots[0].voice_active, 1);

        // These LCWs are valid voice-user metadata. On a TDULC they describe
        // the transmission that just ended and must not open a new voice epoch.
        build_lcw_words(cases[i].format, cases[i].mfid, 0x00, 0x3333, 0x100A, 0x5678);
        processTDULC(&opts, &state);

        DSD_SNPRINTF(label, sizeof(label), "%s remains tuned", cases[i].name);
        rc |= expect_eq_int(label, ctx->state, P25_SM_TUNED);
        DSD_SNPRINTF(label, sizeof(label), "%s remains inactive", cases[i].name);
        rc |= expect_eq_int(label, ctx->slots[0].voice_active, 0);
        DSD_SNPRINTF(label, sizeof(label), "%s preserves hang timer", cases[i].name);
        rc |= expect_true(label, p25_sm_hangtime_started_m(ctx) > 0.0);
        dsd_state_ext_free_all(&state);
    }
    return rc;
}

/* A link control in LDU1 form: format, MFID, then octets 2-8. */
static void
lcw_from_octets(uint8_t lcw[72], uint8_t format, uint8_t mfid, const uint8_t octets[7]) {
    uint8_t bytes[9] = {format, mfid, octets[0], octets[1], octets[2], octets[3], octets[4], octets[5], octets[6]};
    for (int i = 0; i < 72; i++) {
        lcw[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1U);
    }
}

/* A trunked voice channel the grant of 0x010203 -> TG 0x1234 tuned. */
static void
tune_granted_channel(dsd_opts* opts, dsd_state* state) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_hangtime = 2.0F;
    state->p25_cc_freq = 851000000;
    state->synctype = DSD_SYNC_P25P1_POS;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->p25_iden_fdma[1].base_freq = 851000000 / 5;
    state->p25_iden_fdma[1].chan_type = 1;
    state->p25_iden_fdma[1].chan_spac = 100;
    state->p25_iden_fdma[1].trust = 2;
    state->p25_iden_fdma[1].populated = 1;
    state->p25_chan_tdma_explicit[1] = 1;
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, opts, state);
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x100A, 852125000, 0x1234, 0x010203, 0);
    p25_sm_event(ctx, opts, state, &grant);
    g_rs_hard_result = 0;
    g_rs_soft_result = 1;
    g_gps_calls = 0;
}

/* A trunked voice channel whose call 0x010203 -> TG 0x1234 a hard-decoded Group Voice Channel User LC opened. */
static uint64_t
start_checked_call(dsd_opts* opts, dsd_state* state) {
    tune_granted_channel(opts, state);
    const uint8_t user[7] = {0x00, 0x00, 0x12, 0x34, 0x01, 0x02, 0x03};
    uint8_t lcw[72];
    lcw_from_octets(lcw, 0x00, 0x00, user);
    p25_lcw(opts, state, lcw, 0);
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || call.ota_source_id != 0x010203U) {
        return 0U;
    }
    return call.epoch;
}

static int
expect_gps_of_closing_call(const char* tag, uint64_t epoch) {
    char label[96];
    int rc = 0;
    DSD_SNPRINTF(label, sizeof(label), "%s: one GPS dispatch", tag);
    rc |= expect_eq_int(label, g_gps_calls, 1);
    DSD_SNPRINTF(label, sizeof(label), "%s: the closing call", tag);
    rc |= expect_true(label, g_gps_closing_epoch == epoch && g_gps_ctx.call_epoch == epoch && epoch != 0U);
    DSD_SNPRINTF(label, sizeof(label), "%s: checked identity", tag);
    rc |= expect_true(label, g_gps_ctx.source_verified == 1U && g_gps_ctx.source_id == 0x010203U
                                 && g_gps_ctx.target_id == 0x1234U && g_gps_ctx.target_group == 1U);
    DSD_SNPRINTF(label, sizeof(label), "%s: hard decode", tag);
    rc |= expect_true(label, g_gps_ctx.pos_verified == 1U && g_gps_lcw_soft == 0U);
    return rc;
}

/* Issue #800 (spec N2, 2.20): the TDULC ends the call before it dispatches its own link control, so a GPS link control
   in it is attributed to the call it ended, captured just before; a soft-erasure RS recovery marks it unchecked as on
   LDU1; any later GPS link control sees only ACTIVE calls. */
static int
test_terminator_gps(void) {
    static dsd_opts opts;
    static dsd_state state;
    const uint8_t apx[7] = {0x00, 0x00, 0x2A, 0xAA, 0xAA, 0x15, 0x55};
    int rc = 0;

    uint64_t epoch = start_checked_call(&opts, &state);
    rc |= expect_true("terminator APX: checked call", epoch != 0U);
    build_lcw_words(0x06, 0x90, apx[0], (uint16_t)((apx[1] << 8) | apx[2]), (uint16_t)((apx[3] << 8) | apx[4]),
                    (uint16_t)((apx[5] << 8) | apx[6]));
    processTDULC(&opts, &state);
    rc |= expect_gps_of_closing_call("terminator APX", epoch);
    dsd_call_snapshot call;
    rc |= expect_true("terminator APX: the call ended",
                      dsd_call_state_get(&state, 0U, &call) > 0 && call.phase != DSD_CALL_PHASE_ACTIVE);

    // A GPS link control after the call ended (no new voice user heard) names no call and no checked source.
    g_gps_calls = 0;
    build_lcw_words(0x06, 0x90, apx[0], (uint16_t)((apx[1] << 8) | apx[2]), (uint16_t)((apx[3] << 8) | apx[4]),
                    (uint16_t)((apx[5] << 8) | apx[6]));
    processTDULC(&opts, &state);
    rc |= expect_eq_int("after the end: GPS dispatched", g_gps_calls, 1);
    rc |= expect_true("after the end: no call, unverified", g_gps_ctx.call_epoch == 0U && g_gps_closing_epoch == 0U
                                                                && g_gps_ctx.source_verified == 0U
                                                                && g_gps_ctx.source_id == 0U);

    // L3Harris Part A on the call's LDU1, Part B in its terminator.
    epoch = start_checked_call(&opts, &state);
    const uint8_t part_a[7] = {0x00, 0x00, 0x9E, 22, 0x00, 0x00, 0x80};
    uint8_t lcw[72];
    lcw_from_octets(lcw, 0x2A, 0xA4, part_a);
    p25_lcw(&opts, &state, lcw, 0);
    build_lcw_words(0x2B, 0xA4, 45, 0x1C20, 0x8400, 0x0315);
    processTDULC(&opts, &state);
    rc |= expect_gps_of_closing_call("terminator Harris Part B", epoch);
    rc |= expect_true("terminator Harris: Part A then Part B octets",
                      g_gps_body[3] == 22 && g_gps_body[6] == 0x80 && g_gps_body[7] == 45 && g_gps_body[13] == 0x15);
    rc |= expect_true("terminator Harris: Part A's reception", g_gps_ctx.origin != NULL);

    // A terminator whose link control passed only the soft-erasure retry is not checked (spec 2.20).
    epoch = start_checked_call(&opts, &state);
    build_lcw_words(0x06, 0x90, apx[0], (uint16_t)((apx[1] << 8) | apx[2]), (uint16_t)((apx[3] << 8) | apx[4]),
                    (uint16_t)((apx[5] << 8) | apx[6]));
    g_rs_hard_result = 1;
    g_rs_soft_result = 0;
    processTDULC(&opts, &state);
    rc |= expect_eq_int("soft terminator: GPS dispatched", g_gps_calls, 1);
    rc |= expect_true("soft terminator: marked soft while dispatched", g_gps_lcw_soft == 1U);
    rc |= expect_true("soft terminator: position unchecked", g_gps_ctx.pos_verified == 0U);
    rc |= expect_true("soft terminator: still the closing call", g_gps_closing_epoch == epoch);
    rc |= expect_eq_int("soft terminator: mark cleared after", state.p25_p1_lcw_soft, 0);
    g_rs_hard_result = 0;
    g_rs_soft_result = 1;

    dsd_state_ext_free_all(&state);
    return rc;
}

/* Issue #800: a TDULC's GPS link control belongs only to the call that TDULC ended. On a trunked voice channel a header
   can open the call before any voice frame: the trunk state machine follows no voice yet, declines the TDULC's end and
   the call stays open. Neither half of an L3Harris update in such TDULCs, nor a Motorola APX report, is that call's;
   successive TDULCs must not pair their halves against it. */
static int
test_terminator_gps_of_a_call_it_did_not_end(void) {
    static dsd_opts opts;
    static dsd_state state;
    const uint8_t apx[7] = {0x00, 0x00, 0x2A, 0xAA, 0xAA, 0x15, 0x55};
    int rc = 0;

    tune_granted_channel(&opts, &state);
    const p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    rc |= expect_eq_int("header call: tuned", ctx->state, P25_SM_TUNED);
    // The header opens the call; no voice user has reached the trunk state machine.
    rc |= expect_true("header call: opened", seed_active_call(&state, 0x1234, 0x010203, 0x00));
    dsd_call_snapshot call;
    const uint64_t epoch = dsd_call_state_get(&state, 0U, &call) > 0 ? call.epoch : 0U;
    rc |= expect_eq_int("header call: no voice followed", ctx->slots[0].voice_active, 0);

    g_gps_calls = 0;
    build_lcw_words(0x2A, 0xA4, 0x00, 0x009E, (uint16_t)(22U << 8), 0x0080);
    processTDULC(&opts, &state);
    rc |= expect_true("header call: the TDULC did not end it", dsd_call_state_get(&state, 0U, &call) > 0
                                                                   && call.phase == DSD_CALL_PHASE_ACTIVE
                                                                   && call.epoch == epoch);
    rc |= expect_eq_u64("header call: the TDULC ended no call", state.p25_p1_terminator_end_epoch, 0U);
    build_lcw_words(0x2B, 0xA4, 45, 0x1C20, 0x8400, 0x0315);
    processTDULC(&opts, &state);
    rc |= expect_eq_int("header call: TDULC halves never pair", g_gps_calls, 0);

    build_lcw_words(0x06, 0x90, apx[0], (uint16_t)((apx[1] << 8) | apx[2]), (uint16_t)((apx[3] << 8) | apx[4]),
                    (uint16_t)((apx[5] << 8) | apx[6]));
    processTDULC(&opts, &state);
    rc |= expect_eq_int("header call: APX dispatched", g_gps_calls, 1);
    rc |= expect_true("header call: APX names no call", g_gps_ctx.call_epoch == 0U && g_gps_closing_epoch == 0U
                                                            && g_gps_ctx.source_verified == 0U
                                                            && g_gps_ctx.source_id == 0U);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* Issue #800: the next engine run on a reused state (the engine's cleanup frees the call state; the new one goes on
   from the last run's epochs) is not attributed by the last run's hard-decoded voice-user identity: a GPS link control
   of the new run's first call, opened by a soft-recovered link control of the same radio, names no one. */
static int
test_reused_state_forgets_checked_identity(void) {
    static dsd_opts opts;
    static dsd_state state;
    const uint8_t apx[7] = {0x00, 0x00, 0x2A, 0xAA, 0xAA, 0x15, 0x55};
    const uint8_t user[7] = {0x00, 0x00, 0x12, 0x34, 0x01, 0x02, 0x03};
    uint8_t lcw[72];
    int rc = 0;

    const uint64_t first = start_checked_call(&opts, &state);
    rc |= expect_true("first run: checked identity of its call",
                      first != 0U && state.p25_p1_lc_attribution.call_epoch == first
                          && state.p25_p1_lc_attribution.source == 0x010203U);

    dsd_state_ext_free_all(&state);
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &opts, &state);
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x100A, 852125000, 0x1234, 0x010203, 0);
    p25_sm_event(ctx, &opts, &state, &grant);
    lcw_from_octets(lcw, 0x00, 0x00, user);
    state.p25_p1_lcw_soft = 1U;
    p25_lcw(&opts, &state, lcw, 0);
    state.p25_p1_lcw_soft = 0U;
    dsd_call_snapshot call;
    rc |= expect_true("next run: its first call is a new epoch of the same radio",
                      dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                          && call.epoch > first && call.ota_source_id == 0x010203U);

    g_gps_calls = 0;
    lcw_from_octets(lcw, 0x06, 0x90, apx);
    p25_lcw(&opts, &state, lcw, 0);
    rc |= expect_eq_int("next run: GPS dispatched", g_gps_calls, 1);
    rc |=
        expect_true("next run: the call, but no checked identity",
                    g_gps_ctx.call_epoch == call.epoch && g_gps_ctx.source_verified == 0U && g_gps_ctx.target_id == 0U);
    dsd_state_ext_free_all(&state);
    return rc;
}

/* The trunk state machine's diagnostic log, where it records the talkgroup each end it is handed names. */
static char g_sm_log_path[DSD_TEST_PATH_MAX];

/* The talker ends (p25_sm_emit_end_call_at(), `kind=end`) the trunk state machine logged so far, and into @p tg the
   talkgroup the last one named (0: none). A TDULC's own end runs first, so the talker end in its link control finds no
   followed voice and is logged as ignored, with the talkgroup it was handed. */
static int
logged_talker_ends(dsd_opts* opts, int* tg) {
    *tg = -1;
    if (opts->p25_sm_log_f != NULL) {
        (void)fflush(opts->p25_sm_log_f);
    }
    FILE* fp = dsd_fopen_private(g_sm_log_path, "r");
    if (fp == NULL) {
        return 0;
    }
    int count = 0;
    char line[4096];
    while (fgets(line, sizeof line, fp) != NULL) {
        const char* field = strstr(line, " event_tg=");
        if (strstr(line, "event=voice_end_ignored") == NULL || strstr(line, " kind=end") == NULL || field == NULL) {
            continue;
        }
        char* end = NULL;
        const long v = strtol(field + strlen(" event_tg="), &end, 10);
        *tg = (end != NULL && *end == ' ') ? (int)v : -1;
        count++;
    }
    fclose(fp);
    return count;
}

/* A TDULC read at @p clock, as the symbol grid would have it once the frame's sync and NID were read, whose link control
   is a Motorola talker end (MFID $90 LCO $0F) of radio 0x123456: the talkgroup that end delivers to the trunk state
   machine (p25_lcw_call(), from the call p25p1_terminator_note_end() named to p25_lcw_from_tdulc()), 0 for none; -1
   when the TDULC delivered no talker end. */
static int
run_tdulc_at(dsd_opts* opts, dsd_state* state, uint64_t clock) {
    state->symbol_clock = clock;
    build_lcw_words(0x0F, 0x90, 0x00, 0x0000, 0x0012, 0x3456);
    int tg = -1;
    const int before = logged_talker_ends(opts, &tg);
    processTDULC(opts, state);
    const int after = logged_talker_ends(opts, &tg);
    return after == before + 1 ? tg : -1;
}

/* A trunked voice channel whose call 0x123456 -> TG 0x1234 the trunk state machine follows, its diagnostic log in a
   fresh file. */
static int
reset_followed_call_fixture(dsd_opts* opts, dsd_state* state) {
    dsd_p25_sm_log_close(opts);
    if (g_sm_log_path[0] != '\0') {
        (void)remove(g_sm_log_path);
    }
    tune_granted_channel(opts, state);
    const int fd = dsd_test_mkstemp(g_sm_log_path, sizeof g_sm_log_path, "tdulc_sm_log");
    if (fd < 0) {
        return 0;
    }
    (void)dsd_close(fd);
    DSD_SNPRINTF(opts->p25_sm_log_file, sizeof opts->p25_sm_log_file, "%s", g_sm_log_path);
    const p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    if (p25_sm_emit_active_call(opts, state, 0, 0x1234, 0, 0x123456, 1, 0) == 0 || !ctx->slots[0].voice_active) {
        return 0;
    }
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        (void)seed_active_call(state, 0x1234, 0x123456, 0x00);
    }
    return dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
           && call.ota_target_id == 0x1234U;
}

static void
close_followed_call_fixture(dsd_opts* opts, dsd_state* state) {
    dsd_p25_sm_log_close(opts);
    if (g_sm_log_path[0] != '\0') {
        (void)remove(g_sm_log_path);
        g_sm_log_path[0] = '\0';
    }
    dsd_state_ext_free_all(state);
}

/* A TDULC's link control speaks for the call that TDULC ended, and its repeats, which follow it back to back on the
   carrier (each data unit's sync starts where the last one ends, TIA-102.BAAA-A 8.2), for that call too. A TDULC that
   ended no call, its LDUs lost, is a later transmission's terminator: an older call the slot still holds ended is not
   its call (p25p1_terminator_note_end()). Probed through the link control processTDULC() dispatches: the Motorola
   talker end names only its source and takes its talkgroup from that call. */
static int
test_tdulc_link_control_takes_only_the_call_it_ended(void) {
    static dsd_opts opts;
    static dsd_state state;
    const uint64_t start = 100000U;
    const int tg = 0x1234;
    int rc = 0;

    rc |= expect_true("fixture call", reset_followed_call_fixture(&opts, &state));
    rc |= expect_eq_int("the TDULC that ends the call speaks for the call", run_tdulc_at(&opts, &state, start), tg);
    dsd_call_snapshot call;
    rc |= expect_true("the TDULC ended the call",
                      dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_eq_int("its repeat speaks for it too", run_tdulc_at(&opts, &state, start + P25P1_TDULC_DIBITS), tg);
    rc |= expect_eq_int("and so does the repeat after it",
                        run_tdulc_at(&opts, &state, start + (uint64_t)2U * P25P1_TDULC_DIBITS), tg);
    // Much later, a voiceless transmission whose TDULC alone decoded: the ended call is not its call.
    rc |= expect_eq_int("a later TDULC speaks for no older call", run_tdulc_at(&opts, &state, start + 50000U), 0);
    rc |= expect_eq_int("nor does its own repeat", run_tdulc_at(&opts, &state, start + 50000U + P25P1_TDULC_DIBITS), 0);

    // A data unit the decoder never read between them (a TDU or LDU lost to a fade): no repeat of the TDULC that ended
    // the call.
    rc |= expect_true("fixture call 2", reset_followed_call_fixture(&opts, &state));
    rc |= expect_eq_int("ending TDULC", run_tdulc_at(&opts, &state, start), tg);
    rc |= expect_eq_int("a TDULC after a unit the decoder never read",
                        run_tdulc_at(&opts, &state, start + P25P1_TDULC_DIBITS + P25P1_TDU_DIBITS), 0);

    // A TDU that ended the call, then the same transmission's TDULCs back to back: the end is one, so its link
    // control is the call's. So is a TDULC after a TDU that followed the TDULC ending the call.
    rc |= expect_true("fixture call 5", reset_followed_call_fixture(&opts, &state));
    state.symbol_clock = start;
    processTDU(&opts, &state);
    rc |= expect_true("the TDU ended the call",
                      dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_eq_int("the TDULC after the TDU that ended the call speaks for the call",
                        run_tdulc_at(&opts, &state, start + P25P1_TDU_DIBITS), tg);
    rc |=
        expect_eq_int("and its repeat", run_tdulc_at(&opts, &state, start + P25P1_TDU_DIBITS + P25P1_TDULC_DIBITS), tg);
    state.symbol_clock = start + P25P1_TDU_DIBITS + (uint64_t)2U * P25P1_TDULC_DIBITS;
    processTDU(&opts, &state);
    rc |= expect_eq_int(
        "a TDULC after a TDU between the call's TDULCs",
        run_tdulc_at(&opts, &state, start + (uint64_t)2U * P25P1_TDU_DIBITS + (uint64_t)2U * P25P1_TDULC_DIBITS), tg);
    // A TDU much later, then a TDULC: a later transmission's end, which ended no call.
    state.symbol_clock = start + 90000U;
    processTDU(&opts, &state);
    rc |= expect_eq_int("a TDULC after a later TDU that ended no call",
                        run_tdulc_at(&opts, &state, start + 90000U + P25P1_TDU_DIBITS), 0);

    // A call another end closed (a sync loss) is not a TDULC's either, even straight after it.
    rc |= expect_true("fixture call 3", reset_followed_call_fixture(&opts, &state));
    rc |= expect_true("sync loss ends the call", dsd_call_state_end_ex(&state, 0U, 2.0, DSD_CALL_END_SYNC_LOSS) > 0);
    rc |= expect_eq_int("a TDULC after a call a sync loss ended", run_tdulc_at(&opts, &state, start), 0);

    // A carrier boundary inside the TDU that ended the call (a replay retune adopted while its tail was read): the
    // TDU stood on the carrier left, so a TDULC on the next carrier where its repeat would stand is no repeat of it.
    rc |= expect_true("fixture call 6", reset_followed_call_fixture(&opts, &state));
    state.symbol_clock = start;
    g_boundary_in_tail = 1;
    processTDU(&opts, &state);
    g_boundary_in_tail = 0;
    rc |= expect_true("the split TDU moved the carrier count", state.carrier_seq == 1U);
    rc |= expect_eq_int("a TDULC on the carrier after the split TDU",
                        run_tdulc_at(&opts, &state, start + P25P1_TDU_DIBITS), 0);

    // A new carrier between the TDULC that ended the call and the next one: that one is no repeat of it.
    rc |= expect_true("fixture call 4", reset_followed_call_fixture(&opts, &state));
    rc |= expect_eq_int("ending TDULC 4", run_tdulc_at(&opts, &state, start), tg);
    state.carrier_seq++;
    rc |= expect_eq_int("a TDULC on the next carrier", run_tdulc_at(&opts, &state, start + P25P1_TDULC_DIBITS), 0);
    close_followed_call_fixture(&opts, &state);
    return rc;
}

int
main(void) {
    int rc = 0;
    install_trunk_tuning_hooks();

    // Case 1: Retune enabled (baseline)
    build_lcw_words(0x44, 0x00, 0x00, 0x4567, 0x100A, 0x0000);
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_enable = 1;
    opts.p25_lcw_retune = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    opts.trunk_is_tuned = 0;
    opts.floating_point = 1;
    opts.audio_gain = 3.5F;
    opts.payload = 0;
    state.p25_cc_freq = 851000000;
    state.tg_hold = 0;
    const int lastsrc = 0x00ABCDEF;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.p25_chan_iden = 1;
    state.p25_iden_fdma[1].chan_type = 1;
    state.p25_iden_fdma[1].chan_spac = 100;
    state.p25_iden_fdma[1].base_freq = 851000000 / 5;
    state.p25_iden_fdma[1].trust = 2;
    state.p25_iden_fdma[1].populated = 1;
    state.p25_chan_tdma_explicit[1] = 1; // FDMA known
    state.aout_gain = 0.25F;
    g_rs_hard_result = 0;
    g_rs_soft_result = 1;
    g_rs_soft_called = 0;
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &opts, &state);
    rc |= expect_true("tdulc seed active canonical call", seed_active_call(&state, 0x7777, (uint64_t)lastsrc, 0x97));
    processTDULC(&opts, &state);
    rc |= expect_eq_int("grant called", (int)ctx->grant_count, 1);
    rc |= expect_eq_int("grant channel", ctx->vc_channel, 0x100A);
    rc |= expect_eq_int("grant svc", ctx->slots[0].svc_bits, 0x00);
    rc |= expect_eq_int("grant tg", ctx->vc_tg, 0x4567);
    rc |= expect_eq_int("grant has no invented src", ctx->vc_src, 0);
    rc |= expect_eq_int("tdulc duid count", (int)state.p25_p1_duid_tdulc, 1);
    dsd_call_snapshot ended_call;
    rc |= expect_true("tdulc retains ended canonical call", dsd_call_state_get(&state, 0U, &ended_call) > 0);
    rc |=
        expect_true("tdulc ends canonical call once",
                    ended_call.phase == DSD_CALL_PHASE_ENDED && ended_call.kind == DSD_CALL_KIND_GROUP_VOICE
                        && ended_call.ota_target_id == 0x7777 && ended_call.ota_source_id == (uint64_t)lastsrc
                        && ended_call.service_options == 0x97 && ended_call.emergency == 1 && ended_call.priority == 7);
    dsd_recent_activity_snapshot activity;
    rc |= expect_true("tdulc grant activity snapshot", dsd_recent_activity_copy_snapshot(&state, &activity) > 0);
    rc |= expect_true("tdulc grant is recent activity",
                      activity.entries[0].updated_m_ms != 0U
                          && activity.entries[0].observation.kind == DSD_CALL_KIND_GROUP_VOICE
                          && activity.entries[0].observation.ota_target_id == 0x4567
                          && activity.entries[0].observation.ota_source_id == 0U
                          && activity.entries[0].observation.channel == 0x100A);
    // The voice chain's gain level carries across calls in both output formats (issue #653).
    rc |= expect_eq_float("tdulc gain kept", state.aout_gain, 0.25F);
    rc |= expect_true("tdulc monotonic time recorded", state.p25_p1_last_tdu_m > 0.0);
    rc |= expect_true("tdulc vc sync time refreshed", state.last_vc_sync_time_m > 0.0);
    rc |= expect_eq_int("tdulc preserves dispatched grant crypto", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);

    // Case 2: Retune disabled → no grant
    build_lcw_words(0x44, 0x00, 0x00, 0x1234, 0x100A, 0x0000);
    opts.p25_lcw_retune = 0;
    p25_sm_init_ctx(ctx, &opts, &state);
    processTDULC(&opts, &state);
    rc |= expect_eq_int("retune disabled", (int)ctx->grant_count, 0);

    // Case 3: Encrypted svc under lockout reaches key-aware grant classification.
    build_lcw_words(0x44, 0x00, 0x40 /*ENC*/, 0x2222, 0x100A, 0x0000);
    opts.p25_lcw_retune = 1;
    opts.trunk_tune_enc_calls = 0;
    p25_sm_init_ctx(ctx, &opts, &state);
    processTDULC(&opts, &state);
    rc |= expect_eq_int("encrypted grant dispatched", (int)ctx->grant_count, 1);
    rc |= expect_eq_int("encrypted grant classification", state.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    // Case 4: Voice-user metadata in the terminating frame does not restart voice.
    rc |= test_voice_user_metadata_does_not_restart_call();
    rc |= test_tdulc_link_control_takes_only_the_call_it_ended();

    // Case 5: Hard Reed-Solomon failure recovered by soft reliability still dispatches LCW.
    build_lcw_words(0x44, 0x00, 0x00, 0x3456, 0x100A, 0x0000);
    dsd_state_ext_free_all(&state);
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.p25_lcw_retune = 1;
    opts.trunk_tune_enc_calls = 1;
    opts.payload = 1;
    state.p25_cc_freq = 851000000;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.p25_chan_iden = 1;
    state.p25_iden_fdma[1].chan_type = 1;
    state.p25_iden_fdma[1].chan_spac = 100;
    state.p25_iden_fdma[1].base_freq = 851000000 / 5;
    state.p25_iden_fdma[1].trust = 2;
    state.p25_iden_fdma[1].populated = 1;
    state.p25_chan_tdma_explicit[1] = 1;
    g_rs_hard_result = 1;
    g_rs_soft_result = 0;
    g_rs_soft_called = 0;
    p25_sm_init_ctx(ctx, &opts, &state);
    processTDULC(&opts, &state);
    rc |= expect_eq_int("soft rs called", g_rs_soft_called, 1);
    rc |= expect_eq_int("soft rs recovered", (int)state.p25_p1_soft_rs_ok, 1);
    rc |= expect_eq_int("soft rs fec ok", (int)state.p25_p1_voice_fec_ok, 1);
    rc |= expect_eq_int("soft rs fec err", (int)state.p25_p1_voice_fec_err, 0);
    rc |= expect_eq_int("soft rs dispatch", (int)ctx->grant_count, 1);
    rc |= expect_eq_int("soft rs grant tg", ctx->vc_tg, 0x3456);

    /* Case 6 (issue #575): the carrier boundary moved the carrier count in the trailing symbols (a replay read adopting
       a recorded retune). The link control decoded whole before the move but dispatches only after them, so its grant
       would retune for the carrier left after the boundary: nothing of it is dispatched, as on a sync loss. */
    build_lcw_words(0x44, 0x00, 0x00, 0x3456, 0x100A, 0x0000);
    g_rs_hard_result = 0;
    p25_sm_init_ctx(ctx, &opts, &state);
    g_boundary_in_tail = 1;
    processTDULC(&opts, &state);
    rc |= expect_eq_int("split tdulc moved the carrier count", (int)state.carrier_seq, 1);
    rc |= expect_eq_int("split tdulc dispatches no grant", (int)ctx->grant_count, 0);

    /* Case 7: a Signal search's yield, or a live read that gave up on a stalled input, cut the TDULC in its trailing
       symbols: from the cut every symbol is an erasure getSymbol() stood in. Its link control is dropped as one the
       carrier boundary splits is. */
    build_lcw_words(0x44, 0x00, 0x00, 0x3456, 0x100A, 0x0000);
    g_rs_hard_result = 0;
    p25_sm_init_ctx(ctx, &opts, &state);
    g_interrupt_in_tail = 1;
    processTDULC(&opts, &state);
    rc |= expect_eq_int("cut tdulc is interrupted", state.input_interrupted, 1);
    rc |= expect_eq_int("cut tdulc moves no carrier count", (int)state.carrier_seq, 1);
    rc |= expect_eq_int("cut tdulc dispatches no grant", (int)ctx->grant_count, 0);
    state.input_interrupted = 0;

    // Case 8 (issue #800): GPS link controls in the terminator.
    rc |= test_terminator_gps();
    rc |= test_terminator_gps_of_a_call_it_did_not_end();
    rc |= test_reused_state_forgets_checked_identity();

    dsd_state_ext_free_all(&state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
