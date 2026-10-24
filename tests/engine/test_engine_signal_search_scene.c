// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Signal search against the real capture-rate detector and its shared tap: one NFM transmitter on a 5 kHz grid with
   16 kHz windows, fed as capture-rate I/Q for whatever the coordinator tunes. Over several passes and capture
   alignments, the transmitter's own channel is the only one ever visited: the empty channels its sidebands and the
   windows' leakage reach are never tuned. A search started on a tap that a stopped stream has emptied waits for the
   next stream's first block. While listening (an NFM visit through the main-loop tick, a digitally confirmed one through
   the in-frame yield check alone), the live strength follows the transmitter's level, and a level the tap stops
   refreshing (drops or clipping) is withdrawn within a bound, then returns once measurements succeed again. */

#include <assert.h>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/signal_search.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* The coordinator reads this realtime clock (dsd_realtime_mono_ms is mapped here): the system clock, or a test clock
   that a listening scene advances with its blocks. */
#undef dsd_realtime_mono_ms
uint64_t dsd_realtime_mono_ms(void);

enum { RATE = 1536000, BLOCK = 8192, TRANSMITTER = 163012500 };

static dsd_signal_capture capture = {RATE, 0, 1, RATE / 4, RATE / 4, 0, 0};
static dsd_signal_search_status status;
static uint32_t ticket, target, generation = 1;
static int tune_pending, gain_value = 123, gain_auto, autogain = 1;
static uint64_t now_ms = 1000, last_hit;
static uint32_t visited[64];
static int visits;
/* The transmitter's amplitude, and whether a carrier holds the visit open. */
static double level = 0.3;
static int carrier_open;

/* While set, the realtime clock is fixed at a base the scene advances. The tap still stamps its blocks with the system
   clock, which the coordinator's capture-age check (10 s) compares with this one: the base starts SCENE_CLOCK_LEAD_MS
   behind the system clock, and a scene's bounded loops advance it by at most SCENE_MAX_ADVANCE_MS, so this clock never
   reaches a stamp and the check never reads a false stall, however slowly the scene runs. */
enum { SCENE_CLOCK_LEAD_MS = 10000, SCENE_MAX_ADVANCE_MS = 9460 };

static int scene_clock;
static uint64_t scene_base_ms, scene_offset_ms;
/* Statuses published while listening: a live level shown after the scene stopped the tap from refreshing it. */
static int track_stale;
static uint64_t stale_after_ms;
static int stale_shown;

uint64_t
test_dsd_realtime_mono_ms(void) {
    return scene_clock ? scene_base_ms + scene_offset_ms : dsd_realtime_mono_ms();
}

static void
observer(const dsd_signal_search_status* s, const dsd_signal_search_hit* h, void* ctx) {
    (void)h;
    (void)ctx;
    status = *s;
    if (track_stale && s->live_power_valid && test_dsd_realtime_mono_ms() >= stale_after_ms) {
        ++stale_shown;
    }
    /* An ended visit has no current level. */
    assert(!(s->hit.ended && s->live_power_valid));
    if (s->hit.hit_id && s->hit.hit_id != last_hit) {
        last_hit = s->hit.hit_id;
        assert(visits < 64);
        visited[visits++] = s->hit.frequency_hz;
    }
}

int
rtl_stream_is_active(void) {
    return 1;
}

uint32_t
rtl_stream_output_generation(void) {
    return generation;
}

int
rtl_stream_tune_async(uint32_t hz, uint64_t owner, uint32_t* out) {
    (void)owner;
    target = hz;
    *out = ++ticket;
    tune_pending = 1;
    return 0;
}

int
rtl_stream_tune_poll(uint32_t id, int* result) {
    assert(id == ticket);
    if (tune_pending) {
        return 0;
    }
    *result = 0;
    return 1;
}

int
rtl_stream_get_gain(int* gain, int* automatic) {
    *gain = gain_value;
    *automatic = gain_auto;
    return 0;
}

int
rtl_stream_get_tuner_autogain(void) {
    return autogain;
}

void
rtl_stream_enforce_tuner_autogain(int value) {
    autogain = value;
}

void
rtl_stream_clear_pending_retune_profile(void) {}

int
rtl_stream_airspy_controls(const dsd_airspy_config* cfg) {
    (void)cfg;
    return 0;
}

int
rtl_stream_request_analog_profile(int f, int kind, int width) {
    (void)f;
    (void)kind;
    (void)width;
    return 0;
}

