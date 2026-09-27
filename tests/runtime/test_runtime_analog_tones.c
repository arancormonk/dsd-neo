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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
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

/* The verdict in force (issue #527) fails closed: a published OFF while a list policy is configured is no verdict of
   that policy -- the tap had no session to judge with (one it could not allocate) or has not read since the policy came
   on -- and mutes as PENDING until the tap's own verdict replaces it. Without a list policy, or without detection,
   nothing is in force. */
static void
test_gate_in_force_fails_closed(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts != NULL && state != NULL);
    assert(dsd_analog_tone_gate_in_force(NULL, state) == DSD_ANALOG_TONE_GATE_OFF);
    assert(dsd_analog_tone_gate_in_force(opts, NULL) == DSD_ANALOG_TONE_GATE_OFF);
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    assert(dsd_analog_tone_detection_active(opts) == 1);

    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_OFF;
    assert(dsd_analog_tone_gate_in_force(opts, state) == DSD_ANALOG_TONE_GATE_OFF);
    assert(dsd_tone_set_parse("100.0", &opts->analog_tone_set, NULL, 0) == 0);
    assert(dsd_analog_tone_gate_in_force(opts, state) == DSD_ANALOG_TONE_GATE_OFF); /* off keeps its list */
    opts->analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    assert(dsd_analog_tone_gate_in_force(opts, state) == DSD_ANALOG_TONE_GATE_PENDING);
    opts->analog_tone_filter = DSD_TONE_FILTER_BLOCK;
    assert(dsd_analog_tone_gate_in_force(opts, state) == DSD_ANALOG_TONE_GATE_PENDING);
    /* The tap's own verdicts stand, and a value this build does not know still mutes. */
    static const int verdicts[] = {DSD_ANALOG_TONE_GATE_PENDING, DSD_ANALOG_TONE_GATE_ALLOWED,
                                   DSD_ANALOG_TONE_GATE_REJECTED};
    for (size_t i = 0; i < sizeof(verdicts) / sizeof(verdicts[0]); i++) {
        state->analog_rx.gate = verdicts[i];
        assert(dsd_analog_tone_gate_in_force(opts, state) == verdicts[i]);
    }
    state->analog_rx.gate = 7;
    assert(dsd_analog_tone_gate_in_force(opts, state) == DSD_ANALOG_TONE_GATE_PENDING);

    /* A list policy with no list judges nothing, as the tap configures it. */
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_OFF;
    DSD_MEMSET(&opts->analog_tone_set, 0, sizeof(opts->analog_tone_set));
    assert(dsd_analog_tone_gate_in_force(opts, state) == DSD_ANALOG_TONE_GATE_OFF);

    /* No detection, no policy in force: the AM monitor, whatever is published or configured. */
    assert(dsd_tone_set_parse("100.0", &opts->analog_tone_set, NULL, 0) == 0);
    opts->analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    assert(dsd_analog_tone_gate_in_force(opts, state) == DSD_ANALOG_TONE_GATE_OFF);
    state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    assert(dsd_analog_tone_gate_in_force(opts, state) == DSD_ANALOG_TONE_GATE_OFF);
    free(state);
    free(opts);
}

/* What each verdict lets through (issue #527): OFF and ALLOWED only. The monitor plays nothing else, and nothing else is
   scan activity -- a carrier still being checked holds a row only while it lasts, leaving no -t or activity_hold_ms tail,
   and a rejected one holds nothing. A value this build does not know passes nothing. */
static void
test_gate_passes(void) {
    assert(dsd_analog_tone_gate_passes(DSD_ANALOG_TONE_GATE_OFF) == 1);
    assert(dsd_analog_tone_gate_passes(DSD_ANALOG_TONE_GATE_PENDING) == 0);
    assert(dsd_analog_tone_gate_passes(DSD_ANALOG_TONE_GATE_ALLOWED) == 1);
    assert(dsd_analog_tone_gate_passes(DSD_ANALOG_TONE_GATE_REJECTED) == 0);
    assert(dsd_analog_tone_gate_passes(7) == 0 && dsd_analog_tone_gate_passes(-1) == 0);
}

/* ------------------------------------------------------------------------------------------
 * DCS (issue #523)
 * ---------------------------------------------------------------------------------------- */

/* The standard 104-code set, as published (octal), ascending. */
static const int k_expected_dcs[] = {
    0023, 0025, 0026, 0031, 0032, 0036, 0043, 0047, 0051, 0053, 0054, 0065, 0071, 0072, 0073, 0074, 0114, 0115,
    0116, 0122, 0125, 0131, 0132, 0134, 0143, 0145, 0152, 0155, 0156, 0162, 0165, 0172, 0174, 0205, 0212, 0223,
    0225, 0226, 0243, 0244, 0245, 0246, 0251, 0252, 0255, 0261, 0263, 0265, 0266, 0271, 0274, 0306, 0311, 0315,
    0325, 0331, 0332, 0343, 0346, 0351, 0356, 0364, 0365, 0371, 0411, 0412, 0413, 0423, 0431, 0432, 0445, 0446,
    0452, 0454, 0455, 0462, 0464, 0465, 0466, 0503, 0506, 0516, 0523, 0526, 0532, 0546, 0565, 0606, 0612, 0624,
    0627, 0631, 0632, 0654, 0662, 0664, 0703, 0712, 0723, 0731, 0732, 0734, 0743, 0754,
};
_Static_assert((int)(sizeof(k_expected_dcs) / sizeof(k_expected_dcs[0])) == DSD_DCS_CODE_COUNT,
               "the expected DCS table lists every standard code");

