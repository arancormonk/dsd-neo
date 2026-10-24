// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The startup control-channel hunt through the real engine loop (issue #703): it runs on the decoder thread from the
 * outer loop and between frames of a continuous sync, an input switch pending its boundary holds it, a mode change
 * reaches DMR even while another protocol keeps syncing, and a hunt whose owner validated control named is finished
 * between frames. CAP+ recovery must keep running between frames after startup ends: loss and probe deadlines,
 * pending backend tunes, and reacquisition on a new rest channel. Validated DMR content that names DMR without
 * confirming a control channel there (a data preamble, a CAP+ status naming an unmapped rest) never stops the hunt.
 *
 * Auto (-fa) presumes P25 for the startup hunt too: noise NXDN syncs and the GFSK chain the frame-sync hunt forces do
 * not stop it, the frame-sync hunt hears a P25 probe in flight (and rotates between the P25 profiles only then),
 * validated NXDN traffic holds its window, NXDN control (a CAC, a confirmed Type-D SCCH) names NXDN and ends it, and a
 * selected EDACS session (-fh -fa, an EDACS-to-Auto switch) never starts it.
 *
 * getFrameSync() and processFrame() are fakes. The first fake sync, after the engine set its input up, turns rigctl on
 * (a peer no request reaches: the trunk tuning and rigctl query hooks are replaced) and loads a three-row channel map;
 * each later call advances the decode clock.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/edacs_session.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/nxdn/nxdn_cc_hunt.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

#define T0_NS   1000000000000000000ULL
#define ROW0_HZ 451000000L
#define ROW1_HZ 452000000L
#define ROW2_HZ 453000000L

enum scenario {
    SC_OUTER_LOOP = 0,
    SC_SYNCED_LOOP,
    SC_NAMED_BETWEEN_FRAMES,
    SC_INPUT_SWITCH,
    SC_MODE_CHANGE_DMR,
    SC_INPUT_SWITCH_PENDING,
    SC_INPUT_SWITCH_NEW_REQUEST,
    SC_NXDN_OUTER_LOOP,
    SC_NXDN_SYNCED_LOOP,
    SC_NXDN_LOST_CC,
    SC_NXDN_CALL_RETURN,
    SC_CAPPLUS_LOST_REST,
    SC_CAPPLUS_PENDING_PROBE,
    SC_CAPPLUS_LIVE_REST,
    SC_DMR_NAMED_UNCONFIRMED,
    SC_CAPPLUS_UNMAPPED_REST,
    SC_RAS_SYNCED_APPLY,
    SC_AUTO_NOISE_PROBE,
    SC_AUTO_TYPE_D,
    SC_AUTO_NXDN_TRAFFIC,
    SC_EDACS_SESSION,
    SC_PROBE_HOOK_RESTART,
};

static enum scenario g_scenario;
static int g_calls;
static uint64_t g_now_ms;
static int g_tunes;
static long g_first_tune;
static long g_hw_freq;
static uint64_t g_tunes_before_ms; // decode time of the last tune
static int g_validated_sent;
static int g_dmr_armed_seen; // DMR's startup window was armed before the mode change
static int g_failures;
static uint64_t g_pending_request;    // the first probe held in flight
static uint64_t g_new_device_request; // a tune the new device was given between the switch and its boundary
static int g_new_request_kept;
static long g_probe_freqs[8];
static uint64_t g_probe_times_ms[8];
static uint64_t g_probe_completed_ms;
static uint64_t g_last_content_ms;
static int g_ras_frames; // SC_RAS_SYNCED_APPLY: frames dispatched
static int g_ft;         // the run's frame set (run_scenario())
static int g_p25_armed_seen;
static int g_probe_in_flight_seen; // the frame-sync hunt heard a P25 probe in flight
static atomic_int g_guard_held;    // guard_holder() holds the P25 SM tick guard
static atomic_int g_guard_release; // guard_holder() may let it go

/* The watchdog's part: hold the P25 SM tick guard until released. */
static DSD_THREAD_RETURN_TYPE
guard_holder(void* arg) {
    (void)arg;
    p25_sm_tick_guard_enter();
    atomic_store(&g_guard_held, 1);
    while (!atomic_load(&g_guard_release)) {
        dsd_thread_yield();
    }
    p25_sm_tick_guard_leave();
    DSD_THREAD_RETURN;
}

/* The frame-sync probe query while another thread holds the tick guard, as a watchdog tick does; -1 if no thread. */
static int
probe_answer_with_guard_busy(const dsd_opts* opts, const dsd_state* state) {
    atomic_store(&g_guard_held, 0);
    atomic_store(&g_guard_release, 0);
    dsd_thread_t holder;
    if (dsd_thread_create(&holder, guard_holder, NULL) != 0) {
        return -1;
    }
    while (!atomic_load(&g_guard_held)) {
        dsd_thread_yield();
    }
    const int answer = dsd_frame_sync_hook_p25_probe_in_flight(opts, state);
    atomic_store(&g_guard_release, 1);
    dsd_thread_join(holder);
    return answer;
}

