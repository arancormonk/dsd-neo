// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR Tier III C_MOVE (CSBKO 57, TS 102 361-4 §7.1.1.1.3 Table 7.17): a Move TSCC PDU tells the radios its MS address
 * and Mask select to go to another control channel, the Physical Channel Number or, for the absolute escape (0xFFF),
 * the frequency the appended MV_AP block's CdefParms give (Table 7.18). It names no talkgroup, source or timeslot.
 * Its FID is 0: under another FID opcode 57 is a manufacturer's CSBK.
 *
 * The decoder prints the move's fields and acts on none of them: no activity row, no grant to the trunk state
 * machine, no tune, no slot or burst state change, and nothing learned into the channel map. Ordinary grants (logical
 * and absolute) still tune and name their channel, and a CRC-valid CSBK still anchors an unknown control channel at
 * the radio's tuned frequency in a build without librtlsdr (SoapySDR or Airspy only).
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <assert.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#ifdef USE_RTLSDR
#error "This test must compile dmr_csbk.c without USE_RTLSDR."
#endif

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// --- Stubs for external symbols referenced by dmr_csbk.c ---

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation->ota_source_id;
    (void)observation->ota_target_id;
    (void)notice;
    (void)slot;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static unsigned int g_tunes;

static dsd_trunk_tune_result
test_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    g_tunes++;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->trunk_is_tuned = 1;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

// CRC8 helper used in CSBK path (provide a trivial non-zero value)
uint8_t
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

// --- Helpers ---

#define CC_F     851000000L
#define VC_F1    852000000L
#define VC_F2    853000000L
#define VC_F3    854000000L
#define CH_F4    855000000L // 855 MHz: RX INT 855, RX STEP 0
#define VC_LPCN1 0x0010U

static const uint64_t k_t0_ns = 1000000ULL * 1000000000ULL;
static char g_out[8192];

static void
clock_at(double seconds) {
    dsd_decode_clock_test_set_ns(k_t0_ns + (uint64_t)(seconds * 1e9));
}

static void
put_bits(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

static void
init_env(dsd_opts* opts, dsd_state* state) {
    // Free what an earlier step attached (the call-state extension) before the wipe drops its pointer
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    state->trunk_cc_freq = CC_F; // mock CC
}

static void
trunk_env(dsd_opts* opts, dsd_state* state) {
    init_env(opts, state);
    opts->trunk_tune_private_calls = 1;
    opts->trunk_hangtime = 2.0f;
    state->trunk_chan_map[VC_LPCN1] = VC_F1;
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    clock_at(0.0);
    dmr_sm_init(opts, state);
    g_tunes = 0U;
}

/* A CRC-valid TV_GRANT-layout CSBK (opcode @p opcode): LPCN @p lpcn, slot @p slot, target, source. */
static void
csbk_grant(dsd_opts* opts, dsd_state* state, uint8_t opcode, uint16_t lpcn, uint8_t slot, uint32_t target,
           uint32_t source) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = opcode;
    put_bits(bits, 16U, lpcn, 12U);
    bits[28] = (uint8_t)(slot & 1U);
    put_bits(bits, 32U, target, 24U);
    put_bits(bits, 56U, source, 24U);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

/* A CRC-valid MBC grant whose channel is the absolute escape (0xFFF), defined by its CdefParms: LPCN @p apcn on
   @p rx_int MHz plus @p rx_step 125 Hz steps. Handed over as the MBC assembler does with @p appended continuation
   blocks received, or as a standalone CSBK (the header alone, the CdefParms bits then only buffer content) when 0. */
static void
csbk_absolute_grant(dsd_opts* opts, dsd_state* state, uint8_t opcode, uint16_t apcn, uint16_t rx_int, uint16_t rx_step,
                    uint8_t slot, uint32_t target, uint8_t appended) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = opcode;
    put_bits(bits, 16U, 0xFFFU, 12U);
    bits[28] = (uint8_t)(slot & 1U);
    put_bits(bits, 32U, target, 24U);
    put_bits(bits, 56U, 1234U, 24U);
    // The appended block carries its header's CSBKO (bits 98..103, TS 102 361-4 Table 7.16); CdefParms Cdeftype 0
    // (bits 112..115) stays zero; LPCN 118..129, RX INT 153..162, RX STEP 163..175
    put_bits(bits, 98U, opcode, 6U);
    put_bits(bits, 118U, apcn, 12U);
    put_bits(bits, 153U, rx_int, 10U);
    put_bits(bits, 163U, rx_step, 13U);
    if (appended == 0U) {
        dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
    } else {
        dmr_cspdu_mbc(opts, state, bits, bytes, 1U, 0U, appended);
    }
}

