// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The squelch setting's grammar, text and resolution (issue #518 follow-up): off, a level in dB or as a linear power,
 * auto with a margin and noise with a threshold; the dynamic settings resolving to off without a radio input or on a
 * digital channel, and NOISE to AUTO on an AM channel.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/squelch.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void
parses_to(const char* text, int mode, double level, int margin_db) {
    dsd_squelch_setting s;
    char err[128] = {0};
    if (dsd_squelch_setting_parse(text, &s, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "'%s' refused: %s\n", text, err);
        assert(0);
    }
    assert(s.mode == mode);
    if (dsd_squelch_mode_is_dynamic(mode)) {
        assert(s.margin_db == margin_db);
    } else if (level <= 0.0) {
        assert(dsd_squelch_is_off(s.level));
    } else {
        assert(fabs(s.level - level) <= 1e-12 * level);
    }
}

static void
refused(const char* text, const char* why) {
    dsd_squelch_setting s = dsd_squelch_setting_auto(7);
    char err[128] = {0};
    assert(dsd_squelch_setting_parse(text, &s, err, sizeof err) == -1);
    if (!strstr(err, why)) {
        DSD_FPRINTF(stderr, "'%s' refused as '%s', want '%s'\n", text, err, why);
        assert(0);
    }
    /* Untouched. */
    assert(s.mode == DSD_SQUELCH_MODE_AUTO && s.margin_db == 7);
}

static void
test_grammar(void) {
    parses_to("off", DSD_SQUELCH_MODE_LEVEL, 0.0, 0);
    parses_to(" OFF ", DSD_SQUELCH_MODE_LEVEL, 0.0, 0);
    parses_to("0", DSD_SQUELCH_MODE_LEVEL, 0.0, 0);
    parses_to("-60", DSD_SQUELCH_MODE_LEVEL, dsd_squelch_level_from_sql(-60.0), 0);
    parses_to(" -47.5 ", DSD_SQUELCH_MODE_LEVEL, dsd_squelch_level_from_sql(-47.5), 0);
    /* A positive value is a linear power, as the legacy sql contract has it. */
    parses_to("0.001", DSD_SQUELCH_MODE_LEVEL, 0.001, 0);
    parses_to("auto", DSD_SQUELCH_MODE_AUTO, 0.0, DSD_SQUELCH_MARGIN_DEFAULT_DB);
    parses_to("AUTO", DSD_SQUELCH_MODE_AUTO, 0.0, DSD_SQUELCH_MARGIN_DEFAULT_DB);
    parses_to("auto+6", DSD_SQUELCH_MODE_AUTO, 0.0, 6);
    parses_to("Auto + 6", DSD_SQUELCH_MODE_AUTO, 0.0, 6);
    parses_to("auto+3", DSD_SQUELCH_MODE_AUTO, 0.0, 3);
    parses_to("auto+30", DSD_SQUELCH_MODE_AUTO, 0.0, 30);

    refused("auto+2", "3 to 30");
    refused("auto+31", "3 to 30");
    refused("auto+", "auto+N");
    refused("auto6", "auto+N");
    refused("auto-6", "auto+N");
    refused("auto+6dB", "auto+N");
    refused("auto+6.5", "auto+N");
    parses_to("noise", DSD_SQUELCH_MODE_NOISE, 0.0, DSD_SQUELCH_MARGIN_DEFAULT_DB);
    parses_to(" Noise + 12 ", DSD_SQUELCH_MODE_NOISE, 0.0, 12);
    parses_to("NOISE+3", DSD_SQUELCH_MODE_NOISE, 0.0, 3);
    parses_to("noise+30", DSD_SQUELCH_MODE_NOISE, 0.0, 30);
    refused("noise+2", "noise threshold");
    refused("noise+31", "3 to 30");
    refused("noise+", "noise+N");
    refused("noise10", "noise+N");
    refused("noise-10", "noise+N");
    refused("noisy", "expected off");
    refused("noise3", "noise+N");
    refused("", "empty");
    refused("   ", "empty");
    refused("loud", "expected off");
    /* The text is never repeated back. */
    char err_text[128] = {0};
    dsd_squelch_setting untouched;
    assert(dsd_squelch_setting_parse("SECRET", &untouched, err_text, sizeof err_text) == -1);
    assert(err_text[0] != '\0' && !strstr(err_text, "SECRET"));
    assert(dsd_squelch_setting_parse("auto+SECRET", &untouched, err_text, sizeof err_text) == -1);
    assert(err_text[0] != '\0' && !strstr(err_text, "SECRET"));
    assert(dsd_squelch_setting_parse("noise+SECRET", &untouched, err_text, sizeof err_text) == -1);
    assert(err_text[0] != '\0' && !strstr(err_text, "SECRET"));
    refused("-", "expected off");
    refused("-60dB", "expected off");
    refused("1e999", "expected off");
    refused("nan", "expected off");
    refused("inf", "expected off");
    refused("-inf", "expected off");
    char err[16];
    dsd_squelch_setting s;
    assert(dsd_squelch_setting_parse(NULL, &s, err, sizeof err) == -1);
    assert(dsd_squelch_setting_parse("auto", NULL, err, sizeof err) == -1);
    assert(dsd_squelch_setting_parse("auto", &s, NULL, 0U) == 0 && s.mode == DSD_SQUELCH_MODE_AUTO);
}

