// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #518 B17: the M17 stream encoder's microphone level through real Codec2, at 3200 and 1600 bit/s.
 *
 * The stream encoder runs end to end over a scripted UDP microphone: a steady vowel, keyed and unkeyed by the TX
 * toggle between 40 ms reads as its command does, with loud room noise and silence between transmissions. What it
 * sends is taken from its M17 IP frames (the Codec2 bits), and what its monitor plays from the digital output.
 *
 * The encoder's input level is measured on a replay: the same input through the same microphone chain, with the same
 * key state per read, encoded by a Codec2 instance of the test's own. The replay is the encoder's input exactly when
 * its Codec2 frames match the transmitted ones bit for bit, which is checked first. The raw decoded output is the
 * transmitted bits through a Codec2 decoder of the test's own, with no chain or AGC after it.
 */

#include <codec2/codec2.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_voice.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/m17_udp_hooks.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "m17_internal.h"
#include "speech_synth.h"

/* One read: 40 ms at 8 kHz, two Codec2 frames at 3200 bit/s and one at 1600. */
#define ITER_SAMPLES 320
#define MAX_ITERS    200
#define MAX_IP       (MAX_ITERS + 8)
#define MAX_MONITOR  ((long)MAX_IP * ITER_SAMPLES)

typedef struct {
    int iters;
    int keyed;         /* the TX toggle for these reads */
    double vowel_gain; /* the vowel's linear gain: 0 for none */
    double noise;      /* uniform room noise amplitude, int16 units */
} mic_segment;

/* The vowel at -30 dBFS, 12 dB up, loud room noise, silence. */
static const mic_segment k_script[] = {
    {50, 1, 1.0, 0.0},     /* settle (reads 0-49) */
    {25, 0, 0.0, 12000.0}, /* unkeyed room noise (50-74) */
    {10, 1, 1.0, 0.0},     /* the next transmission (75-84) */
    {10, 0, 0.0, 0.0},     /* (85-94) */
    {3, 1, 3.981, 0.0},    /* a 120 ms transmission 12 dB up (95-97) */
    {10, 0, 0.0, 0.0},     /* (98-107) */
    {8, 1, 1.0, 0.0},      /* the next transmission (108-115) */
    {10, 0, 0.0, 0.0},     /* (116-125) */
    {5, 1, 3.981, 0.0},    /* a long transmission: 200 ms 12 dB up (126-130) ... */
    {20, 1, 1.0, 0.0},     /* ... then 800 ms of the vowel, the gain rising over its end (131-150) */
    {10, 0, 0.0, 0.0},     /* (151-160) */
    {12, 1, 1.0, 0.0},     /* the next transmission (161-172) */
    {10, 0, 0.0, 0.0},     /* (173-182): the last transmission ends inside the script */
};

static float g_vowel[MAX_ITERS * ITER_SAMPLES];
static int16_t g_input[MAX_ITERS * ITER_SAMPLES];
static int g_keyed[MAX_ITERS];
/* Reads since the transmission keyed (0 for the read that keys it); -1 while unkeyed. */
static int g_keyed_for[MAX_ITERS];
static long g_reads;
static long g_iters;
static dsd_state* g_state;
static uint64_t g_noise;

/* The M17 IP stream frames sent, as Codec2 bytes, and the monitor's output; each with its count as each read starts. */
static uint8_t g_ip_voice[MAX_IP][16];
static int g_ip_count;
static int g_ip_at[MAX_ITERS + 1];
static int16_t g_monitor[MAX_MONITOR];
static long g_monitor_count;
static long g_monitor_at[MAX_ITERS + 1];

static int g_failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
            DSD_FPRINTF(stderr, "\n");                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static const mic_segment*
segment_at(long iter) {
    for (size_t s = 0U; s < sizeof k_script / sizeof k_script[0]; s++) {
        if (iter < k_script[s].iters) {
            return &k_script[s];
        }
        iter -= k_script[s].iters;
    }
    return NULL;
}

static long
script_iters(void) {
    long n = 0;
    for (size_t s = 0U; s < sizeof k_script / sizeof k_script[0]; s++) {
        n += k_script[s].iters;
    }
    return n;
}

