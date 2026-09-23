// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief Test-only host that scores the analog monitor audio of an I/Q replay (issue #518).
 *
 * iq_decode_check.cmake replays with `-o null`, which leaves only the log to inspect, and the `-6` WAV is taken
 * before the monitor's filters, AGC and squelch gate. This host runs the real engine on the arguments dsd-neo would
 * get. Its lifecycle start hook runs after the engine installed its hooks and opened (no) audio output; it turns the
 * analog monitor's UDP output on and puts a capture in place of the UDP analog hook, so what gets scored is exactly
 * what a listener hears. It also wraps the RTL stream read hook to count the samples the decoder consumes, which is
 * the time base for first-audible and audible durations: muted or squelched blocks never reach the audio hook, so
 * the audio alone cannot say when it started. No product code is involved.
 *
 * The host's own --analog-* options (see analog_usage) are removed before the rest reach dsd_runtime_bootstrap().
 * When live processing ends, the stop hook prints one "ANALOG METRIC:" line and one "ANALOG PROBE:" line per probe
 * frequency, then "ANALOG AUDIO OK" when every requested threshold holds, or an "ANALOG AUDIO FAIL:" line per miss,
 * in which case the host exits 1. Without thresholds it only reports, which is how tools/replay_ab.sh
 * --metric analog uses it.
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
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/io/rtl_stream_fwd.h"

#define ANALOG_MAX_PROBES      8
#define ANALOG_MAX_BLOCK       8192
#define ANALOG_INBAND_LO_HZ    300.0
#define ANALOG_INBAND_HI_HZ    3000.0
#define ANALOG_OUTBAND_LO_HZ   3400.0
#define ANALOG_OUTBAND_HI_HZ   6000.0
#define ANALOG_FULL_SCALE      32768.0
#define ANALOG_POWER_FLOOR     1e-30
#define ANALOG_DB_LIMIT        200.0
#define ANALOG_DEFAULT_AUDIBLE (-50.0)
#define ANALOG_PI              3.14159265358979323846

/* A threshold or setting given on the command line. */
typedef struct {
    int set;
    double value;
} analog_limit;

typedef struct {
    analog_limit expect_tone_hz;
    analog_limit min_snr_db;
    analog_limit min_captured_ms;
    analog_limit min_audible_ms;
    analog_limit max_audible_ms;
    analog_limit min_first_audible_ms;
    analog_limit max_first_audible_ms;
    analog_limit min_inband_db;
    analog_limit max_clip;
    analog_limit audible_dbfs;
} analog_limits;

enum { PROBE_MIN_DBC = 0, PROBE_MAX_DBC, PROBE_MIN_DBFS, PROBE_MAX_DBFS, PROBE_LIMIT_COUNT };

typedef struct {
    double hz;
    analog_limit limit[PROBE_LIMIT_COUNT];
    double power_sum; /* sum over blocks of (probe power x block length), full scale = 1 */
} analog_probe;

typedef struct {
    unsigned int rate_hz;
    uint64_t read_total;          /* samples the RTL stream read hook returned */
    uint64_t samples_captured;    /* samples that reached the audio hook */
    uint64_t samples_audible;     /* ... in blocks at or above the audible level */
    int64_t first_audible_sample; /* stream position where the first audible block began, -1 if none */
    uint64_t clip_count;
    double energy_total; /* sum of squared samples, full scale = 1 */
    double peak;
    double tone_energy;     /* fitted test-tone energy */
    double residual_energy; /* everything else in the blocks the tone was fitted in */
    double inband_energy;
    double outband_energy;
    int no_rate;
} analog_totals;

static analog_limits g_limits;
static analog_probe g_probes[ANALOG_MAX_PROBES];
static int g_probe_count;
static analog_totals g_totals;
static int g_failed;

/* ---- argument handling ---------------------------------------------------------------------------------------- */

typedef struct {
    const char* name;
    analog_limit* target;
    double min_value;
    double max_value;
} analog_scalar_arg;