static int
capplus_scenario(void) {
    return g_scenario == SC_CAPPLUS_LOST_REST || g_scenario == SC_CAPPLUS_PENDING_PROBE
           || g_scenario == SC_CAPPLUS_LIVE_REST;
}

static int
expect(int cond, const char* what) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL [scenario %d]: %s\n", (int)g_scenario, what);
        g_failures++;
        return 1;
    }
    return 0;
}

static dsd_trunk_tune_result
stub_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    if (g_tunes < 8) {
        g_probe_freqs[g_tunes] = freq;
        g_probe_times_ms[g_tunes] = g_now_ms;
    }
    g_tunes++;
    if ((g_scenario == SC_INPUT_SWITCH_PENDING || g_scenario == SC_CAPPLUS_PENDING_PROBE) && g_pending_request == 0U) {
        g_pending_request = request_id;
        opts->rtlsdr_center_freq = (uint32_t)freq;
        state->trunk_cc_freq = freq;
        return DSD_TRUNK_TUNE_RESULT_PENDING;
    }
    if (g_first_tune == 0) {
        g_first_tune = freq;
    }
    g_tunes_before_ms = g_now_ms;
    g_hw_freq = freq;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    state->trunk_cc_freq = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
stub_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
stub_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

static long int
stub_query(const dsd_opts* opts) {
    (void)opts;
    return g_hw_freq;
}

static void
advance(uint64_t ms) {
    g_now_ms += ms;
    dsd_decode_clock_test_set_ns(T0_NS + g_now_ms * 1000000ULL);
}

/* After the engine set its input up: a rigctl-moved radio parked on a dead start frequency, and a channel map. */
static void
install_session(dsd_opts* opts, dsd_state* state) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = stub_tune_to_cc;
    hooks.tune_to_freq_request = stub_tune_to_freq;
    hooks.return_to_cc_request = stub_return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){.get_current_freq_hz = stub_query});
    opts->use_rigctl = 1;
    opts->trunk_enable = 1;
    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", "site.csv");
    state->trunk_lcn_freq[0] = ROW0_HZ;
    state->trunk_lcn_freq[1] = ROW1_HZ;
    state->trunk_lcn_freq[2] = ROW2_HZ;
    state->lcn_freq_count = 3;
    if (capplus_scenario()) {
        g_hw_freq = ROW0_HZ;
        opts->rtlsdr_center_freq = (uint32_t)g_hw_freq;
        state->trunk_chan_map[1] = ROW0_HZ;
        state->trunk_chan_map[3] = ROW1_HZ;
        state->trunk_chan_map[5] = ROW2_HZ;
    }
}

/* An input switch's commit, as the command (or the engine's fallback) makes it: under the tick guard, with the
   replacement. */
static void
commit_input_switch(dsd_state* state) {
    p25_sm_tick_guard_enter();
    dsd_engine_note_input_replaced(state);
    p25_sm_tick_guard_leave();
}

static int
finish(void) {
    dsd_exitflag_store(1);
    return DSD_SYNC_NONE;
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

/* A complete idle Capacity Plus status, through the real parser's CRC/FEC-approved entry point. */
static void
capplus_status(dsd_opts* opts, dsd_state* state, uint8_t rest) {
    uint8_t bytes[24] = {0x3e, 0x10, (uint8_t)(0xc0U | rest)};
    uint8_t bits[196] = {0};
    for (int i = 0; i < 80; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1);
    }
    dmr_cspdu(opts, state, bits, bytes, 1, 0);
    g_validated_sent = 1;
}

/* A CRC-valid data preamble CSBK (0x3D): it names DMR the owner and confirms no control channel. */
static void
dmr_preamble(dsd_opts* opts, dsd_state* state) {
    uint8_t bytes[12] = {0x3d, 0x00};
    uint8_t bits[196] = {0};
    for (int i = 0; i < 80; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1);
    }
    dmr_cspdu(opts, state, bits, bytes, 1, 0);
    g_validated_sent = 1;
}

/* Frames otherwise decode to nothing: in particular, a repeated raw data sync or a discarded frame does not run a
   DMR voice handler's internal tick. Recovery must still advance once the startup hunt has ended on valid control. */
/* SC_RAS_SYNCED_APPLY: a trunk tune's drop the P25 watchdog requested while a DMR frame was handled is applied after
   the synced frame's ticks, which resets the DMR RAS evidence the carrier left (dmr_ras_reset()) before the next frame:
   neither a confirmation nor one signature carries over. This fake processFrame() applies nothing itself. */
