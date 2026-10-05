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
 * The METRIC line ends with the received-tone fields tools/replay_ab.sh reads into its tone, tone_lock_ms and
 * tone_lock_pct columns. They come from the decoder's received-tone publication (dsd_state::analog_rx), read after
 * every block the monitor delivers: the tap that detects tones runs on the same block just before the block reaches
 * the audio hook, and a tone can only lock while the monitor's gate is open. The CTCSS detector (#522) and the DCS
 * detector (#523) fill them. This file owns the field names and the contract below, and replay_ab.sh and the report
 * own the columns; a detector that wants more adds its own field and column. The contract, which replay_ab.sh relies on
 * because it splits the line on spaces:
 *   tone=<label>        the received tone or code as the decoder names it, with no whitespace: "151.4" (Hz, one
 *                       decimal) for CTCSS; for DCS both standard spellings of the code's signal, canonical first
 *                       (dsd_dcs_canonical(), then dsd_dcs_alias()), joined by a slash, such as "D023N/D047I": the
 *                       log's "DCS D023N / D047I" as one token. A receiver cannot tell the two spellings apart, so
 *                       neither alone names what was received. NA when none was confirmed.
 *   tone_lock_ms=<ms>   stream time of the first confirmed lock, on the same clock as first_audible_ms (see above):
 *                       the end of the block after which the publication first read locked, with two decimals; NA
 *                       when nothing locked.
 *   tone_lock_pct=<pct> share of the delivered audio (captured_ms) in blocks after which the publication read
 *                       locked, 0 to 100 with two decimals: 0.00 when nothing locked, NA when no audio came out.
 * When the label changes during a run, tone= is the last one confirmed and tone_lock_ms the first lock of any.
 * --analog-max-tone-lock-ms bounds tone_lock_ms; like every bound, it fails as not measured when nothing locked.
 *
 * --analog-scan-row ROW (issue #526) enters row ROW of the -C channel map before the engine runs, the way the
 * conventional scanner commits a row: its declared class, then its own options (an nfm row's --nfm-bandwidth-hz and
 * --squelch-db included), so the replay opens the front end with the row's receive family and width. I/Q replay
 * cannot retune, so one row is all a run can visit; the host prints "Scan row applied: ..." before the replay.
 *
 * --analog-iq-gain-db DB replays a copy of the --iq-replay capture with every sample scaled by DB about the cu8
 * midpoint: the same air, noise included, as a receiver with that much more (or less) gain would have recorded it.
 * It works on the committed fixtures (tests/fixtures/iq) only, which it opens by their directory entries there. The
 * auto squelch's cases replay one burst at two levels this way and hold both to the same bounds, with no second
 * fixture committed. The copy and its sidecar go to a private temporary directory the host removes when it exits; it
 * refuses a gain that would clip more than one byte in a thousand, which would no longer be the same signal.
 *
 * --analog-pcm-tap RATE (issue #628) replays the capture as audio input instead: the host FM-demodulates the committed
 * capture as a scanner's discriminator tap, or an SDR program with its audio filtering off, would (a 16 kHz channel
 * filter and a polar discriminator, nothing after them), resamples it to RATE Hz (a divisor of the capture's rate),
 * writes it as a 16-bit mono WAV to the same private directory and runs `-i TAP.wav` in place of `--iq-replay`.
 * --analog-pcm-gain-db scales that audio (a louder source) and --analog-pcm-lowpass-hz low-passes it first (an SDR
 * program's audio filter). Stream time on audio input is the WAV's read position, in frames at its own rate, so the
 * bounds mean the same as on a replay; it needs no radio support, and the PCM cases run in every build.
 */

#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../test_support/test_support.h"
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
    analog_limit max_tone_lock_ms;
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

/* Room for the tone= label (see the file comment): a CTCSS value ("151.4") or a DCS code's two spellings
 * ("D023N/D047I"), the library's "DCS D023N / D047I" label less its prefix and spaces. Sized by both library label
 * sizes, so a longer label format grows the field instead of cutting the tone= token short. */
enum {
    ANALOG_TONE_LABEL_SIZE = (int)DSD_CTCSS_LABEL_SIZE > (int)DSD_DCS_LABEL_SIZE ? (int)DSD_CTCSS_LABEL_SIZE
                                                                                 : (int)DSD_DCS_LABEL_SIZE
};

_Static_assert(sizeof("D023N/D047I") <= (size_t)ANALOG_TONE_LABEL_SIZE,
               "a DCS code's two spellings fit the tone= label");

/* The received tone as the decoder published it (see the file comment). */
typedef struct {
    char label[ANALOG_TONE_LABEL_SIZE]; /* last confirmed tone or code, "" = none */
    double first_lock_ms;               /* stream time of the first lock, -1 if none */
    double locked_ms;                   /* delivered audio in blocks after which the publication read locked */
} analog_tone_track;

static analog_limits g_limits;
static analog_probe g_probes[ANALOG_MAX_PROBES];
static int g_probe_count;
static analog_totals g_totals;
static analog_tone_track g_tone;
static int g_failed;
/* The -C map row to enter before the replay (--analog-scan-row); unset runs no scan row. */
static analog_limit g_scan_row;
/* The gain the replayed copy of the capture is scaled by (--analog-iq-gain-db); unset replays the capture itself. */
static analog_limit g_iq_gain_db;
/* Audio input made from the capture (--analog-pcm-tap): its rate, gain and low-pass; unset replays the capture. */
static analog_limit g_pcm_tap_hz;
static analog_limit g_pcm_gain_db;
static analog_limit g_pcm_lowpass_hz;

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
    {"--analog-max-tone-lock-ms", &g_limits.max_tone_lock_ms, 0.0, 1e9},
    {"--analog-scan-row", &g_scan_row, 0.0, 65535.0},
    {"--analog-iq-gain-db", &g_iq_gain_db, -40.0, 40.0},
    {"--analog-pcm-tap", &g_pcm_tap_hz, 8000.0, 96000.0},
    {"--analog-pcm-gain-db", &g_pcm_gain_db, -40.0, 20.0},
    {"--analog-pcm-lowpass-hz", &g_pcm_lowpass_hz, 1000.0, 20000.0},
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
                "  --analog-max-tone-lock-ms MS      stream time when the received tone first locks\n"
                "  --analog-probe-hz HZ              report the level at HZ (repeatable, up to 8)\n"
                "  --analog-probe-{min,max}-dbc HZ:DB   probe level relative to the expected tone\n"
                "  --analog-probe-{min,max}-dbfs HZ:DB  probe level relative to full scale\n"
                "  --analog-scan-row ROW             enter row ROW of the -C channel map before the replay\n"
                "  --analog-iq-gain-db DB            replay a copy of the capture scaled by DB (a hotter receiver)\n"
                "  --analog-pcm-tap RATE             run the capture's discriminator audio as a RATE Hz WAV input\n"
                "  --analog-pcm-gain-db DB           scale that audio by DB\n"
                "  --analog-pcm-lowpass-hz HZ        low-pass that audio at HZ first, as an SDR program's filter\n"
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
 * "--analog-x VALUE" or the "--analog-x=VALUE" spelling. A while loop, because the separate spelling consumes
 * the next argument as well. */
static int
analog_split_args(int argc, char** argv, char** out, int* out_count) {
    int kept = 0;
    int next = 0;
    while (next < argc) {
        char* arg = argv[next];
        const int is_program = next == 0;
        next++;
        char name[64];
        const char* eq = NULL;
        if (is_program || !analog_host_option_name(arg, name, sizeof(name), &eq)) {
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

/* ---- scaled replay (--analog-iq-gain-db) ---------------------------------------------------------------------- */

#define ANALOG_SCALED_DATA    "scaled.iq"
#define ANALOG_SCALED_META    "scaled.iq.json"
#define ANALOG_SIDECAR_MAX    65536L
#define ANALOG_CU8_MIDPOINT   127.5
/* Clipped bytes the copy may hold, per byte: a few noise peaks, never the signal. */
#define ANALOG_MAX_CLIP_SHARE 1e-3

static char g_scaled_dir[DSD_TEST_PATH_MAX];
static char g_scaled_meta[DSD_TEST_PATH_MAX];
static char g_scaled_arg[DSD_TEST_PATH_MAX + 16];

/* The whole of a regular file, NUL-terminated, in a malloc'd buffer; NULL when it cannot be read or exceeds limit. */
static unsigned char*
analog_read_file(const char* path, long limit, size_t* out_len) {
    FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    unsigned char* buf = NULL;
    long size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) {
        size = ftell(fp);
    }
    if (size >= 0 && size <= limit && fseek(fp, 0, SEEK_SET) == 0) {
        buf = (unsigned char*)malloc((size_t)size + 1U);
        if (buf != NULL && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
            free(buf);
            buf = NULL;
        }
    }
    (void)fclose(fp);
    if (buf != NULL) {
        buf[size] = '\0';
        *out_len = (size_t)size;
    }
    return buf;
}

/* Writes len bytes to name inside the scaled copy's directory; 0, or -1. */
static int
analog_write_scaled(const char* name, const void* data, size_t len) {
    char path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(path, sizeof(path), g_scaled_dir, name) != 0) {
        return -1;
    }
    FILE* fp = dsd_fopen_private(path, "wb");
    if (fp == NULL) {
        return -1;
    }
    const int ok = fwrite(data, 1, len, fp) == len;
    return (fclose(fp) == 0 && ok) ? 0 : -1;
}

/* The data_file value of a sidecar: where its text starts and how long it is. The capture writer and the fixture
 * builder write it without escapes, and a value with one is refused rather than misread. */
static int
analog_sidecar_data_file(const char* json, size_t* start, size_t* len) {
    const char* key = strstr(json, "\"data_file\"");
    if (key == NULL) {
        return -1;
    }
    const char* p = key + strlen("\"data_file\"");
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p++ != ':') {
        return -1;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p++ != '"') {
        return -1;
    }
    const char* end = p;
    while (*end != '\0' && *end != '"' && *end != '\\') {
        end++;
    }
    if (*end != '"' || end == p) {
        return -1;
    }
    *start = (size_t)(p - json);
    *len = (size_t)(end - p);
    return 0;
}

#ifndef DSD_NEO_TEST_IQ_FIXTURE_DIR
#error "DSD_NEO_TEST_IQ_FIXTURE_DIR names the committed I/Q fixtures (tests/CMakeLists.txt)"
#endif

/* A committed fixture looked up by name in the listing of tests/fixtures/iq. */
typedef struct {
    const char* wanted;
    size_t wanted_len;
    char* out;
    size_t cap;
    int found;
} analog_fixture_lookup;

static int
analog_fixture_lookup_entry(const char* name, void* user) {
    analog_fixture_lookup* look = (analog_fixture_lookup*)user;
    /* File names compare as the platform does: without case on Windows. */
#if DSD_PLATFORM_WIN_NATIVE
    const int differs = _strnicmp(name, look->wanted, look->wanted_len) != 0;
#else
    const int differs = strncmp(name, look->wanted, look->wanted_len) != 0;
#endif
    if (strlen(name) != look->wanted_len || differs) {
        return 0;
    }
    /* The path is built from the directory entry, so nothing from the command line or a sidecar names a file. */
    const int n = DSD_SNPRINTF(look->out, look->cap, "%s/%s", DSD_NEO_TEST_IQ_FIXTURE_DIR, name);
    look->found = n > 0 && (size_t)n < look->cap;
    return 1;
}

/* The path of the committed fixture file called [name, name + len): a bare file name in tests/fixtures/iq, found in
   that directory's listing. 0, or -1 when there is none. */
static int
analog_fixture_path(const char* name, size_t len, char* out, size_t cap) {
    analog_fixture_lookup look = {name, len, out, cap, 0};
    if (len == 0U || memchr(name, '/', len) != NULL || memchr(name, '\\', len) != NULL
        || dsd_dir_list(DSD_NEO_TEST_IQ_FIXTURE_DIR, analog_fixture_lookup_entry, &look) != 0) {
        return -1;
    }
    return look.found ? 0 : -1;
}

#if DSD_PLATFORM_WIN_NATIVE
/* Windows: _fullpath() resolves a path lexically (drive-relative, rooted on the current drive, UNC, "." and "..")
   without touching the file system; separators then read '/', and paths compare without case. 0, or -1. */
static int
analog_absolute_path(const char* path, char* out, size_t cap) {
    if (_fullpath(out, path, cap) == NULL) {
        return -1;
    }
    for (char* c = out; *c != '\0'; c++) {
        *c = (*c == '\\') ? '/' : *c;
    }
    return 0;
}

static int
analog_same_path(const char* a, const char* b) {
    return _stricmp(a, b) == 0;
}
#else
/* POSIX: @p path made absolute against the working directory and normalised lexically ("." and empty segments
   dropped, ".." taking the one before it), without asking the file system about it. 0, or -1. */
static int
analog_absolute_path(const char* path, char* out, size_t cap) {
    char joined[DSD_TEST_PATH_MAX * 2];
    char cwd[DSD_TEST_PATH_MAX] = "";
    if (cap == 0U || (path[0] != '/' && dsd_test_getcwd(cwd, sizeof cwd) == NULL)) {
        return -1;
    }
    const int n = DSD_SNPRINTF(joined, sizeof joined, "%s/%s", path[0] == '/' ? "" : cwd, path);
    if (n < 0 || (size_t)n >= sizeof joined) {
        return -1;
    }
    size_t used = 0U;
    for (const char* seg = joined; *seg != '\0'; seg += strspn(seg, "/")) {
        const size_t seg_len = strcspn(seg, "/");
        if (seg_len == 2U && seg[0] == '.' && seg[1] == '.') {
            while (used > 0U && out[used - 1U] != '/') {
                used--;
            }
            used -= used > 0U ? 1U : 0U;
        } else if (seg_len > 0U && !(seg_len == 1U && seg[0] == '.')) {
            if (used + seg_len + 2U > cap) {
                return -1;
            }
            out[used++] = '/';
            DSD_MEMCPY(out + used, seg, seg_len);
            used += seg_len;
        }
        seg += seg_len;
    }
    out[used] = '\0';
    return 0;
}

static int
analog_same_path(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}
#endif

/* The fixture @p meta_path names: the scaled copy works on the committed fixtures only, so the sidecar's directory,
   both made absolute without touching the file system, must be tests/fixtures/iq, and the sidecar is opened by its
   directory entry there. 0, or -1. */
static int
analog_fixture_sidecar_path(const char* meta_path, char* out, size_t cap) {
    char full[DSD_TEST_PATH_MAX * 2];
    char fixtures[DSD_TEST_PATH_MAX * 2];
    if (analog_absolute_path(meta_path, full, sizeof full) != 0
        || analog_absolute_path(DSD_NEO_TEST_IQ_FIXTURE_DIR, fixtures, sizeof fixtures) != 0) {
        return -1;
    }
    char* slash = strrchr(full, '/');
    if (slash == NULL || slash[1] == '\0') {
        return -1;
    }
    *slash = '\0';
    const size_t fixtures_len = strlen(fixtures);
    if (fixtures_len > 1U && fixtures[fixtures_len - 1U] == '/') {
        fixtures[fixtures_len - 1U] = '\0';
    }
    if (!analog_same_path(full, fixtures)) {
        return -1;
    }
    return analog_fixture_path(slash + 1, strlen(slash + 1), out, cap);
}

/* Scales every cu8 byte by gain about the midpoint, rounding and saturating as an 8-bit converter would. Returns the
 * bytes that saturated. */
static size_t
analog_scale_cu8(unsigned char* data, size_t len, double gain) {
    size_t clipped = 0U;
    for (size_t i = 0; i < len; i++) {
        const double v = floor((((double)data[i] - ANALOG_CU8_MIDPOINT) * gain) + ANALOG_CU8_MIDPOINT + 0.5);
        if (v < 0.0 || v > 255.0) {
            clipped++;
        }
        data[i] = (unsigned char)(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
    }
    return clipped;
}

/* A capture read whole: its sidecar text, where the sidecar names its data file, and its data. */
typedef struct {
    unsigned char* json;
    size_t json_len;
    size_t name_at; /* the data_file value: offset into json and length */
    size_t name_len;
    unsigned char* data;
    size_t data_len;
} analog_capture;

/* Reads the committed capture meta_path names and the data file its sidecar names beside it; 0, or -1 (what was read
   stays for analog_capture_free()). */
static int
analog_capture_load(const char* meta_path, analog_capture* cap) {
    char sidecar_path[DSD_TEST_PATH_MAX];
    char data_path[DSD_TEST_PATH_MAX];
    if (analog_fixture_sidecar_path(meta_path, sidecar_path, sizeof(sidecar_path)) != 0) {
        DSD_FPRINTF(stderr, "analog replay: --analog-iq-gain-db scales the committed fixtures (%s) only\n",
                    DSD_NEO_TEST_IQ_FIXTURE_DIR);
        return -1;
    }
    cap->json = analog_read_file(sidecar_path, ANALOG_SIDECAR_MAX, &cap->json_len);
    if (cap->json == NULL || analog_sidecar_data_file((const char*)cap->json, &cap->name_at, &cap->name_len) != 0
        || analog_fixture_path((const char*)cap->json + cap->name_at, cap->name_len, data_path, sizeof(data_path))
               != 0) {
        return -1;
    }
    cap->data = analog_read_file(data_path, 1L << 30, &cap->data_len);
    return cap->data != NULL ? 0 : -1;
}

static void
analog_capture_free(analog_capture* cap) {
    free(cap->data);
    free(cap->json);
}

/* Writes the capture's data and its sidecar, the data_file naming the copy beside it, into the scaled copy's
   directory; 0, or -1. */
static int
analog_capture_write(const analog_capture* cap) {
    const size_t name_len = strlen(ANALOG_SCALED_DATA);
    const size_t meta_len = cap->json_len - cap->name_len + name_len;
    char* meta = (char*)malloc(meta_len);
    if (meta == NULL) {
        return -1;
    }
    DSD_MEMCPY(meta, cap->json, cap->name_at);
    DSD_MEMCPY(meta + cap->name_at, ANALOG_SCALED_DATA, name_len);
    DSD_MEMCPY(meta + cap->name_at + name_len, cap->json + cap->name_at + cap->name_len,
               cap->json_len - cap->name_at - cap->name_len);
    const int rc = (analog_write_scaled(ANALOG_SCALED_DATA, cap->data, cap->data_len) == 0
                    && analog_write_scaled(ANALOG_SCALED_META, meta, meta_len) == 0
                    && dsd_test_path_join(g_scaled_meta, sizeof(g_scaled_meta), g_scaled_dir, ANALOG_SCALED_META) == 0)
                       ? 0
                       : -1;
    free(meta);
    return rc;
}

/* Writes the scaled copy of the capture meta_path names, with a sidecar naming it; 0, or -1 with a message. */
static int
analog_make_scaled_copy(const char* meta_path, double gain_db) {
    analog_capture cap;
    DSD_MEMSET(&cap, 0, sizeof cap);
    int rc = -1;
    if (analog_capture_load(meta_path, &cap) != 0
        || dsd_test_mkdtemp(g_scaled_dir, sizeof(g_scaled_dir), "dsdneo_analog_iq") == NULL) {
        DSD_FPRINTF(stderr, "analog replay: --analog-iq-gain-db cannot copy the capture '%s'\n", meta_path);
        g_scaled_dir[0] = '\0';
    } else {
        const size_t clipped = analog_scale_cu8(cap.data, cap.data_len, pow(10.0, gain_db / 20.0));
        DSD_FPRINTF(stderr, "analog replay: capture scaled by %+.1f dB, %zu of %zu bytes clipped\n", gain_db, clipped,
                    cap.data_len);
        if ((double)clipped > ANALOG_MAX_CLIP_SHARE * (double)cap.data_len) {
            DSD_FPRINTF(stderr, "analog replay: --analog-iq-gain-db %+.1f clips the capture; use a smaller gain\n",
                        gain_db);
        } else if (analog_capture_write(&cap) != 0) {
            DSD_FPRINTF(stderr, "analog replay: --analog-iq-gain-db cannot write the scaled copy\n");
        } else {
            rc = 0;
        }
    }
    analog_capture_free(&cap);
    return rc;
}

/* Points the --iq-replay argument at a scaled copy of its capture (--analog-iq-gain-db); 0, or -1. */
static int
analog_replay_scaled(char** args, int count) {
    static const char k_flag[] = "--iq-replay";
    for (int i = 1; i < count; i++) {
        if (strcmp(args[i], k_flag) == 0 && i + 1 < count) {
            if (analog_make_scaled_copy(args[i + 1], g_iq_gain_db.value) != 0) {
                return -1;
            }
            args[i + 1] = g_scaled_meta;
            return 0;
        }
        if (strncmp(args[i], k_flag, sizeof(k_flag) - 1U) == 0 && args[i][sizeof(k_flag) - 1U] == '=') {
            if (analog_make_scaled_copy(args[i] + sizeof(k_flag), g_iq_gain_db.value) != 0) {
                return -1;
            }
            DSD_SNPRINTF(g_scaled_arg, sizeof(g_scaled_arg), "%s=%s", k_flag, g_scaled_meta);
            args[i] = g_scaled_arg;
            return 0;
        }
    }
    DSD_FPRINTF(stderr, "analog replay: --analog-iq-gain-db needs an --iq-replay capture\n");
    return -1;
}

/* ---- discriminator tap (--analog-pcm-tap) -------------------------------------------------------------------- */

#define ANALOG_TAP_WAV            "tap.wav"
#define ANALOG_TAP_WAV_HEADER     44U
#define ANALOG_TAP_CHANNEL_HZ     8000.0 /* the channel filter's cutoff: a 16 kHz channel */
#define ANALOG_TAP_CHANNEL_TAPS   127
#define ANALOG_TAP_LPF_TAPS       255
/* The anti-alias filter ahead of a rate change ends at this fraction of the new rate, as a resampler's does. */
#define ANALOG_TAP_ALIAS_FRACTION 0.45
/* The discriminator's +/-pi radians at a quarter of int16 full scale before any gain: an unclipped tap with room for a
   hotter source. */
#define ANALOG_TAP_SCALE          (0.25 * 32767.0 / ANALOG_PI)

static uint64_t g_tap_frames; /* frames the tap WAV holds */
static int g_tap_rate_hz;     /* ... and its rate */
static char g_tap_arg[DSD_TEST_PATH_MAX];

/* A Blackman-windowed sinc low-pass of @p taps (odd) ending at @p cutoff, a fraction of the rate; unity at DC. */
static void
analog_tap_lowpass(double cutoff, int taps, double* h) {
    const int mid = taps / 2;
    double sum = 0.0;
    for (int i = 0; i < taps; i++) {
        const int k = i - mid;
        const double sinc = k == 0 ? 2.0 * cutoff : sin(2.0 * ANALOG_PI * cutoff * (double)k) / (ANALOG_PI * (double)k);
        const double phase = 2.0 * ANALOG_PI * (double)i / (double)(taps - 1);
        h[i] = sinc * (0.42 - (0.5 * cos(phase)) + (0.08 * cos(2.0 * phase)));
        sum += h[i];
    }
    for (int i = 0; i < taps; i++) {
        h[i] /= sum;
    }
}

/* @p x filtered by @p h, centred, so the filter adds no delay: what a capture holds at a time stays at that time. */
static void
analog_tap_filter(const double* x, size_t n, const double* h, int taps, double* y) {
    const long mid = taps / 2;
    for (size_t i = 0; i < n; i++) {
        double acc = 0.0;
        for (int j = 0; j < taps; j++) {
            const long at = (long)i + mid - (long)j;
            if (at >= 0 && at < (long)n) {
                acc += h[j] * x[at];
            }
        }
        y[i] = acc;
    }
}

/* The sidecar's sample_rate_hz; 0 when it has none. */
static long
analog_tap_capture_rate(const char* json) {
    static const char k_key[] = "\"sample_rate_hz\"";
    const char* at = strstr(json, k_key);
    if (at == NULL || (at = strchr(at + sizeof(k_key) - 1U, ':')) == NULL) {
        return 0;
    }
    char* end = NULL;
    const long rate = strtol(at + 1, &end, 10);
    return (end != at + 1 && rate > 0) ? rate : 0;
}

static void
analog_tap_put_le(unsigned char* p, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; i++) {
        p[i] = (unsigned char)((value >> (8 * i)) & 0xFFU);
    }
}

/* Writes @p count int16 samples at @p rate_hz as a mono WAV into the private directory; 0, or -1. */
static int
analog_tap_write_wav(const int16_t* pcm, size_t count, int rate_hz) {
    const size_t data_len = count * 2U;
    unsigned char* buf = (unsigned char*)malloc(ANALOG_TAP_WAV_HEADER + data_len);
    if (buf == NULL) {
        return -1;
    }
    DSD_MEMCPY(buf, "RIFF", 4);
    analog_tap_put_le(buf + 4, (uint32_t)(36U + data_len), 4);
    DSD_MEMCPY(buf + 8, "WAVEfmt ", 8);
    analog_tap_put_le(buf + 16, 16U, 4);                    /* fmt chunk size */
    analog_tap_put_le(buf + 20, 1U, 2);                     /* PCM */
    analog_tap_put_le(buf + 22, 1U, 2);                     /* mono */
    analog_tap_put_le(buf + 24, (uint32_t)rate_hz, 4);      /* sample rate */
    analog_tap_put_le(buf + 28, (uint32_t)rate_hz * 2U, 4); /* byte rate */
    analog_tap_put_le(buf + 32, 2U, 2);                     /* block align */
    analog_tap_put_le(buf + 34, 16U, 2);                    /* bits */
    DSD_MEMCPY(buf + 36, "data", 4);
    analog_tap_put_le(buf + 40, (uint32_t)data_len, 4);
    for (size_t i = 0; i < count; i++) {
        analog_tap_put_le(buf + ANALOG_TAP_WAV_HEADER + (2U * i), (uint32_t)(uint16_t)pcm[i], 2);
    }
    const int rc = analog_write_scaled(ANALOG_TAP_WAV, buf, ANALOG_TAP_WAV_HEADER + data_len);
    free(buf);
    return rc;
}

/* The capture's discriminator output at its own rate: a 16 kHz channel filter, then the phase step per sample. */
static double*
analog_tap_discriminate(const analog_capture* cap, size_t n, long rate_in) {
    double h[ANALOG_TAP_CHANNEL_TAPS];
    analog_tap_lowpass(ANALOG_TAP_CHANNEL_HZ / (double)rate_in, ANALOG_TAP_CHANNEL_TAPS, h);
    double* raw = (double*)malloc(n * sizeof(double));
    double* i_ch = (double*)malloc(n * sizeof(double));
    double* q_ch = (double*)malloc(n * sizeof(double));
    double* disc = (double*)malloc(n * sizeof(double));
    if (raw == NULL || i_ch == NULL || q_ch == NULL || disc == NULL) {
        free(raw);
        free(i_ch);
        free(q_ch);
        free(disc);
        return NULL;
    }
    for (size_t k = 0; k < n; k++) {
        raw[k] = ((double)cap->data[2U * k] - ANALOG_CU8_MIDPOINT) / ANALOG_CU8_MIDPOINT;
    }
    analog_tap_filter(raw, n, h, ANALOG_TAP_CHANNEL_TAPS, i_ch);
    for (size_t k = 0; k < n; k++) {
        raw[k] = ((double)cap->data[(2U * k) + 1U] - ANALOG_CU8_MIDPOINT) / ANALOG_CU8_MIDPOINT;
    }
    analog_tap_filter(raw, n, h, ANALOG_TAP_CHANNEL_TAPS, q_ch);
    disc[0] = 0.0;
    for (size_t k = 1; k < n; k++) {
        const double re = (i_ch[k] * i_ch[k - 1U]) + (q_ch[k] * q_ch[k - 1U]);
        const double im = (q_ch[k] * i_ch[k - 1U]) - (i_ch[k] * q_ch[k - 1U]);
        disc[k] = atan2(im, re);
    }
    free(raw);
    free(i_ch);
    free(q_ch);
    return disc;
}

/* Low-passes @p x in place at @p cutoff (a fraction of the rate) with a filter of @p taps; 0, or -1. */
static int
analog_tap_lowpass_in_place(double* x, size_t n, double cutoff, int taps) {
    double* h = (double*)malloc((size_t)taps * sizeof(double));
    double* y = (double*)malloc(n * sizeof(double));
    if (h == NULL || y == NULL) {
        free(h);
        free(y);
        return -1;
    }
    analog_tap_lowpass(cutoff, taps, h);
    analog_tap_filter(x, n, h, taps, y);
    DSD_MEMCPY(x, y, n * sizeof(double));
    free(h);
    free(y);
    return 0;
}

/* Writes the discriminator tap of the capture meta_path names (see the file comment); 0, or -1 with a message. */
static int
analog_make_tap(const char* meta_path) {
    analog_capture cap;
    DSD_MEMSET(&cap, 0, sizeof cap);
    int rc = -1;
    if (analog_capture_load(meta_path, &cap) != 0
        || dsd_test_mkdtemp(g_scaled_dir, sizeof(g_scaled_dir), "dsdneo_analog_pcm") == NULL) {
        DSD_FPRINTF(stderr, "analog replay: --analog-pcm-tap cannot read the capture '%s'\n", meta_path);
        g_scaled_dir[0] = '\0';
        analog_capture_free(&cap);
        return -1;
    }
    const long rate_in = analog_tap_capture_rate((const char*)cap.json);
    const long rate_out = (long)g_pcm_tap_hz.value;
    const size_t n = cap.data_len / 2U;
    if (rate_in <= 0 || fabs(g_pcm_tap_hz.value - (double)rate_out) > ANALOG_HZ_EPSILON || rate_out > rate_in
        || rate_in % rate_out != 0 || n < 2U) {
        DSD_FPRINTF(stderr, "analog replay: --analog-pcm-tap %g needs a divisor of the capture's %ld Hz\n",
                    g_pcm_tap_hz.value, rate_in);
        analog_capture_free(&cap);
        return -1;
    }
    double* disc = analog_tap_discriminate(&cap, n, rate_in);
    analog_capture_free(&cap);
    const long factor = rate_in / rate_out;
    const size_t out_n = n / (size_t)factor;
    int16_t* pcm = (int16_t*)malloc(out_n * sizeof(int16_t));
    if (disc != NULL && pcm != NULL
        && (!g_pcm_lowpass_hz.set
            || analog_tap_lowpass_in_place(disc, n, g_pcm_lowpass_hz.value / (double)rate_in, ANALOG_TAP_LPF_TAPS) == 0)
        && (factor == 1
            || analog_tap_lowpass_in_place(disc, n, ANALOG_TAP_ALIAS_FRACTION / (double)factor, ANALOG_TAP_CHANNEL_TAPS)
                   == 0)) {
        const double scale = ANALOG_TAP_SCALE * pow(10.0, (g_pcm_gain_db.set ? g_pcm_gain_db.value : 0.0) / 20.0);
        size_t clipped = 0U;
        for (size_t k = 0; k < out_n; k++) {
            double v = floor((disc[k * (size_t)factor] * scale) + 0.5);
            if (v > 32767.0 || v < -32768.0) {
                clipped++;
                v = v > 0.0 ? 32767.0 : -32768.0;
            }
            pcm[k] = (int16_t)v;
        }
        DSD_FPRINTF(stderr, "analog replay: discriminator tap at %ld Hz, %zu samples, %zu clipped\n", rate_out, out_n,
                    clipped);
        if (analog_tap_write_wav(pcm, out_n, (int)rate_out) == 0
            && dsd_test_path_join(g_tap_arg, sizeof(g_tap_arg), g_scaled_dir, ANALOG_TAP_WAV) == 0) {
            g_tap_frames = (uint64_t)out_n;
            g_tap_rate_hz = (int)rate_out;
            rc = 0;
        }
    }
    if (rc != 0) {
        DSD_FPRINTF(stderr, "analog replay: --analog-pcm-tap cannot write the tap\n");
    }
    free(pcm);
    free(disc);
    return rc;
}

/* Runs the capture's discriminator tap as audio input: `--iq-replay CAPTURE` becomes `-i TAP.wav`; 0, or -1. */
static int
analog_replay_tap(char** args, int count) {
    static char k_input_flag[] = "-i";
    for (int i = 1; i + 1 < count; i++) {
        if (strcmp(args[i], "--iq-replay") == 0) {
            if (analog_make_tap(args[i + 1]) != 0) {
                return -1;
            }
            args[i] = k_input_flag;
            args[i + 1] = g_tap_arg;
            return 0;
        }
    }
    DSD_FPRINTF(stderr, "analog replay: --analog-pcm-tap needs an --iq-replay capture (as two arguments)\n");
    return -1;
}

/* Removes the scaled copy or the tap, if one was made. */
static void
analog_remove_scaled(void) {
    static const char* const k_files[] = {ANALOG_SCALED_DATA, ANALOG_SCALED_META, ANALOG_TAP_WAV, NULL};
    if (g_scaled_dir[0] != '\0' && dsd_test_remove_temp_dir(g_scaled_dir, k_files) != 0) {
        DSD_FPRINTF(stderr, "analog replay: could not remove %s\n", g_scaled_dir);
    }
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

/* A DCS code in the tone= field's spelling (see the file comment): the published, canonical spelling, a slash and the
 * signal's other standard spelling, "D023N/D047I". A publication that names no supported signal leaves the last
 * label as it was. */
static void
analog_format_dcs_label(int code, int inverted) {
    char canon[DSD_DCS_LABEL_SIZE];
    char alias[DSD_DCS_LABEL_SIZE];
    int alias_code = -1;
    int alias_inverted = -1;
    if (dsd_dcs_format(code, inverted, canon, sizeof(canon)) <= 0
        || dsd_dcs_alias(code, inverted, &alias_code, &alias_inverted) != 0
        || dsd_dcs_format(alias_code, alias_inverted, alias, sizeof(alias)) <= 0) {
        return;
    }
    DSD_SNPRINTF(g_tone.label, sizeof(g_tone.label), "%s/%s", canon, alias);
}

/* Reads the received-tone publication after one delivered block, which ended at block_end_ms. The tap updated it
 * from this same block just before the block came here. */
static void
analog_note_tone(const dsd_state* state, double block_end_ms, double block_ms) {
    if (state == NULL || state->analog_rx.tone_state != DSD_ANALOG_TONE_STATE_LOCKED) {
        return;
    }
    g_tone.locked_ms += block_ms;
    if (g_tone.first_lock_ms < 0.0) {
        g_tone.first_lock_ms = block_end_ms;
    }
    if (state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS) {
        (void)dsd_ctcss_format(state->analog_rx.ctcss_tenths_hz, g_tone.label, sizeof(g_tone.label));
    } else if (state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_DCS) {
        analog_format_dcs_label(state->analog_rx.dcs_code, state->analog_rx.dcs_inverted);
    }
}

/* Whether the run reads the discriminator tap as audio input (--analog-pcm-tap). */
static int
analog_pcm_input(const dsd_opts* opts) {
    return g_pcm_tap_hz.set && opts != NULL && opts->audio_in_type == AUDIO_IN_WAV;
}

/* Stream time on audio input: the tap WAV's read position, in frames at its own rate (the decoder reads it a sample at
 * a time, and a staged low rate holds less than one input sample ahead); the whole tap once the reader reached its
 * end and closed it. */
static void
analog_note_pcm_position(const dsd_opts* opts) {
    if (g_tap_rate_hz <= 0) {
        return;
    }
    sf_count_t frames = (sf_count_t)g_tap_frames;
    if (opts->audio_in_file != NULL) {
        frames = sf_seek(opts->audio_in_file, 0, SEEK_CUR);
        if (frames < 0) {
            return;
        }
    }
    const double ms = (double)frames * 1000.0 / (double)g_tap_rate_hz;
    if (ms > g_totals.read_ms) {
        g_totals.read_ms = ms;
    }
}

/* Replaces the UDP analog blaster. nbytes is a byte count of int16 mono samples, as dsd_symbol.c passes it. */
static void
analog_capture_blast(const dsd_opts* opts, dsd_state* state, size_t nbytes, const void* data) {
    size_t n = nbytes / sizeof(short);
    if (data == NULL || n == 0U) {
        return;
    }
    const int pcm_input = analog_pcm_input(opts);
    if (pcm_input) {
        analog_note_pcm_position(opts);
    }
    /* The monitor's rate: the RTL output rate, or on audio input its own (staged) rate. */
    unsigned int rate_hz = pcm_input ? (unsigned int)dsd_opts_current_input_timing_rate(opts)
                                     : dsd_rtl_stream_metrics_hook_output_rate_hz();
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
    analog_note_tone(state, block_start_ms + block_ms, block_ms);
}

#ifdef USE_RADIO
/* The decoder's reads, flagged (the monitor's, with the auto squelch's gate) or not, counted alike. */
static int
analog_counting_read_ex(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    int rc = rtl_stream_read_ex((RtlSdrContext*)rtl_ctx, out, flags, count, out_got);
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

static int
analog_counting_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    return analog_counting_read_ex(rtl_ctx, out, NULL, count, out_got);
}

static double
analog_return_pwr(const void* rtl_ctx) {
    return rtl_stream_return_pwr((const RtlSdrContext*)rtl_ctx);
}
#endif

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
#ifdef USE_RADIO
    dsd_rtl_stream_io_hooks io = {0};
    io.read = analog_counting_read;
    io.read_ex = analog_counting_read_ex;
    io.return_pwr = analog_return_pwr;
    dsd_rtl_stream_io_hooks_set(io);
#endif
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
    int have_lock;
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
    ctx->have_lock = g_tone.first_lock_ms >= 0.0;
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
    /* Received-tone fields (see the file comment). */
    used = strlen(line);
    DSD_SNPRINTF(line + used, sizeof(line) - used, " tone=%s", g_tone.label[0] != '\0' ? g_tone.label : "NA");
    analog_print_value(line, sizeof(line), "tone_lock_ms", ctx->have_lock, g_tone.first_lock_ms);
    const int have_captured = g_totals.captured_ms > 0.0;
    analog_print_value(line, sizeof(line), "tone_lock_pct", have_captured,
                       have_captured ? 100.0 * g_tone.locked_ms / g_totals.captured_ms : 0.0);
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
         * length, so total_ms can come out far too low or far too high. tools/replay_ab.sh (its off_path column)
         * and the -fA cases in tests/CMakeLists.txt match "CQPSK symbols instead of monitor samples". */
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
        DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: input arrived without an RTL output rate; the host scores RTL, "
                            "I/Q replay and --analog-pcm-tap input only\n");
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
    analog_check_max("tone lock ms", ctx->have_lock, g_tone.first_lock_ms, &g_limits.max_tone_lock_ms);
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

/* Commit a -C map row the way the conventional scanner does (issue #526): prepare with the row's options, enter its
 * class, then install the options. Returns 0, or -1 when the row does not exist or declares no class. */
static int
analog_enter_scan_row(dsd_opts* opts, dsd_state* state, double requested) {
    const int row = (int)floor(requested);
    if (fabs(requested - (double)row) > ANALOG_HZ_EPSILON || row >= state->lcn_freq_count) {
        DSD_FPRINTF(stderr, "analog replay: --analog-scan-row %g is not a row of the -C map\n", requested);
        return -1;
    }
    const dsd_scan_mode mode = dsd_channel_mode_get(state, (size_t)row);
    const dsd_scan_row_profile* profile = dsd_channel_profile_get(state, (size_t)row);
    const dsd_scan_option_values* values = profile ? &profile->values : NULL;
    dsd_scan_settings prepared;
    if (mode == DSD_SCAN_MODE_INHERIT || dsd_scan_mode_prepare(opts, state, mode, values, &prepared) != 0
        || dsd_scan_mode_enter(opts, state, mode) != 0 || dsd_scan_mode_options(opts, state, values) != 0) {
        DSD_FPRINTF(stderr, "analog replay: row %d declares no scan class the replay can enter\n", row);
        return -1;
    }
    char width[DSD_ANALOG_WIDTH_TEXT_MAX] = "-";
    if (dsd_opts_is_analog_family(opts)) {
        (void)dsd_analog_width_format(dsd_analog_width_effective_hz(opts->analog_demod, dsd_opts_analog_width_hz(opts)),
                                      width, sizeof width);
    }
    char squelch[DSD_SQUELCH_TEXT_SIZE];
    const dsd_squelch_setting setting = dsd_squelch_setting_of_opts(opts);
    (void)dsd_squelch_setting_format(&setting, squelch, sizeof squelch);
    const uint32_t fields = dsd_scan_mode_option_fields(state);
    DSD_FPRINTF(stderr, "Scan row applied: %d %s; width %s%s; squelch %s%s\n", row, dsd_scan_mode_name(mode), width,
                (fields & DSD_SCAN_OPT_BANDWIDTH) ? " (row)" : "", squelch,
                (fields & DSD_SCAN_OPT_SQUELCH) ? " (row)" : "");
    return 0;
}

static void
analog_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)state;
    (void)context;
    if (analog_pcm_input(opts)) {
        analog_note_pcm_position(opts);
    }
    analog_report();
}

int
main(int argc, char** argv) {
    char** args = (char**)calloc((size_t)argc + 1U, sizeof(*args));
    if (args == NULL) {
        return 1;
    }
    g_totals.first_audible_ms = -1.0;
    g_tone.first_lock_ms = -1.0;
    int kept = 0;
    int args_rc = analog_split_args(argc, argv, args, &kept);
    if (args_rc == 0 && g_iq_gain_db.set && g_pcm_tap_hz.set) {
        DSD_FPRINTF(stderr, "analog replay: --analog-iq-gain-db scales a capture, --analog-pcm-tap replaces it\n");
        args_rc = -1;
    }
    if (args_rc != 0 || (g_iq_gain_db.set && analog_replay_scaled(args, kept) != 0)
        || (g_pcm_tap_hz.set && analog_replay_tap(args, kept) != 0)) {
        analog_remove_scaled();
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
            if (g_scan_row.set && analog_enter_scan_row(opts, state, g_scan_row.value) != 0) {
                rc = 2;
            } else {
                rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
            }
            if (rc == 0 && g_failed) {
                rc = 1;
            }
        }
        dsd_scan_mode_leave(opts, state);
        freeState(state);
    }
    free(state);
    free(opts);
    free((void*)args);
    analog_remove_scaled();
    return rc;
}