static void
accepted_voice(dsd_opts* opts, dsd_state* state, int slot) {
    state->dmr_confidence_voice_open[slot] = 1;
    state->dmr_confidence_voice_m[slot] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(opts, state, slot);
}

/* The slot's call, the followed context and the slot's record (what the policy route is built from) name @p channel of
   @p kind. */
static void
expect_channel(const dsd_state* state, int slot, uint32_t channel, uint8_t kind) {
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, (uint8_t)slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.channel == channel && call.channel_kind == kind);
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    assert(ctx->vc_lpcn == (int)channel && ctx->vc_channel_kind == kind);
    assert(ctx->slots[slot].channel == (int)channel && ctx->slots[slot].channel_kind == kind);
}

/* ---- C_MOVE frames (Table 7.17, and the MV_AP block of Table 7.18) ---- */

typedef struct {
    uint8_t fid;
    uint8_t mask;
    uint8_t reg;
    uint8_t backoff;
    uint16_t pcn;
    uint32_t ms;
} cmove_fields;

/* A Move TSCC PDU: LB set for a single block, clear for an MBC header. Reserved bits 16..24, 30..34 and 40..43 are 0. */
static void
build_cmove(uint8_t* bits, uint8_t* bytes, const cmove_fields* f, int mbc) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = (uint8_t)((mbc ? 0x00U : 0x80U) | 57U);
    bytes[1] = f->fid;
    put_bits(bits, 0U, bytes[0], 8U);
    put_bits(bits, 8U, f->fid, 8U);
    put_bits(bits, 25U, f->mask, 5U);
    bits[35] = (uint8_t)(f->reg & 1U);
    put_bits(bits, 36U, f->backoff, 4U);
    put_bits(bits, 44U, f->pcn, 12U);
    put_bits(bits, 56U, f->ms, 24U);
}

/* The MV_AP block appended to an MBC move: Cdeftype 0, CdefParms naming LPCN @p lpcn on @p rx_int MHz plus
   @p rx_step 125 Hz steps (TX the same). */
static void
append_mv_ap(uint8_t* bits, uint16_t lpcn, uint16_t rx_int, uint16_t rx_step) {
    put_bits(bits, 96U, 0x80U | 57U, 8U); // LB 1, PF 0, CSBKO of the header
    put_bits(bits, 118U, lpcn, 12U);
    put_bits(bits, 130U, rx_int, 10U);
    put_bits(bits, 140U, rx_step, 13U);
    put_bits(bits, 153U, rx_int, 10U);
    put_bits(bits, 163U, rx_step, 13U);
}

/* Send the frame through the CSBK dispatcher, its terminal output in g_out: as a standalone CSBK when @p appended is 0,
   else as the MBC assembler hands over a header with @p appended continuation blocks received. */
static void
run_captured(dsd_opts* opts, dsd_state* state, uint8_t* bits, uint8_t* bytes, uint8_t appended) {
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_t3_cmove") == 0);
    if (appended == 0U) {
        dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
    } else {
        dmr_cspdu_mbc(opts, state, bits, bytes, 1U, 0U, appended);
    }
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, g_out, sizeof g_out) == 0);
}