static void
ras_synced_apply_frame(dsd_state* state) {
    g_ras_frames++;
    switch (g_ras_frames) {
        case 1:
            dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 1U);
            dmr_ras_note_signature(state);
            dmr_ras_note_signature(state);
            expect(dmr_ras_confirmed(state) && (state->rx_id.valid & DSD_RXID_DMR_RAS) != 0U, "RAS confirmed");
            (void)dsd_rxid_request_trunk_tune(state);
            expect(state->rx_id_trunk_tune_pending == 1U && dmr_ras_confirmed(state), "the watchdog's request waits");
            break;
        case 2:
            expect(state->rx_id_trunk_tune_pending == 0U, "the synced loop applied the drop");
            expect(!dmr_ras_confirmed(state) && (state->rx_id.valid & DSD_RXID_DMR_RAS) == 0U,
                   "the synced loop's apply resets a confirmation");
            dmr_ras_note_signature(state);
            (void)dsd_rxid_request_trunk_tune(state);
            break;
        case 3:
            dmr_ras_note_signature(state);
            expect(!dmr_ras_confirmed(state) && (state->rx_id.valid & DSD_RXID_DMR_RAS) == 0U,
                   "one signature on each side of the synced loop's apply does not confirm");
            break;
        default: break;
    }
}

void
processFrame(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    if (g_scenario == SC_RAS_SYNCED_APPLY) {
        ras_synced_apply_frame(state);
        return;
    }
    if (g_scenario == SC_NAMED_BETWEEN_FRAMES && g_tunes == 1 && !g_validated_sent && g_now_ms >= 6000U) {
        p25_sm_note_validated_cc(opts, state);
        g_validated_sent = 1;
    }
    if (g_scenario == SC_AUTO_TYPE_D && g_tunes == 1 && !g_validated_sent && g_now_ms >= g_probe_times_ms[0] + 500U) {
        state->nxdn_confirm_scch_streak = 2; // the SCCH's own run of short CRCs confirmed it
        nxdn_cc_hunt_note_type_d(opts, state);
        g_validated_sent = 1;
    }
    // Validated NXDN traffic on the start frequency (a conventional call): a frame's own CRCs confirmed it.
    if (g_scenario == SC_AUTO_NXDN_TRAFFIC && g_now_ms <= 4000U) {
        state->nxdn_confirm_lock_m = dsd_decode_now_mono_s();
        g_last_content_ms = g_now_ms;
    }
    if ((g_scenario == SC_NXDN_LOST_CC || g_scenario == SC_NXDN_CALL_RETURN) && g_now_ms <= 2000U) {
        nxdn_cc_hunt_note_control(opts, state);
        g_validated_sent = 1;
    }
    // The reported CAP+ stall: validated DMR content on the start frequency that names no rest channel, then silence.
    if ((g_scenario == SC_DMR_NAMED_UNCONFIRMED || g_scenario == SC_CAPPLUS_UNMAPPED_REST) && g_now_ms <= 1000U) {
        state->dmr_confidence_lock_m = dsd_decode_now_mono_s(); // the burst passed the colour-code gate
        if (g_scenario == SC_DMR_NAMED_UNCONFIRMED) {
            dmr_preamble(opts, state);
        } else {
            capplus_status(opts, state, 7U); // rest LSN 7, which the map lacks
        }
        g_last_content_ms = g_now_ms;
    }
    if (capplus_scenario()) {
        if (g_now_ms <= 2000U || g_scenario == SC_CAPPLUS_LIVE_REST) {
            capplus_status(opts, state, 1U);
        } else if (g_hw_freq == ROW2_HZ) {
            capplus_status(opts, state, 5U);
        }
    }
}

static int
capplus_sync(dsd_opts* opts, dsd_state* state) {
    if (g_now_ms == 8000U) {
        expect(g_tunes == 0, "CAP+ retains its six-second loss grace after the last validated status");
        expect(dsd_frame_sync_hook_decoder_ticks_due(opts, state), "known DMR recovery needs decoder-thread ticks");
    }
    if (g_scenario == SC_CAPPLUS_PENDING_PROBE && g_pending_request != 0U && g_probe_completed_ms == 0U
        && g_now_ms >= g_probe_times_ms[0] + 1000U) {
        expect(g_tunes == 1, "no second CAP+ probe while the first backend tune is pending");
        g_hw_freq = ROW1_HZ;
        g_probe_completed_ms = g_now_ms;
        dsd_trunk_tuning_request_publish(g_pending_request, DSD_TRUNK_TUNE_RESULT_OK);
    }
    if (g_now_ms > 18000U) {
        return finish();
    }
    // Under -fa, unrelated syncs after the loss must not starve DMR recovery either.
    return opts->frame_nxdn48 && g_now_ms > 2000U && g_hw_freq != ROW2_HZ ? DSD_SYNC_NXDN_POS
                                                                          : DSD_SYNC_DMR_BS_DATA_POS;
}

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
/* No watchdog thread: it never runs a startup hunt (P25_SM_COLD_START covers that), and here, where decode time runs
   far ahead of the real clock, one watchdog tick holding the P25 SM tick guard could span every pass of a scenario,
   whose outer loop only tries the guard, and the decoder-ticks hook would read the busy guard as due. */
