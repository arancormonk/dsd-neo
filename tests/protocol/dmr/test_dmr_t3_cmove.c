// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * C_MOVE is Move TSCC (TS 102 361-4 Table 7.17): Mask 25-29, Reg 35, Backoff 36-39, the TSCC's Physical Channel Number
 * 44-55 and the MS individual address 56-79. It is informational: a logical move prints and changes nothing, an
 * absolute one (0xFFF) teaches the channel map the TSCC its MV_AP block names (SITE when checked, UNVERIFIED when -F
 * accepted it as RAS) and adds it as a current-site control channel candidate. No C_MOVE follows a call, publishes an
 * activity row or touches the burst indicators. Also verify that a CRC-valid CSBK anchors an unknown control channel
 * at the radio's tuned frequency in a build without librtlsdr (SoapySDR or Airspy only), and the grant channel rules
 * (logical and absolute LPCNs).
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <assert.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_mbc_test_support.h"
#include "dmr_t3_channels_internal.h"
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

int
dsd_event_emit_system_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const char* notice) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)notice;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static dsd_trunk_tune_result
test_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
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
static void
init_env(dsd_opts* opts, dsd_state* state) {
    // Free what an earlier step attached (the call-state extension) before the wipe drops its pointer
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    state->trunk_cc_freq = 851000000; // mock CC
}

// Compose a minimal absolute channel grant (LPCN 0xFFF) + MBC absolute channel parms into bit and byte arrays
static void
build_grant_apcn(uint8_t* bits, uint8_t* bytes, uint8_t opcode, uint16_t apcn, uint16_t rx_int_mhz,
                 uint16_t rx_step_125hz, uint8_t slot, uint32_t target) {
    DSD_MEMSET(bits, 0, 256);
    DSD_MEMSET(bytes, 0, 48);
    bytes[0] = (uint8_t)(opcode & 0x3F);
    // LPCN field (bits 16..27) = 0xFFF (absolute)
    for (int i = 0; i < 12; i++) {
        bits[16 + i] = ((0xFFF >> (11 - i)) & 1);
    }
    // Slot bit (bit 28)
    bits[28] = (slot & 1);
    // Target (bits 32..55)
    for (int i = 0; i < 24; i++) {
        bits[32 + i] = (uint8_t)((target >> (23 - i)) & 1u);
    }
    // MBC CDEFTYPE (bits 112..115) = 0 (absolute channel parms): already zeroed
    // APCN (bits 118..129)
    for (int i = 0; i < 12; i++) {
        bits[118 + i] = ((apcn >> (11 - i)) & 1);
    }
    // RX_INT (bits 153..162) 10 bits
    for (int i = 0; i < 10; i++) {
        bits[153 + i] = ((rx_int_mhz >> (9 - i)) & 1);
    }
    // RX_STEP (bits 163..175) 13 bits
    for (int i = 0; i < 13; i++) {
        bits[163 + i] = ((rx_step_125hz >> (12 - i)) & 1);
    }
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

/* ---- Shared environment ---- */

#define MV_F1    852000000L
#define MV_F2    853000000L
#define MV_F3    854000000L
#define MV_LPCN1 0x0010U
#define MV_LPCN2 0x0011U
#define MV_LPCN3 0x0012U

static const uint64_t k_t0_ns = 1000000ULL * 1000000000ULL;

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

/* A single CSBK laid out as a channel grant: LPCN 16-27, slot 28, target 32-55, source 56-79. */
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

static void
move_env(dsd_opts* opts, dsd_state* state) {
    init_env(opts, state);
    opts->trunk_tune_private_calls = 1;
    opts->trunk_hangtime = 2.0f;
    state->trunk_chan_map[MV_LPCN1] = MV_F1;
    state->trunk_chan_map[MV_LPCN2] = MV_F2;
    state->trunk_chan_map[MV_LPCN3] = MV_F3;
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    clock_at(0.0);
    dmr_sm_init(opts, state);
}

static void
accepted_voice(dsd_opts* opts, dsd_state* state, int slot) {
    state->dmr_confidence_voice_open[slot] = 1;
    state->dmr_confidence_voice_m[slot] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(opts, state, slot);
}

static int
recent_row_target(const dsd_state* state, uint8_t index, uint64_t* target) {
    dsd_recent_activity_snapshot recent;
    if (dsd_recent_activity_copy_snapshot(state, &recent) <= 0
        || recent.entries[index].observation.kind == DSD_CALL_KIND_UNKNOWN) {
        return 0;
    }
    *target = recent.entries[index].observation.ota_target_id;
    return 1;
}

/* ---- C_MOVE: Move TSCC ---- */

/* A Move TSCC CSBK (Table 7.17): Mask, Reg, Backoff, Physical Channel Number @p pcn and MS address @p ms. With @p pcn
   0xFFF and @p rx_int nonzero, its MV_AP block (Table 7.18) defines CHAN @p apcn on @p rx_int MHz plus @p rx_step
   125 Hz steps, and it is handed over as a two-block MBC; otherwise as a single CSBK. */
typedef struct {
    uint8_t mask;
    uint8_t reg;
    uint8_t backoff;
    uint16_t pcn;
    uint32_t ms;
    uint16_t apcn;
    uint16_t rx_int;
    uint16_t rx_step;
} move_tscc;

static void
csbk_move_tscc(dsd_opts* opts, dsd_state* state, const move_tscc* mv) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 57U;
    bits[0] = 1U;
    put_bits(bits, 2U, 57U, 6U);
    put_bits(bits, 25U, mv->mask, 5U);
    bits[35] = (uint8_t)(mv->reg & 1U);
    put_bits(bits, 36U, mv->backoff, 4U);
    put_bits(bits, 44U, mv->pcn, 12U);
    put_bits(bits, 56U, mv->ms, 24U);
    if (mv->pcn == 0xFFFU && mv->rx_int != 0U) {
        put_bits(bits, 118U, mv->apcn, 12U);
        put_bits(bits, 153U, mv->rx_int, 10U);
        put_bits(bits, 163U, mv->rx_step, 13U);
        dsd_test_dmr_cspdu_mbc(opts, state, bits, bytes);
        return;
    }
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

static char g_out[8192];

/* csbk_move_tscc() with what it printed in g_out. */
static void
csbk_move_tscc_captured(dsd_opts* opts, dsd_state* state, const move_tscc* mv) {
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_t3_cmove") == 0);
    csbk_move_tscc(opts, state, mv);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, g_out, sizeof g_out) == 0);
}

