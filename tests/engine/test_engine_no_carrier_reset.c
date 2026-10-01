// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

#ifdef USE_RADIO
static int
fake_rtl_fsk_output_kind(void) {
    return RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
}
#endif

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
static int g_check_p25_tick_guard = 0;

static int
p25_tick_guard_is_held(void) {
    if (!p25_sm_tick_guard_try_enter()) {
        return 1;
    }
    p25_sm_tick_guard_leave();
    return 0;
}

#endif

#ifdef DSD_NEO_TEST_RTL_WRAP
#ifdef USE_RADIO
static int g_p25_tick_guard_held_during_tune = 0;
static int g_rtl_tune_calls = 0;
static uint32_t g_rtl_tune_freq = 0;
static int g_rtl_tune_result = RTL_STREAM_TUNE_OK;
static int g_rtl_output_rate = 48000;
static int g_rtl_cqpsk_enable = 0;
static int g_rtl_symbol_rate_hz = 6000;
static int g_rtl_symbol_levels = 4;
static int g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
static int g_rtl_ted_sps = 8;
static int g_rtl_ted_sps_override = 8;
static int g_rtl_fsk_reacquire_requests = 0;
static int g_pending_active = 0;
static uint32_t g_pending_target_freq_hz = 0;
static int g_pending_cqpsk = -1;
static int g_pending_symbol_rate_hz = 0;
static int g_pending_symbol_levels = 0;
static int g_pending_channel_profile = 0;
static int g_pending_ted_sps = 0;
static int g_pending_ted_override = 0;
/* The receive family a retune profile carries (issue #526): -1 none, else dsd_rx_family. */
static int g_pending_analog_family = -1;
static int g_pending_analog_kind = 0;
static int g_pending_analog_width_hz = 0;
/* Whether the digital family attached says the profile's CQPSK state is the target's own (issue #583). */
static int g_pending_cqpsk_explicit = 0;
static int g_refuse_analog_profile = 0;
static int g_analog_attach_calls = 0;
/* What the fake front end runs: the analog family (monitor output) or not, and its width. */
static int g_rtl_analog_family = 0;
static int g_rtl_analog_width_hz = 0;
static int g_request_demod_calls = 0;
/* The DSP rate the fake stream publishes for analog requests (0: none), and the widest analog width it was asked for. */
static int g_rtl_request_rate_hz = 0;
static int g_analog_attach_max_width_hz = 0;
/* How often the stream was asked for the AM monitor, and how often it tuned g_rtl_watch_freq (0: none watched). */
static int g_analog_attach_am_calls = 0;
static uint32_t g_rtl_watch_freq = 0;
static int g_rtl_watch_tunes = 0;
/* 1: the output rate follows the family, the analog monitor at 48 kHz and the digital family at 24 kHz (bw=24). */
static int g_rtl_family_rates = 0;
/* Issue #583: the output rate the fake front end lands the digital family at (0: as g_rtl_family_rates says), for a
 * front end whose live rate is not the one a switch to the digital family lands on. */
static int g_rtl_digital_landing_rate = 0;
/* Issue #583: the live receive-family requests the fake stream has accepted, the count a retune profile recorded when
 * its family was attached (a request made since supersedes that family, rtl_stream_retune_family_superseded()), and a
 * live analog request (a width edit) still waiting for the demod thread. */
static int g_live_family_requests = 0;
static int g_pending_family_requests = 0;
static int g_rtl_analog_request_queued = 0;
static int g_rtl_analog_request_width_hz = 0;
/* Issue #583: the cqpsk_explicit argument of the last digital-family output-rate query (-1: none since the reset),
 * asked by the tuning side straight from the stream, and by the decoder through the metrics hooks. */
static int g_stream_rate_explicit = -1;
static int g_hook_rate_explicit = -1;
/* The cqpsk_enable argument of the decoder's last digital-family output-rate query (-1: none since the reset). */
static int g_hook_rate_cqpsk = -1;
/* Issue #583: 1 makes the tune in flight complete FAILED the next time the tuning side reads the front end back, which
 * it does before it prepares a retune (dsd_engine_rtl_profile_snapshot_capture()): after the decoder has timed the
 * target that retune is for, and before the tuning side decides the family the retune carries. */
static int g_fail_in_flight_at_tune_snapshot = 0;
/* Issue #583: what the stream answered the decoder's last timing of a row, through the metrics hooks (-1: no timing
 * asked since the reset), and how often the tuning side then found work outstanding that lands a family where that
 * timing had not: a rise from 0 to 1 between a row's timing and a retune, which only such work the decoder thread
 * queued in between could make (dsd_engine_retune_lands_digital_family()). */
static int g_timing_after_pending = -1;
static int g_tuning_rises_after_timing = 0;

/* A retune profile as the fake stream queues it (the g_pending_* fields). */
typedef struct fake_retune_profile {
    int active;
    uint32_t target_freq_hz;
    int cqpsk;
    int symbol_rate_hz;
    int symbol_levels;
    int channel_profile;
    int ted_sps;
    int ted_override;
    int analog_family;
    int analog_kind;
    int analog_width_hz;
    int family_requests;
    int cqpsk_explicit;
} fake_retune_profile;

/* A tune the controller still owns (issue #583). */
typedef struct fake_retune {
    int active;
    uint32_t freq;
    uint64_t request_id;
    uint64_t coalesced_request_id; /* the queued tune this one replaced, which completes with it */
    fake_retune_profile profile;
} fake_retune;

/* 1: a tune that times out stays outstanding, as the controller still owns it: the first is the one it took, a later
 * one waits queued behind it and replaces one already queued (a queued retune coalesces with the next). They land in
 * that order before the next tune that completes, or when a case lands them (land_outstanding_tunes()). */
static int g_rtl_timeout_outstanding = 0;
static fake_retune g_retune_in_flight;
static fake_retune g_retune_queued;
/* The receive family attached to each tune's retune profile, recorded before the fake applies it (-1: none), with the
 * profile's CQPSK request (-2: no profile) and whether the family attached says it is the target's own (-1: none). */
#define FAKE_TUNE_LOG_MAX 16
static uint32_t g_tune_log_freq[FAKE_TUNE_LOG_MAX];
static int g_tune_log_family[FAKE_TUNE_LOG_MAX];
static int g_tune_log_cqpsk[FAKE_TUNE_LOG_MAX];
static int g_tune_log_explicit[FAKE_TUNE_LOG_MAX];
static int g_tune_log_count = 0;

static void
reset_rtl_profile_fakes(void) {
    g_rtl_tune_calls = 0;
    g_rtl_tune_freq = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_output_rate = 48000;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_ted_sps = 8;
    g_rtl_ted_sps_override = 8;
    g_rtl_fsk_reacquire_requests = 0;
    g_pending_active = 0;
    g_pending_target_freq_hz = 0;
    g_pending_cqpsk = -1;
    g_pending_symbol_rate_hz = 0;
    g_pending_symbol_levels = 0;
    g_pending_channel_profile = 0;
    g_pending_ted_sps = 0;
    g_pending_ted_override = 0;
    g_pending_analog_family = -1;
    g_pending_analog_kind = 0;
    g_pending_analog_width_hz = 0;
    g_pending_cqpsk_explicit = 0;
    g_refuse_analog_profile = 0;
    g_analog_attach_calls = 0;
    g_rtl_analog_family = 0;
    g_rtl_analog_width_hz = 0;
    g_request_demod_calls = 0;
    g_rtl_request_rate_hz = 0;
    g_analog_attach_max_width_hz = 0;
    g_analog_attach_am_calls = 0;
    g_rtl_watch_freq = 0;
    g_rtl_watch_tunes = 0;
    g_rtl_family_rates = 0;
    g_live_family_requests = 0;
    g_pending_family_requests = 0;
    g_rtl_analog_request_queued = 0;
    g_rtl_analog_request_width_hz = 0;
    g_stream_rate_explicit = -1;
    g_hook_rate_explicit = -1;
    g_hook_rate_cqpsk = -1;
    g_rtl_digital_landing_rate = 0;
    g_fail_in_flight_at_tune_snapshot = 0;
    g_timing_after_pending = -1;
    g_tuning_rises_after_timing = 0;
    g_rtl_timeout_outstanding = 0;
    g_retune_in_flight = (fake_retune){0};
    g_retune_queued = (fake_retune){0};
    g_tune_log_count = 0;
    g_check_p25_tick_guard = 0;
    g_p25_tick_guard_held_during_tune = 0;
}
#endif

// Rigctl needs a success path here, not just the socket-failure one the rest of the file uses:
// the interesting cases are a hop whose rigctl leg lands and whose RTL leg then does not, and a
// rigctl hop on PCM input, which radio-off builds have too. Failing by default is what the real
// call does on the invalid socket every other case configures.
static int g_rigctl_setfreq_ok = 0;
static int g_rigctl_setfreq_calls = 0;
static long int g_rigctl_setfreq_freq = 0;
// The -B requests the legacy rigctl leg makes (issue #589); the peer takes each one.
static int g_rigctl_setmod_calls = 0;
static int g_rigctl_setmod_bw = 0;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#ifdef USE_RADIO
uint32_t
__wrap_rtl_stream_output_rate(const RtlSdrContext* ctx) {
    (void)ctx;
    if (g_rtl_family_rates) {
        return g_rtl_analog_family ? 48000U : 24000U;
    }
    return (uint32_t)g_rtl_output_rate;
}

/* The output rate the fake front end runs @p family at. */
static unsigned int
fake_output_rate_for_family(int family) {
    if (family == DSD_RX_FAMILY_DIGITAL && g_rtl_digital_landing_rate > 0) {
        return (unsigned int)g_rtl_digital_landing_rate;
    }
    if (g_rtl_family_rates) {
        return family == DSD_RX_FAMILY_ANALOG ? 48000U : 24000U;
    }
    return (uint32_t)g_rtl_output_rate;
}

unsigned int
__wrap_rtl_stream_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz, int cqpsk_explicit) {
    (void)cqpsk_enable;
    (void)symbol_rate_hz;
    if (family == DSD_RX_FAMILY_DIGITAL) {
        g_stream_rate_explicit = cqpsk_explicit;
    }
    return fake_output_rate_for_family(family);
}

int
__wrap_rtl_stream_get_cqpsk_status(int* cqpsk_enable, int* cqpsk_timing_active) {
    if (cqpsk_enable) {
        *cqpsk_enable = g_rtl_cqpsk_enable;
    }
    if (cqpsk_timing_active) {
        *cqpsk_timing_active = g_rtl_cqpsk_enable ? 1 : 0;
    }
    return 0;
}

int
__wrap_rtl_stream_get_symbol_profile_full(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = g_rtl_symbol_rate_hz;
    }
    if (out_levels) {
        *out_levels = g_rtl_symbol_levels;
    }
    if (out_channel_profile) {
        *out_channel_profile = g_rtl_channel_profile;
    }
    return 0;
}

int
__wrap_rtl_stream_get_ted_sps(void) {
    return g_rtl_ted_sps;
}

static void land_outstanding_tune(fake_retune* r, dsd_trunk_tune_result result);

int
__wrap_rtl_stream_get_ted_sps_override(void) {
    if (g_fail_in_flight_at_tune_snapshot) {
        g_fail_in_flight_at_tune_snapshot = 0;
        land_outstanding_tune(&g_retune_in_flight, DSD_TRUNK_TUNE_RESULT_FAILED);
    }
    return g_rtl_ted_sps_override;
}

void
__wrap_rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t target_freq_hz, int cqpsk_enable,
                                                              int symbol_rate_hz, int levels, int channel_profile,
                                                              int ted_sps, int persist_ted_override,
                                                              const rtl_stream_retune_gain_profile* gain_profile) {
    (void)gain_profile;
    g_pending_active = 1;
    g_pending_target_freq_hz = target_freq_hz;
    g_pending_cqpsk = cqpsk_enable;
    g_pending_symbol_rate_hz = symbol_rate_hz;
    g_pending_symbol_levels = levels;
    g_pending_channel_profile = channel_profile;
    g_pending_ted_sps = ted_sps;
    g_pending_ted_override = persist_ted_override ? 1 : 0;
    g_pending_cqpsk_explicit = 0;
}

int
__wrap_rtl_stream_prepare_retune_analog_profile_for_target(uint32_t target_freq_hz,
                                                           const rtl_stream_retune_analog_profile* analog) {
    g_analog_attach_calls++;
    if (analog && analog->family == DSD_RX_FAMILY_ANALOG && analog->width_hz > g_analog_attach_max_width_hz) {
        g_analog_attach_max_width_hz = analog->width_hz;
    }
    if (analog && analog->family == DSD_RX_FAMILY_ANALOG && analog->kind == DSD_ANALOG_DEMOD_AM) {
        g_analog_attach_am_calls++;
    }
    if (!analog || (analog->family == DSD_RX_FAMILY_ANALOG && g_refuse_analog_profile)) {
        return -1;
    }
    if (!g_pending_active || g_pending_target_freq_hz != target_freq_hz) {
        g_pending_active = 1;
        g_pending_target_freq_hz = target_freq_hz;
        g_pending_cqpsk = -1;
        g_pending_symbol_rate_hz = 0;
        g_pending_ted_sps = 0;
    }
    g_pending_analog_family = analog->family;
    g_pending_analog_kind = analog->kind;
    g_pending_analog_width_hz = analog->width_hz;
    g_pending_cqpsk_explicit = (analog->family == DSD_RX_FAMILY_DIGITAL && analog->cqpsk_explicit) ? 1 : 0;
    g_pending_family_requests = g_live_family_requests;
    return 0;
}

int
__wrap_rtl_stream_analog_family_active(void) {
    return g_rtl_analog_family;
}

/* Whether the outstanding tune @p r lands the analog family: it carries it for the target it tunes, and no live family
 * request superseded it. */
static int
fake_retune_lands_analog(const fake_retune* r) {
    return r->active && r->profile.active && (r->profile.target_freq_hz == 0U || r->profile.target_freq_hz == r->freq)
           && r->profile.analog_family == DSD_RX_FAMILY_ANALOG && r->profile.family_requests == g_live_family_requests;
}

/* Whether the outstanding tune @p r lands a receive family, either one: it carries it for the target it tunes, and no
 * live family request superseded it. The digital family lands where the stream predicts its landing even on a front
 * end already digital (issue #583). */
static int
fake_retune_lands_family(const fake_retune* r) {
    return r->active && r->profile.active && (r->profile.target_freq_hz == 0U || r->profile.target_freq_hz == r->freq)
           && r->profile.analog_family >= 0 && r->profile.family_requests == g_live_family_requests;
}

/* The stream's union (issue #583): the analog family published, or a family landed by a tune the controller still
 * owns, or the analog family asked for by a live request still queued. With nothing outstanding it answers as the live
 * family does. */
static int
fake_family_landing_after_pending(void) {
    const int outstanding = fake_retune_lands_family(&g_retune_in_flight) || fake_retune_lands_family(&g_retune_queued)
                            || g_rtl_analog_request_queued;
    return (g_rtl_analog_family || outstanding) ? 1 : 0;
}

/* The tuning side's read (trunk_tuning.c asks the stream itself), which notes a rise since the decoder's last timing. */
int
__wrap_rtl_stream_family_landing_after_pending(void) {
    const int answer = fake_family_landing_after_pending();
    if (answer && g_timing_after_pending == 0) {
        g_tuning_rises_after_timing++;
    }
    return answer;
}

int
__wrap_rtl_stream_get_request_rate_hz(void) {
    return g_rtl_request_rate_hz;
}

int
__wrap_rtl_stream_get_analog_profile(int* out_kind, int* out_width_hz, int* out_lpf_on) {
    if (out_kind) {
        *out_kind = DSD_ANALOG_DEMOD_FM;
    }
    if (out_width_hz) {
        *out_width_hz = g_rtl_analog_family ? g_rtl_analog_width_hz : 0;
    }
    if (out_lpf_on) {
        *out_lpf_on = g_rtl_analog_family;
    }
    return g_rtl_analog_family;
}

int
__wrap_rtl_stream_request_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile,
                                        int ted_sps, int ted_sps_is_override) {
    (void)cqpsk_enable;
    (void)symbol_rate_hz;
    (void)levels;
    (void)channel_profile;
    (void)ted_sps;
    (void)ted_sps_is_override;
    g_request_demod_calls++;
    return 0;
}

void
__wrap_rtl_stream_clear_pending_retune_profile(void) {
    g_pending_analog_family = -1;
    g_pending_cqpsk_explicit = 0;
    g_pending_active = 0;
    g_pending_target_freq_hz = 0;
    g_pending_cqpsk = -1;
    g_pending_symbol_rate_hz = 0;
    g_pending_symbol_levels = 0;
    g_pending_channel_profile = 0;
    g_pending_ted_sps = 0;
    g_pending_ted_override = 0;
}

static void
apply_pending_profile(uint32_t target_freq_hz) {
    if (!g_pending_active) {
        return;
    }
    if (g_pending_target_freq_hz != 0 && g_pending_target_freq_hz != target_freq_hz) {
        return;
    }
    /* The family switch lands before the symbol profile, and the analog family takes none of it. */
    const int family = g_pending_analog_family;
    g_pending_analog_family = -1;
    if (family >= 0 && g_pending_family_requests != g_live_family_requests) {
        /* A live family request made since the family was attached supersedes it: the retune lands neither the family
           nor the symbol profile queued with it. */
        g_pending_active = 0;
        g_pending_target_freq_hz = 0;
        return;
    }
    if (family >= 0) {
        /* A family that lands retires the live request still queued from before it was attached. */
        g_rtl_analog_request_queued = 0;
    }
    if (family == DSD_RX_FAMILY_ANALOG) {
        g_rtl_analog_family = 1;
        g_rtl_analog_width_hz = g_pending_analog_width_hz;
        g_pending_active = 0;
        g_pending_target_freq_hz = 0;
        return;
    }
    if (family == DSD_RX_FAMILY_DIGITAL) {
        g_rtl_analog_family = 0;
    }
    if (g_pending_cqpsk >= 0) {
        g_rtl_cqpsk_enable = g_pending_cqpsk ? 1 : 0;
    }
    if (g_pending_symbol_rate_hz > 0) {
        g_rtl_symbol_rate_hz = g_pending_symbol_rate_hz;
        g_rtl_symbol_levels = g_pending_symbol_levels;
        g_rtl_channel_profile = g_pending_channel_profile;
    }
    if (g_pending_ted_sps > 0) {
        g_rtl_ted_sps = g_pending_ted_sps;
        g_rtl_ted_sps_override = g_pending_ted_override ? g_pending_ted_sps : 0;
    }
    g_pending_active = 0;
    g_pending_target_freq_hz = 0;
}

/* The profile the fake stream has queued, which the fake controller takes (the queue is left empty). */
static fake_retune_profile
fake_take_pending_profile(void) {
    const fake_retune_profile profile = {
        g_pending_active,         g_pending_target_freq_hz,  g_pending_cqpsk,           g_pending_symbol_rate_hz,
        g_pending_symbol_levels,  g_pending_channel_profile, g_pending_ted_sps,         g_pending_ted_override,
        g_pending_analog_family,  g_pending_analog_kind,     g_pending_analog_width_hz, g_pending_family_requests,
        g_pending_cqpsk_explicit,
    };
    __wrap_rtl_stream_clear_pending_retune_profile();
    return profile;
}

static void
fake_put_pending_profile(const fake_retune_profile* profile) {
    g_pending_active = profile->active;
    g_pending_target_freq_hz = profile->target_freq_hz;
    g_pending_cqpsk = profile->cqpsk;
    g_pending_symbol_rate_hz = profile->symbol_rate_hz;
    g_pending_symbol_levels = profile->symbol_levels;
    g_pending_channel_profile = profile->channel_profile;
    g_pending_ted_sps = profile->ted_sps;
    g_pending_ted_override = profile->ted_override;
    g_pending_analog_family = profile->analog_family;
    g_pending_analog_kind = profile->analog_kind;
    g_pending_analog_width_hz = profile->analog_width_hz;
    g_pending_family_requests = profile->family_requests;
    g_pending_cqpsk_explicit = profile->cqpsk_explicit;
}

/* A tune that timed out: the controller took its profile and still owns it (g_rtl_timeout_outstanding). */
static void
hold_outstanding_tune(uint32_t freq, uint64_t request_id) {
    fake_retune* slot = g_retune_in_flight.active ? &g_retune_queued : &g_retune_in_flight;
    const uint64_t replaced = slot->active ? slot->request_id : 0U;
    *slot = (fake_retune){.active = 1, .freq = freq, .request_id = request_id, .coalesced_request_id = replaced};
    slot->profile = fake_take_pending_profile();
}

/* The controller lands @p r and completes its request with @p result: OK applies its profile to the fake front end,
 * FAILED applies none of it. */
static void
land_outstanding_tune(fake_retune* r, dsd_trunk_tune_result result) {
    if (!r->active) {
        return;
    }
    if (result == DSD_TRUNK_TUNE_RESULT_OK) {
        const fake_retune_profile waiting = fake_take_pending_profile();
        fake_put_pending_profile(&r->profile);
        apply_pending_profile(r->freq);
        fake_put_pending_profile(&waiting);
    }
    if (r->coalesced_request_id != 0U) {
        dsd_trunk_tuning_request_publish(r->coalesced_request_id, result);
    }
    if (r->request_id != 0U) {
        dsd_trunk_tuning_request_publish(r->request_id, result);
    }
    *r = (fake_retune){0};
}

static void
land_outstanding_tunes(dsd_trunk_tune_result result) {
    land_outstanding_tune(&g_retune_in_flight, result);
    land_outstanding_tune(&g_retune_queued, result);
}

/* Record the family attached to the profile a tune of @p freq runs with (-1: none, or no profile for it). */
static void
note_tune_family(uint32_t freq) {
    if (g_tune_log_count >= FAKE_TUNE_LOG_MAX) {
        return;
    }
    const int bound = g_pending_active && (g_pending_target_freq_hz == 0U || g_pending_target_freq_hz == freq);
    g_tune_log_freq[g_tune_log_count] = freq;
    g_tune_log_family[g_tune_log_count] = bound ? g_pending_analog_family : -1;
    g_tune_log_cqpsk[g_tune_log_count] = bound ? g_pending_cqpsk : -2;
    g_tune_log_explicit[g_tune_log_count] =
        (bound && g_pending_analog_family == DSD_RX_FAMILY_DIGITAL) ? g_pending_cqpsk_explicit : -1;
    g_tune_log_count++;
}

/* The last tune of @p freq in the tune log (note_tune_family()), or -1 when it was never tuned. */
static int
tune_log_index_for(uint32_t freq) {
    for (int i = g_tune_log_count - 1; i >= 0; i--) {
        if (g_tune_log_freq[i] == freq) {
            return i;
        }
    }
    return -1;
}

/* The family attached to the last tune of @p freq, or -2 when it was never tuned. */
static int
tune_family_for(uint32_t freq) {
    const int i = tune_log_index_for(freq);
    return i >= 0 ? g_tune_log_family[i] : -2;
}

/* The CQPSK request of the profile the last tune of @p freq ran with, or -3 when it was never tuned. */
static int
tune_cqpsk_for(uint32_t freq) {
    const int i = tune_log_index_for(freq);
    return i >= 0 ? g_tune_log_cqpsk[i] : -3;
}

/* Whether the digital family attached to the last tune of @p freq said its CQPSK state is the target's own (-1: no
 * digital family attached), or -2 when it was never tuned. */
static int
tune_explicit_for(uint32_t freq) {
    const int i = tune_log_index_for(freq);
    return i >= 0 ? g_tune_log_explicit[i] : -2;
}

static int
fake_rtl_tune(uint32_t center_freq_hz, uint64_t request_id) {
    if (g_check_p25_tick_guard) {
        g_p25_tick_guard_held_during_tune = p25_tick_guard_is_held();
    }
    g_rtl_tune_calls++;
    g_rtl_tune_freq = center_freq_hz;
    if (g_rtl_watch_freq != 0U && center_freq_hz == g_rtl_watch_freq) {
        g_rtl_watch_tunes++;
    }
    note_tune_family(center_freq_hz);
    if (g_rtl_timeout_outstanding && g_rtl_tune_result == RTL_STREAM_TUNE_TIMEOUT) {
        hold_outstanding_tune(center_freq_hz, request_id);
        return g_rtl_tune_result;
    }
    if (g_rtl_tune_result == RTL_STREAM_TUNE_OK) {
        /* The controller lands the tunes it still owns first, in order. */
        land_outstanding_tunes(DSD_TRUNK_TUNE_RESULT_OK);
        apply_pending_profile(center_freq_hz);
    }
    return g_rtl_tune_result;
}

int
__wrap_rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    return fake_rtl_tune(center_freq_hz, 0U);
}

int
__wrap_rtl_stream_tune_tagged(RtlSdrContext* ctx, uint32_t center_freq_hz, uint64_t request_id) {
    (void)ctx;
    return fake_rtl_tune(center_freq_hz, request_id);
}

/* A width edit on the analog target while its retune is outstanding, as a command makes it: a live analog request the
 * demod thread has not taken yet, newer than any family attached so far. */
static void
fake_live_width_request(int width_hz) {
    g_live_family_requests++;
    g_rtl_analog_request_queued = 1;
    g_rtl_analog_request_width_hz = width_hz;
}

/* The demod thread's next block boundary: it takes the live request still queued. */
static void
fake_demod_boundary(void) {
    if (g_rtl_analog_request_queued) {
        g_rtl_analog_family = 1;
        g_rtl_analog_width_hz = g_rtl_analog_request_width_hz;
        g_rtl_analog_request_queued = 0;
    }
}

int
__wrap_rtl_stream_request_fsk_reacquire(void) {
    g_rtl_fsk_reacquire_requests++;
    return 1;
}
#endif

bool
__wrap_SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    g_rigctl_setfreq_calls++;
    g_rigctl_setfreq_freq = freq;
    return g_rigctl_setfreq_ok ? true : false;
}

