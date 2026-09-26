// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Publishing a decoder symbol profile, the analog channel width and the DSP menu's CQPSK toggle to the SPS hunt
 * and the RTL front end.
 *
 * Its own translation unit rather than part of menu_services.c so that the
 * hermetic unit tests over a single command-handler source can link it without
 * dragging in CSV import, rigctl and the P25 watchdog.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "services.h"

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <stdint.h>

/* Whether the decoder's configured mode, not a scan row's constraint over it, is digital. A typed digital scan row on
   an analog session runs its symbol profile on the analog family's monitor output, as it always has; only a digital
   configured mode moves the front end off the analog family. */
static int
symbol_profile_configured_digital(const dsd_opts* opts, const dsd_state* state) {
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    const int analog_only = configured ? configured->analog_only : opts->analog_only;
    return (analog_only == 1 && opts->m17encoder != 1) ? 0 : 1;
}

/*
 * The last analog monitor request queued here (decoder thread only): a width change, a switch onto the analog family or
 * between FM and AM, the analog profile a republish or a CQPSK toggle back to the monitor asks for. Its number, the kind
 * and the configured width of that kind it carried, and the configured width of that kind from before the change that
 * made it (-1: it changed none), until svc_take_monitor_request_outcome() collects what became of it. Every request in
 * this file goes through the stream's queue, which is last-writer-wins, so only the last one says what the front end
 * is headed for. A request made while the one before it had not reached the front end (still queued, which the new one
 * replaces, or refused and not yet collected) also keeps the configured width from before that earlier change
 * (first_configured_before_hz): the front end ran neither, so a refusal of the new one may leave it on the width from
 * before both (issue #526). That width is kept for each analog kind apart (issue #524), from the first change of that
 * kind, whether a request of its own carried it or it asked the front end for nothing (a width of the kind not in force,
 * symbol_profile_note_width_change()): a refusal puts back only a width of the kind the front end kept, and a width of
 * one kind is never the baseline of the other.
 */
static struct {
    int pending;
    uint32_t seq;
    int kind;
    int width_hz;
    int configured_before_hz;
    int first_configured_before_hz[2]; /* by dsd_analog_demod; -1: none */
} g_monitor_request = {0, 0U, 0, 0, 0, {-1, -1}};

/* A running RTL-family stream the decoder's requests reach. */
static int
symbol_profile_rtl_running(const dsd_opts* opts, const dsd_state* state) {
    return opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx;
}

/* Whether the last request queued here has not reached the front end: still queued, or refused and not yet collected
   (svc_take_monitor_request_outcome()). One the stream settled was taken there, and its width ran. */
static int
symbol_profile_earlier_not_run(void) {
    return g_monitor_request.pending
           && rtl_stream_receive_request_outcome(g_monitor_request.seq) != RTL_STREAM_RX_REQUEST_SETTLED;
}

/* A change of the configured width of analog @p kind from @p configured_before_hz (-1: none) that asked the front end for
   nothing (svc_publish_analog_bandwidth()): while the last request has not reached the front end, the width from
   before it is that kind's baseline, unless an earlier change of that kind set one. */
static void
symbol_profile_note_width_change(int kind, int configured_before_hz) {
    if (configured_before_hz < 0 || !dsd_analog_demod_is_valid(kind)
        || g_monitor_request.first_configured_before_hz[kind] >= 0 || !symbol_profile_earlier_not_run()) {
        return;
    }
    g_monitor_request.first_configured_before_hz[kind] = configured_before_hz;
}

/* The analog monitor with the configured kind and channel width. Entering it turns CQPSK off. @p configured_before_hz
   is the configured width of that kind from before the change the request carries (-1: it carries none). Returns the
   request's result: -1 when the front end refused it at the rate it publishes now. */
static int
symbol_profile_request_monitor(const dsd_opts* opts, int configured_before_hz) {
    /* Read before this request is queued: one the stream settled by then was taken there, and its width ran. */
    const int earlier_not_run = symbol_profile_earlier_not_run();
    if (rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, opts->analog_demod, dsd_opts_analog_width_hz(opts))
        != 0) {
        return -1;
    }
    if (!earlier_not_run) {
        g_monitor_request.first_configured_before_hz[DSD_ANALOG_DEMOD_FM] = -1;
        g_monitor_request.first_configured_before_hz[DSD_ANALOG_DEMOD_AM] = -1;
    }
    if (dsd_analog_demod_is_valid(opts->analog_demod)
        && g_monitor_request.first_configured_before_hz[opts->analog_demod] < 0) {
        g_monitor_request.first_configured_before_hz[opts->analog_demod] = configured_before_hz;
    }
    g_monitor_request.pending = 1;
    g_monitor_request.seq = rtl_stream_receive_request_seq();
    g_monitor_request.kind = opts->analog_demod;
    g_monitor_request.width_hz = dsd_opts_analog_width_hz(opts);
    g_monitor_request.configured_before_hz = configured_before_hz;
    return 0;
}