/*
 * The alias golden table: every supported code sent in inverted polarity is the signal of
 * exactly one supported normal code, and reads as that code (normal first, then the lowest
 * code). {code, the normal code its inverted signal reads as}. Every normal code reads as
 * itself.
 */
static const int k_inverted_alias[][2] = {
    {0023, 0047}, {0025, 0244}, {0026, 0464}, {0031, 0627}, {0032, 0051}, {0036, 0172}, {0043, 0445}, {0047, 0023},
    {0051, 0032}, {0053, 0452}, {0054, 0413}, {0065, 0271}, {0071, 0306}, {0072, 0245}, {0073, 0506}, {0074, 0174},
    {0114, 0712}, {0115, 0152}, {0116, 0754}, {0122, 0225}, {0125, 0365}, {0131, 0364}, {0132, 0546}, {0134, 0223},
    {0143, 0412}, {0145, 0274}, {0152, 0115}, {0155, 0731}, {0156, 0265}, {0162, 0503}, {0165, 0251}, {0172, 0036},
    {0174, 0074}, {0205, 0263}, {0212, 0356}, {0223, 0134}, {0225, 0122}, {0226, 0411}, {0243, 0351}, {0244, 0025},
    {0245, 0072}, {0246, 0523}, {0251, 0165}, {0252, 0462}, {0255, 0446}, {0261, 0732}, {0263, 0205}, {0265, 0156},
    {0266, 0454}, {0271, 0065}, {0274, 0145}, {0306, 0071}, {0311, 0664}, {0315, 0423}, {0325, 0526}, {0331, 0465},
    {0332, 0455}, {0343, 0532}, {0346, 0612}, {0351, 0243}, {0356, 0212}, {0364, 0131}, {0365, 0125}, {0371, 0734},
    {0411, 0226}, {0412, 0143}, {0413, 0054}, {0423, 0315}, {0431, 0723}, {0432, 0516}, {0445, 0043}, {0446, 0255},
    {0452, 0053}, {0454, 0266}, {0455, 0332}, {0462, 0252}, {0464, 0026}, {0465, 0331}, {0466, 0662}, {0503, 0162},
    {0506, 0073}, {0516, 0432}, {0523, 0246}, {0526, 0325}, {0532, 0343}, {0546, 0132}, {0565, 0703}, {0606, 0631},
    {0612, 0346}, {0624, 0632}, {0627, 0031}, {0631, 0606}, {0632, 0624}, {0654, 0743}, {0662, 0466}, {0664, 0311},
    {0703, 0565}, {0712, 0114}, {0723, 0431}, {0731, 0155}, {0732, 0261}, {0734, 0371}, {0743, 0654}, {0754, 0116},
};
_Static_assert((int)(sizeof(k_inverted_alias) / sizeof(k_inverted_alias[0])) == DSD_DCS_CODE_COUNT,
               "the alias table covers every standard code");

#define DCS_MASK ((1U << DSD_DCS_WORD_BITS) - 1U)

/* A word written most significant bit first, the way the references print it; separators
   ('-', '/', ' ') are skipped. */
static uint32_t
bits_msb_first(const char* text) {
    uint32_t word = 0U;
    int count = 0;
    for (const char* p = text; *p != '\0'; p++) {
        if (*p == '0' || *p == '1') {
            word = (word << 1) | (uint32_t)(*p - '0');
            count++;
        }
    }
    assert(count == DSD_DCS_WORD_BITS);
    return word;
}

/* 23 bits of a run printed in the order they were received, from its first-th bit: the earliest
   in bit 0. Separators are skipped. */
static uint32_t
bits_in_arrival_order(const char* text, int first) {
    uint32_t window = 0U;
    int seen = 0;
    int taken = 0;
    for (const char* p = text; *p != '\0' && taken < DSD_DCS_WORD_BITS; p++) {
        if (*p != '0' && *p != '1') {
            continue;
        }
        if (seen++ >= first) {
            window |= (uint32_t)(*p - '0') << taken++;
        }
    }
    assert(taken == DSD_DCS_WORD_BITS);
    return window;
}

static uint32_t
rotate_right(uint32_t word, int k) {
    k %= DSD_DCS_WORD_BITS;
    if (k == 0) {
        return word & DCS_MASK;
    }
    return ((word >> k) | (word << (DSD_DCS_WORD_BITS - k))) & DCS_MASK;
}

static void
test_dcs_table(void) {
    assert(dsd_dcs_code_count() == DSD_DCS_CODE_COUNT);
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        assert(dsd_dcs_code(i) == k_expected_dcs[i]);
        assert(dsd_dcs_code_index(k_expected_dcs[i]) == i);
    }
    assert(dsd_dcs_code(-1) == -1);
    assert(dsd_dcs_code(DSD_DCS_CODE_COUNT) == -1);
    /* Not in the standard set: 000, 017, 050, 645 (offered by some radios), and non-codes. */
    assert(dsd_dcs_code_index(0) == -1);
    assert(dsd_dcs_code_index(0017) == -1);
    assert(dsd_dcs_code_index(0050) == -1);
    assert(dsd_dcs_code_index(0645) == -1);
    assert(dsd_dcs_code_index(-19) == -1);
    assert(dsd_dcs_code_index(01000) == -1);
}