bool
__wrap_SetModulation(dsd_socket_t sockfd, int bandwidth) {
    (void)sockfd;
    g_rigctl_setmod_calls++;
    g_rigctl_setmod_bw = bandwidth;
    return true;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

static int
init_test_runtime(dsd_opts** opts_out, dsd_state** state_out) {
    // dsd_state is multi-megabyte; keep it off the function stack.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }

    initOpts(opts);
    initState(state);

    *opts_out = opts;
    *state_out = state;
    return 0;
}

static void
free_test_runtime(dsd_opts* opts, dsd_state* state) {
    if (state != NULL) {
        freeState(state);
    }
    free(state);
    free(opts);
}

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
static int
test_dmr_explicit_return_destination(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    opts->trunk_enable = opts->use_rigctl = 1;
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;
    opts->setmod_bw = 0;
    state->trunk_cc_freq = 451000000L;
    state->p25_cc_freq = 450000000L;
    dmr_sm_ctx_t ctx;
    dmr_sm_init_ctx(&ctx, opts, state);
    dsd_trunk_tuning_requests_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.return_to_cc_request = dsd_engine_return_to_cc_request});
    g_rigctl_setfreq_ok = 0;
    int rc = expect_true("dmr-return-failure",
                         dmr_sm_return_to_cc(&ctx, opts, state, 452000000L) == DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect_true("dmr-failed-return-keeps-aliases",
                      state->trunk_cc_freq == 451000000L && state->p25_cc_freq == 450000000L);
    g_rigctl_setfreq_ok = 1;
    rc |= expect_true("dmr-return-success",
                      dmr_sm_return_to_cc(&ctx, opts, state, 452000000L) == DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("dmr-return-actual-frequency",
                      g_rigctl_setfreq_freq == 452000000L && state->trunk_cc_freq == 452000000L
                          && ctx.cc_probe_freq_hz == 452000000L && ctx.cc_rx_freq_hz == 452000000L && ctx.cc_acquiring);
    rc |= expect_true("dmr-return-clears-obsolete-alias", state->p25_cc_freq == 0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}

static int
test_trunk_cache_with_mode_metadata(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    dsd_trunk_tuning_requests_reset();
    opts->use_rigctl = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->trunk_enable = 1;
    opts->setmod_bw = 0;
    g_rigctl_setfreq_ok = 1;
    g_rigctl_setfreq_calls = 0;
    rc |= expect_true("trunk cache mode metadata", dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_DMR) == 0);
    for (int i = 0; i < 2; i++) {
        opts->trunk_is_tuned = 1;
        state->trunk_cc_freq = 941012500;
        state->p25_cc_freq = 0;
        state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
        state->last_vc_sync_time = 0;
        noCarrier(opts, state);
    }
    rc |= expect_true("mode metadata preserves redundant return suppression", g_rigctl_setfreq_calls == 1);
    /* Issue #589: a rigctl reconnect ends the suppression; the new connection's peer was sent nothing. */
    dsd_engine_rigctl_tune_cache_forget();
    opts->trunk_is_tuned = 1;
    state->trunk_cc_freq = 941012500;
    state->p25_cc_freq = 0;
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->last_vc_sync_time = 0;
    noCarrier(opts, state);
    rc |= expect_true("return after a rigctl reconnect is sent again",
                      g_rigctl_setfreq_calls == 2 && g_rigctl_setfreq_freq == 941012500);
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}

static int
test_typed_scan_tune_boundaries(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = state->trunk_lcn_freq[1] = 941012500;
    rc |= expect_true("typed mode metadata", dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_NXDN48) == 0);
    rc |= expect_true("typed P25 metadata", dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_P25) == 0);
    state->last_cc_sync_time = time(NULL);
    noCarrier(opts, state);
    rc |= expect_true("typed startup waits for scheduled entry", g_rtl_tune_calls == 0 && !opts->frame_nxdn48);
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("typed automatic mode and profile commit",
                      opts->frame_nxdn48 && state->lcn_freq_roll == 1 && g_rtl_symbol_rate_hz == 2400
                          && g_rtl_symbol_levels == 4 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_6K25);
    state->lcn_scan_hold = 1;
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("typed hold prevents automatic entry", g_rtl_tune_calls == 1 && state->lcn_freq_roll == 1);
    state->lcn_scan_hold = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    const dsd_call_observation call = {.protocol = DSD_SYNC_NXDN_POS,
                                       .kind = DSD_CALL_KIND_GROUP_VOICE,
                                       .ota_target_id = 1201,
                                       .observed_m = dsd_decode_now_mono_s()};
    rc |= expect_true("seed outgoing typed call", dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    noCarrier(opts, state);
    const uint64_t pending = dsd_trunk_tuning_pending_request();
    rc |= expect_true("typed timeout leaves outgoing mode", pending != 0 && opts->frame_nxdn48
                                                                && state->lcn_freq_roll == 1
                                                                && dsd_engine_channel_scan_pending(opts, state));
    dsd_call_snapshot pending_call;
    rc |= expect_true("typed pending hop keeps outgoing call", dsd_call_state_get(state, 0, &pending_call) == 1);
    rc |= expect_true("typed pending hop avoids premature sync loss", pending_call.phase == DSD_CALL_PHASE_ACTIVE);
    apply_pending_profile(941012500);
    dsd_trunk_tuning_request_publish(pending, DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("typed async completion commits",
                      !dsd_engine_channel_scan_pending(opts, state) && opts->frame_p25p1 && opts->frame_p25p2
                          && !opts->frame_dmr && state->lcn_freq_roll == 2 && g_rtl_symbol_rate_hz == 4800);
    dsd_call_snapshot ended;
    rc |= expect_true("typed pending hop retains call", dsd_call_state_get(state, 0, &ended) == 1);
    rc |= expect_true("typed pending hop ends explicitly", ended.end_reason == DSD_CALL_END_EXPLICIT);
    opts->use_rigctl = 1;
    opts->setmod_bw = 0;
    g_rigctl_setfreq_ok = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("typed dual-backend partial failure leaves row", state->lcn_freq_roll == 1 && opts->frame_p25p1);
    rc |= expect_true("typed partial failure closes frame gate",
                      !dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1));
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);
    rc |= expect_true("typed recovery skips failed row", opts->frame_p25p1 && state->lcn_freq_roll == 2);
    rc |= expect_true("typed recovery reopens frame gate",
                      dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1));
    g_rigctl_setfreq_ok = 0;
    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

/* Issue #526: a typed -Y map mixing nfm and digital rows, through the real noCarrier() step and
 * the real scanner tune. Each nfm row queues the analog monitor at its own width (the configured
 * one when the row sets none) with no symbol profile; the digital row after it puts the front end
 * back on the digital family, but only because the configured mode is digital. A width the front
 * end refuses skips the row without tuning, and a failed hop off an nfm row never applies a symbol
 * profile over the monitor that is still running. */
static int
test_typed_scan_nfm_rows_switch_family(int configured_analog) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    if (configured_analog) {
        rc |= expect_true("analog session",
                          dsd_apply_decode_mode_preset(DSDCFG_MODE_ANALOG, DSD_DECODE_PRESET_PROFILE_CLI, opts, state)
                              == 0);
        g_rtl_analog_family = 1;
    }
    opts->analog_nfm_bandwidth_hz = 20000;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->lcn_freq_count = 4;
    const long freqs[] = {461000000L, 154430000L, 155475000L, 461100000L};
    for (int i = 0; i < 4; i++) {
        state->trunk_lcn_freq[i] = freqs[i] + (configured_analog ? 5000L : 0L);
    }
    rc |= expect_true("dmr row", dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_DMR) == 0);
    rc |= expect_true("nfm row with a width", dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_NFM) == 0);
    rc |= expect_true("nfm row", dsd_channel_mode_set(state, 2, DSD_SCAN_MODE_NFM) == 0);
    rc |= expect_true("second dmr row", dsd_channel_mode_set(state, 3, DSD_SCAN_MODE_DMR) == 0);
    dsd_scan_row_profile* profile = NULL;
    rc |= expect_true("row profile", dsd_scan_profile_ensure(&profile) == 0 && profile);
    if (profile) {
        profile->values.present = DSD_SCAN_OPT_BANDWIDTH;
        profile->values.channel_bw_hz = 12500;
        rc |= expect_true("row width", dsd_channel_profile_set(state, 1, profile) == 0);
    }

    /* Row 0, DMR: a digital session's front end is digital already, so nothing is attached. On an
       analog session the typed digital row keeps the monitor family. */
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("dmr row committed", state->lcn_freq_roll == 1 && opts->frame_dmr && !opts->analog_only);
    rc |= expect_true("dmr row attaches no family", g_analog_attach_calls == 0);
    rc |=
        expect_true("dmr row symbol profile", g_rtl_symbol_rate_hz == 4800 && g_rtl_analog_family == configured_analog);

    /* Row 1, NFM with --nfm-bandwidth-hz 12500. */
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("nfm row committed", state->lcn_freq_roll == 2 && opts->analog_only == 1 && !opts->frame_dmr
                                               && opts->analog_nfm_bandwidth_hz == 12500);
    rc |= expect_true("nfm row runs the monitor at its width",
                      g_rtl_analog_family == 1 && g_rtl_analog_width_hz == 12500
                          && g_rtl_tune_freq == (uint32_t)state->trunk_lcn_freq[1]);

    /* Row 2, NFM without a width: the configured one. */
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("second nfm row", state->lcn_freq_roll == 3 && opts->analog_nfm_bandwidth_hz == 20000
                                            && g_rtl_analog_family == 1 && g_rtl_analog_width_hz == 20000);

    /* A failed hop off the nfm row leaves the monitor alone. */
    const int requests_before = g_request_demod_calls;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("failed hop off an nfm row applies no symbol profile",
                      g_request_demod_calls == requests_before && g_rtl_analog_family == 1 && !g_pending_active);
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;

    /* Row 3, DMR after NFM: back to the digital family only on a digital session. */
    state->lcn_freq_roll = 3;
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("digital row after nfm", state->lcn_freq_roll == 4 && opts->frame_dmr && !opts->analog_only
                                                   && opts->analog_nfm_bandwidth_hz == 20000);
    rc |= expect_true("digital row family", g_rtl_analog_family == configured_analog && g_rtl_symbol_rate_hz == 4800);

    /* A width the front end refuses: the row is skipped without a tune. Only the retune profile queued for it is
       dropped: the profile the front end runs is not requested again, which on the digital family would re-apply the
       DMR row's symbol profile at every rotation past the refused row. */
    g_refuse_analog_profile = 1;
    const int tunes_before = g_rtl_tune_calls;
    const int demod_requests_before = g_request_demod_calls;
    state->lcn_freq_roll = 1;
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("refused nfm row is not tuned", g_rtl_tune_calls == tunes_before && state->lcn_freq_roll == 2
                                                          && opts->frame_dmr && !opts->analog_only);
    rc |= expect_true("refused nfm row requests no demod profile",
                      g_request_demod_calls == demod_requests_before && !g_pending_active);
    g_refuse_analog_profile = 0;

    /* A width the published DSP rate cannot fit is refused before the stream is asked, the same way: the nfm row
       without a width of its own runs the configured 20 kHz, which a 16 kHz rate cannot filter. */
    g_rtl_request_rate_hz = 16000;
    state->lcn_freq_roll = 2;
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("rate-refused nfm row is not tuned",
                      g_rtl_tune_calls == tunes_before && state->lcn_freq_roll == 3 && opts->frame_dmr);
    rc |= expect_true("rate-refused nfm row requests no demod profile",
                      g_request_demod_calls == demod_requests_before && !g_pending_active);
    g_rtl_request_rate_hz = 0;

    dsd_engine_channel_scan_leave(opts, state);
    rc |= expect_true("leave restores the configured session",
                      opts->analog_only == configured_analog && opts->analog_nfm_bandwidth_hz == 20000);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    if (rc) {
        DSD_FPRINTF(stderr, "typed nfm rows on a%s session failed\n", configured_analog ? "n analog" : " digital");
    }
    return rc;
}

/* Issue #526: --trunk-scan through the real coordinator and tuning. An nfm-conventional target
 * runs the analog monitor; the trunked P25 target after it re-parks through the control-channel
 * tune, which must put the front end back on the digital family before its symbol profile, or the
 * P25 profile would land on the monitor output and decode nothing. */