int
rtl_stream_prepare_retune_analog_profile_for_target(uint32_t hz, const rtl_stream_retune_analog_profile* p) {
    (void)hz;
    (void)p;
    return 0;
}

void
rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t hz, int cq, int rate, int levels, int profile, int sps,
                                                       int persist, const rtl_stream_retune_gain_profile* g) {
    (void)hz;
    (void)cq;
    (void)rate;
    (void)levels;
    (void)profile;
    (void)sps;
    (void)persist;
    gain_value = g->tuner_gain_tenth_db;
    gain_auto = g->tuner_gain_is_auto;
    autogain = g->tuner_autogain_on;
}

int
dsd_audio_ensure_analog_output(dsd_opts* opts) {
    (void)opts;
    return 0;
}

int
dsd_audio_ensure_digital_output(dsd_opts* opts, const dsd_state* state) {
    (void)state;
    (void)opts;
    return 0;
}

/* Unless a scene opens it, no carrier holds a visit open: each ends quiet once the monitor has judged it. */
int
dsd_scan_analog_carrier_open(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return carrier_open;
}

int
dsd_analog_rx_carrier_observed_now(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return 1;
}

void
dsd_engine_carrier_boundary(dsd_opts* opts, dsd_state* state, dsd_carrier_boundary_kind reason, int guard) {
    (void)opts;
    (void)state;
    (void)reason;
    (void)guard;
}

void
dsd_frame_sync_reset_acquisition(const dsd_opts* opts, dsd_state* state, int clear) {
    (void)opts;
    (void)state;
    (void)clear;
}

/* One block of the scene as the current capture sees it: the transmitter (2.5 kHz deviation, 1 kHz tone, amplitude
   @c level) over a low noise floor, continuous across blocks. */
static void
feed(void) {
    static float iq[2 * BLOCK];
    static double phase;
    static uint64_t sample;
    static uint32_t rng = 0x2468ace1u;
    for (size_t i = 0; i < BLOCK; ++i, ++sample) {
        const double tone = sin(6.283185307179586 * 1000.0 * (double)sample / RATE);
        phase +=
            6.283185307179586 * ((double)TRANSMITTER - capture.capture_hz + capture.rotation_hz + 2500.0 * tone) / RATE;
        phase = fmod(phase, 6.283185307179586);
        double noise[2];
        for (int q = 0; q < 2; ++q) {
            rng = rng * 1664525u + 1013904223u;
            noise[q] = ((double)rng / UINT32_MAX - 0.5) * 0.002;
        }
        iq[2 * i] = (float)(level * cos(phase) + noise[0]);
        iq[2 * i + 1] = (float)(level * sin(phase) + noise[1]);
    }
    rtl_signal_measure_feed(iq, BLOCK, &capture);
}

/* A session over 50 kHz of 5 kHz channels from @p start; @p digital as dsd_signal_search_range's. */
static void
search_start_range(dsd_opts* o, dsd_state* s, uint64_t session, uint32_t start, int digital) {
    dsd_signal_search_config config;
    DSD_MEMSET(&config, 0, sizeof(config));
    config.session_id = session;
    config.range_count = 1;
    dsd_signal_search_range_default(&config.ranges[0]);
    config.ranges[0].start_hz = start;
    config.ranges[0].end_hz = start + 50000;
    config.ranges[0].step_num = 5000;
    config.ranges[0].bandwidth_hz = 16000;
    config.ranges[0].digital = digital;
    config.ranges[0].max_visit_ms = 0;
    char error[192];
    assert(dsd_engine_signal_search_start(o, s, &config, observer, NULL, error, sizeof(error)) == 0);
}

static void
search_start(dsd_opts* o, dsd_state* s, uint64_t session, uint32_t start) {
    search_start_range(o, s, session, start, 0);
}

/* A stopped stream leaves the tap empty (rtl_signal_measure_reset()). A search started on the next stream before that
   stream's first block waits for it, then plans its first span from it. */