void __wrap_p25_sm_watchdog_start(dsd_opts* opts, dsd_state* state);

void
__wrap_p25_sm_watchdog_start(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

int
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    g_calls++;
    if (g_calls == 1) {
        install_session(opts, state);
    }
    advance(250U);
    switch (g_scenario) {
        case SC_OUTER_LOOP:
            if (g_now_ms == 250U) {
                // The session just became eligible after this pass's startup tick: its first tick is due, which a
                // silent input must not starve; without a channel map nothing is.
                char map[sizeof opts->chan_in_file];
                DSD_SNPRINTF(map, sizeof map, "%s", opts->chan_in_file);
                opts->chan_in_file[0] = '\0';
                expect(!dsd_frame_sync_hook_decoder_ticks_due(opts, state), "no decoder timers without a channel map");
                DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", map);
                expect(dsd_frame_sync_hook_decoder_ticks_due(opts, state),
                       "an eligible session's first startup tick is a decoder timer");
            }
            if (g_now_ms == 2000U) {
                expect(dsd_frame_sync_hook_decoder_ticks_due(opts, state),
                       "an armed startup window is a decoder timer a silent input must not starve");
                opts->trunk_enable = 0; // turned off after this pass's tick: the window still needs a tick to go
                expect(dsd_frame_sync_hook_decoder_ticks_due(opts, state),
                       "an armed window stays a decoder timer once trunking is off");
                opts->trunk_enable = 1;
            }
            if (g_now_ms > 7000U) {
                return finish();
            }
            return DSD_SYNC_NONE;
        case SC_SYNCED_LOOP:
            // Unvalidated DMR data syncs that never stop: the outer loop runs only before the first one.
            if (g_now_ms > 7000U) {
                return finish();
            }
            return DSD_SYNC_DMR_BS_DATA_POS;
        case SC_RAS_SYNCED_APPLY:
            if (g_ras_frames >= 3) {
                return finish();
            }
            return DSD_SYNC_DMR_BS_DATA_POS;
        case SC_NAMED_BETWEEN_FRAMES:
            if (g_now_ms > 7500U) {
                return finish();
            }
            return DSD_SYNC_P25P1_POS;
        case SC_INPUT_SWITCH:
            if (g_now_ms == 4750U) {
                commit_input_switch(state); // an input switch committed just before the window runs out
            }
            if (g_now_ms == 5000U) {
                // The first read of the new input: a silent one waits here, so the window must already be running.
                expect(dsd_frame_sync_hook_decoder_ticks_due(opts, state),
                       "the new input's startup window is armed before its first read");
            }
            if (g_now_ms > 11000U) {
                return finish();
            }
            return DSD_SYNC_NONE;
        case SC_INPUT_SWITCH_PENDING:
            if (g_now_ms == 6000U) {
                commit_input_switch(state); // the input is replaced with the probe still in flight
            }
            if (g_now_ms == 6500U && g_pending_request != 0U) {
                dsd_trunk_tuning_request_publish(g_pending_request, DSD_TRUNK_TUNE_RESULT_FAILED); // the old device
            }
            if (g_now_ms > 8000U) {
                return finish();
            }
            return DSD_SYNC_NONE;
        case SC_INPUT_SWITCH_NEW_REQUEST:
            if (g_now_ms == 1000U) {
                commit_input_switch(state);
                // Recovery tuned the new device before the engine reached the switch's boundary.
                g_new_device_request = dsd_trunk_tuning_request_begin();
                dsd_trunk_tuning_request_mark_ready(g_new_device_request);
            }
            if (g_now_ms == 1250U) {
                g_new_request_kept = dsd_trunk_tuning_pending_request() == g_new_device_request;
                dsd_trunk_tuning_request_publish(g_new_device_request, DSD_TRUNK_TUNE_RESULT_OK);
            }
            if (g_now_ms > 1500U) {
                return finish();
            }
            return DSD_SYNC_NONE;
        case SC_NXDN_OUTER_LOOP:
            if (g_now_ms > 9000U) {
                return finish();
            }
            return DSD_SYNC_NONE;
        case SC_NXDN_SYNCED_LOOP:
            // Unvalidated NXDN syncs that never stop: noise passes NXDN's frame sync often.
            if (g_now_ms > 9000U) {
                return finish();
            }
            return DSD_SYNC_NXDN_POS;
        case SC_NXDN_LOST_CC:
            // -fa: CACs name NXDN for two seconds, then only syncs that never validate.
            if (g_now_ms == 3000U || g_now_ms == 9500U) {
                expect(!dsd_frame_sync_hook_p25_probe_in_flight(opts, state),
                       "an NXDN owner's hunt is no P25 probe: the full rotation runs");
            }
            if (g_now_ms > 11000U) {
                return finish();
            }
            return DSD_SYNC_NXDN_POS;
        case SC_AUTO_NOISE_PROBE:
            // Auto on a dead frequency: noise passes NXDN's frame sync, and the frame-sync hunt forced the GFSK chain
            // on a 2400/4 or 9600/2 dwell.
            state->rf_mod = 2;
            if (g_now_ms == 2000U) {
                expect(dsd_frame_sync_hook_decoder_ticks_due(opts, state),
                       "Auto's armed startup window is a decoder timer");
                expect(!dsd_frame_sync_hook_p25_probe_in_flight(opts, state), "no probe while the window runs");
            }
            if (g_tunes >= 1 && g_now_ms == g_probe_times_ms[0] + 500U) {
                g_probe_in_flight_seen = dsd_frame_sync_hook_p25_probe_in_flight(opts, state);
            }
            if (g_now_ms > 9000U) {
                return finish();
            }
            return DSD_SYNC_NXDN_POS;
        case SC_AUTO_TYPE_D:
            if (g_validated_sent && g_now_ms == 20000U) {
                expect(!dsd_frame_sync_hook_p25_probe_in_flight(opts, state), "no P25 probe after a Type-D site");
                expect(!dsd_frame_sync_hook_decoder_ticks_due(opts, state) && !nxdn_cc_hunt_due(opts, state),
                       "nor any hunt due");
            }
            if (g_now_ms > 30000U) {
                return finish();
            }
            return DSD_SYNC_NXDN_POS;
        case SC_AUTO_NXDN_TRAFFIC:
            if (g_now_ms > 11000U) {
                return finish();
            }
            return DSD_SYNC_NXDN_POS;
        case SC_PROBE_HOOK_RESTART:
            // The previous run's last answer was a probe; this run's hooks start it over, so a query the watchdog's
            // guard leaves to that answer finds none.
            expect(probe_answer_with_guard_busy(opts, state) == 0, "a new run forgets the last run's probe answer");
            return finish();
        case SC_EDACS_SESSION:
            if (g_ft == 5 && g_now_ms == 3000U) {
                // The EDACS session switched to Auto mid-session: the EDACS addressing it selected stays.
                dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_INTERACTIVE, opts, state);
                expect(opts->frame_p25p1 == 1 && opts->frame_nxdn96 == 1, "the switch enables Auto's decoders");
            }
            g_p25_armed_seen |= p25_sm_cold_start_active(p25_sm_get_ctx());
            if (g_now_ms > 12000U) {
                return finish();
            }
            return DSD_SYNC_NONE;
        case SC_NXDN_CALL_RETURN:
            // -fi: CACs for two seconds, a followed call, its return at 3 s, then syncs that never validate.
            if (g_now_ms == 2250U) {
                opts->trunk_is_tuned = 1;
            }
            if (g_now_ms == 3000U) {
                opts->trunk_is_tuned = 0;
            }
            if (g_now_ms > 11000U) {
                return finish();
            }
            return DSD_SYNC_NXDN_POS;
        case SC_CAPPLUS_LOST_REST:
        case SC_CAPPLUS_PENDING_PROBE:
        case SC_CAPPLUS_LIVE_REST: return capplus_sync(opts, state);
        case SC_DMR_NAMED_UNCONFIRMED:
        case SC_CAPPLUS_UNMAPPED_REST:
            if (g_now_ms > 14000U) {
                return finish();
            }
            return g_now_ms <= 1000U ? DSD_SYNC_DMR_BS_DATA_POS : DSD_SYNC_NONE;
        case SC_MODE_CHANGE_DMR:
            if (g_now_ms == 2750U) {
                g_dmr_armed_seen = dmr_sm_cold_start_active(dmr_sm_get_ctx());
            }
            if (g_now_ms == 3000U) {
                opts->frame_nxdn48 = opts->frame_nxdn96 = opts->frame_provoice = 1; // switched to -fa mid-session
                opts->frame_p25p1 = opts->frame_p25p2 = 1;
            }
            if (g_now_ms > 9000U) {
                return finish();
            }
            return DSD_SYNC_DMR_BS_DATA_POS;
    }
    return finish();
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

