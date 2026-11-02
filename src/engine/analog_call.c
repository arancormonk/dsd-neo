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
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "analog_preroll.h"
#include "scan_owner.h"

/* The call's per-call recording. A stop or a folder change closes it and the call's next block opens another, so it
   lives inside the call rather than with it. */
typedef struct {
    SNDFILE* wav;
    char path[1024];
    char dir[512]; /* The folder the recording was opened in, where it is published (issue #708). */
    time_t opened;
    /* When the recording's first sample was heard, from the block's wall time (issue #668); 0 when the block carried
       none. The journal and the rdio-scanner sidecar take it as the start, over the row's own. */
    time_t started;
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
    /* The receiver's boundaries epoch at the last block, call or not (issue #668, B5/B7): a block that carries another
       crossed a hard boundary, and a call running ends. Set from the epoch in force when the first block arrives. */
    uint32_t boundaries_seen;
    dsd_receive_identity identity;
    /* Issue #668: the allowed audio heard before a reception, which a new recording opens with. It holds what the
       CALL_WAV lane converted, and the two share one fate: whatever drops the ring starts the lane over, and whatever
       starts the lane over drops the ring, so what the ring keeps always runs on into the lane's next write. */
    analog_preroll preroll;
    /* The lane ran on unbroken through the hold so far, its blocks going into the ring; the 48 kHz samples it made for
       them, those fallen out of the ring too, are the silence a resumed recording is owed instead of owed48. */
    int hold_buffering;
    uint64_t held_frames;
} analog_call;

/* A teardown that skipped dsd_engine_finalize_calls() still publishes an open recording under its final name, without
   the history row it no longer has (and without an rdio export), instead of leaving its TEMP_ file behind. */
static void
analog_call_free(void* ptr) {
    analog_call* call = (analog_call*)ptr;
    if (call->rec.wav) {
        call->rec.wav = close_and_rename_analog_wav_file(call->rec.wav, call->rec.path, call->rec.dir);
    }
    analog_preroll_resize(&call->preroll, 0);
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
        call->rec.wav = close_and_publish_call_wav_ex(call->rec.wav, opts, state, 0, call->rec.path,
                                                      call->rec.dir[0] ? call->rec.dir : opts->wav_out_dir,
                                                      call->rec.opened, call->rec.started);
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
    call->hold_buffering = 0;
    call->held_frames = 0;
}

/* Issue #668: what the pre-roll holds must not reach a file. The lane starts over with it, and a hold buffers nothing
   more, so its resume is the owed silence, as without a pre-roll. With no ring the lane is left as it was. */
static void
analog_call_preroll_invalidate(const dsd_state* state, analog_call* call) {
    call->hold_buffering = 0;
    if (!analog_preroll_active(&call->preroll)) {
        return;
    }
    analog_preroll_clear(&call->preroll);
    dsd_analog_sink_end_call(state);
}

/* The pre-roll in ms the options ask for: none while per-call recording is off. */
static int
analog_call_preroll_ms(const dsd_opts* opts) {
    return opts->dmr_stereo_wav && opts->analog_record_preroll_ms > 0 ? opts->analog_record_preroll_ms : 0;
}

/* The ring follows the options: allocated at the first block that needs one, freed with per-call recording or the
   setting off, and sized anew (empty) when the setting changes. */
static void
analog_call_preroll_sync(const dsd_opts* opts, const dsd_state* state, analog_call* call) {
    const int ms = analog_call_preroll_ms(opts);
    if (ms == call->preroll.ms) {
        return;
    }
    analog_call_preroll_invalidate(state, call);
    analog_preroll_resize(&call->preroll, ms);
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
    /* The pre-roll goes with the lane (issue #668), before the block that caused this end, if any, is looked at: an
       allowed HOLDABLE one then starts the ring afresh. */
    analog_preroll_clear(&call->preroll);
    dsd_analog_sink_end_call(state);
    /* Close against the last accepted block's row before a retune or new service can replace it. */
    analog_call_close_recording(opts, state, call);
    (void)dsd_call_state_end_ex(state, 0, ended_m, DSD_CALL_END_EXPLICIT);
    dsd_event_sync_slot(opts, state, 0);
}

