// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/core/analog_mode.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/analog_sink.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/squelch.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "scan_landing.h"
#include "scan_owner.h"

/* The call's per-call recording. A stop or a folder change closes it and the call's next block opens another, so it
   lives inside the call rather than with it. */
typedef struct {
    SNDFILE* wav;
    char path[1024];
    char dir[512]; /* The folder the recording was opened in, where it is published (issue #708). */
    time_t opened; /* the wall second of its first block's first sample (issue #667) */
    int attempted;
    /* The open file was written: a pause that begins now lies inside it, so it is owed the pause's silence (issue
       #756). A file opened later starts with no silence for a pause it did not span. */
    int spans_pause;
} analog_call_recording;

typedef struct {
    analog_call_recording rec;
    /* A call: the reception ran DSD_ANALOG_CALL_MIN_MS (issue #667 D1), so its canonical call and row exist. */
    int active;
    /* Issue #667 D1: a candidate, a reception not yet that long. It is never observed (no row, no live call, no
       alert), and its recording, opened at its first RECEPTION block, is discarded unpublished if it ends first.
       Before its first RECEPTION block (`received`) it is a lead-in of tone-check blocks under a carrier: their spans
       count toward the minimum, and nothing is recorded. */
    int candidate;
    int received;
    double start_m;       /* decode-mono stamp of the reception's first sample: its first block's stamp less its span */
    double start_wall;    /* the same instant in decode-clock wall seconds, never truncated */
    uint64_t qual48;      /* the candidate's RECEPTION and lead-in spans in 48 kHz samples (held spans do not count) */
    uint64_t qual_rem48;  /* the part of a 48 kHz sample carried from one of those spans to the next */
    int64_t frequency_hz; /* the frequency at the reception's first block (analog_call_frequency_hz()) */
    /* Issue #756: the call waits through a pause the max gap may still join. It stays active meanwhile (its row too),
       and is decided on sample time alone: the blocks that arrive, never a clock. A candidate past its first RECEPTION
       block is held the same way. */
    int held;
    int credited;    /* the hold began as the carrier closed, so the hangover before it counts toward the gap */
    uint64_t age48;  /* the gap so far, in 48 kHz samples: the held blocks' spans, plus the hangover when credited */
    uint64_t owed48; /* the held blocks' spans: the silence that keeps the recording's timing when the call goes on */
    uint64_t rem48;  /* the part of a 48 kHz sample carried from one held block's span to the next, at the block rate */
    /* Decode-mono end of the last RECEPTION block: where the call ends, whenever its end is decided. 0 before one. */
    double last_m;
    uint32_t boundaries_seen; /* the receiver's boundaries epoch when the reception began; a move ends it */
    /* The last identity confirmed on the reception (any, before one is confirmed); a candidate's BEGIN is seeded with
       it, never by rewriting the receiver's publication. */
    dsd_receive_identity identity;
    uint8_t mode; /* dsd_analog_mode the reception began in, as its row has it: its recording's name and log label */
    /* Issue #667 D2: the notice last logged about the squelch (analog_call_allowed()), so each change is said once. */
    uint8_t off_notice;
} analog_call;

/* A teardown that skipped dsd_engine_finalize_calls() still publishes a call's open recording under its final name,
   without the history row it no longer has (and without an rdio export), instead of leaving its TEMP_ file behind. A
   candidate's is discarded: it never became a call (issue #667). */
static void
analog_call_free(void* ptr) {
    analog_call* call = (analog_call*)ptr;
    if (call->rec.wav && call->active) {
        call->rec.wav = close_and_rename_analog_wav_file(call->rec.wav, call->rec.path, call->rec.dir, call->mode);
    } else if (call->rec.wav) {
        call->rec.wav = discard_analog_wav_file(call->rec.wav, call->rec.path);
    }
    free(call);
}

static analog_call*
analog_call_get(const dsd_state* state) {
    return (analog_call*)dsd_engine_scan_owned(state, 2);
}

/* A call or a candidate is running. */
static int
analog_call_running(const analog_call* call) {
    return call && (call->active || call->candidate);
}

/* Close and publish the call's recording against the last accepted block's row, as the call's end does, but leave the
   call running. It is published in the folder it was opened in, which a config may have changed since (issue #708).
   A candidate's recording is discarded unpublished instead (issue #667 D1): it has no row to publish under. */