static double
noise_sample(void) {
    g_noise ^= g_noise >> 12;
    g_noise ^= g_noise << 25;
    g_noise ^= g_noise >> 27;
    return ((double)((g_noise * 2685821657736338717ULL) >> 11) / 4503599627370496.0) - 1.0;
}

static int
fake_udp_mic_read(dsd_opts* opts, int16_t* out) {
    (void)opts;
    const long iter = g_reads / ITER_SAMPLES;
    const mic_segment* seg = segment_at(iter);
    if (seg == NULL || out == NULL) {
        return 0;
    }
    if ((g_reads % ITER_SAMPLES) == 0) {
        g_ip_at[iter] = g_ip_count;
        g_monitor_at[iter] = g_monitor_count;
        g_iters = iter + 1;
        /* What the TX toggle command does (apply_cmd_m17_tx_toggle()), before the read it applies to. */
        if (seg->keyed == 0 && g_state->m17encoder_tx == 1) {
            g_state->m17encoder_eot = 1;
        }
        g_state->m17encoder_tx = seg->keyed;
        g_keyed[iter] = seg->keyed;
        g_keyed_for[iter] = !seg->keyed ? -1 : (iter > 0 && g_keyed[iter - 1] ? g_keyed_for[iter - 1] + 1 : 0);
    }
    const double v = ((double)g_vowel[g_reads] * seg->vowel_gain) + (seg->noise * noise_sample());
    g_input[g_reads] = (int16_t)lrint(v);
    *out = g_input[g_reads];
    g_reads++;
    return 1;
}

