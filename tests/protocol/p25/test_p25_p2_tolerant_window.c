// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A P25 Phase 2 window a tolerant sync opened (dsd_p25p2_sync_anchor), through processP2(): the verdict judges the whole
 * window before anything of it takes effect. Windows are built as a site sends them (p25p2_burst_builder.h) and read
 * through a stand-in symbol reader, so the window position, the descrambling and the real burst handlers all run; the
 * vocoder and the VPDU decoder are stand-ins that count what reached them. The syncs are accepted through the frame
 * sync's own anchor entry points, as the hunt accepts them.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p2_sync_anchor.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/fec/p25p2_isch.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "p25p2_burst_builder.h"
#include "p25p2_seed_recover.h"
#include "test_support.h"

extern int p2bit[4320];

dsd_frame_verdict dsd_dispatch_handle_p25p2(dsd_opts* opts, dsd_state* state);

#define CODE_OF(w, s, n) ((((uint64_t)(w)) << 24) | (((uint64_t)(s)) << 12) | (uint64_t)(n))
#define TEST_CODE        CODE_OF(0xBEE00U, 0x164U, 0x161U)
/* Another system's code: a window scrambled with it fails under TEST_CODE, and code recovery may solve it. */
#define OTHER_CODE       CODE_OF(0xBEE00U, 0x164U, 0x2A5U)
#define TEST_T0_NS       UINT64_C(1000000000000)

enum {
    DUID_4V = 0,
    DUID_SACCH_S = 3,
    DUID_2V = 6,
    DUID_FACCH_S = 9,
    MAC_END_PTT = 2,
    MAC_IDLE = 3,
    STRONG_LLR = 200,
    SYNC_DIBITS = 20,
    WINDOW_DIBITS = 700,
    /* The symbol clock where the first window's sync starts. */
    FIRST_SYNC = 1000,
};

static dsd_opts* g_opts;
static dsd_state* g_state;

/* The window the stand-in symbol reader hands processP2(): bits in p2bit layout, and their LLRs. */
static uint8_t g_bits[1440];
static int16_t g_llr[1400];
static int g_feed_pos = -1;
/* The window dibit at which the stand-in reader adopts a new carrier (a replay read crossing a recorded retune); -1
   none. */
static int g_carrier_cut_at = -1;
/* The window dibit at which the stand-in reader meets a stream flush (an RTL read whose stream generation moved: the
   clock's epoch moves, the carrier count and the tuning generation do not); -1 none. */
static int g_flush_at = -1;
/* The window dibit at which the input ends, -1 none: the session stops, or with g_end_falls_back set the input falls
   back to Pulse. The reader goes on handing out the window's own dibits, as stand-ins a verdict can take for them. */
static int g_end_at = -1;
static int g_end_falls_back;

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    int dibit = 0;
    int16_t llr0 = 0;
    int16_t llr1 = 0;
    if (g_feed_pos >= 0 && g_feed_pos < WINDOW_DIBITS) {
        if (state && g_feed_pos == g_carrier_cut_at) {
            state->carrier_seq++;
        }
        if (state && g_feed_pos == g_flush_at) {
            state->symbol_clock_epoch++;
        }
        if (state && g_feed_pos == g_end_at) {
            if (g_end_falls_back) {
                state->input_fallback_pending = 1;
            } else {
                dsd_exitflag_store(1);
            }
        }
        const int bit = g_feed_pos * 2;
        dibit = (g_bits[bit] << 1) | g_bits[bit + 1];
        llr0 = g_llr[bit];
        llr1 = g_llr[bit + 1];
        g_feed_pos++;
    }
    if (state) {
        state->symbolcnt++;
        state->symbol_clock++;
    }
    if (out_soft) {
        out_soft->reliability = 128;
        out_soft->llr[0] = llr0;
        out_soft->llr[1] = llr1;
    }
    return dibit;
}

/* What reached the VPDU decoder, and the AMBE frames that reached the vocoder (with a running hash of their bits). */
static int g_vpdu_calls;
static int g_ambe_frames;
static uint64_t g_ambe_hash;
/* Set: the next VPDU retunes the receiver (a grant or a return to the control channel accepted mid-window). */
static int g_vpdu_retunes;

void
process_MAC_VPDU_crc(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                     unsigned long long int mac[24], int crc_ok) {
    (void)opts;
    (void)state;
    (void)type;
    (void)pdu_type;
    (void)mac;
    (void)crc_ok;
    g_vpdu_calls++;
    if (g_vpdu_retunes) {
        g_vpdu_retunes = 0;
        dsd_trunk_tuning_generation_advance();
    }
}

void
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                 unsigned long long int mac[24]) {
    process_MAC_VPDU_crc(opts, state, type, pdu_type, mac, 1);
}

void
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    (void)opts;
    (void)protocol;
    (void)imbe_fr;
    (void)imbe7100_fr;
    g_ambe_frames++;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 24; c++) {
            g_ambe_hash = (g_ambe_hash * 1099511628211ULL) ^ (uint64_t)(ambe_fr[r][c].bit & 1);
        }
    }
    g_ambe_hash ^= (uint64_t)state->currentslot;
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, char imbe_fr[8][23], char ambe_fr[4][24],
                char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)protocol;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
}

/* A muted frame is logged, not decoded: no slot's audio is muted here. */
void
dsd_mbe_log_ambe_soft_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit ambe_fr[4][24]) {
    (void)opts;
    (void)state;
    (void)ambe_fr;
}