static void
analog_call_close_recording(const dsd_opts* opts, dsd_state* state, analog_call* call) {
    if (call->rec.wav && call->active) {
        call->rec.wav =
            close_and_publish_call_wav(call->rec.wav, opts, state, 0, call->rec.path,
                                       call->rec.dir[0] ? call->rec.dir : opts->wav_out_dir, call->rec.opened);
        call->rec.path[0] = '\0';
    } else if (call->rec.wav) {
        call->rec.wav = discard_analog_wav_file(call->rec.wav, call->rec.path);
        call->rec.path[0] = '\0';
    }
    call->rec.spans_pause = 0;
}

int
dsd_engine_analog_call_active(const dsd_state* state) {
    const analog_call* call = analog_call_get(state);
    return call && call->active;
}

int
dsd_engine_analog_call_due(const dsd_state* state) {
    return state && analog_call_running(analog_call_get(state)) && state->analog_rx.stale_after_ms
           && dsd_realtime_mono_ms() > state->analog_rx.stale_after_ms;
}

static void
analog_call_release_hold(analog_call* call) {
    call->held = 0;
    call->credited = 0;
    call->age48 = 0;
    call->owed48 = 0;
    call->rem48 = 0;
}

/* The reception is over: nothing of it is left to qualify or end. */
static void
analog_call_forget(analog_call* call) {
    call->active = 0;
    call->candidate = 0;
    call->received = 0;
    call->qual48 = 0;
    call->qual_rem48 = 0;
    call->start_m = 0.0;
    call->start_wall = 0.0;
    call->last_m = 0.0;
    call->rec.attempted = 0;
    analog_call_release_hold(call);
}

/* End the call at its last reception (B1, issue #756): a pause after it, a stalled input or a hold is not the call's.
   Nothing is written for a pause the call no longer spans. */
static void
analog_call_finish(dsd_opts* opts, dsd_state* state, analog_call* call) {
    const double ended_m = call->last_m;
    dsd_analog_sink_end_call(state);
    /* Close against the last accepted block's row before a retune or new service can replace it. */
    analog_call_close_recording(opts, state, call);
    analog_call_forget(call);
    (void)dsd_call_state_end_ex(state, 0, ended_m, DSD_CALL_END_EXPLICIT);
    dsd_event_sync_slot(opts, state, 0);
}

/* Issue #667 D1: the one way a candidate ends. It was never observed, so it leaves no row, live call or alert, and its
   temporary recording is discarded unpublished (an open or write failure it had stays logged). */
static void
analog_call_discard_candidate(const dsd_opts* opts, dsd_state* state, analog_call* call) {
    dsd_analog_sink_end_call(state);
    analog_call_close_recording(opts, state, call);
    analog_call_forget(call);
}

/* Whatever ends the reception ends a call at its last reception, or discards a candidate. */
static void
analog_call_end_reception(dsd_opts* opts, dsd_state* state, analog_call* call) {
    if (call->active) {
        analog_call_finish(opts, state, call);
    } else if (call->candidate) {
        analog_call_discard_candidate(opts, state, call);
    }
}

void
dsd_engine_analog_call_end(dsd_opts* opts, dsd_state* state) {
    analog_call* call = analog_call_get(state);
    if (!opts || !state || !analog_call_running(call)) {
        return;
    }
    analog_call_end_reception(opts, state, call);
}

/* The default max gap is the hangover itself, so a session that never sets one holds nothing (issue #756). */
_Static_assert(DSD_ANALOG_RECORD_MAX_GAP_DEFAULT_MS == DSD_ANALOG_CARRIER_HANGOVER_MS,
               "the default NFM max gap must be the carrier hangover: off");

/* The max gap in 48 kHz samples, or 0 when it is off: a gap no longer than the hangover joins nothing, and the hold is
   a recording setting, so it is off while per-call recording is (a call is then held for no file). */
static uint64_t
analog_call_max_gap48(const dsd_opts* opts) {
    const int ms = opts->analog_record_max_gap_ms;
    return opts->dmr_stereo_wav && ms > DSD_ANALOG_CARRIER_HANGOVER_MS ? (uint64_t)ms * 48U : 0U;
}

