// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The CTCSS table and its text (issue #522). The table is pinned value by value against the
 * published EIA/TIA 50-tone list, so a transposed digit cannot hide behind a count check, and
 * 150.0 Hz is pinned absent: a detector that snapped it would report 151.4 Hz.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>

/* src/runtime/analog_tones.c may not include the IO header, so it mirrors the monitor-audio
   output kind as a bare 0. Pin the mirror here, where the IO names are in reach: reordering
   the IO enum must fail this build, not silently point detection at the wrong output. */
_Static_assert(RTL_STREAM_OUTPUT_AUDIO_MONITOR == 0, "analog_tones.c mirrors the IO monitor-audio output kind");

static const int k_expected_tenths[] = {
    670,  693,  719,  744,  770,  797,  825,  854,  885,  915,  948,  974,  1000, 1035, 1072, 1109, 1148,
    1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799,
    1835, 1862, 1899, 1928, 1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};
_Static_assert((int)(sizeof(k_expected_tenths) / sizeof(k_expected_tenths[0])) == DSD_CTCSS_TONE_COUNT,
               "the expected table lists every standard tone");

static void
test_table(void) {
    assert(dsd_ctcss_tone_count() == DSD_CTCSS_TONE_COUNT);
    for (int i = 0; i < DSD_CTCSS_TONE_COUNT; i++) {
        assert(dsd_ctcss_tone_tenths(i) == k_expected_tenths[i]);
        assert(dsd_ctcss_tone_index(k_expected_tenths[i]) == i);
        if (i > 0) {
            assert(dsd_ctcss_tone_tenths(i) > dsd_ctcss_tone_tenths(i - 1));
        }
    }
    assert(dsd_ctcss_tone_tenths(-1) == -1);
    assert(dsd_ctcss_tone_tenths(DSD_CTCSS_TONE_COUNT) == -1);
    assert(dsd_ctcss_tone_tenths(0) == 670);
    assert(dsd_ctcss_tone_tenths(DSD_CTCSS_TONE_COUNT - 1) == 2541);

    /* Not supported: 150.0 (the would-be 51st tone), 68.2 (between 67.0 and 69.3), and the
       values either side of the table. */
    assert(dsd_ctcss_tone_index(1500) == -1);
    assert(dsd_ctcss_tone_index(682) == -1);
    assert(dsd_ctcss_tone_index(669) == -1);
    assert(dsd_ctcss_tone_index(2542) == -1);
    assert(dsd_ctcss_tone_index(0) == -1);
    assert(dsd_ctcss_tone_index(-670) == -1);
    assert(dsd_ctcss_tone_index(1514) == 24);
}

static void
test_format(void) {
    char buf[DSD_CTCSS_LABEL_SIZE];

    assert(dsd_ctcss_format(1000, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "100.0") == 0);
    assert(dsd_ctcss_format(670, buf, sizeof(buf)) == 4);
    assert(strcmp(buf, "67.0") == 0);
    assert(dsd_ctcss_format(2541, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "254.1") == 0);
    /* Any value can be named, supported or not, so a rejection can say what it saw. */
    assert(dsd_ctcss_format(1500, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "150.0") == 0);
    assert(dsd_ctcss_format(1, buf, sizeof(buf)) == 3);
    assert(strcmp(buf, "0.1") == 0);

    assert(dsd_ctcss_format_label(1000, buf, sizeof(buf)) == 14);
    assert(strcmp(buf, "CTCSS 100.0 Hz") == 0);
    assert(dsd_ctcss_format_label(670, buf, sizeof(buf)) == 13);
    assert(strcmp(buf, "CTCSS 67.0 Hz") == 0);
    assert(dsd_ctcss_format_label(9999, buf, sizeof(buf)) == 14);
    assert(strcmp(buf, "CTCSS 999.9 Hz") == 0);

    /* Out-of-range values and short buffers leave an empty string and report failure. */
    buf[0] = 'x';
    assert(dsd_ctcss_format(0, buf, sizeof(buf)) == -1);
    assert(buf[0] == '\0');
    buf[0] = 'x';
    assert(dsd_ctcss_format(10000, buf, sizeof(buf)) == -1);
    assert(buf[0] == '\0');
    assert(dsd_ctcss_format(-1000, buf, sizeof(buf)) == -1);
    char tiny[5];
    assert(dsd_ctcss_format(1000, tiny, sizeof(tiny)) == -1);
    assert(tiny[0] == '\0');
    char label_tiny[14];
    assert(dsd_ctcss_format_label(1000, label_tiny, sizeof(label_tiny)) == -1);
    assert(label_tiny[0] == '\0');
    assert(dsd_ctcss_format(1000, NULL, 8) == -1);
    assert(dsd_ctcss_format(1000, buf, 0) == -1);
}