void
dsd_engine_analog_call_end(dsd_opts* opts, dsd_state* state) {
    analog_call* call = (opts && state) ? analog_call_get(state) : NULL;
    if (!call) {
        return;
    }
    /* With no call, the pre-roll still goes (issue #668): every caller crossed something the audio before it does not
       belong with (a retune, a scan leave, an input switch, a mode change, Stop). */
    if (!call->active) {
        analog_call_preroll_invalidate(state, call);
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
    if (!call) {
        return;
    }
    /* B6 between blocks: a reset with no block after it (a TCP reconnect that yields to a queued command) moves the
       epoch while the boundaries stay current, and the stream may bring no whole block to see it. The call, held or
       not, ends at its last reception as the next block would end it, and with no call the pre-roll goes (issue #668).
       The epoch seen stays the last block's, so that next block, which may still hold audio from before the reset, is
       dropped too. */
    const int moved = state->analog_rx.boundaries != call->boundaries_seen;
    if (!call->active) {
        if (moved) {
            analog_call_preroll_invalidate(state, call);
        }
        return;
    }
    if (moved) {
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
        call->boundaries_seen = state->analog_rx.boundaries;
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
    /* Every recording transition that commits comes here, a stop, a start, a folder change, open file or not
       (issue #668): the pre-roll heard under the old recording is not the next one's, nor is a held call's pause. */
    analog_call_preroll_invalidate(state, call);
}

void
dsd_engine_analog_call_preroll_changed(const dsd_opts* opts, const dsd_state* state) {
    analog_call* call = (opts && state) ? analog_call_get(state) : NULL;
    if (!call) {
        return;
    }
    analog_call_preroll_invalidate(state, call);
    analog_call_preroll_sync(opts, state, call);
}

/* A wall time read from a nanosecond clock as double seconds is rounded by well under this: a start this close below a
   whole second is that second, so a block that began on one is filed under it. */
static const double k_analog_call_start_slack_s = 1e-6;

/* When the recording that @p block opens began (issue #668): the block's end in wall time, which the DSP read before
   any raw WAV or recording I/O, less the part of the block the file keeps (@p kept samples at the block rate) and the
   pre-roll written before it (@p released samples at 48 kHz), floored once to whole seconds. So neither a slow disk nor
   a recording closed first moves it. 0 when the block carries no wall time. */
static time_t
analog_call_recording_start(const dsd_analog_call_block* block, size_t kept, size_t released) {
    if (!(block->wall_end_s > 0.0)) {
        return 0;
    }
    const double span_s = (double)kept / (double)block->sample_rate + (double)released / 48000.0;
    return (time_t)floor(block->wall_end_s - span_s + k_analog_call_start_slack_s);
}

/* The block as heard before the sink gate when the DSP handed it (with -P on and a pre-roll), else as written. */
static const float*
analog_call_heard(const dsd_analog_call_block* block) {
    return block->heard ? block->heard : block->samples;
}

/* What of a block its recording gets. */
typedef struct {
    const float* samples;
    size_t count;
} analog_call_audio;

/* Issue #668: a new recording opens with the pre-roll, then the block as heard before the sink gate (no ramp-in where
   the squelch opened), so it starts before the squelch did. A block that is not clean is not contiguous with the
   allowed audio before it: the ring goes, and the file starts at the block's eligible point instead of with its
   silenced start. A failed open, a block that did not hear the carrier itself (it only goes on with a call), or a lane
   that broke since the ring filled drops the ring. With no ring, the block goes in whole as the sink gate left it.
   Returns the 48 kHz samples written before the block. */
static size_t
analog_call_release(dsd_state* state, analog_call* call, const dsd_analog_call_block* block, analog_call_audio* audio) {
    const analog_preroll* ring = &call->preroll;
    if (!analog_preroll_active(ring)) {
        return 0;
    }
    if (!call->rec.wav || !block->qualified_carrier) {
        analog_call_preroll_invalidate(state, call);
        return 0;
    }
    const size_t from = block->eligible_from < block->count ? block->eligible_from : block->count;
    if (from > 0 || analog_preroll_lane_moved(state, ring)) {
        analog_call_preroll_invalidate(state, call);
    }
    audio->samples = analog_call_heard(block) + from;
    audio->count = block->count - from;
    const size_t released = ring->len;
    const analog_call_sink sink = {call->rec.wav};
    analog_preroll_take(ring, released, analog_call_write, &sink);
    return released;
}

static void
analog_call_open(dsd_opts* opts, dsd_state* state, analog_call* call, const dsd_analog_call_block* block,
                 analog_call_audio* audio) {
    analog_call_recording* rec = &call->rec;
    rec->attempted = 1;
    rec->wav = open_wav_file(opts->wav_out_dir, rec->path, sizeof(rec->path), 48000, 0);
    DSD_SNPRINTF(rec->dir, sizeof(rec->dir), "%s", opts->wav_out_dir);
    const size_t released = analog_call_release(state, call, block, audio);
    rec->started = analog_call_recording_start(block, audio->count, released);
    /* A block with no wall time (never with -P on) leaves the start to the open and the row, as before. */
    rec->opened = rec->started ? rec->started : dsd_decode_time();
    rec->spans_pause = 0;
}

/* @p resumed: the block goes on with a held call whose pause was just written from the pre-roll, so it continues that
   audio as heard before the sink gate. */
static void
analog_call_record(dsd_opts* opts, dsd_state* state, analog_call* call, const dsd_analog_call_block* block,
                   int resumed) {
    analog_call_recording* rec = &call->rec;
    analog_call_audio audio = {resumed ? analog_call_heard(block) : block->samples, block->count};
    if (!opts->dmr_stereo_wav) {
        rec->attempted = 0;
    }
    if (opts->dmr_stereo_wav && !rec->attempted) {
        analog_call_open(opts, state, call, block, &audio);
    }
    if (!opts->dmr_stereo_wav) {
        analog_call_close_recording(opts, state, call);
    }
    if (rec->wav) {
        const analog_call_sink sink = {rec->wav};
        (void)dsd_analog_sink_write(opts, state, DSD_ANALOG_SINK_CALL_WAV, audio.samples, audio.count, 1.0,
                                    block->sample_rate, 48000, analog_call_write, &sink);
        rec->spans_pause = 1;
    }
    /* The reception follows on from whatever the ring held: the lane's history is the file's now (issue #668). */
    if (analog_preroll_active(&call->preroll)) {
        analog_preroll_consume(state, &call->preroll);
    }
}

/* @p count zeros into @p wav, in chunks: a short write latches the segment (dsd_wav_segment_write()), and nothing more
   reaches it. */
static void
analog_call_write_silence(SNDFILE* wav, uint64_t count) {
    static const short silence[DSD_ANALOG_SINK_CHUNK] = {0};
    for (uint64_t left = count; left > 0U;) {
        const size_t n = left < DSD_ANALOG_SINK_CHUNK ? (size_t)left : DSD_ANALOG_SINK_CHUNK;
        dsd_wav_segment_write(wav, silence, n, "NFM call");
        left -= n;
    }
}

/* The pause a held call goes on after, into the recording that spans it, so the file keeps time. With the lane unbroken
   through the hold (issue #668) the pause is what the lane made of the held blocks: silence for those that fell out of
   the pre-roll, then the newest it kept, so the file runs on as one conversion, frame for frame, and holds the audio
   just before the carrier came back. It needs a resumed carrier heard after a clean start, as a new file's opening
   does. Otherwise the owed silence at block precision, as without a pre-roll, and the lane starts over at the block.
   Returns 1 when the pre-roll was written. */
static int
analog_call_resume(const dsd_opts* opts, dsd_state* state, analog_call* call, const dsd_analog_call_block* block) {
    const analog_call_recording* rec = &call->rec;
    if (!rec->wav || !rec->spans_pause || !opts->dmr_stereo_wav) {
        return 0;
    }
    analog_preroll* ring = &call->preroll;
    if (!call->hold_buffering || !block->qualified_carrier || block->eligible_from != 0
        || analog_preroll_lane_moved(state, ring)) {
        analog_call_preroll_invalidate(state, call);
        analog_call_write_silence(rec->wav, call->owed48);
        return 0;
    }
    const size_t rel = (uint64_t)ring->len < call->held_frames ? ring->len : (size_t)call->held_frames;
    analog_call_write_silence(rec->wav, call->held_frames - rel);
    const analog_call_sink sink = {rec->wav};
    analog_preroll_take(ring, rel, analog_call_write, &sink);
    return 1;
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
   tone check returns under a continuing carrier is not. A hold begun with a pre-roll whose lane ran on from the
   reception before it fills the ring as it goes (issue #668). */
static void
analog_call_hold(dsd_opts* opts, dsd_state* state, analog_call* call, size_t count, int sample_rate) {
    if (!call->held) {
        analog_call_release_hold(call);
        call->held = 1;
        call->credited = !state->analog_rx.carrier_open;
        call->age48 = call->credited ? (uint64_t)DSD_ANALOG_CARRIER_HANGOVER_MS * 48U : 0U;
        call->hold_buffering = analog_preroll_active(&call->preroll) && call->preroll.has_generation;
    }
    const uint64_t span48 = analog_call_span48(call, count, sample_rate);
    call->age48 += span48;
    call->owed48 += span48;
    (void)analog_call_reconcile(opts, state, call);
}

/* Issue #668: a HOLDABLE block, after any end it caused, goes into the pre-roll (D2: only a clean one, whose tone gate
   stayed OFF or ALLOWED throughout; any other drops the ring), held call or not, as heard before the sink gate. Inside
   a hold, the first break of the lane or the ring stops the buffering for the rest of that hold. */
static void
analog_call_buffer(const dsd_opts* opts, dsd_state* state, analog_call* call, const dsd_analog_call_block* block) {
    analog_preroll* ring = &call->preroll;
    if (!analog_preroll_active(ring) || (call->held && !call->hold_buffering)) {
        return;
    }
    const int clean = block->eligible_from == 0;
    if (!clean || analog_preroll_lane_moved(state, ring)) {
        analog_call_preroll_invalidate(state, call);
        if (!clean || call->held) {
            return;
        }
    }
    int moved = 0;
    const uint64_t made =
        analog_preroll_append(opts, state, ring, analog_call_heard(block), block->count, block->sample_rate, &moved);
    if (call->held && moved) {
        analog_call_preroll_invalidate(state, call);
    } else if (call->held) {
        call->held_frames += made;
    }
}

/* A RECEPTION block, stamped @p now_m: the call's audio. A confirmed new identity ends the call (held or not) and
   begins another with this block (D4); a held call otherwise goes on, its pause written first. A call begins only on a
   block that heard the carrier itself (issue #668, B1): the carrier's hangover after a tone verdict holds only what the
   squelch closed on, and a call begun there would publish silence and a history row for it. Such a block still goes
   on with a call already running; with none, the pre-roll before it goes. */
static void
analog_call_receive(dsd_opts* opts, dsd_state* state, analog_call* call, const dsd_analog_call_block* block,
                    double now_m) {
    dsd_receive_identity identity;
    dsd_receive_identity_current(state, DSD_SYNC_ANALOG, 0, &identity);
    if (call->active && dsd_receive_identity_changed(&call->identity, &identity)) {
        analog_call_finish(opts, state, call);
    }
    if (!call->active && !block->qualified_carrier) {
        analog_call_preroll_invalidate(state, call);
        return;
    }
    int resumed = 0;
    if (call->held) {
        resumed = analog_call_resume(opts, state, call, block);
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
    call->active = 1;
    const dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_CLEAR,
        .audio_permitted = 1,
        .observed_m = now_m,
    };
    (void)dsd_call_state_update_crypto(state, 0, &crypto);
    (void)dsd_call_state_update_media(state, 0, 1, now_m);
    dsd_event_sync_slot(opts, state, 0);
    analog_call_record(opts, state, call, block, resumed);
    call->last_m = now_m;
}

/* A block no call spans: BREAK, a block that is not one, trunking, or no detection. */
static int
analog_call_breaks(const dsd_opts* opts, const dsd_analog_call_block* block) {
    return (block->input != DSD_ANALOG_CALL_RECEPTION && block->input != DSD_ANALOG_CALL_HOLDABLE) || !block->samples
           || block->count == 0 || block->sample_rate <= 0 || opts->trunk_enable
           || !dsd_analog_tone_detection_active(opts);
}

/* B6, and issue #668's B5/B7: the receiver crossed a hard boundary (a stream pause, a reconnect, a retune) since the
   last block, call or not. The block that carries the moved epoch straddles it, its first part from before (a pause
   marks no straddle), so no call goes on or begins with it: a call ends at its last reception, the pre-roll goes, the
   block is dropped, and the next block begins afresh. Checked before anything else the block does, a live max gap
   change's end included, which would otherwise leave no call to compare the epoch with. Returns 1 when the block is
   dropped. */
static int
analog_call_crossed_boundary(dsd_opts* opts, dsd_state* state, analog_call* call) {
    if (state->analog_rx.boundaries == call->boundaries_seen) {
        return 0;
    }
    call->boundaries_seen = state->analog_rx.boundaries;
    dsd_engine_analog_call_end(opts, state);
    return 1;
}

void
dsd_engine_analog_call_block(dsd_opts* opts, dsd_state* state, const dsd_analog_call_block* block) {
    if (!opts || !state || !block) {
        return;
    }
    const int input = block->input;
    /* A RECEPTION block's one decode-clock read, before any of its file work on any path: the reconciliation's end, a
       new identity's end and a resumed call's pause all close or write a recording before the block begins or goes on
       with a call, and the time a slow disk takes for that is not the call's. The stamp is where the call starts or
       goes on, and where it ends, whenever that is decided. A held block reads none: the hold runs on sample time. */
    const double now_m = input == DSD_ANALOG_CALL_RECEPTION ? dsd_decode_now_mono_s() : 0.0;
    /* Every block keeps the epoch seen, so the call exists from the first one; with no memory for it, none begins. */
    analog_call* call = analog_call_ensure(state);
    if (!call) {
        return;
    }
    analog_call_preroll_sync(opts, state, call);
    if (analog_call_crossed_boundary(opts, state, call)) {
        return;
    }
    if (call->active) {
        (void)analog_call_reconcile(opts, state, call);
    }
    if (analog_call_breaks(opts, block)) {
        dsd_engine_analog_call_end(opts, state);
        return;
    }
    if (input == DSD_ANALOG_CALL_HOLDABLE) {
        if (call->active) {
            analog_call_hold(opts, state, call, block->count, block->sample_rate);
        }
        analog_call_buffer(opts, state, call, block);
        return;
    }
    analog_call_receive(opts, state, call, block, now_m);
}