static int
fake_m17_connect(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

/* An IP stream frame: the magic (4 bytes), the stream id (2), the LSF (28), the frame number (2), then the two 8-byte
   Codec2 payloads (dsd-neo's m17_str_build_ip_frame()). */
static int
fake_m17_blaster(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    const uint8_t* bytes = (const uint8_t*)data;
    if (nsam == 54U && bytes != NULL && bytes[0] == 0x4D && bytes[1] == 0x31 && g_ip_count < MAX_IP) {
        DSD_MEMCPY(g_ip_voice[g_ip_count], bytes + 36, 16);
        g_ip_count++;
    }
    return 0;
}

static void
fake_udp_audio_blast(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    const size_t count = nsam / sizeof(int16_t);
    if (data == NULL || g_monitor_count + (long)count > MAX_MONITOR) {
        return;
    }
    DSD_MEMCPY(g_monitor + g_monitor_count, data, count * sizeof(int16_t));
    g_monitor_count += (long)count;
}

/* The decoded voice's staging ring: room for every frame the monitor plays, so it never wraps. */
static short g_history[100 + (2 * MAX_MONITOR)];

/* Run the stream encoder over the script at 3200 (@p dt 2) or 1600 bit/s (3) with its monitor on the digital output at
   a fixed -g 3 (which the monitor does not take), the microphone on auto gain with no filters. */
static int
run_encoder(uint8_t dt) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    state.codec2_3200 = codec2_create(CODEC2_MODE_3200);
    state.codec2_1600 = codec2_create(CODEC2_MODE_1600);
    if (!state.codec2_3200 || !state.codec2_1600) {
        DSD_FPRINTF(stderr, "codec2_create failed\n");
        return -1;
    }
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.input_volume_multiplier = 1;
    opts.audio_gainA = 0.0f;
    opts.audio_gain = 3.0f;
    opts.monitor_input_audio = 0;
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    opts.pulse_digi_out_channels = 1;
    opts.pulse_digi_rate_out = 8000;
    opts.m17_use_ip = 1;
    state.audio_out_buf = g_history;
    state.audio_out_buf_p = g_history + 100;
    state.m17_rate = 8000;
    state.m17_can_en = -1;
    state.m17_str_dt = dt;
    g_state = &state;
    g_reads = 0;
    g_iters = 0;
    g_noise = 0x9E3779B97F4A7C15ULL;
    g_ip_count = 0;
    g_monitor_count = 0;
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){.udp_read_sample = fake_udp_mic_read});
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){.connect = fake_m17_connect, .blaster = fake_m17_blaster});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = fake_udp_audio_blast});
    exitflag = 0;
    (void)encodeM17STR(&opts, &state);
    exitflag = 0;
    g_ip_at[g_iters] = g_ip_count;
    g_monitor_at[g_iters] = g_monitor_count;
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_m17_udp_hooks_set((dsd_m17_udp_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
    codec2_destroy(state.codec2_3200);
    codec2_destroy(state.codec2_1600);
    g_state = NULL;
    return 0;
}

/* Per read: the encoder's input (the replay) and the raw decoded output, frame-aligned, ITER_SAMPLES each. */
static int16_t g_encoded_in[MAX_ITERS * ITER_SAMPLES];
static int16_t g_decoded[MAX_ITERS * ITER_SAMPLES];
static int g_sent[MAX_ITERS];

/* Replay the input through a microphone chain of the test's own with the script's key state, encode it with a Codec2
   of the test's own, and check every frame the encoder sent matches; decode what it sent. */
static void
replay_and_decode(uint8_t dt) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.audio_gainA = 0.0f;
    m17_voice_chain chain;
    m17_voice_chain_init(&chain);
    struct CODEC2* enc = codec2_create(dt == 3U ? CODEC2_MODE_1600 : CODEC2_MODE_3200);
    struct CODEC2* dec = codec2_create(dt == 3U ? CODEC2_MODE_1600 : CODEC2_MODE_3200);
    if (!enc || !dec) {
        CHECK(0, "codec2_create failed");
        codec2_destroy(enc);
        codec2_destroy(dec);
        return;
    }
    const int frames = dt == 3U ? 1 : 2;
    const size_t nsam = (size_t)codec2_samples_per_frame(enc);
    int matched = 0;
    int mismatched = 0;
    for (long it = 0; it < g_iters; it++) {
        int16_t* in = g_encoded_in + (it * ITER_SAMPLES);
        DSD_MEMCPY(in, g_input + (it * ITER_SAMPLES), ITER_SAMPLES * sizeof(int16_t));
        uint8_t bits[16];
        DSD_MEMSET(bits, 0, sizeof bits);
        for (int f = 0; f < frames; f++) {
            m17_voice_chain_process(&opts, &state, &chain, in + ((size_t)f * nsam), nsam, g_keyed[it]);
            codec2_encode(enc, bits + ((size_t)f * 8U), in + ((size_t)f * nsam));
        }
        g_sent[it] = g_ip_at[it + 1] - g_ip_at[it];
        if (g_sent[it] != 1) {
            continue;
        }
        const uint8_t* sent = g_ip_voice[g_ip_at[it]];
        if (memcmp(sent, bits, (size_t)frames * 8U) == 0) {
            matched++;
        } else {
            mismatched++;
        }
        for (int f = 0; f < frames; f++) {
            codec2_decode(dec, g_decoded + (it * ITER_SAMPLES) + ((size_t)f * nsam), sent + ((size_t)f * 8U));
        }
    }
    CHECK(matched > 100 && mismatched == 0, "%s: %d sent frames matched the replay, %d did not",
          dt == 3U ? "1600" : "3200", matched, mismatched);
    codec2_destroy(enc);
    codec2_destroy(dec);
}

/* The RMS level of reads [@p from, @p to) of @p x, in dBFS re 32768. */
static double
level_db(const int16_t* x, long from, long to) {
    double acc = 0.0;
    for (long i = from * ITER_SAMPLES; i < to * ITER_SAMPLES; i++) {
        acc += (double)x[i] * (double)x[i];
    }
    const double n = (double)((to - from) * ITER_SAMPLES);
    return acc > 0.0 ? 10.0 * log10(acc / n) - 20.0 * log10(32768.0) : -999.0;
}

/* Every read in [@p from, @p to) was sent as one frame. */
static int
all_sent(long from, long to) {
    int ok = 1;
    for (long it = from; it < to; it++) {
        ok &= g_sent[it] == 1;
    }
    return ok;
}

/* How far the raw decoded output's level moved from reads [@p a0, @p a1) to [@p b0, @p b1), against how far the codec2
   input's did, in dB. */