/* The configured width of the last request's kind the front end ran when it refused that request, which kept
   @p kept_width_hz: the one from before the request's own change when that is the width kept (the earlier change it
   followed had landed after all), else the one from before the first change of that kind the front end ran none of
   (the same when none came before it). The caller restores it only when the front end kept that kind
   (svc_restore_analog_width()). */
static int
symbol_profile_configured_width_run(int kept_width_hz) {
    if (g_monitor_request.configured_before_hz >= 0 && kept_width_hz == g_monitor_request.configured_before_hz) {
        return g_monitor_request.configured_before_hz;
    }
    return dsd_analog_demod_is_valid(g_monitor_request.kind)
               ? g_monitor_request.first_configured_before_hz[g_monitor_request.kind]
               : -1;
}

/* The symbol profile a digital mode runs on: the CQPSK family for @p rf_mod 1, otherwise the FSK discriminator. The
   clamp mirrors the no-override setter this replaced. */
static void
symbol_profile_request_symbols(const dsd_opts* opts, const dsd_state* state, dsd_decode_mode_profile profile,
                               int rf_mod) {
    const int ted_sps = state->samplesPerSymbol < 2 ? 2 : state->samplesPerSymbol;
    (void)rtl_stream_request_demod_profile(
        rf_mod == 1, profile.symbol_rate_hz, profile.levels,
        dsd_rtl_channel_profile_for(opts, profile.symbol_rate_hz, profile.levels, rf_mod), ted_sps, 0);
}
#endif

/* Note the configured digital decode modes with the RTL front end (rtl_stream_set_digital_decode_modes()). Once a live
   switch has moved it onto the digital family, the options it opened with no longer name the modes it runs (a -fA
   session's name none), and these pick the FSK channel profile a CQPSK toggle returns to, as an open with them would.
   Only options that are the configured ones say that: outside a scan row, or while a command updates the configuration
   under one (dsd_scan_mode_updating(), before the row's constraint is reapplied); a running row's options carry the
   row's constraint, and the configured modes it runs under were noted when they were set, or are the ones the stream
   opened with. A mode change made under a row is noted here even though the front end waits for the row's leave to
   switch to it. */
void
svc_note_digital_decode_modes(const dsd_opts* opts, const dsd_state* state) {
#ifdef USE_RADIO
    if (!opts || !state || opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx
        || dsd_scan_mode_configured_view(state) || opts->analog_only == 1) {
        return;
    }
    rtl_stream_set_digital_decode_modes(opts);
#else
    (void)opts;
    (void)state;
#endif
}

int
svc_check_mode_receive_profile(const dsd_opts* opts, const dsd_state* state, dsdneoUserDecodeMode mode) {
    if (!opts || !state || (mode != DSDCFG_MODE_ANALOG && mode != DSDCFG_MODE_AM) || opts->m17encoder == 1
        || dsd_scan_mode_updating(state)) {
        return 0;
    }
#ifdef USE_RADIO
    if (opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return 0;
    }
    /* What svc_publish_symbol_profile() will request once the preset has run: Analog selects NFM and AM the AM
       detector (dsd_apply_decode_mode_preset() sets analog_demod), each with the configured width of its kind. */
    if (mode == DSDCFG_MODE_AM) {
        return rtl_stream_check_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_AM, opts->analog_am_bandwidth_hz);
    }
    return rtl_stream_check_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, opts->analog_nfm_bandwidth_hz);
#else
    return 0;
#endif
}

/* svc_publish_symbol_profile(), with @p configured_before_hz the configured width of the analog kind in force from
   before a change the caller made to the width it publishes (-1: none), which the analog monitor request records. */