/* D10: a held call the max gap in force no longer covers (turned off, lowered below the gap so far, or per-call
   recording stopped) ends at its last reception, a held candidate unpublished. A gap equal to the max gap is still
   joined. Returns 1 when the reception ended. */
static int
analog_call_reconcile(dsd_opts* opts, dsd_state* state, analog_call* call) {
    if (!call->held) {
        return 0;
    }
    const uint64_t max_gap48 = analog_call_max_gap48(opts);
    if (max_gap48 != 0U && call->age48 <= max_gap48) {
        return 0;
    }
    analog_call_end_reception(opts, state, call);
    return 1;
}

static analog_call*
analog_call_ensure(dsd_state* state) {
    analog_call* call = analog_call_get(state);
    if (!call) {
        call = (analog_call*)calloc(1, sizeof(*call));
        if (!call) {
            return NULL;
        }
        if (dsd_engine_scan_own(state, 2, call, analog_call_free)) {
            free(call);
            return NULL;
        }
    }
    return call;
}

/* Whether the tracker follows analog receptions: the analog monitor's tap runs, FM or AM (issue #667 D8), and no
   trunking does. */
static int
analog_call_follows(const dsd_opts* opts) {
    return dsd_analog_monitor_tap_active(opts) && opts->trunk_enable == 0;
}

/* What the D2 notice last said (analog_call::off_notice): nothing, or calls paused with the remedies that work on the
   FM monitor (a tone allow list among them) or on AM (no tone to list). */
enum { ANALOG_CALL_ON = 0, ANALOG_CALL_OFF_FM = 1, ANALOG_CALL_OFF_AM = 2 };

static void
analog_call_say_off(uint8_t notice) {
    switch (notice) {
        case ANALOG_CALL_OFF_FM:
            LOG_INFO("NOTICE: Analog call history and recordings are paused: the squelch is off or at -110 dB or "
                     "below, so noise would hold one call open. Set --squelch <dB>, --squelch auto or noise, or "
                     "--tone-allow.\n");
            break;
        case ANALOG_CALL_OFF_AM:
            LOG_INFO("NOTICE: Analog call history and recordings are paused: the squelch is off or at -110 dB or "
                     "below, so noise would hold one call open. Set --squelch <dB> or --squelch auto.\n");
            break;
        default: LOG_INFO("NOTICE: Analog call history and recordings are back on.\n"); break;
    }
}

/* Issue #667 D2: whether a reception may begin or go on as a call (dsd_squelch_analog_calls_allowed()). Each change is
   said once, when it is first seen, and a change of remedies while paused (an FM monitor switched to AM) too; the
   lift is said only after a pause was. Returns 1 while calls are allowed. */
static int
analog_call_allowed(const dsd_opts* opts, const dsd_state* state, analog_call* call) {
    const int allowed = dsd_squelch_analog_calls_allowed(opts, state);
    uint8_t notice = ANALOG_CALL_ON;
    if (!allowed) {
        notice = dsd_analog_tone_detection_active(opts) ? (uint8_t)ANALOG_CALL_OFF_FM : (uint8_t)ANALOG_CALL_OFF_AM;
    }
    if (call && notice != call->off_notice) {
        analog_call_say_off(notice);
        call->off_notice = notice;
    }
    return allowed;
}

/* What the tick ends on a closed carrier or a tone still checked. With the hold on, a call or a candidate past its
   first RECEPTION block waits for the next block, which decides it on sample time; with it off, either ends. A lead-in
   is a tone check (issue #667 D1), so it ends only on a closed carrier: it has no hold. */
static int
analog_call_tick_closes(const dsd_opts* opts, const dsd_state* state, const analog_call* call, int gate) {
    if (call->candidate && !call->received) {
        return !state->analog_rx.carrier_open;
    }
    return analog_call_max_gap48(opts) == 0U && (!state->analog_rx.carrier_open || !dsd_analog_tone_gate_passes(gate));
}

