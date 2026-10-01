// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief Test-only host that replays an I/Q capture under a perturbed decoder read pattern (issue #572).
 *
 * Under --iq-replay the decoder paces the front end, so what it decodes must not depend on how its reads fall in real
 * time. This host runs the real engine on the arguments dsd-neo would get and perturbs only the decoder's side of the
 * RTL stream: its lifecycle start hook (which runs after the engine installed its hooks) wraps the RTL stream read
 * hook. tests/iq_determinism_check.cmake runs several such legs and requires their decoder output to match.
 *
 * Host options, removed before the rest reach dsd_runtime_bootstrap() ("--opt VALUE" or "--opt=VALUE"):
 *   --replay-jitter-seed N       sleep after some reads, from a xorshift64 stream seeded with N (enables jitter)
 *   --replay-jitter-max-ms MS    longest sleep, 0 to 1500 (default 120); each one is uniform in [0, MS]
 *   --replay-jitter-every N      sleep after about one read in N (default 64)
 *   --replay-jitter-budget-ms MS total sleep the run may add, 0 to 1500 (default 1500): a leg stays bounded
 *   --replay-short-reads N       cap each read at a count uniform in [1, asked], from a stream seeded with N
 *   --replay-sink free|stalled   replace the local audio device with an asynchronous test sink (see below)
 *
 * When live processing ends, the stop hook prints the line the runner compares across legs,
 *   REPLAY STREAM: fsk_samples=F cqpsk_symbols=C monitor_samples=A generation_changes=G media_ms=M
 * which counts what the decoder's reads returned, by the output kind of the replay batch each read took its samples
 * from (FSK discriminator samples and CQPSK symbols are different units and stay apart), the stream generation changes
 * between reads, and the capture time the last sample read ends at. It then prints the line the runner drops,
 *   REPLAY JITTER: ...
 * which says what the host injected; that differs between legs by design. A read that returns samples without a
 * replay batch tag (a live input) prints "REPLAY STREAM FAIL:" and the host exits 1.
 *
 * --replay-sink needs a build that replaces the platform audio device at link time (GNU ld --wrap of
 * dsd_audio_open_output, dsd_audio_write, dsd_audio_drain and dsd_audio_close, DSD_NEO_TEST_AUDIO_WRAP; see
 * tests/CMakeLists.txt). Run with `-o pulse`: the engine then opens its local output through the test sink, which
 * behaves like the real backends' asynchronous output: a write queues into a 1 s ring and never waits for the device,
 * dropping the oldest audio when the ring is full, and a pump thread hands 20 ms chunks to the device. "free" takes
 * every chunk at once. "stalled" takes the first chunk and then never returns from its device write until the stream
 * closes, as a wedged sound server would. Each stream prints at close the decoder side of its record, compared:
 *   REPLAY SINK: stream=I rate=R channels=C async=A writes=W frames=N fnv1a=H drains=D
 * (fnv1a hashes every sample the decoder handed over), then its device side, dropped by the runner as an audio-sink
 * diagnostic:
 *   Replay sink output stats: rate=R ch=C mode=M taken_frames=T dropped_frames=X
 * A stalled stream that took audio but dropped nothing never backed up, and a stalled run in which no stream both took
 * audio and dropped some tested nothing: the host then prints "REPLAY SINK FAIL:" and exits 1. It does the same when
 * the engine opened a synchronous sink, whose write would block, and when the decoder drains a stalled sink, which the
 * real asynchronous drain would wait on for good.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/io/rtl_stream_fwd.h"

#ifdef DSD_NEO_TEST_AUDIO_WRAP
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/threading.h>
#endif

/* The jitter host caps the delay it adds to one run at 1.5 s, so a leg stays close to the plain replay's time. */
#define REPLAY_JITTER_BUDGET_CAP_MS 1500U
#define REPLAY_JITTER_DEFAULT_MAX   120U
#define REPLAY_JITTER_DEFAULT_EVERY 64U
#define REPLAY_JITTER_MAX_EVERY     1000000U