static int
candidates_count(const dsd_state* state) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    return cc != NULL ? cc->count : 0;
}

static int
candidate_flags(const dsd_state* state, long freq, uint8_t* flags) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    for (int i = 0; cc != NULL && i < cc->count; i++) {
        if (cc->candidates[i] == freq) {
            *flags = cc->flags[i];
            return 1;
        }
    }
    return 0;
}

/* The followed call on MV_LPCN1 slot 1 (TG 101), its companion row on slot 2 and both burst indicators set. */
static void
follow_call_with_companion(dsd_opts* opts, dsd_state* state) {
    move_env(opts, state);
    csbk_grant(opts, state, 49U, MV_LPCN1, 0, 101, 1234);
    assert(state->trunk_vc_freq[0] == MV_F1);
    clock_at(0.5);
    accepted_voice(opts, state, 0);
    const dsd_call_observation companion = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS, .slot = 1U, .kind = DSD_CALL_KIND_GROUP_VOICE, .ota_target_id = 202U};
    (void)dsd_recent_activity_publish(state, 1U, &companion, "companion", 0U);
    state->dmrburstL = 16;
    state->dmrburstR = 16;
}

/* Nothing of the followed call changed: its carrier, slot and talkgroup, the tune count, both burst indicators and the
   companion's activity row. */
static void
expect_followed_call_untouched(const dsd_state* state, unsigned int tunes) {
    uint64_t target = 0;
    assert(state->trunk_vc_freq[0] == MV_F1 && state->p25_sm_tune_count == tunes);
    assert(dmr_sm_get_ctx()->state == DMR_SM_TUNED && dmr_sm_get_ctx()->vc_slot == 0);
    assert(dmr_sm_get_ctx()->vc_tg == 101);
    assert(state->dmrburstL == 16 && state->dmrburstR == 16);
    const int companion = recent_row_target(state, 1U, &target);
    assert(companion && target == 202U);
}

/* A logical Move TSCC prints its fields as Table 7.17 places them and the TSCC's frequency when the map has it, and
   changes nothing: no retune, no channel map row, no candidate, no activity row. */
