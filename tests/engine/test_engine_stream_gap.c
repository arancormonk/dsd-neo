// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A gap in the radio stream on the same carrier (issue #518 D5: a USB recovery, an rtl_tcp reconnect, or one a capture
 * recorded) resets the decoding it cut, run for real around a scripted getFrameSync() and a fake live RTL stream that
 * the real getSymbol() reads:
 *  - a frame being decoded when the gap comes unwinds: every symbol it reads past the gap is an unusable 0, it reads as
 *    lost (dsd_state_frame_read_lost()), and the interruption outlives the frame; the engine loop then consumes it and
 *    runs the stream-gap carrier boundary outside the frame, under the P25 SM tick guard it takes itself; the calls end
 *    as on a sync loss, the evidence goes, and the next read resumes at the first sample past the gap;
 *  - a gap a frame-sync hunt read meets between frames resets the same way;
 *  - a pending scan tune, which makes noCarrier() return early, does not skip the reset.
 * A live read that gives way to a queued command while the device has no stream (issue #518 D4: DSD_RTL_READ_YIELD)
 * reaches the engine loop the same way, as the yield interruption: the loop applies the command at its next pump with
 * no carrier boundary and no shutdown, and the next read resumes at the next sample
 * (yield_reaches_engine_and_command_applies).
 * The boundary itself, called directly: it forgets the evidence and the multi-frame assemblies, the seeded P25 Phase 2
 * ESS state and the DMR confirmation (candidate, lock, voice gate), and moves the carrier count, but keeps the
 * carrier's codes, its received identity and the voice channel a trunking session follows. A stream start takes up the
 * stream's count, so a gap before it is never raised on the next stream.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_failures = 0;
static int g_step = 0;

static void
expect(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) {
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

/* ---------------- The fake live stream ---------------- */

/* CQPSK symbols, numbered from 1, read 8 at a time. Each gap sits before a sample: the stream's count moves before
   that sample can be read, so the read that returns it sees the count moved. */
enum { kReadSize = 8, kMaxGaps = 8 };

static uint64_t g_next = 0U;
static uint64_t g_gaps[kMaxGaps];
static int g_gap_count = 0;
static uint64_t g_seq_base = 0U;
static int g_reads = 0;
static int g_rtl_token = 0;

static void
add_gap_before_next_sample(void) {
    if (g_gap_count < kMaxGaps) {
        g_gaps[g_gap_count++] = g_next;
    }
}

/* The device has no stream (a USB stall recovery runs) while g_no_stream is set: a read then yields to a queued command
   (g_command_queued), as the live read does, and the queued command's apply (fake_pump()) brings the stream back. */
static int g_no_stream = 0;
static int g_command_queued = 0;
static int g_yields = 0;
static int g_applied = 0;
static int g_applied_after_consume = 0;

static int
fake_commands_pending(void) {
    return g_command_queued;
}

static void
fake_pump(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    if (!g_command_queued) {
        return;
    }
    g_command_queued = 0;
    g_applied++;
    g_applied_after_consume += state->input_interrupted == 0 && state->input_interrupt_reason == DSD_INTERRUPT_NONE;
    g_no_stream = 0;
}

static int
fake_read_ex(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (!out || !out_got || count == 0U) {
        return -1;
    }
    if (g_no_stream && g_command_queued) {
        g_yields++;
        *out_got = 0;
        return DSD_RTL_READ_YIELD;
    }
    g_reads++;
    size_t n = count < kReadSize ? count : kReadSize;
    for (size_t i = 0; i < n; i++) {
        out[i] = (float)(g_next + i + 1U);
        if (flags) {
            flags[i] = 0U;
        }
    }
    g_next += (uint64_t)n;
    *out_got = (int)n;
    return 0;
}

static int
fake_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    return fake_read_ex(rtl_ctx, out, NULL, count, out_got);
}

static double
fake_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return 0.0;
}

static uint64_t
fake_discontinuity_seq(void) {
    uint64_t seq = g_seq_base;
    for (int i = 0; i < g_gap_count; i++) {
        seq += g_next > g_gaps[i] ? 1U : 0U;
    }
    return seq;
}

static int
fake_output_kind(void) {
    return RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 4800;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    }
    return 0;
}

static uint32_t
fake_generation(void) {
    return 1U;
}

static unsigned int
fake_output_rate_hz(void) {
    return 4800U;
}