static int g_fake_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;

static int
fake_output_kind(void) {
    return g_fake_output_kind;
}

/* The receive tap runs, and detection with it on FM (issue #524), exactly as @p tap says. */
static void
expect_tap(const dsd_opts* opts, int tap) {
    assert(dsd_analog_monitor_tap_active(opts) == tap);
    assert(dsd_analog_tone_detection_active(opts) == (tap && opts->analog_demod == DSD_ANALOG_DEMOD_FM));
}

/* The tap runs for the analog monitor on audio it can hear, and nowhere else, and detection
   for the analog FM monitor: the rules the decoder's tap, the scanners' carrier and every
   frontend's row share. */
static void
test_detection_active(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    assert(opts != NULL);
    assert(dsd_analog_monitor_tap_active(NULL) == 0);
    assert(dsd_analog_tone_detection_active(NULL) == 0);
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;

    static const int pcm[] = {AUDIO_IN_PULSE, AUDIO_IN_STDIN, AUDIO_IN_WAV, AUDIO_IN_UDP, AUDIO_IN_TCP};
    for (size_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++) {
        opts->audio_in_type = pcm[i];
        assert(dsd_analog_tone_detection_active(opts) == 1);
        expect_tap(opts, 1);
    }
    /* Symbol captures and no input carry no audio to hear. */
    static const int no_audio[] = {AUDIO_IN_SYMBOL_BIN, AUDIO_IN_SYMBOL_FLT, AUDIO_IN_NULL};
    for (size_t i = 0; i < sizeof(no_audio) / sizeof(no_audio[0]); i++) {
        opts->audio_in_type = no_audio[i];
        expect_tap(opts, 0);
    }

    /* RTL: only while the stream outputs monitor audio (output kind 0), which is also what the
       hook answers with nothing installed. */
    opts->audio_in_type = AUDIO_IN_RTL;
    dsd_rtl_stream_metrics_hooks_set(NULL);
    assert(dsd_analog_tone_detection_active(opts) == 1);
    expect_tap(opts, 1);
    const dsd_rtl_stream_metrics_hooks hooks = {.output_kind = fake_output_kind};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    g_fake_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    expect_tap(opts, 1);
    g_fake_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR; /* a digital family */
    expect_tap(opts, 0);
    g_fake_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    expect_tap(opts, 0);
    g_fake_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;

    /* The AM monitor (issue #524): CTCSS and DCS are FM signalling, so detection is off there, on RTL and PCM alike,
       while the tap still runs for the carrier the scanners hold a row on (issue #526), on monitor audio only. */
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    assert(dsd_analog_tone_detection_active(opts) == 0);
    expect_tap(opts, 1);
    g_fake_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    expect_tap(opts, 0);
    g_fake_output_kind = RTL_STREAM_OUTPUT_AUDIO_MONITOR;
    opts->audio_in_type = AUDIO_IN_PULSE;
    assert(dsd_analog_tone_detection_active(opts) == 0);
    expect_tap(opts, 1);
    opts->audio_in_type = AUDIO_IN_NULL;
    expect_tap(opts, 0);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->analog_only = 0;
    expect_tap(opts, 0);
    opts->analog_only = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    assert(dsd_analog_tone_detection_active(opts) == 1);

    /* Not the analog monitor: digital decoding, or analog without the input monitored. */
    opts->analog_only = 0;
    expect_tap(opts, 0);
    opts->analog_only = 1;
    opts->monitor_input_audio = 0;
    expect_tap(opts, 0);
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    expect_tap(opts, 0);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    free(opts);
}

int
main(void) {
    test_table();
    test_format();
    test_detection_active();
    return 0;
}