static void
test_logical_move_tscc_is_informational(dsd_opts* opts, dsd_state* state) {
    follow_call_with_companion(opts, state);
    const unsigned int tunes = state->p25_sm_tune_count;
    const uint32_t revision = dmr_t3_chan_revision(state);
    const move_tscc mv = {.mask = 3U, .reg = 1U, .backoff = 5U, .pcn = MV_LPCN3, .ms = 4321U};
    csbk_move_tscc_captured(opts, state, &mv);
    assert(strstr(g_out, " Move TSCC: MS 4321 to channel 18 (854.000000 MHz)") != NULL);
    assert(strstr(g_out, "Mask: 3; Reg: 1; Backoff: 5;") != NULL);
    expect_followed_call_untouched(state, tunes);
    assert(dmr_t3_chan_revision(state) == revision && candidates_count(state) == 0);

    // A TSCC the map has no row for is printed without a frequency, and still learns nothing.
    const move_tscc unmapped = {.pcn = 0x0123U, .ms = 77U};
    csbk_move_tscc_captured(opts, state, &unmapped);
    assert(strstr(g_out, " Move TSCC: MS 77 to channel 291") != NULL && strstr(g_out, "MHz") == NULL);
    expect_followed_call_untouched(state, tunes);
    assert(state->trunk_chan_map[0x0123U] == 0 && candidates_count(state) == 0);
}

#define MV_TSCC_HZ 453500000L // 453 MHz + 4000 x 125 Hz

/* An absolute Move TSCC learns the TSCC its MV_AP names, SITE when checked and UNVERIFIED when -F accepted it as RAS,
   and adds it as a current-site candidate unless it is the control channel the receiver is on; one whose CRC failed
   does neither, and a single block naming 0xFFF (no MV_AP) is refused. None of them retunes. */
static void
test_absolute_move_tscc_learns_the_tscc(dsd_opts* opts, dsd_state* state) {
    static const struct {
        uint8_t crc;
        uint8_t ras_signatures;
        dmr_chan_origin_e want;
    } cases[] = {
        {0U, 0U, DMR_CHAN_ORIGIN_SITE},
        {DSD_CRC_RAS_UNVERIFIED, 2U, DMR_CHAN_ORIGIN_UNVERIFIED},
        {DSD_CRC_FAILED, 2U, DMR_CHAN_ORIGIN_NONE},
    };

    const move_tscc mv = {.pcn = 0xFFFU, .ms = 4321U, .apcn = 0x0123U, .rx_int = 453U, .rx_step = 4000U};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        move_env(opts, state);
        state->currentslot = 0;
        state->event_crc_invalid[0] = cases[i].crc;
        state->dmr_ras_signatures = cases[i].ras_signatures;
        csbk_move_tscc_captured(opts, state, &mv);
        state->event_crc_invalid[0] = 0U;
        assert(strstr(g_out, " Move TSCC: MS 4321 to channel 291 (453.500000 MHz)") != NULL);
        assert(opts->trunk_is_tuned == 0 && state->p25_sm_tune_count == 0U);
        uint8_t flags = 0U;
        if (cases[i].want == DMR_CHAN_ORIGIN_NONE) {
            assert(state->trunk_chan_map[0x0123U] == 0 && candidates_count(state) == 0);
            continue;
        }
        assert(state->trunk_chan_map[0x0123U] == MV_TSCC_HZ);
        assert(dmr_t3_chan_origin(state, 0x0123U) == cases[i].want);
        const int listed = candidate_flags(state, MV_TSCC_HZ, &flags);
        assert(listed && (flags & DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) != 0U);
    }

    // A single-block C_MOVE naming 0xFFF carries no MV_AP: malformed, it is refused before any of its effects.
    move_env(opts, state);
    const move_tscc single = {.pcn = 0xFFFU, .ms = 4321U};
    csbk_move_tscc_captured(opts, state, &single);
    assert(strstr(g_out, "Malformed Control Signalling Block (CSBKO 39); ignored") != NULL);
    assert(strstr(g_out, "Move TSCC: MS 4321") == NULL);
    assert(state->trunk_chan_map[0x0123U] == 0 && candidates_count(state) == 0);

    // A move naming the control channel the receiver is on teaches the map its channel, but it is no candidate.
    move_env(opts, state);
    state->trunk_cc_freq = MV_TSCC_HZ;
    csbk_move_tscc(opts, state, &mv);
    assert(state->trunk_chan_map[0x0123U] == MV_TSCC_HZ);
    assert(dmr_t3_chan_origin(state, 0x0123U) == DMR_CHAN_ORIGIN_SITE && candidates_count(state) == 0);

    // While a call is followed an absolute move still learns, and leaves the call alone.
    follow_call_with_companion(opts, state);
    const unsigned int tunes = state->p25_sm_tune_count;
    csbk_move_tscc(opts, state, &mv);
    expect_followed_call_untouched(state, tunes);
    uint8_t flags = 0U;
    const int listed = candidate_flags(state, MV_TSCC_HZ, &flags);
    assert(state->trunk_chan_map[0x0123U] == MV_TSCC_HZ && listed);
}

