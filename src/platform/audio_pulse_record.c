// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A PulseAudio record stream whose queued audio can be read without waiting (issue #834; audio_pulse_record.h).
 *
 * The context, the stream, their callbacks and the blocking read follow pa_simple (libpulse's simple.c), so the audio
 * read and the errors met are pa_simple's: the same latency flag, the same wait on the mainloop for audio, the same
 * check that the context and the stream are still up, the same skip of a hole in the stream.
 */

#include "audio_pulse_record.h"

#include <dsd-neo/core/safe_api.h>
#include <pulse/context.h>
#include <pulse/def.h>
#include <pulse/sample.h>
#include <pulse/stream.h>
#include <pulse/thread-mainloop.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct pulse_record {
    pa_threaded_mainloop* mainloop;
    pa_context* context;
    pa_stream* stream;
    /* The fragment pa_stream_peek() handed over and the reads have not used up (NULL: none): its bytes left, from
       `index` on. It is dropped (pa_stream_drop()) once used up. */
    const uint8_t* data;
    size_t len;
    size_t index;
    /* Every byte the reads took from the client so far (issue #834): with what the client holds, the stream's count of
       its input (pulse_record_arrived()). Written under the mainloop lock. */
    uint64_t taken;
};

/* Every state change and every arrival of audio wakes a thread waiting on the mainloop. */
static void
pulse_record_context_cb(pa_context* context, void* userdata) {
    (void)context;
    pa_threaded_mainloop_signal(((struct pulse_record*)userdata)->mainloop, 0);
}

static void
pulse_record_stream_cb(pa_stream* stream, void* userdata) {
    (void)stream;
    pa_threaded_mainloop_signal(((struct pulse_record*)userdata)->mainloop, 0);
}

static void
pulse_record_read_cb(pa_stream* stream, size_t bytes, void* userdata) {
    (void)stream;
    (void)bytes;
    pa_threaded_mainloop_signal(((struct pulse_record*)userdata)->mainloop, 0);
}

/* Whether the context and the stream are up; if not, the error in @p err: the context's own when either failed,
   PA_ERR_BADSTATE when it ended. Mainloop lock held. */
static int
pulse_record_alive_locked(const struct pulse_record* rec, int* err) {
    const pa_context_state_t cstate = pa_context_get_state(rec->context);
    const pa_stream_state_t sstate = pa_stream_get_state(rec->stream);
    if (PA_CONTEXT_IS_GOOD(cstate) && PA_STREAM_IS_GOOD(sstate)) {
        return 1;
    }
    *err =
        (cstate == PA_CONTEXT_FAILED || sstate == PA_STREAM_FAILED) ? pa_context_errno(rec->context) : PA_ERR_BADSTATE;
    return 0;
}

/* Make rec->data the next fragment the client holds, without waiting: 1 when it holds bytes, 0 when the client holds
   none now, -1 on error. A hole in the stream is skipped. Mainloop lock held. */
static int
pulse_record_peek_locked(struct pulse_record* rec, int* err) {
    while (rec->data == NULL) {
        const void* data = NULL;
        size_t len = 0U;
        if (pa_stream_peek(rec->stream, &data, &len) < 0) {
            *err = pa_context_errno(rec->context);
            return -1;
        }
        if (len == 0U) {
            return 0;
        }
        if (data == NULL) {
            if (pa_stream_drop(rec->stream) < 0) {
                *err = pa_context_errno(rec->context);
                return -1;
            }
            continue;
        }
        rec->data = (const uint8_t*)data;
        rec->len = len;
        rec->index = 0U;
    }
    return 1;
}

/* Copy up to @p bytes of the fragment held into @p out, dropping it once used up: the bytes copied, or -1 on error.
   Mainloop lock held. */
static long
pulse_record_take_locked(struct pulse_record* rec, uint8_t* out, size_t bytes, int* err) {
    const size_t n = rec->len < bytes ? rec->len : bytes;
    DSD_MEMCPY(out, rec->data + rec->index, n);
    rec->index += n;
    rec->len -= n;
    rec->taken += n;
    if (rec->len == 0U) {
        rec->data = NULL;
        rec->index = 0U;
        if (pa_stream_drop(rec->stream) < 0) {
            *err = pa_context_errno(rec->context);
            return -1;
        }
    }
    return (long)n;
}

/* Wait until the context is ready: 0, or -1 when it failed or ended. Mainloop lock held. */
static int
pulse_record_wait_context_locked(const struct pulse_record* rec, int* err) {
    for (;;) {
        const pa_context_state_t state = pa_context_get_state(rec->context);
        if (state == PA_CONTEXT_READY) {
            return 0;
        }
        if (!PA_CONTEXT_IS_GOOD(state)) {
            *err = pa_context_errno(rec->context);
            return -1;
        }
        pa_threaded_mainloop_wait(rec->mainloop);
    }
}

/* Wait until the stream is ready: 0, or -1 when it failed or ended. Mainloop lock held. */
static int
pulse_record_wait_stream_locked(const struct pulse_record* rec, int* err) {
    for (;;) {
        const pa_stream_state_t state = pa_stream_get_state(rec->stream);
        if (state == PA_STREAM_READY) {
            return 0;
        }
        if (!PA_STREAM_IS_GOOD(state)) {
            *err = pa_context_errno(rec->context);
            return -1;
        }
        pa_threaded_mainloop_wait(rec->mainloop);
    }
}