typedef struct {
    int jitter; /* --replay-jitter-seed given */
    uint32_t jitter_seed;
    uint32_t jitter_max_ms;
    uint32_t jitter_every;
    uint32_t jitter_budget_ms;
    int jitter_tuned; /* a --replay-jitter-* setting other than the seed was given */
    int short_reads;  /* --replay-short-reads given */
    uint32_t short_seed;
    int sink; /* --replay-sink given */
    int sink_stalled;
} replay_options;

typedef struct {
    uint64_t fsk_samples;
    uint64_t cqpsk_symbols;
    uint64_t monitor_samples;
    uint64_t untagged_samples; /* samples a read returned with no replay batch tag */
    uint64_t unknown_kind_samples;
    uint32_t generation_changes;
    uint32_t generation;
    int have_generation;
    uint64_t media_end_ns;
    uint64_t reads;
    uint64_t shortened_reads;
    uint64_t sleeps;
    uint64_t slept_us;
    uint64_t jitter_rng;
    uint64_t short_rng;
} replay_totals;

static replay_options g_options;
static replay_totals g_totals;
static int g_failed;

/* ---- random streams ------------------------------------------------------------------------------------------ */

/* splitmix64 spreads a small seed over the state, and keeps the xorshift state nonzero. */
static uint64_t
replay_rng_init(uint32_t seed, uint64_t stream) {
    uint64_t z = ((uint64_t)seed << 1U) ^ stream;
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return z != 0U ? z : 0x2545F4914F6CDD1DULL;
}

static uint64_t
replay_rng_next(uint64_t* state) {
    uint64_t x = *state;
    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    *state = x;
    return x;
}

/* ---- argument handling ---------------------------------------------------------------------------------------- */

static void
replay_usage(void) {
    DSD_FPRINTF(stderr, "replay jitter host options (removed before the dsd-neo arguments are parsed):\n"
                        "  --replay-jitter-seed N        sleep after some reads, from a stream seeded with N\n"
                        "  --replay-jitter-max-ms MS     longest sleep, 0 to 1500 (default 120)\n"
                        "  --replay-jitter-every N       sleep after about one read in N (default 64)\n"
                        "  --replay-jitter-budget-ms MS  total sleep the run may add, 0 to 1500 (default 1500)\n"
                        "  --replay-short-reads N        cap each read at a count uniform in [1, asked]\n"
                        "  --replay-sink free|stalled    asynchronous test sink in place of the local audio device\n");
}

static int
replay_parse_u32(const char* name, const char* text, uint32_t min_value, uint32_t max_value, uint32_t* out) {
    uint32_t value = 0U;
    if (dsd_parse_uint32_strict(text, 10, max_value, &value) != 0 || value < min_value) {
        DSD_FPRINTF(stderr, "%s: invalid value '%s' (expected %u to %u)\n", name, text, min_value, max_value);
        return -1;
    }
    *out = value;
    return 0;
}

static int
replay_apply_option(const char* name, const char* value) {
    if (strcmp(name, "--replay-jitter-seed") == 0) {
        g_options.jitter = 1;
        return replay_parse_u32(name, value, 0U, UINT32_MAX, &g_options.jitter_seed);
    }
    if (strcmp(name, "--replay-jitter-max-ms") == 0) {
        g_options.jitter_tuned = 1;
        return replay_parse_u32(name, value, 0U, REPLAY_JITTER_BUDGET_CAP_MS, &g_options.jitter_max_ms);
    }
    if (strcmp(name, "--replay-jitter-every") == 0) {
        g_options.jitter_tuned = 1;
        return replay_parse_u32(name, value, 1U, REPLAY_JITTER_MAX_EVERY, &g_options.jitter_every);
    }
    if (strcmp(name, "--replay-jitter-budget-ms") == 0) {
        g_options.jitter_tuned = 1;
        return replay_parse_u32(name, value, 0U, REPLAY_JITTER_BUDGET_CAP_MS, &g_options.jitter_budget_ms);
    }
    if (strcmp(name, "--replay-short-reads") == 0) {
        g_options.short_reads = 1;
        return replay_parse_u32(name, value, 0U, UINT32_MAX, &g_options.short_seed);
    }
    /* --replay-sink */
#ifdef DSD_NEO_TEST_AUDIO_WRAP
    if (strcmp(value, "free") == 0 || strcmp(value, "stalled") == 0) {
        g_options.sink = 1;
        g_options.sink_stalled = strcmp(value, "stalled") == 0;
        return 0;
    }
    DSD_FPRINTF(stderr, "%s: invalid value '%s' (expected free or stalled)\n", name, value);
#else
    DSD_FPRINTF(stderr, "%s: this build cannot replace the audio device (it needs GNU ld --wrap)\n", name);
#endif
    return -1;
}