static void
install_fake_stream(void) {
    dsd_rtl_stream_io_hooks_set(
        (dsd_rtl_stream_io_hooks){.read = fake_read, .return_pwr = fake_pwr, .read_ex = fake_read_ex});
    static const dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .output_kind = fake_output_kind,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_generation,
        .discontinuity_seq = fake_discontinuity_seq,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

/* Whether symbol @p got is sample @p want: the samples are whole numbers, so within a quarter. */
static int
reads_sample(float got, int want) {
    return fabsf(got - (float)want) < 0.25f;
}

/* Read @p count symbols from the stream as the radio input; returns the last one. */
static float
read_symbols(dsd_opts* opts, dsd_state* state, int count, int have_sync) {
    const int saved_type = opts->audio_in_type;
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (struct RtlSdrContext*)&g_rtl_token;
    float symbol = 0.0f;
    for (int i = 0; i < count; i++) {
        symbol = getSymbol(opts, state, have_sync);
    }
    opts->audio_in_type = saved_type;
    /* No stream runs for the engine to stop. */
    state->rtl_ctx = NULL;
    return symbol;
}

/* ---------------- What the gap resets ---------------- */

static void
begin_call(dsd_state* state) {
    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_DMR_BS_VOICE_POS, 0U, 5678U, 1234U);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.policy_target_id = 1234U;
    expect("a call begins", dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) > 0);
}

static uint8_t
call_phase(const dsd_state* state, uint8_t* reason) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    if (dsd_call_state_get(state, 0U, &call) <= 0) {
        *reason = 0xFFU;
        return 0xFFU;
    }
    *reason = call.end_reason;
    return call.phase;
}

static int
call_ended_as_sync_loss(const dsd_state* state) {
    uint8_t reason = 0U;
    return call_phase(state, &reason) == DSD_CALL_PHASE_ENDED && reason == DSD_CALL_END_SYNC_LOSS;
}

static int
call_active(const dsd_state* state) {
    uint8_t reason = 0U;
    return call_phase(state, &reason) == DSD_CALL_PHASE_ACTIVE;
}

/* An open call on a carrier whose codes are decoded and whose transmissions have proved themselves. */
static void
seed_carrier(dsd_state* state) {
    begin_call(state);
    state->dstar_confirmed = 1;
    state->p25_p1_nid_evidence = 1;
    state->p25_p1_nid_evidence_symbolcnt = 99U;
    state->dmr_color_code = 5U;
    state->dmr_confidence_locked = 1U;
    state->dmr_confidence_color_code = 5U;
    state->nac = 0x293;
}

/* The boundary's DMR confirmation reset, watched for where it runs: outside any frame, under the tick guard. Once a gap
   is raised the first reset after it is the boundary's (no no-carrier pass runs before it): g_reset_armed. */
static int g_in_frame = 0;
static int g_reset_armed = 0;
static int g_resets = 0;
static int g_resets_in_frame = 0;
static int g_resets_unguarded = 0;