/*
 * Reference words, as published, pin the layout and the bit order:
 *
 * - onfreq "DPL / DCS Information" (mirrored at kb8zqz.org/onfreq_mirror/syntorx/dcs.html)
 *   prints words most significant bit first as "11 check bits - 100 - code": 023 as
 *   11101100011-100-000/010/011, the inverted 023 word as 00010011100-011-111/101/100, 047
 *   (the inverted 023 word rotated to its third 100 signature match) as
 *   00011111101-100-000/100/111, 020 (a word of another rotation class) as
 *   11110001001100000010000 and 000 as 11000111010-100-000/000/000, and states the word "is
 *   actually sent in the reverse order": 023 as 110/010/000-001-11000110111.
 * - "Manually Decoding a DCS Tone" (ambientmemory.com, 2017) lists bits received off the air,
 *   in arrival order, that decode to 023:
 *   10000001110001101111100100000011100011011111001.
 * - Batlabs "DPL Code Word Specifics" gives the data bits of 525 as xxxxxxxxxxx100101010101.
 */
static void
test_dcs_reference_words(void) {
    assert(dsd_dcs_word(0023, 0) == bits_msb_first("11101100011-100-000/010/011"));
    assert(dsd_dcs_word(0023, 0) == 0x763813U);
    assert(dsd_dcs_word(0023, 1) == bits_msb_first("00010011100-011-111/101/100"));
    assert(dsd_dcs_word(0047, 0) == bits_msb_first("00011111101-100-000/100/111"));
    assert(dsd_dcs_word(0047, 0) == 0x0FD827U);
    assert(dsd_dcs_word(0020, 0) == bits_msb_first("11110001001100000010000"));
    assert(dsd_dcs_word(0020, 0) == 0x789810U);
    assert(dsd_dcs_word(0, 0) == bits_msb_first("11000111010-100-000/000/000"));
    assert((dsd_dcs_word(0525, 0) & 0xFFFU) == (bits_msb_first("00000000000100101010101") & 0xFFFU));

    /* Sent in the reverse of the printed order: bit 0 first. */
    assert(bits_in_arrival_order("110/010/000-001-11000110111", 0) == dsd_dcs_word(0023, 0));

    /* The off-air bits: every 23 consecutive of them are one rotation of 023's word. */
    const char* off_air = "10000001110001101111100100000011100011011111001";
    for (int first = 0; first + DSD_DCS_WORD_BITS <= 47; first++) {
        const uint32_t window = bits_in_arrival_order(off_air, first);
        int found = 0;
        for (int k = 0; k < DSD_DCS_WORD_BITS; k++) {
            found |= rotate_right(dsd_dcs_word(0023, 0), k) == window;
        }
        assert(found);
        int code = -1;
        int inverted = -1;
        assert(dsd_dcs_match(window, &code, &inverted) == 1);
        assert(code == 0023 && inverted == 0);
    }
}

/* Every word of every 9-bit code: "code, then 100", then check bits that make it a multiple of
   the generator, and never 0. */
static void
test_dcs_words(void) {
    for (int code = 0; code <= DSD_DCS_CODE_MAX; code++) {
        const uint32_t word = dsd_dcs_word(code, 0);
        assert(word != 0U && (word & ~DCS_MASK) == 0U);
        assert((word & 0x1FFU) == (uint32_t)code);
        assert(((word >> 9) & 7U) == 4U);
        assert(dsd_dcs_word(code, 1) == (~word & DCS_MASK));
        /* Divide by g(x) = 0xC75 (x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1): no remainder. */
        uint32_t rem = word;
        for (int bit = DSD_DCS_WORD_BITS - 1; bit >= 11; bit--) {
            if ((rem >> bit) & 1U) {
                rem ^= 0xC75U << (bit - 11);
            }
        }
        assert(rem == 0U);
    }
    assert(dsd_dcs_word(-1, 0) == 0U);
    assert(dsd_dcs_word(DSD_DCS_CODE_MAX + 1, 0) == 0U);
}

/* Every supported code's word is balanced: 11 or 12 ones of 23, in either polarity, so its level
   averages to within 1/23 of zero over any 23 bits, and the DCS detector's balance slicer can
   slice a word against its own mean. It is a property of the standard set, not of the code: the
   Golay (23,12) words weigh 0, 7, 8, 11, 12, 15, 16 or 23, and 000's word weighs 7. */
static void
test_dcs_words_are_balanced(void) {
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        for (int inverted = 0; inverted < 2; inverted++) {
            const uint32_t word = dsd_dcs_word(dsd_dcs_code(i), inverted);
            int ones = 0;
            for (int bit = 0; bit < DSD_DCS_WORD_BITS; bit++) {
                ones += (int)((word >> bit) & 1U);
            }
            assert(ones == 11 || ones == 12);
        }
    }
    uint32_t unsupported = dsd_dcs_word(0, 0);
    int ones = 0;
    while (unsupported != 0U) {
        unsupported &= unsupported - 1U;
        ones++;
    }
    assert(ones == 7);
}

