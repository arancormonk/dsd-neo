// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief Test-only host that scores the analog monitor audio of an I/Q replay (issue #518).
 *
 * iq_decode_check.cmake replays with `-o null`, which leaves only the log to inspect, and the `-6` WAV is taken
 * before the monitor's filters, gain stage and squelch gate. This host runs the real engine on the arguments
 * dsd-neo would get. Its lifecycle start hook runs after the engine installed its hooks and opened (no) audio
 * output; it turns the analog monitor's UDP output on and puts a capture in place of the UDP analog hook, so what
 * gets scored is exactly what a listener hears. It also wraps the RTL stream read hook to count what the decoder
 * consumes, which is the time base for first-audible and audible durations: muted or squelched blocks never reach
 * the audio hook, so the audio alone cannot say when it started. No product code is involved.
 *
 * The host's own --analog-* options (see analog_usage) are removed before the rest reach dsd_runtime_bootstrap();
 * any other --analog-* argument is passed through to the CLI parser unchanged. When live processing ends, the stop
 * hook prints one "ANALOG METRIC:" line and one "ANALOG PROBE:" line per probe frequency, then "ANALOG AUDIO OK"
 * when every requested threshold holds, or an "ANALOG AUDIO FAIL:" line per miss, in which case the host exits 1.
 * Without thresholds it only reports, which is how tools/replay_ab.sh --metric analog uses it.
 *
 * Stream time is kept in milliseconds, each read converted at the RTL output rate in force when it was made, so it
 * stays right when the front end changes rate mid-run and is reported even when no audio reached the hook at all.
 * A case that expects silence therefore pairs --analog-max-audible-ms 0 with --analog-min-total-ms, which proves
 * the replay ran rather than stalled.
 *
 * The METRIC line ends with the received-tone fields tools/replay_ab.sh reads into its tone and tone_lock_ms
 * columns. They read NA until a tone detector publishes a received tone; the CTCSS detector (#522) fills them in
 * for CTCSS and the DCS detector (#523) for DCS. The contract, which replay_ab.sh relies on because it splits the
 * line on spaces:
 *   tone=<label>       the received tone or code as the detector names it, with no whitespace: "151.4" (Hz, one
 *                      decimal) for CTCSS, "D023N" or "D023I" for DCS; NA when none was confirmed.
 *   tone_lock_ms=<ms>  stream time of the first confirmed lock, on the same clock as first_audible_ms (see above),
 *                      with two decimals; NA when nothing locked.
 * When the label changes during a run, tone= is the last one confirmed and tone_lock_ms the first lock of any.
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
#define ANALOG_HZ_EPSILON      1e-6
/* A Hann window's main lobe spans +/-2 DFT bins; nearer frequencies read each other's energy. */
#define ANALOG_HANN_LOBE_BINS  2.0

/* A threshold or setting given on the command line. */
typedef struct {
    int set;
    double value;
} analog_limit;

typedef struct {
    analog_limit expect_tone_hz;
    analog_limit min_snr_db;
    analog_limit min_total_ms;
    analog_limit max_total_ms;
    analog_limit min_captured_ms;
    analog_limit min_audible_ms;
    analog_limit max_audible_ms;
    analog_limit min_first_audible_ms;
    analog_limit max_first_audible_ms;
    analog_limit min_inband_db;
    analog_limit min_rms_dbfs;
    analog_limit max_rms_dbfs;
    analog_limit max_peak_dbfs;
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
    unsigned int rate_hz;       /* RTL output rate at the latest read or audio block */
    unsigned int first_rate_hz; /* ... at the first one */
    unsigned int rate_changes;  /* times the rate differed from the one before */
    double read_ms;             /* stream time the RTL stream read hook returned */
    uint64_t unclocked_reads;   /* samples read while no output rate was known */
    uint64_t cqpsk_reads;       /* samples read while the front end ran its CQPSK symbol path */
    uint64_t samples_captured;  /* samples that reached the audio hook */
    double captured_ms;
    double audible_ms;       /* ... in blocks at or above the audible level */
    double first_audible_ms; /* stream time where the first audible block began, -1 if none */
    double widest_bin_hz;    /* coarsest DFT bin of any scored block: rate / block length */
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
    {"--analog-min-total-ms", &g_limits.min_total_ms, 0.0, 1e9},
    {"--analog-max-total-ms", &g_limits.max_total_ms, 0.0, 1e9},
    {"--analog-min-captured-ms", &g_limits.min_captured_ms, 0.0, 1e9},
    {"--analog-min-audible-ms", &g_limits.min_audible_ms, 0.0, 1e9},
    {"--analog-max-audible-ms", &g_limits.max_audible_ms, 0.0, 1e9},
    {"--analog-min-first-audible-ms", &g_limits.min_first_audible_ms, 0.0, 1e9},
    {"--analog-max-first-audible-ms", &g_limits.max_first_audible_ms, 0.0, 1e9},
    {"--analog-min-inband-db", &g_limits.min_inband_db, -ANALOG_DB_LIMIT, ANALOG_DB_LIMIT},
    {"--analog-min-rms-dbfs", &g_limits.min_rms_dbfs, -ANALOG_DB_LIMIT, 0.0},
    {"--analog-max-rms-dbfs", &g_limits.max_rms_dbfs, -ANALOG_DB_LIMIT, 0.0},
    {"--analog-max-peak-dbfs", &g_limits.max_peak_dbfs, -ANALOG_DB_LIMIT, 0.0},
    {"--analog-max-clip", &g_limits.max_clip, 0.0, 1e12},
    {"--analog-audible-dbfs", &g_limits.audible_dbfs, -ANALOG_DB_LIMIT, 0.0},
};

static const char* const k_probe_limit_args[PROBE_LIMIT_COUNT] = {
    "--analog-probe-min-dbc",
    "--analog-probe-max-dbc",
    "--analog-probe-min-dbfs",
    "--analog-probe-max-dbfs",
};

static const char k_probe_hz_arg[] = "--analog-probe-hz";

static void
analog_usage(void) {
    DSD_FPRINTF(stderr,
                "analog replay host options (removed before the dsd-neo arguments are parsed):\n"
                "  --analog-expect-tone-hz HZ        score a test tone: level, SNR, and dBc reference for probes\n"
                "  --analog-min-snr-db DB            fitted tone energy over everything else in the audio\n"
                "  --analog-min-total-ms MS          stream time the decoder consumed, audio or not\n"
                "  --analog-max-total-ms MS\n"
                "  --analog-min-captured-ms MS       audio the monitor delivered at all (gate open), total\n"
                "  --analog-min-audible-ms MS        audio at or above the audible level, total\n"
                "  --analog-max-audible-ms MS\n"
                "  --analog-min-first-audible-ms MS  stream time when audible audio first starts\n"
                "  --analog-max-first-audible-ms MS\n"
                "  --analog-min-inband-db DB         300-3000 Hz energy over 3400-6000 Hz energy\n"
                "  --analog-min-rms-dbfs DB          RMS level of the delivered audio\n"
                "  --analog-max-rms-dbfs DB\n"
                "  --analog-max-peak-dbfs DB         largest sample of the delivered audio\n"
                "  --analog-max-clip N               samples at int16 full scale\n"
                "  --analog-audible-dbfs DB          block RMS counted as audible (default -50)\n"
                "  --analog-probe-hz HZ              report the level at HZ (repeatable, up to 8)\n"
                "  --analog-probe-{min,max}-dbc HZ:DB   probe level relative to the expected tone\n"
                "  --analog-probe-{min,max}-dbfs HZ:DB  probe level relative to full scale\n"
                "Other --analog-* arguments go to dsd-neo unchanged.\n");
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
        if (fabs(g_probes[i].hz - hz) < ANALOG_HZ_EPSILON) {
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

static const analog_scalar_arg*
analog_find_scalar(const char* name) {
    for (size_t i = 0; i < sizeof(k_scalar_args) / sizeof(k_scalar_args[0]); i++) {
        if (strcmp(name, k_scalar_args[i].name) == 0) {
            return &k_scalar_args[i];
        }
    }
    return NULL;
}

/* Index into k_probe_limit_args, or -1. */
static int
analog_find_probe_limit(const char* name) {
    for (int i = 0; i < PROBE_LIMIT_COUNT; i++) {
        if (strcmp(name, k_probe_limit_args[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int
analog_is_host_option(const char* name) {
    return analog_find_scalar(name) != NULL || analog_find_probe_limit(name) >= 0 || strcmp(name, k_probe_hz_arg) == 0;
}

/* Applies one host option (name already known to be one); returns 0, or -1 on a bad value. */
static int
analog_apply_option(const char* name, const char* value) {
    const analog_scalar_arg* scalar = analog_find_scalar(name);
    if (scalar != NULL) {
        scalar->target->set = 1;
        return analog_parse_double(name, value, scalar->min_value, scalar->max_value, &scalar->target->value);
    }
    int which = analog_find_probe_limit(name);
    if (which >= 0) {
        return analog_parse_probe_limit(name, value, which);
    }
    double hz = 0.0;
    if (analog_parse_double(name, value, 1.0, 96000.0, &hz) != 0) {
        return -1;
    }
    return analog_probe_for(hz) != NULL ? 0 : -1;
}

/* Copies the option name of a host option ("--analog-x" or the part of "--analog-x=VALUE" before '=') into name
 * and returns 1; returns 0 for any argument that is not one of the host's options. */
static int
analog_host_option_name(const char* arg, char* name, size_t cap, const char** eq_out) {
    if (strncmp(arg, "--analog-", 9) != 0) {
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
    return analog_is_host_option(name);
}

/* Moves every non-host argument to out (argv[0] included) and applies the host options, in either the
 * "--analog-x VALUE" or the "--analog-x=VALUE" spelling. */
static int
analog_split_args(int argc, char** argv, char** out, int* out_count) {
    int kept = 0;
    for (int i = 0; i < argc; i++) {
        char name[64];
        const char* eq = NULL;
        if (i == 0 || !analog_host_option_name(argv[i], name, sizeof(name), &eq)) {
            out[kept++] = argv[i];
            continue;
        }
        const char* value = NULL;
        if (eq != NULL) {
            value = eq + 1;
        } else if (i + 1 < argc) {
            value = argv[++i];
        }
        if (value == NULL) {
            DSD_FPRINTF(stderr, "analog replay: %s needs a value\n", name);
            analog_usage();
            return -1;
        }
        if (analog_apply_option(name, value) != 0) {
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

/* Records the RTL output rate in force now. */
static void
analog_note_rate(unsigned int rate_hz) {
    if (rate_hz == 0U || rate_hz == g_totals.rate_hz) {
        return;
    }
    if (g_totals.rate_hz == 0U) {
        /* Anything read before the front end reported a rate was read at this one. */
        g_totals.first_rate_hz = rate_hz;
        g_totals.read_ms += (double)g_totals.unclocked_reads * 1000.0 / (double)rate_hz;
        g_totals.unclocked_reads = 0U;
    } else {
        g_totals.rate_changes++;
    }
    g_totals.rate_hz = rate_hz;
}

/* Stream time of the next sample the decoder will read: what the read hook returned, less what the symbol cache
 * still holds unread (at the current rate, the rate those samples were read at). */
static double
analog_consumed_ms(void) {
    int pending = dsd_rtl_stream_metrics_hook_symbol_cache_pending();
    double held_ms = pending > 0 && g_totals.rate_hz > 0U ? (double)pending * 1000.0 / (double)g_totals.rate_hz : 0.0;
    return g_totals.read_ms > held_ms ? g_totals.read_ms - held_ms : 0.0;
}

static double
analog_db(double ratio) {
    double db = 10.0 * log10(ratio > ANALOG_POWER_FLOOR ? ratio : ANALOG_POWER_FLOOR);
    return db > ANALOG_DB_LIMIT ? ANALOG_DB_LIMIT : db;
}

static void
analog_score_levels(const double* x, size_t n, double block_start_ms, double rate) {
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
    double block_ms = (double)n * 1000.0 / rate;
    g_totals.energy_total += sum_sq;
    g_totals.samples_captured += n;
    g_totals.captured_ms += block_ms;
    double audible_dbfs = g_limits.audible_dbfs.set ? g_limits.audible_dbfs.value : ANALOG_DEFAULT_AUDIBLE;
    if (analog_db(sum_sq / (double)n) >= audible_dbfs) {
        g_totals.audible_ms += block_ms;
        if (g_totals.first_audible_ms < 0.0) {
            g_totals.first_audible_ms = block_start_ms;
        }
    }
    if (g_limits.expect_tone_hz.set && analog_measurable(g_limits.expect_tone_hz.value)) {
        double tone = analog_tone_fit_energy(x, n, g_limits.expect_tone_hz.value, rate);
        g_totals.tone_energy += tone;
        g_totals.residual_energy += sum_sq > tone ? sum_sq - tone : 0.0;
    }
}

/* One audio block as the monitor delivered it. The fixed gain (any -n above 0) is the same for every block, and
 * the per-block AGC (-n 0) sets one gain per block, so either way no gain step falls inside the analysis window. */
static void
analog_score_block(const double* x, size_t n, double block_start_ms) {
    static double xw[ANALOG_MAX_BLOCK];
    double rate = (double)g_totals.rate_hz;
    double wsum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double w = 0.5 - (0.5 * cos(2.0 * ANALOG_PI * (double)i / (double)n));
        xw[i] = x[i] * w;
        wsum += w;
    }
    if (rate / (double)n > g_totals.widest_bin_hz) {
        g_totals.widest_bin_hz = rate / (double)n;
    }
    analog_score_levels(x, n, block_start_ms, rate);
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
    analog_note_rate(rate_hz);
    double ms_per_sample = 1000.0 / (double)rate_hz;
    double consumed_ms = analog_consumed_ms();
    double block_ms = (double)n * ms_per_sample;
    double block_start_ms = consumed_ms > block_ms ? consumed_ms - block_ms : 0.0;
    const short* pcm = (const short*)data;
    static double block[ANALOG_MAX_BLOCK];
    size_t done = 0U;
    while (done < n) {
        size_t chunk = n - done < ANALOG_MAX_BLOCK ? n - done : ANALOG_MAX_BLOCK;
        for (size_t i = 0; i < chunk; i++) {
            block[i] = (double)pcm[done + i] / ANALOG_FULL_SCALE;
        }
        analog_score_block(block, chunk, block_start_ms + ((double)done * ms_per_sample));
        done += chunk;
    }
}

static int
analog_counting_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    int rc = rtl_stream_read((RtlSdrContext*)rtl_ctx, out, count, out_got);
    if (rc >= 0 && out_got != NULL && *out_got > 0) {
        analog_note_rate(dsd_rtl_stream_metrics_hook_output_rate_hz());
        if (g_totals.rate_hz > 0U) {
            g_totals.read_ms += (double)*out_got * 1000.0 / (double)g_totals.rate_hz;
        } else {
            g_totals.unclocked_reads += (uint64_t)*out_got;
        }
        int cqpsk = 0;
        int timing = 0;
        if (dsd_rtl_stream_metrics_hook_cqpsk_status(&cqpsk, &timing) == 0 && cqpsk) {
            g_totals.cqpsk_reads += (uint64_t)*out_got;
        }
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
    int have_first;
    double total_ms;
    double rms_dbfs;
    double peak_dbfs;
    double inband_db;
    double snr_db;
    double tone_ref; /* tone mean-square power, full scale = 1 */
} analog_report_ctx;

static void
analog_report_measure(analog_report_ctx* ctx) {
    double captured = (double)g_totals.samples_captured;
    ctx->have_audio = g_totals.samples_captured > 0U && g_totals.rate_hz > 0U;
    ctx->have_time = g_totals.read_ms > 0.0;
    ctx->have_first = g_totals.first_audible_ms >= 0.0;
    ctx->total_ms = analog_consumed_ms();
    ctx->tone_ref = ctx->have_audio ? g_totals.tone_energy / captured : 0.0;
    ctx->have_tone = ctx->have_audio && g_limits.expect_tone_hz.set && analog_measurable(g_limits.expect_tone_hz.value);
    ctx->rms_dbfs = ctx->have_audio ? analog_db(g_totals.energy_total / captured) : 0.0;
    ctx->peak_dbfs = analog_db(g_totals.peak * g_totals.peak);
    double rest = g_totals.residual_energy > ANALOG_POWER_FLOOR ? g_totals.residual_energy : ANALOG_POWER_FLOOR;
    ctx->snr_db = analog_db(g_totals.tone_energy / rest);
    double outband = g_totals.outband_energy > ANALOG_POWER_FLOOR ? g_totals.outband_energy : ANALOG_POWER_FLOOR;
    ctx->inband_db = analog_db(g_totals.inband_energy / outband);
}

static void
analog_print_metric_line(const analog_report_ctx* ctx) {
    char line[512];
    DSD_SNPRINTF(line, sizeof(line), "ANALOG METRIC: rate_hz=%u", g_totals.rate_hz);
    analog_print_value(line, sizeof(line), "total_ms", ctx->have_time, ctx->total_ms);
    analog_print_value(line, sizeof(line), "captured_ms", 1, g_totals.captured_ms);
    analog_print_value(line, sizeof(line), "audible_ms", 1, g_totals.audible_ms);
    analog_print_value(line, sizeof(line), "first_audible_ms", ctx->have_first, g_totals.first_audible_ms);
    analog_print_value(line, sizeof(line), "rms_dbfs", ctx->have_audio, ctx->rms_dbfs);
    analog_print_value(line, sizeof(line), "peak_dbfs", ctx->have_audio, ctx->peak_dbfs);
    size_t used = strlen(line);
    DSD_SNPRINTF(line + used, sizeof(line) - used, " clip=%llu", (unsigned long long)g_totals.clip_count);
    analog_print_value(line, sizeof(line), "inband_db", ctx->have_audio, ctx->inband_db);
    analog_print_value(line, sizeof(line), "tone_hz", g_limits.expect_tone_hz.set, g_limits.expect_tone_hz.value);
    analog_print_value(line, sizeof(line), "tone_dbfs", ctx->have_tone, analog_db(ctx->tone_ref));
    analog_print_value(line, sizeof(line), "tone_snr_db", ctx->have_tone, ctx->snr_db);
    /* Received-tone fields (see the file comment): no detector publishes one yet. */
    used = strlen(line);
    DSD_SNPRINTF(line + used, sizeof(line) - used, " tone=NA tone_lock_ms=NA");
    DSD_FPRINTF(stderr, "%s\n", line);
}

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

/* Warns when two measured frequencies sit inside one block's Hann main lobe: each then reads the other's energy
 * (a probe beside a strong tone measures the tone's leakage, not the probe frequency). Identical frequencies are
 * a deliberate cross-check of the probe against the tone fit and pass silently. */
static void
analog_note_close(double a_hz, double b_hz, double lobe_hz) {
    double apart = fabs(a_hz - b_hz);
    if (apart >= ANALOG_HZ_EPSILON && apart < lobe_hz) {
        DSD_FPRINTF(stderr,
                    "analog replay: warning: %.1f and %.1f Hz are %.1f Hz apart, inside one block's +/-%.0f Hz "
                    "resolution; each level includes the other's energy\n",
                    a_hz, b_hz, apart, lobe_hz);
    }
}

static void
analog_note_resolution(void) {
    double lobe_hz = ANALOG_HANN_LOBE_BINS * g_totals.widest_bin_hz;
    /* The expected tone and every probe frequency, each once (probes are unique already; one may sit on the tone). */
    double hz[ANALOG_MAX_PROBES + 1];
    int count = 0;
    int have_tone = g_limits.expect_tone_hz.set;
    if (have_tone) {
        hz[count++] = g_limits.expect_tone_hz.value;
    }
    for (int i = 0; i < g_probe_count; i++) {
        if (!have_tone || fabs(g_probes[i].hz - g_limits.expect_tone_hz.value) >= ANALOG_HZ_EPSILON) {
            hz[count++] = g_probes[i].hz;
        }
    }
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            analog_note_close(hz[i], hz[j], lobe_hz);
        }
    }
    if (g_totals.cqpsk_reads > 0U) {
        /* Those reads are symbols, not samples at the output rate, and how many arrive is not tied to the stream
         * length, so total_ms can come out far too low or far too high. */
        DSD_FPRINTF(stderr,
                    "analog replay: warning: the RTL front end delivered %llu CQPSK symbols instead of monitor "
                    "samples; total_ms counts them as samples, so it does not measure stream time\n",
                    (unsigned long long)g_totals.cqpsk_reads);
    }
    if (g_totals.rate_changes > 0U) {
        DSD_FPRINTF(stderr,
                    "analog replay: note: the RTL output rate changed %u time(s) during the run (%u Hz at "
                    "the start, %u Hz at the end)\n",
                    g_totals.rate_changes, g_totals.first_rate_hz, g_totals.rate_hz);
    }
}

static void
analog_check_limits(const analog_report_ctx* ctx) {
    if (g_totals.no_rate || g_totals.unclocked_reads > 0U) {
        DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: input arrived without an RTL output rate; the host scores RTL and "
                            "I/Q replay input only\n");
        g_failed = 1;
    }
    analog_check_min("tone SNR dB", ctx->have_tone, ctx->snr_db, &g_limits.min_snr_db);
    analog_check_min("total ms", ctx->have_time, ctx->total_ms, &g_limits.min_total_ms);
    analog_check_max("total ms", ctx->have_time, ctx->total_ms, &g_limits.max_total_ms);
    analog_check_min("captured ms", 1, g_totals.captured_ms, &g_limits.min_captured_ms);
    analog_check_min("audible ms", 1, g_totals.audible_ms, &g_limits.min_audible_ms);
    analog_check_max("audible ms", 1, g_totals.audible_ms, &g_limits.max_audible_ms);
    analog_check_min("first audible ms", ctx->have_first, g_totals.first_audible_ms, &g_limits.min_first_audible_ms);
    analog_check_max("first audible ms", ctx->have_first, g_totals.first_audible_ms, &g_limits.max_first_audible_ms);
    analog_check_min("in-band ratio dB", ctx->have_audio, ctx->inband_db, &g_limits.min_inband_db);
    analog_check_min("RMS dBFS", ctx->have_audio, ctx->rms_dbfs, &g_limits.min_rms_dbfs);
    analog_check_max("RMS dBFS", ctx->have_audio, ctx->rms_dbfs, &g_limits.max_rms_dbfs);
    analog_check_max("peak dBFS", ctx->have_audio, ctx->peak_dbfs, &g_limits.max_peak_dbfs);
    analog_check_max("clipped samples", 1, (double)g_totals.clip_count, &g_limits.max_clip);
    analog_report_probes(ctx, 1);
}

static void
analog_report(void) {
    analog_report_ctx ctx;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    analog_report_measure(&ctx);
    analog_print_metric_line(&ctx);
    analog_report_probes(&ctx, 0);
    analog_note_resolution();
    analog_check_limits(&ctx);
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
    g_totals.first_audible_ms = -1.0;
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
