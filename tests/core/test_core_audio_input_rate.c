// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/dsp/resampler.h"
#include "dsd-neo/platform/file_compat.h"
#include "test_support.h"

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float_eq(const char* label, float got, float want) {
    if (fabsf(got - want) > 1e-6f) {
        fprintf(stderr, "FAIL: %s (got %.6f want %.6f)\n", label, got, want);
        return 1;
    }
    return 0;
}

static dsd_state*
alloc_state(void) {
    return (dsd_state*)calloc(1, sizeof(dsd_state));
}

static int
create_temp_raw_pcm_wav_suffix(const char* prefix, const short* samples, size_t sample_count, char* out_path,
                               size_t out_path_sz) {
    if (!prefix || !samples || sample_count == 0 || !out_path || out_path_sz == 0) {
        fprintf(stderr, "FAIL: invalid raw temp file request\n");
        return 1;
    }

    char base_path[DSD_TEST_PATH_MAX] = {0};
    int fd = dsd_test_mkstemp(base_path, sizeof base_path, prefix);
    if (fd < 0) {
        fprintf(stderr, "FAIL: dsd_test_mkstemp failed for %s\n", prefix);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(base_path);

    if (snprintf(out_path, out_path_sz, "%s.wav", base_path) >= (int)out_path_sz) {
        fprintf(stderr, "FAIL: temp raw wav path too long for %s\n", prefix);
        return 1;
    }

    FILE* fp = fopen(out_path, "wb");
    if (!fp) {
        fprintf(stderr, "FAIL: fopen write failed for %s\n", out_path);
        return 1;
    }

    size_t nwritten = fwrite(samples, sizeof(samples[0]), sample_count, fp);
    fclose(fp);
    if (nwritten != sample_count) {
        fprintf(stderr, "FAIL: fwrite failed for %s\n", out_path);
        (void)remove(out_path);
        return 1;
    }

    return 0;
}

static int
create_temp_wav_family_file(const char* prefix, int sample_rate, int format, const short* samples, size_t sample_count,
                            const char* expected_magic, char* out_path, size_t out_path_sz) {
    if (!prefix || !samples || sample_count == 0 || !expected_magic || !out_path || out_path_sz == 0
        || sample_rate <= 0) {
        fprintf(stderr, "FAIL: invalid wav family temp file request\n");
        return 1;
    }

    char base_path[DSD_TEST_PATH_MAX] = {0};
    int fd = dsd_test_mkstemp(base_path, sizeof base_path, prefix);
    if (fd < 0) {
        fprintf(stderr, "FAIL: dsd_test_mkstemp failed for %s\n", prefix);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(base_path);

    if (snprintf(out_path, out_path_sz, "%s.wav", base_path) >= (int)out_path_sz) {
        fprintf(stderr, "FAIL: temp wav family path too long for %s\n", prefix);
        return 1;
    }

    SF_INFO info = {0};
    info.samplerate = sample_rate;
    info.channels = 1;
    info.format = format;

    SNDFILE* sf = sf_open(out_path, SFM_WRITE, &info);
    if (sf == NULL) {
        fprintf(stderr, "FAIL: sf_open write failed for %s: %s\n", out_path, sf_strerror(NULL));
        (void)remove(out_path);
        return 1;
    }

    if (sf_write_short(sf, samples, (sf_count_t)sample_count) != (sf_count_t)sample_count) {
        fprintf(stderr, "FAIL: sf_write_short failed for %s\n", out_path);
        sf_close(sf);
        (void)remove(out_path);
        return 1;
    }
    if (sf_close(sf) != 0) {
        fprintf(stderr, "FAIL: sf_close failed for %s\n", out_path);
        (void)remove(out_path);
        return 1;
    }

    FILE* fp = fopen(out_path, "rb");
    if (!fp) {
        fprintf(stderr, "FAIL: fopen read failed for %s\n", out_path);
        (void)remove(out_path);
        return 1;
    }

    unsigned char header[12] = {0};
    size_t nread = fread(header, 1, sizeof header, fp);
    fclose(fp);

    if (nread != sizeof header || memcmp(header, expected_magic, 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "FAIL: unexpected wav family header for %s\n", out_path);
        (void)remove(out_path);
        return 1;
    }

    return 0;
}

static int
test_input_rate_rescale_for_72000(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.wav_decimator = 48000;
    opts.wav_sample_rate = 48000;

    dsd_state* state = alloc_state();
    if (!state) {
        fprintf(stderr, "FAIL: alloc state\n");
        dsd_resampler_reset(&opts.input_resampler);
        return 1;
    }

    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    state->jitter = 3;
    init_audio_filters(state, 48000);
    float old_coef = state->RCFilter.coef[0];

    dsd_audio_apply_input_sample_rate(&opts, state, 48000, 72000);

    int rc = 0;
    rc |= expect_int_eq("72000 raw rate applied", opts.wav_sample_rate, 72000);
    rc |= expect_int_eq("72000 interpolator stays 1", opts.wav_interpolator, 1);
    rc |= expect_int_eq("72000 effective rate", dsd_opts_effective_input_rate(&opts), 72000);
    rc |= expect_int_eq("72000 sps rescales", state->samplesPerSymbol, 15);
    rc |= expect_int_eq("72000 center rescales", state->symbolCenter, 6);
    rc |= expect_int_eq("72000 jitter reset", state->jitter, -1);
    rc |= expect_true("72000 filter coefficients rebuilt", fabsf(state->RCFilter.coef[0] - old_coef) > 1e-6f);

    free(state);
    return rc;
}

static int
test_input_rate_rescale_for_44100(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.wav_decimator = 48000;
    opts.wav_sample_rate = 48000;

    dsd_state* state = alloc_state();
    if (!state) {
        fprintf(stderr, "FAIL: alloc state\n");
        dsd_resampler_reset(&opts.input_resampler);
        return 1;
    }

    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    state->jitter = 7;
    init_audio_filters(state, 48000);
    float old_coef = state->RCFilter.coef[0];

    dsd_audio_apply_input_sample_rate(&opts, state, 48000, 44100);

    int rc = 0;
    rc |= expect_int_eq("44100 raw rate applied", opts.wav_sample_rate, 44100);
    rc |= expect_int_eq("44100 interpolator stays 1", opts.wav_interpolator, 1);
    rc |= expect_int_eq("44100 effective rate", dsd_opts_effective_input_rate(&opts), 44100);
    rc |= expect_int_eq("44100 sps rescales", state->samplesPerSymbol, 9);
    rc |= expect_int_eq("44100 center rescales", state->symbolCenter, 4);
    rc |= expect_int_eq("44100 jitter reset", state->jitter, -1);
    rc |= expect_true("44100 filter coefficients rebuilt", fabsf(state->RCFilter.coef[0] - old_coef) > 1e-6f);

    free(state);
    return rc;
}

static int
test_input_rate_rejects_unsupported_staged_upsample_factor(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.wav_decimator = 48000;
    opts.wav_sample_rate = 48000;
    opts.input_upsample_prev = 123.0f;
    opts.input_upsample_len = 3;
    opts.input_upsample_pos = 1;
    opts.input_upsample_prev_valid = 1;
    for (size_t i = 0; i < sizeof(opts.input_upsample_buf) / sizeof(opts.input_upsample_buf[0]); i++) {
        opts.input_upsample_buf[i] = (float)i;
    }

    dsd_state* state = alloc_state();
    if (!state) {
        fprintf(stderr, "FAIL: alloc state\n");
        return 1;
    }

    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    init_audio_filters(state, 48000);

    dsd_audio_apply_input_sample_rate(&opts, state, 48000, 6000);

    int rc = 0;
    rc |= expect_int_eq("6000 raw rate applied", opts.wav_sample_rate, 6000);
    rc |= expect_int_eq("6000 unsupported staged factor falls back to 1", dsd_opts_input_upsample_factor(&opts), 1);
    rc |= expect_int_eq("6000 effective rate stays raw", dsd_opts_effective_input_rate(&opts), 6000);
    rc |= expect_int_eq("6000 sps rescales to low-rate clamp", state->samplesPerSymbol, 2);
    rc |= expect_int_eq("6000 center rescales to low-rate clamp", state->symbolCenter, 0);
    rc |= expect_int_eq("6000 clears staged upsample length", opts.input_upsample_len, 0);
    rc |= expect_int_eq("6000 clears staged upsample position", opts.input_upsample_pos, 0);
    rc |= expect_int_eq("6000 clears staged upsample validity", opts.input_upsample_prev_valid, 0);
    rc |= expect_float_eq("6000 clears staged upsample previous sample", opts.input_upsample_prev, 0.0f);

    free(state);
    return rc;
}

static int
test_input_rate_preserves_48k_effective_timing_for_24000(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.wav_decimator = 48000;
    opts.wav_sample_rate = 48000;
    if (!dsd_resampler_design(&opts.input_resampler, 6, 1)) {
        fprintf(stderr, "FAIL: input resampler design\n");
        return 1;
    }
    opts.input_upsample_prev = 123.0f;
    opts.input_upsample_len = 3;
    opts.input_upsample_pos = 1;
    opts.input_upsample_prev_valid = 1;
    for (size_t i = 0; i < sizeof(opts.input_upsample_buf) / sizeof(opts.input_upsample_buf[0]); i++) {
        opts.input_upsample_buf[i] = (float)i;
    }

    dsd_state* state = alloc_state();
    if (!state) {
        fprintf(stderr, "FAIL: alloc state\n");
        return 1;
    }

    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    init_audio_filters(state, 48000);

    dsd_audio_apply_input_sample_rate(&opts, state, 48000, 24000);

    int rc = 0;
    rc |= expect_int_eq("24000 raw rate applied", opts.wav_sample_rate, 24000);
    rc |= expect_int_eq("24000 effective rate stays 48000 via staged upsample", dsd_opts_effective_input_rate(&opts),
                        48000);
    rc |= expect_int_eq("24000 keeps 48k sps", state->samplesPerSymbol, 10);
    rc |= expect_int_eq("24000 keeps 48k center", state->symbolCenter, 4);
    rc |= expect_int_eq("24000 clears staged upsample length", opts.input_upsample_len, 0);
    rc |= expect_int_eq("24000 clears staged upsample position", opts.input_upsample_pos, 0);
    rc |= expect_int_eq("24000 clears staged upsample validity", opts.input_upsample_prev_valid, 0);
    rc |= expect_float_eq("24000 clears staged upsample previous sample", opts.input_upsample_prev, 0.0f);
    rc |= expect_int_eq("24000 resets compiled input resampler", opts.input_resampler.enabled, 0);
    rc |= expect_int_eq("24000 clears compiled input resampler L", opts.input_resampler.L, 1);
    rc |= expect_int_eq("24000 clears compiled input resampler M", opts.input_resampler.M, 1);
    rc |= expect_true("24000 frees compiled input resampler taps", opts.input_resampler.taps == NULL);
    rc |= expect_true("24000 frees compiled input resampler hist", opts.input_resampler.hist == NULL);

    free(state);
    return rc;
}

static int
test_linear_upsample_block_ends_on_current_sample(void) {
    float out[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    size_t n = dsd_audio_linear_upsample_block_f32(0.0f, 4.0f, 4, out, 4);

    int rc = 0;
    rc |= expect_int_eq("linear block writes requested sample count", (int)n, 4);
    rc |= expect_float_eq("linear block sample 0", out[0], 1.0f);
    rc |= expect_float_eq("linear block sample 1", out[1], 2.0f);
    rc |= expect_float_eq("linear block sample 2", out[2], 3.0f);
    rc |= expect_float_eq("linear block sample 3 reaches current sample", out[3], 4.0f);
    return rc;
}

static int
test_source_effective_input_rate_classifier(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.wav_sample_rate = 72000;

    int rc = 0;

    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse");
    rc |= expect_int_eq("pulse ignores stale file sample rate", dsd_opts_source_uses_effective_input_rate(&opts), 0);

    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "/tmp/input.wav");
    rc |= expect_int_eq("file input uses effective sample rate", dsd_opts_source_uses_effective_input_rate(&opts), 1);

    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl_capture.wav");
    rc |= expect_int_eq("rtl-prefixed filename still uses effective sample rate",
                        dsd_opts_source_uses_effective_input_rate(&opts), 1);

    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "pulse_96000.wav");
    rc |= expect_int_eq("pulse-prefixed filename still uses effective sample rate",
                        dsd_opts_source_uses_effective_input_rate(&opts), 1);

    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy.raw");
    rc |= expect_int_eq("soapy-prefixed filename still uses effective sample rate",
                        dsd_opts_source_uses_effective_input_rate(&opts), 1);

    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "udp:127.0.0.1:7355");
    rc |= expect_int_eq("udp input uses effective sample rate", dsd_opts_source_uses_effective_input_rate(&opts), 1);

    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0:100000000");
    rc |=
        expect_int_eq("rtl input bypasses effective sample rate", dsd_opts_source_uses_effective_input_rate(&opts), 0);

    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:127.0.0.1:1234");
    rc |= expect_int_eq("rtltcp input bypasses effective sample rate", dsd_opts_source_uses_effective_input_rate(&opts),
                        0);

    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=airspy");
    rc |= expect_int_eq("soapy input bypasses effective sample rate", dsd_opts_source_uses_effective_input_rate(&opts),
                        0);

    opts.audio_in_dev[0] = '\0';
    opts.audio_in_type = AUDIO_IN_PULSE;
    rc |= expect_int_eq("pulse type fallback bypasses effective sample rate",
                        dsd_opts_source_uses_effective_input_rate(&opts), 0);

    return rc;
}