static const analog_scalar_arg k_scalar_args[] = {
    {"--analog-expect-tone-hz", &g_limits.expect_tone_hz, 1.0, 96000.0},
    {"--analog-min-snr-db", &g_limits.min_snr_db, -ANALOG_DB_LIMIT, ANALOG_DB_LIMIT},
    {"--analog-min-captured-ms", &g_limits.min_captured_ms, 0.0, 1e9},
    {"--analog-min-audible-ms", &g_limits.min_audible_ms, 0.0, 1e9},
    {"--analog-max-audible-ms", &g_limits.max_audible_ms, 0.0, 1e9},
    {"--analog-min-first-audible-ms", &g_limits.min_first_audible_ms, 0.0, 1e9},
    {"--analog-max-first-audible-ms", &g_limits.max_first_audible_ms, 0.0, 1e9},
    {"--analog-min-inband-db", &g_limits.min_inband_db, -ANALOG_DB_LIMIT, ANALOG_DB_LIMIT},
    {"--analog-max-clip", &g_limits.max_clip, 0.0, 1e12},
    {"--analog-audible-dbfs", &g_limits.audible_dbfs, -ANALOG_DB_LIMIT, 0.0},
};

static const char* const k_probe_limit_args[PROBE_LIMIT_COUNT] = {
    "--analog-probe-min-dbc",
    "--analog-probe-max-dbc",
    "--analog-probe-min-dbfs",
    "--analog-probe-max-dbfs",
};

static void
analog_usage(void) {
    DSD_FPRINTF(stderr,
                "analog replay host options (removed before the dsd-neo arguments are parsed):\n"
                "  --analog-expect-tone-hz HZ        score a test tone: level, SNR, and dBc reference for probes\n"
                "  --analog-min-snr-db DB            fitted tone energy over everything else in the audio\n"
                "  --analog-min-captured-ms MS       audio the monitor delivered at all (gate open), total\n"
                "  --analog-min-audible-ms MS        audio at or above the audible level, total\n"
                "  --analog-max-audible-ms MS\n"
                "  --analog-min-first-audible-ms MS  stream time when audible audio first starts\n"
                "  --analog-max-first-audible-ms MS\n"
                "  --analog-min-inband-db DB         300-3000 Hz energy over 3400-6000 Hz energy\n"
                "  --analog-max-clip N               samples at int16 full scale\n"
                "  --analog-audible-dbfs DB          block RMS counted as audible (default -50)\n"
                "  --analog-probe-hz HZ              report the level at HZ (repeatable, up to 8)\n"
                "  --analog-probe-{min,max}-dbc HZ:DB   probe level relative to the expected tone\n"
                "  --analog-probe-{min,max}-dbfs HZ:DB  probe level relative to full scale\n");
}

static int
analog_parse_double(const char* name, const char* text, double min_value, double max_value, double* out) {
    if (dsd_parse_double_strict(text, min_value, max_value, out) != 0) {
        DSD_FPRINTF(stderr, "%s: invalid value '%s' (expected %g to %g)\n", name, text, min_value, max_value);
        return -1;
    }
    return 0;
}

static analog_probe*
analog_probe_for(double hz) {
    for (int i = 0; i < g_probe_count; i++) {
        if (fabs(g_probes[i].hz - hz) < 1e-6) {
            return &g_probes[i];
        }
    }
    if (g_probe_count >= ANALOG_MAX_PROBES) {
        DSD_FPRINTF(stderr, "analog replay: at most %d probe frequencies\n", ANALOG_MAX_PROBES);
        return NULL;
    }
    analog_probe* probe = &g_probes[g_probe_count++];
    DSD_MEMSET(probe, 0, sizeof(*probe));
    probe->hz = hz;
    return probe;
}