static int
symbol_profile_publish(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile,
                       int configured_before_hz) {
#ifndef USE_RADIO
    (void)configured_before_hz;
#endif
    if (!opts || !state) {
        return 0;
    }

    state->sps_hunt_idx = (int)profile.sps_profile_index;
    svc_note_digital_decode_modes(opts, state);
    if (dsd_scan_mode_updating(state)) {
        /* The saved configuration needs its new profile, but acquisition and
         * frontend changes wait until the row constraint has been reapplied. */
        return 0;
    }
    state->sps_hunt_counter = 0;

#ifdef USE_RADIO
    if (opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return 0;
    }
    /* Analog monitor has no symbol clock, so it gets the analog receive profile
       instead of a symbol profile: dsd_decode_mode_profile_for() has no entry for
       it and falls back on 4800/4, which would ask rtl_stream_set_symbol_profile()
       for the P25 C4FM filter and narrow the monitor audio to a digital channel.
       The analog request is also what moves a live digital front end onto the
       analog family, or between FM and AM, when the operator picks Analog or
       AM mid-session. Only an explicit analog width, or the AM default, can be
       refused for its rate, and every path that commits the decoder to one
       held it to that rate first, refusing with a toast and changing nothing:
       a decode-mode change or a config's [mode] onto Analog or AM (under a scan
       row too), the NFM and AM width commands, a config's [analog] width or DSP
       bandwidth, and RTL_SET_BW. The front end refuses it only when a retune
       moved the rate since: here, which the -1 returned tells the caller, or
       where it lands on the demod thread, which the request's record tells
       svc_take_monitor_request_outcome(). Either way the refusal is logged with
       the validator's text and the front end keeps its receive profile rather
       than run the width without its channel filter, and the decoder puts
       itself back to match (the width it kept, or the mode it had before a
       switch onto the monitor or between FM and AM). */
    if (dsd_opts_is_analog_family(opts)) {
        return symbol_profile_request_monitor(opts, configured_before_hz);
    }
    /* The M17 encoder rides the analog front end without being the analog family. */
    if (opts->analog_only) {
        return 0;
    }
    const int mod = state->rf_mod;
    const int configured_digital = symbol_profile_configured_digital(opts, state);
    if (configured_digital && rtl_stream_analog_family_active()) {
        /* Leaving analog: the family switch lands on the demod thread after this returns, and until then the
           output rate is the analog family's (the monitor's resampled audio rate, or the rate a CQPSK toggle or typed
           row put it on), not the rate the digital stream will run at. Time the decoder, and the front end below,
           for the digital rate. */
        const unsigned int rate_hz =
            rtl_stream_output_rate_for_family(DSD_RX_FAMILY_DIGITAL, mod == 1, profile.symbol_rate_hz);
        if (rate_hz > 0U) {
            state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, (int)rate_hz);
            state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
        }
    }
    /* Queued for the demod thread rather than written into demod state from the
       caller's thread: the digital family first (a no-op unless the front end is
       analog), then the symbol profile it runs on. */
    if (configured_digital) {
        (void)rtl_stream_request_analog_profile(DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0);
    }
    symbol_profile_request_symbols(opts, state, profile, mod);
#endif
    return 0;
}

int
svc_publish_symbol_profile(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile) {
    return symbol_profile_publish(opts, state, profile, -1);
}

int
svc_publish_symbol_profile_changing_width(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile,
                                          int configured_before_hz) {
    return symbol_profile_publish(opts, state, profile, configured_before_hz);
}

int
svc_publish_analog_bandwidth(const dsd_opts* opts, const dsd_state* state, int kind, int configured_before_hz) {
#ifdef USE_RADIO
    if (!opts || !state || !symbol_profile_rtl_running(opts, state)) {
        return 0;
    }
    /* The options in force decide. Under a scan row's suspended scope (a scoped command) they are the configured ones,
       not the row's, so the request waits for the row's constraint to be back: apply_cmd_scoped() publishes after the
       resume, and a typed digital row keeps its own profile until its leave requests the analog profile with this
       width. The width of the other kind waits for a switch to it. The M17 encoder rides the monitor output without
       being the analog family. A change that asks for nothing still counts for a refusal of a request of its kind
       that the front end has yet to reach (symbol_profile_note_width_change()). */
    if (!dsd_opts_is_analog_family(opts) || opts->analog_demod != kind || dsd_scan_mode_updating(state)) {
        symbol_profile_note_width_change(kind, configured_before_hz);
        return 0;
    }
    /* CQPSK toggled on under an analog preset from the DSP menu holds the front end off the monitor on purpose, queued
       or taken: turning it off requests the analog profile with this width (svc_toggle_rtl_cqpsk()). Otherwise the
       front end is headed for the monitor whether or not it has reached it: a switch onto the analog family, a CQPSK
       toggle back to it or a scan row's leave, still queued, has its width replaced, since the requests are
       last-writer-wins. The stream answers for every request queued, whoever queued it
       (rtl_stream_requested_cqpsk()). */
    if (rtl_stream_requested_cqpsk()) {
        symbol_profile_note_width_change(kind, configured_before_hz);
        return 0;
    }
    return symbol_profile_request_monitor(opts, configured_before_hz);
#else
    (void)opts;
    (void)state;
    (void)kind;
    (void)configured_before_hz;
    return 0;
#endif
}