// GNU ld --wrap requires these exact external symbol names.
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __real_dmr_confidence_reset(dsd_state* state);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __wrap_dmr_confidence_reset(dsd_state* state);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_dsd_engine_channel_scan_pending(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_dsd_engine_channel_scan_pending(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_getFrameSync(dsd_opts* opts, dsd_state* state);

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dmr_confidence_reset(dsd_state* state) {
    if (g_reset_armed) {
        g_reset_armed = 0;
        g_resets++;
        g_resets_in_frame += g_in_frame;
        if (p25_sm_tick_guard_try_enter()) {
            p25_sm_tick_guard_leave();
            g_resets_unguarded++;
        }
    }
    __real_dmr_confidence_reset(state);
}

/* A scan tune the engine loop sees pending for the next @p g_pending_passes asks, which noCarrier() would wait out. The
   first ask after a gap was raised notes whether the reset already ran. */
static int g_pending_passes = 0;
static uint32_t g_pending_want_seq = 0U;
static int g_pending_after_reset = -1;

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dsd_engine_channel_scan_pending(dsd_opts* opts, dsd_state* state) {
    if (g_pending_passes > 0) {
        if (g_pending_after_reset < 0) {
            g_pending_after_reset = state->carrier_seq == g_pending_want_seq;
        }
        g_pending_passes--;
        return 1;
    }
    return __real_dsd_engine_channel_scan_pending(opts, state);
}

/* The frame the gap cuts: it reads symbols, the stream has a gap, it reads on. */
static int g_frame_calls = 0;
static uint32_t g_frame_seq = 0U;

void
processFrame(dsd_opts* opts, dsd_state* state) {
    g_frame_calls++;
    g_in_frame = 1;
    const uint32_t carrier_seq = state->carrier_seq;
    const int guard_free = p25_sm_tick_guard_try_enter();
    if (guard_free) {
        p25_sm_tick_guard_leave();
    }
    expect("frame: the frame runs under the tick guard", !guard_free);
    expect("frame: its symbols before the gap are the stream's", reads_sample(read_symbols(opts, state, 8, 1), 8));
    add_gap_before_next_sample();
    g_reset_armed = 1;
    const int reads = g_reads;
    int unusable = 1;
    for (int i = 0; i < 40; i++) {
        state->symbol_replay_symbol_unusable = 0;
        unusable &= reads_sample(read_symbols(opts, state, 1, 1), 0) && state->symbol_replay_symbol_unusable == 1;
    }
    expect("frame: every symbol it reads past the gap is an unusable 0", unusable);
    expect("frame: the gap stands as a stream-gap interruption",
           state->input_interrupted == 1 && state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP);
    expect("frame: the read that met the gap was the only one past it", g_reads == reads + 1);
    expect("frame: the frame reads as lost", dsd_state_frame_read_lost(state, carrier_seq));
    expect("frame: nothing is reset inside the frame", state->carrier_seq == carrier_seq && call_active(state));
    g_frame_seq = carrier_seq;
    g_in_frame = 0;
}

static uint32_t g_seq_before = 0U;
static int g_reads_before_yield = 0;

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    g_step++;
    switch (g_step) {
        case 1:
            /* gap_unwinds_active_frame: a synced frame, which the gap cuts. */
            install_fake_stream();
            state->rtl_stream_gap_seq = 0U;
            seed_carrier(state);
            return DSD_SYNC_P25P1_POS;
        case 2:
            /* The synced-frames loop's next hunt: the interruption outlived the frame, and the real hunt would return
               at once on it. */
            expect("unwind: the frame was decoded", g_frame_calls == 1);
            expect("unwind: the interruption outlives the frame",
                   state->input_interrupted == 1 && state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP);
            expect("unwind: nothing is reset before the engine loop consumes it",
                   state->carrier_seq == g_frame_seq && g_resets == 0);
            return DSD_SYNC_NONE;
        case 3:
            expect("unwind: the engine loop consumed the gap",
                   state->input_interrupted == 0 && state->input_interrupt_reason == DSD_INTERRUPT_NONE);
            expect("unwind: the boundary ran once, outside the frame, under the tick guard",
                   g_resets == 1 && g_resets_in_frame == 0 && g_resets_unguarded == 0);
            expect("unwind: the carrier count moved", state->carrier_seq == g_frame_seq + 1U);
            expect("unwind: the call ended as on a sync loss", call_ended_as_sync_loss(state));
            expect("unwind: the evidence went", state->dstar_confirmed == 0 && state->p25_p1_nid_evidence == 0);
            expect("unwind: the DMR confirmation restarted", state->dmr_confidence_locked == 0U);
            expect("unwind: the carrier's codes stayed", state->dmr_color_code == 5U && state->nac == 0x293);
            expect("unwind: reading resumes at the first sample past the gap",
                   reads_sample(read_symbols(opts, state, 1, 0), 9) && state->input_interrupted == 0);
            /* gap_between_frames_resets: the samples read before the gap, then a hunt read that meets it. */
            (void)read_symbols(opts, state, 7, 0);
            seed_carrier(state);
            add_gap_before_next_sample();
            g_reset_armed = 1;
            g_seq_before = state->carrier_seq;
            expect("between frames: the hunt read meets the gap",
                   reads_sample(read_symbols(opts, state, 1, 0), 0) && state->input_interrupted == 1
                       && state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP);
            return DSD_SYNC_NONE;
        case 4:
            expect("between frames: the boundary ran", g_resets == 2 && g_resets_unguarded == 0);
            expect("between frames: the carrier count moved", state->carrier_seq == g_seq_before + 1U);
            expect("between frames: the call ended as on a sync loss", call_ended_as_sync_loss(state));
            expect("between frames: the evidence went", state->dstar_confirmed == 0 && state->p25_p1_nid_evidence == 0);
            expect("between frames: the codes stayed", state->dmr_color_code == 5U);
            expect("between frames: reading resumes at the first sample past the gap",
                   reads_sample(read_symbols(opts, state, 1, 0), 17));
            /* gap_with_pending_scan_tune_resets: a scan tune pending across the next passes. */
            (void)read_symbols(opts, state, 7, 0);
            seed_carrier(state);
            add_gap_before_next_sample();
            g_reset_armed = 1;
            g_seq_before = state->carrier_seq;
            g_pending_want_seq = g_seq_before + 1U;
            g_pending_passes = 3;
            expect("pending scan tune: the hunt read meets the gap",
                   reads_sample(read_symbols(opts, state, 1, 0), 0)
                       && state->input_interrupt_reason == DSD_INTERRUPT_STREAM_GAP);
            return DSD_SYNC_NONE;
        case 5:
            expect("pending scan tune: the reset ran before the loop waited on the tune", g_pending_after_reset == 1);
            expect("pending scan tune: the boundary ran", g_resets == 3 && state->carrier_seq >= g_seq_before + 1U);
            expect("pending scan tune: the call ended", !call_active(state));
            expect("pending scan tune: the evidence went", state->dstar_confirmed == 0);
            /* yield_reaches_engine_and_command_applies: the samples the cache holds (25..32), then the stream goes
               away with a command queued, and the hunt read yields to it. */
            expect("yield: the samples before the outage read", reads_sample(read_symbols(opts, state, 8, 0), 32));
            dsd_runtime_set_controls_pending(fake_commands_pending);
            dsd_runtime_set_control_pump(fake_pump);
            g_no_stream = 1;
            g_command_queued = 1;
            g_seq_before = state->carrier_seq;
            g_reads_before_yield = g_reads;
            expect("yield: the hunt read yields to the queued command",
                   reads_sample(read_symbols(opts, state, 1, 0), 0) && state->input_interrupted == 1
                       && state->input_interrupt_reason == DSD_INTERRUPT_YIELD && g_yields == 1);
            return DSD_SYNC_NONE;
        case 6:
            expect("yield: the engine loop applied the command", g_applied == 1 && g_command_queued == 0);
            expect("yield: it consumed the yield before it applied the command", g_applied_after_consume == 1);
            expect("yield: the interruption is consumed",
                   state->input_interrupted == 0 && state->input_interrupt_reason == DSD_INTERRUPT_NONE);
            expect("yield: a yield is no carrier boundary", state->carrier_seq == g_seq_before && g_resets == 3);
            expect("yield: a yield asks for no shutdown", dsd_exitflag_load() == 0);
            expect("yield: the read yielded once", g_yields == 1 && g_reads == g_reads_before_yield);
            expect("yield: reading resumes at the next sample", reads_sample(read_symbols(opts, state, 1, 0), 33));
            dsd_runtime_set_control_pump(NULL);
            dsd_runtime_set_controls_pending(NULL);
            dsd_exitflag_store(1);
            return DSD_SYNC_NONE;
        default: dsd_exitflag_store(1); return DSD_SYNC_NONE;
    }
}

/* ---------------- The boundary itself ---------------- */

static void
test_gap_resets_evidence_and_assemblies(dsd_opts* opts, dsd_state* state) {
    state->nxdn_confirmed = 1;
    state->dpmr_confirmed = 1;
    state->dpmr_cch_evidence = 1;
    state->dstar_confirmed = 1;
    state->m17_confirmed = 1;
    state->provoice_confirmed = 1;
    state->ysf_fich_confirmed = 1;
    state->p25_p1_nid_evidence = 1;
    state->tdma_carrier = 1U;
    state->nxdn_part_of_frame = 2;
    state->nxdn_alias_block_number = 2;
    state->dmr_cach_counter = 2;
    state->dmr_embedded_signalling[0][1][0] = 1;
    state->late_entry_mi_fragment[0][1][0] = 5U;
    DSD_MEMSET(state->p25_mac_frag, 0x5A, sizeof(state->p25_mac_frag));
    state->m17_lsf[0] = 1;
    state->ysf_txt[0][0] = 'T';
    const uint32_t carrier_seq = state->carrier_seq;
    const uint32_t epoch = state->symbol_clock_epoch;
    state->carrier_change_seen = 0U;

    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_STREAM_GAP, 0);

    expect("evidence: the confirmation gates restart",
           state->nxdn_confirmed == 0 && state->dpmr_confirmed == 0 && state->dpmr_cch_evidence == 0
               && state->dstar_confirmed == 0 && state->m17_confirmed == 0 && state->provoice_confirmed == 0
               && state->ysf_fich_confirmed == 0 && state->p25_p1_nid_evidence == 0 && state->tdma_carrier == 0U);
    expect("assemblies: the partial assemblies go",
           state->nxdn_part_of_frame == 0 && state->nxdn_alias_block_number == 0 && state->dmr_cach_counter == 0
               && state->dmr_embedded_signalling[0][1][0] == 0 && state->late_entry_mi_fragment[0][1][0] == 0U
               && ((const unsigned char*)state->p25_mac_frag)[0] == 0 && state->m17_lsf[0] == 0
               && state->ysf_txt[0][0] == 0);
    expect("evidence: the carrier count and the symbol epoch move",
           state->carrier_seq == carrier_seq + 1U && state->symbol_clock_epoch != epoch);
    expect("evidence: the carrier change is noted for the hunt", state->carrier_change_seen == 1U);
}