/* A C_MOVE whose bits, read with a channel grant's layout, would name a mapped LPCN, the other slot and the followed
   talkgroup neither follows nor publishes anything, on a voice channel or on the control channel. */
static void
test_grant_layout_move_follows_nothing(dsd_opts* opts, dsd_state* state) {
    follow_call_with_companion(opts, state);
    const unsigned int tunes = state->p25_sm_tune_count;
    csbk_grant(opts, state, 57U, MV_LPCN2, 1, 101, 1234);
    expect_followed_call_untouched(state, tunes);
    csbk_grant(opts, state, 57U, MV_LPCN1, 1, 101, 1234);
    expect_followed_call_untouched(state, tunes);

    move_env(opts, state);
    state->dmrburstL = 9;
    state->dmrburstR = 9;
    uint64_t target = 0;
    csbk_grant(opts, state, 57U, MV_LPCN2, 0, 101, 1234);
    assert(opts->trunk_is_tuned == 0 && state->p25_sm_tune_count == 0U);
    const int row0 = recent_row_target(state, 0U, &target);
    const int row1 = recent_row_target(state, 1U, &target);
    assert(!row0 && !row1);
    assert(state->dmrburstL == 9 && state->dmrburstR == 9);
}

/* ---- The call's channel (its LPCN): one resolved value for the machine, the slot record (the policy route) and the
   call ---- */

#define CH_F4 855000000L // 855 MHz: RX INT 855, RX STEP 0

/* A grant whose channel is the absolute escape (0xFFF), defined by the MBC's CdefParms: LPCN @p apcn on @p rx_int MHz
   plus @p rx_step 125 Hz steps. */