void
dsd_engine_analog_call_tick(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    analog_call* call = analog_call_get(state);
    if (!analog_call_running(call)) {
        /* D2's notice follows a live change (a squelch set from a frontend) with no block to carry it. */
        if (analog_call_follows(opts)) {
            (void)analog_call_allowed(opts, state, analog_call_ensure(state));
        }
        return;
    }
    /* B6 between blocks: a reset with no block after it (a TCP reconnect that yields to a queued command) moves the
       epoch while the boundaries stay current, and the stream may bring no whole block to see it. The call, held or
       not, ends at its last reception as the next block would end it. */
    if (state->analog_rx.boundaries != call->boundaries_seen) {
        analog_call_end_reception(opts, state, call);
        return;
    }
    if (analog_call_reconcile(opts, state, call)) {
        return;
    }
    const int gate = dsd_analog_tone_gate_in_force(opts, state);
    /* What no hold survives, held or not: the receiver left the channel, the tone is rejected, or (D2) the squelch
       lets noise through, a live change that needs no further sample. */
    const int invalid = !dsd_analog_monitor_tap_active(opts) || gate == DSD_ANALOG_TONE_GATE_REJECTED
                        || !dsd_analog_rx_boundaries_current(opts, state) || !analog_call_allowed(opts, state, call);
    /* A held call with input flowing needs no tick. */
    if (invalid || analog_call_tick_closes(opts, state, call, gate) || dsd_engine_analog_call_due(state)) {
        analog_call_end_reception(opts, state, call);
    }
}

/* What the analog sink hands the recording's samples to: the open WAV and its log label, as each sink's own context
   type carries only what its writer needs (symbol_raw_wav_sink, symbol_monitor_sink), never the module's state. */
typedef struct {
    SNDFILE* wav;
    const char* label;
} analog_call_sink;

/* What a short write to the call's recording is logged as: the call by its mode, as its recording is named. */
static const char*
analog_call_label(const analog_call* call) {
    switch (call->mode) {
        case DSD_ANALOG_MODE_NFM: return "NFM call";
        case DSD_ANALOG_MODE_AM: return "AM call";
        default: return "Analog call";
    }
}

static void
analog_call_write(const void* context, const short* samples, size_t count) {
    const analog_call_sink* sink = (const analog_call_sink*)context;
    dsd_wav_segment_write(sink->wav, samples, count, sink->label);
}

void
dsd_engine_analog_call_stop_recording(const dsd_opts* opts, dsd_state* state) {
    analog_call* call = (opts && state) ? analog_call_get(state) : NULL;
    if (!call) {
        return;
    }
    /* Only the recording ends here: a held call ends at its next block or tick if recording stays off, and a folder
       change (recording back on before then) keeps it held. A candidate's recording is discarded, and the candidate
       goes on: its next block opens a new one, which it publishes if it qualifies. */
    analog_call_close_recording(opts, state, call);
    /* A start later in the same call opens a new recording at the next block. */
    call->rec.attempted = 0;
}

/* The decode-clock wall time of the decode-mono instant @p mono_s: the two clocks read together, so any file work done
   since @p mono_s was read does not move it (issue #667). */
static double
analog_call_wall_at(double mono_s) {
    const double wall_s = dsd_decode_now_realtime_s();
    return wall_s - (dsd_decode_now_mono_s() - mono_s);
}

/* Write a RECEPTION block whose first sample is at @p first_m to the call's recording, opening one first if per-call
   recording is on and none was tried since the last close. Its open stamp is that first sample's wall second, taken
   before the open: the time a slow open takes is not the recording's (issue #667). */
static void
analog_call_record(dsd_opts* opts, dsd_state* state, analog_call* call, const float* samples, size_t count,
                   int sample_rate, double first_m) {
    analog_call_recording* rec = &call->rec;
    if (!opts->dmr_stereo_wav) {
        rec->attempted = 0;
    }
    if (opts->dmr_stereo_wav && !rec->attempted) {
        rec->attempted = 1;
        rec->opened = (time_t)floor(analog_call_wall_at(first_m));
        rec->wav = open_wav_file(opts->wav_out_dir, rec->path, sizeof(rec->path), 48000, 0);
        DSD_SNPRINTF(rec->dir, sizeof(rec->dir), "%s", opts->wav_out_dir);
        rec->spans_pause = 0;
    }
    if (!opts->dmr_stereo_wav) {
        analog_call_close_recording(opts, state, call);
    }
    if (rec->wav) {
        const analog_call_sink sink = {rec->wav, analog_call_label(call)};
        (void)dsd_analog_sink_write(opts, state, DSD_ANALOG_SINK_CALL_WAV, samples, count, 1.0, sample_rate, 48000,
                                    analog_call_write, &sink);
        rec->spans_pause = 1;
    }
}