static int
test_current_input_timing_rate_prefers_active_backend(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));

    int rc = 0;

    opts.wav_sample_rate = 72000;
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.pulse_digi_rate_in = 48000;
    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "/tmp/requested.wav");
    rc |= expect_int_eq("mixed pulse/file state keeps live pulse timing", dsd_opts_current_input_timing_rate(&opts),
                        48000);

    opts.wav_sample_rate = 24000;
    opts.audio_in_type = AUDIO_IN_TCP;
    snprintf(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "tcp:127.0.0.1:7355");
    rc |= expect_int_eq("tcp timing uses staged 48k effective rate", dsd_opts_current_input_timing_rate(&opts), 48000);

    return rc;
}

static int
test_headerless_wav_suffix_falls_back_to_raw_pcm(void) {
    const short samples[] = {1234, -2345, 3456, -4567};
    char path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_raw_pcm_wav_suffix("dsdneo_raw_pcm_suffix", samples, sizeof samples / sizeof samples[0], path,
                                       sizeof path)
        != 0) {
        return 1;
    }

    SNDFILE* file = NULL;
    SF_INFO* info = NULL;
    int active_sample_rate = 0;
    int opened_as_container = 1;
    int rc = dsd_audio_open_mono_file_input(path, 24000, &file, &info, &active_sample_rate, &opened_as_container);
    if (rc != 0) {
        fprintf(stderr, "FAIL: dsd_audio_open_mono_file_input failed for %s: %s\n", path, sf_strerror(NULL));
        (void)remove(path);
        return 1;
    }

    short sample = 0;
    sf_count_t nread = sf_read_short(file, &sample, 1);

    rc = 0;
    rc |= expect_int_eq("headerless wav suffix uses configured raw sample rate", active_sample_rate, 24000);
    rc |= expect_int_eq("headerless wav suffix does not open as container", opened_as_container, 0);
    rc |= expect_int_eq("headerless wav suffix keeps mono raw channels", info->channels, 1);
    rc |= expect_int_eq("headerless wav suffix reads first raw sample", (int)nread, 1);
    rc |= expect_int_eq("headerless wav suffix preserves sample data", sample, samples[0]);

    sf_close(file);
    free(info);
    (void)remove(path);
    return rc;
}