/* The alias rule, pinned code by code, and every rotation of every supported word in either
   polarity names its class the same way. */
static void
test_dcs_aliases(void) {
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        const int code = k_inverted_alias[i][0];
        const int alias = k_inverted_alias[i][1];
        assert(code == k_expected_dcs[i]);
        int got_code = -1;
        int got_inverted = -1;
        assert(dsd_dcs_canonical(code, 0, &got_code, &got_inverted) == 0);
        assert(got_code == code && got_inverted == 0);
        assert(dsd_dcs_canonical(code, 1, &got_code, &got_inverted) == 0);
        assert(got_code == alias && got_inverted == 0);
        /* The pairing is symmetric: the alias's inverted signal is this code's normal one. */
        assert(dsd_dcs_canonical(alias, 1, &got_code, &got_inverted) == 0);
        assert(got_code == code && got_inverted == 0);
        for (int inverted = 0; inverted < 2; inverted++) {
            const uint32_t word = dsd_dcs_word(code, inverted);
            for (int k = 0; k < DSD_DCS_WORD_BITS; k++) {
                got_code = -1;
                got_inverted = -1;
                assert(dsd_dcs_match(rotate_right(word, k), &got_code, &got_inverted) == 1);
                assert(got_code == (inverted ? alias : code) && got_inverted == 0);
            }
        }
    }
    /* onfreq's group for 023: the rotations of its word also carry 340 and 766, and those of
       the inverted word 047, 375 and 707, none of which (but 047) is in the standard set. */
    int got_code = -1;
    int got_inverted = -1;
    assert(dsd_dcs_canonical(0340, 0, &got_code, &got_inverted) == 0 && got_code == 0023 && got_inverted == 0);
    assert(dsd_dcs_canonical(0766, 0, &got_code, &got_inverted) == 0 && got_code == 0023 && got_inverted == 0);
    assert(dsd_dcs_canonical(0375, 1, &got_code, &got_inverted) == 0 && got_code == 0023 && got_inverted == 0);
    assert(dsd_dcs_canonical(0707, 1, &got_code, &got_inverted) == 0 && got_code == 0023 && got_inverted == 0);
    /* 000's signal carries no supported code. */
    assert(dsd_dcs_canonical(0, 0, &got_code, &got_inverted) == -1);
    assert(dsd_dcs_canonical(-1, 0, &got_code, &got_inverted) == -1);
    assert(dsd_dcs_canonical(DSD_DCS_CODE_MAX + 1, 0, NULL, NULL) == -1);
}

/*
 * The two spellings of every signal. Of the standard codes, exactly two send each signal, one
 * in each polarity: the canonical normal code and one inverted code (D023N and D047I, D047N and
 * D023I). A receiver cannot tell which of the two a transmitter was set to, so a received code
 * is shown as both, "DCS D023N / D047I", and dsd_dcs_alias() names the inverted one from either.
 * Pinned against the golden alias table, over every standard code in both polarities.
 */
static void
test_dcs_two_spellings(void) {
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        const int code = k_inverted_alias[i][0];
        const int partner = k_inverted_alias[i][1];
        /* Every standard spelling of code's normal signal: code itself, and partner inverted. */
        int spellings = 0;
        for (int j = 0; j < DSD_DCS_CODE_COUNT; j++) {
            for (int inverted = 0; inverted < 2; inverted++) {
                int canon_code = -1;
                int canon_inverted = -1;
                assert(dsd_dcs_canonical(dsd_dcs_code(j), inverted, &canon_code, &canon_inverted) == 0);
                if (canon_code == code && canon_inverted == 0) {
                    spellings++;
                    assert((dsd_dcs_code(j) == code && inverted == 0) || (dsd_dcs_code(j) == partner && inverted == 1));
                }
            }
        }
        assert(spellings == 2);

        /* The same second spelling from either one, and the label names both, canonical first. */
        int alias_code = -1;
        int alias_inverted = -1;
        assert(dsd_dcs_alias(code, 0, &alias_code, &alias_inverted) == 0);
        assert(alias_code == partner && alias_inverted == 1);
        alias_code = -1;
        alias_inverted = -1;
        assert(dsd_dcs_alias(partner, 1, &alias_code, &alias_inverted) == 0);
        assert(alias_code == partner && alias_inverted == 1);
        char expected[DSD_DCS_LABEL_SIZE];
        DSD_SNPRINTF(expected, sizeof(expected), "DCS D%03oN / D%03oI", (unsigned int)code, (unsigned int)partner);
        char label[DSD_DCS_LABEL_SIZE];
        assert(dsd_dcs_format_label(code, 0, label, sizeof(label)) == 17);
        assert(strcmp(label, expected) == 0);
        assert(dsd_dcs_format_label(partner, 1, label, sizeof(label)) == 17);
        assert(strcmp(label, expected) == 0);
    }

    /* A rotation outside the standard set spells a standard signal too. */
    int alias_code = -1;
    int alias_inverted = -1;
    assert(dsd_dcs_alias(0340, 0, &alias_code, &alias_inverted) == 0);
    assert(alias_code == 0047 && alias_inverted == 1);
    assert(dsd_dcs_alias(0023, 0, NULL, NULL) == 0);
    /* No standard code sends 000's signal, and a value past nine bits is no code: the outputs
       are left alone. */
    alias_code = 1234;
    alias_inverted = 5678;
    assert(dsd_dcs_alias(0, 0, &alias_code, &alias_inverted) == -1);
    assert(dsd_dcs_alias(-1, 0, &alias_code, &alias_inverted) == -1);
    assert(dsd_dcs_alias(DSD_DCS_CODE_MAX + 1, 1, &alias_code, &alias_inverted) == -1);
    assert(alias_code == 1234 && alias_inverted == 5678);
}