static int
replay_is_host_option(const char* name) {
    static const char* const k_names[] = {
        "--replay-jitter-seed",      "--replay-jitter-max-ms", "--replay-jitter-every",
        "--replay-jitter-budget-ms", "--replay-short-reads",   "--replay-sink",
    };
    for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        if (strcmp(name, k_names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Copies the option name of a host option ("--replay-x" or the part of "--replay-x=VALUE" before '=') into name and
 * returns 1; returns 0 for any argument that is not one of the host's options. */
static int
replay_host_option_name(const char* arg, char* name, size_t cap, const char** eq_out) {
    if (strncmp(arg, "--replay-", 9) != 0) {
        return 0;
    }
    const char* eq = strchr(arg, '=');
    size_t name_len = eq ? (size_t)(eq - arg) : strlen(arg);
    if (name_len >= cap) {
        return 0;
    }
    DSD_MEMCPY(name, arg, name_len);
    name[name_len] = '\0';
    *eq_out = eq;
    return replay_is_host_option(name);
}

/* Moves every non-host argument to out (argv[0] included) and applies the host options. */
static int
replay_split_args(int argc, char** argv, char** out, int* out_count) {
    int kept = 0;
    int next = 0;
    while (next < argc) {
        char* arg = argv[next];
        const int is_program = next == 0;
        next++;
        char name[64];
        const char* eq = NULL;
        if (is_program || !replay_host_option_name(arg, name, sizeof(name), &eq)) {
            out[kept++] = arg;
            continue;
        }
        const char* value = NULL;
        if (eq != NULL) {
            value = eq + 1;
        } else if (next < argc) {
            value = argv[next];
            next++;
        }
        if (value == NULL) {
            DSD_FPRINTF(stderr, "replay jitter: %s needs a value\n", name);
            replay_usage();
            return -1;
        }
        if (replay_apply_option(name, value) != 0) {
            return -1;
        }
    }
    if (g_options.jitter_tuned && !g_options.jitter) {
        DSD_FPRINTF(stderr, "replay jitter: --replay-jitter-* settings need --replay-jitter-seed\n");
        return -1;
    }
    out[kept] = NULL;
    *out_count = kept;
    return 0;
}

/* ---- the wrapped read ----------------------------------------------------------------------------------------- */

/* Counts what one read returned, by the batch it came from. Runs on the decoder thread, right after the read, which is
 * where dsd_rtl_stream_metrics_hook_replay_batch() may be called. */
static void
replay_note_read(int got) {
    dsd_rtl_stream_replay_batch batch;
    if (dsd_rtl_stream_metrics_hook_replay_batch(&batch) != 1) {
        g_totals.untagged_samples += (uint64_t)got;
        return;
    }
    switch (batch.output_kind) {
        case RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR: g_totals.fsk_samples += (uint64_t)got; break;
        case RTL_STREAM_OUTPUT_SYMBOL_CQPSK: g_totals.cqpsk_symbols += (uint64_t)got; break;
        case RTL_STREAM_OUTPUT_AUDIO_MONITOR: g_totals.monitor_samples += (uint64_t)got; break;
        default: g_totals.unknown_kind_samples += (uint64_t)got; break;
    }
    if (g_totals.have_generation && batch.generation != g_totals.generation) {
        g_totals.generation_changes++;
    }
    g_totals.generation = batch.generation;
    g_totals.have_generation = 1;
    /* A read hands out samples of one batch only, so the last one sits at first_index + got - 1 and ends at the
       batch-relative position first_index + got. */
    uint64_t end_ns = dsd_decode_clock_batch_media_ns(batch.media_start_ns, batch.media_duration_ns, batch.output_count,
                                                      batch.first_index + (uint32_t)got);
    if (end_ns > g_totals.media_end_ns) {
        g_totals.media_end_ns = end_ns;
    }
}

/* After about one read in --replay-jitter-every, sleeps a uniform [0, max] ms while the budget lasts. The decoder holds
 * the batch it just read meanwhile; the replay reader goes on filling the input ring. */
static void
replay_jitter_after_read(void) {
    if (!g_options.jitter) {
        return;
    }
    if (replay_rng_next(&g_totals.jitter_rng) % g_options.jitter_every != 0U) {
        return;
    }
    const uint64_t budget_us = (uint64_t)g_options.jitter_budget_ms * 1000U;
    if (g_totals.slept_us >= budget_us) {
        return;
    }
    uint64_t delay_us = replay_rng_next(&g_totals.jitter_rng) % ((uint64_t)g_options.jitter_max_ms * 1000U + 1U);
    if (delay_us > budget_us - g_totals.slept_us) {
        delay_us = budget_us - g_totals.slept_us;
    }
    g_totals.sleeps++;
    g_totals.slept_us += delay_us;
    if (delay_us > 0U) {
        dsd_sleep_us(delay_us);
    }
}

static int
replay_perturbed_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    size_t asked = count;
    if (g_options.short_reads && count > 1U) {
        asked = 1U + (size_t)(replay_rng_next(&g_totals.short_rng) % (uint64_t)count);
        if (asked < count) {
            g_totals.shortened_reads++;
        }
    }
    int rc = rtl_stream_read((RtlSdrContext*)rtl_ctx, out, asked, out_got);
    g_totals.reads++;
    if (rc >= 0 && out_got != NULL && *out_got > 0) {
        replay_note_read(*out_got);
    }
    replay_jitter_after_read();
    return rc;
}

static double
replay_return_pwr(const void* rtl_ctx) {
    return rtl_stream_return_pwr((const RtlSdrContext*)rtl_ctx);
}

/* Runs after the engine installed its hooks, so this replacement sticks. */
static int
replay_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    dsd_rtl_stream_io_hooks io = {0};
    io.read = replay_perturbed_read;
    io.return_pwr = replay_return_pwr;
    dsd_rtl_stream_io_hooks_set(io);
    return 0;
}

static void
replay_report(void) {
    DSD_FPRINTF(stderr,
                "REPLAY STREAM: fsk_samples=%llu cqpsk_symbols=%llu monitor_samples=%llu generation_changes=%u "
                "media_ms=%llu.%06llu\n",
                (unsigned long long)g_totals.fsk_samples, (unsigned long long)g_totals.cqpsk_symbols,
                (unsigned long long)g_totals.monitor_samples, g_totals.generation_changes,
                (unsigned long long)(g_totals.media_end_ns / 1000000U),
                (unsigned long long)(g_totals.media_end_ns % 1000000U));
    char jitter[160] = "jitter=off";
    if (g_options.jitter) {
        DSD_SNPRINTF(jitter, sizeof(jitter), "jitter_seed=%u max_ms=%u every=%u budget_ms=%u sleeps=%llu slept_ms=%.3f",
                     g_options.jitter_seed, g_options.jitter_max_ms, g_options.jitter_every, g_options.jitter_budget_ms,
                     (unsigned long long)g_totals.sleeps, (double)g_totals.slept_us / 1000.0);
    }
    char short_reads[96] = "short_reads=off";
    if (g_options.short_reads) {
        DSD_SNPRINTF(short_reads, sizeof(short_reads), "short_seed=%u shortened_reads=%llu", g_options.short_seed,
                     (unsigned long long)g_totals.shortened_reads);
    }
    DSD_FPRINTF(stderr, "REPLAY JITTER: %s %s reads=%llu\n", jitter, short_reads, (unsigned long long)g_totals.reads);
    if (g_totals.untagged_samples > 0U || g_totals.unknown_kind_samples > 0U) {
        DSD_FPRINTF(stderr,
                    "REPLAY STREAM FAIL: %llu samples came without a replay batch tag and %llu from an unknown output "
                    "kind; the host measures I/Q replay input only\n",
                    (unsigned long long)g_totals.untagged_samples, (unsigned long long)g_totals.unknown_kind_samples);
        g_failed = 1;
    }
}

static void
replay_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    replay_report();
}