static void
expect_out(const char* needle, int present, const char* tag) {
    const int found = strstr(g_out, needle) != NULL;
    if (found != present) {
        DSD_FPRINTF(stderr, "FAIL: %s: %s '%s' in '%s'\n", tag, present ? "missing" : "unexpected", needle, g_out);
        assert(0);
    }
}

/* What a C_MOVE must leave alone: the activity rows, the burst indicators, the slots' calls, the trunk machine and the
   tuner. */
typedef struct {
    dsd_recent_activity_snapshot recent;
    int recent_ok;
    int burst_l;
    int burst_r;
    long vc_freq[2];
    int tuned;
    unsigned int tunes;
    unsigned int tune_count;
    dmr_sm_ctx_t sm;
    dsd_call_snapshot call[2];
    int call_ok[2];
} cmove_view;

static void
view_take(const dsd_state* state, const dsd_opts* opts, cmove_view* v) {
    DSD_MEMSET(v, 0, sizeof(*v));
    v->recent_ok = dsd_recent_activity_copy_snapshot(state, &v->recent);
    v->burst_l = state->dmrburstL;
    v->burst_r = state->dmrburstR;
    v->vc_freq[0] = state->trunk_vc_freq[0];
    v->vc_freq[1] = state->trunk_vc_freq[1];
    v->tuned = opts->trunk_is_tuned;
    v->tunes = g_tunes;
    v->tune_count = state->p25_sm_tune_count;
    v->sm = *dmr_sm_get_ctx();
    for (uint8_t s = 0; s < 2U; s++) {
        v->call_ok[s] = dsd_call_state_get(state, s, &v->call[s]);
    }
}

static int
recent_entries_equal(const dsd_recent_activity_snapshot* a, const dsd_recent_activity_snapshot* b) {
    for (size_t i = 0; i < (size_t)DSD_RECENT_ACTIVITY_COUNT; i++) {
        const dsd_call_observation* x = &a->entries[i].observation;
        const dsd_call_observation* y = &b->entries[i].observation;
        if (x->kind != y->kind || x->ota_target_id != y->ota_target_id || x->ota_source_id != y->ota_source_id
            || x->channel != y->channel || x->slot != y->slot
            || strcmp(a->entries[i].notice, b->entries[i].notice) != 0) {
            return 0;
        }
    }
    return 1;
}

static void
expect_untouched(const dsd_state* state, const dsd_opts* opts, const cmove_view* before, const char* tag) {
    cmove_view after;
    view_take(state, opts, &after);
    int ok = 1;
    if (after.recent_ok != before->recent_ok
        || (after.recent_ok > 0 && !recent_entries_equal(&after.recent, &before->recent))) {
        DSD_FPRINTF(stderr, "FAIL: %s: an activity row changed\n", tag);
        ok = 0;
    }
    if (after.burst_l != before->burst_l || after.burst_r != before->burst_r) {
        DSD_FPRINTF(stderr, "FAIL: %s: burst state %d/%d -> %d/%d\n", tag, before->burst_l, before->burst_r,
                    after.burst_l, after.burst_r);
        ok = 0;
    }
    if (after.tunes != before->tunes || after.tune_count != before->tune_count || after.tuned != before->tuned
        || after.vc_freq[0] != before->vc_freq[0] || after.vc_freq[1] != before->vc_freq[1]) {
        DSD_FPRINTF(stderr, "FAIL: %s: the tuner moved (%u -> %u tunes, VC %ld -> %ld)\n", tag, before->tunes,
                    after.tunes, before->vc_freq[0], after.vc_freq[0]);
        ok = 0;
    }
    if (after.sm.state != before->sm.state || after.sm.vc_freq_hz != before->sm.vc_freq_hz
        || after.sm.vc_slot != before->sm.vc_slot || after.sm.vc_tg != before->sm.vc_tg
        || after.sm.vc_lpcn != before->sm.vc_lpcn || after.sm.slots[0].target != before->sm.slots[0].target
        || after.sm.slots[1].target != before->sm.slots[1].target) {
        DSD_FPRINTF(stderr, "FAIL: %s: the trunk machine changed\n", tag);
        ok = 0;
    }
    for (int s = 0; s < 2; s++) {
        if (after.call_ok[s] != before->call_ok[s]
            || (after.call_ok[s] > 0
                && (after.call[s].phase != before->call[s].phase
                    || after.call[s].ota_target_id != before->call[s].ota_target_id))) {
            DSD_FPRINTF(stderr, "FAIL: %s: slot %d's call changed\n", tag, s + 1);
            ok = 0;
        }
    }
    assert(ok);
}