static int
test_rifx_wav_suffix_uses_container_metadata(void) {
    const short samples[] = {0x1234, -0x1234, 0x0456, -0x0456};
    char path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_wav_family_file("dsdneo_rifx_suffix", 44100, SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_ENDIAN_BIG,
                                    samples, sizeof samples / sizeof samples[0], "RIFX", path, sizeof path)
        != 0) {
        return 1;
    }

    SNDFILE* file = NULL;
    SF_INFO* info = NULL;
    int active_sample_rate = 0;
    int opened_as_container = 0;
    int rc = dsd_audio_open_mono_file_input(path, 24000, &file, &info, &active_sample_rate, &opened_as_container);
    if (rc != 0) {
        fprintf(stderr, "FAIL: dsd_audio_open_mono_file_input failed for %s: %s\n", path, sf_strerror(NULL));
        (void)remove(path);
        return 1;
    }

    short sample = 0;
    sf_count_t nread = sf_read_short(file, &sample, 1);

    rc = 0;
    rc |= expect_int_eq("RIFX wav suffix uses header sample rate", active_sample_rate, 44100);
    rc |= expect_int_eq("RIFX wav suffix opens as container", opened_as_container, 1);
    rc |= expect_int_eq("RIFX wav suffix preserves mono channels", info->channels, 1);
    rc |= expect_int_eq("RIFX wav suffix reads first sample", (int)nread, 1);
    rc |= expect_int_eq("RIFX wav suffix decodes big-endian sample correctly", sample, samples[0]);

    sf_close(file);
    free(info);
    (void)remove(path);
    return rc;
}