/* HZ:DB for a probe threshold. */
static int
analog_parse_probe_limit(const char* name, const char* text, int which) {
    char hz_text[64];
    const char* colon = strchr(text, ':');
    size_t hz_len = colon ? (size_t)(colon - text) : 0U;
    if (colon == NULL || hz_len == 0U || hz_len >= sizeof(hz_text)) {
        DSD_FPRINTF(stderr, "%s: expected HZ:DB, got '%s'\n", name, text);
        return -1;
    }
    DSD_MEMCPY(hz_text, text, hz_len);
    hz_text[hz_len] = '\0';
    double hz = 0.0;
    double db = 0.0;
    if (analog_parse_double(name, hz_text, 1.0, 96000.0, &hz) != 0
        || analog_parse_double(name, colon + 1, -ANALOG_DB_LIMIT, ANALOG_DB_LIMIT, &db) != 0) {
        return -1;
    }
    analog_probe* probe = analog_probe_for(hz);
    if (probe == NULL) {
        return -1;
    }
    probe->limit[which].set = 1;
    probe->limit[which].value = db;
    return 0;
}

/* Applies one host option; returns 0 when it was consumed, 1 when the name is not one of ours, -1 on error. */
static int
analog_apply_option(const char* name, const char* value) {
    for (size_t i = 0; i < sizeof(k_scalar_args) / sizeof(k_scalar_args[0]); i++) {
        if (strcmp(name, k_scalar_args[i].name) == 0) {
            analog_limit* limit = k_scalar_args[i].target;
            limit->set = 1;
            return analog_parse_double(name, value, k_scalar_args[i].min_value, k_scalar_args[i].max_value,
                                       &limit->value);
        }
    }
    for (int i = 0; i < PROBE_LIMIT_COUNT; i++) {
        if (strcmp(name, k_probe_limit_args[i]) == 0) {
            return analog_parse_probe_limit(name, value, i);
        }
    }
    if (strcmp(name, "--analog-probe-hz") == 0) {
        double hz = 0.0;
        if (analog_parse_double(name, value, 1.0, 96000.0, &hz) != 0) {
            return -1;
        }
        return analog_probe_for(hz) != NULL ? 0 : -1;
    }
    return 1;
}

/* Moves every non-host argument to out (argv[0] included) and applies the host options, in either the
 * "--analog-x VALUE" or the "--analog-x=VALUE" spelling. */
static int
analog_split_args(int argc, char** argv, char** out, int* out_count) {
    int kept = 0;
    for (int i = 0; i < argc; i++) {
        if (i == 0 || strncmp(argv[i], "--analog-", 9) != 0) {
            out[kept++] = argv[i];
            continue;
        }
        const char* option = argv[i];
        char name[64];
        const char* eq = strchr(option, '=');
        size_t name_len = eq ? (size_t)(eq - option) : strlen(option);
        if (name_len >= sizeof(name)) {
            DSD_FPRINTF(stderr, "analog replay: unknown option '%s'\n", option);
            analog_usage();
            return -1;
        }
        DSD_MEMCPY(name, option, name_len);
        name[name_len] = '\0';
        const char* value = eq ? eq + 1 : (i + 1 < argc ? argv[++i] : NULL);
        int rc = value ? analog_apply_option(name, value) : 1;
        if (rc != 0) {
            if (rc > 0) {
                DSD_FPRINTF(stderr, "analog replay: unknown option or missing value '%s'\n", option);
                analog_usage();
            }
            return -1;
        }
    }
    out[kept] = NULL;
    *out_count = kept;
    return 0;
}

/* ---- capture -------------------------------------------------------------------------------------------------- */

/* |X(hz)|^2 of a block by Goertzel recursion. */
static double
analog_goertzel_mag2(const double* x, size_t n, double hz, double rate_hz) {
    double coeff = 2.0 * cos(2.0 * ANALOG_PI * hz / rate_hz);
    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double s0 = x[i] + (coeff * s1) - s2;
        s2 = s1;
        s1 = s0;
    }
    double mag2 = (s1 * s1) + (s2 * s2) - (coeff * s1 * s2);
    return mag2 > 0.0 ? mag2 : 0.0;
}

/* Power of a sinusoid at hz (mean square, full scale = 1) from a Hann-windowed block. The window keeps a strong
 * component elsewhere -- speech, or the test tone -- from leaking into the estimate. wsum is the window's sum. */