/* Windows that are not a supported code's signal name nothing and leave the outputs alone. */
static void
test_dcs_match_rejects(void) {
    int code = 1234;
    int inverted = 5678;
    assert(dsd_dcs_match(0U, &code, &inverted) == 0);
    assert(dsd_dcs_match(DCS_MASK, &code, &inverted) == 0);
    assert(dsd_dcs_match(0x555555U & DCS_MASK, &code, &inverted) == 0);
    assert(dsd_dcs_match(dsd_dcs_word(0, 0), &code, &inverted) == 0);
    /* One bit off a real word is not a match: matching is exact. */
    assert(dsd_dcs_match(dsd_dcs_word(0023, 0) ^ 0x10U, &code, &inverted) == 0);
    assert(code == 1234 && inverted == 5678);
    /* Bits above the window are ignored, and NULL outputs are allowed. */
    assert(dsd_dcs_match(dsd_dcs_word(0023, 0) | 0xFF800000U, NULL, NULL) == 1);
    /* Of all 2^23 windows exactly 104 x 23 are a supported signal: one class per standard code. */
    int matches = 0;
    for (uint32_t w = 0U; w <= DCS_MASK; w += 1U) {
        matches += dsd_dcs_match(w, NULL, NULL);
    }
    assert(matches == DSD_DCS_CODE_COUNT * DSD_DCS_WORD_BITS);
}

static void
test_dcs_format(void) {
    char buf[DSD_DCS_LABEL_SIZE];
    assert(dsd_dcs_format(0023, 0, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "D023N") == 0);
    assert(dsd_dcs_format(0023, 1, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "D023I") == 0);
    assert(dsd_dcs_format(0754, 0, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "D754N") == 0);
    /* Leading zeros always, and any 9-bit code can be named. */
    assert(dsd_dcs_format(0, 0, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "D000N") == 0);
    assert(dsd_dcs_format(07, 1, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "D007I") == 0);
    assert(dsd_dcs_format(0777, 0, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "D777N") == 0);

    /* The received label names the signal by both of its standard spellings, the canonical
       normal one first, whichever of them it is given: a receiver cannot tell which one the
       transmitter was set to. */
    assert(dsd_dcs_format_label(0023, 0, buf, sizeof(buf)) == 17);
    assert(strcmp(buf, "DCS D023N / D047I") == 0);
    assert(dsd_dcs_format_label(0047, 1, buf, sizeof(buf)) == 17);
    assert(strcmp(buf, "DCS D023N / D047I") == 0);
    assert(dsd_dcs_format_label(0023, 1, buf, sizeof(buf)) == 17);
    assert(strcmp(buf, "DCS D047N / D023I") == 0);
    assert(dsd_dcs_format_label(0047, 0, buf, sizeof(buf)) == 17);
    assert(strcmp(buf, "DCS D047N / D023I") == 0);
    assert(dsd_dcs_format_label(0754, 0, buf, sizeof(buf)) == 17);
    assert(strcmp(buf, "DCS D754N / D116I") == 0);
    /* A code outside the standard set whose word is a rotation of a standard word names that
       signal; one whose signal no standard code sends names nothing. */
    assert(dsd_dcs_format_label(0340, 0, buf, sizeof(buf)) == 17);
    assert(strcmp(buf, "DCS D023N / D047I") == 0);
    buf[0] = 'x';
    assert(dsd_dcs_format_label(0, 0, buf, sizeof(buf)) == -1);
    assert(buf[0] == '\0');
    buf[0] = 'x';
    assert(dsd_dcs_format_label(01000, 0, buf, sizeof(buf)) == -1);
    assert(buf[0] == '\0');

    buf[0] = 'x';
    assert(dsd_dcs_format(-1, 0, buf, sizeof(buf)) == -1);
    assert(buf[0] == '\0');
    buf[0] = 'x';
    assert(dsd_dcs_format(01000, 0, buf, sizeof(buf)) == -1);
    assert(buf[0] == '\0');
    char tiny[5];
    assert(dsd_dcs_format(0023, 0, tiny, sizeof(tiny)) == -1);
    assert(tiny[0] == '\0');
    char label_tiny[17];
    assert(dsd_dcs_format_label(0023, 0, label_tiny, sizeof(label_tiny)) == -1);
    assert(label_tiny[0] == '\0');
    char label_exact[18];
    assert(dsd_dcs_format_label(0023, 0, label_exact, sizeof(label_exact)) == 17);
    assert(strcmp(label_exact, "DCS D023N / D047I") == 0);
    assert(dsd_dcs_format(0023, 0, NULL, 8) == -1);
    assert(dsd_dcs_format_label(0023, 0, buf, 0) == -1);
}