/* The three places a C_MOVE is heard: while a voice channel call is followed (with a companion row on the other slot),
   on the control channel with nothing followed, and with trunking off. */
typedef enum {
    CMOVE_CTX_FOLLOWING = 0,
    CMOVE_CTX_ON_CC,
    CMOVE_CTX_NO_TRUNKING,
} cmove_ctx;

static void
cmove_context(dsd_opts* opts, dsd_state* state, cmove_ctx where) {
    trunk_env(opts, state);
    state->trunk_chan_map[2] = VC_F3; // what the grant layout misreads this move's channel as (LPCN 2)
    state->trunk_chan_map[5] = VC_F2; // the move's PCN 5
    if (where == CMOVE_CTX_FOLLOWING) {
        csbk_grant(opts, state, 49U, VC_LPCN1, 0, 101, 1234);
        assert(state->trunk_vc_freq[0] == VC_F1);
        clock_at(0.5);
        accepted_voice(opts, state, 0);
        const dsd_call_observation companion = {.protocol = DSD_SYNC_DMR_BS_VOICE_POS,
                                                .slot = 1U,
                                                .kind = DSD_CALL_KIND_GROUP_VOICE,
                                                .ota_target_id = 202U};
        (void)dsd_recent_activity_publish(state, 1U, &companion, "companion", 0U);
    } else if (where == CMOVE_CTX_NO_TRUNKING) {
        opts->trunk_enable = 0;
    }
    state->dmrburstL = 16;
    state->dmrburstR = 16;
    clock_at(0.6);
}

/* The spec frame (Mask 8, Reg 1, Backoff 3, PCN 5, MS 0x001234), which the grant layout reads as LPCN 2, TS 1 and
   TG 1245189: printed with its own fields, acted on nowhere. */
static void
cmove_spec_frame_prints_only(dsd_opts* opts, dsd_state* state) {
    static const cmove_fields k_move = {.fid = 0x00U, .mask = 8U, .reg = 1U, .backoff = 3U, .pcn = 5U, .ms = 0x001234U};
    static const char* const k_names[] = {"following a call", "on the control channel", "trunking off"};
    for (int where = CMOVE_CTX_FOLLOWING; where <= CMOVE_CTX_NO_TRUNKING; where++) {
        cmove_context(opts, state, (cmove_ctx)where);
        cmove_view before;
        view_take(state, opts, &before);
        uint8_t bits[256];
        uint8_t bytes[48];
        build_cmove(bits, bytes, &k_move, 0);
        run_captured(opts, state, bits, bytes, 0U);
        expect_untouched(state, opts, &before, k_names[where]);
        expect_out("Move TSCC (C_MOVE)", 1, k_names[where]);
        expect_out("Mask: 8; Reg: 1; Backoff: 3; PCN: 0005; MS: 00004660", 1, k_names[where]);
        expect_out("Frequency: 853.000000 MHz", 1, k_names[where]);
        expect_out("1245189", 0, k_names[where]);
    }

    // The move leaves an idle control channel ready: the next grant is followed.
    cmove_context(opts, state, CMOVE_CTX_ON_CC);
    uint8_t bits[256];
    uint8_t bytes[48];
    build_cmove(bits, bytes, &k_move, 0);
    run_captured(opts, state, bits, bytes, 0U);
    csbk_grant(opts, state, 49U, VC_LPCN1, 0, 101, 1234);
    assert(g_tunes == 1U && state->trunk_vc_freq[0] == VC_F1 && dmr_sm_get_ctx()->vc_tg == 101);
}