static double
analog_probe_power(const double* xw, size_t n, double wsum, double hz, double rate_hz) {
    return wsum > 0.0 ? 2.0 * analog_goertzel_mag2(xw, n, hz, rate_hz) / (wsum * wsum) : 0.0;
}

/* Energy of a least-squares fitted sinusoid at hz: the block's projection onto cos and sin at that frequency.
 * What is left over is never negative, so the SNR stays defined however clean the tone is, which subtracting a
 * windowed estimate from the total does not guarantee. */
static double
analog_tone_fit_energy(const double* x, size_t n, double hz, double rate_hz) {
    double step = 2.0 * ANALOG_PI * hz / rate_hz;
    double cc = 0.0;
    double ss = 0.0;
    double cs = 0.0;
    double xc = 0.0;
    double xs = 0.0;
    for (size_t i = 0; i < n; i++) {
        double c = cos(step * (double)i);
        double s = sin(step * (double)i);
        cc += c * c;
        ss += s * s;
        cs += c * s;
        xc += x[i] * c;
        xs += x[i] * s;
    }
    double det = (cc * ss) - (cs * cs);
    if (det <= 1e-9 * cc * ss) {
        return cc > 0.0 ? (xc * xc) / cc : 0.0;
    }
    double a = ((xc * ss) - (xs * cs)) / det;
    double b = ((xs * cc) - (xc * cs)) / det;
    double energy = (a * xc) + (b * xs);
    return energy > 0.0 ? energy : 0.0;
}

/* Hann-windowed energy between lo_hz and hi_hz, summed over DFT bins. Only ratios of it are reported, so the
 * window's constant power loss cancels. */
static double
analog_band_energy(const double* xw, size_t n, double rate_hz, double lo_hz, double hi_hz) {
    double bin_hz = rate_hz / (double)n;
    size_t k_lo = (size_t)ceil(lo_hz / bin_hz);
    size_t k_hi = (size_t)floor(hi_hz / bin_hz);
    double energy = 0.0;
    for (size_t k = k_lo; k <= k_hi && k < n / 2U; k++) {
        energy += analog_goertzel_mag2(xw, n, (double)k * bin_hz, rate_hz);
    }
    return 2.0 * energy / (double)n;
}

/* A frequency the monitor's rate can represent; anything at or above Nyquist would alias and is left unmeasured. */
static int
analog_measurable(double hz) {
    return g_totals.rate_hz > 0U && hz < 0.5 * (double)g_totals.rate_hz;
}

/* Stream position of the next sample the decoder will read: what the read hook returned, less what the symbol
 * cache still holds unread. */
static uint64_t
analog_samples_consumed(void) {
    int pending = dsd_rtl_stream_metrics_hook_symbol_cache_pending();
    uint64_t held = pending > 0 ? (uint64_t)pending : 0U;
    return g_totals.read_total > held ? g_totals.read_total - held : 0U;
}

static double
analog_db(double ratio) {
    double db = 10.0 * log10(ratio > ANALOG_POWER_FLOOR ? ratio : ANALOG_POWER_FLOOR);
    return db > ANALOG_DB_LIMIT ? ANALOG_DB_LIMIT : db;
}

static void
analog_score_levels(const double* x, size_t n, uint64_t block_start) {
    double sum_sq = 0.0;
    for (size_t i = 0; i < n; i++) {
        double mag = fabs(x[i]);
        sum_sq += x[i] * x[i];
        if (mag > g_totals.peak) {
            g_totals.peak = mag;
        }
        if (mag * ANALOG_FULL_SCALE >= 32767.0) {
            g_totals.clip_count++;
        }
    }
    g_totals.energy_total += sum_sq;
    g_totals.samples_captured += n;
    double audible_dbfs = g_limits.audible_dbfs.set ? g_limits.audible_dbfs.value : ANALOG_DEFAULT_AUDIBLE;
    if (analog_db(sum_sq / (double)n) >= audible_dbfs) {
        g_totals.samples_audible += n;
        if (g_totals.first_audible_sample < 0) {
            g_totals.first_audible_sample = (int64_t)block_start;
        }
    }
    if (g_limits.expect_tone_hz.set && analog_measurable(g_limits.expect_tone_hz.value)) {
        double tone = analog_tone_fit_energy(x, n, g_limits.expect_tone_hz.value, (double)g_totals.rate_hz);
        g_totals.tone_energy += tone;
        g_totals.residual_energy += sum_sq > tone ? sum_sq - tone : 0.0;
    }
}