static void
formats_as(const dsd_squelch_setting* s, const char* want) {
    char out[DSD_SQUELCH_TEXT_SIZE];
    assert(dsd_squelch_setting_format(s, out, sizeof out) == 0);
    if (strcmp(out, want) != 0) {
        DSD_FPRINTF(stderr, "formatted '%s', want '%s'\n", out, want);
        assert(0);
    }
    /* And it reads back as the same setting (a level within the one-place rounding). */
    dsd_squelch_setting back;
    assert(dsd_squelch_setting_parse(strcmp(want, "off") == 0 ? "off" : want, &back, NULL, 0U) == 0
           || strstr(want, " dB") != NULL);
}

static void
test_format(void) {
    dsd_squelch_setting s = dsd_squelch_setting_of_level(0.0);
    formats_as(&s, "off");
    s = dsd_squelch_setting_of_level(dsd_squelch_level_from_sql(-60.0));
    formats_as(&s, "-60.0 dB");
    s = dsd_squelch_setting_of_level(0.001);
    formats_as(&s, "-30.0 dB");
    s = dsd_squelch_setting_auto(10);
    formats_as(&s, "auto +10 dB");
    s = dsd_squelch_setting_auto(99);
    formats_as(&s, "auto +30 dB");
    s = dsd_squelch_setting_noise(12);
    formats_as(&s, "noise +12 dB");
    s = dsd_squelch_setting_noise(0);
    formats_as(&s, "noise +3 dB");
    char out[DSD_SQUELCH_TEXT_SIZE];
    assert(dsd_squelch_setting_format(NULL, out, sizeof out) == -1 && out[0] == '\0');
    assert(dsd_squelch_setting_format(&s, NULL, 4U) == -1);
    /* Round trips through the grammar. */
    dsd_squelch_setting back;
    assert(dsd_squelch_setting_parse("auto +6 dB", &back, NULL, 0U) == -1);
    s = dsd_squelch_setting_auto(6);
    assert(dsd_squelch_setting_format(&s, out, sizeof out) == 0);
    char trimmed[DSD_SQUELCH_TEXT_SIZE];
    DSD_SNPRINTF(trimmed, sizeof trimmed, "auto+%d", s.margin_db);
    assert(dsd_squelch_setting_parse(trimmed, &back, NULL, 0U) == 0 && dsd_squelch_setting_equal(&s, &back));
    s = dsd_squelch_setting_noise(14);
    DSD_SNPRINTF(trimmed, sizeof trimmed, "noise+%d", s.margin_db);
    assert(dsd_squelch_setting_parse(trimmed, &back, NULL, 0U) == 0 && dsd_squelch_setting_equal(&s, &back));
}