static void
test_gap_resets_p25p2_ess_state(dsd_opts* opts, dsd_state* state) {
    state->ess_b[0][0] = 1;
    state->ess_b[1][3] = 1;
    state->ess_b_llr[0][0] = 900;
    state->fourv_counter[0] = 2;
    state->fourv_counter[1] = 3;
    state->p25_p2_ess_b_stale[0] = 0U;
    state->p25_p2_ess_b_stale[1] = 0U;
    state->p2_is_lcch = 1;
    DSD_MEMSET(&state->p25_p2_rekey[0], 0x5A, sizeof(state->p25_p2_rekey[0]));

    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_STREAM_GAP, 0);

    expect("phase 2: the slots' ESS_B and 4V counts go",
           state->ess_b[0][0] == 0 && state->ess_b[1][3] == 0 && state->ess_b_llr[0][0] == 0
               && state->fourv_counter[0] == 0 && state->fourv_counter[1] == 0);
    expect("phase 2: the ESS_B reads as the gap's left side until a 4V burst collects a fragment",
           state->p25_p2_ess_b_stale[0] == 1U && state->p25_p2_ess_b_stale[1] == 1U);
    expect("phase 2: the staged rekey and the LCCH mark go",
           ((const unsigned char*)&state->p25_p2_rekey[0])[0] == 0 && state->p2_is_lcch == 0);
}