static int
test_rf64_wav_suffix_uses_container_metadata(void) {
    const short samples[] = {0x1234, -0x1234, 0x0456, -0x0456};
    char path[DSD_TEST_PATH_MAX] = {0};
    if (create_temp_wav_family_file("dsdneo_rf64_suffix", 32000, SF_FORMAT_RF64 | SF_FORMAT_PCM_16, samples,
                                    sizeof samples / sizeof samples[0], "RF64", path, sizeof path)
        != 0) {
        return 1;
    }

    SNDFILE* file = NULL;
    SF_INFO* info = NULL;
    int active_sample_rate = 0;
    int opened_as_container = 0;
    int rc = dsd_audio_open_mono_file_input(path, 24000, &file, &info, &active_sample_rate, &opened_as_container);
    if (rc != 0) {
        fprintf(stderr, "FAIL: dsd_audio_open_mono_file_input failed for %s: %s\n", path, sf_strerror(NULL));
        (void)remove(path);
        return 1;
    }

    short sample = 0;
    sf_count_t nread = sf_read_short(file, &sample, 1);

    rc = 0;
    rc |= expect_int_eq("RF64 wav suffix uses header sample rate", active_sample_rate, 32000);
    rc |= expect_int_eq("RF64 wav suffix opens as container", opened_as_container, 1);
    rc |= expect_int_eq("RF64 wav suffix preserves mono channels", info->channels, 1);
    rc |= expect_int_eq("RF64 wav suffix reads first sample", (int)nread, 1);
    rc |= expect_int_eq("RF64 wav suffix decodes first sample from audio data", sample, samples[0]);

    sf_close(file);
    free(info);
    (void)remove(path);
    return rc;
}