/* The pause a held call goes on after, as silence in the recording that spans it, so the file keeps time at block
   precision. A short write latches the segment (dsd_wav_segment_write()), and nothing more reaches it. */
static void
analog_call_write_pause(const dsd_opts* opts, analog_call* call) {
    static const short silence[DSD_ANALOG_SINK_CHUNK] = {0};
    const analog_call_recording* rec = &call->rec;
    if (!rec->wav || !rec->spans_pause || !opts->dmr_stereo_wav) {
        return;
    }
    for (uint64_t left = call->owed48; left > 0U;) {
        const size_t n = left < DSD_ANALOG_SINK_CHUNK ? (size_t)left : DSD_ANALOG_SINK_CHUNK;
        dsd_wav_segment_write(rec->wav, silence, n, analog_call_label(call));
        left -= n;
    }
}

/* A block of @p count samples at @p sample_rate, in 48 kHz samples: the remainder (@p rem48) carries to the next block
   of the same count. */
static uint64_t
analog_call_span48(uint64_t* rem48, size_t count, int sample_rate) {
    const uint64_t total = (uint64_t)count * 48000U + *rem48;
    *rem48 = total % (uint64_t)sample_rate;
    return total / (uint64_t)sample_rate;
}

/* Issue #756: a block with nothing to record (the carrier closed after its hangover, or its tone still checked). The
   call is held, and the block's span ages the gap and is owed as silence; the reconciliation after it ends the call
   when the gap is past the max gap, or at once with the hold off, as every such block did before. A hold that begins
   as the carrier closes is credited the hangover, which the blocks before it already recorded; one that begins as the
   tone check returns under a continuing carrier is not. A candidate past its first RECEPTION block is held alike, and
   its held spans do not count toward the minimum (issue #667). */
static void
analog_call_hold(dsd_opts* opts, dsd_state* state, analog_call* call, size_t count, int sample_rate) {
    if (!call->held) {
        analog_call_release_hold(call);
        call->held = 1;
        call->credited = !state->analog_rx.carrier_open;
        call->age48 = call->credited ? (uint64_t)DSD_ANALOG_CARRIER_HANGOVER_MS * 48U : 0U;
    }
    const uint64_t span48 = analog_call_span48(&call->rem48, count, sample_rate);
    call->age48 += span48;
    call->owed48 += span48;
    (void)analog_call_reconcile(opts, state, call);
}

/* The mode a call is heard in (issue #667 D7): the demodulator dsd-neo runs on a radio input, and unknown on audio
   input, which another receiver demodulated. */
static uint8_t
analog_call_mode(const dsd_opts* opts) {
    if (!dsd_opts_input_is_radio(opts)) {
        return (uint8_t)DSD_ANALOG_MODE_UNKNOWN;
    }
    return opts->analog_demod == DSD_ANALOG_DEMOD_AM ? (uint8_t)DSD_ANALOG_MODE_AM : (uint8_t)DSD_ANALOG_MODE_NFM;
}

/* The frequency a reception is heard on (issue #667 D9): the tuned one, or on audio input, which tunes nothing, the one
   a scan confirmed it landed its rigctl peer on (scan_landing.h); 0 when neither says. */
static int64_t
analog_call_frequency_hz(const dsd_opts* opts, const dsd_state* state) {
    const uint32_t tuned_hz = dsd_opts_tuned_freq_hz(opts);
    return (int64_t)(tuned_hz != 0U ? tuned_hz : dsd_engine_scan_landing_hz(opts, state));
}

/* A confirmed new identity (D4) ends the running reception before this block, which begins the next: a call at its
   last reception before its CONTINUE would split it inside the call state, a candidate unpublished (issue #667). An
   identity confirmed only now, or one still acquiring, changes nothing. */