static void
search_before_first_block(dsd_opts* o, dsd_state* s, uint32_t start) {
    DSD_MEMSET(&status, 0, sizeof(status));
    capture.capture_hz = start + RATE / 4;
    feed(); /* the previous stream's last block */
    rtl_signal_measure_reset();
    dsd_signal_capture none;
    assert(!rtl_signal_capture_get(&none) && rtl_signal_capture_last_ms() == 0);
    search_start(o, s, 41, start);
    for (int step = 0; step < 3; ++step) {
        (void)dsd_engine_signal_search_tick(o, s);
        assert(status.phase == DSD_SEARCH_SEARCHING && !tune_pending);
    }
    feed(); /* the new stream's first block */
    (void)dsd_engine_signal_search_tick(o, s);
    assert(status.phase == DSD_SEARCH_TUNING && tune_pending);
    tune_pending = 0;
    dsd_engine_signal_search_shutdown(o, s);
    assert(!dsd_engine_signal_search_active(s));
}

/* A session over 50 kHz of 5 kHz channels from @p start: tune, measure and visit until three passes have finished. */
static void
search_from(dsd_opts* o, dsd_state* s, uint64_t session, uint32_t start) {
    visits = 0;
    last_hit = 0;
    DSD_MEMSET(&status, 0, sizeof(status));
    capture.capture_hz = start + RATE / 4;
    feed();
    search_start(o, s, session, start);
    for (int step = 0; step < 20000 && status.pass < 4; ++step) {
        (void)dsd_engine_signal_search_tick(o, s);
        assert(status.phase != DSD_SEARCH_ERROR);
        if (tune_pending) {
            tune_pending = 0;
            ++generation;
            capture.generation = generation;
            capture.capture_hz = target + RATE / 4;
        }
        feed();
        now_ms += 5;
        dsd_decode_clock_test_set_ns(now_ms * 1000000);
    }
    assert(status.pass >= 4);
    dsd_engine_signal_search_shutdown(o, s);
    assert(!dsd_engine_signal_search_active(s));
    /* Every pass visited the transmitter, and nothing else. */
    assert(visits >= 3);
    for (int i = 0; i < visits; ++i) {
        assert(visited[i] == TRANSMITTER);
    }
}

/* One scene step: a tune the coordinator queued lands with fresh samples, the next block arrives (@p drops counts a
   producer drop with it) and both clocks advance by about its duration. */
static void
scene_step(int drops) {
    if (tune_pending) {
        tune_pending = 0;
        ++generation;
        capture.generation = generation;
        capture.capture_hz = target + RATE / 4;
    }
    capture.drops += (uint64_t)drops;
    feed();
    now_ms += 5;
    scene_offset_ms += 5;
    dsd_decode_clock_test_set_ns(now_ms * 1000000);
}

/* Listening services the live strength from the main-loop tick (@p yield_path 0) or, for a digitally confirmed
   transmission that keeps the decoder in its synced-frame loop, from the in-frame yield check alone. */
static void
listen_step(dsd_opts* o, dsd_state* s, int yield_path, int drops) {
    if (yield_path) {
        assert(!dsd_engine_signal_search_should_yield(o, s));
        /* Validated frames keep the confirmed visit active. */
        if (now_ms % 100 == 0) {
            dsd_engine_signal_search_evidence(s, "DMR", 0, 0, NULL);
        }
    } else {
        (void)dsd_engine_signal_search_tick(o, s);
    }
    assert(status.phase == DSD_SEARCH_LISTENING);
    scene_step(drops);
}

/* Listens until a live strength within @p tolerance of @p expected is published; fails after @p bound_ms. */
static float
live_reaches(dsd_opts* o, dsd_state* s, int yield_path, float expected, float tolerance, uint64_t bound_ms) {
    const uint64_t until = scene_offset_ms + bound_ms;
    while (!(status.live_power_valid && fabsf(status.live_power_dbfs - expected) <= tolerance)) {
        assert(scene_offset_ms < until);
        listen_step(o, s, yield_path, 0);
    }
    return status.live_power_dbfs;
}

/* A landed listening visit to the transmitter whose live strength follows level changes, is withdrawn once the tap
   stops refreshing it (@p clip: clipped samples, otherwise a producer drop with every block), returns afterwards, and
   ends with the visit. */