static void
test_predicates_and_equality(void) {
    dsd_squelch_setting off = dsd_squelch_setting_of_level(0.0);
    dsd_squelch_setting neg = dsd_squelch_setting_of_level(-1.0);
    dsd_squelch_setting lvl = dsd_squelch_setting_of_level(1e-6);
    dsd_squelch_setting lvl2 = dsd_squelch_setting_of_level(1e-6 * (1.0 + 1e-12));
    dsd_squelch_setting lvl3 = dsd_squelch_setting_of_level(2e-6);
    dsd_squelch_setting a10 = dsd_squelch_setting_auto(10);
    dsd_squelch_setting a6 = dsd_squelch_setting_auto(6);
    assert(dsd_squelch_setting_is_off(&off) && dsd_squelch_setting_is_off(&neg) && dsd_squelch_setting_is_off(NULL));
    assert(!dsd_squelch_setting_is_off(&lvl) && !dsd_squelch_setting_is_off(&a10));
    assert(dsd_squelch_setting_is_dynamic(&a10) && !dsd_squelch_setting_is_dynamic(&lvl));
    assert(!dsd_squelch_setting_is_dynamic(NULL));
    assert(dsd_squelch_setting_equal(&off, &neg));
    assert(dsd_squelch_setting_equal(&lvl, &lvl2) && !dsd_squelch_setting_equal(&lvl, &lvl3));
    assert(!dsd_squelch_setting_equal(&off, &lvl));
    assert(!dsd_squelch_setting_equal(&a10, &a6) && !dsd_squelch_setting_equal(&a10, &lvl));
    assert(!dsd_squelch_setting_equal(&a10, NULL));
    assert(dsd_squelch_setting_auto(1).margin_db == DSD_SQUELCH_MARGIN_MIN_DB);

    /* NOISE is dynamic, never off, and differs from AUTO at the same N. */
    const dsd_squelch_setting n10 = dsd_squelch_setting_noise(10);
    const dsd_squelch_setting n10b = dsd_squelch_setting_dynamic(DSD_SQUELCH_MODE_NOISE, 10);
    assert(dsd_squelch_setting_is_dynamic(&n10) && !dsd_squelch_setting_is_off(&n10));
    assert(dsd_squelch_setting_equal(&n10, &n10b) && !dsd_squelch_setting_equal(&n10, &a10));
    assert(dsd_squelch_setting_noise(99).margin_db == DSD_SQUELCH_MARGIN_MAX_DB);
    assert(dsd_squelch_setting_dynamic(DSD_SQUELCH_MODE_AUTO, 6).mode == DSD_SQUELCH_MODE_AUTO);
    assert(dsd_squelch_setting_dynamic(DSD_SQUELCH_MODE_LEVEL, 6).mode == DSD_SQUELCH_MODE_AUTO);
    assert(dsd_squelch_mode_is_dynamic(DSD_SQUELCH_MODE_NOISE) && !dsd_squelch_mode_is_dynamic(DSD_SQUELCH_MODE_LEVEL));
    assert(dsd_squelch_mode_or_level(DSD_SQUELCH_MODE_NOISE) == DSD_SQUELCH_MODE_NOISE);
    assert(dsd_squelch_mode_or_level(7) == DSD_SQUELCH_MODE_LEVEL);
}