/* The absolute escape: PCN 0xFFF on an MBC header, the TSCC's frequency in the appended MV_AP block's CdefParms.
   Printed; not tuned, not published, and not learned into the channel map. Without an appended block (a single block)
   there is no frequency to print. */
static void
cmove_mbc_channel_form_prints_only(dsd_opts* opts, dsd_state* state) {
    static const cmove_fields k_move = {
        .fid = 0x00U, .mask = 0U, .reg = 0U, .backoff = 2U, .pcn = 0xFFFU, .ms = 0x000000U};
    for (int where = CMOVE_CTX_FOLLOWING; where <= CMOVE_CTX_NO_TRUNKING; where++) {
        cmove_context(opts, state, (cmove_ctx)where);
        cmove_view before;
        view_take(state, opts, &before);
        uint8_t bits[256];
        uint8_t bytes[48];
        build_cmove(bits, bytes, &k_move, 1);
        append_mv_ap(bits, 0x123U, 855U, 0U);
        run_captured(opts, state, bits, bytes, 1U);
        expect_untouched(state, opts, &before, "mbc move");
        assert(state->trunk_chan_map[0x123] == 0);
        expect_out("Move TSCC (C_MOVE) - Absolute", 1, "mbc move");
        expect_out("PCN: 4095; MS: 00000000", 1, "mbc move");
        expect_out("TSCC APCN: 0291; RX INT: 855; RX STEP: 0;", 1, "mbc move");
        expect_out("Frequency: 855.000000 MHz", 1, "mbc move");
    }

    /* No MV_AP block received: a single block (LB set), or an MBC header (LB clear) that reached the dispatcher on its
       own, as a standalone CSBK. Bits past the first block are padding there, never a frequency. */
    for (int mbc_header = 0; mbc_header <= 1; mbc_header++) {
        cmove_context(opts, state, CMOVE_CTX_ON_CC);
        cmove_view before;
        view_take(state, opts, &before);
        uint8_t bits[256];
        uint8_t bytes[48];
        build_cmove(bits, bytes, &k_move, mbc_header);
        append_mv_ap(bits, 0x123U, 855U, 0U);
        run_captured(opts, state, bits, bytes, 0U);
        expect_untouched(state, opts, &before, "absolute move without its block");
        expect_out("Move TSCC (C_MOVE) - Absolute", 1, "absolute move without its block");
        expect_out("Frequency", 0, "absolute move without its block");
        expect_out("APCN", 0, "absolute move without its block");
        expect_out("MV_AP", 0, "absolute move without its block");
    }

    /* A caller that hands over exactly one decoded block (96 bits, 12 octets): nothing past it is read. */
    cmove_context(opts, state, CMOVE_CTX_ON_CC);
    uint8_t full_bits[256];
    uint8_t full_bytes[48];
    build_cmove(full_bits, full_bytes, &k_move, 1);
    uint8_t* one_block_bits = (uint8_t*)malloc(96U);
    uint8_t* one_block_bytes = (uint8_t*)malloc(12U);
    assert(one_block_bits != NULL && one_block_bytes != NULL);
    DSD_MEMCPY(one_block_bits, full_bits, 96U);
    DSD_MEMCPY(one_block_bytes, full_bytes, 12U);
    run_captured(opts, state, one_block_bits, one_block_bytes, 0U);
    free(one_block_bits);
    free(one_block_bytes);
    expect_out("Move TSCC (C_MOVE) - Absolute", 1, "one-block header");
    expect_out("Frequency", 0, "one-block header");
}