int
svc_take_monitor_request_outcome(const dsd_opts* opts, const dsd_state* state, svc_monitor_refusal* out) {
#ifdef USE_RADIO
    if (!g_monitor_request.pending) {
        return SVC_MONITOR_REQUEST_NONE;
    }
    if (!opts || !state || !symbol_profile_rtl_running(opts, state)) {
        g_monitor_request.pending = 0; /* the stream it went to is gone; the next start opens on the options */
        return SVC_MONITOR_REQUEST_NONE;
    }
    const int outcome = rtl_stream_receive_request_outcome(g_monitor_request.seq);
    if (outcome == RTL_STREAM_RX_REQUEST_PENDING) {
        return SVC_MONITOR_REQUEST_NONE;
    }
    g_monitor_request.pending = 0;
    int kept_analog = 1;
    int kept_width_hz = 0;
    int kept_kind = g_monitor_request.kind;
    if (outcome != RTL_STREAM_RX_REQUEST_REFUSED
        || !rtl_stream_receive_request_refusal(g_monitor_request.seq, &kept_analog, &kept_width_hz, &kept_kind)) {
        return SVC_MONITOR_REQUEST_TAKEN;
    }
    if (out) {
        out->kind = g_monitor_request.kind;
        out->width_hz = g_monitor_request.width_hz;
        out->kept_analog = kept_analog ? 1 : 0;
        out->kept_kind = kept_kind;
        out->kept_width_hz = kept_width_hz;
        out->configured_before_hz = symbol_profile_configured_width_run(kept_width_hz);
    }
    return SVC_MONITOR_REQUEST_REFUSED;
#else
    (void)opts;
    (void)state;
    (void)out;
    return SVC_MONITOR_REQUEST_NONE;
#endif
}

void
svc_toggle_rtl_cqpsk(const dsd_opts* opts) {
#ifdef USE_RADIO
    /* Flips what the front end was last asked for, which a toggle drained right after another is not yet running. The
       family flip is queued for the demod thread; the symbol profile (rate<=0) and timing (ted_sps<0) stay. */
    const int cqpsk = rtl_stream_requested_cqpsk() ? 0 : 1;
    /* Back onto the analog monitor: through the analog profile, which carries the configured kind and channel width,
       rather than returning to the width the monitor had when CQPSK was switched on. A width set meanwhile waited for
       this (svc_publish_analog_bandwidth()). Once accepted, the analog request replaces the demod profile queued
       first, and entering the monitor turns CQPSK off. A CQPSK-off profile left on its own would put the FSK channel
       profile on the monitor output with the FM discriminator, which no AM signal survives, so a request the front end
       refuses (its rate cannot filter the width, the AM default included) leaves CQPSK on instead, as one refused where
       it lands does (a retune moved the rate since): checked before anything is queued, and should the rate move
       between that check and the request, the CQPSK-on profile goes back in place of the CQPSK-off one. Every refusal
       is logged with the validator's text. */
    const int to_monitor = !cqpsk && opts && dsd_opts_is_analog_family(opts);
    if (to_monitor
        && rtl_stream_check_analog_profile(DSD_RX_FAMILY_ANALOG, opts->analog_demod, dsd_opts_analog_width_hz(opts))
               != 0) {
        return;
    }
    if (rtl_stream_request_demod_profile(cqpsk, 0, 0, -1, -1, 0) != 0) {
        return;
    }
    if (to_monitor && symbol_profile_request_monitor(opts, -1) != 0) {
        (void)rtl_stream_request_demod_profile(1, 0, 0, -1, -1, 0);
    }
#else
    (void)opts;
#endif
}