/* One audio block as the monitor delivered it: one AGC gain, so no gain step inside the analysis window. */
static void
analog_score_block(const double* x, size_t n, uint64_t block_start) {
    static double xw[ANALOG_MAX_BLOCK];
    double rate = (double)g_totals.rate_hz;
    double wsum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double w = 0.5 - (0.5 * cos(2.0 * ANALOG_PI * (double)i / (double)n));
        xw[i] = x[i] * w;
        wsum += w;
    }
    analog_score_levels(x, n, block_start);
    for (int i = 0; i < g_probe_count; i++) {
        g_probes[i].power_sum += analog_probe_power(xw, n, wsum, g_probes[i].hz, rate) * (double)n;
    }
    g_totals.inband_energy += analog_band_energy(xw, n, rate, ANALOG_INBAND_LO_HZ, ANALOG_INBAND_HI_HZ);
    g_totals.outband_energy += analog_band_energy(xw, n, rate, ANALOG_OUTBAND_LO_HZ, ANALOG_OUTBAND_HI_HZ);
}

/* Replaces the UDP analog blaster. nbytes is a byte count of int16 mono samples, as dsd_symbol.c passes it. */
static void
analog_capture_blast(const dsd_opts* opts, dsd_state* state, size_t nbytes, const void* data) {
    (void)opts;
    (void)state;
    size_t n = nbytes / sizeof(short);
    if (data == NULL || n == 0U) {
        return;
    }
    unsigned int rate_hz = dsd_rtl_stream_metrics_hook_output_rate_hz();
    if (rate_hz == 0U) {
        g_totals.no_rate = 1;
        return;
    }
    g_totals.rate_hz = rate_hz;
    uint64_t consumed = analog_samples_consumed();
    uint64_t block_start = consumed > (uint64_t)n ? consumed - (uint64_t)n : 0U;
    const short* pcm = (const short*)data;
    static double block[ANALOG_MAX_BLOCK];
    size_t done = 0U;
    while (done < n) {
        size_t chunk = n - done < ANALOG_MAX_BLOCK ? n - done : ANALOG_MAX_BLOCK;
        for (size_t i = 0; i < chunk; i++) {
            block[i] = (double)pcm[done + i] / ANALOG_FULL_SCALE;
        }
        analog_score_block(block, chunk, block_start + done);
        done += chunk;
    }
}

static int
analog_counting_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    int rc = rtl_stream_read((RtlSdrContext*)rtl_ctx, out, count, out_got);
    if (rc >= 0 && out_got != NULL && *out_got > 0) {
        g_totals.read_total += (uint64_t)*out_got;
    }
    return rc;
}

static double
analog_return_pwr(const void* rtl_ctx) {
    return rtl_stream_return_pwr((const RtlSdrContext*)rtl_ctx);
}

/* Runs after the engine installed its hooks and opened audio output, so these replacements stick. */
static int
analog_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)state;
    (void)context;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    dsd_udp_audio_hooks audio = {0};
    audio.blast_analog = analog_capture_blast;
    dsd_udp_audio_hooks_set(audio);
    dsd_rtl_stream_io_hooks io = {0};
    io.read = analog_counting_read;
    io.return_pwr = analog_return_pwr;
    dsd_rtl_stream_io_hooks_set(io);
    return 0;
}

/* ---- report --------------------------------------------------------------------------------------------------- */

static void
analog_fail(const char* what, double got, double limit) {
    DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: %s %.2f (limit %.2f)\n", what, got, limit);
    g_failed = 1;
}