static void
analog_call_identity_boundary(dsd_opts* opts, dsd_state* state, analog_call* call,
                              const dsd_receive_identity* identity) {
    if (analog_call_running(call) && dsd_receive_identity_changed(&call->identity, identity)) {
        analog_call_end_reception(opts, state, call);
    }
}

/* A block of a reception that is not a call yet (issue #667 D1), stamped @p now_m: the first begins the candidate, whose
   reception starts at the block's first sample and whose mode and frequency are taken here, once (a later landing
   never rewrites them); each counts its span toward the minimum. The candidate keeps the last identity confirmed on it
   for its BEGIN. */
static void
analog_call_admit(const dsd_opts* opts, const dsd_state* state, analog_call* call, const dsd_receive_identity* identity,
                  size_t count, int sample_rate, double now_m) {
    if (!call->candidate) {
        call->candidate = 1;
        call->start_m = now_m - (double)count / (double)sample_rate;
        call->start_wall = analog_call_wall_at(call->start_m);
        call->identity = *identity;
        call->mode = analog_call_mode(opts);
        call->frequency_hz = analog_call_frequency_hz(opts, state);
        call->boundaries_seen = state->analog_rx.boundaries;
    } else if (identity->known) {
        call->identity = *identity;
    }
    call->qual48 += analog_call_span48(&call->qual_rem48, count, sample_rate);
}

/* The call's audio up to @p now_m: in the clear and permitted, and its row rendered (which also names a seeded call). */
static void
analog_call_note_media(dsd_opts* opts, dsd_state* state, double now_m) {
    const dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_CLEAR,
        .audio_permitted = 1,
        .observed_m = now_m,
    };
    (void)dsd_call_state_update_crypto(state, 0, &crypto);
    (void)dsd_call_state_update_media(state, 0, 1, now_m);
    dsd_event_sync_slot(opts, state, 0);
}

/* Issue #667 D1: the candidate ran the minimum, so it becomes a call. Its BEGIN starts the call at the reception's
   first sample (decode-mono and wall stamps of the one instant) and is heard with the identity the candidate kept, its
   open recording going on. A BEGIN that fails (the call state could not be allocated) leaves the candidate, which
   tries again on its next RECEPTION block. */
static void
analog_call_qualify(dsd_opts* opts, dsd_state* state, analog_call* call, double now_m) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_ANALOG,
        .slot = 0,
        .kind = DSD_CALL_KIND_VOICE,
        .frequency_hz = call->frequency_hz,
        .analog_mode = call->mode,
        .has_receive_seed = 1U,
        .receive_seed = call->identity,
        .start_wall = call->start_wall,
        .observed_m = call->start_m,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) < 0) {
        return;
    }
    call->active = 1;
    call->candidate = 0;
    call->received = 0;
    analog_call_note_media(opts, state, now_m);
}

/* A call's RECEPTION block, stamped @p now_m. The last confirmed tone stays the call's until the call ends: a block
   whose tone is still being acquired names no service of its own, and a later confirmed tone is compared with the
   confirmed one before it. */
static void
analog_call_continue(dsd_opts* opts, dsd_state* state, analog_call* call, const dsd_receive_identity* identity,
                     double now_m) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_ANALOG,
        .slot = 0,
        .kind = DSD_CALL_KIND_VOICE,
        .frequency_hz = call->frequency_hz,
        .analog_mode = call->mode,
        .observed_m = now_m,
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
    if (identity->known) {
        call->identity = *identity;
    }
    analog_call_note_media(opts, state, now_m);
}

/* A RECEPTION block, stamped @p now_m: the call's audio, or a candidate's, which becomes a call on the block that
   brings it to the minimum. A held reception otherwise goes on, its pause written first. */
static void
analog_call_receive(dsd_opts* opts, dsd_state* state, analog_call* call, const float* samples, size_t count,
                    int sample_rate, double now_m) {
    dsd_receive_identity identity;
    dsd_receive_identity_current(state, DSD_SYNC_ANALOG, 0, &identity);
    analog_call_identity_boundary(opts, state, call, &identity);
    if (call->held) {
        analog_call_write_pause(opts, call);
        analog_call_release_hold(call);
    }
    if (call->active) {
        analog_call_continue(opts, state, call, &identity, now_m);
    } else {
        analog_call_admit(opts, state, call, &identity, count, sample_rate, now_m);
        call->received = 1;
        if (call->qual48 >= (uint64_t)DSD_ANALOG_CALL_MIN_MS * 48U) {
            analog_call_qualify(opts, state, call, now_m);
        }
    }
    analog_call_record(opts, state, call, samples, count, sample_rate, now_m - (double)count / (double)sample_rate);
    call->last_m = now_m;
}