/* ---- the asynchronous test sink (DSD_NEO_TEST_AUDIO_WRAP) ---------------------------------------------------- */

#ifdef DSD_NEO_TEST_AUDIO_WRAP

#define REPLAY_SINK_MAX_STREAMS 4
#define REPLAY_SINK_RING_MS     1000U /* the real backends' ring and chunk (DSD_PULSE_OUTPUT_RING_MS, _CHUNK_MS) */
#define REPLAY_SINK_CHUNK_MS    20U
#define REPLAY_FNV_OFFSET       0xCBF29CE484222325ULL
#define REPLAY_FNV_PRIME        0x100000001B3ULL

typedef struct {
    int index;
    int sample_rate;
    int channels;
    int async_output;
    int stalled;
    /* Device side: the pump thread and the ring it takes from, all under mu. Only counts are kept; what the device
       would play does not matter here. */
    dsd_mutex_t mu;
    dsd_cond_t cv;
    dsd_thread_t pump;
    int pump_started;
    int stop;
    uint64_t ring_capacity; /* samples */
    uint64_t ring_count;
    uint64_t chunk_samples;
    uint64_t taken;
    uint64_t dropped;
    /* Decoder side: what the decoder handed over, on its own thread. */
    uint64_t writes;
    uint64_t frames;
    uint64_t fnv;
    uint64_t drains;
} replay_sink_stream;