static void
analog_check_min(const char* what, int have, double got, const analog_limit* limit) {
    if (!limit->set) {
        return;
    }
    if (!have) {
        DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: %s not measured (limit %.2f)\n", what, limit->value);
        g_failed = 1;
    } else if (got < limit->value) {
        analog_fail(what, got, limit->value);
    }
}

static void
analog_check_max(const char* what, int have, double got, const analog_limit* limit) {
    if (!limit->set) {
        return;
    }
    if (!have) {
        DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: %s not measured (limit %.2f)\n", what, limit->value);
        g_failed = 1;
    } else if (got > limit->value) {
        analog_fail(what, got, limit->value);
    }
}

static void
analog_print_value(char* line, size_t cap, const char* key, int have, double value) {
    size_t used = strlen(line);
    if (used >= cap) {
        return;
    }
    if (have) {
        DSD_SNPRINTF(line + used, cap - used, " %s=%.2f", key, value);
    } else {
        DSD_SNPRINTF(line + used, cap - used, " %s=NA", key);
    }
}

typedef struct {
    int have_audio;
    int have_tone;
    int have_time;
    double ms_per_sample;
    double tone_ref; /* tone mean-square power, full scale = 1 */
} analog_report_ctx;

/* Prints the ANALOG PROBE lines (check == 0) or applies the probe thresholds (check == 1), so every metric line
 * comes out before the first FAIL line. */
static void
analog_report_probes(const analog_report_ctx* ctx, int check) {
    for (int i = 0; i < g_probe_count; i++) {
        const analog_probe* probe = &g_probes[i];
        double mean = ctx->have_audio ? probe->power_sum / (double)g_totals.samples_captured : 0.0;
        int have_level = ctx->have_audio && analog_measurable(probe->hz);
        int have_dbc = have_level && ctx->have_tone;
        double dbfs = analog_db(mean);
        double dbc = analog_db(mean / (ctx->tone_ref > ANALOG_POWER_FLOOR ? ctx->tone_ref : ANALOG_POWER_FLOOR));
        if (!check) {
            char line[160];
            DSD_SNPRINTF(line, sizeof(line), "ANALOG PROBE: hz=%.1f", probe->hz);
            analog_print_value(line, sizeof(line), "dbfs", have_level, dbfs);
            analog_print_value(line, sizeof(line), "dbc", have_dbc, dbc);
            DSD_FPRINTF(stderr, "%s\n", line);
            continue;
        }
        char what[64];
        DSD_SNPRINTF(what, sizeof(what), "probe %.1f Hz dBc", probe->hz);
        analog_check_min(what, have_dbc, dbc, &probe->limit[PROBE_MIN_DBC]);
        analog_check_max(what, have_dbc, dbc, &probe->limit[PROBE_MAX_DBC]);
        DSD_SNPRINTF(what, sizeof(what), "probe %.1f Hz dBFS", probe->hz);
        analog_check_min(what, have_level, dbfs, &probe->limit[PROBE_MIN_DBFS]);
        analog_check_max(what, have_level, dbfs, &probe->limit[PROBE_MAX_DBFS]);
    }
}