static void
test_gap_resets_dmr_confirmation_keeps_colour_code(dsd_opts* opts, dsd_state* state) {
    state->dmr_color_code = 5U;
    state->dmr_confidence_locked = 1U;
    state->dmr_confidence_color_code = 5U;
    state->dmr_confidence_candidate_cc = 5U;
    state->dmr_confidence_candidate_count = 3U;
    state->dmr_confidence_mismatch_count = 2U;
    state->dmr_confidence_voice_sync_seen[0] = 1U;
    state->dmr_confidence_voice_open[0] = 1U;
    state->dmr_confidence_voice_count[1] = 4U;
    state->nxdn_last_ran = 7U;
    state->dpmr_color_code = 3;
    state->nac = 0x293;
    state->p2_cc_verified = 1U;

    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_STREAM_GAP, 0);

    expect("dmr: the confirmation restarts (candidate, lock, voice gate)",
           state->dmr_confidence_locked == 0U && state->dmr_confidence_candidate_count == 0U
               && state->dmr_confidence_mismatch_count == 0U && state->dmr_confidence_voice_sync_seen[0] == 0U
               && state->dmr_confidence_voice_open[0] == 0U && state->dmr_confidence_voice_count[1] == 0U);
    expect("dmr: the published colour code stays", state->dmr_color_code == 5U);
    expect("codes: the RAN, dPMR colour code, NAC and Phase 2 seed proof stay",
           state->nxdn_last_ran == 7U && state->dpmr_color_code == 3 && state->nac == 0x293
               && state->p2_cc_verified == 1U);
}