/* Opcode 57 under a manufacturer's FID is not a C_MOVE: it prints as a manufacturer CSBK and nothing else. */
static void
cmove_nonzero_fid_ignored(dsd_opts* opts, dsd_state* state) {
    static const cmove_fields k_move = {.fid = 0x10U, .mask = 8U, .reg = 1U, .backoff = 3U, .pcn = 5U, .ms = 0x001234U};
    for (int where = CMOVE_CTX_FOLLOWING; where <= CMOVE_CTX_NO_TRUNKING; where++) {
        cmove_context(opts, state, (cmove_ctx)where);
        cmove_view before;
        view_take(state, opts, &before);
        uint8_t bits[256];
        uint8_t bytes[48];
        build_cmove(bits, bytes, &k_move, 0);
        run_captured(opts, state, bits, bytes, 0U);
        expect_untouched(state, opts, &before, "FID 0x10 opcode 57");
        expect_out("C_MOVE", 0, "FID 0x10 opcode 57");
        expect_out("Manufacturer CSBK: FID 10; Opcode 39;", 1, "FID 0x10 opcode 57");
    }
}

/* ---- Grants: the call's channel (its LPCN), one resolved value for the machine, the slot record and the call ---- */

/* A logical grant names its LPCN, 1..4094 alike; LPCN 0 (CHNULL) names no channel, and nothing to tune. */
static void
test_t3_grant_lpcn_kind(dsd_opts* opts, dsd_state* state) {
    static const uint16_t lpcns[] = {12U, 1U, 4094U};
    for (size_t i = 0; i < sizeof lpcns / sizeof lpcns[0]; i++) {
        trunk_env(opts, state);
        state->trunk_chan_map[lpcns[i]] = CH_F4;
        csbk_grant(opts, state, 49U, lpcns[i], 0, 101, 1234);
        assert(state->trunk_vc_freq[0] == CH_F4);
        clock_at(0.5);
        accepted_voice(opts, state, 0);
        expect_channel(state, 0, lpcns[i], DSD_CALL_CHANNEL_LPCN);
    }
    trunk_env(opts, state);
    csbk_grant(opts, state, 49U, 0U, 0, 101, 1234);
    assert(state->trunk_vc_freq[0] == 0 && dmr_sm_get_ctx()->state != DMR_SM_TUNED);
}

/* An absolute grant (0xFFF) is on the channel its MBC defines: CdefParms LPCN 1..4094, which the channel map learns. */
static void
test_t3_absolute_grant_uses_mbc_lpcn(dsd_opts* opts, dsd_state* state) {
    static const uint16_t lpcns[] = {37U, 1U, 4094U};
    for (size_t i = 0; i < sizeof lpcns / sizeof lpcns[0]; i++) {
        trunk_env(opts, state);
        csbk_absolute_grant(opts, state, 49U, lpcns[i], 855U, 0U, 0, 101, 1U);
        assert(state->trunk_vc_freq[0] == CH_F4);
        assert(state->trunk_chan_map[lpcns[i]] == CH_F4);
        clock_at(0.5);
        accepted_voice(opts, state, 0);
        expect_channel(state, 0, lpcns[i], DSD_CALL_CHANNEL_LPCN);
    }
}

/* An absolute grant whose CdefParms LPCN is no channel (0, 4095) is still followed on its frequency, but names no
   channel, and the channel map learns nothing from it. */
static void
test_t3_absolute_grant_invalid_mbc_lpcn(dsd_opts* opts, dsd_state* state) {
    static const uint16_t lpcns[] = {0U, 4095U};
    for (size_t i = 0; i < sizeof lpcns / sizeof lpcns[0]; i++) {
        trunk_env(opts, state);
        csbk_absolute_grant(opts, state, 49U, lpcns[i], 855U, 0U, 0, 101, 1U);
        assert(state->trunk_vc_freq[0] == CH_F4);
        assert(state->trunk_chan_map[0] == 0 && state->trunk_chan_map[4095] == 0);
        clock_at(0.5);
        accepted_voice(opts, state, 0);
        expect_channel(state, 0, 0U, DSD_CALL_CHANNEL_NONE);
    }
}