static void
analog_report(void) {
    analog_report_ctx ctx;
    ctx.have_audio = g_totals.samples_captured > 0U && g_totals.rate_hz > 0U;
    ctx.have_time = g_totals.rate_hz > 0U && g_totals.read_total > 0U;
    ctx.ms_per_sample = g_totals.rate_hz > 0U ? 1000.0 / (double)g_totals.rate_hz : 0.0;
    double captured = (double)g_totals.samples_captured;
    ctx.tone_ref = ctx.have_audio ? g_totals.tone_energy / captured : 0.0;
    ctx.have_tone = ctx.have_audio && g_limits.expect_tone_hz.set && analog_measurable(g_limits.expect_tone_hz.value);
    int have_tone = ctx.have_tone;
    int have_first = ctx.have_time && g_totals.first_audible_sample >= 0;
    double audible_ms = (double)g_totals.samples_audible * ctx.ms_per_sample;
    double first_ms = (double)g_totals.first_audible_sample * ctx.ms_per_sample;
    double rest = g_totals.residual_energy > ANALOG_POWER_FLOOR ? g_totals.residual_energy : ANALOG_POWER_FLOOR;
    double snr_db = analog_db(g_totals.tone_energy / rest);
    double outband = g_totals.outband_energy > ANALOG_POWER_FLOOR ? g_totals.outband_energy : ANALOG_POWER_FLOOR;
    double inband_db = analog_db(g_totals.inband_energy / outband);

    char line[512];
    DSD_SNPRINTF(line, sizeof(line), "ANALOG METRIC: rate_hz=%u", g_totals.rate_hz);
    analog_print_value(line, sizeof(line), "total_ms", ctx.have_time,
                       (double)analog_samples_consumed() * ctx.ms_per_sample);
    analog_print_value(line, sizeof(line), "captured_ms", 1, captured * ctx.ms_per_sample);
    analog_print_value(line, sizeof(line), "audible_ms", 1, audible_ms);
    analog_print_value(line, sizeof(line), "first_audible_ms", have_first, first_ms);
    analog_print_value(line, sizeof(line), "rms_dbfs", ctx.have_audio, analog_db(g_totals.energy_total / captured));
    analog_print_value(line, sizeof(line), "peak_dbfs", ctx.have_audio, analog_db(g_totals.peak * g_totals.peak));
    size_t used = strlen(line);
    DSD_SNPRINTF(line + used, sizeof(line) - used, " clip=%llu", (unsigned long long)g_totals.clip_count);
    analog_print_value(line, sizeof(line), "inband_db", ctx.have_audio, inband_db);
    analog_print_value(line, sizeof(line), "tone_hz", g_limits.expect_tone_hz.set, g_limits.expect_tone_hz.value);
    analog_print_value(line, sizeof(line), "tone_dbfs", have_tone, analog_db(ctx.tone_ref));
    analog_print_value(line, sizeof(line), "tone_snr_db", have_tone, snr_db);
    DSD_FPRINTF(stderr, "%s\n", line);
    analog_report_probes(&ctx, 0);

    if (g_totals.no_rate) {
        DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: audio arrived without an RTL output rate; the host scores RTL and "
                            "I/Q replay input only\n");
        g_failed = 1;
    }
    analog_check_min("tone SNR dB", have_tone, snr_db, &g_limits.min_snr_db);
    analog_check_min("captured ms", 1, captured * ctx.ms_per_sample, &g_limits.min_captured_ms);
    analog_check_min("audible ms", 1, audible_ms, &g_limits.min_audible_ms);
    analog_check_max("audible ms", 1, audible_ms, &g_limits.max_audible_ms);
    analog_check_min("first audible ms", have_first, first_ms, &g_limits.min_first_audible_ms);
    analog_check_max("first audible ms", have_first, first_ms, &g_limits.max_first_audible_ms);
    analog_check_min("in-band ratio dB", ctx.have_audio, inband_db, &g_limits.min_inband_db);
    analog_check_max("clipped samples", 1, (double)g_totals.clip_count, &g_limits.max_clip);
    analog_report_probes(&ctx, 1);
    if (!g_failed) {
        DSD_FPRINTF(stderr, "ANALOG AUDIO OK\n");
    }
}

static void
analog_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    analog_report();
}

int
main(int argc, char** argv) {
    char** args = (char**)calloc((size_t)argc + 1U, sizeof(*args));
    if (args == NULL) {
        return 1;
    }
    g_totals.first_audible_sample = -1;
    int kept = 0;
    if (analog_split_args(argc, argv, args, &kept) != 0) {
        free((void*)args);
        return 2;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    int rc = 1;
    if (opts != NULL && state != NULL) {
        initOpts(opts);
        initState(state);
        dsd_engine_lifecycle_hooks hooks = {analog_start, analog_stop, NULL};
        if (dsd_runtime_bootstrap(kept, args, opts, state, NULL, &rc) == DSD_BOOTSTRAP_CONTINUE) {
            rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
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