/* Frame sets: 0 -fs, 1 -ft, 2 -fi, 3 the -fa decoders by hand, 4 Auto (the -fa preset), 5 EDACS/ProVoice (switched to
   Auto at 3 s), 6 -fh -fa. */
static int
run_scenario(enum scenario sc, int ft) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    g_scenario = sc;
    g_calls = 0;
    g_now_ms = 0U;
    g_tunes = 0;
    g_first_tune = 0;
    g_hw_freq = 450000000L;
    g_tunes_before_ms = 0U;
    g_validated_sent = 0;
    g_dmr_armed_seen = 0;
    g_pending_request = 0U;
    g_new_device_request = 0U;
    g_new_request_kept = 0;
    DSD_MEMSET(g_probe_freqs, 0, sizeof(g_probe_freqs));
    DSD_MEMSET(g_probe_times_ms, 0, sizeof(g_probe_times_ms));
    g_probe_completed_ms = 0U;
    g_last_content_ms = 0U;
    g_ras_frames = 0;
    g_ft = ft;
    g_p25_armed_seen = 0;
    g_probe_in_flight_seen = 0;
    dsd_decode_clock_use_test(T0_NS);
    dsd_exitflag_store(0);
    dsd_trunk_tuning_requests_reset();

    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;
    opts->frame_dstar = opts->frame_x2tdma = opts->frame_ysf = opts->frame_m17 = 0;
    opts->frame_nxdn48 = opts->frame_nxdn96 = opts->frame_provoice = opts->frame_dpmr = 0;
    opts->frame_dmr = ft == 2 ? 0 : 1;
    opts->frame_p25p1 = opts->frame_p25p2 = (ft == 1 || ft == 3) ? 1 : 0;
    opts->frame_nxdn48 = (ft == 2 || ft == 3) ? 1 : 0;
    opts->frame_nxdn96 = opts->frame_provoice = ft == 3 ? 1 : 0;
    if (ft == 4 || ft == 6) {
        expect(dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0, "-fa");
    }
    if (ft == 5) {
        expect(dsd_apply_decode_mode_preset(DSDCFG_MODE_EDACS_PV, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0,
               "EDACS/ProVoice");
    }
    if (ft == 6) {
        state->ea_mode = 0; // -fh before -fa: EDACS standard addressing
    }

    const int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    expect(rc == 0, "the engine run ends cleanly");

    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    switch (sc) {
        case SC_OUTER_LOOP:
            expect(g_tunes >= 1 && g_first_tune == ROW0_HZ, "the outer loop runs the startup hunt");
            expect(g_tunes_before_ms > 5000U, "not before the window ran out");
            break;
        case SC_SYNCED_LOOP:
            expect(g_tunes >= 1 && g_first_tune == ROW0_HZ, "continuous unvalidated syncs do not starve the hunt");
            expect(state->trunk_cc_freq == 0 && state->p25_cc_freq == 0, "a probe is no control channel");
            break;
        case SC_NAMED_BETWEEN_FRAMES:
            expect(g_validated_sent == 1, "the validated block was decoded on the probe");
            expect(p25->cold_start_hunt == 0 && state->p25_cc_freq == ROW0_HZ,
                   "a hunt whose owner was named is finished between frames");
            expect(g_tunes == 1, "and never probes again");
            break;
        case SC_INPUT_SWITCH:
            expect(g_tunes >= 1 && g_tunes_before_ms > 9750U,
                   "an input switch commit holds the probe until a full window after its boundary");
            break;
        case SC_INPUT_SWITCH_PENDING:
            expect(g_pending_request != 0U, "a probe was left in flight across the switch");
            expect(dsd_trunk_tuning_pending_request() == 0U
                       && dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()),
                   "the replaced device's requests no longer gate the new input's frames");
            break;
        case SC_INPUT_SWITCH_NEW_REQUEST:
            expect(g_new_device_request != 0U && g_new_request_kept,
                   "a tune the new device was given before the switch's boundary still gates its frames");
            break;
        case SC_NXDN_OUTER_LOOP:
            expect(g_tunes >= 1 && g_first_tune == ROW0_HZ, "an NXDN session hunts a dead start frequency");
            expect(g_tunes_before_ms > 6000U, "not before NXDN's window ran out");
            expect(state->trunk_cc_freq == 0 && state->p25_cc_freq == 0, "a probe is no control channel");
            break;
        case SC_NXDN_SYNCED_LOOP:
            expect(g_tunes >= 1 && g_first_tune == ROW0_HZ, "continuous unvalidated NXDN syncs do not starve the hunt");
            expect(state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_UNKNOWN, "nor name an owner");
            break;
        case SC_NXDN_LOST_CC:
            expect(g_validated_sent == 1 && state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_NXDN,
                   "CACs named NXDN the owner");
            expect(g_tunes >= 1 && g_first_tune == ROW0_HZ && g_tunes_before_ms > 8000U,
                   "a lost NXDN control channel is hunted between frames once the grace ran out");
            expect(state->p25_cc_freq == 450000000L, "the return destination stays until a CAC proves another");
            break;
        case SC_NXDN_CALL_RETURN:
            expect(g_tunes >= 1 && g_first_tune == ROW0_HZ && g_tunes_before_ms > 9000U,
                   "after a call's return, a quiet control channel is hunted between frames once the grace ran out");
            break;
        case SC_CAPPLUS_LOST_REST:
        case SC_CAPPLUS_PENDING_PROBE:
            expect(g_validated_sent && state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_DMR,
                   "validated CAP+ rest status names DMR the recovery owner");
            expect(g_tunes == 3 && g_probe_freqs[0] == ROW1_HZ && g_probe_freqs[1] == ROW0_HZ
                       && g_probe_freqs[2] == ROW2_HZ,
                   "CAP+ hunts inactive frequencies in order, revisits its anchor, then stays on the active rest");
            expect(g_probe_times_ms[0] > 8000U && g_probe_times_ms[0] <= 8500U,
                   "continuous syncs do not extend CAP+ loss grace beyond two engine ticks");
            const uint64_t first_ready = sc == SC_CAPPLUS_PENDING_PROBE ? g_probe_completed_ms : g_probe_times_ms[0];
            expect(g_probe_times_ms[1] > first_ready + 2000U && g_probe_times_ms[1] <= first_ready + 2500U
                       && g_probe_times_ms[2] > g_probe_times_ms[1] + 2000U
                       && g_probe_times_ms[2] <= g_probe_times_ms[1] + 2500U,
                   "each inactive CAP+ probe gets two seconds from backend completion before the hunt advances");
            expect(g_hw_freq == ROW2_HZ && state->trunk_cc_freq == ROW2_HZ && dmr->cc_confirmed && !dmr->cc_acquiring,
                   "validated CAP+ status confirms the new rest channel and stops recovery");
            break;
        case SC_CAPPLUS_LIVE_REST:
            expect(g_tunes == 0 && dmr->cc_confirmed && state->trunk_cc_freq == ROW0_HZ,
                   "continuous validated CAP+ rest status keeps the receiver parked");
            break;
        case SC_DMR_NAMED_UNCONFIRMED:
        case SC_CAPPLUS_UNMAPPED_REST:
            expect(g_validated_sent && state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_DMR && !dmr->cc_confirmed,
                   "validated DMR content named DMR without confirming a control channel");
            expect(g_tunes >= 2 && g_probe_freqs[0] == ROW0_HZ && g_probe_freqs[1] == ROW1_HZ,
                   "the session goes on down the map instead of staying where no rest channel is");
            expect(g_probe_times_ms[0] > g_last_content_ms + 6000U && g_probe_times_ms[0] <= g_last_content_ms + 6500U,
                   "six seconds after the last validated content, not before");
            break;
        case SC_RAS_SYNCED_APPLY: expect(g_ras_frames == 3, "three frames dispatched through the synced loop"); break;
        case SC_MODE_CHANGE_DMR:
            expect(g_dmr_armed_seen == 1, "DMR armed its startup window between frames");
            expect(dmr->cold_start_armed_m <= 0.0 && dmr->cold_start_hunt == 0,
                   "a mode change releases DMR's startup window while frames keep syncing");
            // Auto presumes P25 instead: its window opens at the switch, so the first probe is P25's, after it.
            expect(g_tunes >= 1 && g_probe_times_ms[0] > 8000U && p25->cold_start_hunt == 1,
                   "the switch to -fa hands the startup hunt to P25, a full window after the switch");
            break;
        case SC_AUTO_NOISE_PROBE:
            expect(g_tunes >= 1 && g_first_tune == ROW0_HZ && g_probe_times_ms[0] > 5000U,
                   "noise NXDN syncs and the hunt's GFSK do not stop Auto's P25 startup hunt");
            expect(state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_UNKNOWN, "nor name an owner");
            expect(g_probe_in_flight_seen == 1, "the frame-sync hunt hears the P25 probe in flight");
            break;
        case SC_AUTO_TYPE_D:
            expect(g_validated_sent && state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_NXDN,
                   "a confirmed Type-D SCCH names NXDN");
            expect(p25->cold_start_hunt == 0 && !p25_sm_cold_start_active(p25), "and ends the P25 startup hunt");
            expect(g_tunes == 1, "nothing hunts away from the Type-D site afterwards");
            break;
        case SC_AUTO_NXDN_TRAFFIC:
            expect(g_tunes >= 1 && g_probe_times_ms[0] > g_last_content_ms + 5000U
                       && g_probe_times_ms[0] <= g_last_content_ms + 5500U,
                   "validated NXDN traffic holds the P25 startup window, which runs out from its last frame");
            expect(state->trunk_recovery_protocol == DSD_TRUNK_RECOVERY_UNKNOWN, "NXDN traffic names no owner");
            break;
        case SC_PROBE_HOOK_RESTART: break;
        case SC_EDACS_SESSION:
            expect(dsd_edacs_session(opts, state), "a selected EDACS session");
            expect(g_tunes == 0 && g_p25_armed_seen == 0, "a selected EDACS session never starts the P25 startup hunt");
            break;
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_decode_clock_use_system();
    p25_sm_init_ctx(p25, NULL, NULL);
    dmr_sm_init_ctx(dmr, NULL, NULL);
    freeState(state);
    free(state);
    free(opts);
    return 0;
}