static int
test_trunk_scan_nfm_target_then_trunked_target(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "nfm-trunk-scan-");
    if (fd < 0) {
        free_test_runtime(opts, state);
        return 1;
    }
    dsd_close(fd);
    FILE* fp = dsd_fopen_private(path, "w");
    int rc = expect_true("targets file", fp != NULL);
    if (fp) {
        DSD_FPRINTF(fp, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                        "fire,nfm-conventional,154430000,,250,250,,--nfm-bandwidth-hz 12500\n"
                        "county,p25-trunk,851012500,,250,,,\n");
        (void)fclose(fp);
    }
    reset_rtl_profile_fakes();
    g_rtl_symbol_rate_hz = 4800;
    g_rtl_cqpsk_enable = 0;
    dsd_trunk_tuning_requests_reset();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = dsd_engine_trunk_tune_to_freq_request,
                                                        .tune_to_cc_request = dsd_engine_trunk_tune_to_cc_request,
                                                        .return_to_cc_request = dsd_engine_return_to_cc_request});
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", path);
    char err[256] = {0};
    rc |= expect_true("nfm trunk scan init", dsd_engine_trunk_scan_init(opts, state, err, sizeof err) == 0);
    rc |= expect_true("nfm target runs the monitor at its width", opts->analog_only == 1 && g_rtl_analog_family == 1
                                                                      && g_rtl_analog_width_hz == 12500
                                                                      && g_rtl_tune_freq == 154430000U);
    rc |= expect_true("advance to the trunked target",
                      dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
    rc |= expect_true("trunked target leaves the monitor for the digital family",
                      opts->analog_only == 0 && g_rtl_analog_family == 0 && g_rtl_tune_freq == 851012500U
                          && g_rtl_symbol_rate_hz == 4800);
    dsd_engine_trunk_scan_shutdown(opts, state);
    rc |= expect_true("shutdown restores the digital session", opts->analog_only == 0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_trunk_tuning_requests_reset();
    state->rtl_ctx = NULL;
    (void)remove(path);
    free_test_runtime(opts, state);
    return rc;
}

// The live-scanner loop runs dsd_engine_scan_visit_tick() immediately before every noCarrier()
// pass, so by the time the step predicate runs the row on air has always been reconciled with the
// anchor. These cases drive noCarrier() in isolation, so they stand in for that tick.
static void
seed_visit_anchor(dsd_state* state, double anchor_m) {
    dsd_scan_voice_gate_note_retune(state, anchor_m);
    state->scan_visit_roll_seen = state->lcn_freq_roll;
}

/*
 * The -Y per-visit cap (issue #507) through the real noCarrier() step. Every case here needs the
 * cap to be the only thing that could hop: the legacy -t 10 deadline is fresh, or the voice gate
 * is holding on media that keeps arriving. Frequencies are unique to this case because engine.c
 * caches its last tune in file statics that outlive free_test_runtime().
 */
static int
test_visit_cap_scanner_hops(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_hangtime = 10;
    opts->scan_max_visit_ms = 2000;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 952012500;
    state->trunk_lcn_freq[1] = 953012500;
    state->trunk_lcn_freq[2] = 954012500;
    state->lcn_freq_count = 3;
    state->lcn_freq_roll = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;

    // Legacy hangtime mode: the -t 10 deadline is 10 s away, so nothing but the cap can move the
    // rotation. The visit is 5 s old against a 2 s cap, and the call open on the outgoing row has
    // to close as an explicit release rather than a sync loss -- the frequency moved under it.
    double now_m = dsd_decode_now_mono_s();
    state->last_cc_sync_time = time(NULL);
    seed_visit_anchor(state, now_m - 5.0);
    dsd_call_observation capped_call = {0};
    capped_call.protocol = DSD_SYNC_NXDN_POS;
    capped_call.slot = 0U;
    capped_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    capped_call.ota_target_id = 7301U;
    capped_call.policy_target_id = 7301U;
    capped_call.ota_source_id = 8301U;
    capped_call.observed_m = now_m - 4.0;
    rc |= expect_true("visit-cap-legacy-seeds-call",
                      dsd_call_state_observe(state, &capped_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("visit-cap-legacy-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 952012500U);
    rc |= expect_true("visit-cap-legacy-advanced", state->lcn_freq_roll == 1);
    dsd_call_snapshot capped_snapshot;
    rc |= expect_true("visit-cap-legacy-retains-snapshot", dsd_call_state_get(state, 0U, &capped_snapshot) == 1);
    rc |= expect_true("visit-cap-legacy-ends-call", capped_snapshot.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("visit-cap-legacy-ends-call-explicitly",
                      capped_snapshot.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);
    // The hop anchors a fresh visit, so an immediate second pass has nothing to expire. The row it
    // moved to is reconciled first, so this tests the anchor and not the row change.
    state->scan_visit_roll_seen = state->lcn_freq_roll;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("visit-cap-fresh-anchor-no-second-hop", g_rtl_tune_calls == 0 && state->lcn_freq_roll == 1);

    // Voice-gate mode with media still arriving: the gate holds the row for as long as voice keeps
    // coming, which is exactly the open-microphone case the cap exists for.
    opts->scan_voice_only = 1;
    opts->scan_voice_qualify_ms = 1000;
    opts->scan_voice_hold_ms = 2000;
    state->lcn_freq_roll = 1;
    state->last_cc_sync_time = time(NULL);
    now_m = dsd_decode_now_mono_s();
    seed_visit_anchor(state, now_m - 5.0);
    dsd_call_observation live_call = {0};
    live_call.protocol = DSD_SYNC_NXDN_POS;
    live_call.slot = 0U;
    live_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    live_call.ota_target_id = 7302U;
    live_call.policy_target_id = 7302U;
    live_call.ota_source_id = 8302U;
    live_call.observed_m = now_m - 0.4;
    rc |= expect_true("visit-cap-gate-seeds-call",
                      dsd_call_state_observe(state, &live_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    rc |= expect_true("visit-cap-gate-seeds-media", dsd_call_state_update_media(state, 0U, 1, now_m - 0.4) == 1
                                                        && dsd_call_state_update_media(state, 0U, 1, now_m) == 1);
    dsd_scan_voice_gate_tick(opts, state, 1, now_m);
    rc |=
        expect_true("visit-cap-gate-holds", dsd_scan_voice_gate_should_step(opts, state, dsd_decode_now_mono_s()) == 0);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("visit-cap-gate-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 953012500U);
    rc |= expect_true("visit-cap-gate-advanced", state->lcn_freq_roll == 2);
    opts->scan_voice_only = 0;

    // The operator hold suspends the cap: the receiver stays, the roll stands still, and the
    // legacy dwell anchor is left alone so the release grants a full window.
    state->lcn_scan_hold = 1;
    state->lcn_freq_roll = 2;
    state->last_cc_sync_time = time(NULL);
    const time_t held_dwell_anchor = state->last_cc_sync_time;
    now_m = dsd_decode_now_mono_s();
    seed_visit_anchor(state, now_m - 5.0);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("visit-cap-hold-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("visit-cap-hold-keeps-roll", state->lcn_freq_roll == 2);
    rc |= expect_true("visit-cap-hold-leaves-dwell-alone", state->last_cc_sync_time == held_dwell_anchor);
    state->lcn_scan_hold = 0;

    // Disabled (the default): an ancient anchor changes nothing, and the hangtime rule still
    // decides the rotation entirely on its own.
    opts->scan_max_visit_ms = 0;
    state->lcn_freq_roll = 2;
    state->last_cc_sync_time = time(NULL);
    now_m = dsd_decode_now_mono_s();
    seed_visit_anchor(state, now_m - 600.0);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("visit-cap-disabled-no-retune", g_rtl_tune_calls == 0 && state->lcn_freq_roll == 2);
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("visit-cap-disabled-keeps-hangtime",
                      g_rtl_tune_calls > 0 && g_rtl_tune_freq == 954012500U && state->lcn_freq_roll == 3);

    // One usable row: a hop would land back on the same frequency, so the cap re-arms instead of
    // tearing down audio it would immediately have to rebuild.
    opts->scan_max_visit_ms = 2000;
    state->trunk_lcn_freq[0] = 955012500;
    state->lcn_freq_count = 1;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL);
    now_m = dsd_decode_now_mono_s();
    seed_visit_anchor(state, now_m - 5.0);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("visit-cap-single-row-no-retune", g_rtl_tune_calls == 0 && state->lcn_freq_roll == 0);

    // Requirement 5 is a re-arm, not a freeze: the loop's tick slides the anchor while the rotation
    // has nowhere to go, so handing it a second row does not hop the instant that row appears.
    dsd_engine_scan_visit_tick(opts, state, dsd_decode_now_mono_s());
    state->trunk_lcn_freq[1] = 956012500;
    state->lcn_freq_count = 2;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("visit-cap-rearmed-row-no-instant-hop", g_rtl_tune_calls == 0 && state->lcn_freq_roll == 0);

    // The cap's hop walks the avoid list exactly as the dwell's does: the avoided row is stepped
    // over in the same pass rather than costing a visit of its own.
    state->trunk_lcn_freq[2] = 957012500;
    state->lcn_freq_count = 3;
    rc |= expect_true("visit-cap-avoid-set", dsd_state_trunk_lcn_avoid_set(state, 1U, 1) == 0);
    state->lcn_freq_roll = 1;
    state->last_cc_sync_time = time(NULL);
    now_m = dsd_decode_now_mono_s();
    seed_visit_anchor(state, now_m - 5.0);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("visit-cap-avoid-skips-row", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 957012500U);
    rc |= expect_true("visit-cap-avoid-advanced-past", state->lcn_freq_roll == 3);

    // A failed hop must reopen the current row's windows so sync can be decoded again.
    // Also cover the preexisting voice-gate escape with the visit cap disabled.
    (void)dsd_state_trunk_lcn_avoid_clear(state);
    static const int failed_tunes[] = {RTL_STREAM_TUNE_TIMEOUT, RTL_STREAM_TUNE_DEFERRED, RTL_STREAM_TUNE_FAILED};
    for (int gate = 0; gate < 2; gate++) {
        opts->scan_max_visit_ms = gate ? 0 : 1000;
        opts->scan_voice_only = gate;
        for (size_t i = 0; i < sizeof failed_tunes / sizeof failed_tunes[0]; i++) {
            state->lcn_freq_roll = 0;
            state->trunk_lcn_freq[0] = 958012500;
            now_m = dsd_decode_now_mono_s();
            seed_visit_anchor(state, now_m - 5.0);
            state->scan_voice_gate_sync_m = gate ? now_m - 4.0 : -1.0;
            state->last_cc_sync_time = time(NULL) - (gate ? 11 : 0);
            g_rtl_tune_result = failed_tunes[i];
            g_rtl_tune_calls = 0;
            noCarrier(opts, state);
            rc |= expect_true("abandoned-scan-attempted", g_rtl_tune_calls == 1 && state->lcn_freq_roll == 0);
            rc |= expect_true("abandoned-scan-cap-rearmed",
                              !dsd_engine_scan_visit_expired(opts, state, dsd_decode_now_mono_s()));
            rc |= expect_true("abandoned-scan-gate-rearmed",
                              !dsd_scan_voice_gate_should_step(opts, state, dsd_decode_now_mono_s()));
            noCarrier(opts, state);
            rc |= expect_true("abandoned-scan-no-immediate-retry", g_rtl_tune_calls == 1);
        }
    }
    // Recovery remains possible once the fresh interval expires.
    opts->scan_max_visit_ms = 1000;
    opts->scan_voice_only = 0;
    seed_visit_anchor(state, dsd_decode_now_mono_s() - 2.0);
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);
    rc |= expect_true("abandoned-scan-recovers", g_rtl_tune_calls == 2 && state->lcn_freq_roll == 1);

    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

/* --- Issue #526: a front end whose output rate follows its receive family, as the RTL stream's does at bw=24: the
 * analog monitor resampled to 48 kHz, the digital family at the 24 kHz DSP rate. --- */
static unsigned int
fake_family_output_rate_hz(void) {
    return __wrap_rtl_stream_output_rate(NULL);
}

static int
fake_family_analog_active(void) {
    return g_rtl_analog_family;
}

/* The decoder's output-rate query, which the tuning side's is told apart from by what each records. */
static unsigned int
fake_hook_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz, int cqpsk_explicit) {
    (void)symbol_rate_hz;
    if (family == DSD_RX_FAMILY_DIGITAL) {
        g_hook_rate_explicit = cqpsk_explicit;
        g_hook_rate_cqpsk = cqpsk_enable;
    }
    return fake_output_rate_for_family(family);
}

/* The decoder's read of the same answer, when it times a row (dsd_scan_mode_symbol_timing_rate_hz()). */
static int
fake_hook_family_landing_after_pending(void) {
    g_timing_after_pending = fake_family_landing_after_pending();
    return g_timing_after_pending;
}

/* Whether no retune found work outstanding that lands a family where the decoder's last timing had not (issue #583):
 * the answer can fall between a row's timing and its retune, as outstanding analog work fails, but no scan path queues
 * such work in between that could make it rise. */
static int
expect_no_rise_after_timing(const char* label) {
    if (g_tuning_rises_after_timing != 0) {
        DSD_FPRINTF(stderr, "%s: a retune found a family landing outstanding %d time(s) after a timing that had not\n",
                    label, g_tuning_rises_after_timing);
        return 1;
    }
    return 0;
}

/* The decoder side reads the stream through the metrics hooks, the tuning side through the wrapped stream calls: both
 * see the same front end. */
static void
install_family_rate_hooks(void) {
    g_rtl_family_rates = 1;
    dsd_rtl_stream_metrics_hooks hooks = {0};
    hooks.output_rate_hz = fake_family_output_rate_hz;
    hooks.analog_family_active = fake_family_analog_active;
    hooks.family_landing_after_pending = fake_hook_family_landing_after_pending;
    hooks.output_rate_for_family = fake_hook_output_rate_for_family;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

/* A digital -Y row after an nfm row is timed for the family its tune lands: the decoder and the TED the retune profile
 * carries alike, since nothing re-pushes the TED once the switch has landed. DMR gets 5 samples per symbol at 24 kHz
 * and NXDN48 10, not the 10 and 20 the monitor's 48 kHz would give. */
static int
test_typed_scan_digital_row_after_nfm_timed_for_digital(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    install_family_rate_hooks();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->lcn_freq_count = 4;
    const long freqs[] = {154230000L, 461200000L, 155230000L, 461300000L};
    const dsd_scan_mode modes[] = {DSD_SCAN_MODE_NFM, DSD_SCAN_MODE_DMR, DSD_SCAN_MODE_NFM, DSD_SCAN_MODE_NXDN48};
    for (int i = 0; i < 4; i++) {
        state->trunk_lcn_freq[i] = freqs[i];
        rc |= expect_true("family timing row mode", dsd_channel_mode_set(state, (size_t)i, modes[i]) == 0);
    }
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("family timing nfm row", state->lcn_freq_roll == 1 && g_rtl_analog_family == 1);

    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("dmr row after nfm lands digital",
                      state->lcn_freq_roll == 2 && g_rtl_analog_family == 0 && g_rtl_symbol_rate_hz == 4800);
    rc |= expect_true("dmr row after nfm timed for 24 kHz", state->samplesPerSymbol == 5 && g_rtl_ted_sps == 5);
    if (state->samplesPerSymbol != 5 || g_rtl_ted_sps != 5) {
        DSD_FPRINTF(stderr, "  dmr row: decoder sps=%d, TED %d\n", state->samplesPerSymbol, g_rtl_ted_sps);
    }

    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("family timing second nfm row", state->lcn_freq_roll == 3 && g_rtl_analog_family == 1);

    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("nxdn48 row after nfm lands digital",
                      state->lcn_freq_roll == 4 && g_rtl_analog_family == 0 && g_rtl_symbol_rate_hz == 2400);
    rc |= expect_true("nxdn48 row after nfm timed for 24 kHz", state->samplesPerSymbol == 10 && g_rtl_ted_sps == 10);
    if (state->samplesPerSymbol != 10 || g_rtl_ted_sps != 10) {
        DSD_FPRINTF(stderr, "  nxdn48 row: decoder sps=%d, TED %d\n", state->samplesPerSymbol, g_rtl_ted_sps);
    }
    rc |= expect_no_rise_after_timing("typed -Y rows after nfm rows");

    dsd_engine_channel_scan_leave(opts, state);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

static int g_leave_demod_ted_sps = 0;

static int
record_leave_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps) {
    (void)cqpsk_enable;
    (void)symbol_rate_hz;
    (void)levels;
    (void)channel_profile;
    g_leave_demod_ted_sps = ted_sps;
    return 0;
}

/* A decode-mode change made while an nfm row is on air, as apply_cmd_scoped() runs DECODE_MODE_SET: the command runs on
 * the configured baseline with the front end still on the row's analog family, so it times the new mode at the live
 * rate, the monitor's 48 kHz. The baseline the scope keeps is timed for the digital family (24 kHz), which an untyped
 * row's tune and the leave land on and which nothing retimes afterwards: the untyped row after it, and the session
 * after leaving the scan on a DMR row, run DMR at 5 samples per symbol in the decoder and the TED alike, not 10. */
static int
test_scoped_mode_change_on_nfm_row_times_the_baseline_for_digital(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    install_family_rate_hooks();
    dsd_rtl_stream_metrics_hooks hooks = {0};
    hooks.output_rate_hz = fake_family_output_rate_hz;
    hooks.analog_family_active = fake_family_analog_active;
    hooks.output_rate_for_family = __wrap_rtl_stream_output_rate_for_family;
    hooks.apply_demod_profile = record_leave_demod_profile;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    g_leave_demod_ted_sps = 0;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    /* A configured P25 Phase 1 session on the digital family at 24 kHz. */
    rc |= expect_true("baseline p25",
                      dsd_apply_decode_mode_preset(DSDCFG_MODE_P25P1, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    const dsd_decode_mode_profile p25 = dsd_decode_mode_profile_for(DSDCFG_MODE_P25P1);
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, p25.symbol_rate_hz, 24000);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
    state->sps_hunt_idx = (int)p25.sps_profile_index;
    state->lcn_freq_count = 3;
    const long freqs[] = {154730000L, 461600000L, 461700000L};
    const dsd_scan_mode modes[] = {DSD_SCAN_MODE_NFM, DSD_SCAN_MODE_INHERIT, DSD_SCAN_MODE_DMR};
    for (int i = 0; i < 3; i++) {
        state->trunk_lcn_freq[i] = freqs[i];
        rc |= expect_true("scoped mode row", dsd_channel_mode_set(state, (size_t)i, modes[i]) == 0);
    }
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("scoped mode nfm row on air", state->lcn_freq_roll == 1 && g_rtl_analog_family == 1);

    /* DECODE_MODE_SET -> DMR under the row: suspend, the preset, decode_mode_republish()'s timing at the live rate,
       svc_publish_symbol_profile()'s hunt index (it stops there while the scope is suspended), resume. */
    rc |= expect_true("scoped mode suspends", dsd_scan_mode_suspend(opts, state) == 1);
    rc |= expect_true("scoped mode dmr preset",
                      dsd_apply_decode_mode_preset(DSDCFG_MODE_DMR, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    const dsd_decode_mode_profile dmr = dsd_decode_mode_profile_for(DSDCFG_MODE_DMR);
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, dmr.symbol_rate_hz, (int)fake_family_output_rate_hz());
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
    state->sps_hunt_idx = (int)dmr.sps_profile_index;
    rc |= expect_true("the command timed dmr at the monitor's rate", state->samplesPerSymbol == 10);
    (void)dsd_scan_mode_resume(opts, state);
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    rc |= expect_true("configured dmr timed for the digital family",
                      configured != NULL && configured->state_samplesPerSymbol == 5);
    rc |= expect_true("the nfm row stays on its monitor", g_rtl_analog_family == 1 && opts->analog_only == 1);

    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("untyped row after the change lands digital",
                      state->lcn_freq_roll == 2 && g_rtl_analog_family == 0);
    rc |= expect_true("untyped row runs dmr at 24 kHz", state->samplesPerSymbol == 5 && g_rtl_ted_sps == 5);
    if (state->samplesPerSymbol != 5 || g_rtl_ted_sps != 5) {
        DSD_FPRINTF(stderr, "  untyped row: decoder sps=%d, TED %d\n", state->samplesPerSymbol, g_rtl_ted_sps);
    }

    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("dmr row on air", state->lcn_freq_roll == 3 && g_rtl_analog_family == 0);
    dsd_engine_channel_scan_leave(opts, state);
    rc |= expect_true("leaving on the dmr row keeps dmr at 24 kHz",
                      state->samplesPerSymbol == 5 && g_leave_demod_ted_sps == 5);
    if (state->samplesPerSymbol != 5 || g_leave_demod_ted_sps != 5) {
        DSD_FPRINTF(stderr, "  after leave: decoder sps=%d, TED %d\n", state->samplesPerSymbol, g_leave_demod_ted_sps);
    }

    dsd_rtl_stream_metrics_hooks_set(NULL);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

/* The same through --trunk-scan and the real tuning: a DMR conventional target after an nfm-conventional one. */
static int
test_trunk_scan_digital_target_after_nfm_timed_for_digital(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "nfm-family-timing-");
    if (fd < 0) {
        free_test_runtime(opts, state);
        return 1;
    }
    dsd_close(fd);
    FILE* fp = dsd_fopen_private(path, "w");
    int rc = expect_true("family timing targets file", fp != NULL);
    if (fp) {
        DSD_FPRINTF(fp, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                        "fire,nfm-conventional,154330000,,250,250,,\n"
                        "dmr,dmr-conventional,461400000,,250,250,,\n");
        (void)fclose(fp);
    }
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    install_family_rate_hooks();
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = dsd_engine_trunk_tune_to_freq_request,
                                                        .tune_to_cc_request = dsd_engine_trunk_tune_to_cc_request,
                                                        .return_to_cc_request = dsd_engine_return_to_cc_request});
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", path);
    char err[256] = {0};
    rc |= expect_true("family timing trunk scan init", dsd_engine_trunk_scan_init(opts, state, err, sizeof err) == 0);
    rc |= expect_true("family timing nfm target", g_rtl_analog_family == 1 && g_rtl_tune_freq == 154330000U);
    rc |= expect_true("family timing advance",
                      dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
    rc |= expect_true("dmr target after nfm lands digital",
                      g_rtl_analog_family == 0 && g_rtl_tune_freq == 461400000U && g_rtl_symbol_rate_hz == 4800);
    rc |= expect_true("dmr target after nfm timed for 24 kHz", state->samplesPerSymbol == 5 && g_rtl_ted_sps == 5);
    if (state->samplesPerSymbol != 5 || g_rtl_ted_sps != 5) {
        DSD_FPRINTF(stderr, "  dmr target: decoder sps=%d, TED %d\n", state->samplesPerSymbol, g_rtl_ted_sps);
    }
    dsd_engine_trunk_scan_shutdown(opts, state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_trunk_tuning_requests_reset();
    state->rtl_ctx = NULL;
    (void)remove(path);
    free_test_runtime(opts, state);
    return rc;
}

/* --- Issue #583: a digital target's retune queued while an nfm-conventional target's retune, or a width edit made for
 * it, is still outstanding. Tunes that time out stay outstanding (g_rtl_timeout_outstanding), and the decoder reads
 * the front end through the family-rate hooks. --- */

static const char kOutstandingTargets[] = "fire,nfm-conventional,154330000,,250,250,,\n"
                                          "dmr,dmr-conventional,461400000,,250,250,,\n";

static int
expect_case(const char* label, const char* what, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: %s failed\n", label, what);
        return 1;
    }
    return 0;
}

/* Start a --trunk-scan over @p targets (under the CSV @p header, written to @p path) on a digital RTL session, through
 * the real coordinator and tuning, with the family-rate hooks installed. With @p outstanding the nfm target's first
 * retune times out: it lands only when the case lands it. */
static int
start_family_trunk_scan(dsd_opts* opts, dsd_state* state, char* path, size_t path_size, const char* header,
                        const char* targets, int outstanding) {
    const int fd = dsd_test_mkstemp(path, path_size, "nfm-family-scan-");
    if (fd < 0) {
        path[0] = '\0';
        return 1;
    }
    dsd_close(fd);
    FILE* fp = dsd_fopen_private(path, "w");
    int rc = expect_true("family scan targets file", fp != NULL);
    if (fp) {
        DSD_FPRINTF(fp, "%s%s", header, targets);
        (void)fclose(fp);
    }
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    install_family_rate_hooks();
    if (outstanding) {
        g_rtl_timeout_outstanding = 1;
        g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = dsd_engine_trunk_tune_to_freq_request,
                                                        .tune_to_cc_request = dsd_engine_trunk_tune_to_cc_request,
                                                        .return_to_cc_request = dsd_engine_return_to_cc_request});
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_scan_enabled = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", path);
    char err[256] = {0};
    rc |= expect_true("family scan init", dsd_engine_trunk_scan_init(opts, state, err, sizeof err) == 0);
    return rc;
}

/* start_family_trunk_scan() with the nfm target's first retune timing out, still in flight. */
static int
start_outstanding_trunk_scan(dsd_opts* opts, dsd_state* state, char* path, size_t path_size, const char* targets) {
    int rc =
        start_family_trunk_scan(opts, state, path, path_size,
                                "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n", targets, 1);
    rc |= expect_true("the nfm retune is in flight", g_retune_in_flight.active && g_retune_in_flight.freq == 154330000U
                                                         && fake_retune_lands_analog(&g_retune_in_flight)
                                                         && g_rtl_analog_family == 0);
    return rc;
}

/* Stop the scan start_family_trunk_scan() started, first checking that no retune of it found the analog family
 * outstanding where the timing before it had not (expect_no_rise_after_timing()). Returns that check's result. */
static int
stop_outstanding_trunk_scan(dsd_opts* opts, dsd_state* state, const char* path) {
    const int rc = expect_no_rise_after_timing("trunk scan");
    dsd_engine_trunk_scan_shutdown(opts, state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_trunk_tuning_requests_reset();
    reset_rtl_profile_fakes();
    state->rtl_ctx = NULL;
    if (path[0] != '\0') {
        (void)remove(path);
    }
    free_test_runtime(opts, state);
    return rc;
}

/* Item 4: an Advance or Avoid while the nfm target's retune is still in flight (it outlasted the tune wait) queues the
 * DMR target's retune behind it. The nfm retune lands the analog family first, so the DMR retune has to carry the
 * digital family, or its FSK profile would run on the monitor output and decode nothing; after Avoid in a two-target
 * scan nothing would ask for the digital family again. The decoder and the TED the retune carries are timed for the
 * family it lands. */
static int
test_trunk_scan_moves_on_while_an_nfm_retune_is_in_flight(int op, const char* label) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    char path[DSD_TEST_PATH_MAX];
    int rc = start_outstanding_trunk_scan(opts, state, path, sizeof path, kOutstandingTargets);
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    rc |= expect_case(label, "the scanner moves on", dsd_engine_trunk_scan_control(opts, state, op) == 0);
    rc |= expect_case(label, "the dmr retune carries the digital family",
                      tune_family_for(461400000U) == DSD_RX_FAMILY_DIGITAL);
    rc |= expect_case(label, "the dmr target lands on the FSK discriminator after the nfm retune",
                      !g_retune_in_flight.active && g_rtl_tune_freq == 461400000U && g_rtl_analog_family == 0
                          && g_rtl_symbol_rate_hz == 4800 && g_rtl_cqpsk_enable == 0);
    rc |= expect_case(label, "the dmr target is timed for the digital family",
                      state->samplesPerSymbol == 5 && g_rtl_ted_sps == 5);
    if (rc) {
        DSD_FPRINTF(stderr, "  %s: family %d, analog %d, decoder sps=%d, TED %d\n", label, tune_family_for(461400000U),
                    g_rtl_analog_family, state->samplesPerSymbol, g_rtl_ted_sps);
    }
    rc |= stop_outstanding_trunk_scan(opts, state, path);
    return rc;
}

/* Item 4 with the DMR retune still queued behind the nfm one when the operator moves on again: the controller
 * coalesces the next retune into the queued one and takes its profile, so the third target's retune lands straight
 * after the nfm one and carries the digital family too. */
static int
test_trunk_scan_coalesced_retune_behind_an_nfm_retune(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    const char* label = "coalesced behind an nfm retune";
    char path[DSD_TEST_PATH_MAX];
    int rc = start_outstanding_trunk_scan(opts, state, path, sizeof path,
                                          "fire,nfm-conventional,154330000,,250,250,,\n"
                                          "dmr,dmr-conventional,461400000,,250,250,,\n"
                                          "dmr2,dmr-conventional,461500000,,250,250,,\n");
    rc |= expect_case(label, "advance to dmr",
                      dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
    rc |= expect_case(label, "the dmr retune waits behind the nfm one",
                      g_retune_in_flight.freq == 154330000U && g_retune_queued.freq == 461400000U);
    rc |= expect_case(label, "advance to dmr2",
                      dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
    rc |= expect_case(label, "the dmr2 retune replaced the queued one",
                      g_retune_in_flight.freq == 154330000U && g_retune_queued.freq == 461500000U);
    rc |= expect_case(label, "the dmr2 retune carries the digital family",
                      tune_family_for(461500000U) == DSD_RX_FAMILY_DIGITAL);
    land_outstanding_tunes(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_case(label, "dmr2 lands on the FSK discriminator after the nfm retune",
                      g_rtl_analog_family == 0 && g_rtl_symbol_rate_hz == 4800 && g_rtl_cqpsk_enable == 0);
    rc |= stop_outstanding_trunk_scan(opts, state, path);
    return rc;
}

/* Item 2: a width edit made while the nfm target's retune is outstanding queues a live analog request the demod thread
 * has not taken, which also supersedes the family that retune carries. When the retune then completes FAILED, the
 * tick's automatic advance tunes the DMR target: its retune must carry the digital family, which retires the older
 * width request, or the demod thread's next block boundary would put the front end on the 16 kHz monitor while the
 * scanner sits on DMR for its whole visit. */
static int
test_trunk_scan_failed_nfm_retune_with_a_width_edit_queued(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    const char* label = "failed nfm retune, width edit queued";
    char path[DSD_TEST_PATH_MAX];
    int rc = start_outstanding_trunk_scan(opts, state, path, sizeof path, kOutstandingTargets);
    fake_live_width_request(16000);
    rc |= expect_case(label, "the width edit supersedes the nfm retune's family",
                      !fake_retune_lands_analog(&g_retune_in_flight) && g_rtl_analog_request_queued);
    land_outstanding_tunes(DSD_TRUNK_TUNE_RESULT_FAILED);
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    dsd_engine_trunk_scan_tick(opts, state);
    rc |= expect_case(label, "the tick advances to the dmr target", g_rtl_tune_freq == 461400000U);
    rc |= expect_case(label, "the dmr retune carries the digital family",
                      tune_family_for(461400000U) == DSD_RX_FAMILY_DIGITAL);
    fake_demod_boundary();
    rc |= expect_case(label, "the width request was retired: the dmr target stays on FSK",
                      g_rtl_analog_family == 0 && g_rtl_symbol_rate_hz == 4800 && g_rtl_cqpsk_enable == 0);
    rc |= stop_outstanding_trunk_scan(opts, state, path);
    return rc;
}

/* Item 2 without the failure: the nfm retune is still in flight when the width edit is made and the operator advances.
 * That retune lands neither its family nor its profile (the edit superseded it), and only the queued width request
 * says the front end may still go analog; the DMR retune carries the digital family and retires it. */
static int
test_trunk_scan_advance_with_a_width_edit_queued(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    const char* label = "advance with a width edit queued";
    char path[DSD_TEST_PATH_MAX];
    int rc = start_outstanding_trunk_scan(opts, state, path, sizeof path, kOutstandingTargets);
    fake_live_width_request(16000);
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    rc |= expect_case(label, "advance to dmr",
                      dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
    rc |= expect_case(label, "the dmr retune carries the digital family",
                      tune_family_for(461400000U) == DSD_RX_FAMILY_DIGITAL);
    fake_demod_boundary();
    rc |= expect_case(label, "the width request was retired: the dmr target stays on FSK",
                      !g_retune_in_flight.active && g_rtl_analog_family == 0 && g_rtl_symbol_rate_hz == 4800
                          && g_rtl_cqpsk_enable == 0);
    rc |= stop_outstanding_trunk_scan(opts, state, path);
    return rc;
}

/* --- Issue #583 item 3 and its GFSK sibling: a trunk-scan target's own CQPSK choice after an nfm-conventional target,
 * under a DSD_NEO_CQPSK that says otherwise. The tunes land at once. Where the retune lands is the stream's
 * (IO_RTL_RETUNE_PREPARE); these check that the digital family the tuning side attaches says the profile's CQPSK state
 * is the target's own, and that the decoder's timing asks for the landing rate the same way, the two by one rule. --- */

static const char kModulationHeader[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation\n";

/* The same as stop_outstanding_trunk_scan(), with DSD_NEO_CQPSK unset again. */
static int
stop_cqpsk_env_trunk_scan(dsd_opts* opts, dsd_state* state, const char* path) {
    const int rc = stop_outstanding_trunk_scan(opts, state, path);
    (void)dsd_unsetenv("DSD_NEO_CQPSK");
    dsd_neo_config_init();
    return rc;
}

/* One digital target after an nfm-conventional one, advanced to under DSD_NEO_CQPSK=@p cqpsk_env. */
typedef struct {
    const char* label;
    const char* cqpsk_env;
    const char* digital_target; /* its CSV row, at 461400000 Hz (DMR) or 851012500 Hz (P25) */
    uint32_t digital_hz;
    int want_cqpsk;    /* the CQPSK request of the profile its tune queues (-1: left to the stream) */
    int want_explicit; /* whether the digital family attached says that request is the target's own */
    int want_rf_mod;
    int want_stream_explicit; /* the tuning side's own landing-rate query for the TED (-1: none, as for P25) */
} cqpsk_choice_case;

static int
run_cqpsk_choice_case(const cqpsk_choice_case* c) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    (void)dsd_setenv("DSD_NEO_CQPSK", c->cqpsk_env, 1);
    dsd_neo_config_init();
    char targets[256];
    DSD_SNPRINTF(targets, sizeof targets, "fire,nfm-conventional,154330000,,250,250,,\n%s", c->digital_target);
    char path[DSD_TEST_PATH_MAX];
    int rc = start_family_trunk_scan(opts, state, path, sizeof path, kModulationHeader, targets, 0);
    rc |= expect_case(c->label, "the nfm target runs the monitor",
                      g_rtl_analog_family == 1 && g_rtl_tune_freq == 154330000U);
    g_hook_rate_explicit = -1;
    g_stream_rate_explicit = -1;
    rc |= expect_case(c->label, "advance to the digital target",
                      dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
    rc |= expect_case(c->label, "its retune carries the digital family",
                      g_rtl_tune_freq == c->digital_hz && tune_family_for(c->digital_hz) == DSD_RX_FAMILY_DIGITAL);
    rc |= expect_case(c->label, "its profile asks for the target's CQPSK state",
                      tune_cqpsk_for(c->digital_hz) == c->want_cqpsk && state->rf_mod == c->want_rf_mod);
    rc |= expect_case(c->label, "the digital family says whose choice it is",
                      tune_explicit_for(c->digital_hz) == c->want_explicit);
    rc |= expect_case(c->label, "the decoder is timed by the same rule", g_hook_rate_explicit == c->want_explicit);
    rc |= expect_case(c->label, "and so is the TED the tune queues", g_stream_rate_explicit == c->want_stream_explicit);
    if (rc) {
        DSD_FPRINTF(stderr,
                    "  %s: family %d, cqpsk %d, explicit %d, rf_mod %d, decoder asked explicit %d, tuning asked "
                    "explicit %d\n",
                    c->label, tune_family_for(c->digital_hz), tune_cqpsk_for(c->digital_hz),
                    tune_explicit_for(c->digital_hz), state->rf_mod, g_hook_rate_explicit, g_stream_rate_explicit);
    }
    rc |= stop_cqpsk_env_trunk_scan(opts, state, path);
    return rc;
}

/* Item 3: a P25 target that names its modulation keeps it over DSD_NEO_CQPSK after an nfm target, as it does after a
 * digital target: modulation=cqpsk under DSD_NEO_CQPSK=0, and the mirror, modulation=c4fm under =1. auto is the
 * target's own choice too (C4FM for a site not yet heard on CQPSK), and stands under =1. A P25 target with no
 * modulation still lands where an open would, as the override says: its digital family says nothing of its own. */
static int
test_trunk_scan_p25_modulation_stands_after_nfm(void) {
    static const cqpsk_choice_case cases[] = {
        {"cqpsk target under DSD_NEO_CQPSK=0", "0", "lsm,p25-conventional,851012500,,250,250,,cqpsk\n", 851012500U, 1,
         1, 1, -1},
        {"auto target under DSD_NEO_CQPSK=1", "1", "lsm,p25-conventional,851012500,,250,250,,auto\n", 851012500U, 0, 1,
         0, -1},
        {"c4fm target under DSD_NEO_CQPSK=1", "1", "lsm,p25-conventional,851012500,,250,250,,c4fm\n", 851012500U, 0, 1,
         0, -1},
        {"p25 target without modulation under DSD_NEO_CQPSK=0", "0", "lsm,p25-conventional,851012500,,250,250,,\n",
         851012500U, -1, 0, 0, -1},
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rc |= run_cqpsk_choice_case(&cases[i]);
    }
    return rc;
}

/* The GFSK sibling: a DMR target always runs the FSK discriminator, and the GFSK chain queues CQPSK off for it. Under
 * DSD_NEO_CQPSK=1 that stands after an nfm target as it does after a digital one, and the TED the tuning side computes
 * for the landing is asked for the same way. */
static int
test_trunk_scan_dmr_target_stays_fsk_after_nfm(void) {
    static const cqpsk_choice_case c = {
        "dmr target under DSD_NEO_CQPSK=1", "1", "dmr,dmr-conventional,461400000,,250,250,,\n", 461400000U, 0, 1, 2, 1};
    return run_cqpsk_choice_case(&c);
}

/* A P25 target with no modulation under -mq and DSD_NEO_CQPSK=0, advanced to while the nfm target's retune is still in
 * flight: the nfm retune may land the analog family first, so the P25 retune carries the digital family, and its
 * profile leaves the CQPSK state to the stream, which lands the one the override names. The P25 chain times the
 * decoder, and the TED the retune carries, for that landing by the same answer: the digital family's rate, asked for
 * -mq's CQPSK with no choice of the target's own. Should the nfm retune then fail, the P25 retune lands on the digital
 * family it finds, where the stream still lands it as a switch would (IO_RTL_RETUNE_PREPARE). */
static int
test_trunk_scan_p25_target_without_modulation_behind_an_nfm_retune(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    const char* label = "p25 target without modulation behind an nfm retune";
    (void)dsd_setenv("DSD_NEO_CQPSK", "0", 1);
    dsd_neo_config_init();
    /* -mq: the CQPSK lock a target with no modulation keeps. */
    opts->mod_c4fm = 0;
    opts->mod_qpsk = 1;
    opts->mod_gfsk = 0;
    opts->mod_cli_lock = 1;
    state->rf_mod = 1;
    char path[DSD_TEST_PATH_MAX];
    int rc = start_family_trunk_scan(opts, state, path, sizeof path, kModulationHeader,
                                     "fire,nfm-conventional,154330000,,250,250,,\n"
                                     "lsm,p25-conventional,851012500,,250,250,,\n",
                                     1);
    rc |= expect_case(label, "the nfm retune is in flight",
                      g_retune_in_flight.freq == 154330000U && fake_retune_lands_analog(&g_retune_in_flight)
                          && g_rtl_analog_family == 0);
    g_hook_rate_cqpsk = -1;
    g_hook_rate_explicit = -1;
    rc |= expect_case(label, "advance to the p25 target",
                      dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
    rc |= expect_case(label, "its retune waits behind the nfm one", g_retune_queued.freq == 851012500U);
    rc |= expect_case(label, "its retune carries the digital family, with no choice of its own",
                      tune_family_for(851012500U) == DSD_RX_FAMILY_DIGITAL && tune_explicit_for(851012500U) == 0);
    rc |= expect_case(label, "its profile leaves the CQPSK state to the stream",
                      tune_cqpsk_for(851012500U) == -1 && state->rf_mod == 1);
    rc |= expect_case(label, "the decoder is timed for the switch's landing",
                      g_hook_rate_cqpsk == 1 && g_hook_rate_explicit == 0 && state->samplesPerSymbol == 5);
    rc |= expect_case(label, "and so is the TED its retune carries", g_retune_queued.profile.ted_sps == 5);
    if (rc) {
        DSD_FPRINTF(stderr,
                    "  %s: family %d, cqpsk %d, explicit %d, rf_mod %d, decoder asked cqpsk %d explicit %d, sps %d, "
                    "TED %d\n",
                    label, tune_family_for(851012500U), tune_cqpsk_for(851012500U), tune_explicit_for(851012500U),
                    state->rf_mod, g_hook_rate_cqpsk, g_hook_rate_explicit, state->samplesPerSymbol,
                    g_retune_queued.profile.ted_sps);
    }
    land_outstanding_tune(&g_retune_in_flight, DSD_TRUNK_TUNE_RESULT_FAILED);
    land_outstanding_tune(&g_retune_queued, DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_case(label, "the p25 retune lands on the digital family the failed nfm retune left",
                      g_rtl_analog_family == 0 && g_rtl_symbol_rate_hz == 4800);
    rc |= stop_cqpsk_env_trunk_scan(opts, state, path);
    return rc;
}

/* A digital target advanced to behind an nfm-conventional target's retune, which completes FAILED once the target has
 * been timed and before its retune is prepared (g_fail_in_flight_at_tune_snapshot). */
typedef struct {
    const char* label;
    const char* digital_target; /* its CSV row */
    uint32_t digital_hz;
    int mq; /* 1: -mq, the CQPSK lock a P25 target with no modulation keeps */
} failed_after_timing_case;

/* Issue #583, one landing-family decision per row: the target was timed for the output rate the digital family lands
 * on while the nfm retune was outstanding, so its retune carries that family, which lands it there, although by the
 * time the retune is prepared nothing outstanding says the analog family may still run. The front end runs CQPSK at
 * 78125 Hz, left by an earlier target that asked for it, where the switch lands the FSK discriminator at 48 kHz under
 * DSD_NEO_CQPSK=0: 10 samples per symbol, where the live rate would give 16. Without the family the retune would keep
 * the CQPSK front end at 78125 Hz under a decoder, and a TED, timed for 48 kHz. */
static int
run_nfm_retune_fails_after_the_target_was_timed(const failed_after_timing_case* c) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    (void)dsd_setenv("DSD_NEO_CQPSK", "0", 1);
    dsd_neo_config_init();
    if (c->mq) {
        opts->mod_c4fm = 0;
        opts->mod_qpsk = 1;
        opts->mod_gfsk = 0;
        opts->mod_cli_lock = 1;
        state->rf_mod = 1;
    }
    char targets[256];
    DSD_SNPRINTF(targets, sizeof targets, "fire,nfm-conventional,154330000,,250,250,,\n%s", c->digital_target);
    char path[DSD_TEST_PATH_MAX];
    int rc = start_family_trunk_scan(opts, state, path, sizeof path, kModulationHeader, targets, 1);
    rc |= expect_case(c->label, "the nfm retune is in flight",
                      g_retune_in_flight.freq == 154330000U && fake_retune_lands_analog(&g_retune_in_flight)
                          && g_rtl_analog_family == 0);
    g_rtl_family_rates = 0;
    g_rtl_output_rate = 78125;
    g_rtl_cqpsk_enable = 1;
    g_rtl_ted_sps = 16;
    g_rtl_ted_sps_override = 0;
    g_rtl_digital_landing_rate = 48000;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_fail_in_flight_at_tune_snapshot = 1;
    rc |= expect_case(c->label, "advance to the digital target",
                      dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
    rc |= expect_case(c->label, "the nfm retune failed between the target's timing and its retune",
                      !g_fail_in_flight_at_tune_snapshot && !g_retune_in_flight.active && g_rtl_analog_family == 0
                          && g_rtl_tune_freq == c->digital_hz);
    rc |= expect_case(c->label, "the target is timed for the switch's landing", state->samplesPerSymbol == 10);
    rc |= expect_case(c->label, "its retune carries the digital family, which lands it there",
                      tune_family_for(c->digital_hz) == DSD_RX_FAMILY_DIGITAL);
    rc |= expect_case(c->label, "and the TED it carries is timed for the same landing", g_rtl_ted_sps == 10);
    if (rc) {
        DSD_FPRINTF(stderr, "  %s: family %d, decoder sps=%d, TED %d\n", c->label, tune_family_for(c->digital_hz),
                    state->samplesPerSymbol, g_rtl_ted_sps);
    }
    rc |= stop_cqpsk_env_trunk_scan(opts, state, path);
    return rc;
}

/* The P25 chain, whose TED is the one the target was timed with, and the GFSK chain, which asks the landing rate for
 * its TED itself. */
static int
test_trunk_scan_nfm_retune_fails_after_the_target_was_timed(void) {
    static const failed_after_timing_case cases[] = {
        {"p25 target timed before the nfm retune failed", "lsm,p25-conventional,851012500,,250,250,,\n", 851012500U, 1},
        {"dmr target timed before the nfm retune failed", "dmr,dmr-conventional,461400000,,250,250,,\n", 461400000U, 0},
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rc |= run_nfm_retune_fails_after_the_target_was_timed(&cases[i]);
    }
    return rc;
}

/* A scoped command re-times a P25 target whose retune, carrying the digital family, is still outstanding. */
typedef struct {
    const char* label;
    int in_flight; /* 1: the nfm retune fails before the target's retune is prepared, which then times out in flight;
                      0: the target's retune waits queued behind the nfm one, which then fails */
} resume_behind_digital_retune_case;

/* Issue #583: the P25 target (no modulation, -mq, DSD_NEO_CQPSK=0) was timed for the output rate the digital family
 * lands on, 48 kHz on the FSK discriminator, and its retune carries that family, but the nfm retune it was timed behind
 * failed: the front end still runs CQPSK at 78125 Hz and nothing analog is outstanding. A command made under the target
 * before its retune lands (an option edit that changes what the target runs) resumes the scope, which times the target
 * again: its retune still lands where the digital family's prediction says, so the decoder is timed there, 10 samples
 * per symbol, not the 16 the live rate would give. */
static int
run_resume_behind_an_outstanding_digital_family_retune(const resume_behind_digital_retune_case* c) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    (void)dsd_setenv("DSD_NEO_CQPSK", "0", 1);
    dsd_neo_config_init();
    opts->mod_c4fm = 0;
    opts->mod_qpsk = 1;
    opts->mod_gfsk = 0;
    opts->mod_cli_lock = 1;
    state->rf_mod = 1;
    char path[DSD_TEST_PATH_MAX];
    int rc = start_family_trunk_scan(opts, state, path, sizeof path, kModulationHeader,
                                     "fire,nfm-conventional,154330000,,250,250,,\n"
                                     "lsm,p25-conventional,851012500,,250,250,,\n",
                                     1);
    rc |= expect_case(c->label, "the nfm retune is in flight",
                      g_retune_in_flight.freq == 154330000U && fake_retune_lands_analog(&g_retune_in_flight)
                          && g_rtl_analog_family == 0);
    g_rtl_family_rates = 0;
    g_rtl_output_rate = 78125;
    g_rtl_cqpsk_enable = 1;
    g_rtl_ted_sps = 16;
    g_rtl_ted_sps_override = 0;
    g_rtl_digital_landing_rate = 48000;
    g_fail_in_flight_at_tune_snapshot = c->in_flight;
    rc |= expect_case(c->label, "advance to the p25 target",
                      dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
    if (!c->in_flight) {
        land_outstanding_tune(&g_retune_in_flight, DSD_TRUNK_TUNE_RESULT_FAILED);
    }
    const fake_retune* p25 = c->in_flight ? &g_retune_in_flight : &g_retune_queued;
    rc |= expect_case(c->label, "its retune carries the digital family and is still outstanding",
                      tune_family_for(851012500U) == DSD_RX_FAMILY_DIGITAL && p25->active && p25->freq == 851012500U
                          && p25->profile.analog_family == DSD_RX_FAMILY_DIGITAL);
    rc |= expect_case(c->label, "the nfm retune failed: the front end still runs CQPSK at 78125 Hz",
                      !fake_retune_lands_analog(&g_retune_in_flight) && !fake_retune_lands_analog(&g_retune_queued)
                          && g_rtl_analog_family == 0 && g_rtl_cqpsk_enable == 1);
    rc |= expect_case(c->label, "the target is timed for the switch's landing", state->samplesPerSymbol == 10);

    rc |= expect_case(c->label, "the command suspends the scope", dsd_scan_mode_suspend(opts, state) == 1);
    opts->inverted_dmr = opts->inverted_dmr ? 0 : 1;
    rc |= expect_case(c->label, "the edit changes what the target runs", dsd_scan_mode_resume(opts, state) == 1);
    rc |= expect_case(c->label, "the resume times the target where its retune lands", state->samplesPerSymbol == 10);
    if (rc) {
        DSD_FPRINTF(stderr, "  %s: family %d, decoder sps=%d, TED %d\n", c->label, tune_family_for(851012500U),
                    state->samplesPerSymbol, p25->profile.ted_sps);
    }
    land_outstanding_tunes(DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_case(c->label, "the p25 retune lands on the digital family",
                      g_rtl_analog_family == 0 && g_rtl_symbol_rate_hz == 4800);
    rc |= stop_cqpsk_env_trunk_scan(opts, state, path);
    return rc;
}

static int
test_trunk_scan_resume_behind_an_outstanding_digital_family_retune(void) {
    static const resume_behind_digital_retune_case cases[] = {
        {"resume behind a p25 retune in flight", 1},
        {"resume behind a p25 retune queued", 0},
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rc |= run_resume_behind_an_outstanding_digital_family_retune(&cases[i]);
    }
    return rc;
}

/* A row width the published DSP rate cannot fit is skipped at every visit without asking the stream for it: the stream
 * would log its refusal again each time the valid row beside it re-armed the log. Only the valid row's width ever
 * reaches the stream, and only its frequency is tuned. */
static int
test_typed_scan_refused_width_skipped_without_the_stream(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    g_rtl_request_rate_hz = 16000;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 154530000L;
    state->trunk_lcn_freq[1] = 155530000L;
    const int widths[] = {12500, 20000};
    for (int row = 0; row < 2; row++) {
        rc |= expect_true("refusal row mode", dsd_channel_mode_set(state, (size_t)row, DSD_SCAN_MODE_NFM) == 0);
        dsd_scan_row_profile* profile = NULL;
        rc |= expect_true("refusal row profile", dsd_scan_profile_ensure(&profile) == 0 && profile);
        if (profile) {
            profile->values.present = DSD_SCAN_OPT_BANDWIDTH;
            profile->values.channel_bw_hz = widths[row];
            rc |= expect_true("refusal row width", dsd_channel_profile_set(state, (size_t)row, profile) == 0);
        }
    }
    for (int visit = 0; visit < 6; visit++) {
        state->last_cc_sync_time = time(NULL) - 11;
        noCarrier(opts, state);
    }
    rc |= expect_true("refused width never reaches the stream",
                      g_analog_attach_calls > 0 && g_analog_attach_max_width_hz == 12500);
    rc |= expect_true("refused row never tuned", g_rtl_tune_freq == 154530000U && g_rtl_analog_width_hz == 12500);
    if (g_analog_attach_max_width_hz != 12500) {
        DSD_FPRINTF(stderr, "  widest width asked of the stream: %d Hz\n", g_analog_attach_max_width_hz);
    }
    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

/* DSD_NEO_CHANNEL_LPF=0 turns off the channel filter every explicit width needs, and the stream refuses such a width
 * at any rate. A row with its own width is then skipped at every visit without asking the stream, as a width the rate
 * cannot fit is, so a valid row beside it cannot re-arm the stream's refusal log at every rotation; a row on the unset
 * default still runs. */
static int
test_typed_scan_width_skipped_under_channel_lpf_override(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    (void)dsd_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    g_rtl_request_rate_hz = 48000;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 154530000L;
    state->trunk_lcn_freq[1] = 155530000L;
    for (int row = 0; row < 2; row++) {
        rc |= expect_true("lpf row mode", dsd_channel_mode_set(state, (size_t)row, DSD_SCAN_MODE_NFM) == 0);
    }
    dsd_scan_row_profile* profile = NULL;
    rc |= expect_true("lpf row profile", dsd_scan_profile_ensure(&profile) == 0 && profile);
    if (profile) {
        profile->values.present = DSD_SCAN_OPT_BANDWIDTH;
        profile->values.channel_bw_hz = 12500;
        rc |= expect_true("lpf row width", dsd_channel_profile_set(state, 1U, profile) == 0);
    }
    for (int visit = 0; visit < 6; visit++) {
        state->last_cc_sync_time = time(NULL) - 11;
        noCarrier(opts, state);
    }
    rc |= expect_true("explicit width never reaches the stream",
                      g_analog_attach_calls > 0 && g_analog_attach_max_width_hz == 0);
    rc |= expect_true("default-width row tuned", g_rtl_tune_freq == 154530000U && g_rtl_analog_width_hz == 0);
    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

/* An am row that sets no width of its own runs the AM default, 6 kHz, which always runs its channel filter (issue
 * #524), so a DSP rate too narrow for it (@p lpf_off 0: 6 kHz here, which filters at most 4.2 kHz) or
 * DSD_NEO_CHANNEL_LPF=0 (@p lpf_off 1) refuses it as it refuses an explicit width. The configured AM width is unset.
 * Such a row is skipped at every visit without asking the stream for the AM monitor, as a refused explicit width is,
 * while the nfm row beside it on the unset NFM default, which is never refused, runs. */
static int
typed_scan_am_default_width_skipped_without_the_stream(int lpf_off) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    if (lpf_off) {
        (void)dsd_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
        dsd_neo_config_init();
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    opts->analog_am_bandwidth_hz = 0;
    state->rtl_ctx = (RtlSdrContext*)state;
    g_rtl_request_rate_hz = lpf_off ? 48000 : 6000;
    g_rtl_watch_freq = 118300000U;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 154530000L;
    state->trunk_lcn_freq[1] = 118300000L;
    rc |= expect_true("am default nfm row mode", dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_NFM) == 0);
    rc |= expect_true("am default am row mode", dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_AM) == 0);
    for (int visit = 0; visit < 6; visit++) {
        state->last_cc_sync_time = time(NULL) - 11;
        noCarrier(opts, state);
    }
    rc |=
        expect_true("am default never reaches the stream", g_analog_attach_calls > 0 && g_analog_attach_am_calls == 0);
    rc |= expect_true("am default row never tuned", g_rtl_watch_tunes == 0 && g_rtl_tune_freq == 154530000U);
    if (rc) {
        DSD_FPRINTF(stderr, "  %s: AM monitor asked %d times, am row tuned %d times\n",
                    lpf_off ? "DSD_NEO_CHANNEL_LPF=0" : "6 kHz DSP rate", g_analog_attach_am_calls, g_rtl_watch_tunes);
    }
    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    if (lpf_off) {
        (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
        dsd_neo_config_init();
    }
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

static int
test_typed_scan_am_default_width_skipped_without_the_stream(void) {
    return typed_scan_am_default_width_skipped_without_the_stream(0)
           | typed_scan_am_default_width_skipped_without_the_stream(1);
}

/* What the analog monitor does for each block while its carrier is open: stamp carrier activity, the -Y hangtime
 * anchor. */
static void
stamp_monitor_carrier(dsd_state* state) {
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
}

/* The monitor stamps carrier activity whatever audio_out says (DSP_SYMBOL_REPLAY proves the stamp with audio_out = 0);
 * here the -Y rotation keeps an analog row of class @p mode (nfm, or am: the AM monitor stamps its carrier the same
 * way, issue #526) on air while those stamps keep arriving, a visit already past -t included, and a global
 * --scan-voice-only does not take the row over. Once they stop, the row steps -t later; and a row --scan-max-visit-ms
 * ends a visit whose carrier never drops. */
static int
typed_scan_analog_row_holds_on_carrier(dsd_scan_mode mode) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    opts->audio_out = 0;
    opts->scan_voice_only = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 154630000L;
    state->trunk_lcn_freq[1] = 461500000L;
    rc |= expect_true("hold analog row", dsd_channel_mode_set(state, 0, mode) == 0);
    rc |= expect_true("hold dmr row", dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_DMR) == 0);
    dsd_scan_row_profile* profile = NULL;
    rc |= expect_true("hold row profile", dsd_scan_profile_ensure(&profile) == 0 && profile);
    if (profile) {
        profile->values.present = DSD_SCAN_OPT_MAX_VISIT;
        profile->values.max_visit_ms = 1000;
        rc |= expect_true("hold row cap", dsd_channel_profile_set(state, 0, profile) == 0);
    }

    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("hold analog row on air", state->lcn_freq_roll == 1 && opts->analog_only == 1
                                                    && opts->scan_max_visit_ms == 1000
                                                    && opts->analog_demod == dsd_scan_mode_analog_kind(mode));
    rc |= expect_true("voice gate never owns the analog row", !dsd_scan_voice_gate_owns_step(opts, state));

    /* Carrier keeps arriving on a visit already 5 s old, past -t: the row stays. The cap is out of the way here. */
    opts->scan_max_visit_ms = 0;
    const int tunes_before = g_rtl_tune_calls;
    for (int pass = 0; pass < 4; pass++) {
        stamp_monitor_carrier(state);
        seed_visit_anchor(state, dsd_decode_now_mono_s() - 5.0);
        dsd_engine_scan_visit_tick(opts, state, dsd_decode_now_mono_s());
        noCarrier(opts, state);
    }
    rc |= expect_true("carrier holds the analog row past -t",
                      state->lcn_freq_roll == 1 && g_rtl_tune_calls == tunes_before);

    /* The carrier stops: -t after the last stamp the rotation moves on. */
    state->last_cc_sync_time = time(NULL) - 2;
    dsd_engine_scan_visit_tick(opts, state, dsd_decode_now_mono_s());
    noCarrier(opts, state);
    rc |= expect_true("analog row steps -t after the carrier",
                      state->lcn_freq_roll == 2 && g_rtl_tune_freq == 461500000U);

    /* Back on the analog row with its --scan-max-visit-ms 1000 in force: a carrier that never drops still ends a visit
       that has lasted past the cap. The voice gate is off, so the DMR row between does not wait out its window. */
    opts->scan_voice_only = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("analog row again", state->lcn_freq_roll == 1 && opts->scan_max_visit_ms == 1000);
    stamp_monitor_carrier(state);
    seed_visit_anchor(state, dsd_decode_now_mono_s() - 5.0);
    noCarrier(opts, state);
    rc |= expect_true("visit cap ends a carrier-held analog row",
                      state->lcn_freq_roll == 2 && g_rtl_tune_freq == 461500000U);

    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    if (rc) {
        DSD_FPRINTF(stderr, "%s row carrier hold failed\n", dsd_scan_mode_name(mode));
    }
    return rc;
}

static int
test_typed_scan_analog_rows_hold_on_carrier(void) {
    return typed_scan_analog_row_holds_on_carrier(DSD_SCAN_MODE_NFM)
           | typed_scan_analog_row_holds_on_carrier(DSD_SCAN_MODE_AM);
}

/* Issue #527: a typed nfm row with its own tone policy (--tone-allow 100.0) on a digital-configured -Y list. The row's
 * policy goes on air with the row; a carrier it is still checking holds the row, as does one it lets through; an
 * operator hold keeps the row, muted, when the traffic turns out to carry another tone; and once released, rejected
 * traffic steps the row at the next pass though -t has long to run. Departure puts the configured policy (off) back. */
static int
test_typed_scan_nfm_row_tone_rejection_steps(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 30;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 154645000L;
    state->trunk_lcn_freq[1] = 461525000L;
    rc |= expect_true("tone nfm row", dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_NFM) == 0);
    rc |= expect_true("tone dmr row", dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_DMR) == 0);
    dsd_scan_row_profile* profile = NULL;
    rc |= expect_true("tone row profile", dsd_scan_profile_ensure(&profile) == 0 && profile);
    if (profile) {
        profile->values.present = DSD_SCAN_OPT_TONE;
        profile->values.tone_filter = DSD_TONE_FILTER_ALLOW;
        rc |= expect_true("tone row list", dsd_tone_set_parse("100.0", &profile->values.tone_set, NULL, 0) == 0);
        rc |= expect_true("tone row profile set", dsd_channel_profile_set(state, 0, profile) == 0);
    }

    state->last_cc_sync_time = time(NULL) - 40;
    noCarrier(opts, state);
    rc |= expect_true("tone row on air", state->lcn_freq_roll == 1 && opts->analog_only == 1
                                             && opts->analog_tone_filter == DSD_TONE_FILTER_ALLOW);
    state->analog_rx.carrier_open = 1;
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_PENDING;
    noCarrier(opts, state);
    rc |= expect_true("tone check holds the row", state->lcn_freq_roll == 1);
    /* The check stamps no carrier activity: with -t long run out since the row landed, it still holds the row. */
    state->last_cc_sync_time = time(NULL) - 40;
    noCarrier(opts, state);
    rc |= expect_true("tone check holds the row past -t", state->lcn_freq_roll == 1 && g_rtl_tune_freq == 154645000U);
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_ALLOWED;
    stamp_monitor_carrier(state);
    noCarrier(opts, state);
    rc |= expect_true("allowed traffic holds the row", state->lcn_freq_roll == 1 && g_rtl_tune_freq == 154645000U);
    /* The allowed traffic turns out to carry another tone, under the operator's hold: muted, and the row stays. */
    state->lcn_scan_hold = 1;
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    noCarrier(opts, state);
    noCarrier(opts, state);
    rc |= expect_true("a hold keeps the rejected row", state->lcn_freq_roll == 1 && g_rtl_tune_freq == 154645000U
                                                           && opts->analog_tone_filter == DSD_TONE_FILTER_ALLOW);
    state->lcn_scan_hold = 0;
    noCarrier(opts, state);
    rc |= expect_true("rejection steps the row", state->lcn_freq_roll == 2 && g_rtl_tune_freq == 461525000U);
    rc |= expect_true("departure restores the policy",
                      opts->analog_tone_filter == DSD_TONE_FILTER_OFF && opts->analog_only == 0);

    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

/* Issue #527: a row skipped at every visit for a width the DSP rate cannot filter is nowhere to go either. Rejected
 * traffic on the one row the scanner can tune stays muted where it is, pass after pass, instead of stepping onto the
 * refused row, failing there, and landing back on the same row to end the reception and judge it again. The row on
 * air is where the receiver landed: a start that failed on the refused row just before the traffic came does not make
 * the list look as if it had somewhere else to go. */
static int
test_typed_scan_tone_rejection_with_a_refused_row(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    g_rtl_request_rate_hz = 16000;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 154645000L;
    state->trunk_lcn_freq[1] = 155645000L;
    for (int row = 0; row < 2; row++) {
        rc |= expect_true("refused-row mode", dsd_channel_mode_set(state, (size_t)row, DSD_SCAN_MODE_NFM) == 0);
        dsd_scan_row_profile* profile = NULL;
        rc |= expect_true("refused-row profile", dsd_scan_profile_ensure(&profile) == 0 && profile);
        if (!profile) {
            continue;
        }
        if (row == 0) {
            profile->values.present = DSD_SCAN_OPT_TONE;
            profile->values.tone_filter = DSD_TONE_FILTER_ALLOW;
            rc |= expect_true("refused-row list", dsd_tone_set_parse("100.0", &profile->values.tone_set, NULL, 0) == 0);
        } else {
            profile->values.present = DSD_SCAN_OPT_BANDWIDTH;
            profile->values.channel_bw_hz = 20000; /* does not fit the 16 kHz DSP rate */
        }
        rc |= expect_true("refused-row profile set", dsd_channel_profile_set(state, (size_t)row, profile) == 0);
    }

    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("refused-row: the tunable row on air", state->lcn_freq_roll == 1 && g_rtl_tune_freq == 154645000U
                                                                 && opts->analog_tone_filter == DSD_TONE_FILTER_ALLOW);
    /* The idle row's hangtime runs out: the step tries the refused row, which fails before any backend moves. */
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |=
        expect_true("refused-row: the refused row failed", state->lcn_freq_roll == 2 && g_rtl_tune_freq == 154645000U);

    /* Traffic comes on the row on air, and the policy rejects it: it stays, muted, pass after pass, -t long run out. */
    state->analog_rx.carrier_open = 1;
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    const uint32_t generation = state->analog_rx.generation;
    const int tunes_before = g_rtl_tune_calls;
    for (int pass = 0; pass < 4; pass++) {
        noCarrier(opts, state);
    }
    rc |= expect_true("refused-row: rejected traffic stays",
                      g_rtl_tune_calls == tunes_before && g_rtl_tune_freq == 154645000U && state->lcn_freq_roll == 2);
    rc |= expect_true("refused-row: the reception is kept", state->analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED
                                                                && state->analog_rx.generation == generation);
    if (state->analog_rx.generation != generation || g_rtl_tune_calls != tunes_before) {
        DSD_FPRINTF(stderr, "  generation %u -> %u, tunes %d -> %d, roll %d\n", generation, state->analog_rx.generation,
                    tunes_before, g_rtl_tune_calls, state->lcn_freq_roll);
    }

    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

/* Issue #527 beside #526: an am row is weighed as a place for rejected traffic to go by the AM width it runs, never the
 * NFM one. The configured --am-bandwidth-hz is 20000 and the configured NFM width unset. @p own_am_hz 0: the am row
 * sets no width, so it runs the configured 20 kHz, which the 16 kHz DSP rate cannot filter, where an nfm row would run
 * the unset NFM default, which is never refused; rejected traffic on the nfm row stays there, muted, until a 48 kHz
 * rate fits the am row, and then steps to it. @p own_am_hz 5000: the am row's own AM width, which the 16 kHz rate
 * filters and which is below the NFM range; rejected traffic steps to it at the first pass. */
static int
typed_scan_tone_rejection_weighs_an_am_row(int own_am_hz) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 30;
    opts->analog_nfm_bandwidth_hz = 0;
    opts->analog_am_bandwidth_hz = 20000;
    state->rtl_ctx = (RtlSdrContext*)state;
    g_rtl_request_rate_hz = 16000;
    g_rtl_watch_freq = 118300000U;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 154645000L;
    state->trunk_lcn_freq[1] = 118300000L;
    rc |= expect_true("am-alternate nfm row", dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_NFM) == 0);
    rc |= expect_true("am-alternate am row", dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_AM) == 0);
    dsd_scan_row_profile* profile = NULL;
    rc |= expect_true("am-alternate tone profile", dsd_scan_profile_ensure(&profile) == 0 && profile);
    if (profile) {
        profile->values.present = DSD_SCAN_OPT_TONE;
        profile->values.tone_filter = DSD_TONE_FILTER_ALLOW;
        rc |= expect_true("am-alternate list", dsd_tone_set_parse("100.0", &profile->values.tone_set, NULL, 0) == 0);
        rc |= expect_true("am-alternate tone profile set", dsd_channel_profile_set(state, 0, profile) == 0);
    }
    if (own_am_hz > 0) {
        profile = NULL;
        rc |= expect_true("am-alternate width profile", dsd_scan_profile_ensure(&profile) == 0 && profile);
        if (profile) {
            profile->values.present = DSD_SCAN_OPT_BANDWIDTH;
            profile->values.channel_bw_hz = own_am_hz;
            profile->values.channel_bw_kind = DSD_ANALOG_DEMOD_AM;
            rc |= expect_true("am-alternate width profile set", dsd_channel_profile_set(state, 1, profile) == 0);
        }
    }

    state->last_cc_sync_time = time(NULL) - 40;
    noCarrier(opts, state);
    rc |= expect_true("am-alternate: the nfm row on air", state->lcn_freq_roll == 1 && g_rtl_tune_freq == 154645000U
                                                              && opts->analog_tone_filter == DSD_TONE_FILTER_ALLOW);

    state->analog_rx.carrier_open = 1;
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    const uint32_t generation = state->analog_rx.generation;
    const int tunes_before = g_rtl_tune_calls;
    if (own_am_hz == 0) {
        for (int pass = 0; pass < 4; pass++) {
            noCarrier(opts, state);
        }
        rc |= expect_true("am-alternate: rejected traffic stays beside a refused am row",
                          g_rtl_tune_calls == tunes_before && g_rtl_watch_tunes == 0 && state->lcn_freq_roll == 1
                              && state->analog_rx.generation == generation);
        /* A DSP rate that fits the configured AM width makes the am row somewhere to go. */
        g_rtl_request_rate_hz = 48000;
    }
    noCarrier(opts, state);
    rc |= expect_true("am-alternate: rejected traffic steps to the am row",
                      state->lcn_freq_roll == 2 && g_rtl_watch_tunes == 1 && g_rtl_tune_freq == 118300000U);
    if (rc) {
        DSD_FPRINTF(stderr, "  am row width %d at %d Hz: generation %u -> %u, tunes %d -> %d, am tunes %d, roll %d\n",
                    own_am_hz, g_rtl_request_rate_hz, generation, state->analog_rx.generation, tunes_before,
                    g_rtl_tune_calls, g_rtl_watch_tunes, state->lcn_freq_roll);
    }

    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

static int
test_typed_scan_tone_rejection_weighs_an_am_row(void) {
    return typed_scan_tone_rejection_weighs_an_am_row(0) | typed_scan_tone_rejection_weighs_an_am_row(5000);
}

/* Row @p row of a typed -Y list declaring @p mode, with its own tone policy on @p list unless @p tone_filter is -1. */
static int
set_tone_scan_row(dsd_state* state, int row, dsd_scan_mode mode, int tone_filter, const char* list) {
    int rc = expect_true("tone scan row mode", dsd_channel_mode_set(state, (size_t)row, mode) == 0);
    if (tone_filter < 0) {
        return rc;
    }
    dsd_scan_row_profile* profile = NULL;
    rc |= expect_true("tone scan row profile", dsd_scan_profile_ensure(&profile) == 0 && profile);
    if (!profile) {
        return 1;
    }
    profile->values.present = DSD_SCAN_OPT_TONE;
    profile->values.tone_filter = tone_filter;
    rc |= expect_true("tone scan row list", dsd_tone_set_parse(list, &profile->values.tone_set, NULL, 0) == 0);
    rc |= expect_true("tone scan row profile set", dsd_channel_profile_set(state, (size_t)row, profile) == 0);
    return rc;
}

/* What the tap publishes for traffic the tone policy rejected: the CTCSS tone the detectors hold locked (@p tenths),
 * or, for 0, no tone found within the window. */
static void
seed_tone_rejection(dsd_state* state, int tenths) {
    state->analog_rx.carrier_open = 1;
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    state->analog_rx.tone_state = tenths > 0 ? DSD_ANALOG_TONE_STATE_LOCKED : DSD_ANALOG_TONE_STATE_NONE;
    state->analog_rx.tone_kind = tenths > 0 ? DSD_ANALOG_TONE_KIND_CTCSS : DSD_ANALOG_TONE_KIND_NONE;
    state->analog_rx.ctcss_tenths_hz = tenths;
    state->analog_rx.gate_no_tone = tenths > 0 ? 0 : 1;
}

/* One case of test_typed_scan_tone_rejection_weighs_same_frequency_rows(): row 0 an nfm row with --tone-allow 100.0,
 * row 1 on the same frequency declaring @p mode with its own @p tone_filter and @p list (-1: none), traffic rejected on
 * row 0 as seed_tone_rejection() publishes it for @p tenths. With @p want_step the next pass lands on row 1; otherwise
 * row 0 keeps the traffic, muted, pass after pass, the reception never ended, under "Carrier" with no timer. */
static int
typed_scan_same_frequency_case(const char* stage, dsd_scan_mode mode, int tone_filter, const char* list, int tenths,
                               int want_step) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 30;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 154430000L;
    state->trunk_lcn_freq[1] = 154430000L;
    rc |= set_tone_scan_row(state, 0, DSD_SCAN_MODE_NFM, DSD_TONE_FILTER_ALLOW, "100.0");
    rc |= set_tone_scan_row(state, 1, mode, tone_filter, list);

    state->last_cc_sync_time = time(NULL) - 40;
    noCarrier(opts, state);
    rc |= expect_true("same-frequency: the allow-100.0 row on air",
                      state->lcn_freq_roll == 1 && opts->analog_only == 1
                          && opts->analog_tone_filter == DSD_TONE_FILTER_ALLOW
                          && dsd_tone_set_contains_ctcss(&opts->analog_tone_set, 1000));
    seed_tone_rejection(state, tenths);
    const uint32_t generation = state->analog_rx.generation;
    for (int pass = 0; pass < (want_step ? 1 : 4); pass++) {
        noCarrier(opts, state);
    }
    if (want_step) {
        rc |= expect_true("same-frequency: rejected traffic steps to the second row", state->lcn_freq_roll == 2);
    } else {
        rc |= expect_true("same-frequency: rejected traffic stays",
                          state->lcn_freq_roll == 1 && state->analog_rx.generation == generation
                              && state->analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED);
        dsd_engine_scan_y_timing_tick(opts, state, dsd_decode_now_mono_s(), (double)time(NULL));
        rc |= expect_true("same-frequency: Carrier with no timer",
                          state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_CARRIER
                              && state->scan_timing.deadline_m < 0.0);
    }
    if (rc) {
        DSD_FPRINTF(stderr, "  %s: roll %d, generation %u -> %u, reason %u, deadline %.3f\n", stage,
                    state->lcn_freq_roll, generation, state->analog_rx.generation, (unsigned)state->scan_timing.reason,
                    state->scan_timing.deadline_m);
    }

    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}

/* Issue #527: a typed -Y list may list one frequency on several rows, each with its own class and options. A row on the
 * frequency on air is somewhere to take rejected traffic when a step to it would judge that traffic otherwise: a row of
 * another class (a mixed-mode repeater's dmr row, an am row, a row inheriting the digital decode mode), which runs no
 * tone check, or an nfm row whose policy -- its own, or the configured one (off here) -- passes what the verdict rests
 * on: the tone locked, or no tone at all. Fire and EMS sharing a repeater, each with its own allow list: EMS traffic
 * rejected on the fire row moves to the EMS row. A row that would reject the traffic too -- the same policy, or another
 * that rejects this tone as well -- is nowhere to go, since a step would land there, end the reception, reject the
 * same traffic and step back, for as long as it lasted. */
static int
test_typed_scan_tone_rejection_weighs_same_frequency_rows(void) {
    int rc = 0;
    rc |= typed_scan_same_frequency_case("a dmr row", DSD_SCAN_MODE_DMR, -1, NULL, 0, 1);
    rc |= typed_scan_same_frequency_case("an am row", DSD_SCAN_MODE_AM, -1, NULL, 0, 1);
    rc |= typed_scan_same_frequency_case("a row inheriting the digital mode", DSD_SCAN_MODE_INHERIT, -1, NULL, 0, 1);
    rc |= typed_scan_same_frequency_case("an nfm row allowing the tone", DSD_SCAN_MODE_NFM, DSD_TONE_FILTER_ALLOW,
                                         "131.8", 1318, 1);
    rc |= typed_scan_same_frequency_case("an nfm row rejecting the tone too", DSD_SCAN_MODE_NFM, DSD_TONE_FILTER_ALLOW,
                                         "131.8", 1035, 0);
    rc |= typed_scan_same_frequency_case("an nfm row with the same policy", DSD_SCAN_MODE_NFM, DSD_TONE_FILTER_ALLOW,
                                         "100.0", 0, 0);
    rc |= typed_scan_same_frequency_case("an nfm row blocking only other tones", DSD_SCAN_MODE_NFM,
                                         DSD_TONE_FILTER_BLOCK, "67.0", 0, 1);
    rc |= typed_scan_same_frequency_case("an nfm row blocking the tone", DSD_SCAN_MODE_NFM, DSD_TONE_FILTER_BLOCK,
                                         "103.5", 1035, 0);
    rc |= typed_scan_same_frequency_case("an nfm row running the configured policy", DSD_SCAN_MODE_NFM, -1, NULL, 0, 1);
    return rc;
}

/* Issue #527: a code is one DCS signal under either of its spellings, so a same-frequency row whose own list only
 * respells the policy in force (D047N for D023I) runs that policy and would reject the same traffic: it is nowhere to
 * go, even once the value the rejection rested on is lost (no locked code, no "no tone" verdict to weigh). A list of
 * the code's other polarity is another policy, which may pass the traffic, so that row is somewhere to go. */
static int
test_typed_scan_respelled_policy_rejects_alike(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    opts->scanner_mode = 1;
    opts->analog_only = 1;
    opts->analog_tone_filter = DSD_TONE_FILTER_BLOCK;
    rc |= expect_true("respelled: the policy in force",
                      dsd_tone_set_parse("100.0/D023I", &opts->analog_tone_set, NULL, 0) == 0);
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 1; /* row 0 is on air */
    state->trunk_lcn_freq[0] = 154430000L;
    state->trunk_lcn_freq[1] = 154430000L;
    rc |= set_tone_scan_row(state, 0, DSD_SCAN_MODE_NFM, DSD_TONE_FILTER_BLOCK, "100.0/D023I");
    rc |= set_tone_scan_row(state, 1, DSD_SCAN_MODE_NFM, DSD_TONE_FILTER_BLOCK, "D047N/100");
    state->analog_rx.carrier_open = 1;
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    rc |= expect_true("respelled: the same policy is nowhere to go",
                      dsd_engine_channel_scan_has_other_row(opts, state) == 0);
    rc |= set_tone_scan_row(state, 1, DSD_SCAN_MODE_NFM, DSD_TONE_FILTER_BLOCK, "D047I/100");
    rc |= expect_true("respelled: another polarity is somewhere to go",
                      dsd_engine_channel_scan_has_other_row(opts, state) == 1);
    free_test_runtime(opts, state);
    return rc;
}

/* Issue #527 beside #526: an am row runs the AM monitor, which hears no CTCSS or DCS, so the configured tone policy
 * (--tone-allow 100.0 here) judges nothing there. A verdict still published (a rejection an nfm row left behind) is not
 * in force: the row's carrier holds it under "Carrier", never "Tone check", and nothing steps it; the configured policy
 * stays as it was. */
static int
test_typed_scan_am_row_ignores_the_tone_filter(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 30;
    opts->analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    rc |= expect_true("am tone list", dsd_tone_set_parse("100.0", &opts->analog_tone_set, NULL, 0) == 0);
    state->rtl_ctx = (RtlSdrContext*)state;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 118300000L;
    state->trunk_lcn_freq[1] = 461525000L;
    rc |= expect_true("am tone row", dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_AM) == 0);
    rc |= expect_true("am tone dmr row", dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_DMR) == 0);

    state->last_cc_sync_time = time(NULL) - 40;
    noCarrier(opts, state);
    rc |= expect_true("am row on air", state->lcn_freq_roll == 1 && opts->analog_only == 1
                                           && opts->analog_demod == DSD_ANALOG_DEMOD_AM
                                           && opts->analog_tone_filter == DSD_TONE_FILTER_ALLOW);
    for (int pass = 0; pass < 3; pass++) {
        state->analog_rx.carrier_open = 1;
        state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
        stamp_monitor_carrier(state);
        rc |= expect_true("no verdict in force on the AM monitor",
                          dsd_scan_analog_carrier_open(opts, state)
                              && dsd_scan_analog_tone_gate(opts, state) == DSD_ANALOG_TONE_GATE_OFF);
        noCarrier(opts, state);
    }
    rc |= expect_true("the carrier holds the am row", state->lcn_freq_roll == 1 && g_rtl_tune_freq == 118300000U);
    dsd_engine_scan_y_timing_tick(opts, state, dsd_decode_now_mono_s(), (double)time(NULL));
    rc |= expect_true("am row reads Carrier", state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_CARRIER);

    dsd_engine_channel_scan_leave(opts, state);
    rc |= expect_true("configured policy kept", opts->analog_tone_filter == DSD_TONE_FILTER_ALLOW
                                                    && dsd_tone_set_contains_ctcss(&opts->analog_tone_set, 1000));
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}
#endif

#ifdef DSD_NEO_TEST_RTL_WRAP
/* A tone the analog tap published before the step. Every case that uses these helpers needs
   the RTL wrap, so they share its guard; without it they would be unused functions. */
static void
seed_rx_tone_publication(dsd_state* state, int tenths) {
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = tenths;
}

static int
rx_tone_publication_cleared(const dsd_state* state, uint32_t seeded_generation) {
    return state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE
           && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE && state->analog_rx.ctcss_tenths_hz == 0
           && state->analog_rx.carrier_open == 0 && state->analog_rx.generation != seeded_generation;
}

/* The legacy -Y step by rigctl on PCM input, the one radio-off builds have: a hop that lands
   clears the received tone (issue #522), and a refused one leaves the receiver -- and so its
   tone -- where they were. */
static int
test_rx_tone_rigctl_scan_step(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->setmod_bw = 0;
    opts->trunk_hangtime = 1;
    // Frequencies no other case uses: engine.c caches the last rigctl tune across cases.
    state->trunk_lcn_freq[0] = 951012500;
    state->trunk_lcn_freq[1] = 952012500;
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 0;
    seed_rx_tone_publication(state, 1318);
    const uint32_t generation = state->analog_rx.generation;

    g_rigctl_setfreq_ok = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rigctl-step-refused-keeps-rx-tone",
                      state->lcn_freq_roll == 0 && state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                          && state->analog_rx.ctcss_tenths_hz == 1318 && state->analog_rx.generation == generation);

    g_rigctl_setfreq_ok = 1;
    g_rigctl_setfreq_calls = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rigctl-step-moved",
                      g_rigctl_setfreq_calls > 0 && g_rigctl_setfreq_freq == 951012500 && state->lcn_freq_roll == 1);
    rc |= expect_true("rigctl-step-clears-rx-tone", rx_tone_publication_cleared(state, generation));
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}

/* Issue #589: the legacy rigctl leg (the untyped -Y step here, and the direct control-channel return) skips a
   frequency and a -B it sent last. What it sent describes one connection: once rigctl reconnects, the new
   connection's peer -- another one, or the same one restarted -- was sent nothing, so both go out again. */
static int
test_rigctl_reconnect_forgets_the_legacy_tune_cache(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->setmod_bw = 12500;
    opts->trunk_hangtime = 1;
    // A frequency no other case uses: engine.c caches the last rigctl tune across cases.
    state->trunk_lcn_freq[0] = 953012500;
    state->lcn_freq_count = 1;
    state->lcn_freq_roll = 0;
    g_rigctl_setfreq_ok = 1;
    g_rigctl_setfreq_calls = 0;
    g_rigctl_setmod_calls = 0;

    for (int i = 0; i < 2; i++) {
        state->last_cc_sync_time = time(NULL) - 11;
        noCarrier(opts, state);
    }
    rc |= expect_true("rigctl-step-suppresses-a-repeat-on-one-connection",
                      g_rigctl_setfreq_calls == 1 && g_rigctl_setmod_calls == 1);

    dsd_engine_rigctl_tune_cache_forget();
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rigctl-step-after-a-reconnect-sends-both-again",
                      g_rigctl_setfreq_calls == 2 && g_rigctl_setfreq_freq == 953012500 && g_rigctl_setmod_calls == 2
                          && g_rigctl_setmod_bw == 12500);
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}
#endif

#ifdef DSD_NEO_TEST_RTL_WRAP
/* What the tap publishes for an analog carrier under a tone policy (issue #527): the carrier and the verdict. */
static void
seed_tone_gate(dsd_state* state, int gate) {
    state->analog_rx.carrier_open = 1;
    state->analog_rx.gate = gate;
}

/*
 * The legacy untyped -Y (a -fA channel map with no mode column), the analog scanning people run today, stepped by
 * rigctl on PCM input, the path radio-off builds have (issue #527). A carrier the tone policy is still checking holds
 * the row while it lasts, under "Tone check", and stamps nothing; traffic it rejected moves on at the next noCarrier
 * pass, though the hangtime has long to run; the operator's hold keeps the row, muted, until it is released; and with
 * no detection running (the -8 monitor under digital decoding) no verdict is in force to step on.
 */
static int
test_tone_rejection_steps_the_legacy_scan(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->setmod_bw = 0;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->trunk_hangtime = 30;
    // Frequencies no other case uses: engine.c caches the last rigctl tune across cases.
    state->trunk_lcn_freq[0] = 955012500;
    state->trunk_lcn_freq[1] = 956012500;
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 0;
    g_rigctl_setfreq_ok = 1;

    seed_tone_gate(state, DSD_ANALOG_TONE_GATE_PENDING);
    state->last_cc_sync_time = time(NULL);
    noCarrier(opts, state);
    rc |= expect_true("tone-check-holds", state->lcn_freq_roll == 0);
    dsd_engine_scan_y_timing_tick(opts, state, dsd_decode_now_mono_s(), (double)time(NULL));
    rc |= expect_true("tone-check-reason", state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_TONE_PENDING);
    /* The check holds the row by its verdict alone, since it stamps no hangtime anchor: a pass with -t long run out
       keeps the row too. */
    state->last_cc_sync_time = time(NULL) - 40;
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("tone-check-holds-past-t", g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 0);
    seed_tone_gate(state, DSD_ANALOG_TONE_GATE_ALLOWED);
    dsd_engine_scan_y_timing_tick(opts, state, dsd_decode_now_mono_s(), (double)time(NULL));
    rc |= expect_true("allowed-carrier-reason", state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_CARRIER);

    seed_tone_gate(state, DSD_ANALOG_TONE_GATE_REJECTED);
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("rejected-steps",
                      g_rigctl_setfreq_calls > 0 && g_rigctl_setfreq_freq == 955012500 && state->lcn_freq_roll == 1);
    /* The step ended the reception: nothing is left for the next row to step on. */
    rc |= expect_true("step-clears-verdict", state->analog_rx.gate == DSD_ANALOG_TONE_GATE_OFF);

    seed_tone_gate(state, DSD_ANALOG_TONE_GATE_REJECTED);
    state->last_cc_sync_time = time(NULL);
    state->lcn_scan_hold = 1;
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("hold-keeps-rejected-row", g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 1);
    state->lcn_scan_hold = 0;
    noCarrier(opts, state);
    rc |= expect_true("release-steps", g_rigctl_setfreq_calls > 0 && state->lcn_freq_roll == 2);

    /* No detection, no verdict in force: a published REJECTED from an earlier session steps nothing. */
    opts->analog_only = 0;
    seed_tone_gate(state, DSD_ANALOG_TONE_GATE_REJECTED);
    state->last_cc_sync_time = time(NULL);
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("no-detection-no-step", g_rigctl_setfreq_calls == 0);
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}
#endif

#ifdef DSD_NEO_TEST_RTL_WRAP
/*
 * A -Y list with nowhere else to go keeps rejected traffic muted where it is, as a fixed frequency does (issue #527):
 * a one-row list, or one whose other rows are avoided, neither steps at the next pass nor once -t has run out, so the
 * reception is not ended and judged again every second. A second usable row moves it on as usual.
 */
static int
test_tone_rejection_stays_on_a_single_row(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->setmod_bw = 0;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->trunk_hangtime = 1;
    // Frequencies no other case uses: engine.c caches the last rigctl tune across cases.
    state->trunk_lcn_freq[0] = 957012500;
    state->trunk_lcn_freq[1] = 958012500;
    state->lcn_freq_count = 1;
    state->lcn_freq_roll = 1; /* row 0 is on air */
    g_rigctl_setfreq_ok = 1;

    seed_tone_gate(state, DSD_ANALOG_TONE_GATE_REJECTED);
    const uint32_t generation = state->analog_rx.generation;
    state->last_cc_sync_time = time(NULL) - 40;
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    noCarrier(opts, state);
    rc |= expect_true("single-row-stays", g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 1);
    rc |= expect_true("single-row-keeps-reception", state->analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED
                                                        && state->analog_rx.generation == generation);
    /* It stays for as long as the carrier lasts: "Carrier", with no timer, rather than a hangtime countdown that the
       carrier no longer restarts and that never steps. */
    dsd_engine_scan_y_timing_tick(opts, state, dsd_decode_now_mono_s(), (double)time(NULL));
    rc |= expect_true("single-row-reads-carrier", state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_CARRIER
                                                      && state->scan_timing.deadline_m < 0.0);

    /* A second row on the same frequency runs the same mode and policy, as every row of a list without modes does: a
       step would land there and reject the same traffic again, so it is nowhere to go either. */
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[1] = 957012500;
    noCarrier(opts, state);
    rc |= expect_true("same-frequency-row-stays", g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 1
                                                      && state->analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED
                                                      && state->analog_rx.generation == generation);
    state->trunk_lcn_freq[1] = 958012500;

    /* A second row the operator avoided is nowhere to go either. */
    state->lcn_freq_count = 2;
    rc |= expect_true("single-row-avoid", dsd_state_trunk_lcn_avoid_set(state, 1U, 1) == 0);
    noCarrier(opts, state);
    rc |= expect_true("avoided-row-stays", g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 1
                                               && state->analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED);
    (void)dsd_state_trunk_lcn_avoid_clear(state);
    noCarrier(opts, state);
    rc |= expect_true("second-row-steps",
                      g_rigctl_setfreq_calls > 0 && g_rigctl_setfreq_freq == 958012500 && state->lcn_freq_roll == 2);
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}
#endif

/* The TEST decode clock at @p ms past a fixed origin (5000 s); the real clocks are left alone. */
static void
decode_clock_at_ms(uint64_t ms) {
    dsd_decode_clock_use_test((5000ULL * 1000ULL + ms) * 1000000ULL);
}

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
/*
 * The FSK no-sync reacquire watchdog is a decode decision, so it runs on the decode clock: a replay reaches the same
 * request at the same capture time however fast it is read. Driven through the TEST decode clock with no real time
 * passing: the gap that opened at decode +0 requests a reacquire at +11 s, not at +9 s, and the 0.75 s cooldown after
 * it is on the decode clock too: held at +11.5 s, the next request at +11.8 s.
 */
static int
test_fsk_reacquire_watchdog_runs_on_the_decode_clock(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    dsd_rtl_stream_metrics_hooks hooks = {.output_kind = fake_rtl_fsk_output_kind};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (struct RtlSdrContext*)state;
    decode_clock_at_ms(0U);
    const time_t t0 = dsd_decode_time();
    const double t0_m = dsd_decode_now_mono_s();
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = t0;
    state->last_cc_sync_time_m = t0_m;
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
    state->rtl_fsk_reacquire_last_sync_time = t0;
    state->rtl_fsk_reacquire_last_sync_m = t0_m;
    state->rtl_fsk_reacquire_gap_start_m = t0_m;
    state->rtl_fsk_reacquire_last_request_m = 0.0;
    g_rtl_fsk_reacquire_requests = 0;

    noCarrier(opts, state);
    rc |= expect_true("decode-clock fsk watchdog: none as the gap opens", g_rtl_fsk_reacquire_requests == 0);
    decode_clock_at_ms(9000U);
    noCarrier(opts, state);
    rc |= expect_true("decode-clock fsk watchdog: none at +9 s", g_rtl_fsk_reacquire_requests == 0);
    decode_clock_at_ms(11000U);
    noCarrier(opts, state);
    rc |= expect_true("decode-clock fsk watchdog: requests at +11 s",
                      g_rtl_fsk_reacquire_requests == 1
                          && fabs(state->rtl_fsk_reacquire_last_request_m - (t0_m + 11.0)) < 1e-6);
    decode_clock_at_ms(11500U);
    noCarrier(opts, state);
    rc |= expect_true("decode-clock fsk watchdog: cooldown holds at +11.5 s", g_rtl_fsk_reacquire_requests == 1);
    decode_clock_at_ms(11800U);
    noCarrier(opts, state);
    rc |= expect_true("decode-clock fsk watchdog: cooldown over at +11.8 s", g_rtl_fsk_reacquire_requests == 2);

    dsd_decode_clock_use_system();
    dsd_rtl_stream_metrics_hooks_set(NULL);
    free_test_runtime(opts, state);
    return rc;
}

/*
 * Issue #572: a replay left mid-run (an app-control restart or input switch, through
 * dsd_engine_decode_clock_leave_replay()) puts the decode clock back on the system clock without moving decode-mono
 * time back to the platform clock's origin, so the watchdog's stamps from the replay go on ageing. The gap that opened
 * at capture +0 requested a reacquire at +11 s on the replay's clock; right after the leave its 0.75 s cooldown still
 * holds, and 0.8 s of real time later it is over and the next request goes out. A clock that fell back to the
 * platform origin would leave both stamps ~1.8e9 s in the future, and the watchdog would never request again.
 */
static int
test_fsk_reacquire_cooldown_runs_across_a_replay_leave(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    dsd_rtl_stream_metrics_hooks hooks = {.output_kind = fake_rtl_fsk_output_kind};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (struct RtlSdrContext*)state;
    dsd_decode_clock_use_replay(1788245497LL); /* 2026-09-01T06:51:37Z */
    const double t0_m = dsd_decode_now_mono_s();
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = t0_m;
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
    /* Ahead of every wall stamp the leave's rebase writes (2100-01-01), so the restamped sync stamps do not read as a
       new sync that restarts the gap: this case is about the cooldown. */
    state->rtl_fsk_reacquire_last_sync_time = (time_t)4102444800LL;
    state->rtl_fsk_reacquire_last_sync_m = t0_m;
    state->rtl_fsk_reacquire_gap_start_m = t0_m;
    state->rtl_fsk_reacquire_last_request_m = 0.0;
    g_rtl_fsk_reacquire_requests = 0;

    noCarrier(opts, state);
    rc |= expect_true("replay leave fsk watchdog: none as the gap opens", g_rtl_fsk_reacquire_requests == 0);
    dsd_decode_clock_set_media_ns(11ULL * 1000000000ULL);
    noCarrier(opts, state);
    rc |= expect_true("replay leave fsk watchdog: requests at capture +11 s", g_rtl_fsk_reacquire_requests == 1);

    dsd_engine_decode_clock_leave_replay(opts, state);
    rc |= expect_true("replay leave fsk watchdog: the decode clock is the system's",
                      dsd_decode_clock_source() == DSD_DECODE_CLOCK_SYSTEM);
    const double since_request = dsd_decode_now_mono_s() - state->rtl_fsk_reacquire_last_request_m;
    rc |= expect_true("replay leave fsk watchdog: the request is not in the future", since_request >= 0.0);
    rc |= expect_true("replay leave fsk watchdog: and was just now", since_request < 0.5);
    noCarrier(opts, state);
    rc |= expect_true("replay leave fsk watchdog: cooldown holds right after the leave",
                      g_rtl_fsk_reacquire_requests == 1);
    dsd_sleep_ms(800U);
    noCarrier(opts, state);
    rc |= expect_true("replay leave fsk watchdog: cooldown over 0.8 s after the leave",
                      g_rtl_fsk_reacquire_requests == 2);

    dsd_decode_clock_use_system();
    dsd_rtl_stream_metrics_hooks_set(NULL);
    free_test_runtime(opts, state);
    return rc;
}
#endif

/* The manufacturer and branding a Hytera XPT control channel leaves on the state. */
static void
seed_xpt_identity(dsd_state* state) {
    state->dmr_mfid = 0x68;
    DSD_SNPRINTF(state->dmr_branding, sizeof state->dmr_branding, "%s", "  Hytera");
    DSD_SNPRINTF(state->dmr_branding_sub, sizeof state->dmr_branding_sub, "%s", "XPT ");
}

static int
xpt_identity_intact(const dsd_state* state) {
    return state->dmr_mfid == 0x68 && strcmp(state->dmr_branding, "  Hytera") == 0
           && strcmp(state->dmr_branding_sub, "XPT ") == 0;
}

static void
put_bits(uint8_t* bits, size_t at, uint32_t value, size_t width) {
    for (size_t i = 0; i < width; i++) {
        bits[at + i] = (uint8_t)((value >> (width - 1U - i)) & 1U);
    }
}

/*
 * Decode a standard group Preamble CSBK (opcode 61, feature set 0) to target 0x123456 from source 0x0ABCDE. Returns
 * the address width it printed: 24 as the standard reads it, 16 as an XPT site's short addresses read it (the low 16
 * bits of each: 0x3456, 0xBCDE), or 0 when neither line appeared.
 */
static int
preamble_csbk_address_bits(dsd_opts* opts, dsd_state* state) {
    uint8_t bits[256] = {0};
    uint8_t bytes[48] = {0};
    bytes[0] = 0x3DU;
    bytes[1] = 0x00U;
    put_bits(bits, 0U, bytes[0], 8U);
    put_bits(bits, 8U, bytes[1], 8U);
    bits[17] = 1U;
    put_bits(bits, 32U, 0x123456U, 24U);
    put_bits(bits, 56U, 0x0ABCDEU, 24U);
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "preamble-csbk") != 0) {
        return -1;
    }
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
    (void)dsd_test_capture_stderr_end(&cap);
    char log[4096];
    if (dsd_test_capture_stderr_read(&cap, log, sizeof log) != 0) {
        return -1;
    }
    if (strstr(log, "Source: 703710 - Target: 1193046") != NULL) {
        return 24;
    }
    if (strstr(log, "Source: 48350 - Target: 13398") != NULL) {
        return 16;
    }
    return 0;
}

/*
 * The DMR stale-follow clear is a decode decision: a control channel quiet for more than 10 s of decode time forgets
 * the manufacturer and branding it announced, so a replay forgets them at the same capture time however fast it is
 * read. Driven through the TEST decode clock with no real time passing, on a trunked setup that follows no voice
 * channel (nothing to preserve): the XPT identity stands at decode +10 s and is gone at +11 s, after which a standard
 * Preamble CSBK reads 24-bit addresses instead of XPT's 16-bit ones.
 */
static int
test_dmr_stale_follow_clear_runs_on_the_decode_clock(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 0;
    decode_clock_at_ms(0U);
    seed_xpt_identity(state);
    rc |=
        expect_true("stale-follow: an XPT site reads 16-bit addresses", preamble_csbk_address_bits(opts, state) == 16);
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;

    decode_clock_at_ms(10000U);
    noCarrier(opts, state);
    rc |= expect_true("stale-follow: +10 s keeps the XPT identity", xpt_identity_intact(state));
    decode_clock_at_ms(11000U);
    noCarrier(opts, state);
    rc |= expect_true("stale-follow: +11 s clears manufacturer and branding",
                      state->dmr_mfid == -1 && state->dmr_branding[0] == '\0' && state->dmr_branding_sub[0] == '\0');
    rc |= expect_true("stale-follow: then a Preamble CSBK reads 24-bit addresses",
                      preamble_csbk_address_bits(opts, state) == 24);

    dsd_decode_clock_use_system();
    free_test_runtime(opts, state);
    return rc;
}

/*
 * The conventional control for the case above: without trunking, noCarrier() drops the branding at every pass
 * whatever the clock says, while the manufacturer id still waits out more than 10 s of decode time since the last
 * control-channel sync.
 */
static int
test_dmr_mfid_clear_runs_on_the_decode_clock_without_trunking(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    opts->trunk_enable = 0;
    decode_clock_at_ms(0U);
    seed_xpt_identity(state);
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();

    noCarrier(opts, state);
    rc |= expect_true("conventional: branding cleared at every pass",
                      state->dmr_branding[0] == '\0' && state->dmr_branding_sub[0] == '\0');
    rc |= expect_true("conventional: manufacturer kept at +0 s", state->dmr_mfid == 0x68);
    decode_clock_at_ms(10000U);
    noCarrier(opts, state);
    rc |= expect_true("conventional: manufacturer kept at +10 s", state->dmr_mfid == 0x68);
    decode_clock_at_ms(11000U);
    noCarrier(opts, state);
    rc |= expect_true("conventional: manufacturer cleared at +11 s", state->dmr_mfid == -1);

    dsd_decode_clock_use_system();
    free_test_runtime(opts, state);
    return rc;
}

/*
 * The received tone (issue #522) has to survive noCarrier(): in analog mode it runs on every
 * no-sync pass, about every 375 ms, and a reset there would keep any tone from ever locking.
 * Locks one through the real tap, then checks the publication and the detector behind it
 * come out of repeated no-carrier passes untouched.
 */
static void
feed_rx_tone(dsd_opts* opts, dsd_state* state, int blocks) {
    static double phase = 0.0;
    float block[960];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 960; i++) {
            block[i] = (float)(3000.0 * cos(phase));
            phase += 2.0 * M_PI * 100.0 / 48000.0;
        }
        dsd_analog_rx_tap(opts, state, block, 960U);
    }
}

static int
test_rx_tone_survives_no_carrier(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    opts->scanner_mode = 0;
    opts->trunk_enable = 0;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->rtl_pwr = 1.0;
    opts->rtl_squelch_level = 0.0;
    feed_rx_tone(opts, state, 30);
    rc |= expect_true("rx-tone-locked-before-no-carrier", state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                                                              && state->analog_rx.ctcss_tenths_hz == 1000);
    const uint32_t generation = state->analog_rx.generation;
    const void* detector = dsd_state_ext_get_const(state, DSD_STATE_EXT_DSP_ANALOG_RX);
    for (int pass = 0; pass < 3; pass++) {
        noCarrier(opts, state);
        dsd_engine_reset_no_carrier_state(opts, state);
    }
    rc |= expect_true("rx-tone-survives-no-carrier", state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                                                         && state->analog_rx.ctcss_tenths_hz == 1000
                                                         && state->analog_rx.generation == generation);
    rc |= expect_true("rx-tone-detector-survives-no-carrier",
                      detector != NULL && dsd_state_ext_get_const(state, DSD_STATE_EXT_DSP_ANALOG_RX) == detector);
    /* And the next block carries on from the same lock rather than starting over. */
    feed_rx_tone(opts, state, 1);
    rc |= expect_true("rx-tone-continues-after-no-carrier", state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
                                                                && state->analog_rx.generation == generation);
    free_test_runtime(opts, state);
    return rc;
}

#ifdef DSD_NEO_TEST_RTL_WRAP
/* Feed 20 ms blocks of silence until the tap's carrier hangover has run out. */
static void
feed_rx_silence_until_closed(dsd_opts* opts, dsd_state* state) {
    const float silence[960] = {0};
    for (int b = 0; state->analog_rx.carrier_open && b < 20; b++) {
        dsd_analog_rx_tap(opts, state, silence, 960U);
    }
}

/* A -Y session on the legacy untyped list, rigctl on PCM input, with an allow list the 100.0 Hz tone feed_rx_tone()
   sends is not on: row 0 (@p row0_hz) on air, and @p rows rows in all. */
static void
setup_tone_rejection_ended(dsd_opts* opts, dsd_state* state, long row0_hz, int rows) {
    opts->scanner_mode = 1;
    opts->trunk_enable = 0;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->setmod_bw = 0;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->rtl_pwr = 1.0;
    opts->rtl_squelch_level = 0.0;
    opts->trunk_hangtime = 30;
    opts->analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    (void)dsd_tone_set_parse("67.0", &opts->analog_tone_set, NULL, 0);
    state->trunk_lcn_freq[0] = row0_hz;
    state->trunk_lcn_freq[1] = row0_hz + 1000000L;
    state->lcn_freq_count = rows;
    state->lcn_freq_roll = 1; /* row 0 is on air */
}

/* Rejected traffic that ended, as the tap leaves it: the policy rejected the tone once it locked, and the carrier's
   hangover ran out before the next pass. The hangtime anchor is fresh, as the row's landing or earlier traffic the
   policy passed leaves it; the check itself stamps none. */
static int
seed_tone_rejection_ended(dsd_opts* opts, dsd_state* state) {
    feed_rx_tone(opts, state, 30);
    int rc = expect_true("ended: rejected on air", state->analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED);
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    feed_rx_silence_until_closed(opts, state);
    rc |= expect_true("ended: carrier gone, rejection kept for the scanner",
                      state->analog_rx.carrier_open == 0 && state->analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING
                          && state->analog_rx.gate_rejected_ended == 1
                          && dsd_scan_analog_tone_rejection_ended(opts, state) == 1);
    return rc;
}

/*
 * Rejected traffic that ends between two noCarrier passes (issue #527), about 375 ms apart, holds the row no more than
 * traffic still on air would: the next pass moves on though the hangtime has 30 s to run, and an operator hold keeps
 * the row until it is released. A carrier that opens before that pass is a new transmission,
 * checked afresh, which holds the row. With nowhere else to go the hangtime decides, as after any carrier.
 */
static int
test_tone_rejection_that_ended_steps_the_legacy_scan(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    // Frequencies no other case uses: engine.c caches the last rigctl tune across cases.
    setup_tone_rejection_ended(opts, state, 961012500L, 2);
    g_rigctl_setfreq_ok = 1;
    rc |= seed_tone_rejection_ended(opts, state);
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("ended: the next pass steps",
                      g_rigctl_setfreq_calls > 0 && g_rigctl_setfreq_freq == 962012500 && state->lcn_freq_roll == 2);
    rc |= expect_true("ended: the step forgets it", state->analog_rx.gate_rejected_ended == 0
                                                        && dsd_scan_analog_tone_rejection_ended(opts, state) == 0);
    free_test_runtime(opts, state);

    /* A new carrier before the pass: checked again, it holds the row. */
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    setup_tone_rejection_ended(opts, state, 963012500L, 2);
    rc |= seed_tone_rejection_ended(opts, state);
    feed_rx_tone(opts, state, 1);
    rc |= expect_true("ended: a new carrier is checked", state->analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING
                                                             && state->analog_rx.gate_rejected_ended == 0);
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("ended: the new carrier holds", g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 1);
    free_test_runtime(opts, state);

    /* The operator's hold keeps the row; the release lets the next pass move on. */
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    setup_tone_rejection_ended(opts, state, 965012500L, 2);
    rc |= seed_tone_rejection_ended(opts, state);
    state->lcn_scan_hold = 1;
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("ended: the hold keeps the row", g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 1);
    state->lcn_scan_hold = 0;
    noCarrier(opts, state);
    rc |= expect_true("ended: the release steps", g_rigctl_setfreq_calls > 0 && state->lcn_freq_roll == 2);
    free_test_runtime(opts, state);

    /* A single row has nowhere else to go: the hangtime runs as after any carrier. */
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    setup_tone_rejection_ended(opts, state, 967012500L, 1);
    rc |= seed_tone_rejection_ended(opts, state);
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("ended: a single row waits out the hangtime",
                      g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 1);
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}

/* Offer the tap @p count samples of the listed 67.0 Hz tone at 2500 Hz one at a time, as the symbol path does while a
   monitor block fills, without finishing the block. */
static void
feed_low_rate_tone_partial(dsd_opts* opts, dsd_state* state, int count) {
    for (int i = 0; i < count; i++) {
        state->analog_out_f[i] = (float)(3000.0 * cos(2.0 * M_PI * 67.0 * (double)i / 2500.0));
        state->analog_sample_counter = i + 1;
        dsd_analog_rx_tap_partial(opts, state, state->analog_out_f, (unsigned int)(i + 1));
    }
}

/*
 * A carrier the tone policy is still checking holds the legacy -Y row until its verdict (issue #527), whatever the
 * hangtime anchor says: the check is no carrier activity and stamps nothing, and the tap publishes the carrier and its
 * pending verdict as the samples arrive, here 128 ms into a 960-sample monitor block that lasts 384 ms at 2500 Hz. A
 * pass while it runs, -t having run out since the last carrier, keeps the traffic on air instead of leaving it; the
 * per-visit cap still ends such a visit.
 */
static int
test_tone_check_holds_the_legacy_scan_while_the_verdict_is_pending(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    // Frequencies no other case uses: engine.c caches the last rigctl tune across cases.
    setup_tone_rejection_ended(opts, state, 969012500L, 2);
    opts->wav_sample_rate = 2500;
    opts->trunk_hangtime = 1;
    g_rigctl_setfreq_ok = 1;
    state->last_cc_sync_time = time(NULL) - 2;
    state->last_cc_sync_time_m = dsd_decode_now_mono_s() - 2.0;
    feed_low_rate_tone_partial(opts, state, 320);
    rc |= expect_true("low rate: carrier heard 128 ms in, still checked, no activity",
                      dsd_scan_analog_tone_gate(opts, state) == DSD_ANALOG_TONE_GATE_PENDING
                          && !dsd_scan_analog_carrier_open(opts, state));
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("low rate: the tone check holds the row past -t",
                      g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 1
                          && dsd_scan_analog_tone_gate(opts, state) == DSD_ANALOG_TONE_GATE_PENDING);
    dsd_engine_scan_y_timing_tick(opts, state, dsd_decode_now_mono_s(), (double)time(NULL));
    rc |= expect_true("low rate: Tone check, nothing counting down",
                      state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_TONE_PENDING
                          && state->scan_timing.deadline_m < 0.0);

    /* The per-visit cap outranks the check, as it does every other reason to stay: a visit 5 s old against 1 s. */
    opts->scan_max_visit_ms = 1000;
    dsd_scan_voice_gate_note_retune(state, dsd_decode_now_mono_s() - 5.0);
    state->scan_visit_roll_seen = state->lcn_freq_roll;
    noCarrier(opts, state);
    rc |= expect_true("low rate: the visit cap ends a tone check",
                      g_rigctl_setfreq_calls > 0 && g_rigctl_setfreq_freq == 970012500 && state->lcn_freq_roll == 2);
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}

/* Offer the tap the listed 100.0 Hz tone at 22050 Hz one sample at a time, as the symbol path fills its 960-sample
   monitor blocks (dsd_symbol.c symbol_process_unsynced_analog()): a partial read as each sample arrives and, as each
   block ends, the read of the rest and a fresh block, where the monitor stamps the -Y hangtime anchor for traffic the
   policy passes. Stops at the first sample after which the verdict is ALLOWED and returns its offset in the block being
   filled: 0 when that sample ended a block, whose end stamps the traffic, or when no verdict came within 1 s. */
static unsigned int
feed_tone_until_allowed_mid_block(dsd_opts* opts, dsd_state* state) {
    const unsigned int block = 960U;
    unsigned int filled = 0U;
    for (int n = 0; n < 22050; n++) {
        state->analog_out_f[filled++] = (float)(3000.0 * cos(2.0 * M_PI * 100.0 * (double)n / 22050.0));
        state->analog_sample_counter = (int)filled;
        dsd_analog_rx_tap_partial(opts, state, state->analog_out_f, filled);
        if (filled == block) {
            dsd_analog_rx_tap(opts, state, state->analog_out_f, block);
            if (state->analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED) {
                return 0U;
            }
            dsd_analog_rx_block_restart(state);
            state->analog_sample_counter = 0;
            filled = 0U;
        } else if (state->analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED) {
            return filled;
        }
    }
    return 0U;
}

/*
 * Traffic the tone policy has allowed holds the legacy -Y row while its carrier lasts (issue #527), whatever the
 * hangtime anchor says, as a check does. A check stamps no anchor, and the monitor stamps allowed traffic only as a
 * block ends, while the tap publishes its verdict at each 20 ms read: at 22050 Hz a 960-sample block spans two reads
 * and a remainder, so the verdict can turn ALLOWED part-way through a block with the anchor still where it was before
 * the check. A pass in that gap, -t having run out while the check held the row, keeps the traffic just allowed on air
 * instead of stepping off it. This case never ends that block; the stamp its end leaves is DSP_SYMBOL_REPLAY's.
 */
static int
test_allowed_traffic_holds_the_legacy_scan_before_its_block_ends(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    // Frequencies no other case uses: engine.c caches the last rigctl tune across cases.
    setup_tone_rejection_ended(opts, state, 975012500L, 2);
    rc |= expect_true("mid-block: allow 100.0", dsd_tone_set_parse("100.0", &opts->analog_tone_set, NULL, 0) == 0);
    opts->wav_sample_rate = 22050;
    opts->trunk_hangtime = 2;
    g_rigctl_setfreq_ok = 1;
    /* The row landed 3 s ago, and the check stamped nothing since: -t has run out. */
    state->last_cc_sync_time = time(NULL) - 3;
    state->last_cc_sync_time_m = dsd_decode_now_mono_s() - 3.0;
    const unsigned int offset = feed_tone_until_allowed_mid_block(opts, state);
    rc |= expect_true("mid-block: allowed part-way through a block",
                      offset > 0U && offset < 960U
                          && dsd_scan_analog_tone_gate(opts, state) == DSD_ANALOG_TONE_GATE_ALLOWED
                          && dsd_scan_analog_carrier_open(opts, state));
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("mid-block: the allowed traffic holds the row past -t",
                      g_rigctl_setfreq_calls == 0 && state->lcn_freq_roll == 1);
    dsd_engine_scan_y_timing_tick(opts, state, dsd_decode_now_mono_s(), (double)time(NULL));
    rc |= expect_true("mid-block: Carrier", state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_CARRIER);
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}

/* Feed @p blocks 20 ms blocks at 48 kHz of a tone at @p hz, or of silence (a closed squelch) for 0, through the tap.
   Nothing here stamps the -Y hangtime anchor: the monitor's stamp at each block's end is proven through the real
   monitor block by DSP_SYMBOL_REPLAY (only traffic the policy passes stamps), and the case below sets the anchor to
   what that stamp leaves. */
static void
feed_tap_blocks(dsd_opts* opts, dsd_state* state, int blocks, double hz) {
    static double phase = 0.0;
    float block[960];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 960; i++) {
            block[i] = hz > 0.0 ? (float)(3000.0 * cos(phase)) : 0.0f;
            phase = fmod(phase + (2.0 * M_PI * hz / 48000.0), 2.0 * M_PI);
        }
        dsd_analog_rx_tap(opts, state, block, 960U);
    }
}

/* Hold the decode clock (wall and monotonic alike) at @p second. */
static void
decode_clock_at_s(time_t second) {
    dsd_decode_clock_use_test((uint64_t)second * 1000000000ULL);
}

/* Run a -Y pass with the decode clock at @p wall; whether it stepped off the row on air. */
static int
legacy_scan_pass_steps_at(dsd_opts* opts, dsd_state* state, time_t wall) {
    decode_clock_at_s(wall);
    g_rigctl_setfreq_calls = 0;
    noCarrier(opts, state);
    return g_rigctl_setfreq_calls > 0;
}

/*
 * The -Y step rule against the hangtime anchor the monitor leaves (issue #527), which this case sets itself, on the
 * decode clock it holds (the rule compares whole decode seconds). A carrier the tone policy is still checking is
 * no activity, so the monitor stamps no anchor for it (DSP_SYMBOL_REPLAY): it holds the legacy -Y row while it lasts,
 * under "Tone check" with nothing counting down, and short no-tone bursts under an allow list -- kerchunks, noise --
 * each end before a verdict and leave the anchor where the row's landing put it. With a burst in every second of -t,
 * the pass at -t after the landing still holds and the pass a second later moves on: however many came, they cannot
 * park the scanner on a muted row. Traffic the policy allows is stamped as any carrier is, and leaves the ordinary -t
 * tail from its last block: the pass at -t after it holds, counting down the last second, and the next one steps.
 */
static int
test_tone_check_leaves_no_hangtime_tail(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    // Frequencies no other case uses: engine.c caches the last rigctl tune across cases.
    setup_tone_rejection_ended(opts, state, 971012500L, 2);
    rc |= expect_true("no tail: allow 100.0", dsd_tone_set_parse("100.0", &opts->analog_tone_set, NULL, 0) == 0);
    opts->trunk_hangtime = 2;
    g_rigctl_setfreq_ok = 1;
    decode_clock_at_ms(0U);
    const time_t landed = dsd_decode_time();
    state->last_cc_sync_time = landed;
    for (int burst = 0; burst <= 2; burst++) {
        const time_t now = landed + burst;
        decode_clock_at_s(now);
        feed_tap_blocks(opts, state, 15, 1000.0);
        rc |= expect_true("no tail: a burst is checked",
                          dsd_scan_analog_tone_gate(opts, state) == DSD_ANALOG_TONE_GATE_PENDING);
        rc |= expect_true("no tail: the check holds the row",
                          !legacy_scan_pass_steps_at(opts, state, now) && state->lcn_freq_roll == 1);
        dsd_engine_scan_y_timing_tick(opts, state, dsd_decode_now_mono_s(), (double)now);
        rc |= expect_true("no tail: Tone check, nothing counting down",
                          state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_TONE_PENDING
                              && state->scan_timing.deadline_m < 0.0);
        feed_tap_blocks(opts, state, 20, 0.0);
        rc |= expect_true("no tail: the burst ends unjudged",
                          state->analog_rx.carrier_open == 0 && state->analog_rx.gate_rejected_ended == 0);
        rc |= expect_true("no tail: -t since the landing still runs, the anchor where it was",
                          !legacy_scan_pass_steps_at(opts, state, now) && state->lcn_freq_roll == 1
                              && state->last_cc_sync_time == landed);
    }
    rc |= expect_true("no tail: the pass once -t since the landing has run out steps",
                      legacy_scan_pass_steps_at(opts, state, landed + 3) && g_rigctl_setfreq_freq == 972012500
                          && state->lcn_freq_roll == 2);
    dsd_decode_clock_use_system();
    free_test_runtime(opts, state);

    /* The listed tone: checked, then allowed and stamped by its last block; once it ends, the row waits out -t. */
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    setup_tone_rejection_ended(opts, state, 973012500L, 2);
    rc |= expect_true("tail: allow 100.0", dsd_tone_set_parse("100.0", &opts->analog_tone_set, NULL, 0) == 0);
    opts->trunk_hangtime = 2;
    g_rigctl_setfreq_ok = 1;
    decode_clock_at_ms(0U);
    const time_t heard = dsd_decode_time();
    /* The row landed 10 s ago: -t has run out, and the check stamps nothing. */
    state->last_cc_sync_time = heard - 10;
    feed_tap_blocks(opts, state, 30, 100.0);
    rc |= expect_true("tail: allowed", dsd_scan_analog_tone_gate(opts, state) == DSD_ANALOG_TONE_GATE_ALLOWED);
    /* Where the monitor's stamp leaves the anchor: the last block the policy passed. */
    state->last_cc_sync_time = heard;
    feed_tap_blocks(opts, state, 20, 0.0);
    rc |= expect_true("tail: ended", state->analog_rx.carrier_open == 0 && state->analog_rx.gate_rejected_ended == 0);
    rc |= expect_true("tail: the row waits out -t",
                      !legacy_scan_pass_steps_at(opts, state, heard + 2) && state->lcn_freq_roll == 1);
    const double now_m = dsd_decode_now_mono_s();
    dsd_engine_scan_y_timing_tick(opts, state, now_m, (double)(heard + 2));
    rc |= expect_true("tail: Hangtime, its last second counting down",
                      state->scan_timing.reason == (uint8_t)DSD_SCAN_STAY_HANGTIME
                          && fabs(state->scan_timing.deadline_m - (now_m + 1.0)) < 1e-6);
    rc |= expect_true("tail: the pass once -t has run out steps", legacy_scan_pass_steps_at(opts, state, heard + 3)
                                                                      && g_rigctl_setfreq_freq == 974012500
                                                                      && state->lcn_freq_roll == 2);
    dsd_decode_clock_use_system();
    g_rigctl_setfreq_ok = 0;
    free_test_runtime(opts, state);
    return rc;
}
#endif

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
/* The legacy untyped -Y step never runs the acquisition reset the typed rows do, so it
   clears the received tone itself: a new channel must not inherit the old one's. */
static int
test_rx_tone_resets_on_legacy_scan_step(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 946012500;
    state->trunk_lcn_freq[1] = 947012500;
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;

    /* A fresh hangtime: noCarrier() runs but does not step, and the tone stays. */
    seed_rx_tone_publication(state, 1318);
    const uint32_t generation = state->analog_rx.generation;
    state->last_cc_sync_time = time(NULL);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("rx-tone-no-step-keeps-tone",
                      g_rtl_tune_calls == 0 && state->lcn_freq_roll == 0 && state->analog_rx.ctcss_tenths_hz == 1318);

    /* The hangtime runs out: the step retunes and the tone is gone. */
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rx-tone-step-retuned", g_rtl_tune_calls == 1 && state->lcn_freq_roll == 1);
    rc |= expect_true("rx-tone-step-clears-tone", rx_tone_publication_cleared(state, generation));
    free_test_runtime(opts, state);
    return rc;
}
#endif

int
main(void) {
    int rc = 0;
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;

    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    dsd_call_observation observation = {0};
    observation.protocol = DSD_SYNC_P25P1_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 5001U;
    observation.policy_target_id = 5001U;
    observation.ota_source_id = 6001U;
    observation.observed_m = 1.0;
    rc |= expect_true("seed canonical call", dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(opts, state, 0U);

    // DMR payload and soft-decision history share noCarrier's generic reset path.
    // Seed both buffers with sentinels and move the payload pointer into the
    // dibit buffer to catch regressions that reset through the wrong backing
    // store after carrier loss.
    for (int i = 0; i < 200; i++) {
        state->dmr_payload_buf[i] = 0x7F7F7F7F;
        if (state->dmr_soft_buf != NULL) {
            state->dmr_soft_buf[i].reliability = 0xA5U;
        }
    }

    state->dmr_payload_p = state->dibit_buf + 321;
    if (state->dmr_soft_buf != NULL) {
        state->dmr_soft_p = state->dmr_soft_buf + 321;
    }
    state->p25_mac_frag[0].active = 1U;
    state->p25_mac_frag[0].opcode = 0x89U;
    state->p25_mac_frag[0].data_len = 4U;
    state->p25_mac_frag[0].collected = 2U;
    state->p25_mac_frag[0].data[0] = 0xAAU;
    state->p25_mac_frag[1].active = 1U;
    state->p25_mac_frag[1].opcode = 0x8AU;
    state->p25_mac_frag[1].data_len = 8U;
    state->p25_mac_frag[1].collected = 6U;
    state->p25_mac_frag[1].data[5] = 0xBBU;
    state->rtl_fsk_sps_num = 48000;
    state->rtl_fsk_sps_den = 4800;
    state->rtl_fsk_sps_accum = 2400;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state->p25_crypto_state[1] = DSD_P25_CRYPTO_DECRYPTABLE;
    state->p25_p2_audio_allowed[0] = 1;
    state->p25_p2_audio_allowed[1] = 1;
    state->data_header_dd_format[0] = 0x16U;
    state->data_header_dd_format[1] = 0x18U;
    state->data_header_bit_padding[0] = 16U;
    state->data_header_bit_padding[1] = 7U;
    // The --dmr-tg-key-csv lookup reads dmr_data_target_is_group[] to tell a data PDU's talkgroup
    // from a colliding radio id, so a stale group flag surviving carrier loss would qualify the
    // next target written. Both slots are seeded so a slot-0-only reset still fails.
    state->dmr_lrrp_target[0] = 1234U;
    state->dmr_lrrp_target[1] = 5678U;
    state->dmr_data_target_is_group[0] = 1U;
    state->dmr_data_target_is_group[1] = 1U;
    // NXDN's CRC evidence is per-transmission: carrying it across a carrier loss would let
    // the next channel's first noise frame stop a scan and unmute (issue #398).
    state->nxdn_confirmed = 1;
    state->nxdn_confirm_weak_streak = 1;
    state->nxdn_confirm_frame_evidence = 2;
    // YSF's FICH evidence is per-transmission the same way (issue #391): processYSF() reports
    // productive to the SPS hunt for as long as this flag stands, so carrying it across a carrier
    // loss would let the next channel's first FICH failure buy dwell it validated nothing for.
    state->ysf_fich_confirmed = 1U;
    // D-STAR and ProVoice carry the same kind of evidence (issue #421). Both lean on a weak
    // streak that only means anything while the frames stay adjacent, so carrying a streak --
    // or a confirmation -- across a carrier loss would let the next channel's first false
    // match buy the 1992 or 736 symbols it consumed.
    state->dstar_confirmed = 1;
    state->dstar_confirm_weak_streak = 1;
    state->dstar_confirm_frame_evidence = 2;
    state->provoice_confirmed = 1;
    state->provoice_confirm_weak_streak = 1;
    state->provoice_confirm_frame_evidence = 2;

    noCarrier(opts, state);

    rc |= expect_true("nxdn-confirmation-reset", state->nxdn_confirmed == 0 && state->nxdn_confirm_weak_streak == 0
                                                     && state->nxdn_confirm_frame_evidence == 0);
    rc |= expect_true("ysf-fich-confirmation-reset", state->ysf_fich_confirmed == 0U);
    rc |= expect_true("dstar-confirmation-reset", state->dstar_confirmed == 0 && state->dstar_confirm_weak_streak == 0
                                                      && state->dstar_confirm_frame_evidence == 0);
    rc |= expect_true("provoice-confirmation-reset", state->provoice_confirmed == 0
                                                         && state->provoice_confirm_weak_streak == 0
                                                         && state->provoice_confirm_frame_evidence == 0);

    dsd_call_snapshot ended_call;
    rc |= expect_true("no-carrier retains canonical snapshot", dsd_call_state_get(state, 0U, &ended_call) == 1);
    rc |= expect_true("no-carrier ends canonical call", ended_call.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("no-carrier commits canonical history",
                      state->event_history_s[0].Event_History_Items[1].target_id == 5001U);

    rc |= expect_true("dmr-payload-pointer-buffer", state->dmr_payload_p == state->dmr_payload_buf + 200);
    rc |= expect_true("dmr-payload-pointer-not-dibit", state->dmr_payload_p != state->dibit_buf + 200);
    rc |= expect_true("dibit-pointer-reset", state->dibit_buf_p == state->dibit_buf + 200);
    rc |= expect_true("p25-mac-fragment-reset",
                      state->p25_mac_frag[0].active == 0U && state->p25_mac_frag[0].opcode == 0U
                          && state->p25_mac_frag[0].data_len == 0U && state->p25_mac_frag[0].collected == 0U
                          && state->p25_mac_frag[0].data[0] == 0U && state->p25_mac_frag[1].active == 0U
                          && state->p25_mac_frag[1].opcode == 0U && state->p25_mac_frag[1].data_len == 0U
                          && state->p25_mac_frag[1].collected == 0U && state->p25_mac_frag[1].data[5] == 0U);
    rc |= expect_true("rtl-fsk-sps-cache-reset",
                      state->rtl_fsk_sps_num == 0 && state->rtl_fsk_sps_den == 0 && state->rtl_fsk_sps_accum == 0);
    rc |= expect_true("p25-crypto-readiness-reset", state->p25_crypto_state[0] == DSD_P25_CRYPTO_UNKNOWN
                                                        && state->p25_crypto_state[1] == DSD_P25_CRYPTO_UNKNOWN);
    rc |= expect_true("p25-crypto-audio-gates-reset",
                      state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_allowed[1] == 0);
    rc |= expect_true("dmr-short-data-metadata-reset",
                      state->data_header_dd_format[0] == 0U && state->data_header_dd_format[1] == 0U
                          && state->data_header_bit_padding[0] == 0U && state->data_header_bit_padding[1] == 0U);
    rc |= expect_true("dmr-data-target-reset", state->dmr_lrrp_target[0] == 0U && state->dmr_lrrp_target[1] == 0U);
    rc |= expect_true("dmr-data-target-group-flag-reset",
                      state->dmr_data_target_is_group[0] == 0U && state->dmr_data_target_is_group[1] == 0U);

    for (int i = 0; i < 200; i++) {
        if (state->dmr_payload_buf[i] != 0) {
            DSD_FPRINTF(stderr, "dmr payload buf[%d] not reset: %d\n", i, state->dmr_payload_buf[i]);
            rc = 1;
            break;
        }
    }

    if (state->dmr_soft_buf != NULL) {
        rc |= expect_true("dmr-soft-pointer-buffer", state->dmr_soft_p == state->dmr_soft_buf + 200);
        for (int i = 0; i < 200; i++) {
            if (state->dmr_soft_buf[i].reliability != 0U) {
                DSD_FPRINTF(stderr, "dmr soft buf[%d] not reset: %u\n", i,
                            (unsigned)state->dmr_soft_buf[i].reliability);
                rc = 1;
                break;
            }
        }
    }

    // A recent P25 voice-channel sync means noCarrier should keep trunk tuning
    // state intact even when the control-channel timer is stale. This preserves
    // an active voice call rather than forcing an unnecessary control-channel
    // reacquisition.
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL);
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;

    noCarrier(opts, state);

    rc |= expect_true("p25-vc-sync-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("p25-vc-sync-preserves-freq", state->p25_vc_freq[0] == 851012500);

#ifdef USE_RADIO
    // A CQPSK recovery queued by frame sync must also suppress the generic
    // noCarrier return that runs later in the same no-sync cycle. This hold is
    // state-machine-owned and does not refresh the voice-sync timestamp.
    const int saved_audio_in_type = opts->audio_in_type;
    const double recovery_now_m = dsd_decode_now_mono_s();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->last_vc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time_m = recovery_now_m - 11.0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 851012500;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 851012500;

    p25_sm_ctx_t* recovery_ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(recovery_ctx, opts, state);
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_P25);
    recovery_ctx->state = P25_SM_TUNED;
    recovery_ctx->vc_freq_hz = 851012500;
    recovery_ctx->vc_channel = (2 << 12) | 2;
    recovery_ctx->vc_tg = 7001;
    recovery_ctx->vc_is_tdma = 1;
    recovery_ctx->t_tune_m = recovery_now_m - 1.0;
    recovery_ctx->t_vc_reacquire_m = recovery_now_m;
    recovery_ctx->vc_reacquire_eligible = 1;
    recovery_ctx->vc_reacquire_attempted = 1;
    recovery_ctx->slots[0].grant_active = 1;
    recovery_ctx->slots[0].freq_hz = recovery_ctx->vc_freq_hz;
    recovery_ctx->slots[0].last_grant_m = recovery_ctx->t_tune_m;

    noCarrier(opts, state);

    rc |= expect_true("p25-vc-reacquire-hold-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("p25-vc-reacquire-hold-preserves-freq",
                      state->p25_vc_freq[0] == 851012500 && state->p25_vc_freq[1] == 851012500);
    rc |= expect_true("p25-vc-reacquire-hold-preserves-sync-deadline",
                      fabs(state->last_vc_sync_time_m - (recovery_now_m - 11.0)) <= 1.0e-9);

    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_UNKNOWN);
    recovery_ctx->t_vc_reacquire_m = 0.0;
    opts->audio_in_type = saved_audio_in_type;
    p25_sm_init_ctx(recovery_ctx, opts, state);
#endif

    // Once both control and voice sync are stale, the same reset path should
    // clear the tuned flags and cached voice frequencies so scanning can resume
    // from a clean trunking state.
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;

    noCarrier(opts, state);

    rc |= expect_true("p25-stale-vc-clears-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-stale-vc-clears-freq", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL);
    state->last_vc_sync_time = time(NULL) - 3;
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;
    state->trunk_vc_freq[0] = 851012500;
    state->trunk_vc_freq[1] = 851012500;
    state->p25_p2_active_slot = 0;
    observation.protocol = DSD_SYNC_P25P2_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 7001U;
    observation.policy_target_id = 7001U;
    observation.ota_source_id = 8001U;
    observation.channel = 1U;
    observation.frequency_hz = 851012500;
    observation.observed_m = 0.0;
    rc |= expect_true("p25-no-cc-hangtime-seeds-call",
                      dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);

    noCarrier(opts, state);

    rc |= expect_true("p25-no-cc-hangtime-clears-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-no-cc-hangtime-clears-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0
                                                          && state->trunk_vc_freq[0] == 0
                                                          && state->trunk_vc_freq[1] == 0);
    rc |= expect_true("p25-no-cc-hangtime-clears-active-slot", state->p25_p2_active_slot == -1);
    dsd_call_snapshot stale_call = {0};
    rc |= expect_true("p25-no-cc-hangtime-retains-call-snapshot", dsd_call_state_get(state, 0U, &stale_call) > 0);
    rc |= expect_true("p25-no-cc-hangtime-ends-active", stale_call.phase == DSD_CALL_PHASE_ENDED);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->trunk_cc_freq = 851012500;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL);
    state->trunk_vc_freq[0] = 852012500;
    state->trunk_vc_freq[1] = 852012500;

    noCarrier(opts, state);

    rc |= expect_true("generic-vc-sync-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("generic-vc-sync-preserves-freq",
                      state->trunk_vc_freq[0] == 852012500 && state->trunk_vc_freq[1] == 852012500);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->dmr_rest_channel = 4;
    state->trunk_chan_map[4] = 851012500;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 852012500;
    state->trunk_vc_freq[1] = 852012500;

    noCarrier(opts, state);

    rc |= expect_true("dmr-rest-only-stale-clears-rest", state->dmr_rest_channel == -1);
    rc |= expect_true("dmr-rest-only-stale-clears-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("dmr-rest-only-stale-clears-vc", state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);
    rc |= expect_true("dmr-rest-only-stale-keeps-cc-empty", state->p25_cc_freq == 0 && state->trunk_cc_freq == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 0;
    state->trunk_cc_freq = 936000000;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 936500000;
    state->trunk_vc_freq[1] = 936500000;

    noCarrier(opts, state);

    rc |= expect_true("generic-trunk-cc-only-keeps-p25-empty",
                      state->p25_cc_freq == 0 && state->trunk_cc_freq == 936000000);
    rc |= expect_true("generic-trunk-cc-only-clears-vc", state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // An accepted return to the control channel retunes the receiver, so a call still open on the
    // voice channel was left behind rather than faded. Reporting that as a sync loss would leave it
    // reacquirable, and the next transmission to appear on the control channel inside the window
    // would be folded into its history row.
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 0;
    state->trunk_cc_freq = 936000000;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 936500000;
    state->trunk_vc_freq[1] = 936500000;

    dsd_call_observation retuned_call = {0};
    retuned_call.protocol = DSD_SYNC_NXDN_POS;
    retuned_call.slot = 0U;
    retuned_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    retuned_call.ota_target_id = 7001U;
    retuned_call.policy_target_id = 7001U;
    retuned_call.ota_source_id = 8001U;
    retuned_call.observed_m = 1.0;
    rc |=
        expect_true("cc-return-seeds-call", dsd_call_state_observe(state, &retuned_call, DSD_CALL_BOUNDARY_BEGIN) == 1);

    noCarrier(opts, state);

    dsd_call_snapshot retuned_snapshot;
    rc |= expect_true("cc-return-retains-snapshot", dsd_call_state_get(state, 0U, &retuned_snapshot) == 1);
    rc |= expect_true("cc-return-ends-call", retuned_snapshot.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("cc-return-ends-call-explicitly", retuned_snapshot.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP) && DSD_NEO_TEST_RTL_WRAP
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // The scanner half of the same rule. noCarrier() steps the scanner before it finalizes calls,
    // so a hop that succeeds leaves the finalizer closing a call that belongs to the frequency the
    // receiver just left.
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 938012500;
    state->trunk_lcn_freq[1] = 939012500;
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_tune_calls = 0;

    dsd_call_observation scanned_call = {0};
    scanned_call.protocol = DSD_SYNC_NXDN_POS;
    scanned_call.slot = 0U;
    scanned_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    scanned_call.ota_target_id = 7101U;
    scanned_call.policy_target_id = 7101U;
    scanned_call.ota_source_id = 8101U;
    scanned_call.observed_m = 1.0;
    rc |= expect_true("scanner-hop-seeds-call",
                      dsd_call_state_observe(state, &scanned_call, DSD_CALL_BOUNDARY_BEGIN) == 1);

    noCarrier(opts, state);

    dsd_call_snapshot scanned_snapshot;
    rc |= expect_true("scanner-hop-retuned", g_rtl_tune_calls > 0);
    rc |= expect_true("scanner-hop-advanced", state->lcn_freq_roll == 1);
    rc |= expect_true("scanner-hop-retains-snapshot", dsd_call_state_get(state, 0U, &scanned_snapshot) == 1);
    rc |= expect_true("scanner-hop-ends-call", scanned_snapshot.phase == DSD_CALL_PHASE_ENDED);
    rc |=
        expect_true("scanner-hop-ends-call-explicitly", scanned_snapshot.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // Scan lists past 26 entries spill into a heap tail; the scanner step has to
    // hop through those via dsd_state_trunk_lcn_slot() exactly like the embedded
    // slots, and wrap from the tail back to the head of the list.
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    rc |= expect_true("scanner-ext-tail-reserve", dsd_state_trunk_lcn_reserve(state, 30) == 0);
    for (int i = 0; i < 30; i++) {
        *dsd_state_trunk_lcn_slot(state, i) = 944012500 + 12500 * (long)i;
    }
    state->lcn_freq_count = 30;
    state->lcn_freq_roll = 26;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_tune_calls = 0;

    noCarrier(opts, state);

    rc |= expect_true("scanner-ext-tail-hop-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944337500U);
    rc |= expect_true("scanner-ext-tail-hop-advanced", state->lcn_freq_roll == 27);

    state->lcn_freq_roll = 29;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-ext-tail-last-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944375000U);
    rc |= expect_true("scanner-ext-tail-last-advanced", state->lcn_freq_roll == 30);

    // roll == count wraps to the head on the next pass (engine.c clamps before reading).
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-ext-tail-wraps-to-head", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944012500U);
    rc |= expect_true("scanner-ext-tail-wrap-advanced", state->lcn_freq_roll == 1);

    // A scan hold pauses the rotation where it stands: the dwell may have expired, but the
    // receiver stays put, the roll does not move, and a call still open on the row is not
    // reported as an explicit release because nothing moved.
    state->lcn_scan_hold = 1;
    state->lcn_freq_roll = 5;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t held_dwell_started = state->last_cc_sync_time;
    g_rtl_tune_calls = 0;
    dsd_call_observation held_call = {0};
    held_call.protocol = DSD_SYNC_NXDN_POS;
    held_call.slot = 0U;
    held_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    held_call.ota_target_id = 7201U;
    held_call.policy_target_id = 7201U;
    held_call.ota_source_id = 8201U;
    held_call.observed_m = 1.0;
    rc |=
        expect_true("scanner-hold-seeds-call", dsd_call_state_observe(state, &held_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    noCarrier(opts, state);
    dsd_call_snapshot held_snapshot;
    rc |= expect_true("scanner-hold-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("scanner-hold-keeps-roll", state->lcn_freq_roll == 5);
    // The dwell timer is left alone under hold: the release command restarts it, so the row gets a
    // full hangtime then rather than hopping the instant the hold comes off.
    rc |= expect_true("scanner-hold-leaves-dwell-alone", state->last_cc_sync_time == held_dwell_started);
    rc |= expect_true("scanner-hold-retains-snapshot", dsd_call_state_get(state, 0U, &held_snapshot) == 1);
    rc |= expect_true("scanner-hold-ends-call-as-sync-loss",
                      held_snapshot.phase == DSD_CALL_PHASE_ENDED
                          && held_snapshot.end_reason != (uint8_t)DSD_CALL_END_EXPLICIT);
    state->lcn_scan_hold = 0;

    // An avoided row is stepped over in the same pass, so the hop lands on the next row the
    // operator still wants and the roll moves past the avoided one.
    rc |= expect_true("scanner-avoid-set", dsd_state_trunk_lcn_avoid_set(state, 6U, 1) == 0);
    state->lcn_freq_roll = 6;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-avoid-skips-row", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944100000U);
    rc |= expect_true("scanner-avoid-advanced-past", state->lcn_freq_roll == 8);

    // Avoided rows at the end of the heap tail wrap the walk back to the head.
    rc |= expect_true("scanner-avoid-tail-27", dsd_state_trunk_lcn_avoid_set(state, 27U, 1) == 0);
    rc |= expect_true("scanner-avoid-tail-28", dsd_state_trunk_lcn_avoid_set(state, 28U, 1) == 0);
    rc |= expect_true("scanner-avoid-tail-29", dsd_state_trunk_lcn_avoid_set(state, 29U, 1) == 0);
    state->lcn_freq_roll = 27;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-avoid-wraps-to-head", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944012500U);
    rc |= expect_true("scanner-avoid-wrap-advanced", state->lcn_freq_roll == 1);

    // Every row avoided (the UI refuses this, but the flags can be set directly): no hop.
    for (int i = 0; i < 30; i++) {
        rc |= expect_true("scanner-avoid-all-set", dsd_state_trunk_lcn_avoid_set(state, (size_t)i, 1) == 0);
    }
    state->lcn_freq_roll = 3;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-avoid-all-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("scanner-avoid-all-keeps-roll", state->lcn_freq_roll == 3);
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // Voice-gated scan (issue #381): a synced row with no decoded voice steps as soon as the
    // qualify window lapses even while the hangtime would keep parking there; decoded voice
    // holds the row past qualify; an unsynced visit and a disabled gate keep the hangtime
    // rule; the operator hold wins over all of it. Anchors ride the real monotonic clock
    // with second-scale margins, and every hop here uses a frequency no earlier case tunes
    // so engine.c's tune cache cannot swallow the retune.
    reset_rtl_profile_fakes();
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scan_voice_only = 1;
    opts->scan_voice_qualify_ms = 1000;
    opts->scan_voice_hold_ms = 2000;
    opts->trunk_hangtime = 10;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 947012500;
    state->trunk_lcn_freq[1] = 948012500;
    state->trunk_lcn_freq[2] = 949012500;
    state->trunk_lcn_freq[3] = 950012500;
    state->lcn_freq_count = 4;
    state->lcn_freq_roll = 0;

    // An IDLE row: synced frames, no voice media. The hangtime (10 s, deadline 1 s old)
    // would keep parking here, but the qualify window (1 s, synced 5 s ago) has lapsed.
    double gate_now_m = dsd_decode_now_mono_s();
    state->last_cc_sync_time = time(NULL) - 1;
    dsd_scan_voice_gate_note_retune(state, gate_now_m - 5.0);
    dsd_scan_voice_gate_tick(opts, state, 1, gate_now_m - 5.0);
    dsd_call_observation gate_idle_call = {0};
    gate_idle_call.protocol = DSD_SYNC_NXDN_POS;
    gate_idle_call.slot = 0U;
    gate_idle_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    gate_idle_call.ota_target_id = 7201U;
    gate_idle_call.policy_target_id = 7201U;
    gate_idle_call.ota_source_id = 8201U;
    gate_idle_call.observed_m = gate_now_m - 5.0;
    rc |= expect_true("voice-gate-idle-seeds-call",
                      dsd_call_state_observe(state, &gate_idle_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    rc |=
        expect_true("voice-gate-idle-due", dsd_scan_voice_gate_should_step(opts, state, dsd_decode_now_mono_s()) != 0);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    dsd_call_snapshot gate_idle_snapshot;
    rc |= expect_true("voice-gate-idle-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 947012500U);
    rc |= expect_true("voice-gate-idle-advanced", state->lcn_freq_roll == 1);
    rc |= expect_true("voice-gate-idle-retains-snapshot", dsd_call_state_get(state, 0U, &gate_idle_snapshot) == 1);
    rc |= expect_true("voice-gate-idle-ends-call", gate_idle_snapshot.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("voice-gate-idle-ends-call-explicitly",
                      gate_idle_snapshot.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);
    gate_now_m = dsd_decode_now_mono_s();
    rc |= expect_true("voice-gate-idle-resets-sync", state->scan_voice_gate_sync_m < 0.0);
    rc |= expect_true("voice-gate-idle-resets-voice", state->scan_voice_gate_voice_m < 0.0);
    rc |= expect_true("voice-gate-idle-restamps-arrive", fabs(state->scan_voice_gate_arrive_m - gate_now_m) < 5.0);

    // A terminator-ended call discovered on an unsynced tick still obeys a custom five-second
    // voice hold even though the legacy hangtime (1 s, deadline 11 s old) is due. This is the
    // post-dispatch shape of DMR BS: one processFrame() consumes the voice and terminator before
    // the engine gets its first gate tick, so sync_m is not available to select the gate.
    opts->trunk_hangtime = 1;
    opts->scan_voice_hold_ms = 5000;
    state->lcn_freq_roll = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    gate_now_m = dsd_decode_now_mono_s();
    dsd_scan_voice_gate_note_retune(state, gate_now_m - 3.0);
    dsd_call_observation gate_voice_call = {0};
    gate_voice_call.protocol = DSD_SYNC_NXDN_POS;
    gate_voice_call.slot = 0U;
    gate_voice_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    gate_voice_call.ota_target_id = 7202U;
    gate_voice_call.policy_target_id = 7202U;
    gate_voice_call.ota_source_id = 8202U;
    gate_voice_call.observed_m = gate_now_m - 2.8;
    rc |= expect_true("voice-gate-voice-seeds-call",
                      dsd_call_state_observe(state, &gate_voice_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    rc |= expect_true("voice-gate-voice-seeds-media",
                      dsd_call_state_update_media(state, 0U, 1, gate_now_m - 2.8) == 1
                          && dsd_call_state_update_media(state, 0U, 1, gate_now_m - 2.6) == 1);
    rc |= expect_true("voice-gate-voice-terminates-before-tick",
                      dsd_call_state_end_ex(state, 0U, gate_now_m - 2.5, DSD_CALL_END_TERMINATOR) == 1);
    dsd_scan_voice_gate_tick(opts, state, 0, gate_now_m);
    rc |= expect_true("voice-gate-tail-phase", state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_TAIL);
    rc |= expect_true("voice-gate-sync-remains-unset", state->scan_voice_gate_sync_m < 0.0);
    rc |= expect_true("voice-gate-retained-media-arms", state->scan_voice_gate_voice_m > 0.0);
    rc |= expect_true("voice-gate-voice-holds",
                      dsd_scan_voice_gate_should_step(opts, state, dsd_decode_now_mono_s()) == 0);
    const time_t voice_hold_deadline = state->last_cc_sync_time;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-voice-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("voice-gate-voice-keeps-roll", state->lcn_freq_roll == 1);
    rc |= expect_true("voice-gate-voice-keeps-deadline", state->last_cc_sync_time == voice_hold_deadline);
    opts->scan_voice_hold_ms = 2000;

    // Gate off: the retained gate anchor above is ignored and the hangtime rule decides alone.
    opts->scan_voice_only = 0;
    state->last_cc_sync_time = time(NULL);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-off-fresh-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("voice-gate-off-fresh-keeps-roll", state->lcn_freq_roll == 1);
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-off-stale-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 948012500U);
    rc |= expect_true("voice-gate-off-stale-advanced", state->lcn_freq_roll == 2);

    // Gate on but never synced this visit: the gate abstains and the hangtime rule steps.
    // The voice epoch above is retired first so its media cannot hold the visit instead.
    opts->scan_voice_only = 1;
    gate_now_m = dsd_decode_now_mono_s();
    // The voice epoch above is already ended by the earlier noCarrier passes; retiring it
    // again is a no-op so its media cannot hold the visit instead.
    (void)dsd_call_state_end_ex(state, 0U, gate_now_m, DSD_CALL_END_EXPLICIT);
    state->lcn_freq_roll = 2;

    state->last_cc_sync_time = time(NULL) - 11;
    gate_now_m = dsd_decode_now_mono_s();
    dsd_scan_voice_gate_note_retune(state, gate_now_m);
    dsd_scan_voice_gate_tick(opts, state, 0, gate_now_m);
    rc |= expect_true("voice-gate-unsynced-abstains",
                      dsd_scan_voice_gate_should_step(opts, state, dsd_decode_now_mono_s()) == 0);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-unsynced-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 949012500U);
    rc |= expect_true("voice-gate-unsynced-advanced", state->lcn_freq_roll == 3);

    // The operator hold wins: qualify lapsed, hangtime due, but the row stays put.
    state->lcn_freq_roll = 3;
    state->last_cc_sync_time = time(NULL) - 11;
    state->lcn_scan_hold = 1;
    gate_now_m = dsd_decode_now_mono_s();
    dsd_scan_voice_gate_note_retune(state, gate_now_m - 5.0);
    dsd_scan_voice_gate_tick(opts, state, 1, gate_now_m - 5.0);
    rc |=
        expect_true("voice-gate-hold-due", dsd_scan_voice_gate_should_step(opts, state, dsd_decode_now_mono_s()) == 0);
    const time_t operator_hold_deadline = state->last_cc_sync_time;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-hold-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("voice-gate-hold-keeps-roll", state->lcn_freq_roll == 3);
    rc |= expect_true("voice-gate-hold-keeps-deadline", state->last_cc_sync_time == operator_hold_deadline);
    state->lcn_scan_hold = 0;

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // With both backends configured the rigctl leg runs first. If it lands and the RTL leg then
    // fails, the scan step is abandoned -- but the radio has already moved off the frequency the
    // open call was decoded from. The end must still be EXPLICIT: reporting sync loss would leave
    // the call reacquirable by whatever the new frequency happens to carry.
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->setmod_bw = 0;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    // Frequencies no earlier case in this file uses: engine.c caches the last rigctl and RTL tune
    // in file statics that outlive free_test_runtime(), and a repeat would be skipped as a no-op.
    state->trunk_lcn_freq[0] = 942012500;
    state->trunk_lcn_freq[1] = 943012500;
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t partial_hop_scan_time = state->last_cc_sync_time;
    g_rigctl_setfreq_ok = 1;
    g_rigctl_setfreq_calls = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    g_rtl_tune_calls = 0;

    dsd_call_observation partial_hop_call = {0};
    partial_hop_call.protocol = DSD_SYNC_NXDN_POS;
    partial_hop_call.slot = 0U;
    partial_hop_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    partial_hop_call.ota_target_id = 7201U;
    partial_hop_call.policy_target_id = 7201U;
    partial_hop_call.ota_source_id = 8201U;
    partial_hop_call.observed_m = 1.0;
    rc |= expect_true("partial-hop-seeds-call",
                      dsd_call_state_observe(state, &partial_hop_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    seed_rx_tone_publication(state, 1318);
    const uint32_t partial_hop_tone_generation = state->analog_rx.generation;

    noCarrier(opts, state);

    dsd_call_snapshot partial_hop_snapshot;
    rc |= expect_true("partial-hop-moved-rigctl", g_rigctl_setfreq_calls > 0 && g_rigctl_setfreq_freq == 942012500);
    rc |= expect_true("partial-hop-rtl-failed", g_rtl_tune_calls > 0);
    // The step itself is still abandoned: the candidate and the deadline are untouched so the next
    // pass retries this entry rather than skipping it.
    rc |= expect_true("partial-hop-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("partial-hop-keeps-deadline", state->last_cc_sync_time == partial_hop_scan_time);
    rc |= expect_true("partial-hop-retains-snapshot", dsd_call_state_get(state, 0U, &partial_hop_snapshot) == 1);
    rc |= expect_true("partial-hop-ends-call", partial_hop_snapshot.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("partial-hop-ends-call-explicitly",
                      partial_hop_snapshot.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);
    // The received tone (issue #522) goes the same way: the radio is on another frequency, so
    // the tone heard on the old one no longer describes it, even though the step failed.
    rc |= expect_true("partial-hop-clears-rx-tone", rx_tone_publication_cleared(state, partial_hop_tone_generation));
    g_rigctl_setfreq_ok = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
#endif

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->scanner_mode = 1;
    state->trunk_lcn_freq[0] = 938012500;
    state->lcn_freq_count = 1;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t missing_backend_scan_time = state->last_cc_sync_time;

    noCarrier(opts, state);

    rc |= expect_true("scanner-missing-backend-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("scanner-missing-backend-keeps-deadline", state->last_cc_sync_time == missing_backend_scan_time);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->scanner_mode = 1;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    state->trunk_lcn_freq[0] = 938012500;
    state->lcn_freq_count = 1;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t failed_rigctl_scan_time = state->last_cc_sync_time;

    noCarrier(opts, state);

    rc |= expect_true("rigctl-scanner-failure-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("rigctl-scanner-failure-keeps-deadline", state->last_cc_sync_time == failed_rigctl_scan_time);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->trunk_cc_freq = 939012500;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 939512500;
    state->trunk_vc_freq[1] = 939512500;

    noCarrier(opts, state);

    rc |= expect_true("rigctl-direct-failure-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("rigctl-direct-failure-preserves-vc",
                      state->trunk_vc_freq[0] == 939512500 && state->trunk_vc_freq[1] == 939512500);
    rc |= expect_true("rigctl-direct-failure-preserves-cc", state->trunk_cc_freq == 939012500);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // noCarrier can run from control pumping inside guarded frame dispatch.
    // Guard contention must defer the P25 return without blocking or clearing
    // the voice state, then allow the next main-loop pass to complete it.
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    int preheld_guard = p25_sm_tick_guard_try_enter();
    rc |= expect_true("p25-nocarrier-contention-setup", preheld_guard == 1);
    if (preheld_guard) {
        noCarrier(opts, state);
        rc |= expect_true("p25-nocarrier-contention-preserves-state", opts->trunk_is_tuned == 1
                                                                          && state->p25_vc_freq[0] == 771056250
                                                                          && state->p25_vc_freq[1] == 771056250);
        p25_sm_tick_guard_leave();
    }

    noCarrier(opts, state);
    rc |= expect_true("p25-nocarrier-contention-retries", opts->trunk_is_tuned == 0 && state->p25_vc_freq[0] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->trunk_cc_freq = 940012500;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 940512500;
    state->trunk_vc_freq[1] = 940512500;

    noCarrier(opts, state);

    rc |= expect_true("rtl-direct-missing-context-does-not-tune", g_rtl_tune_calls == 0);
    rc |= expect_true("rtl-direct-missing-context-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("rtl-direct-missing-context-preserves-vc",
                      state->trunk_vc_freq[0] == 940512500 && state->trunk_vc_freq[1] == 940512500);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_cc_freq = 941012500;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 941512500;
    state->trunk_vc_freq[1] = 941512500;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;

    noCarrier(opts, state);

    rc |= expect_true("rtl-direct-failure-attempts", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 941012500U);
    rc |= expect_true("rtl-direct-failure-preserves-state", opts->trunk_is_tuned == 1
                                                                && state->trunk_vc_freq[0] == 941512500
                                                                && state->trunk_vc_freq[1] == 941512500);

    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    noCarrier(opts, state);

    rc |= expect_true("rtl-direct-timeout-retries-uncached", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 941012500U);
    rc |= expect_true("rtl-direct-timeout-preserves-state", opts->trunk_is_tuned == 1
                                                                && state->trunk_vc_freq[0] == 941512500
                                                                && state->trunk_vc_freq[1] == 941512500);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);

    rc |= expect_true("rtl-direct-success-retries-uncached", g_rtl_tune_calls == 3 && g_rtl_tune_freq == 941012500U);
    rc |= expect_true("rtl-direct-success-clears-voice-state",
                      opts->trunk_is_tuned == 0 && state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    g_rtl_output_rate = 96000;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    opts->slot1_on = 0;
    opts->slot2_on = 0;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769768750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 0;
    state->p25_p2_active_slot = 0;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;
    state->samplesPerSymbol = 8;
    state->symbolCenter = 3;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->sps_hunt_counter = 23;
    state->p25_p1_validated_rf_mod = 1;
    state->p25_p1_nid_evidence = 1;
    state->p25_p1_nid_evidence_symbolcnt = 4321U;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-cc-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    /* Issue #423: which modulation carried the last validated P25p1 frame is system knowledge,
     * not acquisition state, so losing the carrier must not erase it -- otherwise every fade
     * costs an LSM control channel the chain it had already earned. */
    rc |= expect_true("p25-rtl-nocarrier-keeps-learned-modulation", state->p25_p1_validated_rf_mod == 1);
    /* Issue #400: what a decoded NID vouches for is the other kind of thing. The benefit of the
     * doubt it lends the failures around it is about a transmission in progress, so it cannot
     * outlast the carrier -- the next one proves itself again. */
    rc |= expect_true("p25-rtl-nocarrier-clears-nid-evidence",
                      state->p25_p1_nid_evidence == 0 && state->p25_p1_nid_evidence_symbolcnt == 0U);
    rc |= expect_true("p25-rtl-nocarrier-syncs-selected-cc",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);
    rc |= expect_true("p25-rtl-nocarrier-cc-profile-rate", g_rtl_symbol_rate_hz == 4800);
    rc |= expect_true("p25-rtl-nocarrier-cc-profile-cqpsk", g_rtl_cqpsk_enable == 1);
    rc |= expect_true("p25-rtl-nocarrier-cc-profile-ted", g_rtl_ted_sps == 20 && g_rtl_ted_sps_override == 0);
    rc |= expect_true("p25-rtl-nocarrier-dynamic-symbol-timing",
                      state->samplesPerSymbol == 20 && state->symbolCenter == 9);
    rc |= expect_true("p25-rtl-nocarrier-selects-four-level-profile",
                      state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 && state->sps_hunt_counter == 0);
    rc |= expect_true("p25-rtl-nocarrier-reenables-slots", opts->slot1_on == 1 && opts->slot2_on == 1);
    rc |= expect_true("p25-rtl-nocarrier-clear-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-rtl-nocarrier-clear-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);
    rc |= expect_true("p25-rtl-nocarrier-uses-return-grace",
                      p25_sm_get_ctx()->cc_acquisition_origin == P25_SM_CC_ACQUISITION_RETURN);

    // A controller wait timeout remains correlated until the controller
    // publishes the physical retune result.
    reset_rtl_profile_fakes();
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 771056250;
    p25_sm_ctx_t* pending_ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(pending_ctx, opts, state);
    g_check_p25_tick_guard = 1;

    noCarrier(opts, state);
    g_check_p25_tick_guard = 0;

    rc |= expect_true("p25-rtl-nocarrier-timeout-accepted", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    const uint64_t pending_cc_request_id = pending_ctx->cc_tune_request_id;
    rc |= expect_true(
        "p25-rtl-nocarrier-timeout-waits-for-completion",
        pending_ctx->cc_tune_pending == 1 && pending_ctx->t_cc_tune_m == 0.0 && pending_cc_request_id != 0U
            && pending_ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_RETURN
            && dsd_trunk_tuning_request_status(pending_cc_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    rc |= expect_true("p25-rtl-nocarrier-timeout-serializes-tune", g_p25_tick_guard_held_during_tune == 1);
    int guard_released = p25_sm_tick_guard_try_enter();
    rc |= expect_true("p25-rtl-nocarrier-timeout-releases-guard", guard_released == 1);
    if (guard_released) {
        p25_sm_tick_guard_leave();
    }
    rc |= expect_true("p25-rtl-nocarrier-timeout-keeps-frame-gate-closed",
                      !dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
    dsd_trunk_tuning_request_publish(pending_cc_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    p25_sm_tick_ctx(pending_ctx, opts, state);
    rc |= expect_true("p25-rtl-nocarrier-completion-starts-acquisition",
                      pending_ctx->cc_tune_pending == 0 && pending_ctx->t_cc_tune_m > 0.0
                          && pending_ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_RETURN);
    rc |= expect_true("p25-rtl-nocarrier-completion-opens-frame-gate",
                      dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 773456250;
    state->lcn_freq_count = 1;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t deferred_scan_time = state->last_cc_sync_time;
    g_rtl_tune_result = RTL_STREAM_TUNE_DEFERRED;

    noCarrier(opts, state);

    rc |= expect_true("rtl-scanner-deferred-attempt", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 773456250U);
    rc |= expect_true("rtl-scanner-deferred-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("rtl-scanner-deferred-keeps-deadline", state->last_cc_sync_time == deferred_scan_time);

    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    noCarrier(opts, state);

    rc |= expect_true("rtl-scanner-failure-retries-uncached", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 773456250U);
    rc |= expect_true("rtl-scanner-failure-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("rtl-scanner-failure-keeps-deadline", state->last_cc_sync_time == deferred_scan_time);

    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    noCarrier(opts, state);

    rc |= expect_true("rtl-scanner-timeout-retries-uncached", g_rtl_tune_calls == 3 && g_rtl_tune_freq == 773456250U);
    rc |= expect_true("rtl-scanner-timeout-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("rtl-scanner-timeout-keeps-deadline", state->last_cc_sync_time == deferred_scan_time);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);

    rc |= expect_true("rtl-scanner-deferred-retries", g_rtl_tune_calls == 4 && g_rtl_tune_freq == 773456250U);
    rc |= expect_true("rtl-scanner-retry-advances-candidate", state->lcn_freq_roll == 1);
    rc |= expect_true("rtl-scanner-retry-restarts-deadline", state->last_cc_sync_time > deferred_scan_time);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 771156250;
    state->lcn_freq_count = 1;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-cache-prime", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 769868750U);

    state->last_cc_sync_time = time(NULL) - 11;
    state->lcn_freq_roll = 0;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-cache-allows-scan-retune",
                      g_rtl_tune_calls == 3 && g_rtl_tune_freq == 771156250U);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-auto-delayed-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-auto-delayed-keeps-p25-cc",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);
    rc |= expect_true("p25-rtl-nocarrier-auto-delayed-profile", g_rtl_symbol_rate_hz == 4800);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 2;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-mixed-no-identity-retune",
                      g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-mixed-no-identity-profile",
                      g_rtl_symbol_rate_hz == 4800 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    rc |= expect_true("p25-rtl-nocarrier-mixed-no-identity-sync",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);
    rc |= expect_true("p25-rtl-nocarrier-mixed-no-identity-clears-tuned", opts->trunk_is_tuned == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769768750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-delayed-selected-cc-retune",
                      g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-delayed-selected-cc-profile", g_rtl_symbol_rate_hz == 4800);
    rc |= expect_true("p25-rtl-nocarrier-delayed-selected-cc-sync",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769768750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;
    state->trunk_vc_freq[0] = 771056250;
    state->trunk_vc_freq[1] = 771056250;
    g_rtl_tune_result = RTL_STREAM_TUNE_DEFERRED;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-deferred-tune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-deferred-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("p25-rtl-nocarrier-deferred-preserves-vc",
                      state->p25_vc_freq[0] == 771056250 && state->p25_vc_freq[1] == 771056250
                          && state->trunk_vc_freq[0] == 771056250 && state->trunk_vc_freq[1] == 771056250);
    rc |= expect_true("p25-rtl-nocarrier-deferred-preserves-selected-cc",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-deferred-retries", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-deferred-retry-profile",
                      g_rtl_symbol_rate_hz == 4800 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    rc |= expect_true("p25-rtl-nocarrier-retry-syncs-cc",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);
    rc |= expect_true("p25-rtl-nocarrier-retry-clears-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-rtl-nocarrier-retry-clears-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0
                                                               && state->trunk_vc_freq[0] == 0
                                                               && state->trunk_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;
    state->trunk_vc_freq[0] = 771056250;
    state->trunk_vc_freq[1] = 771056250;
    state->p25_p2_active_slot = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-failed-tune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-failed-clears-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-rtl-nocarrier-failed-clears-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0
                                                                && state->trunk_vc_freq[0] == 0
                                                                && state->trunk_vc_freq[1] == 0);
    rc |= expect_true("p25-rtl-nocarrier-failed-clears-active-slot", state->p25_p2_active_slot == -1);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 935000000;
    state->trunk_cc_freq = 935000000;
    state->p25_cc_is_tdma = 2;
    state->p25_p2_active_slot = 0;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;

    noCarrier(opts, state);

    rc |= expect_true("generic-rtl-nocarrier-slot-zero-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 935000000U);
    rc |= expect_true("generic-rtl-nocarrier-preserves-cc-alias",
                      state->p25_cc_freq == 935000000 && state->trunk_cc_freq == 935000000);
    rc |= expect_true("generic-rtl-nocarrier-keeps-profile",
                      g_rtl_symbol_rate_hz == 6000 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 936000000;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 936500000;
    state->trunk_vc_freq[1] = 936500000;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;

    noCarrier(opts, state);

    rc |= expect_true("generic-rtl-nocarrier-stale-p25-alias-retune",
                      g_rtl_tune_calls == 1 && g_rtl_tune_freq == 936000000U);
    rc |= expect_true("generic-rtl-nocarrier-stale-p25-alias-cleared",
                      state->p25_cc_freq == 0 && state->trunk_cc_freq == 936000000);
    rc |= expect_true("generic-rtl-nocarrier-stale-p25-keeps-profile",
                      g_rtl_symbol_rate_hz == 6000 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 938000000;
    state->trunk_cc_freq = 938000000;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->p25_sys_is_tdma = 1;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL);
    state->p25_vc_freq[0] = 938500000;
    state->p25_vc_freq[1] = 938500000;
    state->trunk_vc_freq[0] = 938500000;
    state->trunk_vc_freq[1] = 938500000;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;

    noCarrier(opts, state);

    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;

    noCarrier(opts, state);

    rc |=
        expect_true("generic-rtl-repeated-nocarrier-cc-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 938000000U);
    rc |= expect_true("generic-rtl-repeated-nocarrier-preserves-cc-alias",
                      state->p25_cc_freq == 938000000 && state->trunk_cc_freq == 938000000);
    rc |= expect_true("generic-rtl-repeated-nocarrier-keeps-profile",
                      g_rtl_symbol_rate_hz == 6000 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);
    rc |= expect_true("generic-rtl-repeated-nocarrier-clears-tuned", opts->trunk_is_tuned == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 770168750;
    state->trunk_cc_freq = 770168750;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->dmr_rest_channel = 7;
    state->trunk_chan_map[7] = 0;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-unmapped-rest-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 770168750U);
    rc |= expect_true("p25-rtl-nocarrier-unmapped-rest-profile", g_rtl_symbol_rate_hz == 4800);
    rc |= expect_true("p25-rtl-nocarrier-unmapped-rest-clears", state->dmr_rest_channel == -1);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_dmr = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 852012500;
    state->trunk_cc_freq = 851012500;
    state->dmr_rest_channel = 7;
    state->trunk_chan_map[7] = 853012500;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;

    noCarrier(opts, state);

    rc |= expect_true("dmr-rtl-nocarrier-rest-cc-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 853012500U);
    rc |= expect_true("dmr-rtl-nocarrier-clears-rest", state->dmr_rest_channel == -1);
    rc |= expect_true("dmr-rtl-nocarrier-clears-stale-p25-cc",
                      state->trunk_cc_freq == 853012500 && state->p25_cc_freq == 0);
    rc |= expect_true("dmr-rtl-nocarrier-keeps-generic-profile",
                      g_rtl_symbol_rate_hz == 6000 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // An unresolved generic tune keeps dispatch gated. Once it fails, a
    // controller timeout remains pending until its exact completion arrives.
    reset_rtl_profile_fakes();
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 936000000;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 936500000;
    state->trunk_vc_freq[1] = 936500000;

    const uint64_t failed_generation = dsd_trunk_tuning_generation();
    rc |= expect_true("generic-rtl-recovery-clean-gate", dsd_trunk_tuning_frame_is_current(failed_generation));
    const uint64_t failed_request_id = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_mark_ready(failed_request_id);

    noCarrier(opts, state);

    rc |= expect_true("generic-rtl-recovery-pending-suppresses-retune",
                      failed_request_id != 0U && g_rtl_tune_calls == 0 && opts->trunk_is_tuned == 1
                          && dsd_trunk_tuning_request_status(failed_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    dsd_trunk_tuning_request_publish(failed_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect_true("generic-rtl-recovery-seeds-failed-gate",
                      dsd_trunk_tuning_request_status(failed_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED
                          && !dsd_trunk_tuning_frame_is_current(failed_generation));

    noCarrier(opts, state);

    const uint64_t recovery_generation = dsd_trunk_tuning_generation();
    const uint64_t recovery_request_id = dsd_trunk_tuning_pending_request();
    rc |=
        expect_true("generic-rtl-recovery-timeout-remains-pending",
                    g_rtl_tune_calls == 1 && g_rtl_tune_freq == 936000000U && recovery_generation == failed_generation
                        && recovery_request_id > failed_request_id
                        && dsd_trunk_tuning_request_status(recovery_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    rc |= expect_true("generic-rtl-recovery-timeout-stages-state",
                      opts->trunk_is_tuned == 1 && state->trunk_vc_freq[0] == 0 && state->trunk_cc_freq == 936000000
                          && state->p25_cc_freq == 0);
    rc |= expect_true("generic-rtl-recovery-timeout-keeps-gate-closed",
                      !dsd_trunk_tuning_frame_is_current(recovery_generation));
    rc |=
        expect_true("generic-rtl-recovery-preserves-profile", g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    dsd_trunk_tuning_request_publish(recovery_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    noCarrier(opts, state);
    const uint64_t completed_recovery_generation = dsd_trunk_tuning_generation();
    rc |= expect_true("generic-rtl-recovery-completion-commits-state",
                      opts->trunk_is_tuned == 0 && state->trunk_vc_freq[0] == 0 && state->trunk_cc_freq == 936000000
                          && state->p25_cc_freq == 0);
    rc |=
        expect_true("generic-rtl-recovery-completion-opens-gate",
                    completed_recovery_generation == failed_generation + 1U && dsd_trunk_tuning_pending_request() == 0U
                        && dsd_trunk_tuning_frame_is_current(completed_recovery_generation));

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
#endif

    // Leaving trunking retires terminal correlated failures so scanner/manual
    // modes cannot inherit a process-wide frame-dispatch gate. In-flight
    // requests remain gated until their backend publishes a terminal result.
    const uint64_t inactive_generation = dsd_trunk_tuning_generation();
    const uint64_t inactive_failed_request = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_publish(inactive_failed_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect_true("inactive-trunking-seeds-failed-gate",
                      inactive_failed_request != 0U && dsd_trunk_tuning_pending_request() == inactive_failed_request
                          && !dsd_trunk_tuning_frame_is_current(inactive_generation));
    opts->scanner_mode = 1;
    noCarrier(opts, state);
    rc |= expect_true("inactive-trunking-retires-failed-gate",
                      dsd_trunk_tuning_pending_request() == 0U && dsd_trunk_tuning_generation() == inactive_generation
                          && dsd_trunk_tuning_frame_is_current(inactive_generation));

    // Trunk scan keeps long-lived discovery state across carrier gaps. The test
    // keeps DMR confidence and P25 control-channel candidates populated while
    // still requiring transient P25 frame metrics to be reset.
    opts->trunk_scan_enabled = 1;
    state->dmr_color_code = 5;
    state->dmr_confidence_locked = 1;
    state->dmr_confidence_color_code = 5;
    state->dmr_confidence_candidate_cc = 5;
    state->dmr_confidence_candidate_count = 2;
    state->dmr_confidence_voice_sync_seen[0] = 1;
    state->p25_cc_cache_loaded = 1;
    state->p25_p1_fec_ok = 7;
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(state);
    if (cc == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: cc-candidates\n");
        rc = 1;
    } else {
        cc->count = 2;
        cc->idx = 1;
        cc->candidates[0] = 851012500L;
        cc->candidates[1] = 852012500L;
    }

    noCarrier(opts, state);

    rc |=
        expect_true("trunk-scan-preserves-dmr-confidence",
                    state->dmr_color_code == 5 && state->dmr_confidence_locked == 1
                        && state->dmr_confidence_color_code == 5 && state->dmr_confidence_candidate_cc == 5
                        && state->dmr_confidence_candidate_count == 2 && state->dmr_confidence_voice_sync_seen[0] == 1);
    rc |= expect_true("trunk-scan-preserves-p25-cache", state->p25_cc_cache_loaded == 1 && cc != NULL && cc->count == 2
                                                            && cc->idx == 1 && cc->candidates[0] == 851012500L
                                                            && cc->candidates[1] == 852012500L);
    rc |= expect_true("trunk-scan-still-resets-p25-metrics", state->p25_p1_fec_ok == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // Key-file sets arm keyloader and are transmission-scoped. Their canonical
    // AES bytes must be erased with the scalar aliases on carrier loss.
    state->keyloader = 1;
    state->K = 7ULL;
    state->K1 = state->H = 0x0011223344556677ULL;
    state->A1[0] = state->A1[1] = state->K1;
    state->aes_key_loaded[0] = state->aes_key_loaded[1] = 1;
    state->aes_key_segments[0] = state->aes_key_segments[1] = 2U;
    DSD_MEMSET(state->aes_key, 0xA5, sizeof(state->aes_key));
    noCarrier(opts, state);
    int aes_cleared = 1;
    for (size_t i = 0U; i < sizeof(state->aes_key); i++) {
        if (state->aes_key[i] != 0U) {
            aes_cleared = 0;
            break;
        }
    }
    rc |= expect_true("keyloader-carrier-reset-clears-aes-bytes", aes_cleared && state->K == 0ULL && state->K1 == 0ULL
                                                                      && state->H == 0ULL
                                                                      && state->aes_key_loaded[0] == 0);

    // Embedded direct keys deliberately use keyloader=0, matching -b/-H:
    // noCarrier preserves them and must not alter the operator's mute policy.
    dsd_key_set direct;
    DSD_MEMSET(&direct, 0, sizeof(direct));
    if (dsd_key_set_load_direct(&direct, "00112233445566778899AABBCCDDEEFF", "7") != DSD_KEY_DIRECT_OK) {
        free_test_runtime(opts, state);
        return 1;
    }
    dsd_key_set_install(state, &direct);
    dsd_key_set_free(&direct);
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;
    noCarrier(opts, state);
    rc |= expect_true("direct-key-carrier-reset-persists", state->keyloader == 0 && state->K == 7ULL
                                                               && state->K1 == 0x0011223344556677ULL
                                                               && state->aes_key[15] == 0xFFU);
    rc |= expect_true("direct-key-carrier-reset-preserves-mute-policy",
                      opts->dmr_mute_encL == 1 && opts->dmr_mute_encR == 1);

#ifdef USE_RADIO
    // Radio builds also exercise the RTL/FSK reacquisition counters. A recovered
    // sync must close the current gap and refresh the last-sync timer without
    // depending on real hardware.
    dsd_rtl_stream_metrics_hooks hooks = {.output_kind = fake_rtl_fsk_output_kind};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (struct RtlSdrContext*)state;
    state->lastsynctype = DSD_SYNC_YSF_POS;
    state->rtl_fsk_reacquire_gap_start_m = dsd_decode_now_mono_s() - 1.0;
    state->rtl_fsk_reacquire_last_sync_m = state->rtl_fsk_reacquire_gap_start_m - 1.0;
    state->rtl_fsk_reacquire_last_sync_time = time(NULL) - 2;
    double old_reacquire_sync_m = state->rtl_fsk_reacquire_last_sync_m;

    noCarrier(opts, state);

    rc |= expect_true("rtl-fsk-recovered-sync-clears-gap", state->rtl_fsk_reacquire_gap_start_m == 0.0);
    rc |= expect_true("rtl-fsk-recovered-sync-refreshes-timer",
                      state->rtl_fsk_reacquire_last_sync_m > old_reacquire_sync_m);
#if defined(DSD_NEO_TEST_RTL_WRAP)
    g_rtl_fsk_reacquire_requests = 0;
    double reacquire_now_m = dsd_decode_now_mono_s();
    time_t reacquire_now = time(NULL);
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = reacquire_now - 1;
    state->last_vc_sync_time = 0;
    state->last_cc_sync_time_m = reacquire_now_m - 1.0;
    state->last_vc_sync_time_m = 0.0;
    state->rtl_fsk_reacquire_last_sync_time = reacquire_now - 1;
    state->rtl_fsk_reacquire_last_sync_m = reacquire_now_m - 1.0;
    state->rtl_fsk_reacquire_gap_start_m = reacquire_now_m - 1.0;
    state->rtl_fsk_reacquire_last_request_m = 0.0;

    noCarrier(opts, state);

    rc |= expect_true("rtl-fsk-short-nosync-gap-does-not-reacquire", g_rtl_fsk_reacquire_requests == 0);

    g_rtl_fsk_reacquire_requests = 0;
    reacquire_now_m = dsd_decode_now_mono_s();
    reacquire_now = time(NULL);
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = reacquire_now - 11;
    state->last_vc_sync_time = 0;
    state->last_cc_sync_time_m = reacquire_now_m - 11.0;
    state->last_vc_sync_time_m = 0.0;
    state->rtl_fsk_reacquire_last_sync_time = reacquire_now - 11;
    state->rtl_fsk_reacquire_last_sync_m = reacquire_now_m - 11.0;
    state->rtl_fsk_reacquire_gap_start_m = reacquire_now_m - 11.0;
    state->rtl_fsk_reacquire_last_request_m = 0.0;

    noCarrier(opts, state);

    rc |= expect_true("rtl-fsk-long-nosync-gap-reacquires-once", g_rtl_fsk_reacquire_requests == 1);
    rc |= expect_true("rtl-fsk-long-nosync-gap-records-request", state->rtl_fsk_reacquire_last_request_m > 0.0);
#endif

#if defined(DSD_NEO_TEST_RTL_WRAP) && DSD_NEO_TEST_RTL_WRAP
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // Per-row keys under -Y. A hop onto a keyed row installs its set, a hop
    // back onto an unkeyed row restores the globals, a zero-frequency row
    // parks in place keeping the previous set, and a failed tune leg swaps
    // nothing.
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 940012500;
    state->trunk_lcn_freq[1] = 941012500;
    state->trunk_lcn_freq[2] = 0;
    state->lcn_freq_count = 3;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    state->keyloader = 0;
    state->K = 0xBEEFULL;
    state->rkey_array[3] = 111ULL;
    state->rkey_array_loaded[3] = 1U;
    {
        dsd_key_set ks;
        DSD_MEMSET(&ks, 0, sizeof(ks));
        ks.entries = (dsd_key_set_entry*)calloc(1U, sizeof(*ks.entries));
        if (ks.entries == NULL) {
            free_test_runtime(opts, state);
            return 1;
        }
        ks.count = 1U;
        ks.present = 1;
        ks.keyloader = 1;
        ks.entries[0].index = 9U;
        ks.entries[0].value = 999ULL;
        ks.entries[0].loaded = 1U;
        if (dsd_state_trunk_lcn_keys_set(state, 0U, &ks) != 0) {
            free_test_runtime(opts, state);
            return 1;
        }
    }
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_tune_calls = 0;

    noCarrier(opts, state);
    rc |= expect_true("rowkey-hop-installs", state->rkey_array[9] == 999ULL && state->keyloader == 1 && state->K == 0ULL
                                                 && state->lcn_freq_roll == 1);

    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rowkey-hop-restores", state->rkey_array[9] == 0ULL && state->keyloader == 0
                                                 && state->K == 0xBEEFULL && state->rkey_array[3] == 111ULL
                                                 && state->lcn_freq_roll == 2);

    // Re-park on the keyed row, then step onto the zero-frequency row: it parks
    // in place, so the installed set stays.
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rowkey-rehop-installs", state->rkey_array[9] == 999ULL && state->scan_keys_active_set == 1);
    state->lcn_freq_roll = 2;
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rowkey-zero-row-keeps-set",
                      state->rkey_array[9] == 999ULL && state->scan_keys_active_set == 1 && state->lcn_freq_roll == 3);

    // A failed tune leg leaves the receiver (and the installed set) where it was.
    state->lcn_freq_roll = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    noCarrier(opts, state);
    rc |= expect_true("rowkey-failed-tune-keeps-roll", state->lcn_freq_roll == 1);
    rc |=
        expect_true("rowkey-failed-tune-keeps-set", state->rkey_array[9] == 999ULL && state->scan_keys_active_set == 1);
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
#endif

    dsd_rtl_stream_metrics_hooks_set(NULL);
#endif

    free_test_runtime(opts, state);

    rc |= test_rx_tone_survives_no_carrier();
    rc |= test_dmr_stale_follow_clear_runs_on_the_decode_clock();
    rc |= test_dmr_mfid_clear_runs_on_the_decode_clock_without_trunking();
#ifdef DSD_NEO_TEST_RTL_WRAP
    rc |= test_rx_tone_rigctl_scan_step();
    rc |= test_rigctl_reconnect_forgets_the_legacy_tune_cache();
    rc |= test_tone_rejection_steps_the_legacy_scan();
    rc |= test_tone_rejection_stays_on_a_single_row();
    rc |= test_tone_rejection_that_ended_steps_the_legacy_scan();
    rc |= test_tone_check_holds_the_legacy_scan_while_the_verdict_is_pending();
    rc |= test_allowed_traffic_holds_the_legacy_scan_before_its_block_ends();
    rc |= test_tone_check_leaves_no_hangtime_tail();
#endif
#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    rc |= test_rx_tone_resets_on_legacy_scan_step();
    rc |= test_typed_scan_nfm_row_tone_rejection_steps();
    rc |= test_typed_scan_tone_rejection_with_a_refused_row();
    rc |= test_typed_scan_tone_rejection_weighs_an_am_row();
    rc |= test_typed_scan_tone_rejection_weighs_same_frequency_rows();
    rc |= test_typed_scan_respelled_policy_rejects_alike();
    rc |= test_typed_scan_am_row_ignores_the_tone_filter();
    rc |= test_typed_scan_tune_boundaries();
    rc |= test_typed_scan_nfm_rows_switch_family(0);
    rc |= test_typed_scan_nfm_rows_switch_family(1);
    rc |= test_trunk_scan_nfm_target_then_trunked_target();
    rc |= test_visit_cap_scanner_hops();
    rc |= test_typed_scan_digital_row_after_nfm_timed_for_digital();
    rc |= test_trunk_scan_digital_target_after_nfm_timed_for_digital();
    rc |= test_trunk_scan_moves_on_while_an_nfm_retune_is_in_flight(DSD_TRUNK_SCAN_CONTROL_ADVANCE,
                                                                    "advance while an nfm retune is in flight");
    rc |= test_trunk_scan_moves_on_while_an_nfm_retune_is_in_flight(DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE,
                                                                    "avoid while an nfm retune is in flight");
    rc |= test_trunk_scan_coalesced_retune_behind_an_nfm_retune();
    rc |= test_trunk_scan_failed_nfm_retune_with_a_width_edit_queued();
    rc |= test_trunk_scan_advance_with_a_width_edit_queued();
    rc |= test_trunk_scan_p25_modulation_stands_after_nfm();
    rc |= test_trunk_scan_dmr_target_stays_fsk_after_nfm();
    rc |= test_trunk_scan_p25_target_without_modulation_behind_an_nfm_retune();
    rc |= test_trunk_scan_nfm_retune_fails_after_the_target_was_timed();
    rc |= test_trunk_scan_resume_behind_an_outstanding_digital_family_retune();
    rc |= test_scoped_mode_change_on_nfm_row_times_the_baseline_for_digital();
    rc |= test_typed_scan_refused_width_skipped_without_the_stream();
    rc |= test_typed_scan_width_skipped_under_channel_lpf_override();
    rc |= test_typed_scan_am_default_width_skipped_without_the_stream();
    rc |= test_typed_scan_analog_rows_hold_on_carrier();
    rc |= test_trunk_cache_with_mode_metadata();
    rc |= test_dmr_explicit_return_destination();
    rc |= test_fsk_reacquire_watchdog_runs_on_the_decode_clock();
    rc |= test_fsk_reacquire_cooldown_runs_across_a_replay_leave();
#endif

    if (rc == 0) {
        printf("ENGINE_NO_CARRIER_RESET: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