/* An absolute grant is followed only with the block that gives its frequency: the header alone (a standalone CSBK)
   names no frequency, so it publishes none, tunes nothing and teaches the channel map nothing; the same grant with
   its continuation tunes to the block's frequency. */
static void
test_t3_absolute_grant_needs_its_block(dsd_opts* opts, dsd_state* state) {
    trunk_env(opts, state);
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_t3_abs_grant") == 0);
    csbk_absolute_grant(opts, state, 49U, 37U, 855U, 0U, 0, 101, 0U);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, g_out, sizeof g_out) == 0);
    assert(g_tunes == 0U && state->trunk_vc_freq[0] == 0 && opts->trunk_is_tuned == 0);
    assert(dmr_sm_get_ctx()->state != DMR_SM_TUNED);
    assert(state->trunk_chan_map[37] == 0);
    expect_out("TV_GRANT", 1, "lone absolute grant");
    expect_out("Frequency", 0, "lone absolute grant");
    expect_out("RX APCN", 0, "lone absolute grant");
    dsd_recent_activity_snapshot recent;
    if (dsd_recent_activity_copy_snapshot(state, &recent) > 0
        && recent.entries[0].observation.kind != DSD_CALL_KIND_UNKNOWN) {
        assert(recent.entries[0].observation.frequency_hz == 0 && recent.entries[0].observation.channel == 0U);
    }

    // With trunking off a grant's frequency would be written as the VC frequency: none either.
    trunk_env(opts, state);
    opts->trunk_enable = 0;
    csbk_absolute_grant(opts, state, 49U, 37U, 855U, 0U, 0, 101, 0U);
    assert(state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);

    trunk_env(opts, state);
    csbk_absolute_grant(opts, state, 49U, 37U, 855U, 0U, 0, 101, 1U);
    assert(g_tunes == 1U && state->trunk_vc_freq[0] == CH_F4 && state->trunk_chan_map[37] == CH_F4);
}

/* With no CC known yet, a CRC-valid CSBK received on a radio input anchors the CC at the tuned frequency. */
static void
test_csbk_anchors_unknown_cc(dsd_opts* opts, dsd_state* state) {
    static const cmove_fields k_move = {.fid = 0x00U, .mask = 8U, .reg = 1U, .backoff = 3U, .pcn = 5U, .ms = 0x001234U};
    init_env(opts, state);
    state->trunk_cc_freq = 0;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 851012500U;
    uint8_t bits[256];
    uint8_t bytes[48];
    build_cmove(bits, bytes, &k_move, 0);
    run_captured(opts, state, bits, bytes, 0U);
    assert(opts->trunk_is_tuned == 0);
    assert(state->trunk_cc_freq == 851012500);
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    static dsd_opts opts;
    static dsd_state state;
    init_env(&opts, &state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_to_freq,
        .return_to_cc_request = test_return_to_cc,
    });
    dsd_decode_clock_use_test(k_t0_ns);

    cmove_spec_frame_prints_only(&opts, &state);
    cmove_mbc_channel_form_prints_only(&opts, &state);
    cmove_nonzero_fid_ignored(&opts, &state);
    test_t3_grant_lpcn_kind(&opts, &state);
    test_t3_absolute_grant_uses_mbc_lpcn(&opts, &state);
    test_t3_absolute_grant_invalid_mbc_lpcn(&opts, &state);
    test_t3_absolute_grant_needs_its_block(&opts, &state);
    test_csbk_anchors_unknown_cc(&opts, &state);

    dsd_decode_clock_use_system();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_state_ext_free_all(&state);
    printf("DMR_T3_CMOVE: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
