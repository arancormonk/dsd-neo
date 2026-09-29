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
#include <dsd-neo/engine/channel_scan.h>
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
 * between FM and AM, the analog profile a republish or a CQPSK toggle back to the monitor asks for, or a scan leave's
 * return to the monitor (svc_leave_channel_scan()). Its number, the kind and the configured width of that kind it
 * carried, and the configured width of that kind from before the change that made it (-1: it changed none), until
 * svc_take_monitor_request_outcome() collects what became of it. Every request in this file goes through the stream's
 * queue, which is last-writer-wins, so only the last one says what the front end is headed for. A request made while
 * the one before it had not reached the front end (still queued, which the new one replaces, or refused and not yet
 * collected) also keeps the configured width from before that earlier change (first_configured_before_hz): the front
 * end ran neither, so a refusal of the new one may leave it on the width from before both (issue #526). That width is
 * kept for each analog kind apart (issue #524), from the first change of that kind, whether a request of its own
 * carried it or it asked the front end for nothing (a width of the kind not in force,
 * symbol_profile_note_width_change()): a refusal puts back only a width of the kind the front end kept, and a width of
 * one kind is never the baseline of the other.
 *
 * A scan leave's return to the monitor is recorded with scan_leave set (issue #578), and so is a request that replaces
 * it before it reached the front end (a width change made while it was still queued), and the kind's default a refused
 * leave falls back on (svc_publish_symbol_profile_after_scan_leave()), with the width from before that fallback: a
 * refusal then reconciles the decoder with what the front end kept after a scan, rather than only put a width back. A
 * leave the front end refused at once queued nothing (refused_at_once): seq is then the last request queued before it,
 * and what the front end kept is what it publishes once that one has settled. A request queued after the recorded one
 * that does not take the record over (a CQPSK toggle on, a symbol profile) supersedes it, whenever the refusal comes:
 * that request decides the front end, and a refused leave is left to it (svc_monitor_refusal::superseded).
 *
 * The record goes with the stream the request was made of (stream_starts: svc_rtl_start_count() then). A stream
 * app-control starts since (a restart, a reopen, an input switch or the restart of a rollback) opens on the configured
 * options, and its open settles whatever the old stream left queued and forgets a refusal it recorded
 * (rtl_stream_receive_request_outcome()), so the record says nothing of it: it reads as taken, never as not run, and a
 * leave refused at once is not read against what the new stream publishes.
 */
static struct {
    int pending;
    uint32_t seq;
    int kind;
    int width_hz;
    int configured_before_hz;
    int first_configured_before_hz[2]; /* by dsd_analog_demod; -1: none */
    int scan_leave;
    int refused_at_once;
    unsigned int stream_starts;
} g_monitor_request = {0, 0U, 0, 0, 0, {-1, -1}, 0, 0, 0U};

/* A running RTL-family stream the decoder's requests reach. */
static int
symbol_profile_rtl_running(const dsd_opts* opts, const dsd_state* state) {
    return opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx;
}

/* Whether the last request queued here went to the stream running now: none app-control started since replaced it. */
static int
symbol_profile_record_on_this_stream(void) {
    return g_monitor_request.stream_starts == svc_rtl_start_count();
}

/* Whether the last request queued here has not reached the front end: still queued, or refused (where it landed, or at
   once by a scan leave) and not yet collected (svc_take_monitor_request_outcome()), by the stream running now. One the
   stream settled was taken there, and its width ran; a stream started since opened on the configured options. */
static int
symbol_profile_earlier_not_run(void) {
    return g_monitor_request.pending && symbol_profile_record_on_this_stream()
           && (g_monitor_request.refused_at_once
               || rtl_stream_receive_request_outcome(g_monitor_request.seq) != RTL_STREAM_RX_REQUEST_SETTLED);
}

/* A new record for the request just made (@p seq), replacing the last one. @p earlier_not_run, read before it was made,
   says whether the last one had not reached the front end: the baselines it kept then stand for this one too. */
static void
symbol_profile_record_request(const dsd_opts* opts, uint32_t seq, int configured_before_hz, int earlier_not_run) {
    if (!earlier_not_run) {
        g_monitor_request.first_configured_before_hz[DSD_ANALOG_DEMOD_FM] = -1;
        g_monitor_request.first_configured_before_hz[DSD_ANALOG_DEMOD_AM] = -1;
    }
    if (dsd_analog_demod_is_valid(opts->analog_demod)
        && g_monitor_request.first_configured_before_hz[opts->analog_demod] < 0) {
        g_monitor_request.first_configured_before_hz[opts->analog_demod] = configured_before_hz;
    }
    g_monitor_request.pending = 1;
    g_monitor_request.seq = seq;
    g_monitor_request.stream_starts = svc_rtl_start_count();
    g_monitor_request.kind = opts->analog_demod;
    g_monitor_request.width_hz = dsd_opts_analog_width_hz(opts);
    g_monitor_request.configured_before_hz = configured_before_hz;
}

/* A change of the configured width of analog @p kind from @p configured_before_hz (-1: none) that asked the front end for
   nothing (svc_publish_analog_bandwidth()), or that a config apply made (svc_note_analog_width_change()): while the last
   request has not reached the front end, the width from before it is that kind's baseline, unless an earlier change of
   that kind set one. */
static void
symbol_profile_note_width_change(int kind, int configured_before_hz) {
    if (configured_before_hz < 0 || !dsd_analog_demod_is_valid(kind)
        || g_monitor_request.first_configured_before_hz[kind] >= 0 || !symbol_profile_earlier_not_run()) {
        return;
    }
    g_monitor_request.first_configured_before_hz[kind] = configured_before_hz;
}

/* The analog monitor with the configured kind and channel width. Entering it turns CQPSK off. @p configured_before_hz
   is the configured width of that kind from before the change the request carries (-1: it carries none). A request
   that replaces a scan leave's return to the monitor before it reached the front end carries that return on (a width
   set right after the scanner stopped), and so does one made for a refused leave (@p scan_leave: the kind's default it
   falls back on): its refusal is reconciled as the leave's would have been. Returns the request's result: -1 when the
   front end refused it at the rate it publishes now, which leaves the record as it was. */
static int
symbol_profile_request_monitor(const dsd_opts* opts, int configured_before_hz, int scan_leave) {
    /* Read before this request is queued: one the stream settled by then was taken there, and its width ran. */
    const int earlier_not_run = symbol_profile_earlier_not_run();
    const int carries_leave = scan_leave || (earlier_not_run && g_monitor_request.scan_leave);
    if (rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, opts->analog_demod, dsd_opts_analog_width_hz(opts))
        != 0) {
        return -1;
    }
    symbol_profile_record_request(opts, rtl_stream_receive_request_seq(), configured_before_hz, earlier_not_run);
    g_monitor_request.scan_leave = carries_leave;
    g_monitor_request.refused_at_once = 0;
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
   before a change the caller made to the width it publishes (-1: none), which the analog monitor request records, and
   @p scan_leave set for the default a refused scan leave falls back on, which that request carries the leave on for. */
static int
symbol_profile_publish(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile,
                       int configured_before_hz, int scan_leave) {
#ifndef USE_RADIO
    (void)configured_before_hz;
    (void)scan_leave;
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
        return symbol_profile_request_monitor(opts, configured_before_hz, scan_leave);
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
    return symbol_profile_publish(opts, state, profile, -1, 0);
}

int
svc_publish_symbol_profile_changing_width(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile,
                                          int configured_before_hz) {
    return symbol_profile_publish(opts, state, profile, configured_before_hz, 0);
}

int
svc_publish_symbol_profile_after_scan_leave(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile,
                                            int configured_before_hz) {
    return symbol_profile_publish(opts, state, profile, configured_before_hz, 1);
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
    return symbol_profile_request_monitor(opts, configured_before_hz, 0);
#else
    (void)opts;
    (void)state;
    (void)kind;
    (void)configured_before_hz;
    return 0;
#endif
}

void
svc_note_analog_width_change(const dsd_opts* opts, const dsd_state* state, int kind, int configured_before_hz) {
#ifdef USE_RADIO
    if (opts && state && symbol_profile_rtl_running(opts, state)) {
        symbol_profile_note_width_change(kind, configured_before_hz);
    }
#else
    (void)opts;
    (void)state;
    (void)kind;
    (void)configured_before_hz;
#endif
}

int
svc_leave_channel_scan(dsd_opts* opts, dsd_state* state) {
#ifdef USE_RADIO
    const int running = opts && state && symbol_profile_rtl_running(opts, state);
    /* Read before the leave queues its request, as symbol_profile_request_monitor() reads it before its own: the demod
       thread can take an earlier request and the leave's together, and that earlier one ran no more than this does. */
    const int earlier_not_run = running && symbol_profile_earlier_not_run();
    const int result = dsd_engine_channel_scan_leave(opts, state);
    /* A leave that asked the monitor for nothing (a digital session, or a second leave with no scan left) keeps the
       record of the last request that did. */
    if (!running || result == 0 || !dsd_opts_is_analog_family(opts)) {
        return result;
    }
    /* Queued, the request is the last one the leave made; refused at once, it queued nothing, and the number names the
       request before it, whose effect is what the front end kept. */
    symbol_profile_record_request(opts, rtl_stream_receive_request_seq(), -1, earlier_not_run);
    g_monitor_request.scan_leave = 1;
    g_monitor_request.refused_at_once = result < 0 ? 1 : 0;
    return result;
#else
    return dsd_engine_channel_scan_leave(opts, state);
#endif
}

#ifdef USE_RADIO
/* What the front end kept when a scan leave's return to the monitor was refused at once, which queued nothing: what it
   publishes, once the requests queued before it have settled, filled into @p kept. The monitor output publishes its
   kind and effective width, not the width setting, so a width the channel filter sets reads as that explicit width
   (the kind's default design included) and one the DSP rate limits as the kind's default. Off the monitor (a typed
   digital row's channel profile, CQPSK under the analog family) the front end publishes no kind, and the request's
   stands for it. Returns 0 when the front end runs what the leave asked for after all (a retune in flight, or a request
   queued before the leave, left it on a rate and a monitor that run it): nothing was refused. A leave that asked for
   the kind's default asked for its default design, so a monitor of that kind whose channel filter runs that design
   (the AM default always does, published as 6 kHz) runs it, as one the DSP rate limits does. A stream started since is
   never read here: the record went with the stream it was made of (svc_take_monitor_request_outcome()). */
static int
symbol_profile_published_kept(svc_monitor_refusal* kept) {
    int kind = g_monitor_request.kind;
    int width_hz = 0;
    int lpf_on = 0;
    kept->kept_monitor = rtl_stream_get_analog_profile(&kind, &width_hz, &lpf_on) == 1 ? 1 : 0;
    kept->kept_analog = (kept->kept_monitor || rtl_stream_analog_family_active()) ? 1 : 0;
    kept->kept_kind = kept->kept_monitor ? kind : g_monitor_request.kind;
    kept->kept_width_hz = (kept->kept_monitor && lpf_on) ? width_hz : 0;
    const int asked_hz = g_monitor_request.width_hz;
    const int runs_asked =
        kept->kept_monitor && kept->kept_kind == g_monitor_request.kind
        && (kept->kept_width_hz == asked_hz
            || (asked_hz == 0 && kept->kept_width_hz == dsd_analog_width_default_hz(g_monitor_request.kind)));
    return !runs_asked;
}

/* What the front end kept when it refused the recorded request, at once or where it landed (@p outcome, settled or
   refused), filled into @p refusal with what the request carried. Returns 0 when it did not refuse it after all. */
static int
symbol_profile_read_refusal(int outcome, svc_monitor_refusal* refusal) {
    refusal->kind = g_monitor_request.kind;
    refusal->width_hz = g_monitor_request.width_hz;
    refusal->kept_analog = 1;
    refusal->kept_kind = g_monitor_request.kind;
    refusal->scan_leave = g_monitor_request.scan_leave;
    if (g_monitor_request.refused_at_once) {
        return symbol_profile_published_kept(refusal);
    }
    return outcome == RTL_STREAM_RX_REQUEST_REFUSED
           && rtl_stream_receive_request_refusal(g_monitor_request.seq, &refusal->kept_analog, &refusal->kept_width_hz,
                                                 &refusal->kept_kind, &refusal->kept_monitor);
}
#endif

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
    if (!symbol_profile_record_on_this_stream()) {
        /* A stream app-control started since opened on the configured options: nothing the old stream did with the
           request is left to reconcile. */
        g_monitor_request.pending = 0;
        return SVC_MONITOR_REQUEST_TAKEN;
    }
    const int outcome = rtl_stream_receive_request_outcome(g_monitor_request.seq);
    if (outcome == RTL_STREAM_RX_REQUEST_PENDING) {
        return SVC_MONITOR_REQUEST_NONE;
    }
    g_monitor_request.pending = 0;
    svc_monitor_refusal refusal = {0};
    if (!symbol_profile_read_refusal(outcome, &refusal)) {
        return SVC_MONITOR_REQUEST_TAKEN;
    }
    /* A request queued after the one recorded, from anywhere (a CQPSK toggle, a symbol profile), whether the demod
       thread took it with the refused one or has yet to, decides what the front end runs from here. One that replaced
       the recorded request before it reached the front end took its record over, so the numbers differ only for a
       request that did not. */
    refusal.superseded = rtl_stream_receive_request_seq() != g_monitor_request.seq ? 1 : 0;
    refusal.kept_analog = refusal.kept_analog ? 1 : 0;
    refusal.kept_monitor = refusal.kept_monitor ? 1 : 0;
    refusal.configured_before_hz = symbol_profile_configured_width_run(refusal.kept_width_hz);
    if (out) {
        *out = refusal;
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
    /* Back onto the analog monitor: through the analog profile alone, which carries the configured kind and channel
       width, rather than returning to the width the monitor had when CQPSK was switched on. A width set meanwhile
       waited for this (svc_publish_analog_bandwidth()). Entering the monitor turns CQPSK off, so no CQPSK-off profile
       is queued with it: the demod thread could take one on its own at a block boundary before the analog request was
       queued, and a refusal of that request would then leave the FSK channel profile on the monitor output with the FM
       discriminator, which no AM signal survives. A request the front end refuses, at once (its rate cannot filter the
       width, the AM default included) or where it lands (a retune moved the rate since), leaves CQPSK on, and the
       refusal is logged with the validator's text. */
    if (!cqpsk && opts && dsd_opts_is_analog_family(opts)) {
        (void)symbol_profile_request_monitor(opts, -1, 0);
        return;
    }
    (void)rtl_stream_request_demod_profile(cqpsk, 0, 0, -1, -1, 0);
#else
    (void)opts;
#endif
}