/* A block no call spans: BREAK, a block that is not one, trunking, no monitor tap (FM or AM), or (B6) the receiver
   crossed a hard boundary since the reception began. The block that carries a moved epoch straddles it, its first part
   the old reception, so it is not written either: the call ends at its last reception and the next block begins the
   next. */
static int
analog_call_breaks(const dsd_opts* opts, const dsd_state* state, const analog_call* call, const float* samples,
                   size_t count, int sample_rate, int input) {
    if ((input != DSD_ANALOG_CALL_RECEPTION && input != DSD_ANALOG_CALL_HOLDABLE && input != DSD_ANALOG_CALL_PENDING)
        || !samples || count == 0 || sample_rate <= 0 || !analog_call_follows(opts)) {
        return 1;
    }
    return analog_call_running(call) && state->analog_rx.boundaries != call->boundaries_seen;
}

/* A block with nothing to record: a call, or a candidate past its first RECEPTION block, holds on it (a PENDING block
   is a tone check like any other then). Before that, a PENDING block is the lead-in (issue #667 D1), and any other
   discards it. Returns 1 when the block is done with. */
static int
analog_call_unrecorded(dsd_opts* opts, dsd_state* state, analog_call* call, size_t count, int sample_rate, int input) {
    if (input == DSD_ANALOG_CALL_RECEPTION) {
        return 0;
    }
    if (call && (call->active || call->received)) {
        analog_call_hold(opts, state, call, count, sample_rate);
        /* A tone check under the carrier that ended the reception (its hold off, or past the max gap) is the next
           one's lead-in, as it is when the reconciliation before the block ends it. */
        return input != DSD_ANALOG_CALL_PENDING || analog_call_running(call);
    }
    if (input == DSD_ANALOG_CALL_PENDING) {
        return 0;
    }
    if (analog_call_running(call)) {
        analog_call_discard_candidate(opts, state, call);
    }
    return 1;
}

void
dsd_engine_analog_call_block(dsd_opts* opts, dsd_state* state, const float* samples, size_t count, int sample_rate,
                             int input) {
    if (!opts || !state) {
        return;
    }
    /* A RECEPTION or lead-in block's one decode-clock read, before any of its file work on any path: the
       reconciliation's end, a new identity's end and a resumed call's pause all close or write a recording before the
       block begins or goes on with a reception, and the time a slow disk takes for that is not the call's. The stamp
       is where the reception starts or goes on, and where it ends, whenever that is decided. A held block's stamp is
       never used: the hold runs on sample time. */
    const double now_m =
        input == DSD_ANALOG_CALL_RECEPTION || input == DSD_ANALOG_CALL_PENDING ? dsd_decode_now_mono_s() : 0.0;
    analog_call* call = analog_call_get(state);
    if (analog_call_running(call)) {
        (void)analog_call_reconcile(opts, state, call);
    }
    if (analog_call_breaks(opts, state, call, samples, count, sample_rate, input)) {
        dsd_engine_analog_call_end(opts, state);
        return;
    }
    call = analog_call_ensure(state);
    if (!call) {
        return;
    }
    /* D2: no reception begins or goes on while the squelch lets noise through. */
    if (!analog_call_allowed(opts, state, call)) {
        analog_call_end_reception(opts, state, call);
        return;
    }
    if (analog_call_unrecorded(opts, state, call, count, sample_rate, input)) {
        return;
    }
    if (input == DSD_ANALOG_CALL_PENDING) {
        dsd_receive_identity identity;
        dsd_receive_identity_current(state, DSD_SYNC_ANALOG, 0, &identity);
        analog_call_identity_boundary(opts, state, call, &identity);
        analog_call_admit(opts, state, call, &identity, count, sample_rate, now_m);
        return;
    }
    analog_call_receive(opts, state, call, samples, count, sample_rate, now_m);
}
