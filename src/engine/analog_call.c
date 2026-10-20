// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
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
    time_t opened;
    int active;
    int recording_attempted;
    dsd_receive_identity identity;
} analog_call;

static void
analog_call_free(void* ptr) {
    analog_call* call = (analog_call*)ptr;
    if (call->wav) {
        sf_close(call->wav);
    }
    free(call);
}

static analog_call*
analog_call_get(const dsd_state* state) {
    return (analog_call*)dsd_engine_scan_owned(state, 2);
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
    if (call->wav) {
        call->wav = close_and_rename_wav_file(call->wav, opts, call->path, opts->wav_out_dir,
                                              state->event_history_s ? &state->event_history_s[0] : NULL, call->opened);
        call->path[0] = '\0';
    }
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

static void
analog_call_write(const void* context, const short* samples, size_t count) {
    analog_call* call = (analog_call*)context;
    if (call->wav) {
        (void)sf_write_short(call->wav, samples, (sf_count_t)count);
    }
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

static void
analog_call_record(dsd_opts* opts, dsd_state* state, analog_call* call, const float* samples, size_t count,
                   int sample_rate) {
    if (!opts->dmr_stereo_wav) {
        call->recording_attempted = 0;
    }
    if (opts->dmr_stereo_wav && !call->recording_attempted) {
        call->recording_attempted = 1;
        call->wav = open_wav_file(opts->wav_out_dir, call->path, sizeof(call->path), 48000, 0);
        call->opened = dsd_decode_time();
    }
    if (!opts->dmr_stereo_wav && call->wav) {
        call->wav = close_and_rename_wav_file(call->wav, opts, call->path, opts->wav_out_dir,
                                              state->event_history_s ? &state->event_history_s[0] : NULL, call->opened);
    }
    if (call->wav) {
        (void)dsd_analog_sink_write(opts, state, DSD_ANALOG_SINK_CALL_WAV, samples, count, 1.0, sample_rate, 48000,
                                    analog_call_write, call);
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
    call->active = 1;
    call->identity = identity;
    const dsd_call_crypto_update crypto = {.classification = DSD_CALL_CRYPTO_CLEAR, .audio_permitted = 1};
    (void)dsd_call_state_update_crypto(state, 0, &crypto);
    (void)dsd_call_state_update_media(state, 0, 1, 0.0);
    dsd_event_sync_slot(opts, state, 0);
    analog_call_record(opts, state, call, samples, count, sample_rate);
}