static void
live_strength_scene(dsd_opts* o, dsd_state* s, uint64_t session, int yield_path, int clip) {
    const uint32_t start = 162987500;
    visits = 0;
    last_hit = 0;
    level = 0.3;
    DSD_MEMSET(&status, 0, sizeof(status));
    scene_clock = 1;
    const uint64_t system_ms = dsd_realtime_mono_ms();
    scene_base_ms = system_ms > SCENE_CLOCK_LEAD_MS ? system_ms - SCENE_CLOCK_LEAD_MS : 0;
    scene_offset_ms = 0;
    carrier_open = !yield_path;
    capture.capture_hz = start + RATE / 4;
    feed();
    search_start_range(o, s, session, start, yield_path ? 1 : 0);
    for (int step = 0;
         step < 400 && !(status.hit.hit_id && !status.hit.ended && status.phase >= DSD_SEARCH_IDENTIFYING); ++step) {
        (void)dsd_engine_signal_search_tick(o, s);
        assert(status.phase != DSD_SEARCH_ERROR);
        scene_step(0);
    }
    assert(status.hit.frequency_hz == TRANSMITTER);
    if (yield_path) {
        assert(status.phase == DSD_SEARCH_IDENTIFYING);
        dsd_engine_signal_search_evidence(s, "DMR", 0, 0, NULL);
        assert(status.hit.confirmed);
    }
    assert(status.phase == DSD_SEARCH_LISTENING && !status.live_power_valid);

    /* The first reading, then a 12 dB fade and a 6 dB rise, each reached within three intervals and a measurement. */
    const float first = live_reaches(o, s, yield_path, -10.5f, 1.0f, 800);
    level = 0.3 * pow(10.0, -12.0 / 20.0);
    (void)live_reaches(o, s, yield_path, first - 12.0f, 0.3f, 800);
    level = 0.3 * pow(10.0, -6.0 / 20.0);
    (void)live_reaches(o, s, yield_path, first - 6.0f, 0.3f, 800);

    /* The tap stops refreshing the level: it stays shown through single failures, then is withdrawn (and published)
       once three intervals and a retry have passed since the last accepted measurement, and is not shown again while
       the failures last. */
    const uint64_t failing = scene_offset_ms;
    if (clip) {
        level = 1.2;
    }
    while (status.live_power_valid) {
        assert(scene_offset_ms <= failing + 1750 + 10);
        listen_step(o, s, yield_path, clip ? 0 : 1);
    }
    assert(scene_offset_ms >= failing + 1750 - 300);
    track_stale = 1;
    stale_shown = 0;
    stale_after_ms = test_dsd_realtime_mono_ms();
    for (int step = 0; step < 300; ++step) {
        listen_step(o, s, yield_path, clip ? 0 : 1);
    }
    assert(!stale_shown && !status.live_power_valid);
    track_stale = 0;

    /* Measurements succeed again: a fresh level returns. */
    level = 0.3;
    (void)live_reaches(o, s, yield_path, first, 0.3f, 800);

    /* The visit ends (the carrier closes, or the confirmed transmission stops): the level goes with it. */
    carrier_open = 0;
    for (int step = 0; step < 200 && status.phase != DSD_SEARCH_TUNING; ++step) {
        (void)dsd_engine_signal_search_tick(o, s);
        scene_step(0);
    }
    assert(status.phase == DSD_SEARCH_TUNING && status.hit.ended && !status.live_power_valid);
    /* Landing (2 s), three level changes (0.8 s each), the withdrawal (1.76 s), the failures that follow (1.5 s), the
       recovery (0.8 s) and the visit's end (1 s) are each bounded. */
    assert(scene_offset_ms <= SCENE_MAX_ADVANCE_MS);
    dsd_engine_signal_search_shutdown(o, s);
    assert(!dsd_engine_signal_search_active(s));
    scene_clock = 0;
}

int
main(void) {
    dsd_opts* o = calloc(1, sizeof(*o));
    dsd_state* s = calloc(1, sizeof(*s));
    assert(o && s);
    o->audio_in_type = AUDIO_IN_RTL;
    o->rtlsdr_center_freq = 163000000;
    o->rtl_dsp_bw_khz = 48;
    s->samplesPerSymbol = 10;
    s->rtl_ctx = (void*)1;
    DSD_SNPRINTF(o->audio_in_dev, sizeof(o->audio_in_dev), "%s", "rtl:0:163M");
    dsd_decode_clock_use_test(now_ms * 1000000);
    search_before_first_block(o, s, 162987500);
    /* Each start shifts the capture by a 5 kHz step, which moves the transmitter a third of an FFT bin. */
    for (uint32_t shift = 0; shift < 6; ++shift) {
        search_from(o, s, 42 + shift, 162987500 - 5000 * shift);
    }
    live_strength_scene(o, s, 50, 0, 0);
    live_strength_scene(o, s, 51, 1, 1);
    s->rtl_ctx = NULL;
    dsd_state_ext_free_all(s);
    free(s);
    free(o);
    dsd_decode_clock_use_system();
    return 0;
}