/* Whether two received identities hold the same values, every field (the record carries padding, so not memcmp). */
static int
rxid_equal(const dsd_received_identity* a, const dsd_received_identity* b) {
    return a->epoch == b->epoch && a->valid == b->valid && a->fresh == b->fresh && a->p25_wacn == b->p25_wacn
           && a->nxdn_sys == b->nxdn_sys && a->nxdn_typed_sys == b->nxdn_typed_sys && a->cc_freq_hz == b->cc_freq_hz
           && a->access == b->access && a->p25_sysid == b->p25_sysid && a->nxdn_site == b->nxdn_site
           && a->dmr_rest_lsn == b->dmr_rest_lsn && a->dmr_t3_syscode == b->dmr_t3_syscode
           && a->edacs_site == b->edacs_site && a->edacs_area == b->edacs_area && a->edacs_system == b->edacs_system
           && a->edacs_cc_lcn == b->edacs_cc_lcn && a->tetra_mcc == b->tetra_mcc && a->tetra_mnc == b->tetra_mnc
           && a->tetra_la == b->tetra_la && a->family == b->family && a->access_kind == b->access_kind
           && a->p25_rfss == b->p25_rfss && a->p25_site == b->p25_site && a->p25_lra == b->p25_lra
           && a->nxdn_category == b->nxdn_category && a->nxdn_idas_area == b->nxdn_idas_area
           && a->nxdn_idas_site_type == b->nxdn_idas_site_type && a->nxdn_idas_site_code == b->nxdn_idas_site_code
           && a->nxdn_typed_site == b->nxdn_typed_site && a->dmr_xpt_free_lcn == b->dmr_xpt_free_lcn
           && a->dmr_trunk == b->dmr_trunk && a->dmr_vendor == b->dmr_vendor && a->edacs_ea_mode == b->edacs_ea_mode
           && a->tetra_colour == b->tetra_colour
           && strncmp(a->dmr_site_text, b->dmr_site_text, sizeof a->dmr_site_text) == 0
           && strncmp(a->dmr_brand, b->dmr_brand, sizeof a->dmr_brand) == 0;
}

static void
test_stream_gap_mid_call_keeps_follow(dsd_opts* opts, dsd_state* state) {
    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    p25->state = P25_SM_TUNED;
    dmr->state = DMR_SM_TUNED;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 851012500L;
    state->p25_vc_freq[0] = 851012500L;
    state->rx_id.valid = 0x3U;
    state->rx_id.p25_wacn = 0xBEE00U;
    state->rx_id.p25_sysid = 0x3A5U;
    /* Parts the no-carrier pass or a reset would change: fresh (cleared by the pass), and values beside the system. */
    state->rx_id.fresh = 0x3U;
    state->rx_id.p25_rfss = 2U;
    state->rx_id.p25_site = 7U;
    state->rx_id.cc_freq_hz = 851012500U;
    const dsd_received_identity identity = state->rx_id;
    begin_call(state);
    const int resets = g_resets;
    g_reset_armed = 1;

    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_STREAM_GAP, 0);

    expect("follow: the call ends as on a sync loss", call_ended_as_sync_loss(state));
    expect("follow: the state machines keep the voice channel they follow",
           p25_sm_get_state(p25) == P25_SM_TUNED && dmr->state == DMR_SM_TUNED);
    expect("follow: the followed assignment stays", opts->trunk_is_tuned == 1 && state->trunk_vc_freq[0] == 851012500L
                                                        && state->trunk_vc_freq[1] == 851012500L
                                                        && state->p25_vc_freq[0] == 851012500L);
    expect("follow: the received identity stays", rxid_equal(&identity, &state->rx_id));
    expect("follow: the boundary took the tick guard", g_resets == resets + 1 && g_resets_unguarded == 0);
    const int guard_free = p25_sm_tick_guard_try_enter();
    if (guard_free) {
        p25_sm_tick_guard_leave();
    }
    expect("follow: the boundary released the tick guard", guard_free);
    p25->state = P25_SM_IDLE;
    dmr->state = DMR_SM_IDLE;
    opts->trunk_enable = 0;
    opts->trunk_is_tuned = 0;
}

/* A stream start takes up the stream's count: a gap the decoder never read past is not raised on the next stream. */
static void
test_stream_start_takes_up_the_count(dsd_opts* opts, dsd_state* state) {
    install_fake_stream();
    g_seq_base = 41U;
    state->rtl_stream_gap_seq = 3U;
    const uint32_t carrier_seq = state->carrier_seq;
    dsd_engine_note_stream_source(opts, state, 0);
    expect("stream start: the decoder takes up the stream's count",
           state->rtl_stream_gap_seq == fake_discontinuity_seq());
    expect("stream start: the same source's start is no carrier boundary", state->carrier_seq == carrier_seq);
    g_seq_base = 0U;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;

    const int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    expect("the engine ran", rc == 0);
    expect("the script ran to its end", g_step == 6);
    dsd_exitflag_store(0);

    test_gap_resets_evidence_and_assemblies(opts, state);
    test_gap_resets_p25p2_ess_state(opts, state);
    test_gap_resets_dmr_confirmation_keeps_colour_code(opts, state);
    test_stream_gap_mid_call_keeps_follow(opts, state);
    test_stream_start_takes_up_the_count(opts, state);

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    freeState(state);
    free(state);
    free(opts);
    if (g_failures == 0) {
        printf("ENGINE_STREAM_GAP: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