void
l3h_embedded_alias_decode(const dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, const uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

/* Calls of the VC-sync hook: the channel activity a sync notes on a followed voice channel. */
static int g_vc_sync_hooks;

static void
count_vc_sync(dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_vc_sync_hooks++;
}

static int
expect(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

/* A fresh decoder on a site whose scrambling code is set, both slots' audio open. */
static void
fresh_decoder(void) {
    dsd_state_ext_free_all(g_state);
    free(g_state->event_history_s);
    DSD_MEMSET(g_opts, 0, sizeof(*g_opts));
    DSD_MEMSET(g_state, 0, sizeof(*g_state));
    dsd_decode_clock_use_test(TEST_T0_NS);
    p25_p2_frame_reset();
    Event_History_I* history = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    if (history != NULL) {
        init_event_history(&history[0], 0, 255);
        init_event_history(&history[1], 0, 255);
    }
    g_state->event_history_s = history;
    g_state->p2_wacn = TEST_CODE >> 24;
    g_state->p2_sysid = (TEST_CODE >> 12) & 0xFFFU;
    g_state->p2_cc = TEST_CODE & 0xFFFU;
    for (int slot = 0; slot < 2; slot++) {
        const dsd_call_observation observation = {
            .protocol = DSD_SYNC_P25P2_POS,
            .slot = (uint8_t)slot,
            .kind = DSD_CALL_KIND_GROUP_VOICE,
            .ota_target_id = 100U + (uint64_t)slot,
            .policy_target_id = 100U + (uint64_t)slot,
            .ota_source_id = 2000U + (uint64_t)slot,
        };
        (void)dsd_call_state_observe(g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
        g_state->p25_crypto_state[slot] = DSD_P25_CRYPTO_CLEAR;
        g_state->p25_p2_audio_allowed[slot] = 1;
    }
    g_state->payload_algid = 0x80;
    g_state->payload_algidR = 0x80;
    g_state->rf_mod = 1;
    g_state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    g_state->symbol_clock = FIRST_SYNC;
    g_state->symbolcnt = FIRST_SYNC;
    g_vpdu_calls = 0;
    g_ambe_frames = 0;
    g_ambe_hash = 0;
    g_vpdu_retunes = 0;
    g_vc_sync_hooks = 0;
}

/* The opcode of the MAC PDU a built window's signalling carries. */
static int g_mac_opcode = MAC_IDLE;
/* Cleared: the built MAC PDUs carry no CRC, so each fails its checksum behind a clean R-S word. */
static int g_mac_fill_crc = 1;

/* A MAC PDU (g_mac_opcode) whose payload varies by @p salt, CRC filled (unless g_mac_fill_crc is cleared). */
static void
idle_slot(p25p2_bb_slot* slot, int duid, int salt) {
    DSD_MEMSET(slot, 0, sizeof(*slot));
    slot->duid = duid;
    slot->fill_crc = g_mac_fill_crc;
    slot->mac[0] = (uint8_t)(g_mac_opcode << 5);
    for (int i = 1; i < 18; i++) {
        slot->mac[i] = (uint8_t)((salt * 37) + (i * 11));
    }
}

/* The scrambling code built windows are sent with. */
static uint64_t g_build_code = TEST_CODE;

/* A voice window: 4V, FACCH, 4V, SACCH (or four 4V when @p all_voice). */
static void
build(int t0, int all_voice) {
    p25p2_bb_slot slots[4];
    DSD_MEMSET(slots, 0, sizeof(slots));
    slots[0].duid = DUID_4V;
    slots[2].duid = DUID_4V;
    if (all_voice) {
        slots[1].duid = DUID_4V;
        slots[3].duid = DUID_4V;
    } else {
        idle_slot(&slots[1], DUID_FACCH_S, t0);
        idle_slot(&slots[3], DUID_SACCH_S, t0 + 1);
    }
    p25p2_bb_window(g_build_code, t0, slots, g_bits);
    for (int i = 0; i < 1400; i++) {
        g_llr[i] = (int16_t)(g_bits[i] ? STRONG_LLR : -STRONG_LLR);
    }
}

static void
set_isch(int fc, uint64_t word) {
    for (int i = 0; i < 40; i++) {
        const int bit = 320 + (360 * fc) + i;
        g_bits[bit] = (uint8_t)((word >> (39 - i)) & 1U);
        g_llr[bit] = (int16_t)(g_bits[bit] ? STRONG_LLR : -STRONG_LLR);
    }
}

/* The DUID of window timeslot @p ts received as @p byte. */
static void
set_duid(int ts, uint8_t byte) {
    static const int offsets[8] = {0, 1, 74, 75, 244, 245, 318, 319};
    for (int i = 0; i < 8; i++) {
        g_bits[(ts * 360) + offsets[i]] = (uint8_t)((byte >> (7 - i)) & 1U);
    }
}

/* The I-ISCHs of a window starting at @p t0 erased (the all-zero word decodes to nothing): only the S-ISCH and
   continuity place it. */
static void
erase_i_ischs(int t0) {
    set_isch((t0 & 1) ? 0 : 1, 0U);
    set_isch((t0 & 1) ? 1 : 2, 0U);
}

/* The 20 sync dibits read, and the sync accepted exactly, as frame_sync_accept_p25p2() takes it. */
static void
sync_exact(void) {
    g_state->symbol_clock += SYNC_DIBITS;
    g_state->symbolcnt += SYNC_DIBITS;
    frame_sync_p25p2_anchor_note_accept(g_state, DSD_SYNC_P25P2_POS);
    frame_sync_p25p2_anchor_note_sync(g_state, DSD_SYNC_P25P2_POS, -1, 0, 0);
    dsd_frame_sync_p25p2_note_sync_activity(g_opts, g_state);
}

/* The 20 sync dibits read at the anchor's expected position, and the sync accepted within the tolerance. Returns the
   start the anchor predicts, or -1 when no anchor stood there. */
static int
sync_tolerant(int bit_errors) {
    g_state->symbol_clock += SYNC_DIBITS;
    g_state->symbolcnt += SYNC_DIBITS;
    uint8_t map_idx = 0;
    int inverted = 0;
    int start = -1;
    if (!frame_sync_p25p2_anchor_take(g_state, &map_idx, &inverted, &start)) {
        return -1;
    }
    frame_sync_p25p2_anchor_note_accept(g_state, DSD_SYNC_P25P2_POS);
    frame_sync_p25p2_anchor_note_sync(g_state, DSD_SYNC_P25P2_POS, start, bit_errors, 0);
    return start;
}

/* A sync the hunt missed: its dibits and the window behind it go by unread, as a hunt through them reads them. */
static void
sync_missed(void) {
    g_state->symbol_clock += SYNC_DIBITS + WINDOW_DIBITS;
    g_state->symbolcnt += SYNC_DIBITS + WINDOW_DIBITS;
}

static p25p2_window_outcome
feed(void) {
    g_feed_pos = 0;
    const p25p2_window_outcome outcome = processP2(g_opts, g_state);
    g_feed_pos = -1;
    return outcome;
}

/* What a window may change of the decoder's state, besides the symbol counts and the frame sync's own record. The MAC
   fragments, ESS_B fragments and playout are kept as their bytes. */
typedef struct {
    int p2_scramble_offset;
    int p2_vch_chan_num;
    int currentslot;
    int fourv_counter[2];
    int dmr_stereo;
    int p2_is_lcch;
    time_t last_vc_sync_time;
    time_t last_cc_sync_time;
    double last_cc_sync_time_m;
    unsigned int p2_cc_verified;
    unsigned int tdma_carrier;
    int p25_p2_audio_allowed[2];
    unsigned char p25_mac_frag[sizeof(((dsd_state*)0)->p25_mac_frag)];
    unsigned char ess_b[sizeof(((dsd_state*)0)->ess_b)];
    unsigned char playout[sizeof(dsd_p25p2_playout)];
    int vpdu_calls;
    int ambe_frames;
    uint64_t ambe_hash;
    /* Calls, recent activity and event history: their revisions move with any change. */
    uint64_t call_revision;
    uint64_t recent_revision;
    int call_phase[2];
    uint64_t call_epoch[2];
    uint64_t history_revision[2];
    uint64_t history_push_seq[2];
    /* Scrambling code recovery: its state extension, and the candidate it holds. */
    int seed_recover;
    int seed_pending;
    uint64_t seed_code;
    /* MBE capture: the open handles and their flags. */
    const FILE* mbe_out_f;
    const FILE* mbe_out_fR;
    int mbe_out;
    int mbe_outR;
    int vc_sync_hooks;
} window_effects;

static void
snapshot_calls_and_capture(window_effects* out) {
    static dsd_call_state_snapshot calls;
    static dsd_recent_activity_snapshot recent;
    DSD_MEMSET(&calls, 0, sizeof(calls));
    DSD_MEMSET(&recent, 0, sizeof(recent));
    (void)dsd_call_state_copy_snapshot(g_state, &calls);
    (void)dsd_recent_activity_copy_snapshot(g_state, &recent);
    out->call_revision = calls.revision;
    out->recent_revision = recent.revision;
    for (int slot = 0; slot < 2; slot++) {
        out->call_phase[slot] = (int)calls.slots[slot].phase;
        out->call_epoch[slot] = calls.slots[slot].epoch;
        out->history_revision[slot] = g_state->event_history_s[slot].revision;
        out->history_push_seq[slot] = g_state->event_history_s[slot].push_seq;
    }
    out->seed_recover = dsd_state_ext_get_const(g_state, DSD_STATE_EXT_PROTO_P25P2_SEED_RECOVER) != NULL;
    out->seed_pending = p25p2_seed_recover_pending(g_state, &out->seed_code);
    out->mbe_out_f = g_opts->mbe_out_f;
    out->mbe_out_fR = g_opts->mbe_out_fR;
    out->mbe_out = g_opts->mbe_out;
    out->mbe_outR = g_opts->mbe_outR;
    out->vc_sync_hooks = g_vc_sync_hooks;
}

static void
snapshot(window_effects* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    out->p2_scramble_offset = g_state->p2_scramble_offset;
    out->p2_vch_chan_num = g_state->p2_vch_chan_num;
    out->currentslot = g_state->currentslot;
    out->fourv_counter[0] = g_state->fourv_counter[0];
    out->fourv_counter[1] = g_state->fourv_counter[1];
    out->dmr_stereo = g_state->dmr_stereo;
    out->p2_is_lcch = g_state->p2_is_lcch;
    out->last_vc_sync_time = g_state->last_vc_sync_time;
    out->last_cc_sync_time = g_state->last_cc_sync_time;
    out->last_cc_sync_time_m = g_state->last_cc_sync_time_m;
    out->p2_cc_verified = g_state->p2_cc_verified;
    out->tdma_carrier = g_state->tdma_carrier;
    out->p25_p2_audio_allowed[0] = g_state->p25_p2_audio_allowed[0];
    out->p25_p2_audio_allowed[1] = g_state->p25_p2_audio_allowed[1];
    DSD_MEMCPY(out->p25_mac_frag, g_state->p25_mac_frag, sizeof(out->p25_mac_frag));
    DSD_MEMCPY(out->ess_b, g_state->ess_b, sizeof(out->ess_b));
    dsd_p25p2_playout playout = g_state->p25p2_playout;
    /* The voice burst serial runs on across cases (a module counter): only its use within a case is compared. */
    playout.slot[0].burst_serial = 0U;
    playout.slot[1].burst_serial = 0U;
    DSD_MEMCPY(out->playout, &playout, sizeof(out->playout));
    out->vpdu_calls = g_vpdu_calls;
    out->ambe_frames = g_ambe_frames;
    out->ambe_hash = g_ambe_hash;
    snapshot_calls_and_capture(out);
}

static int
same_state_fields(const window_effects* a, const window_effects* b) {
    return a->p2_scramble_offset == b->p2_scramble_offset && a->p2_vch_chan_num == b->p2_vch_chan_num
           && a->currentslot == b->currentslot && a->fourv_counter[0] == b->fourv_counter[0]
           && a->fourv_counter[1] == b->fourv_counter[1] && a->dmr_stereo == b->dmr_stereo
           && a->p2_is_lcch == b->p2_is_lcch && a->last_vc_sync_time == b->last_vc_sync_time
           && a->last_cc_sync_time == b->last_cc_sync_time
           && fabs(a->last_cc_sync_time_m - b->last_cc_sync_time_m) < 1e-9;
}

static int
same_calls(const window_effects* a, const window_effects* b) {
    return a->call_revision == b->call_revision && a->recent_revision == b->recent_revision
           && a->call_phase[0] == b->call_phase[0] && a->call_phase[1] == b->call_phase[1]
           && a->call_epoch[0] == b->call_epoch[0] && a->call_epoch[1] == b->call_epoch[1]
           && a->history_revision[0] == b->history_revision[0] && a->history_revision[1] == b->history_revision[1]
           && a->history_push_seq[0] == b->history_push_seq[0] && a->history_push_seq[1] == b->history_push_seq[1];
}

static int
same_calls_and_capture(const window_effects* a, const window_effects* b) {
    return same_calls(a, b) && a->seed_recover == b->seed_recover && a->seed_pending == b->seed_pending
           && a->seed_code == b->seed_code && a->mbe_out_f == b->mbe_out_f && a->mbe_out_fR == b->mbe_out_fR
           && a->mbe_out == b->mbe_out && a->mbe_outR == b->mbe_outR && a->vc_sync_hooks == b->vc_sync_hooks;
}

static int
same_effects(const window_effects* a, const window_effects* b) {
    return same_state_fields(a, b) && same_calls_and_capture(a, b) && a->p2_cc_verified == b->p2_cc_verified
           && a->tdma_carrier == b->tdma_carrier && a->p25_p2_audio_allowed[0] == b->p25_p2_audio_allowed[0]
           && a->p25_p2_audio_allowed[1] == b->p25_p2_audio_allowed[1]
           && memcmp(a->p25_mac_frag, b->p25_mac_frag, sizeof(a->p25_mac_frag)) == 0
           && memcmp(a->ess_b, b->ess_b, sizeof(a->ess_b)) == 0
           && memcmp(a->playout, b->playout, sizeof(a->playout)) == 0 && a->vpdu_calls == b->vpdu_calls
           && a->ambe_frames == b->ambe_frames && a->ambe_hash == b->ambe_hash;
}

/* An exact window at superframe timeslot 2 with its I-ISCHs: it anchors the next window at 6. Its signalling is
   MAC_IDLE, which ends the calls: a case that follows voice past it builds it all voice (@p all_voice). */
static int
anchored_by_exact_window_of(int all_voice) {
    build(2, all_voice);
    sync_exact();
    int rc = expect("exact window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("exact window anchors", g_state->p25_p2_sync_anchor.valid == 1U);
    rc |= expect("anchor start", g_state->p25_p2_sync_anchor.start == 2U);
    return rc;
}

static int
anchored_by_exact_window(void) {
    return anchored_by_exact_window_of(0);
}

/* An MBE capture open on both slots, as -d keeps one through a call. */
static FILE*
open_capture(char* path, size_t path_size) {
    const int fd = dsd_test_mkstemp(path, path_size, "p25p2_tolerant_mbe");
    if (fd < 0) {
        return NULL;
    }
    (void)dsd_close(fd);
    return dsd_fopen_private(path, "wb");
}

static void
close_capture(FILE** handle, short int* flag, const char* path) {
    if (*handle != NULL) {
        (void)fclose(*handle);
        *handle = NULL;
    }
    *flag = 0;
    (void)remove(path);
}

/* The windows a tolerant sync's verdict fails. */
typedef enum {
    /* MAC_IDLE signalling, every ISCH erased: nothing corroborates the start. */
    UNCONFIRMED_IDLE = 0,
    /* As IDLE but MAC_END_PTT, on a followed voice channel (the VC-sync hook notes activity) with an MBE capture open
       on both slots: dispatched, it ends the calls and closes the capture. */
    UNCONFIRMED_FOLLOWED = 1,
    /* The ISCHs intact but timeslots 2 and 3 carry DUIDs nothing decodes, sent with another code on a carrier whose
       code is unproven: dispatched, its own I-ISCH locates it and code recovery takes up the signalling that fails
       under the code in force. */
    UNCONFIRMED_DUIDS = 2,
} unconfirmed_kind;

/* What a window of @p kind does, opened by a tolerant sync whose verdict fails (@p tolerant) or by an exact sync, which
   continuity places when its ISCHs are erased. */
static int
run_unconfirmed_window(unconfirmed_kind kind, int tolerant, window_effects* before, window_effects* after,
                       p25p2_window_outcome* outcome) {
    char path[2][512];
    const int followed = kind == UNCONFIRMED_FOLLOWED;
    fresh_decoder();
    int rc = anchored_by_exact_window_of(1);
    if (kind == UNCONFIRMED_DUIDS) {
        rc |= expect("code unproven", g_state->p2_cc_verified == 0U);
    }
    if (followed) {
        g_opts->trunk_enable = 1;
        g_opts->trunk_is_tuned = 1;
        g_opts->mbe_out_f = open_capture(path[0], sizeof(path[0]));
        g_opts->mbe_out_fR = open_capture(path[1], sizeof(path[1]));
        g_opts->mbe_out = 1;
        g_opts->mbe_outR = 1;
        rc |= expect("capture open", g_opts->mbe_out_f != NULL && g_opts->mbe_out_fR != NULL);
    }
    if (tolerant) {
        rc |= expect("tolerant sync at the expected position", sync_tolerant(3) == 6);
    } else {
        sync_exact();
    }
    g_mac_opcode = followed ? MAC_END_PTT : MAC_IDLE;
    g_build_code = kind == UNCONFIRMED_DUIDS ? OTHER_CODE : TEST_CODE;
    build(6, 0);
    g_mac_opcode = MAC_IDLE;
    g_build_code = TEST_CODE;
    if (kind == UNCONFIRMED_DUIDS) {
        set_duid(2, 0x03U);
        set_duid(3, 0x03U);
    } else {
        set_isch(0, 0U);
        set_isch(1, 0U);
        set_isch(2, 0U);
    }
    /* Stale values a dispatched window would overwrite. */
    g_state->p2_vch_chan_num = 3;
    g_state->dmr_stereo = 0;
    g_state->p2_is_lcch = 0;
    g_state->p25_mac_frag[0].active = 1U;
    g_state->p25_mac_frag[0].collected = 7U;
    snapshot(before);
    *outcome = feed();
    snapshot(after);
    if (followed) {
        close_capture(&g_opts->mbe_out_f, &g_opts->mbe_out, path[0]);
        close_capture(&g_opts->mbe_out_fR, &g_opts->mbe_outR, path[1]);
    }
    return rc;
}

/* A window a tolerant sync opened whose verdict fails changes nothing: no ISCH, position, slot, playout, burst, MAC
   fragment, call, event, code recovery, MBE capture or channel activity, and the dispatcher's frame verdict is
   UNPRODUCTIVE (below). The anchor is gone. Its bursts are real voice and signalling. The same window behind an exact
   sync changes the calls, the capture, the activity and the code recovery, so the comparison can see them. */
static int
test_failed_window_has_no_effects(void) {
    int rc = 0;
    for (int kind = UNCONFIRMED_IDLE; kind <= UNCONFIRMED_DUIDS; kind++) {
        window_effects before;
        window_effects after;
        p25p2_window_outcome outcome = P25P2_WINDOW_DISPATCHED;
        rc |= run_unconfirmed_window((unconfirmed_kind)kind, 1, &before, &after, &outcome);
        rc |= expect("failed window is rejected", outcome == P25P2_WINDOW_REJECTED);
        rc |= expect("failed window has no effects", same_effects(&before, &after));
        rc |= expect("no burst reached a decoder",
                     g_vpdu_calls == before.vpdu_calls && g_ambe_frames == before.ambe_frames);
        rc |= expect("anchor cleared", g_state->p25_p2_sync_anchor.valid == 0U);
        rc |= expect("window context spent", g_state->p25_p2_sync_anchor.frame_valid == 0U);
        rc |= expect("tolerant window not counted", g_state->p25_p2_sync_anchor.tolerant_windows == 0U);
        rc |= expect("read whole",
                     g_state->symbol_clock == (uint64_t)FIRST_SYNC + ((uint64_t)2U * (SYNC_DIBITS + WINDOW_DIBITS)));

        rc |= run_unconfirmed_window((unconfirmed_kind)kind, 0, &before, &after, &outcome);
        rc |= expect("exact window dispatches", outcome == P25P2_WINDOW_DISPATCHED);
        if (kind == UNCONFIRMED_DUIDS) {
            rc |= expect("exact window: code recovery", !before.seed_pending && after.seed_pending);
        } else {
            rc |= expect("exact window: calls change", after.call_revision != before.call_revision);
        }
        if (kind == UNCONFIRMED_FOLLOWED) {
            rc |= expect("exact window: capture closed", after.mbe_out_f == NULL && after.mbe_out_fR == NULL);
            rc |= expect("exact sync: VC-sync hook", before.vc_sync_hooks == 1);
        }
    }
    return rc;
}

/* The dispatcher's frame verdict for each outcome: a failed window validated nothing, which the SPS hunt pays for as
   for a missed sync (UNPRODUCTIVE, not WITHHELD: the handler ran and found nothing). */
static int
test_dispatcher_verdict(void) {
    fresh_decoder();
    int rc = anchored_by_exact_window();
    (void)sync_tolerant(2);
    build(6, 0);
    set_isch(0, 0U);
    set_isch(1, 0U);
    set_isch(2, 0U);
    g_feed_pos = 0;
    rc |= expect("failed window: unproductive",
                 dsd_dispatch_handle_p25p2(g_opts, g_state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    g_feed_pos = -1;

    fresh_decoder();
    rc |= anchored_by_exact_window();
    (void)sync_tolerant(2);
    build(6, 0);
    g_feed_pos = 0;
    rc |=
        expect("passed window: productive", dsd_dispatch_handle_p25p2(g_opts, g_state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    g_feed_pos = -1;
    return rc;
}

/* A later-slot failure: timeslot 0 is a good 4V burst, timeslots 2 and 3 carry DUIDs nothing decodes. Opened exactly,
   the window dispatches timeslot 0's voice before the DUID errors abort it; opened by a tolerant sync, the verdict
   judges all four first and no AMBE frame reaches the vocoder. */
static int
test_later_slot_failure(void) {
    int rc = 0;
    for (int tolerant = 0; tolerant <= 1; tolerant++) {
        fresh_decoder();
        rc |= anchored_by_exact_window_of(1);
        const int frames_before = g_ambe_frames;
        if (tolerant) {
            rc |= expect("tolerant sync", sync_tolerant(1) == 6);
        } else {
            sync_exact();
        }
        build(6, 1);
        set_duid(2, 0x03U);
        set_duid(3, 0x03U);
        const p25p2_window_outcome outcome = feed();
        if (tolerant) {
            rc |= expect("tolerant later-slot failure rejected", outcome == P25P2_WINDOW_REJECTED);
            rc |= expect("tolerant later-slot failure: no voice", g_ambe_frames == frames_before);
        } else {
            rc |= expect("exact window dispatches", outcome == P25P2_WINDOW_DISPATCHED);
            rc |= expect("exact window: timeslot 0's voice", g_ambe_frames > frames_before);
        }
    }
    return rc;
}

/* A voice window that passes its verdict decodes exactly as the same window opened by an exact sync: the same frames to
   the vocoder, the same playout, the same state; and its channel activity is noted (once the verdict passed). */
static int
test_passed_window_matches_exact(void) {
    window_effects result[2];
    int rc = 0;
    for (int tolerant = 0; tolerant <= 1; tolerant++) {
        fresh_decoder();
        rc |= anchored_by_exact_window_of(1);
        const int anchor_frames = g_ambe_frames;
        g_state->last_cc_sync_time = 0;
        g_state->last_cc_sync_time_m = 0.0;
        if (tolerant) {
            rc |= expect("tolerant sync", sync_tolerant(4) == 6);
            rc |= expect("no activity before the verdict", g_state->last_cc_sync_time == 0);
        } else {
            sync_exact();
        }
        build(6, 1);
        rc |= expect("window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
        rc |= expect("its voice reached the vocoder", g_ambe_frames == anchor_frames + 16);
        rc |= expect("its channel activity noted", g_state->last_cc_sync_time != 0);
        build(10, 0);
        sync_exact();
        rc |= expect("next window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
        snapshot(&result[tolerant]);
        if (tolerant) {
            rc |= expect("tolerant window counted", g_state->p25_p2_sync_anchor.tolerant_windows == 1U);
        }
    }
    rc |= expect("tolerant window decodes as the exact one", same_effects(&result[0], &result[1]));
    return rc;
}

/* After a failed window the next exact window finds four timeslots missing from its continuity and retires them as
   lost, exactly as when the hunt missed that sync. */
static int
test_after_failure_as_after_missed_sync(void) {
    window_effects result[2];
    int rc = 0;
    for (int missed = 0; missed <= 1; missed++) {
        fresh_decoder();
        rc |= anchored_by_exact_window();
        if (missed) {
            sync_missed();
        } else {
            rc |= expect("tolerant sync", sync_tolerant(2) == 6);
            build(6, 0);
            set_isch(0, 0U);
            set_isch(1, 0U);
            set_isch(2, 0U);
            rc |= expect("rejected", feed() == P25P2_WINDOW_REJECTED);
        }
        build(10, 0);
        sync_exact();
        rc |= expect("next window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
        snapshot(&result[missed]);
    }
    rc |= expect("failed window leaves what a missed sync leaves", same_effects(&result[0], &result[1]));
    return rc;
}

/* What installs an anchor: an exact window with an I-ISCH does; one placed only by continuity, or by its S-ISCH, does
   not; a tolerant window renews it only with an I-ISCH; a retune while the window was dispatched, or a change of the
   stream after its sync, installs nothing. */
static int
test_installation(void) {
    int rc = 0;

    /* Continuity alone (the I-ISCHs erased, the S-ISCH intact) places the second window: no anchor. */
    fresh_decoder();
    rc |= anchored_by_exact_window();
    build(6, 0);
    erase_i_ischs(6);
    sync_exact();
    rc |= expect("continuity window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("continuity window placed", g_state->p2_scramble_offset % 12 == 6);
    rc |= expect("continuity window anchors nothing", g_state->p25_p2_sync_anchor.valid == 0U);

    /* A tolerant window that passes on its S-ISCH alone dispatches and anchors nothing; with an I-ISCH it renews. */
    for (int with_i_isch = 0; with_i_isch <= 1; with_i_isch++) {
        fresh_decoder();
        rc |= anchored_by_exact_window();
        rc |= expect("tolerant sync", sync_tolerant(1) == 6);
        build(6, 0);
        if (!with_i_isch) {
            erase_i_ischs(6);
        }
        rc |= expect("tolerant window passes", feed() == P25P2_WINDOW_DISPATCHED);
        rc |= expect("renewed only with an I-ISCH", g_state->p25_p2_sync_anchor.valid == (unsigned)with_i_isch);
        if (with_i_isch) {
            rc |= expect("renewed start", g_state->p25_p2_sync_anchor.start == 6U);
            rc |= expect("tolerant sync at the renewed position", sync_tolerant(2) == 10);
        }
    }

    /* A retune accepted by a burst of the window: nothing installs. */
    fresh_decoder();
    build(2, 0);
    sync_exact();
    g_vpdu_retunes = 1;
    rc |= expect("retuning window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("retune installs nothing", g_state->p25_p2_sync_anchor.valid == 0U);

    /* The stream moved between the sync and the window (a flush): nothing installs. */
    fresh_decoder();
    build(2, 0);
    sync_exact();
    g_state->symbol_clock_epoch++;
    rc |= expect("flushed window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("flush installs nothing", g_state->p25_p2_sync_anchor.valid == 0U);

    /* A window starting where no S-ISCH precedes it (no hunt opens one there): nothing installs. */
    fresh_decoder();
    build(0, 0);
    sync_exact();
    rc |= expect("slot-0 window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("slot-0 window anchors nothing", g_state->p25_p2_sync_anchor.valid == 0U);
    return rc;
}

/* The stream moving after a tolerant sync fails the verdict: the window's context no longer holds, or continuity no
   longer predicts the anchor's start. */
static int
test_context_fails_verdict(void) {
    int rc = 0;
    for (int which = 0; which < 3; which++) {
        fresh_decoder();
        rc |= anchored_by_exact_window();
        rc |= expect("tolerant sync", sync_tolerant(1) == 6);
        if (which == 0) {
            g_state->symbol_clock_epoch++;
        } else if (which == 1) {
            dsd_trunk_tuning_generation_advance();
        } else {
            p25_p2_frame_reset();
        }
        build(6, 0);
        rc |= expect("moved stream rejects", feed() == P25P2_WINDOW_REJECTED);
        rc |= expect("nothing decoded", g_vpdu_calls == 2 && g_state->p25_p2_sync_anchor.valid == 0U);
    }
    return rc;
}

/* A window an input interruption or a carrier boundary cut while it was collected never reaches the verdict: behind a
   tolerant sync it is charged as the same cut behind an exact sync (PRODUCTIVE, as before) and leaves the same state.
   None of its bursts dispatches, the anchor and the window context are spent, and no tolerant window is counted. The
   exact sync noted its channel activity when it was accepted, and the tolerant one never does: both stamps are cleared
   after the sync, and the tolerant window must leave them clear. */
static int
test_cut_window_charged_as_exact(void) {
    int rc = 0;
    for (int carrier_cut = 0; carrier_cut <= 1; carrier_cut++) {
        window_effects result[2];
        for (int tolerant = 0; tolerant <= 1; tolerant++) {
            fresh_decoder();
            rc |= anchored_by_exact_window();
            const int vpdu_before = g_vpdu_calls;
            if (tolerant) {
                rc |= expect("tolerant sync", sync_tolerant(1) == 6);
            } else {
                sync_exact();
            }
            g_state->last_cc_sync_time = 0;
            g_state->last_cc_sync_time_m = 0.0;
            build(6, 0);
            if (carrier_cut) {
                g_carrier_cut_at = 350;
            } else {
                g_state->input_interrupted = 1;
            }
            g_feed_pos = 0;
            const dsd_frame_verdict verdict = dsd_dispatch_handle_p25p2(g_opts, g_state);
            g_feed_pos = -1;
            g_carrier_cut_at = -1;
            g_state->input_interrupted = 0;
            snapshot(&result[tolerant]);
            rc |= expect("cut window: productive", verdict == DSD_FRAME_VERDICT_PRODUCTIVE);
            rc |= expect("cut window dispatches no burst", g_vpdu_calls == vpdu_before);
            rc |= expect("cut window anchors nothing", g_state->p25_p2_sync_anchor.valid == 0U);
            rc |= expect("cut window context spent", g_state->p25_p2_sync_anchor.frame_valid == 0U
                                                         && g_state->p25_p2_sync_anchor.frame_tolerant == 0U);
            rc |= expect("cut window not counted", g_state->p25_p2_sync_anchor.tolerant_windows == 0U);
            rc |= expect("cut window notes no activity", g_state->last_cc_sync_time == 0);
        }
        rc |= expect("cut tolerant window leaves what the cut exact one leaves", same_effects(&result[0], &result[1]));
    }
    return rc;
}

/* The input ends two dibits before a window's end (a capture that runs out, a file that falls back to Pulse), and what
   the reader hands out then completes a window whose verdict passes: behind a tolerant sync it is charged as a cut
   window (test_cut_window_charged_as_exact()), none of its bursts dispatches, it anchors nothing and no tolerant window
   is counted. Behind an exact sync the bursts read before the end decode, as before. */
static int
test_input_end_cuts_the_window(void) {
    int rc = 0;
    for (int falls_back = 0; falls_back <= 1; falls_back++) {
        for (int tolerant = 0; tolerant <= 1; tolerant++) {
            fresh_decoder();
            rc |= anchored_by_exact_window();
            const int vpdu_before = g_vpdu_calls;
            if (tolerant) {
                rc |= expect("tolerant sync", sync_tolerant(1) == 6);
            } else {
                sync_exact();
            }
            build(6, 0);
            g_end_falls_back = falls_back;
            g_end_at = WINDOW_DIBITS - 2;
            (void)feed();
            g_end_at = -1;
            dsd_exitflag_store(0);
            g_state->input_fallback_pending = 0;
            g_state->input_interrupted = 0;
            g_state->input_interrupt_reason = DSD_INTERRUPT_NONE;
            if (!tolerant) {
                rc |= expect("ended exact window decodes", g_vpdu_calls > vpdu_before);
                continue;
            }
            rc |= expect("ended window dispatches no burst", g_vpdu_calls == vpdu_before);
            rc |= expect("ended window anchors nothing", g_state->p25_p2_sync_anchor.valid == 0U);
            rc |= expect("ended window not counted", g_state->p25_p2_sync_anchor.tolerant_windows == 0U);
        }
    }
    return rc;
}

/* What the windows handed the CQPSK timing loop as their P25 proof: the calls, and the generation of the last. */
static int g_p25_proofs;
static uint64_t g_p25_proof;

static void
capture_p25_proof(uint64_t proof) {
    g_p25_proofs++;
    g_p25_proof = proof;
}

/* A window proves the carrier P25 to the CQPSK timing loop, once, when it is done: one with a MAC PDU that passed its
   CRC, or one a tolerant sync opened whose verdict passed, on the tuning generation it was dispatched on. A window of
   voice alone behind an exact sync proves nothing, nor does one whose MAC PDUs failed their CRC, a window whose verdict
   failed, one a burst of which tuned away, or one read on a stream that is no longer the one its sync was accepted on
   (a flush between the sync and the window or inside it, which moves no tuning generation). */
static int
test_window_proves_p25(void) {
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.p25_timing_proof = capture_p25_proof;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    int rc = 0;

    fresh_decoder();
    g_p25_proofs = 0;
    build(2, 0);
    sync_exact();
    rc |= expect("MAC window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("a MAC PDU that passed its CRC proves P25 once",
                 g_p25_proofs == 1 && g_p25_proof == dsd_trunk_tuning_generation() + 1U);

    fresh_decoder();
    g_p25_proofs = 0;
    build(2, 1);
    sync_exact();
    rc |= expect("voice window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("voice alone behind an exact sync proves nothing", g_p25_proofs == 0);

    fresh_decoder();
    g_p25_proofs = 0;
    g_mac_fill_crc = 0;
    build(2, 0);
    g_mac_fill_crc = 1;
    sync_exact();
    rc |= expect("CRC-failed window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("MAC PDUs that failed their CRC prove nothing", g_p25_proofs == 0);

    fresh_decoder();
    rc |= anchored_by_exact_window_of(1);
    g_p25_proofs = 0;
    rc |= expect("tolerant sync", sync_tolerant(2) == 6);
    build(6, 1);
    rc |= expect("tolerant voice window passes", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("a passed verdict proves P25", g_p25_proofs == 1 && g_p25_proof == dsd_trunk_tuning_generation() + 1U);

    fresh_decoder();
    rc |= anchored_by_exact_window_of(1);
    g_p25_proofs = 0;
    rc |= expect("tolerant sync", sync_tolerant(2) == 6);
    build(6, 0);
    set_isch(0, 0U);
    set_isch(1, 0U);
    set_isch(2, 0U);
    rc |= expect("failed window rejected", feed() == P25P2_WINDOW_REJECTED);
    rc |= expect("a failed verdict proves nothing", g_p25_proofs == 0);

    fresh_decoder();
    g_p25_proofs = 0;
    build(2, 0);
    sync_exact();
    g_vpdu_retunes = 1;
    rc |= expect("retuning window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("a window that tuned away proves nothing", g_p25_proofs == 0);

    for (int inside = 0; inside <= 1; inside++) {
        fresh_decoder();
        g_p25_proofs = 0;
        build(2, 0);
        sync_exact();
        const uint64_t generation = dsd_trunk_tuning_generation();
        if (inside) {
            g_flush_at = 350;
        } else {
            g_state->symbol_clock_epoch++;
        }
        rc |= expect("flushed MAC window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
        g_flush_at = -1;
        rc |= expect("the flush moved no tuning generation", dsd_trunk_tuning_generation() == generation);
        rc |= expect("a window read across a flush proves nothing", g_p25_proofs == 0);
        /* The next window, its sync accepted after the flush, proves again. */
        build(2, 0);
        sync_exact();
        rc |= expect("next MAC window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
        rc |= expect("the next window proves P25", g_p25_proofs == 1 && g_p25_proof == generation + 1U);
    }

    dsd_rtl_stream_metrics_hooks_set(NULL);
    return rc;
}

/* The fourth ISCH a window would hold (bits 1400-1439, the next sync) is never read, whatever p2bit holds there: a
   channel-1 I-ISCH placed there does not position a window its own ISCHs leave to continuity. */
static int
test_fourth_isch_ignored(void) {
    fresh_decoder();
    int rc = anchored_by_exact_window();
    build(6, 0);
    erase_i_ischs(6);
    const uint64_t poison = dsd_p25p2_i_isch_codewords[(1 << 5) | (2 << 3)];
    for (int i = 0; i < 40; i++) {
        p2bit[1400 + i] = (int)((poison >> (39 - i)) & 1U);
    }
    sync_exact();
    rc |= expect("window dispatches", feed() == P25P2_WINDOW_DISPATCHED);
    rc |= expect("continuity places it, not the fourth ISCH", g_state->p2_scramble_offset % 12 == 6);
    for (int i = 0; i < 40; i++) {
        p2bit[1400 + i] = 0;
    }
    return rc;
}

int
main(void) {
    g_opts = (dsd_opts*)calloc(1U, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1U, sizeof(*g_state));
    if (g_opts == NULL || g_state == NULL) {
        return 1;
    }
    dsd_frame_sync_hooks saved_hooks;
    dsd_frame_sync_hooks_get(&saved_hooks);
    dsd_frame_sync_hooks hooks = saved_hooks;
    hooks.p25_sm_vc_sync = count_vc_sync;
    dsd_frame_sync_hooks_set(&hooks);
    int rc = 0;
    rc |= test_failed_window_has_no_effects();
    rc |= test_dispatcher_verdict();
    rc |= test_later_slot_failure();
    rc |= test_passed_window_matches_exact();
    rc |= test_after_failure_as_after_missed_sync();
    rc |= test_installation();
    rc |= test_context_fails_verdict();
    rc |= test_cut_window_charged_as_exact();
    rc |= test_input_end_cuts_the_window();
    rc |= test_fourth_isch_ignored();
    rc |= test_window_proves_p25();
    dsd_frame_sync_hooks_set(&saved_hooks);
    dsd_state_ext_free_all(g_state);
    free(g_state->event_history_s);
    free(g_state);
    free(g_opts);
    if (rc == 0) {
        printf("P25_P2_TOLERANT_WINDOW: OK\n");
    }
    return rc;
}