static void
csbk_absolute(dsd_opts* opts, dsd_state* state, uint8_t opcode, uint16_t apcn, uint16_t rx_int, uint16_t rx_step,
              uint8_t slot, uint32_t target) {
    uint8_t bits[256];
    uint8_t bytes[48];
    build_grant_apcn(bits, bytes, opcode, apcn, rx_int, rx_step, slot, target);
    put_bits(bits, 56U, 1234U, 24U);
    dsd_test_dmr_cspdu_mbc(opts, state, bits, bytes);
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

/* A logical grant names its LPCN, 1..4094 alike; LPCN 0 (CHNULL) names no channel, and nothing to tune. */
static void
test_t3_grant_lpcn_kind(dsd_opts* opts, dsd_state* state) {
    static const uint16_t lpcns[] = {12U, 1U, 4094U};
    for (size_t i = 0; i < sizeof lpcns / sizeof lpcns[0]; i++) {
        move_env(opts, state);
        state->trunk_chan_map[lpcns[i]] = CH_F4;
        csbk_grant(opts, state, 49U, lpcns[i], 0, 101, 1234);
        assert(state->trunk_vc_freq[0] == CH_F4);
        clock_at(0.5);
        accepted_voice(opts, state, 0);
        expect_channel(state, 0, lpcns[i], DSD_CALL_CHANNEL_LPCN);
    }
    move_env(opts, state);
    csbk_grant(opts, state, 49U, 0U, 0, 101, 1234);
    assert(state->trunk_vc_freq[0] == 0 && dmr_sm_get_ctx()->state != DMR_SM_TUNED);
}

/* An absolute grant (0xFFF) is on the channel its MBC defines: CdefParms LPCN 1..4094, which the channel map learns. */
static void
test_t3_absolute_grant_uses_mbc_lpcn(dsd_opts* opts, dsd_state* state) {
    static const uint16_t lpcns[] = {37U, 1U, 4094U};
    for (size_t i = 0; i < sizeof lpcns / sizeof lpcns[0]; i++) {
        move_env(opts, state);
        csbk_absolute(opts, state, 49U, lpcns[i], 855U, 0U, 0, 101);
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
        move_env(opts, state);
        csbk_absolute(opts, state, 49U, lpcns[i], 855U, 0U, 0, 101);
        assert(state->trunk_vc_freq[0] == CH_F4);
        assert(state->trunk_chan_map[0] == 0 && state->trunk_chan_map[4095] == 0);
        clock_at(0.5);
        accepted_voice(opts, state, 0);
        expect_channel(state, 0, 0U, DSD_CALL_CHANNEL_NONE);
    }
}

/* The user's entry for @p lpcn as dmr_t3_overlay_apply() makes it, beside the shared row: SET at @p hz, HIDE for 0. */
static void
user_entry(dsd_state* state, uint16_t lpcn, long hz) {
    dmr_ext_t* ext = dmr_ext_get_or_create(state);
    assert(ext != NULL && ext->overlay.op[lpcn] == 0U);
    ext->overlay.op[lpcn] = (uint8_t)(hz != 0 ? DMR_T3_OVERLAY_SET : DMR_T3_OVERLAY_HIDE);
    ext->overlay.hz[lpcn] = hz;
    ext->overlay.count++;
}

/* A logical Move TSCC prints the frequency DMR uses for its channel (the user's entry over the
   shared row): a set channel whose row another protocol emptied shows the user's frequency, a hidden one whose row
   another protocol filled shows none. */
static void
test_logical_move_tscc_uses_the_users_channels(dsd_opts* opts, dsd_state* state) {
    move_env(opts, state);
    user_entry(state, 0x0124U, 453612500L);
    user_entry(state, 0x0125U, 0L);
    dsd_state_set_trunk_chan_freq(state, 0x0124U, 0L);
    dsd_state_set_trunk_chan_freq(state, 0x0125U, 453637500L);
    const move_tscc set = {.pcn = 0x0124U, .ms = 78U};
    csbk_move_tscc_captured(opts, state, &set);
    assert(strstr(g_out, " Move TSCC: MS 78 to channel 292 (453.612500 MHz)") != NULL);
    const move_tscc hidden = {.pcn = 0x0125U, .ms = 79U};
    csbk_move_tscc_captured(opts, state, &hidden);
    assert(strstr(g_out, " Move TSCC: MS 79 to channel 293") != NULL && strstr(g_out, "MHz") == NULL);
}

static void
test_move_tscc_and_grant_channels(dsd_opts* opts, dsd_state* state) {
    dsd_decode_clock_use_test(k_t0_ns);
    test_logical_move_tscc_is_informational(opts, state);
    test_logical_move_tscc_uses_the_users_channels(opts, state);
    test_absolute_move_tscc_learns_the_tscc(opts, state);
    test_grant_layout_move_follows_nothing(opts, state);
    test_t3_grant_lpcn_kind(opts, state);
    test_t3_absolute_grant_uses_mbc_lpcn(opts, state);
    test_t3_absolute_grant_invalid_mbc_lpcn(opts, state);
    dsd_decode_clock_use_system();
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

    // Step 1: tune to initial VC via SM grant
    long f1 = 852000000;
    dmr_sm_emit_group_grant(&opts, &state, f1, /*lpcn*/ 0x0010, /*tg*/ 101, /*src*/ 1234);
    assert(opts.trunk_is_tuned == 1);
    assert(state.trunk_vc_freq[0] == f1);

    // Step 2: with no CC known yet, a CRC-valid CSBK received on a radio input anchors the CC at the tuned frequency
    init_env(&opts, &state);
    state.trunk_cc_freq = 0;
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 851012500U;
    const move_tscc mv = {.pcn = 0x0123U, .ms = 4321U};
    csbk_move_tscc(&opts, &state, &mv);
    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_cc_freq == 851012500);

    test_move_tscc_and_grant_channels(&opts, &state);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_state_ext_free_all(&state);
    printf("DMR_T3_CMOVE: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
