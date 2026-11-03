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
 * consumes, which is the time base for first-audible, last-audible and audible durations: muted or squelched blocks
 * never reach the audio hook, so the audio alone cannot say when it started or stopped. No product code is involved.
 *
 * The host's own --analog-* options (see analog_usage) are removed before the rest reach dsd_runtime_bootstrap();
 * any other --analog-* argument is passed through to the CLI parser unchanged. When live processing ends, the stop
 * hook prints one "ANALOG METRIC:" line and one "ANALOG PROBE:" line per probe frequency, then "ANALOG AUDIO OK"
 * when every requested threshold holds, or an "ANALOG AUDIO FAIL:" line per miss, in which case the host exits 1.
 * Without thresholds it only reports, which is how tools/replay_ab.sh --metric analog uses it.
 *
 * The monitor's audio arrives at the analog sink rate (48 kHz) whatever rate the monitor runs at (issue #633): the host
 * scores it at that rate, one monitor block at a time however many writes the block came in, and keeps stream time at
 * the monitor's own rate. --analog-max-delivery-error-pct bounds how far the length of the delivered audio strays from
 * the input its blocks span, which on audio input is 960 samples a block at the monitor's rate.
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
 *                       decimal) for CTCSS; for DCS both supported spellings of the code's signal, canonical first
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
 * filter and a polar discriminator, nothing after them), resamples it to RATE Hz (decimated for a divisor of the
 * capture's rate, interpolated with a windowed sinc for any other), writes it as a 16-bit mono WAV to the same private
 * directory and runs `-i TAP.wav` in place of `--iq-replay`.
 * --analog-pcm-gain-db scales that audio (a louder source) and --analog-pcm-lowpass-hz low-passes it first (an SDR
 * program's audio filter). Stream time on audio input is the WAV's read position, in frames at its own rate, so the
 * bounds mean the same as on a replay; it needs no radio support, and the PCM cases run in every build.
 *
 * --analog-raw-wav-min-rms-dbfs DB (issue #643) records the run's `-6` raw WAV too: the host adds `-6 FILE` for a
 * private temporary file, reads it back when live processing ends, prints one "ANALOG RAW WAV:" line (its frames, RMS
 * and peak dBFS, clipped samples) after the METRIC line, fails when its RMS is below DB, and removes the file when it
 * exits.
 *
 * --analog-synth-pauses SEED (issue #756) makes a 9.2 s NFM capture at run time, five voice-like bursts with pauses of
 * 0.3, 0.6, 1.5 and 3.0 s between them, writes it to the private directory and adds `--iq-replay` for it (the case
 * names no capture of its own); with --analog-pcm-tap the run takes its discriminator tap as `-i` instead. With per-call
 * recording on (-P), the stop hook, which runs after the engine committed the session's last calls, also prints one
 * "ANALOG CALL ROW: start=S end=E tone=T" line per analog call history row, oldest first (decode time, whole seconds;
 * T the row's received tone as its text names it, "CTCSS 100.0 Hz", or "none").
 * tests/analog_recording_check.cmake reads those lines with the recordings and the journal.
 *
 * --analog-gap-join AT_MS:GAP_MS:FIXTURE:THEN_MS (issue #667) replays the first AT_MS of the --iq-replay capture, then
 * a GAP_MS gap in the radio stream as a USB recovery records one (a RETUNE to the same centre, the muted samples and a
 * RESET, each named usb_recovery), then the first THEN_MS of FIXTURE, another committed 48 kHz capture named without
 * its .iq.json and found by its directory entry there, as the scaled copy's capture is. The joined copy goes to the
 * private directory, as the scaled copy does.
 * --analog-scene NAME (issue #668) makes one of the scenes below at run time and takes it as the input instead:
 * bursts of voice-band noise that never repeats, as I/Q, as audio input at --analog-pcm-tap's rate, or sent live over
 * UDP. With -P and --wav-journal, once the engine has stopped, the host finds every published recording in the scene
 * and prints one "ANALOG RECORDING:" line for it: where its first sample, and its first sample that is not 0, fell
 * against its burst's start. --analog-record-{min,max}-{start,audio}-ms bound those. A scene's run prints "ANALOG AUDIO
 * OK" only after its recordings were checked. --analog-keep-output keeps the session's own output settings instead of
 * capturing the monitor (-o null stays muted, as engine setup leaves it), prints one "ANALOG OUTPUT:" line and fails
 * unless the output is off and nothing reached it.
 */

#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
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
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

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
    analog_limit min_last_audible_ms;
    analog_limit max_last_audible_ms;
    analog_limit min_inband_db;
    analog_limit min_rms_dbfs;
    analog_limit max_rms_dbfs;
    analog_limit max_peak_dbfs;
    analog_limit max_clip;
    analog_limit audible_dbfs;
    analog_limit max_tone_lock_ms;
    analog_limit max_delivery_error_pct;
} analog_limits;

enum { PROBE_MIN_DBC = 0, PROBE_MAX_DBC, PROBE_MIN_DBFS, PROBE_MAX_DBFS, PROBE_LIMIT_COUNT };

typedef struct {
    double hz;
    analog_limit limit[PROBE_LIMIT_COUNT];
    double power_sum; /* sum over blocks of (probe power x block length), full scale = 1 */
} analog_probe;

typedef struct {
    unsigned int rate_hz;       /* RTL output rate at the latest read or audio block */
    unsigned int deliver_hz;    /* the analog sink rate the monitor's audio arrives at (issue #633) */
    uint64_t blocks_scored;     /* monitor blocks delivered */
    uint64_t native_scored;     /* ... and the input samples they span at the monitor's own rate */
    unsigned int first_rate_hz; /* ... at the first one */
    unsigned int rate_changes;  /* times the rate differed from the one before */
    double read_ms;             /* stream time the RTL stream read hook returned */
    uint64_t unclocked_reads;   /* samples read while no output rate was known */
    uint64_t cqpsk_reads;       /* samples read while the front end ran its CQPSK symbol path */
    uint64_t samples_captured;  /* samples that reached the audio hook */
    double captured_ms;
    double audible_ms;       /* ... in blocks at or above the audible level */
    double first_audible_ms; /* stream time where the first audible block began, -1 if none */
    double last_audible_ms;  /* stream time where the last audible block ended, -1 if none */
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
/* The run read audio input, where the delivery check applies (--analog-max-delivery-error-pct). */
static int g_delivery_pcm;
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
/* The lower bound on the -6 raw WAV's RMS level (--analog-raw-wav-min-rms-dbfs); unset records no raw WAV. */
static analog_limit g_raw_wav_min_rms_dbfs;
/* The seed of the capture with pauses the host makes and replays (--analog-synth-pauses); unset makes none. */
static analog_limit g_synth_pauses;
/* Where each recording of a scene starts against its burst (--analog-record-*-ms, issue #668); unset checks nothing. */
static analog_limit g_record_min_start_ms;
static analog_limit g_record_max_start_ms;
static analog_limit g_record_min_audio_ms;
static analog_limit g_record_max_audio_ms;
/* The lowest level each recording may keep over its burst's first 300 ms, against its median there
   (--analog-record-min-level, issue #668); unset checks nothing. */
static analog_limit g_record_min_level;
/* The session's own output settings stay (--analog-keep-output, issue #668): -o null stays muted, nothing captured. */
static int g_keep_output;
/* The stop hook reported the run's live checks (analog_report()). */
static int g_reported;
/* The option that names a scene (--analog-scene, issue #668) and the one that takes no value. */
static const char k_scene_arg[] = "--analog-scene";
static const char k_keep_output_arg[] = "--analog-keep-output";

/* The stream gap the replayed capture is joined across (--analog-gap-join); unset joins none. */
typedef struct {
    int set;
    long at_ms;
    long gap_ms;
    char then[128];
    long then_ms;
} analog_gap_join;

static analog_gap_join g_gap_join;
static const char k_gap_join_arg[] = "--analog-gap-join";

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
    {"--analog-min-last-audible-ms", &g_limits.min_last_audible_ms, 0.0, 1e9},
    {"--analog-max-last-audible-ms", &g_limits.max_last_audible_ms, 0.0, 1e9},
    {"--analog-min-inband-db", &g_limits.min_inband_db, -ANALOG_DB_LIMIT, ANALOG_DB_LIMIT},
    {"--analog-min-rms-dbfs", &g_limits.min_rms_dbfs, -ANALOG_DB_LIMIT, 0.0},
    {"--analog-max-rms-dbfs", &g_limits.max_rms_dbfs, -ANALOG_DB_LIMIT, 0.0},
    {"--analog-max-peak-dbfs", &g_limits.max_peak_dbfs, -ANALOG_DB_LIMIT, 0.0},
    {"--analog-max-clip", &g_limits.max_clip, 0.0, 1e12},
    {"--analog-audible-dbfs", &g_limits.audible_dbfs, -ANALOG_DB_LIMIT, 0.0},
    {"--analog-max-tone-lock-ms", &g_limits.max_tone_lock_ms, 0.0, 1e9},
    {"--analog-max-delivery-error-pct", &g_limits.max_delivery_error_pct, 0.0, 100.0},
    {"--analog-scan-row", &g_scan_row, 0.0, 65535.0},
    {"--analog-iq-gain-db", &g_iq_gain_db, -40.0, 40.0},
    {"--analog-pcm-tap", &g_pcm_tap_hz, 8000.0, 96000.0},
    {"--analog-pcm-gain-db", &g_pcm_gain_db, -40.0, 20.0},
    {"--analog-pcm-lowpass-hz", &g_pcm_lowpass_hz, 1000.0, 20000.0},
    {"--analog-raw-wav-min-rms-dbfs", &g_raw_wav_min_rms_dbfs, -ANALOG_DB_LIMIT, 0.0},
    {"--analog-synth-pauses", &g_synth_pauses, 1.0, 4294967295.0},
    {"--analog-record-min-start-ms", &g_record_min_start_ms, -1e5, 1e5},
    {"--analog-record-max-start-ms", &g_record_max_start_ms, -1e5, 1e5},
    {"--analog-record-min-audio-ms", &g_record_min_audio_ms, -1e5, 1e5},
    {"--analog-record-max-audio-ms", &g_record_max_audio_ms, -1e5, 1e5},
    {"--analog-record-min-level", &g_record_min_level, 0.0, 1.0},
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
                "  --analog-min-last-audible-ms MS   stream time when the last audible audio ends\n"
                "  --analog-max-last-audible-ms MS\n"
                "  --analog-min-inband-db DB         300-3000 Hz energy over 3400-6000 Hz energy\n"
                "  --analog-min-rms-dbfs DB          RMS level of the delivered audio\n"
                "  --analog-max-rms-dbfs DB\n"
                "  --analog-max-peak-dbfs DB         largest sample of the delivered audio\n"
                "  --analog-max-clip N               samples at int16 full scale\n"
                "  --analog-audible-dbfs DB          block RMS counted as audible (default -50)\n"
                "  --analog-max-tone-lock-ms MS      stream time when the received tone first locks\n"
                "  --analog-max-delivery-error-pct P delivered audio vs the input it spans, at the sink rate\n"
                "  --analog-probe-hz HZ              report the level at HZ (repeatable, up to 8)\n"
                "  --analog-probe-{min,max}-dbc HZ:DB   probe level relative to the expected tone\n"
                "  --analog-probe-{min,max}-dbfs HZ:DB  probe level relative to full scale\n"
                "  --analog-scan-row ROW             enter row ROW of the -C channel map before the replay\n"
                "  --analog-iq-gain-db DB            replay a copy of the capture scaled by DB (a hotter receiver)\n"
                "  --analog-pcm-tap RATE             run the capture's discriminator audio as a RATE Hz WAV input\n"
                "  --analog-pcm-gain-db DB           scale that audio by DB\n"
                "  --analog-pcm-lowpass-hz HZ        low-pass that audio at HZ first, as an SDR program's filter\n"
                "  --analog-raw-wav-min-rms-dbfs DB  record the -6 raw WAV to a private file; RMS level of that WAV\n"
                "  --analog-synth-pauses SEED        replay a made NFM capture of bursts with pauses between them\n"
                "  --analog-gap-join AT_MS:GAP_MS:FIXTURE:THEN_MS  replay the capture to AT_MS, a recorded stream\n"
                "                                    gap, then FIXTURE to THEN_MS\n"
                "  --analog-scene NAME               replay a made scene of voice bursts, find each recording in it\n"
                "  --analog-record-{min,max}-start-ms MS  where each recording starts against its burst\n"
                "  --analog-record-{min,max}-audio-ms MS  where its first sample that is not 0 is\n"
                "  --analog-keep-output              keep the session's own output (-o null stays muted)\n"
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
    return analog_find_scalar(name) != NULL || analog_find_probe_limit(name) >= 0 || strcmp(name, k_probe_hz_arg) == 0
           || strcmp(name, k_gap_join_arg) == 0 || strcmp(name, k_scene_arg) == 0
           || strcmp(name, k_keep_output_arg) == 0;
}

/* One ':'-ended field of @p *text, a whole number of ms from 1 to 60000; advances past it. */
static int
analog_parse_gap_ms(const char** text, long* out) {
    char field[16];
    const char* colon = strchr(*text, ':');
    const size_t len = colon != NULL ? (size_t)(colon - *text) : strlen(*text);
    if (len == 0U || len >= sizeof field) {
        return -1;
    }
    DSD_MEMCPY(field, *text, len);
    field[len] = '\0';
    *text = colon != NULL ? colon + 1 : *text + len;
    return dsd_parse_long_strict(field, 10, 1L, 60000L, out);
}

/* AT_MS:GAP_MS:FIXTURE:THEN_MS (--analog-gap-join). */
static int
analog_parse_gap_join(const char* text) {
    const char* p = text;
    if (analog_parse_gap_ms(&p, &g_gap_join.at_ms) != 0 || analog_parse_gap_ms(&p, &g_gap_join.gap_ms) != 0) {
        DSD_FPRINTF(stderr, "%s: expected AT_MS:GAP_MS:FIXTURE:THEN_MS, got '%s'\n", k_gap_join_arg, text);
        return -1;
    }
    const char* colon = strchr(p, ':');
    const size_t len = colon != NULL ? (size_t)(colon - p) : 0U;
    if (len == 0U || len >= sizeof g_gap_join.then) {
        DSD_FPRINTF(stderr, "%s: expected AT_MS:GAP_MS:FIXTURE:THEN_MS, got '%s'\n", k_gap_join_arg, text);
        return -1;
    }
    DSD_MEMCPY(g_gap_join.then, p, len);
    g_gap_join.then[len] = '\0';
    p = colon + 1;
    if (analog_parse_gap_ms(&p, &g_gap_join.then_ms) != 0 || *p != '\0') {
        DSD_FPRINTF(stderr, "%s: expected AT_MS:GAP_MS:FIXTURE:THEN_MS, got '%s'\n", k_gap_join_arg, text);
        return -1;
    }
    g_gap_join.set = 1;
    return 0;
}

static int analog_apply_scene(const char* value);

/* Applies one host option (name already known to be one); returns 0, or -1 on a bad value. */
static int
analog_apply_option(const char* name, const char* value) {
    if (strcmp(name, k_gap_join_arg) == 0) {
        return analog_parse_gap_join(value);
    }
    const analog_scalar_arg* scalar = analog_find_scalar(name);
    if (scalar != NULL) {
        scalar->target->set = 1;
        return analog_parse_double(name, value, scalar->min_value, scalar->max_value, &scalar->target->value);
    }
    if (strcmp(name, k_scene_arg) == 0) {
        return analog_apply_scene(value);
    }
    if (strcmp(name, k_keep_output_arg) == 0) {
        DSD_FPRINTF(stderr, "analog replay: %s takes no value\n", name);
        return -1;
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
        if (strcmp(name, k_keep_output_arg) == 0 && eq == NULL) {
            g_keep_output = 1;
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
    const char* key = strstr(json, k_key);
    if (key == NULL) {
        return 0;
    }
    const char* at = strchr(key + sizeof(k_key) - 1U, ':');
    if (at == NULL) {
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
    /* Zeroed: GCC at -O2 cannot see that the loops below fill it before each filter reads it. */
    double* raw = (double*)calloc(n, sizeof(double));
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

/* Input samples each side of the point the windowed-sinc interpolator evaluates. */
#define ANALOG_TAP_SINC_HALF 64

/* @p x (@p n samples at @p rate_in) at @p rate_out, for a rate the capture's does not divide (issue #633): a
 * Hann-windowed sinc evaluated at each output instant, low-passed at ANALOG_TAP_ALIAS_FRACTION of the lower rate. It
 * shares nothing with the converter under test. Returns the samples (free() them) and their count in @p out_n, or NULL.
 */
static double*
analog_tap_resample(const double* x, size_t n, long rate_in, long rate_out, size_t* out_n) {
    const size_t m = (size_t)(((uint64_t)n * (uint64_t)rate_out) / (uint64_t)rate_in);
    double* y = m > 0U ? (double*)malloc(m * sizeof(double)) : NULL;
    if (y == NULL) {
        return NULL;
    }
    const double lower = (double)(rate_in < rate_out ? rate_in : rate_out);
    const double cutoff = ANALOG_TAP_ALIAS_FRACTION * lower / (double)rate_in; /* cycles per input sample */
    for (size_t k = 0; k < m; k++) {
        const double t = (double)k * (double)rate_in / (double)rate_out;
        const long center = (long)floor(t);
        double acc = 0.0;
        for (long j = center - ANALOG_TAP_SINC_HALF + 1; j <= center + ANALOG_TAP_SINC_HALF; j++) {
            if (j < 0 || j >= (long)n) {
                continue;
            }
            const double d = t - (double)j;
            const double w = 0.5 + (0.5 * cos(ANALOG_PI * d / (double)ANALOG_TAP_SINC_HALF));
            const double arg = 2.0 * cutoff * d;
            const double sinc = fabs(arg) < ANALOG_HZ_EPSILON ? 1.0 : sin(ANALOG_PI * arg) / (ANALOG_PI * arg);
            acc += x[j] * 2.0 * cutoff * sinc * w;
        }
        y[k] = acc;
    }
    *out_n = m;
    return y;
}

/* Scales one sample in @p step of @p disc (@p disc_n samples) to int16 into @p pcm (@p out_n samples); returns how many
 * clipped. The reads stop at the end of @p disc as well as at the end of @p pcm. */
static size_t
analog_tap_quantize(const double* disc, size_t disc_n, size_t step, int16_t* pcm, size_t out_n) {
    const double scale = ANALOG_TAP_SCALE * pow(10.0, (g_pcm_gain_db.set ? g_pcm_gain_db.value : 0.0) / 20.0);
    size_t clipped = 0U;
    size_t k = 0U;
    for (size_t src = 0U; k < out_n && src < disc_n; src += step, k++) {
        double v = floor((disc[src] * scale) + 0.5);
        if (v > 32767.0 || v < -32768.0) {
            clipped++;
            v = v > 0.0 ? 32767.0 : -32768.0;
        }
        pcm[k] = (int16_t)v;
    }
    return clipped;
}

/* Writes the discriminator tap of @p cap into the private directory (see the file comment); 0, or -1 with a message. */
static int
analog_tap_write(const analog_capture* cap) {
    int rc = -1;
    const long rate_in = analog_tap_capture_rate((const char*)cap->json);
    const long rate_out = (long)g_pcm_tap_hz.value;
    const size_t n = cap->data_len / 2U;
    if (rate_in <= 0 || rate_out <= 0 || fabs(g_pcm_tap_hz.value - (double)rate_out) > ANALOG_HZ_EPSILON || n < 2U) {
        DSD_FPRINTF(stderr, "analog replay: --analog-pcm-tap %g cannot be made from the capture's %ld Hz\n",
                    g_pcm_tap_hz.value, rate_in);
        return -1;
    }
    double* disc = analog_tap_discriminate(cap, n, rate_in);
    /* A divisor of the capture's rate decimates after a low-pass; any other rate is interpolated (issue #633). */
    const int divides = rate_out <= rate_in && rate_in % rate_out == 0;
    const long factor = divides ? rate_in / rate_out : 1;
    size_t out_n = divides ? n / (size_t)factor : 0U;
    size_t disc_n = n; /* the samples disc holds */
    if (disc != NULL && g_pcm_lowpass_hz.set
        && analog_tap_lowpass_in_place(disc, n, g_pcm_lowpass_hz.value / (double)rate_in, ANALOG_TAP_LPF_TAPS) != 0) {
        free(disc);
        disc = NULL;
    }
    if (disc != NULL && !divides) {
        double* resampled = analog_tap_resample(disc, n, rate_in, rate_out, &out_n);
        free(disc);
        disc = resampled;
        disc_n = out_n;
    }
    int16_t* pcm = out_n > 0U ? (int16_t*)calloc(out_n, sizeof(int16_t)) : NULL;
    if (disc != NULL && pcm != NULL
        && (factor == 1
            || analog_tap_lowpass_in_place(disc, disc_n, ANALOG_TAP_ALIAS_FRACTION / (double)factor,
                                           ANALOG_TAP_CHANNEL_TAPS)
                   == 0)) {
        const size_t clipped = analog_tap_quantize(disc, disc_n, (size_t)factor, pcm, out_n);
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

/* Writes the discriminator tap of the committed capture meta_path names; 0, or -1 with a message. */
static int
analog_make_tap(const char* meta_path) {
    analog_capture cap;
    DSD_MEMSET(&cap, 0, sizeof cap);
    int rc = -1;
    if (analog_capture_load(meta_path, &cap) != 0
        || dsd_test_mkdtemp(g_scaled_dir, sizeof(g_scaled_dir), "dsdneo_analog_pcm") == NULL) {
        DSD_FPRINTF(stderr, "analog replay: --analog-pcm-tap cannot read the capture '%s'\n", meta_path);
        g_scaled_dir[0] = '\0';
    } else {
        rc = analog_tap_write(&cap);
    }
    analog_capture_free(&cap);
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

/* ---- a capture with pauses (--analog-synth-pauses) ------------------------------------------------------------ */

/* Issue #756: five voice-like NFM bursts with pauses of 0.3, 0.6, 1.5 and 3.0 s between them, which a per-call
 * recording's max gap joins or splits. The host makes it at run time, as the analog fixture budget is spent
 * (docs/testing.md, "Analog fixtures"), the way tools/build_iq_fixtures.py makes nfm_burst_synth: complex Gaussian
 * noise at 3 LSB a component across 48 kHz, the carrier 16 dB over it, keyed with 2 ms raised-cosine ramps, cu8 at its
 * own level. Each burst carries 700 and 1300 Hz at up to 3 kHz deviation, swelling at 3 Hz, so it is not one steady
 * line. The seed fixes the noise. */
#define ANALOG_SYNTH_RATE_HZ      48000
#define ANALOG_SYNTH_LEAD_S       0.5 /* noise alone first: a noise squelch learns its reference from it */
#define ANALOG_SYNTH_BURST_S      0.5
#define ANALOG_SYNTH_BURSTS       5
/* After the last burst: shorter than a max gap of 1000 ms, so such a call is still held when the input ends. */
#define ANALOG_SYNTH_TAIL_S       0.8
#define ANALOG_SYNTH_RAMP_S       0.002
#define ANALOG_SYNTH_NOISE_LSB    3.0
#define ANALOG_SYNTH_CARRIER_DB   16.0
#define ANALOG_SYNTH_DEVIATION_HZ 3000.0
#define ANALOG_SYNTH_DATA         "synth.iq"
#define ANALOG_SYNTH_JSON_MAX     1024

static const double k_synth_pauses_s[ANALOG_SYNTH_BURSTS - 1] = {0.3, 0.6, 1.5, 3.0};

/* splitmix64: the same noise for a seed on every platform, unlike rand(). */
static uint64_t
analog_synth_next(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform on (0, 1), never 0, so the logarithm below is finite. */
static double
analog_synth_uniform(uint64_t* state) {
    return ((double)(analog_synth_next(state) >> 11) + 0.5) / 9007199254740992.0;
}

/* The keying envelope at @p t seconds: 1 inside a burst, 0 in a pause, raised-cosine ramps at the edges. */
static double
analog_synth_envelope(double t) {
    double start = ANALOG_SYNTH_LEAD_S;
    for (int b = 0; b < ANALOG_SYNTH_BURSTS; b++) {
        const double end = start + ANALOG_SYNTH_BURST_S;
        if (t > start && t < end) {
            const double key = fmin(t - start, end - t) / ANALOG_SYNTH_RAMP_S;
            return 0.5 - (0.5 * cos(ANALOG_PI * fmin(key, 1.0)));
        }
        start = end + (b < ANALOG_SYNTH_BURSTS - 1 ? k_synth_pauses_s[b] : 0.0);
    }
    return 0.0;
}

static unsigned char
analog_synth_cu8(double x) {
    const double v = floor((x * ANALOG_CU8_MIDPOINT) + ANALOG_CU8_MIDPOINT + 0.5);
    return (unsigned char)fmin(fmax(v, 0.0), 255.0);
}

/* The capture's sidecar, as tools/build_iq_fixtures.py writes nfm_burst_synth's, for @p data_bytes of cu8 data. */
static int
analog_synth_sidecar(analog_capture* cap, size_t data_bytes) {
    cap->json = (unsigned char*)malloc(ANALOG_SYNTH_JSON_MAX);
    if (cap->json == NULL) {
        return -1;
    }
    const int n = DSD_SNPRINTF(
        (char*)cap->json, ANALOG_SYNTH_JSON_MAX,
        "{\n  \"format\": \"dsd-neo-iq\",\n  \"version\": 1,\n  \"sample_format\": \"cu8\",\n  \"iq_order\": \"IQ\",\n"
        "  \"endianness\": \"none\",\n  \"capture_stage\": \"post_mute_pre_widen\",\n  \"sample_rate_hz\": %d,\n"
        "  \"center_frequency_hz\": 851375000,\n  \"capture_center_frequency_hz\": 851375000,\n  \"ppm\": 0,\n"
        "  \"tuner_gain_tenth_db\": 270,\n  \"rtl_dsp_bw_khz\": 48,\n  \"base_decimation\": 1,\n"
        "  \"post_downsample\": 1,\n  \"demod_rate_hz\": %d,\n  \"offset_tuning_enabled\": false,\n"
        "  \"fs4_shift_enabled\": false,\n  \"combine_rotate_enabled\": false,\n  \"muted_bytes_excluded\": true,\n"
        "  \"contains_retunes\": false,\n  \"capture_retune_count\": 0,\n  \"source_backend\": \"rtl\",\n"
        "  \"source_args\": \"fixture\",\n  \"capture_started_utc\": \"1970-01-01T00:00:00Z\",\n"
        "  \"data_file\": \"" ANALOG_SYNTH_DATA "\",\n  \"data_bytes\": %zu,\n  \"capture_drops\": 0,\n"
        "  \"capture_drop_blocks\": 0,\n  \"input_ring_drops\": 0,\n"
        "  \"notes\": \"made by the analog replay host\"\n}\n",
        ANALOG_SYNTH_RATE_HZ, ANALOG_SYNTH_RATE_HZ, data_bytes);
    if (n <= 0 || n >= ANALOG_SYNTH_JSON_MAX) {
        return -1;
    }
    cap->json_len = (size_t)n;
    return analog_sidecar_data_file((const char*)cap->json, &cap->name_at, &cap->name_len);
}

/* Makes the capture with pauses in memory (see above); 0, or -1 (what was made stays for analog_capture_free()). */
static int
analog_synth_capture(analog_capture* cap, uint64_t seed) {
    double total_s = ANALOG_SYNTH_LEAD_S + (ANALOG_SYNTH_BURSTS * ANALOG_SYNTH_BURST_S) + ANALOG_SYNTH_TAIL_S;
    for (int b = 0; b < ANALOG_SYNTH_BURSTS - 1; b++) {
        total_s += k_synth_pauses_s[b];
    }
    const size_t n = (size_t)lround(total_s * ANALOG_SYNTH_RATE_HZ);
    cap->data = (unsigned char*)malloc(2U * n);
    if (cap->data == NULL || analog_synth_sidecar(cap, 2U * n) != 0) {
        return -1;
    }
    cap->data_len = 2U * n;
    const double sigma = ANALOG_SYNTH_NOISE_LSB / ANALOG_CU8_MIDPOINT;
    const double amplitude = sqrt(2.0 * sigma * sigma * pow(10.0, ANALOG_SYNTH_CARRIER_DB / 10.0));
    uint64_t rng = seed;
    double phase = 0.0;
    for (size_t k = 0; k < n; k++) {
        const double t = (double)k / ANALOG_SYNTH_RATE_HZ;
        const double swell = 0.6 + (0.4 * sin(2.0 * ANALOG_PI * 3.0 * t));
        const double voice = (0.6 * sin(2.0 * ANALOG_PI * 700.0 * t)) + (0.4 * sin(2.0 * ANALOG_PI * 1300.0 * t));
        phase = fmod(phase + (2.0 * ANALOG_PI * ANALOG_SYNTH_DEVIATION_HZ * voice * swell / ANALOG_SYNTH_RATE_HZ),
                     2.0 * ANALOG_PI);
        const double carrier = amplitude * analog_synth_envelope(t);
        /* Box-Muller: one pair of independent Gaussians, one for I and one for Q. */
        const double r = sigma * sqrt(-2.0 * log(analog_synth_uniform(&rng)));
        const double theta = 2.0 * ANALOG_PI * analog_synth_uniform(&rng);
        cap->data[2U * k] = analog_synth_cu8((carrier * cos(phase)) + (r * cos(theta)));
        cap->data[(2U * k) + 1U] = analog_synth_cu8((carrier * sin(phase)) + (r * sin(theta)));
    }
    return 0;
}

/* Whether @p arg names an --iq-replay capture ("--iq-replay" or "--iq-replay=PATH", not "--iq-replay-rate"). */
static int
analog_is_replay_arg(const char* arg) {
    static const char k_flag[] = "--iq-replay";
    const size_t len = sizeof(k_flag) - 1U;
    return strncmp(arg, k_flag, len) == 0 && (arg[len] == '\0' || arg[len] == '=');
}

/* Makes the capture with pauses and replays it: adds `--iq-replay CAPTURE`, or with --analog-pcm-tap `-i TAP.wav` (args
   has room for two more); 0, or -1 with a message. */
static int
analog_replay_synth(char** args, int* count) {
    static char k_replay_flag[] = "--iq-replay";
    static char k_input_flag[] = "-i";
    for (int i = 1; i < *count; i++) {
        if (analog_is_replay_arg(args[i])) {
            DSD_FPRINTF(stderr, "analog replay: --analog-synth-pauses makes the capture; drop --iq-replay\n");
            return -1;
        }
    }
    const double seed = g_synth_pauses.value;
    if (fabs(seed - floor(seed)) > ANALOG_HZ_EPSILON) {
        DSD_FPRINTF(stderr, "analog replay: --analog-synth-pauses %g is not a whole number\n", seed);
        return -1;
    }
    analog_capture cap;
    DSD_MEMSET(&cap, 0, sizeof cap);
    int rc = -1;
    if (analog_synth_capture(&cap, (uint64_t)seed) != 0
        || dsd_test_mkdtemp(g_scaled_dir, sizeof(g_scaled_dir), "dsdneo_analog_synth") == NULL) {
        DSD_FPRINTF(stderr, "analog replay: --analog-synth-pauses cannot make its capture\n");
        g_scaled_dir[0] = '\0';
    } else if (g_pcm_tap_hz.set) {
        rc = analog_tap_write(&cap);
    } else {
        rc = analog_capture_write(&cap);
        if (rc != 0) {
            DSD_FPRINTF(stderr, "analog replay: --analog-synth-pauses cannot write its capture\n");
        }
    }
    analog_capture_free(&cap);
    if (rc == 0) {
        args[(*count)++] = g_pcm_tap_hz.set ? k_input_flag : k_replay_flag;
        args[(*count)++] = g_pcm_tap_hz.set ? g_tap_arg : g_scaled_meta;
        args[*count] = NULL;
    }
    return rc;
}

/* ---- a recorded stream gap (--analog-gap-join) ---------------------------------------------------------------- */

/* Issue #667 A3: two committed captures joined across a gap in the radio stream, as a capture records a USB recovery
 * (rtl_stream_note_discontinuity()): a RETUNE to the centre it was on, the samples the gap lost as a MUTE (left out of
 * the data), and the RESET whose usb_recovery reason replays as a stream gap. A fixed start after 2000-01-01, so the
 * decode clock reads the capture's time in every run. */
#define ANALOG_GAP_RATE_HZ   48000
#define ANALOG_GAP_CENTRE_HZ 851375000
#define ANALOG_GAP_JSON_MAX  2048

static int
analog_gap_sidecar(analog_capture* cap, size_t data_bytes, size_t gap_at, size_t gap_bytes) {
    cap->json = (unsigned char*)malloc(ANALOG_GAP_JSON_MAX);
    if (cap->json == NULL) {
        return -1;
    }
    const int n = DSD_SNPRINTF(
        (char*)cap->json, ANALOG_GAP_JSON_MAX,
        "{\n  \"format\": \"dsd-neo-iq\",\n  \"version\": 2,\n  \"sample_format\": \"cu8\",\n  \"iq_order\": \"IQ\",\n"
        "  \"endianness\": \"none\",\n  \"capture_stage\": \"post_mute_pre_widen\",\n  \"sample_rate_hz\": %d,\n"
        "  \"center_frequency_hz\": %d,\n  \"capture_center_frequency_hz\": %d,\n  \"ppm\": 0,\n"
        "  \"tuner_gain_tenth_db\": 270,\n  \"rtl_dsp_bw_khz\": 48,\n  \"base_decimation\": 1,\n"
        "  \"post_downsample\": 1,\n  \"demod_rate_hz\": %d,\n  \"offset_tuning_enabled\": false,\n"
        "  \"fs4_shift_enabled\": false,\n  \"combine_rotate_enabled\": false,\n  \"muted_bytes_excluded\": true,\n"
        "  \"contains_retunes\": true,\n  \"capture_retune_count\": 1,\n  \"source_backend\": \"rtl\",\n"
        "  \"source_args\": \"fixture\",\n  \"capture_started_utc\": \"2026-07-30T00:00:00Z\",\n"
        "  \"data_file\": \"" ANALOG_SCALED_DATA "\",\n  \"data_bytes\": %zu,\n  \"capture_drops\": 0,\n"
        "  \"capture_drop_blocks\": 0,\n  \"input_ring_drops\": 0,\n  \"notes\": \"made by the analog replay host\",\n"
        "  \"events\": [\n"
        "    {\"kind\": \"RETUNE\", \"byte_offset\": %zu, \"center_frequency_hz\": %d, "
        "\"capture_center_frequency_hz\": %d, \"sample_rate_hz\": %d, \"reason\": \"usb_recovery\"},\n"
        "    {\"kind\": \"MUTE\", \"byte_offset\": %zu, \"duration_bytes\": %zu, \"reason\": \"usb_recovery\"},\n"
        "    {\"kind\": \"RESET\", \"byte_offset\": %zu, \"center_frequency_hz\": %d, "
        "\"capture_center_frequency_hz\": %d, \"sample_rate_hz\": %d, \"reason\": \"usb_recovery\"}\n"
        "  ]\n}\n",
        ANALOG_GAP_RATE_HZ, ANALOG_GAP_CENTRE_HZ, ANALOG_GAP_CENTRE_HZ, ANALOG_GAP_RATE_HZ, data_bytes, gap_at,
        ANALOG_GAP_CENTRE_HZ, ANALOG_GAP_CENTRE_HZ, ANALOG_GAP_RATE_HZ, gap_at, gap_bytes, gap_at, ANALOG_GAP_CENTRE_HZ,
        ANALOG_GAP_CENTRE_HZ, ANALOG_GAP_RATE_HZ);
    if (n <= 0 || n >= ANALOG_GAP_JSON_MAX) {
        return -1;
    }
    cap->json_len = (size_t)n;
    return analog_sidecar_data_file((const char*)cap->json, &cap->name_at, &cap->name_len);
}

/* The cu8 bytes of @p ms at the capture rate. */
static size_t
analog_gap_bytes(long ms) {
    return (size_t)ms * (ANALOG_GAP_RATE_HZ / 1000U) * 2U;
}

/* Joins the first AT_MS of @p before and the first THEN_MS of @p after across the gap into @p out; 0, or -1. */
static int
analog_gap_join_captures(const analog_capture* before, const analog_capture* after, analog_capture* out) {
    const size_t at = analog_gap_bytes(g_gap_join.at_ms);
    const size_t then = analog_gap_bytes(g_gap_join.then_ms);
    if (analog_tap_capture_rate((const char*)before->json) != ANALOG_GAP_RATE_HZ
        || analog_tap_capture_rate((const char*)after->json) != ANALOG_GAP_RATE_HZ || at > before->data_len
        || then > after->data_len) {
        DSD_FPRINTF(stderr, "analog replay: %s joins %d Hz captures within their lengths\n", k_gap_join_arg,
                    ANALOG_GAP_RATE_HZ);
        return -1;
    }
    out->data = (unsigned char*)malloc(at + then);
    if (out->data == NULL) {
        return -1;
    }
    DSD_MEMCPY(out->data, before->data, at);
    DSD_MEMCPY(out->data + at, after->data, then);
    out->data_len = at + then;
    return analog_gap_sidecar(out, out->data_len, at, analog_gap_bytes(g_gap_join.gap_ms));
}

/* Writes the joined copy of the capture meta_path names (see above); 0, or -1 with a message. */
static int
analog_make_gap_copy(const char* meta_path) {
    char then_meta[DSD_TEST_PATH_MAX];
    analog_capture before;
    analog_capture after;
    analog_capture joined;
    DSD_MEMSET(&before, 0, sizeof before);
    DSD_MEMSET(&after, 0, sizeof after);
    DSD_MEMSET(&joined, 0, sizeof joined);
    int rc = -1;
    /* FIXTURE is looked up by its directory entry in tests/fixtures/iq, never used as a path. */
    char then_name[sizeof g_gap_join.then + 16];
    const int n = DSD_SNPRINTF(then_name, sizeof then_name, "%s.iq.json", g_gap_join.then);
    if (n <= 0 || (size_t)n >= sizeof then_name
        || analog_fixture_path(then_name, (size_t)n, then_meta, sizeof then_meta) != 0
        || analog_capture_load(meta_path, &before) != 0 || analog_capture_load(then_meta, &after) != 0
        || analog_gap_join_captures(&before, &after, &joined) != 0
        || dsd_test_mkdtemp(g_scaled_dir, sizeof(g_scaled_dir), "dsdneo_analog_gap") == NULL) {
        DSD_FPRINTF(stderr, "analog replay: %s cannot join '%s' and '%s'\n", k_gap_join_arg, meta_path,
                    g_gap_join.then);
        g_scaled_dir[0] = '\0';
    } else if (analog_capture_write(&joined) != 0) {
        DSD_FPRINTF(stderr, "analog replay: %s cannot write the joined copy\n", k_gap_join_arg);
    } else {
        rc = 0;
    }
    analog_capture_free(&joined);
    analog_capture_free(&after);
    analog_capture_free(&before);
    return rc;
}

/* Points the --iq-replay argument (as two arguments) at the joined copy of its capture; 0, or -1. */
static int
analog_replay_gap(char** args, int count) {
    for (int i = 1; i + 1 < count; i++) {
        if (strcmp(args[i], "--iq-replay") == 0) {
            if (analog_make_gap_copy(args[i + 1]) != 0) {
                return -1;
            }
            args[i + 1] = g_scaled_meta;
            return 0;
        }
    }
    DSD_FPRINTF(stderr, "analog replay: %s needs an --iq-replay capture (as two arguments)\n", k_gap_join_arg);
    return -1;
}

/* ---- scenes for where a recording starts (--analog-scene, issue #668) ------------------------------------------ */

/* Issue #668: where a per-call NFM recording starts against the transmission it holds. A scene is a few bursts of
 * voice-band noise (300-2500 Hz, made from a fixed seed, so no stretch of it repeats): any piece of a recording matches
 * one place in the scene and nowhere else, and the host finds each published recording there after the run (see the
 * recording oracle below). The kinds:
 *  - iq: the carrier 16 dB over complex noise at 3 LSB a component, as --analog-synth-pauses makes it, at up to 3 kHz
 *    deviation (2.5 kHz and a 600 Hz CTCSS tone when the scene has one), replayed as I/Q, or with --analog-pcm-tap as
 *    its discriminator tap. A receiver with its squelch open: noise between bursts.
 *  - pcm: the audio of a radio that squelches itself, at --analog-pcm-tap's rate: the voice at -20 dBFS RMS (the CTCSS
 *    tone left in at -26 dBFS), -60 dBFS of noise between bursts. What the level squelch is for.
 *  - udp: the pcm kind sent live to `-i udp` at --analog-pcm-tap's rate in 20 ms datagrams, paced in real time, with
 *    the sender stopping for a second before the sample pause_s names: a stream pause part-way through a monitor block.
 *  - udp-tap: the iq kind's discriminator tap sent the same way, for the noise squelch on a live input.
 * Every burst is keyed with 2 ms raised-cosine ramps. */
#define ANALOG_SCENE_MAX_BURSTS  8
#define ANALOG_SCENE_SEED        668U
#define ANALOG_SCENE_VOICE_LO_HZ 300.0
#define ANALOG_SCENE_VOICE_HI_HZ 2500.0
#define ANALOG_SCENE_CTCSS_HZ    100.0
#define ANALOG_SCENE_PCM_VOICE   0.1   /* RMS, full scale 1: -20 dBFS */
#define ANALOG_SCENE_PCM_CTCSS   0.05  /* peak: -26 dBFS */
#define ANALOG_SCENE_PCM_FLOOR   0.001 /* RMS: -60 dBFS */
#define ANALOG_SCENE_PCM_CLIP    0.9
#define ANALOG_SCENE_IQ_DEV_HZ   3000.0
#define ANALOG_SCENE_IQ_TONED_HZ 2500.0
#define ANALOG_SCENE_IQ_CTCSS_HZ 600.0
#define ANALOG_SCENE_UDP_STOP_MS 1000U
#define ANALOG_SCENE_UDP_END_MS  1500U
/* The udp scene's monitor blocks are 960 samples (symbol_analog_block_size()); at 8192 Hz each is 117 ms. */
#define ANALOG_SCENE_UDP_RATE    8192.0

typedef enum { ANALOG_SCENE_IQ = 0, ANALOG_SCENE_PCM, ANALOG_SCENE_UDP, ANALOG_SCENE_UDP_TAP } analog_scene_kind;

typedef struct {
    const char* name;
    double total_s; /* the scene's length */
    double pause_s; /* udp: the sender stops before the sample at this time; 0 for none */
    double onset_s[ANALOG_SCENE_MAX_BURSTS];
    double len_s[ANALOG_SCENE_MAX_BURSTS];
    analog_scene_kind kind;
    int ctcss; /* the bursts carry a 100.0 Hz CTCSS tone */
    int bursts;
    int old_bursts; /* how many bursts at the start no recording may hold: the oracle measures against the rest */
} analog_scene;

/* The udp scene in its own samples at 8192 Hz: nine 960-sample blocks of the floor, then 200 samples of a burst (the
   old source, part of the tenth block, which no recording may hold: B7, issue #668), the sender's stop, and the new
   source's burst from there on. */
#define ANALOG_SCENE_UDP_OLD_AT  (8640.0 / ANALOG_SCENE_UDP_RATE)
#define ANALOG_SCENE_UDP_STOP_AT (8840.0 / ANALOG_SCENE_UDP_RATE)

static const analog_scene k_scenes[] = {
    /* Four 1 s bursts 2 s apart, each starting at another point of a 20 ms block. */
    {
        .name = "onsets",
        .total_s = 8.6,
        .onset_s = {1.003, 3.008, 5.013, 7.017},
        .len_s = {1.0, 1.0, 1.0, 1.0},
        .kind = ANALOG_SCENE_IQ,
        .bursts = 4,
    },
    {
        .name = "onsets-ctcss",
        .total_s = 8.6,
        .onset_s = {1.003, 3.008, 5.013, 7.017},
        .len_s = {1.0, 1.0, 1.0, 1.0},
        .kind = ANALOG_SCENE_IQ,
        .ctcss = 1,
        .bursts = 4,
    },
    /* Five 0.6 s bursts 3.2 s apart, 160 noise-squelch windows at 44.1 kHz, so the squelch's own 20 ms grid falls on
       the 960-sample blocks the same way for each, give or take 0.5 ms more per burst: the squelch opens a few
       samples before a block ends, and its 5 ms ramp-in runs on into the next block (issue #668). */
    {
        .name = "onsets-late",
        .total_s = 19.2,
        .onset_s = {4.6355, 7.8360, 11.0365, 14.2370, 17.4375},
        .len_s = {0.6, 0.6, 0.6, 0.6, 0.6},
        .kind = ANALOG_SCENE_IQ,
        .bursts = 5,
    },
    /* Carriers a tone filter does not pass while they are on the air (B1, issue #668), then one it does. */
    {
        .name = "ctcss-short",
        .total_s = 7.4,
        .onset_s = {1.003, 2.508, 4.013, 5.517},
        .len_s = {0.26, 0.28, 0.30, 0.6},
        .kind = ANALOG_SCENE_IQ,
        .ctcss = 1,
        .bursts = 4,
    },
    {
        .name = "level-onsets",
        .total_s = 7.4,
        .onset_s = {1.010, 3.040, 5.070},
        .len_s = {1.0, 1.0, 1.0},
        .kind = ANALOG_SCENE_PCM,
        .bursts = 3,
    },
    /* The same at 8192 Hz, where a 960-sample block lasts 117 ms and the tone is passed sooner: 0.10 to 0.22 s. */
    {
        .name = "level-ctcss-short",
        .total_s = 9.2,
        .onset_s = {1.003, 2.008, 3.013, 4.017, 5.003, 6.008, 7.013, 8.017},
        .len_s = {0.10, 0.12, 0.14, 0.16, 0.18, 0.20, 0.22, 0.6},
        .kind = ANALOG_SCENE_PCM,
        .ctcss = 1,
        .bursts = 8,
    },
    /* 40 ms from 45 ms into the tenth 960-sample block at 8192 Hz (1.0547 s): the block's level is 4.7 dB under the
       burst's, and the tap read from 60 ms is all burst. */
    {
        .name = "level-blip",
        .total_s = 2.5,
        .onset_s = {1.0996875},
        .len_s = {0.040},
        .kind = ANALOG_SCENE_PCM,
        .bursts = 1,
    },
    {
        .name = "udp-pause",
        .total_s = 3.0,
        .pause_s = ANALOG_SCENE_UDP_STOP_AT,
        .onset_s = {ANALOG_SCENE_UDP_OLD_AT, ANALOG_SCENE_UDP_STOP_AT},
        .len_s = {ANALOG_SCENE_UDP_STOP_AT - ANALOG_SCENE_UDP_OLD_AT, 1.0},
        .kind = ANALOG_SCENE_UDP,
        .bursts = 2,
        .old_bursts = 1,
    },
    /* The noise squelch open when the stream stops (the burst runs to the stop), then noise alone (issue #668). */
    {
        .name = "udp-tap-pause",
        .total_s = 5.0,
        .pause_s = 2.503,
        .onset_s = {1.503},
        .len_s = {1.0},
        .kind = ANALOG_SCENE_UDP_TAP,
        .bursts = 1,
    },
};

/* The scene the run replays (--analog-scene) and what the recording oracle compares the recordings with. */
static struct {
    const analog_scene* scene;
    float* voice; /* the voice-band noise at 48 kHz, as long as the scene, RMS 1 */
    size_t n;     /* its samples */
    int16_t* pcm; /* udp: what the sender sends, at pcm_rate */
    size_t pcm_n;
    int pcm_rate;
    int port;
    char input_arg[64];
    char rate_arg[16];
    int feeding;
    dsd_thread_t feeder;
} g_scene;

static int
analog_apply_scene(const char* value) {
    for (size_t i = 0; i < sizeof(k_scenes) / sizeof(k_scenes[0]); i++) {
        if (strcmp(value, k_scenes[i].name) == 0) {
            g_scene.scene = &k_scenes[i];
            return 0;
        }
    }
    DSD_FPRINTF(stderr, "analog replay: --analog-scene %s is not a scene\n", value);
    return -1;
}

/* The scene's keying envelope at @p t seconds: 1 inside a burst, 0 between, 2 ms raised-cosine ramps at its edges. */
static double
analog_scene_envelope(const analog_scene* scene, double t) {
    for (int b = 0; b < scene->bursts; b++) {
        const double start = scene->onset_s[b];
        const double end = start + scene->len_s[b];
        if (t > start && t < end) {
            const double key = fmin(t - start, end - t) / ANALOG_SYNTH_RAMP_S;
            return 0.5 - (0.5 * cos(ANALOG_PI * fmin(key, 1.0)));
        }
    }
    return 0.0;
}

/* One standard Gaussian (Box-Muller, the cosine half). */
static double
analog_scene_gauss(uint64_t* rng) {
    const double r = sqrt(-2.0 * log(analog_synth_uniform(rng)));
    return r * cos(2.0 * ANALOG_PI * analog_synth_uniform(rng));
}

/* An RBJ biquad, low-pass (@p high_pass 0) or high-pass, Q 0.707, at @p hz for 48 kHz, run over @p x in place. */
static void
analog_scene_biquad(double* x, size_t n, double hz, int high_pass) {
    const double w0 = 2.0 * ANALOG_PI * hz / ANALOG_SYNTH_RATE_HZ;
    const double alpha = sin(w0) / (2.0 * 0.7071067811865476);
    const double c = cos(w0);
    const double a0 = 1.0 + alpha;
    const double b1 = high_pass ? -(1.0 + c) : 1.0 - c;
    const double b0 = high_pass ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
    for (size_t k = 0; k < n; k++) {
        const double y = ((b0 * x[k]) + (b1 * x1) + (b0 * x2) + (2.0 * c * y1) - ((1.0 - alpha) * y2)) / a0;
        x2 = x1;
        x1 = x[k];
        y2 = y1;
        y1 = y;
        x[k] = y;
    }
}

/* The scene's voice: Gaussian noise through two high-pass and two low-pass sections, scaled to RMS 1. 0, or -1. */
static int
analog_scene_make_voice(size_t n) {
    double* x = (double*)malloc(n * sizeof(double));
    g_scene.voice = (float*)malloc(n * sizeof(float));
    if (x == NULL || g_scene.voice == NULL) {
        free(x);
        return -1;
    }
    uint64_t rng = ANALOG_SCENE_SEED;
    for (size_t k = 0; k < n; k++) {
        x[k] = analog_scene_gauss(&rng);
    }
    for (int pass = 0; pass < 2; pass++) {
        analog_scene_biquad(x, n, ANALOG_SCENE_VOICE_LO_HZ, 1);
        analog_scene_biquad(x, n, ANALOG_SCENE_VOICE_HI_HZ, 0);
    }
    double power = 0.0;
    for (size_t k = 0; k < n; k++) {
        power += x[k] * x[k];
    }
    const double scale = power > 0.0 ? 1.0 / sqrt(power / (double)n) : 0.0;
    for (size_t k = 0; k < n; k++) {
        g_scene.voice[k] = (float)(x[k] * scale);
    }
    g_scene.n = n;
    free(x);
    return 0;
}

/* The iq kind as a capture: the FM carrier of the voice (and tone) in noise; 0, or -1. */
static int
analog_scene_capture(analog_capture* cap, const analog_scene* scene) {
    const size_t n = g_scene.n;
    cap->data = (unsigned char*)malloc(2U * n);
    if (cap->data == NULL || analog_synth_sidecar(cap, 2U * n) != 0) {
        return -1;
    }
    cap->data_len = 2U * n;
    const double sigma = ANALOG_SYNTH_NOISE_LSB / ANALOG_CU8_MIDPOINT;
    const double amplitude = sqrt(2.0 * sigma * sigma * pow(10.0, ANALOG_SYNTH_CARRIER_DB / 10.0));
    const double dev = scene->ctcss ? ANALOG_SCENE_IQ_TONED_HZ : ANALOG_SCENE_IQ_DEV_HZ;
    uint64_t rng = ANALOG_SCENE_SEED + 1U;
    double phase = 0.0;
    for (size_t k = 0; k < n; k++) {
        const double t = (double)k / ANALOG_SYNTH_RATE_HZ;
        /* The voice with its RMS at a third of the peak deviation, clipped at the peak, as a limiter would. */
        double hz = dev * fmax(-1.0, fmin(1.0, (double)g_scene.voice[k] / 3.0));
        if (scene->ctcss) {
            hz += ANALOG_SCENE_IQ_CTCSS_HZ * sin(2.0 * ANALOG_PI * ANALOG_SCENE_CTCSS_HZ * t);
        }
        phase = fmod(phase + (2.0 * ANALOG_PI * hz / ANALOG_SYNTH_RATE_HZ), 2.0 * ANALOG_PI);
        const double carrier = amplitude * analog_scene_envelope(scene, t);
        const double r = sigma * sqrt(-2.0 * log(analog_synth_uniform(&rng)));
        const double theta = 2.0 * ANALOG_PI * analog_synth_uniform(&rng);
        cap->data[2U * k] = analog_synth_cu8((carrier * cos(phase)) + (r * cos(theta)));
        cap->data[(2U * k) + 1U] = analog_synth_cu8((carrier * sin(phase)) + (r * sin(theta)));
    }
    return 0;
}

/* The pcm and udp kinds: the radio's audio at @p rate_hz in int16 (free() it), @p out_n samples; NULL on failure. */
static int16_t*
analog_scene_audio(const analog_scene* scene, long rate_hz, size_t* out_n) {
    const size_t n = g_scene.n;
    double* x = (double*)malloc(n * sizeof(double));
    if (x == NULL) {
        return NULL;
    }
    uint64_t rng = ANALOG_SCENE_SEED + 2U;
    for (size_t k = 0; k < n; k++) {
        const double t = (double)k / ANALOG_SYNTH_RATE_HZ;
        double v = ANALOG_SCENE_PCM_VOICE * (double)g_scene.voice[k];
        if (scene->ctcss) {
            v += ANALOG_SCENE_PCM_CTCSS * sin(2.0 * ANALOG_PI * ANALOG_SCENE_CTCSS_HZ * t);
        }
        v = fmax(-ANALOG_SCENE_PCM_CLIP, fmin(ANALOG_SCENE_PCM_CLIP, v));
        x[k] = (v * analog_scene_envelope(scene, t)) + (ANALOG_SCENE_PCM_FLOOR * analog_scene_gauss(&rng));
    }
    size_t m = n;
    if (rate_hz != ANALOG_SYNTH_RATE_HZ) {
        double* y = analog_tap_resample(x, n, ANALOG_SYNTH_RATE_HZ, rate_hz, &m);
        free(x);
        x = y;
    }
    int16_t* pcm = x != NULL ? (int16_t*)malloc(m * sizeof(int16_t)) : NULL;
    if (pcm != NULL) {
        for (size_t k = 0; k < m; k++) {
            pcm[k] = (int16_t)lround(fmax(-32768.0, fmin(32767.0, x[k] * 32767.0)));
        }
        *out_n = m;
    }
    free(x);
    return pcm;
}

/* Writes the pcm kind as the tap WAV the run takes as `-i`; 0, or -1. */
static int
analog_scene_write_audio(const analog_scene* scene, long rate_hz) {
    size_t m = 0U;
    int16_t* pcm = analog_scene_audio(scene, rate_hz, &m);
    int rc = -1;
    if (pcm != NULL && analog_tap_write_wav(pcm, m, (int)rate_hz) == 0
        && dsd_test_path_join(g_tap_arg, sizeof(g_tap_arg), g_scaled_dir, ANALOG_TAP_WAV) == 0) {
        g_tap_frames = (uint64_t)m;
        g_tap_rate_hz = (int)rate_hz;
        rc = 0;
    }
    free(pcm);
    return rc;
}

/* A loopback UDP port nothing is bound to now: bound and let go again. 0 when none could be had. */
static int
analog_scene_free_port(void) {
    struct sockaddr_in addr;
    if (dsd_socket_init() != 0 || dsd_socket_resolve("127.0.0.1", 0, &addr) != 0) {
        return 0;
    }
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_DGRAM, 0);
    if (sock == DSD_INVALID_SOCKET) {
        return 0;
    }
    int port = 0;
#if DSD_PLATFORM_WIN_NATIVE
    int len = (int)sizeof(addr);
#else
    socklen_t len = (socklen_t)sizeof(addr);
#endif
    if (dsd_socket_bind(sock, (const struct sockaddr*)&addr, (int)sizeof(addr)) == 0
        && getsockname(sock, (struct sockaddr*)&addr, &len) == 0) {
        port = (int)ntohs(addr.sin_port);
    }
    (void)dsd_socket_close(sock);
    return port;
}

/* The udp kinds: the audio the sender sends is in g_scene.pcm; a free port and the arguments for `-i udp`; 0, or -1. */
static int
analog_scene_listen(long rate_hz) {
    g_scene.pcm_rate = (int)rate_hz;
    g_scene.port = analog_scene_free_port();
    if (g_scene.pcm == NULL || g_scene.port <= 0) {
        return -1;
    }
    (void)DSD_SNPRINTF(g_scene.input_arg, sizeof(g_scene.input_arg), "udp:127.0.0.1:%d", g_scene.port);
    (void)DSD_SNPRINTF(g_scene.rate_arg, sizeof(g_scene.rate_arg), "%ld", rate_hz);
    return 0;
}

/* The udp-tap kind's audio: the tap WAV analog_tap_write() made, read back for the sender; 0, or -1. */
static int
analog_scene_read_tap(void) {
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* wav = sf_open(g_tap_arg, SFM_READ, &info);
    if (wav == NULL) {
        return -1;
    }
    int rc = -1;
    if (info.channels == 1 && info.frames > 0) {
        g_scene.pcm = (int16_t*)malloc((size_t)info.frames * sizeof(int16_t));
    }
    if (g_scene.pcm != NULL && sf_read_short(wav, g_scene.pcm, info.frames) == info.frames) {
        g_scene.pcm_n = (size_t)info.frames;
        rc = 0;
    }
    sf_close(wav);
    return rc;
}

/* Waits until @p due_ms on the real-time clock. */
static void
analog_scene_wait_until(uint64_t due_ms) {
    const uint64_t now = dsd_realtime_mono_ms();
    if (due_ms > now) {
        dsd_sleep_ms((unsigned int)(due_ms - now));
    }
}

/* The udp scene's sender: 20 ms datagrams, each sent when its first sample is due, the stop before the sample at
   pause_s, and then, once the decoder had time to read the last of it, the end of the run. */
static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    analog_scene_feed(void* arg) {
    (void)arg;
    struct sockaddr_in to;
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_DGRAM, 0);
    if (sock != DSD_INVALID_SOCKET && dsd_socket_resolve("127.0.0.1", g_scene.port, &to) == 0) {
        const size_t chunk = (size_t)g_scene.pcm_rate / 50U;
        const size_t stop_at = (size_t)lround(g_scene.scene->pause_s * (double)g_scene.pcm_rate);
        const uint64_t began = dsd_realtime_mono_ms();
        size_t at = 0U;
        while (at < g_scene.pcm_n) {
            size_t take = g_scene.pcm_n - at < chunk ? g_scene.pcm_n - at : chunk;
            if (at < stop_at && at + take > stop_at) {
                take = stop_at - at;
            }
            const uint64_t stopped = (stop_at > 0U && at >= stop_at) ? ANALOG_SCENE_UDP_STOP_MS : 0U;
            analog_scene_wait_until(began + stopped + (uint64_t)at * 1000U / (uint64_t)g_scene.pcm_rate);
            (void)dsd_socket_sendto(sock, g_scene.pcm + at, take * sizeof(int16_t), 0, (const struct sockaddr*)&to,
                                    (int)sizeof(to));
            at += take;
        }
        dsd_sleep_ms(ANALOG_SCENE_UDP_END_MS);
    }
    if (sock != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(sock);
    }
    dsd_exitflag_store(1);
    DSD_THREAD_RETURN;
}

/* Makes the scene and replaces the input with it: `--iq-replay CAPTURE`, `-i TAP.wav`, or `-i udp:... -s RATE` (args
   has room for four more); 0, or -1 with a message. */
static int
analog_replay_scene(char** args, int* count) {
    static char k_replay_flag[] = "--iq-replay";
    static char k_input_flag[] = "-i";
    static char k_rate_flag[] = "-s";
    const analog_scene* scene = g_scene.scene;
    const long rate_hz = g_pcm_tap_hz.set ? (long)g_pcm_tap_hz.value : 0L;
    if (scene->kind != ANALOG_SCENE_IQ && rate_hz <= 0) {
        DSD_FPRINTF(stderr, "analog replay: --analog-scene %s needs --analog-pcm-tap RATE\n", scene->name);
        return -1;
    }
    for (int i = 1; i < *count; i++) {
        if (analog_is_replay_arg(args[i]) || strcmp(args[i], "-i") == 0) {
            DSD_FPRINTF(stderr, "analog replay: --analog-scene makes the input; drop %s\n", args[i]);
            return -1;
        }
    }
    analog_capture cap;
    DSD_MEMSET(&cap, 0, sizeof cap);
    int rc = -1;
    if (analog_scene_make_voice((size_t)lround(scene->total_s * ANALOG_SYNTH_RATE_HZ)) != 0
        || dsd_test_mkdtemp(g_scaled_dir, sizeof(g_scaled_dir), "dsdneo_analog_scene") == NULL) {
        g_scaled_dir[0] = '\0';
    } else if (scene->kind == ANALOG_SCENE_UDP) {
        g_scene.pcm = analog_scene_audio(scene, rate_hz, &g_scene.pcm_n);
        rc = analog_scene_listen(rate_hz);
    } else if (scene->kind == ANALOG_SCENE_UDP_TAP) {
        rc = (analog_scene_capture(&cap, scene) == 0 && analog_tap_write(&cap) == 0 && analog_scene_read_tap() == 0)
                 ? analog_scene_listen(rate_hz)
                 : -1;
    } else if (scene->kind == ANALOG_SCENE_PCM) {
        rc = analog_scene_write_audio(scene, rate_hz);
    } else if (analog_scene_capture(&cap, scene) == 0) {
        rc = rate_hz > 0 ? analog_tap_write(&cap) : analog_capture_write(&cap);
    }
    analog_capture_free(&cap);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "analog replay: --analog-scene %s cannot make its input\n", scene->name);
        return -1;
    }
    if (scene->kind == ANALOG_SCENE_UDP || scene->kind == ANALOG_SCENE_UDP_TAP) {
        args[(*count)++] = k_input_flag;
        args[(*count)++] = g_scene.input_arg;
        args[(*count)++] = k_rate_flag;
        args[(*count)++] = g_scene.rate_arg;
    } else {
        args[(*count)++] = scene->kind == ANALOG_SCENE_IQ && rate_hz <= 0 ? k_replay_flag : k_input_flag;
        args[(*count)++] = scene->kind == ANALOG_SCENE_IQ && rate_hz <= 0 ? g_scaled_meta : g_tap_arg;
    }
    args[*count] = NULL;
    return 0;
}

static void
analog_scene_free(void) {
    free(g_scene.voice);
    free(g_scene.pcm);
    g_scene.voice = NULL;
    g_scene.pcm = NULL;
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

/* A frequency the delivered audio can represent; anything at or above Nyquist would alias and is left unmeasured. The
   audio arrives at the analog sink rate, whatever rate the monitor runs at (issue #633). */
static int
analog_measurable(double hz) {
    return g_totals.deliver_hz > 0U && hz < 0.5 * (double)g_totals.deliver_hz;
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
        g_totals.last_audible_ms = block_start_ms + block_ms;
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
    double rate = (double)g_totals.deliver_hz;
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
 * signal's other supported spelling, "D023N/D047I". A publication that names no supported signal leaves the last
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

/* The received-tone publication as one delivered block found it: the tap updated it from this same block just before
 * the block came here. */
typedef struct {
    int locked;
    int kind;
    int ctcss_tenths_hz;
    int dcs_code;
    int dcs_inverted;
} analog_tone_snapshot;

static analog_tone_snapshot
analog_take_tone_snapshot(const dsd_state* state) {
    analog_tone_snapshot snap;
    DSD_MEMSET(&snap, 0, sizeof(snap));
    if (state != NULL && state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED) {
        snap.locked = 1;
        snap.kind = (int)state->analog_rx.tone_kind;
        snap.ctcss_tenths_hz = (int)state->analog_rx.ctcss_tenths_hz;
        snap.dcs_code = (int)state->analog_rx.dcs_code;
        snap.dcs_inverted = (int)state->analog_rx.dcs_inverted;
    }
    return snap;
}

/* Applies the publication after one delivered block, which ended at block_end_ms. */
static void
analog_note_tone(const analog_tone_snapshot* snap, double block_end_ms, double block_ms) {
    if (!snap->locked) {
        return;
    }
    g_tone.locked_ms += block_ms;
    if (g_tone.first_lock_ms < 0.0) {
        g_tone.first_lock_ms = block_end_ms;
    }
    if (snap->kind == (int)DSD_ANALOG_TONE_KIND_CTCSS) {
        (void)dsd_ctcss_format(snap->ctcss_tenths_hz, g_tone.label, sizeof(g_tone.label));
    } else if (snap->kind == (int)DSD_ANALOG_TONE_KIND_DCS) {
        analog_format_dcs_label(snap->dcs_code, snap->dcs_inverted);
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

/* One monitor block, as its writes arrive: the monitor hands a block to the sink in writes of at most 960 samples at the
 * analog sink rate (issue #633), all at one stream position, since the decoder reads no input between them. The block
 * is scored once, whole, so the spectral windows and stream times do not depend on how it was cut into writes. */
#define ANALOG_PENDING_MAX 65536

static struct {
    int active;
    double consumed_ms; /* stream time when its first write came */
    size_t n;
    analog_tone_snapshot tone;
    short pcm[ANALOG_PENDING_MAX];
} g_pending;

/* Scores the pending block: it ends at the stream time its writes came at and spans as much stream time as its audio
   lasts at the sink rate. */
static void
analog_flush_block(void) {
    if (!g_pending.active) {
        return;
    }
    g_pending.active = 0;
    if (g_pending.n == 0U || g_totals.deliver_hz == 0U) {
        return;
    }
    const double ms_per_sample = 1000.0 / (double)g_totals.deliver_hz;
    const double block_ms = (double)g_pending.n * ms_per_sample;
    const double block_start_ms = g_pending.consumed_ms > block_ms ? g_pending.consumed_ms - block_ms : 0.0;
    static double block[ANALOG_MAX_BLOCK];
    size_t done = 0U;
    while (done < g_pending.n) {
        size_t chunk = g_pending.n - done < ANALOG_MAX_BLOCK ? g_pending.n - done : ANALOG_MAX_BLOCK;
        for (size_t i = 0; i < chunk; i++) {
            block[i] = (double)g_pending.pcm[done + i] / ANALOG_FULL_SCALE;
        }
        analog_score_block(block, chunk, block_start_ms + ((double)done * ms_per_sample));
        done += chunk;
    }
    g_totals.blocks_scored++;
    analog_note_tone(&g_pending.tone, block_start_ms + block_ms, block_ms);
}

/* One write of @p n samples at stream time @p consumed_ms: it starts a new block when the stream moved since the last
   write, which scores the block before it, and joins the block in hand otherwise. @p native_samples is the input a new
   block spans (0 when unknown). */
static void
analog_take_write(double consumed_ms, const short* pcm, size_t n, const analog_tone_snapshot* tone,
                  uint64_t native_samples) {
    if (!g_pending.active || fabs(consumed_ms - g_pending.consumed_ms) > ANALOG_HZ_EPSILON) {
        analog_flush_block();
        g_pending.active = 1;
        g_pending.consumed_ms = consumed_ms;
        g_pending.n = 0U;
        g_pending.tone = *tone;
        g_totals.native_scored += native_samples;
    }
    const size_t room = ANALOG_PENDING_MAX - g_pending.n;
    const size_t take = n < room ? n : room;
    DSD_MEMCPY(g_pending.pcm + g_pending.n, pcm, take * sizeof(short));
    g_pending.n += take;
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
    /* The monitor's rate, the stream's clock: the RTL output rate, or on audio input its own (staged) rate. Its audio
       arrives at the analog sink rate (issue #633). */
    unsigned int rate_hz = pcm_input ? (unsigned int)dsd_opts_current_input_timing_rate(opts)
                                     : dsd_rtl_stream_metrics_hook_output_rate_hz();
    if (rate_hz == 0U) {
        g_totals.no_rate = 1;
        return;
    }
    analog_note_rate(rate_hz);
    g_totals.deliver_hz = (unsigned int)dsd_opts_analog_sink_rate_hz(opts);
    const analog_tone_snapshot tone = analog_take_tone_snapshot(state);
    /* On audio input every block spans 960 input samples (symbol_analog_block_size()). */
    analog_take_write(analog_consumed_ms(), (const short*)data, n, &tone, pcm_input ? 960U : 0U);
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

/* Runs after the engine installed its hooks and opened audio output, so these replacements stick. With
   --analog-keep-output the session's own output stays, and the capture only shows that nothing reached it. A udp
   scene's sender starts here, once the input listens. */
static int
analog_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)state;
    (void)context;
    if (!g_keep_output) {
        opts->audio_out = 1;
        opts->audio_out_type = 8;
    }
    if (g_scene.pcm != NULL && !g_scene.feeding) {
        if (dsd_thread_create(&g_scene.feeder, analog_scene_feed, NULL) != 0) {
            DSD_FPRINTF(stderr, "analog replay: cannot start the udp scene's sender\n");
            return -1;
        }
        g_scene.feeding = 1;
    }
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

/* ---- raw WAV (--analog-raw-wav-min-rms-dbfs) ------------------------------------------------------------------ */

#define ANALOG_RAW_WAV_CHUNK 4096

/* The private file the run's -6 raw WAV goes to, "" when none was made. */
static char g_raw_wav_path[DSD_TEST_PATH_MAX];

/* What the raw WAV held when live processing ended. */
static struct {
    int have; /* it was read and holds frames */
    long long frames;
    int rate_hz;
    double rms_dbfs;
    double peak_dbfs;
    uint64_t clip_count;
} g_raw_wav;

/* Makes the private file and adds `-6 FILE` to the dsd-neo arguments (args has room for two more); 0, or -1. */
static int
analog_raw_wav_add(char** args, int* count) {
    static char k_raw_wav_flag[] = "-6";
    const int fd = dsd_test_mkstemp(g_raw_wav_path, sizeof(g_raw_wav_path), "dsdneo_analog_raw");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "analog replay: --analog-raw-wav-min-rms-dbfs cannot make its raw WAV file\n");
        g_raw_wav_path[0] = '\0';
        return -1;
    }
    (void)dsd_close(fd);
    args[(*count)++] = k_raw_wav_flag;
    args[(*count)++] = g_raw_wav_path;
    args[*count] = NULL;
    return 0;
}

/* Reads the raw WAV back. The engine closes it only after the stop hook, so the host closes it here first, which writes
   its final header; the engine's cleanup then finds it closed. */
static void
analog_raw_wav_measure(dsd_opts* opts) {
    if (opts->wav_out_raw != NULL) {
        sf_close(opts->wav_out_raw);
        opts->wav_out_raw = NULL;
    }
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* wav = sf_open(g_raw_wav_path, SFM_READ, &info);
    if (wav == NULL) {
        return;
    }
    static short pcm[ANALOG_RAW_WAV_CHUNK];
    double sum_sq = 0.0;
    double peak = 0.0;
    long long frames = 0;
    sf_count_t got = 0;
    while ((got = sf_read_short(wav, pcm, ANALOG_RAW_WAV_CHUNK)) > 0) {
        for (sf_count_t i = 0; i < got; i++) {
            const double x = (double)pcm[i] / ANALOG_FULL_SCALE;
            sum_sq += x * x;
            peak = fabs(x) > peak ? fabs(x) : peak;
            if (pcm[i] >= 32767 || pcm[i] <= -32768) {
                g_raw_wav.clip_count++;
            }
        }
        frames += (long long)got;
    }
    sf_close(wav);
    g_raw_wav.frames = frames;
    g_raw_wav.rate_hz = info.samplerate;
    g_raw_wav.have = frames > 0 && info.channels == 1;
    g_raw_wav.rms_dbfs = frames > 0 ? analog_db(sum_sq / (double)frames) : 0.0;
    g_raw_wav.peak_dbfs = analog_db(peak * peak);
}

static void
analog_raw_wav_remove(void) {
    if (g_raw_wav_path[0] != '\0' && remove(g_raw_wav_path) != 0) {
        DSD_FPRINTF(stderr, "analog replay: could not remove %s\n", g_raw_wav_path);
    }
}

/* ---- the recording oracle (--analog-scene with -P, issue #668) -------------------------------------------------- */

/* After the run, every recording the journal published is found in the scene: 50 ms pieces of it, its loudest one and
 * then from its middle outwards in tenths of its length, are compared with the voice around each burst (normalised
 * correlation, sign ignored, on every sixth sample first, then sample by sample around the best match) until one
 * matches well, and the best match says which burst the recording holds and where its first sample was in the scene.
 * The host prints one line per recording, in publication order:
 *   ANALOG RECORDING: n=N burst=B score=S start_ms=X audio_ms=Y ms=L
 * start_ms is where the recording's first sample was against the burst's start (negative: before it), audio_ms the
 * same for its first sample that is not 0 (the squelch writes 0), ms its length. A scene's old bursts are no burst
 * of their own: a recording that holds one starts before the burst after it. A recording that matches no burst
 * (score under 0.5) fails, and so does one outside the --analog-record-* bounds. The CTCSS tone is not in the voice the
 * recording is compared with, and the voice filters take it out of the recording. */
#define ANALOG_ORACLE_RATE         48000
#define ANALOG_ORACLE_STEP         6
#define ANALOG_ORACLE_WINDOW       2400 /* 50 ms */
#define ANALOG_ORACLE_BEFORE_S     0.6  /* how far before a burst's start a recording may begin */
#define ANALOG_ORACLE_AFTER_S      0.3  /* ... and its matched piece end after the burst does */
#define ANALOG_ORACLE_MIN_SCORE    0.5
#define ANALOG_ORACLE_GOOD_SCORE   0.7 /* a match this good ends the search */
#define ANALOG_ORACLE_JOURNAL_MAX  (1L << 22)
#define ANALOG_ORACLE_LEVEL_WINDOW 120 /* 2.5 ms */
#define ANALOG_ORACLE_LEVEL_SPAN_S 0.3
#define ANALOG_ORACLE_LEVEL_MAX    120 /* windows in the span */

typedef struct {
    double score;
    int burst;
    long start; /* the recording's first sample in the scene, at 48 kHz */
} analog_match;

/* |normalised correlation| of x[0, n) with the voice from @p at, every @p step-th sample. */
static double
analog_oracle_score(const short* x, size_t n, long at, size_t step) {
    double xy = 0.0;
    double xx = 0.0;
    double yy = 0.0;
    for (size_t i = 0; i < n; i += step) {
        const double a = (double)x[i];
        const double b = (double)g_scene.voice[(size_t)at + i];
        xy += a * b;
        xx += a * a;
        yy += b * b;
    }
    return (xx > 0.0 && yy > 0.0) ? fabs(xy) / sqrt(xx * yy) : 0.0;
}

/* The best place for the piece @p x (ANALOG_ORACLE_WINDOW samples, @p offset into its recording) in burst @p b. */
static void
analog_oracle_search(const short* x, long offset, int b, analog_match* best) {
    const analog_scene* scene = g_scene.scene;
    const long lo = lround((scene->onset_s[b] - ANALOG_ORACLE_BEFORE_S) * ANALOG_ORACLE_RATE);
    long hi = lround((scene->onset_s[b] + scene->len_s[b] + ANALOG_ORACLE_AFTER_S) * ANALOG_ORACLE_RATE);
    hi = hi < (long)g_scene.n - ANALOG_ORACLE_WINDOW ? hi : (long)g_scene.n - ANALOG_ORACLE_WINDOW;
    long found = -1;
    double found_score = 0.0;
    for (long at = lo > 0 ? lo : 0; at < hi; at += ANALOG_ORACLE_STEP) {
        const double s = analog_oracle_score(x, ANALOG_ORACLE_WINDOW, at, ANALOG_ORACLE_STEP);
        if (s > found_score) {
            found_score = s;
            found = at;
        }
    }
    for (long at = found - ANALOG_ORACLE_STEP; found >= 0 && at <= found + ANALOG_ORACLE_STEP; at++) {
        const double s = (at >= 0 && at < hi) ? analog_oracle_score(x, ANALOG_ORACLE_WINDOW, at, 1U) : 0.0;
        if (s > best->score) {
            best->score = s;
            best->burst = b;
            best->start = at - offset;
        }
    }
}

static void
analog_oracle_check(const char* what, int n, double got, const analog_limit* min, const analog_limit* max) {
    if ((min->set && got < min->value) || (max->set && got > max->value)) {
        DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: recording %d %s %.2f ms (limits %.2f to %.2f)\n", n, what, got,
                    min->set ? min->value : -1e9, max->set ? max->value : 1e9);
        g_failed = 1;
    }
}

/* Where the loudest piece of the recording starts, in steps of a tenth of a piece: a short burst among silence. */
static long
analog_oracle_loudest(const short* x, size_t frames) {
    const size_t step = ANALOG_ORACLE_WINDOW / 10U;
    double energy = 0.0;
    for (size_t i = 0; i < ANALOG_ORACLE_WINDOW; i++) {
        energy += (double)x[i] * (double)x[i];
    }
    double loudest = energy;
    long at = 0;
    for (size_t from = step; from + ANALOG_ORACLE_WINDOW <= frames; from += step) {
        for (size_t i = from - step; i < from; i++) {
            const double in = (double)x[i + ANALOG_ORACLE_WINDOW];
            energy += (in * in) - ((double)x[i] * (double)x[i]);
        }
        if (energy > loudest) {
            loudest = energy;
            at = (long)from;
        }
    }
    return at;
}

static int
analog_oracle_compare_double(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

/* How far the recording's level dips over its burst's first 300 ms (issue #668): in 2.5 ms windows from the burst's
   second window on (or the recording's first after it), the RMS of the recording over the RMS of the voice it holds
   there, the smallest of them against their median. About 1 for a recording that keeps its level; a sink-gate ramp
   written into it after a start written as heard takes a window to about 0.3. 1 with no window to compare. */
static double
analog_oracle_level_floor(const short* x, size_t frames, long start, long onset) {
    double ratio[ANALOG_ORACLE_LEVEL_MAX];
    int n = 0;
    long from = onset + ANALOG_ORACLE_LEVEL_WINDOW - start;
    from = from > 0 ? from : 0;
    const long to = onset + lround(ANALOG_ORACLE_LEVEL_SPAN_S * ANALOG_ORACLE_RATE) - start;
    for (long k = from; k + ANALOG_ORACLE_LEVEL_WINDOW <= to && k + ANALOG_ORACLE_LEVEL_WINDOW <= (long)frames
                        && start + k + ANALOG_ORACLE_LEVEL_WINDOW <= (long)g_scene.n && n < ANALOG_ORACLE_LEVEL_MAX;
         k += ANALOG_ORACLE_LEVEL_WINDOW) {
        double xx = 0.0;
        double vv = 0.0;
        for (long i = k; i < k + ANALOG_ORACLE_LEVEL_WINDOW; i++) {
            const double v = (double)g_scene.voice[start + i];
            xx += (double)x[i] * (double)x[i];
            vv += v * v;
        }
        if (vv > 0.0) {
            ratio[n++] = sqrt(xx / vv);
        }
    }
    if (n == 0) {
        return 1.0;
    }
    double low = ratio[0];
    for (int i = 1; i < n; i++) {
        low = ratio[i] < low ? ratio[i] : low;
    }
    qsort(ratio, (size_t)n, sizeof(ratio[0]), analog_oracle_compare_double);
    const double median = ratio[n / 2];
    return median > 0.0 ? low / median : 0.0;
}

/* Finds recording @p n (@p frames samples at 48 kHz) in the scene, prints its line and checks its bounds. */
static void
analog_oracle_recording(int n, const short* x, size_t frames) {
    /* The loudest piece (-1), then the middle, 40%, 60%, 30%, 70% ... of the recording. */
    static const int k_tenths[] = {-1, 5, 4, 6, 3, 7, 2, 8, 1, 9};
    analog_match best = {0.0, -1, 0};
    for (size_t i = 0; i < sizeof(k_tenths) / sizeof(k_tenths[0]) && frames >= (size_t)ANALOG_ORACLE_WINDOW * 2U
                       && best.score < ANALOG_ORACLE_GOOD_SCORE;
         i++) {
        long offset = k_tenths[i] < 0 ? analog_oracle_loudest(x, frames)
                                      : (long)(frames * (size_t)k_tenths[i] / 10U) - (ANALOG_ORACLE_WINDOW / 2);
        offset = offset + ANALOG_ORACLE_WINDOW > (long)frames ? (long)frames - ANALOG_ORACLE_WINDOW : offset;
        offset = offset < 0 ? 0 : offset;
        for (int b = g_scene.scene->old_bursts; b < g_scene.scene->bursts; b++) {
            analog_oracle_search(x + offset, offset, b, &best);
        }
    }
    size_t first = 0U;
    while (first < frames && x[first] == 0) {
        first++;
    }
    if (best.burst < 0 || best.score < ANALOG_ORACLE_MIN_SCORE) {
        DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: recording %d (%zu frames) matches no burst (score %.2f)\n", n, frames,
                    best.score);
        g_failed = 1;
        return;
    }
    const long onset = lround(g_scene.scene->onset_s[best.burst] * ANALOG_ORACLE_RATE);
    const double start_ms = (double)(best.start - onset) * 1000.0 / ANALOG_ORACLE_RATE;
    const double audio_ms = (double)(best.start + (long)first - onset) * 1000.0 / ANALOG_ORACLE_RATE;
    DSD_FPRINTF(stderr, "ANALOG RECORDING: n=%d burst=%d score=%.2f start_ms=%.2f audio_ms=%.2f ms=%.2f\n", n,
                best.burst, best.score, start_ms, audio_ms, (double)frames * 1000.0 / ANALOG_ORACLE_RATE);
    analog_oracle_check("starts at", n, start_ms, &g_record_min_start_ms, &g_record_max_start_ms);
    analog_oracle_check("has audio from", n, audio_ms, &g_record_min_audio_ms, &g_record_max_audio_ms);
    if (g_record_min_level.set) {
        const double level = analog_oracle_level_floor(x, frames, best.start, onset);
        DSD_FPRINTF(stderr, "ANALOG LEVEL: n=%d min=%.3f\n", n, level);
        if (level < g_record_min_level.value) {
            DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: recording %d dips to %.3f of its level (limit %.3f)\n", n, level,
                        g_record_min_level.value);
            g_failed = 1;
        }
    }
}

/* Reads recording @p n, the file @p name in @p folder, and checks it; 0, or -1 when it cannot be read as a mono 48 kHz
   WAV. */
static int
analog_oracle_file(int n, const char* folder, const char* name) {
    char path[DSD_TEST_PATH_MAX];
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* wav = dsd_test_path_join(path, sizeof(path), folder, name) == 0 ? sf_open(path, SFM_READ, &info) : NULL;
    if (wav == NULL) {
        return -1;
    }
    int rc = -1;
    short* x = NULL;
    if (info.channels == 1 && info.samplerate == ANALOG_ORACLE_RATE && info.frames > 0) {
        x = (short*)malloc((size_t)info.frames * sizeof(short));
    }
    if (x != NULL && sf_read_short(wav, x, info.frames) == info.frames) {
        analog_oracle_recording(n, x, (size_t)info.frames);
        rc = 0;
    }
    free(x);
    sf_close(wav);
    return rc;
}

/* The file name a journal "publish" line names, past any folder ('/' or a JSON-escaped '\'), into @p out; 0, or -1. */
static int
analog_oracle_published_name(const char* line, size_t len, char* out, size_t cap) {
    static const char k_key[] = "\"file\":\"";
    const char* end = line + len;
    const char* at = NULL;
    for (const char* p = line; p + sizeof(k_key) - 1U <= end; p++) {
        if (strncmp(p, k_key, sizeof(k_key) - 1U) == 0) {
            at = p + sizeof(k_key) - 1U;
            break;
        }
    }
    if (at == NULL) {
        return -1;
    }
    const char* name = at;
    const char* p = at;
    while (p < end && *p != '"') {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
        p += (*p == '\\' && p + 1 < end) ? 2 : 1;
    }
    const size_t name_len = (size_t)(p - name);
    if (p >= end || name_len == 0U || name_len >= cap) {
        return -1;
    }
    DSD_MEMCPY(out, name, name_len);
    out[name_len] = '\0';
    return 0;
}

/* After the run: every recording the journal published, in publication order (see above). */
static void
analog_oracle_run(const dsd_opts* opts) {
    if (g_scene.scene == NULL || !opts->dmr_stereo_wav) {
        return;
    }
    size_t len = 0U;
    char* journal = opts->wav_journal_file[0] != '\0'
                        ? (char*)analog_read_file(opts->wav_journal_file, ANALOG_ORACLE_JOURNAL_MAX, &len)
                        : NULL;
    int n = 0;
    for (const char* line = journal; line != NULL && *line != '\0';) {
        const char* eol = strchr(line, '\n');
        const size_t line_len = eol != NULL ? (size_t)(eol - line) : strlen(line);
        char name[DSD_TEST_PATH_MAX];
        const char* publish = strstr(line, "\"type\":\"publish\"");
        const int published = publish != NULL && publish < line + line_len;
        n += published;
        if (published
            && (analog_oracle_published_name(line, line_len, name, sizeof(name)) != 0
                || analog_oracle_file(n, opts->wav_out_dir, name) != 0)) {
            DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: cannot read recording %d of the journal\n", n);
            g_failed = 1;
        }
        line = eol != NULL ? eol + 1 : NULL;
    }
    free(journal);
    DSD_FPRINTF(stderr, "ANALOG RECORDINGS: %d\n", n);
}

/* The verdict of a scene's run, after its recordings were checked: only when the stop hook reported (analog_report()). */
static void
analog_report_scene(void) {
    if (g_scene.scene != NULL && g_reported && !g_failed) {
        DSD_FPRINTF(stderr, "ANALOG AUDIO OK\n");
    }
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
    int have_last;
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
    ctx->have_last = g_totals.last_audible_ms >= 0.0;
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
    analog_print_value(line, sizeof(line), "last_audible_ms", ctx->have_last, g_totals.last_audible_ms);
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

/* --analog-keep-output: the session's output as the engine left it, which must be muted with nothing played. */
static void
analog_check_kept_output(const dsd_opts* opts) {
    if (!g_keep_output) {
        return;
    }
    DSD_FPRINTF(stderr, "ANALOG OUTPUT: audio_out=%d type=%d played=%llu\n", opts->audio_out, opts->audio_out_type,
                (unsigned long long)g_totals.samples_captured);
    if (opts->audio_out != 0 || g_totals.samples_captured != 0U) {
        DSD_FPRINTF(stderr, "ANALOG AUDIO FAIL: the session's output is not muted\n");
        g_failed = 1;
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
    analog_check_min("last audible ms", ctx->have_last, g_totals.last_audible_ms, &g_limits.min_last_audible_ms);
    analog_check_max("last audible ms", ctx->have_last, g_totals.last_audible_ms, &g_limits.max_last_audible_ms);
    analog_check_min("in-band ratio dB", ctx->have_audio, ctx->inband_db, &g_limits.min_inband_db);
    analog_check_min("RMS dBFS", ctx->have_audio, ctx->rms_dbfs, &g_limits.min_rms_dbfs);
    analog_check_max("RMS dBFS", ctx->have_audio, ctx->rms_dbfs, &g_limits.max_rms_dbfs);
    analog_check_max("peak dBFS", ctx->have_audio, ctx->peak_dbfs, &g_limits.max_peak_dbfs);
    analog_check_max("clipped samples", 1, (double)g_totals.clip_count, &g_limits.max_clip);
    analog_check_max("tone lock ms", ctx->have_lock, g_tone.first_lock_ms, &g_limits.max_tone_lock_ms);
    /* Every block's audio at the sink rate lasts as long as the input it spans (issue #633): measured on audio input,
       where each block spans 960 input samples at the monitor's rate. */
    const int have_delivery = g_delivery_pcm && g_totals.native_scored > 0U && g_totals.rate_hz > 0U;
    const double want_samples =
        have_delivery ? (double)g_totals.native_scored * (double)g_totals.deliver_hz / (double)g_totals.rate_hz : 0.0;
    const double delivery_error_pct =
        have_delivery ? 100.0 * fabs((double)g_totals.samples_captured - want_samples) / want_samples : 0.0;
    analog_check_max("delivery error pct", have_delivery, delivery_error_pct, &g_limits.max_delivery_error_pct);
    analog_check_min("raw WAV RMS dBFS", g_raw_wav.have, g_raw_wav.rms_dbfs, &g_raw_wav_min_rms_dbfs);
    analog_report_probes(ctx, 1);
}

/* The ANALOG RAW WAV line, when the run recorded one (--analog-raw-wav-min-rms-dbfs). */
static void
analog_print_raw_wav_line(void) {
    if (!g_raw_wav_min_rms_dbfs.set) {
        return;
    }
    char line[256];
    DSD_SNPRINTF(line, sizeof(line), "ANALOG RAW WAV: frames=%lld rate_hz=%d", g_raw_wav.frames, g_raw_wav.rate_hz);
    analog_print_value(line, sizeof(line), "rms_dbfs", g_raw_wav.have, g_raw_wav.rms_dbfs);
    analog_print_value(line, sizeof(line), "peak_dbfs", g_raw_wav.have, g_raw_wav.peak_dbfs);
    const size_t used = strlen(line);
    DSD_SNPRINTF(line + used, sizeof(line) - used, " clip=%llu", (unsigned long long)g_raw_wav.clip_count);
    DSD_FPRINTF(stderr, "%s\n", line);
}

static void
analog_report(void) {
    analog_report_ctx ctx;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    analog_report_measure(&ctx);
    analog_print_metric_line(&ctx);
    analog_print_raw_wav_line();
    analog_report_probes(&ctx, 0);
    analog_note_resolution();
    analog_check_limits(&ctx);
    /* A scene's recordings are checked once the engine has stopped (analog_oracle_run()), and the verdict waits for
       them. */
    g_reported = 1;
    if (!g_failed && g_scene.scene == NULL) {
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

/* The tone an analog row names (its confirmed CTCSS tone or DCS code, as the row's text spells it), or "none". */
static void
analog_row_tone(const dsd_receive_identity* id, char* out, size_t cap) {
    int written = -1;
    if (id->known && id->kind == (uint8_t)DSD_RECEIVE_CTCSS) {
        written = dsd_ctcss_format_label((int)id->value, out, cap);
    } else if (id->known && id->kind == (uint8_t)DSD_RECEIVE_DCS) {
        written = dsd_dcs_format_label((int)(id->value / 2U), (int)(id->value % 2U), out, cap);
    }
    if (written <= 0) {
        DSD_SNPRINTF(out, cap, "%s", "none");
    }
}

/* With per-call recording on (-P), one line per analog call history row the run committed, oldest first: when its call
   began and ended, in whole seconds of decode time. The stop hook runs after the engine committed the session's last
   calls, so a call still held when the input ended is among them (issue #756). */
static void
analog_print_call_rows(const dsd_opts* opts, const dsd_state* state) {
    if (!opts->dmr_stereo_wav || state->event_history_s == NULL) {
        return;
    }
    const Event_History_I* history = &state->event_history_s[0];
    for (int i = DSD_EVENT_HISTORY_LEN - 1; i >= 1; i--) {
        const Event_History* row = &history->Event_History_Items[i];
        if (row->systype == DSD_SYNC_ANALOG) {
            char tone[ANALOG_TONE_LABEL_SIZE];
            analog_row_tone(&row->receive_identity, tone, sizeof tone);
            DSD_FPRINTF(stderr, "ANALOG CALL ROW: start=%lld end=%lld tone=%s\n", (long long)row->event_start_time,
                        (long long)row->event_time, tone);
        }
    }
}

static void
analog_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    analog_flush_block();
    if (analog_pcm_input(opts)) {
        analog_note_pcm_position(opts);
        g_delivery_pcm = 1;
    }
    if (g_raw_wav_min_rms_dbfs.set) {
        analog_raw_wav_measure(opts);
    }
    analog_check_kept_output(opts);
    analog_report();
    analog_print_call_rows(opts, state);
}

/* Scores four blocks of a 1 kHz tone at 48 kHz -- the third silent, a squelched gap -- each written whole when @p split
   is 0, or as 960 samples and the rest when it is 1, and returns the totals. */
static analog_totals
analog_selftest_run(int split) {
    DSD_MEMSET(&g_totals, 0, sizeof(g_totals));
    DSD_MEMSET(&g_pending, 0, sizeof(g_pending));
    DSD_MEMSET(&g_tone, 0, sizeof(g_tone));
    g_totals.first_audible_ms = -1.0;
    g_tone.first_lock_ms = -1.0;
    g_totals.rate_hz = 44100U;
    g_totals.deliver_hz = 48000U;
    static short pcm[1045];
    analog_tone_snapshot tone;
    DSD_MEMSET(&tone, 0, sizeof(tone));
    for (int b = 0; b < 4; b++) {
        for (int i = 0; i < 1045; i++) {
            pcm[i] =
                b == 2 ? 0 : (short)lround(8000.0 * sin(2.0 * ANALOG_PI * 1000.0 * (double)(b * 1045 + i) / 48000.0));
        }
        const double at_ms = (double)(b + 1) * 960.0 * 1000.0 / 44100.0;
        if (split) {
            analog_take_write(at_ms, pcm, 960U, &tone, 960U);
            analog_take_write(at_ms, pcm + 960, 85U, &tone, 960U);
        } else {
            analog_take_write(at_ms, pcm, 1045U, &tone, 960U);
        }
    }
    analog_flush_block();
    return g_totals;
}

/* --analog-selftest: a block scores the same whether it arrived as one write or as several (issue #633). */
static int
analog_selftest(void) {
    g_limits.audible_dbfs.set = 1;
    g_limits.audible_dbfs.value = -40.0;
    const analog_totals whole = analog_selftest_run(0);
    const analog_totals split = analog_selftest_run(1);
    const int same = whole.blocks_scored == 4U && split.blocks_scored == 4U && whole.native_scored == 3840U
                     && split.native_scored == 3840U && whole.samples_captured == split.samples_captured
                     && fabs(whole.captured_ms - split.captured_ms) < 1e-9
                     && fabs(whole.audible_ms - split.audible_ms) < 1e-9
                     && fabs(whole.first_audible_ms - split.first_audible_ms) < 1e-9
                     && fabs(whole.energy_total - split.energy_total) < 1e-9
                     && fabs(whole.inband_energy - split.inband_energy) < 1e-9
                     && fabs(whole.outband_energy - split.outband_energy) < 1e-9
                     && fabs(whole.widest_bin_hz - split.widest_bin_hz) < 1e-9;
    const int gap = fabs(whole.audible_ms - 3.0 * 1045.0 * 1000.0 / 48000.0) < 1e-6;
    DSD_FPRINTF(stderr, "analog replay self-test: %s\n", same && gap ? "OK" : "FAIL");
    return same && gap ? 0 : 1;
}

/* --analog-gap-join, which replays a joined copy of the capture and so takes no other option that replaces it; 0, or -1
   with a message. */
static int
analog_prepare_gap(char** args, int count) {
    if (g_iq_gain_db.set || g_pcm_tap_hz.set || g_synth_pauses.set) {
        DSD_FPRINTF(stderr, "analog replay: %s replays a joined copy of the capture, alone\n", k_gap_join_arg);
        return -1;
    }
    return analog_replay_gap(args, count);
}

/* Applies the host options that change the dsd-neo arguments: the capture replayed (a scaled copy, a made one, or its
   discriminator tap as audio input) and the raw WAV. 0, or -1 with a message. */
static int
analog_prepare_args(char** args, int* count) {
    if (g_iq_gain_db.set && (g_pcm_tap_hz.set || g_synth_pauses.set)) {
        DSD_FPRINTF(stderr, "analog replay: --analog-iq-gain-db scales a committed capture, --analog-pcm-tap and "
                            "--analog-synth-pauses replace it\n");
        return -1;
    }
    if (g_scene.scene != NULL && (g_synth_pauses.set || g_iq_gain_db.set)) {
        DSD_FPRINTF(stderr, "analog replay: --analog-scene makes the input, as --analog-synth-pauses does\n");
        return -1;
    }
    if (g_gap_join.set && analog_prepare_gap(args, *count) != 0) {
        return -1;
    }
    if (g_iq_gain_db.set && analog_replay_scaled(args, *count) != 0) {
        return -1;
    }
    if (g_scene.scene != NULL) {
        if (analog_replay_scene(args, count) != 0) {
            return -1;
        }
    } else if (g_synth_pauses.set ? analog_replay_synth(args, count) != 0
                                  : (g_pcm_tap_hz.set && analog_replay_tap(args, *count) != 0)) {
        return -1;
    }
    return (g_raw_wav_min_rms_dbfs.set && analog_raw_wav_add(args, count) != 0) ? -1 : 0;
}

int
main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--analog-selftest") == 0) {
        return analog_selftest();
    }
    /* Room for the NULL after the arguments, for the `-6 FILE` --analog-raw-wav-min-rms-dbfs adds and for the input
       --analog-synth-pauses or --analog-scene adds (`-i udp:... -s RATE` at most). */
    char** args = (char**)calloc((size_t)argc + 7U, sizeof(*args));
    if (args == NULL) {
        return 1;
    }
    g_totals.first_audible_ms = -1.0;
    g_totals.last_audible_ms = -1.0;
    g_tone.first_lock_ms = -1.0;
    int kept = 0;
    if (analog_split_args(argc, argv, args, &kept) != 0 || analog_prepare_args(args, &kept) != 0) {
        analog_remove_scaled();
        analog_raw_wav_remove();
        analog_scene_free();
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
            if (g_scene.feeding) {
                (void)dsd_thread_join(g_scene.feeder);
                g_scene.feeding = 0;
            }
            analog_oracle_run(opts);
            analog_report_scene();
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
    analog_raw_wav_remove();
    analog_scene_free();
    return rc;
}