/* With the mainloop running and its lock held: the context connected and ready, then the stream. 0 or -1. */
static int
pulse_record_connect_locked(struct pulse_record* rec, const char* dev, const pa_sample_spec* spec,
                            const pa_buffer_attr* attr, int* err) {
    if (pulse_record_wait_context_locked(rec, err) != 0) {
        return -1;
    }
    rec->stream = pa_stream_new(rec->context, "Audio Input", spec, NULL);
    if (rec->stream == NULL) {
        *err = pa_context_errno(rec->context);
        return -1;
    }
    pa_stream_set_state_callback(rec->stream, pulse_record_stream_cb, rec);
    pa_stream_set_read_callback(rec->stream, pulse_record_read_cb, rec);
    /* The latency flag pa_simple_new() sets, which has the server size its source latency to the fragment asked for.
       The timing flags it sets beside it serve pa_simple_get_latency() only, and nothing here reads the latency. */
    if (pa_stream_connect_record(rec->stream, dev, attr, PA_STREAM_ADJUST_LATENCY) < 0) {
        *err = pa_context_errno(rec->context);
        return -1;
    }
    return pulse_record_wait_stream_locked(rec, err);
}

struct pulse_record*
pulse_record_open(const char* app, const char* dev, const pa_sample_spec* spec, const pa_buffer_attr* attr, int* err) {
    *err = PA_ERR_INTERNAL;
    struct pulse_record* rec = (struct pulse_record*)calloc(1, sizeof(*rec));
    if (rec == NULL) {
        return NULL;
    }
    rec->mainloop = pa_threaded_mainloop_new();
    if (rec->mainloop == NULL) {
        pulse_record_free(rec);
        return NULL;
    }
    rec->context = pa_context_new(pa_threaded_mainloop_get_api(rec->mainloop), app);
    if (rec->context == NULL) {
        pulse_record_free(rec);
        return NULL;
    }
    pa_context_set_state_callback(rec->context, pulse_record_context_cb, rec);
    if (pa_context_connect(rec->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        *err = pa_context_errno(rec->context);
        pulse_record_free(rec);
        return NULL;
    }
    pa_threaded_mainloop_lock(rec->mainloop);
    const int rc =
        pa_threaded_mainloop_start(rec->mainloop) < 0 ? -1 : pulse_record_connect_locked(rec, dev, spec, attr, err);
    pa_threaded_mainloop_unlock(rec->mainloop);
    if (rc != 0) {
        pulse_record_free(rec);
        return NULL;
    }
    *err = 0;
    return rec;
}

int
pulse_record_read(struct pulse_record* rec, void* data, size_t bytes, int* err) {
    uint8_t* out = (uint8_t*)data;
    int rc = 0;
    pa_threaded_mainloop_lock(rec->mainloop);
    while (bytes > 0U) {
        if (!pulse_record_alive_locked(rec, err)) {
            rc = -1;
            break;
        }
        const int held = pulse_record_peek_locked(rec, err);
        if (held < 0) {
            rc = -1;
            break;
        }
        if (held == 0) {
            /* No bound here, as in pa_simple_read(): the decoder's own reads wait for the source. */
            pa_threaded_mainloop_wait(rec->mainloop);
            continue;
        }
        const long took = pulse_record_take_locked(rec, out, bytes, err);
        if (took < 0) {
            rc = -1;
            break;
        }
        out += took;
        bytes -= (size_t)took;
    }
    pa_threaded_mainloop_unlock(rec->mainloop);
    return rc;
}

long
pulse_record_read_held(struct pulse_record* rec, void* data, size_t bytes, int* err) {
    uint8_t* out = (uint8_t*)data;
    long total = 0;
    pa_threaded_mainloop_lock(rec->mainloop);
    while (bytes > 0U) {
        if (!pulse_record_alive_locked(rec, err)) {
            total = -1;
            break;
        }
        const int held = pulse_record_peek_locked(rec, err);
        if (held <= 0) {
            total = held < 0 ? -1 : total;
            break;
        }
        const long took = pulse_record_take_locked(rec, out, bytes, err);
        if (took < 0) {
            total = -1;
            break;
        }
        out += took;
        bytes -= (size_t)took;
        total += took;
    }
    pa_threaded_mainloop_unlock(rec->mainloop);
    return total;
}

uint64_t
pulse_record_arrived(struct pulse_record* rec) {
    pa_threaded_mainloop_lock(rec->mainloop);
    /* The client's queue still holds the fragment handed over, the part of it the reads took included. */
    const size_t readable = pa_stream_readable_size(rec->stream);
    const uint64_t queued = readable != (size_t)-1 ? (uint64_t)readable : 0U;
    const uint64_t taken_of_fragment = rec->data != NULL ? (uint64_t)rec->index : 0U;
    const uint64_t arrived = rec->taken + (queued > taken_of_fragment ? queued - taken_of_fragment : 0U);
    pa_threaded_mainloop_unlock(rec->mainloop);
    return arrived;
}

uint64_t
pulse_record_taken(const struct pulse_record* rec) {
    return rec->taken;
}

void
pulse_record_free(struct pulse_record* rec) {
    if (rec == NULL) {
        return;
    }
    /* pa_simple_free()'s order: the mainloop thread stops first, so no callback runs while the rest goes. */
    if (rec->mainloop != NULL) {
        pa_threaded_mainloop_stop(rec->mainloop);
    }
    if (rec->stream != NULL) {
        pa_stream_unref(rec->stream);
    }
    if (rec->context != NULL) {
        pa_context_disconnect(rec->context);
        pa_context_unref(rec->context);
    }
    if (rec->mainloop != NULL) {
        pa_threaded_mainloop_free(rec->mainloop);
    }
    free(rec);
}
