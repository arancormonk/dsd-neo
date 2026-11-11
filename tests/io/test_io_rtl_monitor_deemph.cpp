// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #518 B8/B9: the fixed analog gain's calibration holds whatever de-emphasis the RTL monitor runs. The reference
 * signal (1 kHz at 3 kHz deviation, AM at 50%) goes through the real demodulator, de-emphasis and output ring, and
 * the decoder's side reads it back as dsd_symbol.c does (the `vol` 2 trim) into the analog chain at `-n 50`, with
 * the stream's own de-emphasis gain and generation installed in the runtime hooks as the engine installs them.
 *
 * - Off, 50, 75 and 750 us (DSD_NEO_DEEMPH off, 50, unset, nfm): the tone plays at -15 dBFS RMS +/-0.2, and every
 *   de-emphasis within 0.05 dB of none. Before the fix 75 us read -15.9 and 750 us -28.7.
 * - Across a mode change, FM -> AM -> FM: the switch moves the generation, the gain follows (AM runs none), and each
 *   stretch plays at the reference level.
 * - At a 48 kHz and a 24 kHz live DSP rate, the 78,125 Hz an Airspy forces, and an I/Q replay that decimates by 2
 *   after the demodulator, where the de-emphasis runs at the decimated rate, not the discriminator's.
 * - A stream open starts the next session at no gain (1.0), whatever the last session published.
 *
 * A passing run prints only its OK line; a failing case prints what it measured.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_audio.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "rtl_stream_test_support.h"
#include "test_env.h"

namespace {

constexpr int kSegmentBlocks = 60; /* 600 ms of input per stretch */
constexpr int kSettleBlocks = 20;  /* the first 200 ms of a stretch settle (filters, the AM carrier estimate) */
constexpr double kReferenceDbfs = -15.0;
constexpr double kReferenceTolDb = 0.2;
constexpr double kAgreeTolDb = 0.05;

/* What the decoder's side measured over each stretch's settled blocks. */
struct ToneLevels {
    dsd_opts* opts;
    dsd_state* state;
    double sum_sq[3];
    long long count[3];
    double gain[3]; /* the gain the stream published at the end of the stretch */
    int last_segment;
};

void
tone_sink(const rtl_stream_test_tone_block* block, void* ctx) {
    ToneLevels* t = static_cast<ToneLevels*>(ctx);
    static float buf[16384];
    const int n = block->count < (int)(sizeof buf / sizeof buf[0]) ? block->count : (int)(sizeof buf / sizeof buf[0]);
    for (int i = 0; i < n; i++) {
        buf[i] = block->samples[i] * (float)t->opts->rtl_volume_multiplier;
    }
    dsd_analog_audio_block_begin(t->opts, t->state, DSD_ANALOG_AUDIO_CHAIN_MONITOR);
    (void)dsd_analog_audio_process_f(t->opts, t->state, DSD_ANALOG_AUDIO_CHAIN_MONITOR, buf, (size_t)n,
                                     DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR, block->rate_hz, DSD_ANALOG_AUDIO_PLAYING);
    const int seg = block->segment;
    if (seg != t->last_segment) {
        t->last_segment = seg;
        t->count[seg] = -kSettleBlocks;
    }
    if (t->count[seg] < 0) {
        t->count[seg]++;
    } else {
        for (int i = 0; i < n; i++) {
            t->sum_sq[seg] += (double)buf[i] * (double)buf[i];
        }
        t->count[seg] += n;
    }
    t->gain[seg] = rtl_stream_monitor_deemph_gain();
}

double
rms_dbfs(double sum_sq, long long count) {
    return count > 0 ? 10.0 * std::log10(sum_sq / (double)count / (32768.0 * 32768.0)) : -200.0;
}

struct RateCase {
    const char* name;
    int rate_hz;
    int forced_rate_hz;
    uint32_t replay_post;
};

struct DeemphCase {
    const char* name;
    const char* env; /* NULL: unset, the 75 us default */
    int tau_us;
};

/* The FM -> AM -> FM levels for one de-emphasis at one rate; @p fm_out the first FM stretch's level. */
int
run_case(const RateCase& rc, const DeemphCase& dc, double* fm_out) {
    if (dc.env) {
        (void)dsd_test_setenv("DSD_NEO_DEEMPH", dc.env, 1);
    } else {
        (void)dsd_test_unsetenv("DSD_NEO_DEEMPH");
    }
    dsd_neo_config_init();

    dsd_opts* opts = static_cast<dsd_opts*>(std::calloc(1, sizeof(dsd_opts)));
    dsd_state* state = static_cast<dsd_state*>(std::calloc(1, sizeof(dsd_state)));
    if (!opts || !state) {
        std::fprintf(stderr, "out of memory\n");
        std::exit(2);
    }
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_volume_multiplier = 2;
    opts->use_pbf = 1;
    opts->audio_gainA = 50.0f;

    ToneLevels levels = {};
    levels.opts = opts;
    levels.state = state;
    levels.last_segment = -1;
    const rtl_stream_test_tone_segment segments[3] = {
        {DSD_ANALOG_DEMOD_FM, kSegmentBlocks},
        {DSD_ANALOG_DEMOD_AM, kSegmentBlocks},
        {DSD_ANALOG_DEMOD_FM, kSegmentBlocks},
    };
    rtl_stream_test_tone_result r = {};
    int fail = 0;
    if (rtl_stream_test_monitor_tone(opts, rc.rate_hz, rc.forced_rate_hz, rc.replay_post, segments, 3, tone_sink,
                                     &levels, &r)
        != 0) {
        std::fprintf(stderr, "%s, %s: the run failed\n", rc.name, dc.name);
        fail = 1;
    }
    if (r.deemph_tau_us != dc.tau_us) {
        std::fprintf(stderr, "%s, %s: de-emphasis %d us, want %d\n", rc.name, dc.name, r.deemph_tau_us, dc.tau_us);
        fail = 1;
    }
    static const char* const kStretch[3] = {"FM", "AM", "FM after AM"};
    for (int seg = 0; seg < 3; seg++) {
        const double level = rms_dbfs(levels.sum_sq[seg], levels.count[seg]);
        if (std::fabs(level - kReferenceDbfs) > kReferenceTolDb) {
            std::fprintf(stderr, "%s, %s, %s: -n 50 plays the reference at %.3f dBFS RMS, want %.1f +/-%.1f\n", rc.name,
                         dc.name, kStretch[seg], level, kReferenceDbfs, kReferenceTolDb);
            fail = 1;
        }
    }
    /* AM runs no de-emphasis: the stream publishes none for it. */
    if (std::fabs(levels.gain[1] - 1.0) > 1e-6) {
        std::fprintf(stderr, "%s, %s: AM published gain %.6f, want 1\n", rc.name, dc.name, levels.gain[1]);
        fail = 1;
    }
    const double fm_after = rms_dbfs(levels.sum_sq[2], levels.count[2]);
    *fm_out = rms_dbfs(levels.sum_sq[0], levels.count[0]);
    if (std::fabs(fm_after - *fm_out) > kAgreeTolDb) {
        std::fprintf(stderr, "%s, %s: FM after AM %.3f dBFS, before %.3f\n", rc.name, dc.name, fm_after, *fm_out);
        fail = 1;
    }
    /* What a failing case measured, stretch by stretch. */
    for (int seg = 0; fail && seg < 3; seg++) {
        std::fprintf(stderr, "  %s, %s, %s: %.3f dBFS RMS (stream gain %.4f, de-emphasis at %d Hz)\n", rc.name, dc.name,
                     kStretch[seg], rms_dbfs(levels.sum_sq[seg], levels.count[seg]), levels.gain[seg], r.rate_out_hz);
    }
    dsd_state_ext_free_all(state);
    std::free(state);
    std::free(opts);
    return fail;
}

/* A stream open or stop starts a new session at no de-emphasis gain (1.0), whatever the last session published: the
   gain a reader takes before the new stream's first block is not the old stream's. A 750 us session runs before the
   boundary (the one dsd_rtl_stream_open() and dsd_rtl_stream_soft_stop() both move), and a 75 us one after it. */
int
test_session_resets_gain(void) {
    double fm = 0.0;
    const DeemphCase nfm = {"750 us (nfm), first session", "nfm", 750};
    const RateCase live = {"live 48 kHz", 48000, 0, 0U};
    int fail = run_case(live, nfm, &fm);
    const double published = rtl_stream_monitor_deemph_gain();
    const uint32_t session = rtl_stream_session_generation();
    rtl_stream_test_session_boundary();
    const double after = rtl_stream_monitor_deemph_gain();
    if (std::fabs(published - 4.8123) > 1e-3 || rtl_stream_session_generation() == session
        || std::fabs(after - 1.0) > 1e-9) {
        std::fprintf(stderr, "session reset: published %.4f, after the session boundary %.4f, want 4.8123 then 1\n",
                     published, after);
        fail = 1;
    }
    const DeemphCase def = {"75 us (default), second session", NULL, 75};
    fail |= run_case(live, def, &fm);
    return fail;
}

} // namespace