static double
decoded_tracks_input(long a0, long a1, long b0, long b1) {
    const double in = level_db(g_encoded_in, b0, b1) - level_db(g_encoded_in, a0, a1);
    const double out = level_db(g_decoded, b0, b1) - level_db(g_decoded, a0, a1);
    return out - in;
}

/* The codec2 input (exact, from the replay) and the raw decoded output through the key-ups. Codec2 does not keep the
   level exactly: it sends each frame's energy in 5 bits over -10..40 dB, steps of about 1.6 dB, and decodes this vowel
   up to 2.4 dB off its input (measured on the system libcodec2 1.2.0 and on dsd-neo's pinned fork, which Windows and
   macOS build, compiled with GCC -O3, with Clang and FMA contraction, and with -ffast-math). So the decoded output is held
   to its input within 3 dB, and its level changes to the input's within 3 dB (measured up to 1.9 dB, at 1600); a
   decoded comparison skips a transmission's first two reads, where the decoder is still coming out of the frames
   before. */
static void
check_levels(uint8_t dt) {
    const char* rate = dt == 3U ? "1600" : "3200";
    /* Settled: the vowel's peaks at the AGC's target, -10 dBFS. */
    double peak = 0.0;
    double acc = 0.0;
    for (long i = 40L * ITER_SAMPLES; i < 50L * ITER_SAMPLES; i++) {
        peak = fmax(peak, fabs((double)g_input[i]));
        acc += (double)g_input[i] * (double)g_input[i];
    }
    const double crest_db = 20.0 * log10(peak / sqrt(acc / (10.0 * ITER_SAMPLES)));
    const double target_db = 20.0 * log10((double)DSD_VOICE_AGC_TARGET_PEAK / 32768.0);
    const double settled_in = level_db(g_encoded_in, 40, 50);
    const double settled_out = level_db(g_decoded, 40, 50);
    CHECK(all_sent(0, 50), "%s: the settling transmission was not all sent", rate);
    CHECK(fabs(settled_in - (target_db - crest_db)) < 0.05, "%s: codec2 input %.2f dBFS, want %.2f", rate, settled_in,
          target_db - crest_db);
    CHECK(fabs(settled_out - settled_in) < 3.0, "%s: raw decoded %.2f dBFS against an input of %.2f", rate, settled_out,
          settled_in);

    /* Unkeyed room noise moved nothing: the next transmission goes out at the level the last one ended on. */
    CHECK(all_sent(75, 85), "%s: the transmission after the noise was not all sent", rate);
    const double after_noise = level_db(g_encoded_in, 75, 80) - level_db(g_encoded_in, 45, 50);
    CHECK(fabs(after_noise) < 0.05, "%s: after unkeyed room noise the codec2 input moved %.2f dB", rate, after_noise);
    const double noise_out = decoded_tracks_input(40, 50, 77, 85);
    CHECK(fabs(noise_out) < 3.0, "%s: after unkeyed room noise the decoded output is %.2f dB off the input", rate,
          noise_out);

    /* A 120 ms transmission 12 dB up leaves the gain 12 dB lower for the next one. */
    const double short_drop = 20.0 * log10(1.0 / 3.981);
    CHECK(all_sent(108, 116), "%s: the transmission after the short one was not all sent", rate);
    const double after_short = level_db(g_encoded_in, 108, 113) - level_db(g_encoded_in, 45, 50);
    CHECK(fabs(after_short - short_drop) < 0.05,
          "%s: after a 120 ms transmission the codec2 input moved %.2f dB, want "
          "%.2f",
          rate, after_short, short_drop);
    const double short_out = decoded_tracks_input(40, 50, 110, 116);
    CHECK(fabs(short_out) < 3.0, "%s: after a 120 ms transmission the decoded output is %.2f dB off the input", rate,
          short_out);

    /* A long transmission whose gain rose over its end: the next goes on where it ended (one read's release). */
    CHECK(all_sent(161, 173), "%s: the transmission after the long one was not all sent", rate);
    const double rose_in = level_db(g_encoded_in, 150, 151) - level_db(g_encoded_in, 144, 145);
    const double next_in = level_db(g_encoded_in, 161, 162) - level_db(g_encoded_in, 150, 151);
    CHECK(rose_in > 4.0, "%s: the gain rose only %.2f dB over the long transmission's end", rate, rose_in);
    CHECK(fabs(next_in - 0.8) < 0.1, "%s: after a long transmission the codec2 input moved %.2f dB, want 0.8", rate,
          next_in);
    const double long_out = decoded_tracks_input(141, 151, 163, 173);
    CHECK(fabs(long_out) < 3.0, "%s: after a long transmission the decoded output is %.2f dB off the input", rate,
          long_out);
}