static int
test_reset_input_upsample_state_clears_staging_only(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    if (!dsd_resampler_design(&opts.input_resampler, 6, 1)) {
        fprintf(stderr, "FAIL: input resampler design\n");
        return 1;
    }
    int enabled = opts.input_resampler.enabled;
    int L = opts.input_resampler.L;
    int M = opts.input_resampler.M;
    int taps_len = opts.input_resampler.taps_len;
    int taps_per_phase = opts.input_resampler.taps_per_phase;
    int phase = opts.input_resampler.phase;
    int hist_head = opts.input_resampler.hist_head;
    float* taps = opts.input_resampler.taps;
    float* hist = opts.input_resampler.hist;
    opts.input_upsample_prev = 123.0f;
    opts.input_upsample_len = 4;
    opts.input_upsample_pos = 2;
    opts.input_upsample_prev_valid = 1;
    for (size_t i = 0; i < sizeof(opts.input_upsample_buf) / sizeof(opts.input_upsample_buf[0]); i++) {
        opts.input_upsample_buf[i] = (float)(i + 1);
    }

    dsd_opts_reset_input_upsample_state(&opts);

    int rc = 0;
    rc |= expect_int_eq("reset preserves input resampler enabled", opts.input_resampler.enabled, enabled);
    rc |= expect_int_eq("reset preserves input resampler L", opts.input_resampler.L, L);
    rc |= expect_int_eq("reset preserves input resampler M", opts.input_resampler.M, M);
    rc |= expect_int_eq("reset preserves input resampler taps len", opts.input_resampler.taps_len, taps_len);
    rc |= expect_int_eq("reset preserves input resampler taps/phase", opts.input_resampler.taps_per_phase,
                        taps_per_phase);
    rc |= expect_int_eq("reset preserves input resampler phase", opts.input_resampler.phase, phase);
    rc |= expect_int_eq("reset preserves input resampler hist head", opts.input_resampler.hist_head, hist_head);
    rc |= expect_true("reset preserves input resampler taps", opts.input_resampler.taps == taps);
    rc |= expect_true("reset preserves input resampler hist", opts.input_resampler.hist == hist);
    rc |= expect_float_eq("reset clears previous upsample sample", opts.input_upsample_prev, 0.0f);
    rc |= expect_int_eq("reset clears staged upsample length", opts.input_upsample_len, 0);
    rc |= expect_int_eq("reset clears staged upsample position", opts.input_upsample_pos, 0);
    rc |= expect_int_eq("reset clears staged upsample validity", opts.input_upsample_prev_valid, 0);
    for (size_t i = 0; i < sizeof(opts.input_upsample_buf) / sizeof(opts.input_upsample_buf[0]); i++) {
        char label[64];
        snprintf(label, sizeof label, "reset clears staged sample %zu", i);
        rc |= expect_float_eq(label, opts.input_upsample_buf[i], 0.0f);
    }
    dsd_resampler_reset(&opts.input_resampler);
    return rc;
}