int
main(void) {
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.stream_generation = rtl_stream_output_generation;
    hooks.monitor_deemph_gain = rtl_stream_monitor_deemph_gain;
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    static const RateCase kRates[] = {
        {"live 48 kHz", 48000, 0, 0U},
        {"live 24 kHz", 24000, 0, 0U},
        {"live forced 78125 Hz", 48000, 78125, 0U},
        {"replay 96 kHz decimated by 2", 48000, 0, 2U},
    };
    static const DeemphCase kDeemph[] = {
        {"de-emphasis off", "off", 0},
        {"50 us", "50", 50},
        {"75 us (default)", NULL, 75},
        {"750 us (nfm)", "nfm", 750},
    };
    int fail = 0;
    for (const RateCase& rc : kRates) {
        double off_level = 0.0;
        for (const DeemphCase& dc : kDeemph) {
            double fm = 0.0;
            fail |= run_case(rc, dc, &fm);
            if (dc.tau_us == 0) {
                off_level = fm;
            } else if (std::fabs(fm - off_level) > kAgreeTolDb) {
                std::fprintf(stderr, "%s, %s: FM %.3f dBFS, %.3f with none\n", rc.name, dc.name, fm, off_level);
                fail = 1;
            }
        }
    }
    fail |= test_session_resets_gain();
    (void)dsd_test_unsetenv("DSD_NEO_DEEMPH");
    dsd_neo_config_init();
    dsd_rtl_stream_metrics_hooks_set(NULL);
    if (fail) {
        return 1;
    }
    std::printf("IO_RTL_MONITOR_DEEMPH: OK\n");
    return 0;
}