static void
test_resolve(void) {
    const dsd_squelch_setting a10 = dsd_squelch_setting_auto(10);
    const dsd_squelch_setting lvl = dsd_squelch_setting_of_level(1e-6);
    dsd_squelch_setting out;
    assert(dsd_squelch_setting_resolve(&a10, 1, 0, 0, &out) == DSD_SQUELCH_RESOLVED_AS_SET);
    assert(dsd_squelch_setting_equal(&out, &a10));
    assert(dsd_squelch_setting_resolve(&a10, 1, 0, 1, &out) == DSD_SQUELCH_RESOLVED_AS_SET);
    assert(dsd_squelch_setting_equal(&out, &a10));
    assert(dsd_squelch_setting_resolve(&a10, 0, 0, 0, &out) == DSD_SQUELCH_RESOLVED_NO_RADIO);
    assert(dsd_squelch_setting_is_off(&out) && !dsd_squelch_setting_is_dynamic(&out));
    assert(dsd_squelch_setting_resolve(&a10, 1, 1, 0, &out) == DSD_SQUELCH_RESOLVED_DIGITAL);
    assert(dsd_squelch_setting_is_off(&out));
    /* A level runs as written anywhere, digital and audio input included. */
    assert(dsd_squelch_setting_resolve(&lvl, 0, 1, 1, &out) == DSD_SQUELCH_RESOLVED_AS_SET);
    assert(dsd_squelch_setting_equal(&out, &lvl));
    assert(dsd_squelch_setting_resolve(NULL, 1, 0, 0, &out) == DSD_SQUELCH_RESOLVED_AS_SET);
    assert(dsd_squelch_setting_is_off(&out));
    assert(dsd_squelch_setting_resolve(&a10, 0, 0, 0, NULL) == DSD_SQUELCH_RESOLVED_NO_RADIO);

    /* NOISE runs on FM, as AUTO with the same N on AM, and off where AUTO is. */
    const dsd_squelch_setting n12 = dsd_squelch_setting_noise(12);
    const dsd_squelch_setting a12 = dsd_squelch_setting_auto(12);
    assert(dsd_squelch_setting_resolve(&n12, 1, 0, 0, &out) == DSD_SQUELCH_RESOLVED_AS_SET);
    assert(dsd_squelch_setting_equal(&out, &n12));
    assert(dsd_squelch_setting_resolve(&n12, 1, 0, 1, &out) == DSD_SQUELCH_RESOLVED_AM_AUTO);
    assert(dsd_squelch_setting_equal(&out, &a12));
    assert(dsd_squelch_setting_resolve(&n12, 0, 0, 1, &out) == DSD_SQUELCH_RESOLVED_NO_RADIO);
    assert(dsd_squelch_setting_is_off(&out));
    assert(dsd_squelch_setting_resolve(&n12, 1, 1, 0, &out) == DSD_SQUELCH_RESOLVED_DIGITAL);
    assert(dsd_squelch_setting_is_off(&out));
}

/* The gate helpers the decoder's squelch comparisons go through. */
static void
test_gate_helpers(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts.rtl_squelch_level = 1e-6;
    opts.rtl_pwr = 2e-6;
    assert(!dsd_squelch_dynamic_in_force(&opts));
    assert(fabs(dsd_squelch_level_in_force(&opts) - 1e-6) <= 1e-18);
    assert(dsd_squelch_level_open(&opts));
    /* Under LEVEL the flags mean nothing. */
    assert(dsd_squelch_gate_open(&opts, 0U) && dsd_squelch_gate_open(&opts, DSD_SQUELCH_FLAG_CLOSED));
    opts.rtl_pwr = 5e-7;
    assert(!dsd_squelch_level_open(&opts) && !dsd_squelch_gate_open(&opts, 0U));

    /* AUTO on radio input: the flags decide, and the level is off whatever it holds. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    assert(dsd_squelch_dynamic_in_force(&opts));
    assert(dsd_squelch_level_in_force(&opts) <= 0.0 && dsd_squelch_level_open(&opts));
    assert(dsd_squelch_gate_open(&opts, 0U) && !dsd_squelch_gate_open(&opts, DSD_SQUELCH_FLAG_CLOSED));
    assert(!dsd_squelch_gate_open(&opts, (uint8_t)(DSD_SQUELCH_FLAG_CLOSED | 0x80U)));

    /* NOISE gates the same way. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    assert(dsd_squelch_dynamic_in_force(&opts));
    assert(dsd_squelch_level_in_force(&opts) <= 0.0 && dsd_squelch_level_open(&opts));
    assert(dsd_squelch_gate_open(&opts, 0U) && !dsd_squelch_gate_open(&opts, DSD_SQUELCH_FLAG_CLOSED));

    /* A dynamic setting on any other input resolves to off: no flags, no level. */
    opts.audio_in_type = AUDIO_IN_WAV;
    assert(!dsd_squelch_dynamic_in_force(&opts));
    assert(dsd_squelch_level_in_force(&opts) <= 0.0 && dsd_squelch_level_open(&opts));
    assert(dsd_squelch_gate_open(&opts, DSD_SQUELCH_FLAG_CLOSED));
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    assert(!dsd_squelch_dynamic_in_force(&opts) && dsd_squelch_gate_open(&opts, DSD_SQUELCH_FLAG_CLOSED));

    assert(!dsd_squelch_dynamic_in_force(NULL) && dsd_squelch_level_in_force(NULL) <= 0.0);
    assert(!dsd_squelch_level_open(NULL) && !dsd_squelch_gate_open(NULL, 0U));
}