/* ------------------------------------------------------------------------------------------
 * Tone lists (issue #527)
 * ---------------------------------------------------------------------------------------- */

/* Parse @p text, which must succeed, and check it formats back as @p canonical. */
static dsd_tone_set
parse_ok(const char* text, const char* canonical) {
    dsd_tone_set set;
    DSD_MEMSET(&set, 0xA5, sizeof(set));
    char err[DSD_TONE_LIST_ERROR_SIZE] = "untouched";
    assert(dsd_tone_set_parse(text, &set, err, sizeof(err)) == 0);
    char buf[DSD_TONE_LIST_TEXT_MAX + 1];
    assert(dsd_tone_set_format(&set, buf, sizeof(buf)) == (int)strlen(canonical));
    assert(strcmp(buf, canonical) == 0);
    /* What is written reads back as the same set. */
    dsd_tone_set again;
    assert(dsd_tone_set_parse(buf, &again, NULL, 0) == 0);
    assert(dsd_tone_set_equal(&set, &again));
    return set;
}

/* Parse @p text, which must fail with exactly @p why, leaving the output alone. */
static void
parse_fails(const char* text, const char* why) {
    dsd_tone_set set;
    DSD_MEMSET(&set, 0x5A, sizeof(set));
    dsd_tone_set before = set;
    char err[DSD_TONE_LIST_ERROR_SIZE] = "";
    assert(dsd_tone_set_parse(text, &set, err, sizeof(err)) == -1);
    assert(memcmp(&set, &before, sizeof(set)) == 0);
    if (strcmp(err, why) != 0) {
        DSD_FPRINTF(stderr, "tone list refusal: got \"%s\", want \"%s\"\n", err, why);
    }
    assert(strcmp(err, why) == 0);
}

static void
test_tone_list_parse(void) {
    /* '100' and '100.0' are the same tone, written back with its decimal; entries come back sorted,
       tones first. */
    dsd_tone_set set = parse_ok("100", "100.0");
    assert(dsd_tone_set_count(&set) == 1 && dsd_tone_set_contains_ctcss(&set, 1000));
    assert(!dsd_tone_set_contains_ctcss(&set, 1035) && !dsd_tone_set_has_dcs(&set));
    set = parse_ok("100.0", "100.0");
    set = parse_ok("D023N/254.1/67/d023i", "67.0/254.1/D023N/D023I");
    assert(dsd_tone_set_count(&set) == 4 && dsd_tone_set_has_dcs(&set));
    /* A bare code is N, and any case is taken. */
    set = parse_ok("D023", "D023N");
    set = parse_ok("d754n", "D754N");
    set = parse_ok("D047i", "D047I");
    /* Every standard tone and code in one list. */
    char all[DSD_TONE_LIST_TEXT_MAX + 1] = "";
    size_t used = 0;
    for (int i = 0; i < DSD_CTCSS_TONE_COUNT; i++) {
        char entry[16];
        assert(dsd_ctcss_format(dsd_ctcss_tone_tenths(i), entry, sizeof(entry)) > 0);
        used += (size_t)DSD_SNPRINTF(all + used, sizeof(all) - used, "%s%s", used ? "/" : "", entry);
    }
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        char entry[16];
        assert(dsd_dcs_format(dsd_dcs_code(i), 0, entry, sizeof(entry)) > 0);
        used += (size_t)DSD_SNPRINTF(all + used, sizeof(all) - used, "/%s", entry);
    }
    assert(used < sizeof(all) - 1U);
    set = parse_ok(all, all);
    assert(dsd_tone_set_count(&set) == DSD_CTCSS_TONE_COUNT + DSD_DCS_CODE_COUNT);
}

