// SPDX-License-Identifier: GPL-3.0-or-later
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
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "scan_owner.h"

/* The call's per-call recording. A stop or a folder change closes it and the call's next block opens another, so it
   lives inside the call rather than with it. */
typedef struct {
    SNDFILE* wav;
    char path[1024];
    char dir[512]; /* The folder the recording was opened in, where it is published (issue #708). */
    time_t opened;
    int attempted;
    /* The open file was written: a pause that begins now lies inside it, so it is owed the pause's silence (issue
       #756). A file opened later starts with no silence for a pause it did not span. */
    int spans_pause;
} analog_call_recording;

typedef struct {
    analog_call_recording rec;
    int active;
    /* Issue #756: the call waits through a pause the max gap may still join. It stays active meanwhile (its row too),
       and is decided on sample time alone: the blocks that arrive, never a clock. */
    int held;
    int credited;    /* the hold began as the carrier closed, so the hangover before it counts toward the gap */
    uint64_t age48;  /* the gap so far, in 48 kHz samples: the held blocks' spans, plus the hangover when credited */
    uint64_t owed48; /* the held blocks' spans: the silence that keeps the recording's timing when the call goes on */
    uint64_t rem48;  /* the part of a 48 kHz sample carried from one held block's span to the next, at the block rate */
    /* Decode-mono end of the last RECEPTION block: where the call ends, whenever its end is decided. 0 before one. */
    double last_m;
    uint32_t boundaries_seen; /* the receiver's boundaries epoch when the call began; a move ends it */
    dsd_receive_identity identity;
} analog_call;

/* A teardown that skipped dsd_engine_finalize_calls() still publishes an open recording under its final name, without
   the history row it no longer has (and without an rdio export), instead of leaving its TEMP_ file behind. */
static void
analog_call_free(void* ptr) {
    analog_call* call = (analog_call*)ptr;
    if (call->rec.wav) {
        call->rec.wav = close_and_rename_analog_wav_file(call->rec.wav, call->rec.path, call->rec.dir);
    }
    free(call);
}

static analog_call*
analog_call_get(const dsd_state* state) {
    return (analog_call*)dsd_engine_scan_owned(state, 2);
}

/* Close and publish the call's recording against the last accepted block's row, as the call's end does, but leave the
   call running. It is published in the folder it was opened in, which a config may have changed since (issue #708). */