static int
test_reset_pcm_input_state_clears_resampler_and_staging(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    if (!dsd_resampler_design(&opts.input_resampler, 6, 1)) {
        fprintf(stderr, "FAIL: input resampler design\n");
        return 1;
    }
    opts.input_upsample_prev = 123.0f;
    opts.input_upsample_len = 4;
    opts.input_upsample_pos = 2;
    opts.input_upsample_prev_valid = 1;
    for (size_t i = 0; i < sizeof(opts.input_upsample_buf) / sizeof(opts.input_upsample_buf[0]); i++) {
        opts.input_upsample_buf[i] = (float)(i + 1);
    }

    dsd_opts_reset_pcm_input_state(&opts);

    int rc = 0;
    rc |= expect_int_eq("reset pcm input clears input resampler enabled", opts.input_resampler.enabled, 0);
    rc |= expect_int_eq("reset pcm input clears input resampler L", opts.input_resampler.L, 1);
    rc |= expect_int_eq("reset pcm input clears input resampler M", opts.input_resampler.M, 1);
    rc |= expect_true("reset pcm input frees input resampler taps", opts.input_resampler.taps == NULL);
    rc |= expect_true("reset pcm input frees input resampler hist", opts.input_resampler.hist == NULL);
    rc |= expect_float_eq("reset pcm input clears previous upsample sample", opts.input_upsample_prev, 0.0f);
    rc |= expect_int_eq("reset pcm input clears staged upsample length", opts.input_upsample_len, 0);
    rc |= expect_int_eq("reset pcm input clears staged upsample position", opts.input_upsample_pos, 0);
    rc |= expect_int_eq("reset pcm input clears staged upsample validity", opts.input_upsample_prev_valid, 0);
    for (size_t i = 0; i < sizeof(opts.input_upsample_buf) / sizeof(opts.input_upsample_buf[0]); i++) {
        char label[64];
        snprintf(label, sizeof label, "reset pcm input clears staged sample %zu", i);
        rc |= expect_float_eq(label, opts.input_upsample_buf[i], 0.0f);
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_input_rate_rescale_for_72000();
    rc |= test_input_rate_rescale_for_44100();
    rc |= test_input_rate_rejects_unsupported_staged_upsample_factor();
    rc |= test_input_rate_preserves_48k_effective_timing_for_24000();
    rc |= test_linear_upsample_block_ends_on_current_sample();
    rc |= test_source_effective_input_rate_classifier();
    rc |= test_current_input_timing_rate_prefers_active_backend();
    rc |= test_headerless_wav_suffix_falls_back_to_raw_pcm();
    rc |= test_rifx_wav_suffix_uses_container_metadata();
    rc |= test_rf64_wav_suffix_uses_container_metadata();
    rc |= test_reset_input_upsample_state_clears_staging_only();
    rc |= test_reset_pcm_input_state_clears_resampler_and_staging();
    return rc;
}