/* A radio stream restarted on another device (a config apply that changes the RTL device): the replaced device's tune
   requests go with it, as at an input switch, so its failure cannot gate the new device's frames. */
static void
test_source_change_resets_requests(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        g_failures++;
        return;
    }
    initOpts(opts);
    initState(state);
    dsd_trunk_tuning_requests_reset();
    const uint64_t failed = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_mark_ready(failed);
    dsd_trunk_tuning_request_publish(failed, DSD_TRUNK_TUNE_RESULT_FAILED);
    expect(dsd_trunk_tuning_pending_request() == failed, "the old device's failure gates frames");
    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_SOURCE, 0);
    expect(dsd_trunk_tuning_pending_request() == 0U, "a new radio source drops the old device's requests");
    freeState(state);
    free(state);
    free(opts);
}

/* The engine's frame-sync probe query, installed by the last run: a P25-driven probe is a hunting machine under a P25
   or unknown owner with trunking on; while the watchdog holds the guard the last answer stands; without trunking a
   machine left hunting is no probe, and the query takes no guard. Leaves the last answer at "probe". */
static void
test_probe_hook_answers(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        g_failures++;
        return;
    }
    g_scenario = SC_PROBE_HOOK_RESTART;
    initOpts(opts);
    initState(state);
    opts->trunk_enable = 1;
    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    p25_sm_init_ctx(p25, opts, state);
    expect(!dsd_frame_sync_hook_p25_probe_in_flight(opts, state), "an idle machine probes nothing");
    p25->state = P25_SM_HUNTING; // a lost P25 control channel, or the startup hunt between probes
    state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_P25;
    expect(dsd_frame_sync_hook_p25_probe_in_flight(opts, state), "recovery under a P25 owner probes");
    state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_UNKNOWN;
    expect(dsd_frame_sync_hook_p25_probe_in_flight(opts, state), "so does the startup hunt");
    state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_NXDN;
    expect(!dsd_frame_sync_hook_p25_probe_in_flight(opts, state), "another owner's session has no P25 probe");
    state->trunk_recovery_protocol = DSD_TRUNK_RECOVERY_P25;
    expect(dsd_frame_sync_hook_p25_probe_in_flight(opts, state) == 1 && probe_answer_with_guard_busy(opts, state) == 1,
           "while the watchdog holds the guard the last answer stands");
    opts->trunk_enable = 0; // trunking turned off: nothing ticks the machine it leaves in HUNTING
    expect(!dsd_frame_sync_hook_p25_probe_in_flight(opts, state), "without trunking no probe runs");
    opts->trunk_enable = 1;
    expect(dsd_frame_sync_hook_p25_probe_in_flight(opts, state) == 1, "the last answer is a probe");
    opts->trunk_enable = 0;
    expect(probe_answer_with_guard_busy(opts, state) == 0, "a session without trunking does not ask the machine");
    opts->trunk_enable = 1;
    expect(dsd_frame_sync_hook_p25_probe_in_flight(opts, state) == 1, "the last answer is a probe again");
    p25_sm_init_ctx(p25, NULL, NULL);
    freeState(state);
    free(state);
    free(opts);
}