static replay_sink_stream* g_sink_streams[REPLAY_SINK_MAX_STREAMS];
static int g_sink_opened;
static int g_sink_closed;
static int g_sink_backed_up; /* stalled streams that took audio from the decoder and dropped some */

// GNU ld --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
dsd_audio_stream* __real_dsd_audio_open_output(const dsd_audio_params* params);
int __real_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames);
int __real_dsd_audio_drain(dsd_audio_stream* stream);
void __real_dsd_audio_close(dsd_audio_stream* stream);
dsd_audio_stream* __wrap_dsd_audio_open_output(const dsd_audio_params* params);
int __wrap_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames);
int __wrap_dsd_audio_drain(dsd_audio_stream* stream);
void __wrap_dsd_audio_close(dsd_audio_stream* stream);

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

static replay_sink_stream*
replay_sink_find(const dsd_audio_stream* stream) {
    for (int i = 0; i < g_sink_opened; i++) {
        if (g_sink_streams[i] != NULL && (const void*)g_sink_streams[i] == (const void*)stream) {
            return g_sink_streams[i];
        }
    }
    return NULL;
}

/* The device: "free" takes each chunk as soon as there is one; "stalled" takes the first chunk and its write to the
 * device never returns until the stream closes. The ring lock is not held across the device write, as in the real
 * backends' pumps, so the decoder's writes go on queuing (and dropping) meanwhile. */
static DSD_THREAD_RETURN_TYPE
replay_sink_pump(void* arg) {
    replay_sink_stream* s = (replay_sink_stream*)arg;
    dsd_mutex_lock(&s->mu);
    for (;;) {
        while (!s->stop && s->ring_count == 0U) {
            (void)dsd_cond_wait(&s->cv, &s->mu);
        }
        if (s->stop) {
            break;
        }
        uint64_t take = s->ring_count < s->chunk_samples ? s->ring_count : s->chunk_samples;
        s->ring_count -= take;
        s->taken += take;
        if (s->stalled) {
            /* The device write that never completes. */
            while (!s->stop) {
                (void)dsd_cond_wait(&s->cv, &s->mu);
            }
            break;
        }
    }
    dsd_mutex_unlock(&s->mu);
    DSD_THREAD_RETURN;
}

static void
replay_sink_free(replay_sink_stream* s) {
    (void)dsd_cond_destroy(&s->cv);
    (void)dsd_mutex_destroy(&s->mu);
    free(s);
}

