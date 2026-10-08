// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #575: the frequency the receiver is tuned to, as every frontend reads it (dsd_opts_tuned_freq_hz()), the note
 * the replay read paths leave for it (dsd_opts_note_iq_replay_center()), and the I/Q replay predicate the frontends'
 * tune refusal keys on (dsd_opts_input_is_iq_replay()). A replay's recorded RETUNEs
 * move the demod without touching rtlsdr_center_freq, so the centre the decoder last read samples from wins while it
 * is set; an audio input has no tuner reading at all.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

static dsd_opts*
make_opts(int audio_in_type, const char* audio_in_dev) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    opts->audio_in_type = audio_in_type;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", audio_in_dev);
    opts->rtlsdr_center_freq = 851012500U;
    return opts;
}

static void
test_tuned_freq(void) {
    assert(dsd_opts_tuned_freq_hz(NULL) == 0U);

    /* An audio input (a rigctl peer's audio included) carries no tuner reading, a stale replay centre neither. */
    dsd_opts* opts = make_opts(AUDIO_IN_PULSE, "pulse");
    assert(dsd_opts_tuned_freq_hz(opts) == 0U);
    opts->iq_replay_center_freq = 851500000U;
    assert(dsd_opts_tuned_freq_hz(opts) == 0U);
    opts->audio_in_type = AUDIO_IN_WAV;
    assert(dsd_opts_tuned_freq_hz(opts) == 0U);
    free(opts);

    /* A live radio input reads the centre DSD-neo tuned it to. */
    opts = make_opts(AUDIO_IN_RTL, "rtl:0");
    assert(dsd_opts_tuned_freq_hz(opts) == 851012500U);

    /* During a replay the centre the samples being decoded were captured on wins, and once the replay stops (0) the
       tuned centre reads again. */
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "iqreplay:capture.iq.json");
    assert(dsd_opts_tuned_freq_hz(opts) == 851012500U);
    opts->iq_replay_center_freq = 851500000U;
    assert(dsd_opts_tuned_freq_hz(opts) == 851500000U);
    opts->iq_replay_center_freq = 0U;
    assert(dsd_opts_tuned_freq_hz(opts) == 851012500U);

    /* The full unsigned range: a SoapySDR or Airspy centre above 2^31 Hz reads as itself. */
    opts->rtlsdr_center_freq = 2400000000U;
    assert(dsd_opts_tuned_freq_hz(opts) == 2400000000U);
    free(opts);
}

/* The replay read paths note each sample's capture centre (dsd_opts_note_iq_replay_center()): a centre is stored, and
   none (0: a live read) leaves the reading as it was. */
static void
test_note_iq_replay_center(void) {
    dsd_opts_note_iq_replay_center(NULL, 851500000U);

    dsd_opts* opts = make_opts(AUDIO_IN_RTL, "iqreplay:capture.iq.json");
    dsd_opts_note_iq_replay_center(opts, 0U);
    assert(opts->iq_replay_center_freq == 0U);
    dsd_opts_note_iq_replay_center(opts, 851500000U);
    assert(opts->iq_replay_center_freq == 851500000U);
    assert(dsd_opts_tuned_freq_hz(opts) == 851500000U);
    dsd_opts_note_iq_replay_center(opts, 0U);
    assert(opts->iq_replay_center_freq == 851500000U);
    dsd_opts_note_iq_replay_center(opts, 851625000U);
    assert(dsd_opts_tuned_freq_hz(opts) == 851625000U);
    free(opts);
}

static void
test_input_is_iq_replay(void) {
    assert(dsd_opts_input_is_iq_replay(NULL) == 0);

    dsd_opts* opts = make_opts(AUDIO_IN_RTL, "iqreplay:capture.iq.json");
    assert(dsd_opts_input_is_iq_replay(opts) == 1);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "iqreplay");
    assert(dsd_opts_input_is_iq_replay(opts) == 1);

    /* A live radio is no replay, whatever the session's replay flags still say. */
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "rtl:0:851.0125M");
    opts->iq_replay_requested = 1;
    opts->iq_replay_active = 1;
    assert(dsd_opts_input_is_iq_replay(opts) == 0);

    /* Nor is a PCM input that a replay spec still names: the input in force is the type. */
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "iqreplay:capture.iq.json");
    opts->audio_in_type = AUDIO_IN_WAV;
    assert(dsd_opts_input_is_iq_replay(opts) == 0);
    free(opts);
}

int
main(void) {
    test_tuned_freq();
    test_note_iq_replay_center();
    test_input_is_iq_replay();
    return 0;
}