/* The monitor plays what was sent at the level it was decoded at, at -g 3 as at unity: over every read sent, the level of
   the raw decoded output, and read for read on the vowel once a transmission is two reads in. Levels, not samples, and
   not read for read on noise or at a key-up: codec2 synthesizes unvoiced sound with random phases from one process-wide
   generator, so the monitor's decoder and the test's, drawing at different times, differ sample by sample there, and a
   read's level with them (up to 3.9 dB measured across codec2 builds). On the vowel the two stay within 0.4 dB in every
   build measured (see check_levels()), and within 0.1 dB over the whole run; the limits leave room for builds not
   measured and still catch a monitor that took -g 3 (+9.5 dB). */
static void
check_monitor(uint8_t dt) {
    long compared = 0;
    long voiced = 0;
    long off = 0;
    double worst = 0.0;
    double mon_acc = 0.0;
    double dec_acc = 0.0;
    for (long it = 0; it < g_iters; it++) {
        if (g_sent[it] != 1 || g_monitor_at[it + 1] - g_monitor_at[it] != ITER_SAMPLES) {
            continue;
        }
        double mon = 0.0;
        double dec = 0.0;
        for (long i = 0; i < ITER_SAMPLES; i++) {
            const double d = (double)g_decoded[(it * ITER_SAMPLES) + i];
            const double m = (double)g_monitor[g_monitor_at[it] + i];
            dec += d * d;
            mon += m * m;
        }
        if (!(dec > 0.0 && mon > 0.0)) {
            continue;
        }
        mon_acc += mon;
        dec_acc += dec;
        compared++;
        const mic_segment* seg = segment_at(it);
        if (g_keyed_for[it] >= 2 && seg != NULL && seg->vowel_gain > 0.0) {
            const double diff = 10.0 * log10(mon / dec);
            worst = fabs(diff) > fabs(worst) ? diff : worst;
            off += fabs(diff) > 1.0;
            voiced++;
        }
    }
    const double overall = dec_acc > 0.0 && mon_acc > 0.0 ? 10.0 * log10(mon_acc / dec_acc) : 999.0;
    CHECK(
        compared > 100 && voiced > 80 && off == 0 && fabs(overall) < 0.2,
        "%s: the monitor played %ld reads, %+.3f dB off the raw decoded output; %ld of %ld vowel reads more than 1 dB "
        "off (worst %+.2f dB)",
        dt == 3U ? "1600" : "3200", compared, overall, off, voiced, worst);
}

int
main(void) {
    const long iters = script_iters();
    if (iters > MAX_ITERS) {
        DSD_FPRINTF(stderr, "script too long\n");
        return 1;
    }
    dsd_test_vowel_params v;
    dsd_test_vowel_defaults(&v, 7U);
    v.level_dbfs = -30.0;
    v.f0_hz = 125.0; /* 64 samples a period: every read holds five */
    v.duration_s = (double)(iters * ITER_SAMPLES) / 8000.0;
    v.steady = 1;
    if (dsd_test_synth_vowel(g_vowel, (size_t)(iters * ITER_SAMPLES), 8000, &v) != 0) {
        DSD_FPRINTF(stderr, "vowel synthesis failed\n");
        return 1;
    }
    const uint8_t rates[2] = {2U, 3U};
    for (int r = 0; r < 2; r++) {
        if (run_encoder(rates[r]) != 0) {
            return 1;
        }
        CHECK(g_iters == iters, "the encoder read %ld of %ld reads", g_iters, iters);
        replay_and_decode(rates[r]);
        check_levels(rates[r]);
        check_monitor(rates[r]);
    }
    if (g_failures) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("M17_ENCODER_CODEC2: OK\n");
    return 0;
}