dsd_audio_stream*
__wrap_dsd_audio_open_output(const dsd_audio_params* params) {
    if (!g_options.sink) {
        return __real_dsd_audio_open_output(params);
    }
    if (params == NULL || params->sample_rate <= 0 || params->channels <= 0
        || g_sink_opened >= REPLAY_SINK_MAX_STREAMS) {
        return NULL;
    }
    replay_sink_stream* s = (replay_sink_stream*)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->index = g_sink_opened;
    s->sample_rate = params->sample_rate;
    s->channels = params->channels;
    s->async_output = params->async_output;
    s->stalled = g_options.sink_stalled;
    s->fnv = REPLAY_FNV_OFFSET;
    const uint64_t channels = (uint64_t)params->channels;
    s->chunk_samples = (uint64_t)params->sample_rate * REPLAY_SINK_CHUNK_MS / 1000U * channels;
    s->ring_capacity = (uint64_t)params->sample_rate * REPLAY_SINK_RING_MS / 1000U * channels;
    if (s->chunk_samples == 0U) {
        s->chunk_samples = channels;
    }
    if (s->ring_capacity < s->chunk_samples) {
        s->ring_capacity = s->chunk_samples;
    }
    if (dsd_mutex_init(&s->mu) != 0 || dsd_cond_init(&s->cv) != 0) {
        free(s);
        return NULL;
    }
    if (dsd_thread_create(&s->pump, replay_sink_pump, s) != 0) {
        replay_sink_free(s);
        return NULL;
    }
    s->pump_started = 1;
    g_sink_streams[g_sink_opened++] = s;
    return (dsd_audio_stream*)(void*)s;
}

int
__wrap_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    replay_sink_stream* s = replay_sink_find(stream);
    if (s == NULL) {
        return __real_dsd_audio_write(stream, buffer, frames);
    }
    if (buffer == NULL) {
        return -1;
    }
    const uint64_t samples = (uint64_t)frames * (uint64_t)s->channels;
    s->writes++;
    s->frames += (uint64_t)frames;
    for (uint64_t i = 0; i < samples; i++) {
        const uint16_t v = (uint16_t)buffer[i];
        s->fnv = (s->fnv ^ (uint64_t)(v & 0xFFU)) * REPLAY_FNV_PRIME;
        s->fnv = (s->fnv ^ (uint64_t)(v >> 8U)) * REPLAY_FNV_PRIME;
    }
    dsd_mutex_lock(&s->mu);
    if (s->stop) {
        dsd_mutex_unlock(&s->mu);
        return -1;
    }
    if (samples >= s->ring_capacity) {
        /* Keep only the newest window. */
        s->dropped += s->ring_count + (samples - s->ring_capacity);
        s->ring_count = s->ring_capacity;
    } else {
        const uint64_t room = s->ring_capacity - s->ring_count;
        if (room < samples) {
            s->dropped += samples - room;
            s->ring_count -= samples - room;
        }
        s->ring_count += samples;
    }
    (void)dsd_cond_signal(&s->cv);
    dsd_mutex_unlock(&s->mu);
    return (int)frames;
}

int
__wrap_dsd_audio_drain(dsd_audio_stream* stream) {
    replay_sink_stream* s = replay_sink_find(stream);
    if (s == NULL) {
        return __real_dsd_audio_drain(stream);
    }
    s->drains++;
    if (s->stalled) {
        /* The real asynchronous drain waits for the pump to empty the ring, which a wedged device never lets it do:
           a replay that drains would hang there. Fail the leg instead of letting the drain pass unseen. */
        DSD_FPRINTF(stderr, "REPLAY SINK FAIL: the decoder drained stalled stream %d, which would block it\n",
                    s->index);
        g_failed = 1;
        return -1;
    }
    return 0;
}

