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
#include <sndfile.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "scan_owner.h"

typedef struct {
    SNDFILE* wav;
    char path[1024];
    char dir[512]; /* The folder the recording was opened in, for a teardown that has no dsd_opts. */
    time_t opened;
    int active;
    int recording_attempted;
    dsd_receive_identity identity;
} analog_call;

/* A teardown that skipped dsd_engine_finalize_calls() still publishes an open recording under its final name, without
   the history row it no longer has (and without an rdio export), instead of leaving its TEMP_ file behind. */
static void
analog_call_free(void* ptr) {
    analog_call* call = (analog_call*)ptr;
    if (call->wav) {
        call->wav = close_and_rename_analog_wav_file(call->wav, call->path, call->dir);
    }
    free(call);
}

static analog_call*
analog_call_get(const dsd_state* state) {
    return (analog_call*)dsd_engine_scan_owned(state, 2);
}

/* Close and publish the call's recording against the last accepted block's row, as the call's end does, but leave the
   call running. */
static void
analog_call_close_recording(dsd_opts* opts, dsd_state* state, analog_call* call) {
    if (call->wav) {
        call->wav = close_and_rename_wav_file(call->wav, opts, call->path, opts->wav_out_dir,
                                              state->event_history_s ? &state->event_history_s[0] : NULL, call->opened);
        call->path[0] = '\0';
    }
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

void
dsd_engine_analog_call_end(dsd_opts* opts, dsd_state* state) {
    analog_call* call = analog_call_get(state);
    if (!opts || !state || !call || !call->active) {
        return;
    }
    call->active = 0;
    call->recording_attempted = 0;
    dsd_analog_sink_end_call(state);
    /* Close against the last accepted block's row before a retune or new service can replace it. */
    analog_call_close_recording(opts, state, call);
    (void)dsd_call_state_end_ex(state, 0, 0.0, DSD_CALL_END_EXPLICIT);
    dsd_event_sync_slot(opts, state, 0);
}

void
dsd_engine_analog_call_tick(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || !dsd_engine_analog_call_active(state)) {
        return;
    }
    if (!dsd_analog_tone_detection_active(opts) || !dsd_analog_rx_carrier_open_now(opts, state)
        || !dsd_analog_tone_gate_passes(dsd_analog_tone_gate_in_force(opts, state))
        || dsd_engine_analog_call_due(state)) {
        dsd_engine_analog_call_end(opts, state);
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
dsd_engine_analog_call_stop_recording(dsd_opts* opts, dsd_state* state) {
    analog_call* call = (opts && state) ? analog_call_get(state) : NULL;
    if (!call) {
        return;
    }
    analog_call_close_recording(opts, state, call);
    /* A start later in the same call opens a new recording at the next block. */
    call->recording_attempted = 0;
}

static void
analog_call_record(dsd_opts* opts, dsd_state* state, analog_call* call, const float* samples, size_t count,
                   int sample_rate) {
    if (!opts->dmr_stereo_wav) {
        call->recording_attempted = 0;
    }
    if (opts->dmr_stereo_wav && !call->recording_attempted) {
        call->recording_attempted = 1;
        call->wav = open_wav_file(opts->wav_out_dir, call->path, sizeof(call->path), 48000, 0);
        DSD_SNPRINTF(call->dir, sizeof(call->dir), "%s", opts->wav_out_dir);
        call->opened = dsd_decode_time();
    }
    if (!opts->dmr_stereo_wav) {
        analog_call_close_recording(opts, state, call);
    }
    if (call->wav) {
        const analog_call_sink sink = {call->wav};
        (void)dsd_analog_sink_write(opts, state, DSD_ANALOG_SINK_CALL_WAV, samples, count, 1.0, sample_rate, 48000,
                                    analog_call_write, &sink);
    }
}

void
dsd_engine_analog_call_block(dsd_opts* opts, dsd_state* state, const float* samples, size_t count, int sample_rate,
                             int accepted) {
    if (!opts || !state) {
        return;
    }
    if (!accepted || !samples || count == 0 || sample_rate <= 0 || opts->trunk_enable
        || !dsd_analog_tone_detection_active(opts)) {
        dsd_engine_analog_call_end(opts, state);
        return;
    }
    analog_call* call = analog_call_ensure(state);
    if (!call) {
        return;
    }
    dsd_receive_identity identity;
    dsd_receive_identity_current(state, DSD_SYNC_ANALOG, 0, &identity);
    if (call->active && dsd_receive_identity_changed(&call->identity, &identity)) {
        dsd_engine_analog_call_end(opts, state);
    }
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_ANALOG,
        .slot = 0,
        .kind = DSD_CALL_KIND_VOICE,
        .frequency_hz = (int64_t)dsd_opts_tuned_freq_hz(opts),
    };
    (void)dsd_call_state_observe(state, &observation,
                                 call->active ? DSD_CALL_BOUNDARY_CONTINUE : DSD_CALL_BOUNDARY_BEGIN);
    /* The last confirmed tone stays the call's until the call ends: a block whose tone is still being acquired names
       no service of its own, and a later confirmed tone is compared with the confirmed one before it. */
    if (!call->active || identity.known) {
        call->identity = identity;
    }
    call->active = 1;
    const dsd_call_crypto_update crypto = {.classification = DSD_CALL_CRYPTO_CLEAR, .audio_permitted = 1};
    (void)dsd_call_state_update_crypto(state, 0, &crypto);
    (void)dsd_call_state_update_media(state, 0, 1, 0.0);
    dsd_event_sync_slot(opts, state, 0);
    analog_call_record(opts, state, call, samples, count, sample_rate);
}