int
main(void) {
    test_source_change_resets_requests();
    run_scenario(SC_OUTER_LOOP, 1);
    run_scenario(SC_SYNCED_LOOP, 1);
    run_scenario(SC_NAMED_BETWEEN_FRAMES, 1);
    run_scenario(SC_INPUT_SWITCH, 1);
    run_scenario(SC_MODE_CHANGE_DMR, 0);
    run_scenario(SC_INPUT_SWITCH_PENDING, 1);
    run_scenario(SC_NXDN_OUTER_LOOP, 2);
    run_scenario(SC_NXDN_SYNCED_LOOP, 2);
    run_scenario(SC_NXDN_LOST_CC, 3);
    run_scenario(SC_NXDN_CALL_RETURN, 2);
    run_scenario(SC_INPUT_SWITCH_NEW_REQUEST, 1);
    run_scenario(SC_CAPPLUS_LOST_REST, 0);
    run_scenario(SC_CAPPLUS_LOST_REST, 3);
    run_scenario(SC_CAPPLUS_PENDING_PROBE, 0);
    run_scenario(SC_CAPPLUS_LIVE_REST, 0);
    run_scenario(SC_DMR_NAMED_UNCONFIRMED, 0);
    run_scenario(SC_CAPPLUS_UNMAPPED_REST, 0);
    run_scenario(SC_RAS_SYNCED_APPLY, 0);
    run_scenario(SC_OUTER_LOOP, 4);
    run_scenario(SC_NAMED_BETWEEN_FRAMES, 4);
    run_scenario(SC_AUTO_NOISE_PROBE, 4);
    run_scenario(SC_NXDN_LOST_CC, 4);
    run_scenario(SC_AUTO_TYPE_D, 4);
    run_scenario(SC_AUTO_NXDN_TRAFFIC, 4);
    run_scenario(SC_EDACS_SESSION, 5);
    run_scenario(SC_EDACS_SESSION, 6);
    test_probe_hook_answers();
    run_scenario(SC_PROBE_HOOK_RESTART, 1);
    if (g_failures == 0) {
        printf("engine cold start hunt: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