void
__wrap_dsd_audio_close(dsd_audio_stream* stream) {
    replay_sink_stream* s = replay_sink_find(stream);
    if (s == NULL) {
        __real_dsd_audio_close(stream);
        return;
    }
    dsd_mutex_lock(&s->mu);
    s->stop = 1;
    (void)dsd_cond_broadcast(&s->cv);
    dsd_mutex_unlock(&s->mu);
    if (s->pump_started) {
        (void)dsd_thread_join(s->pump);
    }
    const uint64_t channels = (uint64_t)s->channels;
    DSD_FPRINTF(stderr,
                "REPLAY SINK: stream=%d rate=%d channels=%d async=%d writes=%llu frames=%llu fnv1a=%016llx "
                "drains=%llu\n",
                s->index, s->sample_rate, s->channels, s->async_output, (unsigned long long)s->writes,
                (unsigned long long)s->frames, (unsigned long long)s->fnv, (unsigned long long)s->drains);
    DSD_FPRINTF(stderr, "Replay sink output stats: rate=%d ch=%d mode=%s taken_frames=%llu dropped_frames=%llu\n",
                s->sample_rate, s->channels, s->stalled ? "stalled" : "free", (unsigned long long)(s->taken / channels),
                (unsigned long long)(s->dropped / channels));
    if (!s->async_output) {
        DSD_FPRINTF(stderr,
                    "REPLAY SINK FAIL: stream %d was opened synchronous; a stalled device would block the "
                    "decoder's write\n",
                    s->index);
        g_failed = 1;
    }
    if (s->stalled && s->writes > 0U && s->dropped == 0U) {
        DSD_FPRINTF(stderr, "REPLAY SINK FAIL: stream %d stalled but dropped nothing, so it never backed up\n",
                    s->index);
        g_failed = 1;
    }
    if (s->stalled && s->writes > 0U && s->dropped > 0U) {
        g_sink_backed_up++;
    }
    for (int i = 0; i < g_sink_opened; i++) {
        if (g_sink_streams[i] == s) {
            g_sink_streams[i] = NULL;
        }
    }
    g_sink_closed++;
    replay_sink_free(s);
}

/* After the engine returned: a sink the run asked for must have been opened and closed, and a stalled one must have
 * backed up on at least one stream. A replay whose decoder handed no audio over would otherwise pass vacuously. */
static void
replay_sink_finish(void) {
    if (!g_options.sink) {
        return;
    }
    if (g_sink_opened == 0) {
        DSD_FPRINTF(stderr, "REPLAY SINK FAIL: the engine opened no local audio output (run with -o pulse)\n");
        g_failed = 1;
    }
    if (g_options.sink_stalled && g_sink_backed_up == 0) {
        DSD_FPRINTF(stderr, "REPLAY SINK FAIL: no stalled stream both took audio from the decoder and dropped some, so "
                            "the stall was never tested\n");
        g_failed = 1;
    }
    if (g_sink_closed != g_sink_opened) {
        DSD_FPRINTF(stderr, "REPLAY SINK FAIL: %d of %d sink streams were left open\n", g_sink_opened - g_sink_closed,
                    g_sink_opened);
        g_failed = 1;
    }
}

#else

static void
replay_sink_finish(void) {}

#endif /* DSD_NEO_TEST_AUDIO_WRAP */

int
main(int argc, char** argv) {
    char** args = (char**)calloc((size_t)argc + 1U, sizeof(*args));
    if (args == NULL) {
        return 1;
    }
    g_options.jitter_max_ms = REPLAY_JITTER_DEFAULT_MAX;
    g_options.jitter_every = REPLAY_JITTER_DEFAULT_EVERY;
    g_options.jitter_budget_ms = REPLAY_JITTER_BUDGET_CAP_MS;
    int kept = 0;
    if (replay_split_args(argc, argv, args, &kept) != 0) {
        free((void*)args);
        return 2;
    }
    g_totals.jitter_rng = replay_rng_init(g_options.jitter_seed, 0x6A09E667F3BCC908ULL);
    g_totals.short_rng = replay_rng_init(g_options.short_seed, 0xBB67AE8584CAA73BULL);

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    int rc = 1;
    if (opts != NULL && state != NULL) {
        initOpts(opts);
        initState(state);
        dsd_engine_lifecycle_hooks hooks = {replay_start, replay_stop, NULL};
        if (dsd_runtime_bootstrap(kept, args, opts, state, NULL, &rc) == DSD_BOOTSTRAP_CONTINUE) {
            rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
            replay_sink_finish();
            if (rc == 0 && g_failed) {
                rc = 1;
            }
        }
        freeState(state);
    }
    free(state);
    free(opts);
    free((void*)args);
    return rc;
}