static void
analog_call_close_recording(const dsd_opts* opts, dsd_state* state, analog_call* call) {
    if (call->rec.wav) {
        call->rec.wav =
            close_and_publish_call_wav(call->rec.wav, opts, state, 0, call->rec.path,
                                       call->rec.dir[0] ? call->rec.dir : opts->wav_out_dir, call->rec.opened);
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
    return state && dsd_engine_analog_call_active(state) && state->analog_rx.stale_after_ms
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

/* End the call at its last reception (B1, issue #756): a pause after it, a stalled input or a hold is not the call's.
   Nothing is written for a pause the call no longer spans. */
static void
analog_call_finish(dsd_opts* opts, dsd_state* state, analog_call* call) {
    const double ended_m = call->last_m;
    call->active = 0;
    call->last_m = 0.0;
    call->rec.attempted = 0;
    analog_call_release_hold(call);
    dsd_analog_sink_end_call(state);
    /* Close against the last accepted block's row before a retune or new service can replace it. */
    analog_call_close_recording(opts, state, call);
    (void)dsd_call_state_end_ex(state, 0, ended_m, DSD_CALL_END_EXPLICIT);
    dsd_event_sync_slot(opts, state, 0);
}

void
dsd_engine_analog_call_end(dsd_opts* opts, dsd_state* state) {
    analog_call* call = analog_call_get(state);
    if (!opts || !state || !call || !call->active) {
        return;
    }
    analog_call_finish(opts, state, call);
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
   recording stopped) ends at its last reception. A gap equal to the max gap is still joined. Returns 1 when the call
   ended. */
static int
analog_call_reconcile(dsd_opts* opts, dsd_state* state, analog_call* call) {
    if (!call->held) {
        return 0;
    }
    const uint64_t max_gap48 = analog_call_max_gap48(opts);
    if (max_gap48 != 0U && call->age48 <= max_gap48) {
        return 0;
    }
    analog_call_finish(opts, state, call);
    return 1;
}

void
dsd_engine_analog_call_tick(dsd_opts* opts, dsd_state* state) {
    analog_call* call = (opts && state) ? analog_call_get(state) : NULL;
    if (!call || !call->active) {
        return;
    }
    /* B6 between blocks: a reset with no block after it (a TCP reconnect that yields to a queued command) moves the
       epoch while the boundaries stay current, and the stream may bring no whole block to see it. The call, held or
       not, ends at its last reception as the next block would end it. */
    if (state->analog_rx.boundaries != call->boundaries_seen) {
        analog_call_finish(opts, state, call);
        return;
    }
    if (analog_call_reconcile(opts, state, call)) {
        return;
    }
    const int gate = dsd_analog_tone_gate_in_force(opts, state);
    /* What no hold survives, held or not: the receiver left the channel, or the tone is rejected. */
    const int invalid = !dsd_analog_tone_detection_active(opts) || gate == DSD_ANALOG_TONE_GATE_REJECTED
                        || !dsd_analog_rx_boundaries_current(opts, state);
    /* With the hold on, a closed carrier or a tone still checked waits for the next block, which decides it on sample
       time; a held call with input flowing needs no tick. */
    const int closed =
        analog_call_max_gap48(opts) == 0U && (!state->analog_rx.carrier_open || !dsd_analog_tone_gate_passes(gate));
    if (invalid || closed || dsd_engine_analog_call_due(state)) {
        analog_call_finish(opts, state, call);
    }
}

/* What the analog sink hands the recording's samples to: the open WAV alone, as each sink's own context type carries
   only what its writer needs (symbol_raw_wav_sink, symbol_monitor_sink), never the module's state. */
typedef struct {
    SNDFILE* wav;
} analog_call_sink;

static void
analog_call_write(const void* context, const short* samples, size_t count) {
    const analog_call_sink* sink = (const analog_call_sink*)context;
    dsd_wav_segment_write(sink->wav, samples, count, "NFM call");
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

void
dsd_engine_analog_call_stop_recording(const dsd_opts* opts, dsd_state* state) {
    analog_call* call = (opts && state) ? analog_call_get(state) : NULL;
    if (!call) {
        return;
    }
    /* Only the recording ends here: a held call ends at its next block or tick if recording stays off, and a folder
       change (recording back on before then) keeps it held. */
    analog_call_close_recording(opts, state, call);
    /* A start later in the same call opens a new recording at the next block. */
    call->rec.attempted = 0;
}

static void
analog_call_record(dsd_opts* opts, dsd_state* state, analog_call* call, const float* samples, size_t count,
                   int sample_rate) {
    analog_call_recording* rec = &call->rec;
    if (!opts->dmr_stereo_wav) {
        rec->attempted = 0;
    }
    if (opts->dmr_stereo_wav && !rec->attempted) {
        rec->attempted = 1;
        rec->wav = open_wav_file(opts->wav_out_dir, rec->path, sizeof(rec->path), 48000, 0);
        DSD_SNPRINTF(rec->dir, sizeof(rec->dir), "%s", opts->wav_out_dir);
        rec->opened = dsd_decode_time();
        rec->spans_pause = 0;
    }
    if (!opts->dmr_stereo_wav) {
        analog_call_close_recording(opts, state, call);
    }
    if (rec->wav) {
        const analog_call_sink sink = {rec->wav};
        (void)dsd_analog_sink_write(opts, state, DSD_ANALOG_SINK_CALL_WAV, samples, count, 1.0, sample_rate, 48000,
                                    analog_call_write, &sink);
        rec->spans_pause = 1;
    }
}

/* The pause a held call goes on after, as silence in the recording that spans it, so the file keeps time at block
   precision. A short write latches the segment (dsd_wav_segment_write()), and nothing more reaches it. */
static void
analog_call_write_pause(const dsd_opts* opts, analog_call* call) {
    static const short silence[DSD_ANALOG_SINK_CHUNK];
    const analog_call_recording* rec = &call->rec;
    if (!rec->wav || !rec->spans_pause || !opts->dmr_stereo_wav) {
        return;
    }
    for (uint64_t left = call->owed48; left > 0U;) {
        const size_t n = left < DSD_ANALOG_SINK_CHUNK ? (size_t)left : DSD_ANALOG_SINK_CHUNK;
        dsd_wav_segment_write(rec->wav, silence, n, "NFM call");
        left -= n;
    }
}

/* A block of @p count samples at @p sample_rate, in 48 kHz samples: the remainder carries to the next held block. */
static uint64_t
analog_call_span48(analog_call* call, size_t count, int sample_rate) {
    const uint64_t total = (uint64_t)count * 48000U + call->rem48;
    call->rem48 = total % (uint64_t)sample_rate;
    return total / (uint64_t)sample_rate;
}

/* Issue #756: a block with nothing to record (the carrier closed after its hangover, or its tone still checked). The
   call is held, and the block's span ages the gap and is owed as silence; the reconciliation after it ends the call
   when the gap is past the max gap, or at once with the hold off, as every such block did before. A hold that begins
   as the carrier closes is credited the hangover, which the blocks before it already recorded; one that begins as the
   tone check returns under a continuing carrier is not. */
static void
analog_call_hold(dsd_opts* opts, dsd_state* state, analog_call* call, size_t count, int sample_rate) {
    if (!call->held) {
        analog_call_release_hold(call);
        call->held = 1;
        call->credited = !state->analog_rx.carrier_open;
        call->age48 = call->credited ? (uint64_t)DSD_ANALOG_CARRIER_HANGOVER_MS * 48U : 0U;
    }
    const uint64_t span48 = analog_call_span48(call, count, sample_rate);
    call->age48 += span48;
    call->owed48 += span48;
    (void)analog_call_reconcile(opts, state, call);
}

/* A RECEPTION block, stamped @p now_m: the call's audio. A confirmed new identity ends the call (held or not) and
   begins another with this block (D4); a held call otherwise goes on, its pause written first. */
static void
analog_call_receive(dsd_opts* opts, dsd_state* state, analog_call* call, const float* samples, size_t count,
                    int sample_rate, double now_m) {
    dsd_receive_identity identity;
    dsd_receive_identity_current(state, DSD_SYNC_ANALOG, 0, &identity);
    if (call->active && dsd_receive_identity_changed(&call->identity, &identity)) {
        analog_call_finish(opts, state, call);
    }
    if (call->held) {
        analog_call_write_pause(opts, call);
        analog_call_release_hold(call);
    }
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_ANALOG,
        .slot = 0,
        .kind = DSD_CALL_KIND_VOICE,
        .frequency_hz = (int64_t)dsd_opts_tuned_freq_hz(opts),
        .observed_m = now_m,
    };
    (void)dsd_call_state_observe(state, &observation,
                                 call->active ? DSD_CALL_BOUNDARY_CONTINUE : DSD_CALL_BOUNDARY_BEGIN);
    /* The last confirmed tone stays the call's until the call ends: a block whose tone is still being acquired names
       no service of its own, and a later confirmed tone is compared with the confirmed one before it. */
    if (!call->active || identity.known) {
        call->identity = identity;
    }
    if (!call->active) {
        call->boundaries_seen = state->analog_rx.boundaries;
    }
    call->active = 1;
    const dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_CLEAR,
        .audio_permitted = 1,
        .observed_m = now_m,
    };
    (void)dsd_call_state_update_crypto(state, 0, &crypto);
    (void)dsd_call_state_update_media(state, 0, 1, now_m);
    dsd_event_sync_slot(opts, state, 0);
    analog_call_record(opts, state, call, samples, count, sample_rate);
    call->last_m = now_m;
}

/* A block no call spans: BREAK, a block that is not one, trunking, no detection, or (B6) the receiver crossed a hard
   boundary since the call began. The block that carries a moved epoch straddles it, its first part the old reception,
   so it is not written either: the call ends at its last reception and the next block begins the next. */
static int
analog_call_breaks(const dsd_opts* opts, const dsd_state* state, const analog_call* call, const float* samples,
                   size_t count, int sample_rate, int input) {
    if ((input != DSD_ANALOG_CALL_RECEPTION && input != DSD_ANALOG_CALL_HOLDABLE) || !samples || count == 0
        || sample_rate <= 0 || opts->trunk_enable || !dsd_analog_tone_detection_active(opts)) {
        return 1;
    }
    return call && call->active && state->analog_rx.boundaries != call->boundaries_seen;
}

void
dsd_engine_analog_call_block(dsd_opts* opts, dsd_state* state, const float* samples, size_t count, int sample_rate,
                             int input) {
    if (!opts || !state) {
        return;
    }
    /* A RECEPTION block's one decode-clock read, before any of its file work on any path: the reconciliation's end, a
       new identity's end and a resumed call's pause all close or write a recording before the block begins or goes on
       with a call, and the time a slow disk takes for that is not the call's. The stamp is where the call starts or
       goes on, and where it ends, whenever that is decided. A held block reads none: the hold runs on sample time. */
    const double now_m = input == DSD_ANALOG_CALL_RECEPTION ? dsd_decode_now_mono_s() : 0.0;
    analog_call* call = analog_call_get(state);
    if (call && call->active) {
        (void)analog_call_reconcile(opts, state, call);
    }
    if (analog_call_breaks(opts, state, call, samples, count, sample_rate, input)) {
        dsd_engine_analog_call_end(opts, state);
        return;
    }
    if (input == DSD_ANALOG_CALL_HOLDABLE) {
        if (call && call->active) {
            analog_call_hold(opts, state, call, count, sample_rate);
        }
        return;
    }
    call = analog_call_ensure(state);
    if (call) {
        analog_call_receive(opts, state, call, samples, count, sample_rate, now_m);
    }
}