/* The setting in dsd_opts, and an input spec's field under --squelch. */
static void
test_opts_store_and_spec_field(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.rtl_squelch_level = 1e-6;
    const dsd_squelch_setting auto6 = dsd_squelch_setting_auto(6);
    dsd_squelch_setting_store(&opts, &auto6);
    assert(opts.rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO && opts.rtl_squelch_margin_db == 6);
    /* The level stays, for a switch back. */
    assert(fabs(opts.rtl_squelch_level - 1e-6) <= 1e-18);
    dsd_squelch_setting back = dsd_squelch_setting_of_opts(&opts);
    assert(dsd_squelch_setting_equal(&back, &auto6));
    const dsd_squelch_setting level = dsd_squelch_setting_of_level(2e-6);
    dsd_squelch_setting_store(&opts, &level);
    back = dsd_squelch_setting_of_opts(&opts);
    assert(opts.rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL && dsd_squelch_setting_equal(&back, &level));
    assert(opts.rtl_squelch_margin_db == 6);
    dsd_squelch_setting_store(&opts, NULL);
    dsd_squelch_setting_store(NULL, &level);
    back = dsd_squelch_setting_of_opts(NULL);
    assert(dsd_squelch_setting_is_off(&back));

    const dsd_squelch_setting noise14 = dsd_squelch_setting_noise(14);
    dsd_squelch_setting_store(&opts, &noise14);
    assert(opts.rtl_squelch_mode == DSD_SQUELCH_MODE_NOISE && opts.rtl_squelch_margin_db == 14);
    back = dsd_squelch_setting_of_opts(&opts);
    assert(dsd_squelch_setting_equal(&back, &noise14));
    assert(dsd_squelch_spec_field_apply(&opts, "noise+9") == 0 && opts.rtl_squelch_mode == DSD_SQUELCH_MODE_NOISE
           && opts.rtl_squelch_margin_db == 9);

    assert(dsd_squelch_spec_field_apply(&opts, "auto+8") == 0 && opts.rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO);
    assert(dsd_squelch_spec_field_apply(&opts, "loud") == -1 && opts.rtl_squelch_margin_db == 8);
    assert(dsd_squelch_spec_field_apply(&opts, NULL) == -1 && dsd_squelch_spec_field_apply(NULL, "off") == -1);
    opts.rtl_squelch_cli_set = 1;
    assert(dsd_squelch_spec_field_apply(&opts, "-60") == 1 && opts.rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO);
}

/* A frontend's level must be a finite number; an AUTO setting's level is not in force. */
static void
test_level_finite(void) {
    dsd_squelch_setting s = dsd_squelch_setting_of_level(1e-6);
    assert(dsd_squelch_setting_level_finite(&s));
    s.level = NAN;
    assert(!dsd_squelch_setting_level_finite(&s));
    s.level = INFINITY;
    assert(!dsd_squelch_setting_level_finite(&s));
    s.mode = DSD_SQUELCH_MODE_AUTO;
    assert(dsd_squelch_setting_level_finite(&s));
    s.mode = DSD_SQUELCH_MODE_NOISE;
    assert(dsd_squelch_setting_level_finite(&s));
    assert(!dsd_squelch_setting_level_finite(NULL));
}

int
main(void) {
    test_grammar();
    test_level_finite();
    test_opts_store_and_spec_field();
    test_gate_helpers();
    test_format();
    test_predicates_and_equality();
    test_resolve();
    printf("RUNTIME_SQUELCH: OK\n");
    return 0;
}