static void
test_tone_list_refusals(void) {
    parse_fails("", "the list is empty");
    parse_fails("100.0,D023N", "use / between entries, not commas");
    parse_fails("100.0/,", "use / between entries, not commas");
    parse_fails("100.0//D023N", "entry 2 is empty");
    parse_fails("/100.0", "entry 1 is empty");
    parse_fails("100.0/", "entry 2 is empty");
    /* 150.0 is not in the standard table, nor is 68.2; 100.00 and 1000 are not tone spellings. */
    parse_fails("67.0/150.0", "entry 2 is not a standard CTCSS tone or DCS code");
    parse_fails("68.2", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails("100.00", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails("1000", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails("100.", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails(".5", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails(" 100.0", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails("+100.0", "entry 1 is not a standard CTCSS tone or DCS code");
    /* D024 is no standard code, D08 no octal code, and a code takes one polarity letter. */
    parse_fails("100.0/D024N", "entry 2 is not a standard CTCSS tone or DCS code");
    parse_fails("D089", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails("D23", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails("D023NN", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails("D023X", "entry 1 is not a standard CTCSS tone or DCS code");
    parse_fails("CTCSS", "entry 1 is not a standard CTCSS tone or DCS code");
    /* Duplicates, alias spellings included, name the entry they repeat and the canonical spelling. */
    parse_fails("100/100.0", "entry 2 repeats entry 1 (100.0 Hz)");
    parse_fails("D023N/67.0/D023", "entry 3 repeats entry 1 (D023N)");
    parse_fails("D023N/D047I", "entry 2 is the same DCS signal as entry 1 (D023N)");
    parse_fails("D023I/D047N", "entry 2 is the same DCS signal as entry 1 (D047N)");
    /* Longer than an INI line can hold. */
    char longest[DSD_TONE_LIST_TEXT_MAX + 2];
    DSD_MEMSET(longest, '/', sizeof(longest));
    longest[sizeof(longest) - 1] = '\0';
    parse_fails(longest, "the list is longer than 1023 characters");

    /* No output or no text is refused; a NULL or zero-sized error buffer is allowed. */
    dsd_tone_set set;
    assert(dsd_tone_set_parse(NULL, &set, NULL, 0) == -1);
    assert(dsd_tone_set_parse("100.0", NULL, NULL, 0) == -1);
    char err[4] = "abc";
    assert(dsd_tone_set_parse("68.2", &set, err, 0) == -1);
    assert(strcmp(err, "abc") == 0);
}

/* Matching goes by the DCS signal: a listed D023I matches a received D047N, and the reverse. */
static void
test_tone_list_match_by_signal(void) {
    dsd_tone_set set;
    assert(dsd_tone_set_parse("D023I/100.0", &set, NULL, 0) == 0);
    assert(dsd_tone_set_contains_dcs(&set, 0023, 1));
    assert(dsd_tone_set_contains_dcs(&set, 0047, 0));
    assert(!dsd_tone_set_contains_dcs(&set, 0023, 0));
    assert(!dsd_tone_set_contains_dcs(&set, 0047, 1));
    assert(dsd_tone_set_contains_ctcss(&set, 1000));
    assert(dsd_tone_set_parse("D047N", &set, NULL, 0) == 0);
    assert(dsd_tone_set_contains_dcs(&set, 0023, 1) && dsd_tone_set_contains_dcs(&set, 0047, 0));
    /* Every standard code in either polarity matches exactly the two spellings of its signal. */
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        const int code = dsd_dcs_code(i);
        for (int inverted = 0; inverted < 2; inverted++) {
            char entry[8];
            assert(dsd_dcs_format(code, inverted, entry, sizeof(entry)) > 0);
            assert(dsd_tone_set_parse(entry, &set, NULL, 0) == 0);
            int alias_code = -1;
            int alias_inverted = -1;
            int canon_code = -1;
            int canon_inverted = -1;
            assert(dsd_dcs_alias(code, inverted, &alias_code, &alias_inverted) == 0);
            assert(dsd_dcs_canonical(code, inverted, &canon_code, &canon_inverted) == 0);
            assert(dsd_tone_set_contains_dcs(&set, canon_code, canon_inverted));
            assert(dsd_tone_set_contains_dcs(&set, alias_code, alias_inverted));
            assert(!dsd_tone_set_contains_dcs(&set, code, inverted ? 0 : 1));
        }
    }
    assert(!dsd_tone_set_contains_dcs(NULL, 0023, 0));
    assert(!dsd_tone_set_contains_ctcss(NULL, 1000));
    assert(dsd_tone_set_count(NULL) == 0 && !dsd_tone_set_has_dcs(NULL));
}

/* Two lists pass and block the same traffic when they hold the same tones and the same DCS signals, however each code
 * is spelled (issue #527): a policy reloaded in the other spelling of its codes is the same policy. Exact equality
 * still tells the spellings apart. */
static void
test_tone_list_same_signals(void) {
    dsd_tone_set a;
    dsd_tone_set b;
    assert(dsd_tone_set_parse("100.0/D023I", &a, NULL, 0) == 0);
    assert(dsd_tone_set_parse("D047N/100", &b, NULL, 0) == 0);
    assert(!dsd_tone_set_equal(&a, &b));
    assert(dsd_tone_set_same_signals(&a, &b) && dsd_tone_set_same_signals(&b, &a));
    assert(dsd_tone_set_same_signals(&a, &a));
    /* Another polarity is another signal; so is another tone, and a list with one entry more. */
    assert(dsd_tone_set_parse("100.0/D023N", &b, NULL, 0) == 0);
    assert(!dsd_tone_set_same_signals(&a, &b));
    assert(dsd_tone_set_parse("103.5/D047N", &b, NULL, 0) == 0);
    assert(!dsd_tone_set_same_signals(&a, &b));
    assert(dsd_tone_set_parse("100.0/D047N/D754I", &b, NULL, 0) == 0);
    assert(!dsd_tone_set_same_signals(&a, &b) && !dsd_tone_set_same_signals(&b, &a));
    /* Every standard code in either polarity is the same signal as its other spelling, and not as its own other
       polarity. */
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        const int code = dsd_dcs_code(i);
        for (int inverted = 0; inverted < 2; inverted++) {
            char entry[8];
            char alias[8];
            char flipped[8];
            int alias_code = -1;
            int alias_inverted = -1;
            assert(dsd_dcs_format(code, inverted, entry, sizeof(entry)) > 0);
            assert(dsd_dcs_alias(code, inverted, &alias_code, &alias_inverted) == 0);
            assert(dsd_dcs_format(alias_code, alias_inverted, alias, sizeof(alias)) > 0);
            assert(dsd_dcs_format(code, inverted ? 0 : 1, flipped, sizeof(flipped)) > 0);
            assert(dsd_tone_set_parse(entry, &a, NULL, 0) == 0);
            assert(dsd_tone_set_parse(alias, &b, NULL, 0) == 0);
            assert(dsd_tone_set_same_signals(&a, &b));
            assert(dsd_tone_set_parse(flipped, &b, NULL, 0) == 0);
            assert(!dsd_tone_set_same_signals(&a, &b));
        }
    }
    const dsd_tone_set empty = {0};
    assert(dsd_tone_set_same_signals(NULL, NULL));
    assert(!dsd_tone_set_same_signals(&a, NULL) && !dsd_tone_set_same_signals(NULL, &a));
    assert(dsd_tone_set_same_signals(&empty, &empty) && !dsd_tone_set_same_signals(&a, &empty));
}

static void
test_tone_list_format(void) {
    dsd_tone_set set;
    assert(dsd_tone_set_parse("D023N/100", &set, NULL, 0) == 0);
    char buf[64];
    assert(dsd_tone_set_format_display(&set, buf, sizeof(buf)) == 14);
    assert(strcmp(buf, "100.0 Hz/D023N") == 0);
    /* An exact fit, and one byte short of it. */
    char exact[12];
    assert(dsd_tone_set_format(&set, exact, sizeof(exact)) == 11);
    assert(strcmp(exact, "100.0/D023N") == 0);
    char short_buf[11];
    assert(dsd_tone_set_format(&set, short_buf, sizeof(short_buf)) == -1);
    assert(short_buf[0] == '\0');
    /* The empty set is "", in either form. */
    const dsd_tone_set empty = {0};
    assert(dsd_tone_set_format(&empty, buf, sizeof(buf)) == 0 && buf[0] == '\0');
    assert(dsd_tone_set_format_display(&empty, buf, sizeof(buf)) == 0 && buf[0] == '\0');
    /* Display text that does not fit counts what it left out. */
    assert(dsd_tone_set_parse("67/71.9/74.4/77/79.7/D023/D754I", &set, NULL, 0) == 0);
    assert(dsd_tone_set_format_display(&set, buf, 32) == 29);
    assert(strcmp(buf, "67.0 Hz/71.9 Hz/74.4 Hz/\xE2\x80\xA6+4") == 0);
    char small[24];
    assert(dsd_tone_set_format_display(&set, small, sizeof(small)) == 21);
    assert(strcmp(small, "67.0 Hz/71.9 Hz/\xE2\x80\xA6+5") == 0);
    assert(dsd_tone_set_format_display(&set, small, 23) == -1 && small[0] == '\0');
    /* Everything fits: no count. */
    assert(dsd_tone_set_format_display(&set, buf, sizeof(buf)) == 51);
    assert(strcmp(buf, "67.0 Hz/71.9 Hz/74.4 Hz/77.0 Hz/79.7 Hz/D023N/D754I") == 0);
    assert(dsd_tone_set_format(NULL, buf, sizeof(buf)) == -1);
    assert(dsd_tone_set_format_display(&set, NULL, 32) == -1);

    assert(dsd_tone_set_equal(NULL, NULL) && !dsd_tone_set_equal(&set, NULL));
    assert(dsd_tone_set_equal(&empty, &empty) && !dsd_tone_set_equal(&set, &empty));
}

static void
test_tone_filter_mode(void) {
    int mode = -1;
    assert(dsd_tone_filter_mode_parse("off", &mode) == 0 && mode == DSD_TONE_FILTER_OFF);
    assert(dsd_tone_filter_mode_parse("Allow", &mode) == 0 && mode == DSD_TONE_FILTER_ALLOW);
    assert(dsd_tone_filter_mode_parse("BLOCK", &mode) == 0 && mode == DSD_TONE_FILTER_BLOCK);
    mode = 77;
    assert(dsd_tone_filter_mode_parse("deny", &mode) == -1 && mode == 77);
    assert(dsd_tone_filter_mode_parse("", &mode) == -1 && mode == 77);
    assert(dsd_tone_filter_mode_parse(NULL, &mode) == -1 && mode == 77);
    assert(dsd_tone_filter_mode_parse("off", NULL) == -1);
    assert(strcmp(dsd_tone_filter_mode_name(DSD_TONE_FILTER_OFF), "off") == 0);
    assert(strcmp(dsd_tone_filter_mode_name(DSD_TONE_FILTER_ALLOW), "allow") == 0);
    assert(strcmp(dsd_tone_filter_mode_name(DSD_TONE_FILTER_BLOCK), "block") == 0);
    assert(dsd_tone_filter_mode_name(3) == NULL && dsd_tone_filter_mode_name(-1) == NULL);
}

int
main(void) {
    test_tone_list_parse();
    test_tone_list_refusals();
    test_tone_list_match_by_signal();
    test_tone_list_same_signals();
    test_tone_list_format();
    test_tone_filter_mode();
    test_table();
    test_format();
    test_detection_active();
    test_gate_in_force_fails_closed();
    test_gate_passes();
    test_dcs_table();
    test_dcs_reference_words();
    test_dcs_words();
    test_dcs_words_are_balanced();
    test_dcs_aliases();
    test_dcs_two_spellings();
    test_dcs_match_rejects();
    test_dcs_format();
    return 0;
}
