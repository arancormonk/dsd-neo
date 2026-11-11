// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #518 B10: the FSK voice lane through the real stream. EDACS reads analog calls on the 9600 bit/s FSK
 * discriminator, whose output the modem normalises to a +/-30000 peak whatever the signal: the release register needs
 * that, but the level of the voice is gone from it. The stream now carries a second, calibrated lane beside it (the
 * discriminator ahead of the normaliser, at the FM monitor's scale and through its own de-emphasis), which the demod
 * makes, the output resampler resamples with its own history, and the output ring carries at the same positions as the
 * samples and their squelch flags, while EDACS asks for it (rtl_stream_set_voice_lane()).
 *
 * Synthetic captures replay through rtl_stream_create()/rtl_stream_start() and rtl_stream_read_lanes(), with the auto
 * squelch on (so the flags are the 9600 bit/s path's own): a 1 kHz tone at 3 kHz deviation, the same tone 20 dB down,
 * a stretch of carrier-off noise, a driver overflow (a gap the demod starts over at) and a retune, at three rates: 48 kHz
 * with no resampler, 60 kHz resampled to 48 kHz, and 24 kHz left at 24 kHz (which EDACS converts to 48 kHz itself).
 * A replay holds one rate chain per stream: a capture cannot change its rate part-way (a RETUNE at another rate seals
 * the writer, issue #806 X9), and a run-time bandwidth or mode change re-plans the replay by stopping its stream and
 * resuming from the checkpoint on the new chain (issue #701). So the rate change covered is that one: a 48 kHz replay
 * stopped part-way with the lane on and resumed at 24 kHz, resampled back to 48 kHz. DSP_FSK_VOICE_LANE covers the
 * lane's de-emphasis following a rate change, and a resampler redesign, inside one demodulator.
 *
 * - The normalised samples and the flags are bit for bit the same whether the lane runs, stops part-way or never runs.
 * - Every batch the demod made with the lane off reads silence on it; with it on, the lane carries the tone at the
 *   monitor's level (0.125 peak once the published de-emphasis gain makes up the lane's de-emphasis, which reads 1
 *   while the lane is off), and the tone 20 dB down reads 20 dB down.
 * - With no de-emphasis the lane is the normalised lane times its slowly moving normaliser gain, sample for sample: at
 *   the same positions, not one sample either side, through the resampler, the gap and the retunes.
 *
 * Before, the stream had one lane and EDACS played the normalised one, so neither a fixed gain nor the AGC could set a
 * level from it.
 *
 * Given a capture sidecar and a mode (`provoice`, `dmr`, `nxdn48`, `nxdn96`, `dpmr`, `dstar`, `ysf`, `m17`, `p25p1`,
 * `x2tdma`), it instead replays that capture with the lane off and on throughout and checks the normalised samples and
 * flags are bit for bit the same: the evidence run over the committed FSK fixtures (docs/testing.md).
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/mem.h>
#include <memory>
#include <string>
#include <vector>
#include "dsd-neo/core/iq_replay_checkpoint.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "test_env.h"
#include "test_support.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kReferencePeak = 0.125;

/* Each replay's centre, a new one each time: the auto squelch remembers a channel's floor for the process (its floor
   cache), so a replay on a centre heard before would start from that floor and set other flags. */
uint64_t g_center_hz = 851000000ULL;

std::vector<std::string> g_dirs;
int g_failures = 0;

void
expect(const char* label, bool cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

/* What a capture holds, in order. */
enum SegmentKind { kTone, kNoise, kOverflow, kRetune };

struct Segment {
    double seconds;      /* kTone, kNoise */
    double deviation;    /* kTone: Hz */
    uint64_t mute_bytes; /* kOverflow */
    SegmentKind kind;
};

/* The I/Q a capture carries, kept across segments: an FM carrier's phase and the noise generator. */
struct Synth {
    double phase = 0.0;
    double t = 0.0;
    uint32_t seed = 0x518B10U;

    double
    noise() {
        seed = seed * 1664525U + 1013904223U;
        return ((double)(seed >> 8) / 16777216.0) - 0.5;
    }

    /* @p n pairs at @p rate_hz into cu8 bytes: the carrier (deviation > 0) or none, under a little noise. */
    void
    emit(std::vector<uint8_t>* out, size_t n, uint32_t rate_hz, double deviation, int carrier) {
        for (size_t i = 0; i < n; i++) {
            phase += 2.0 * kPi * deviation * std::cos(2.0 * kPi * 1000.0 * t) / (double)rate_hz;
            t += 1.0 / (double)rate_hz;
            const double a = carrier ? 80.0 : 0.0;
            const double in_i = (a * std::cos(phase)) + (6.0 * noise());
            const double in_q = (a * std::sin(phase)) + (6.0 * noise());
            out->push_back((uint8_t)std::lrint(std::min(255.0, std::max(0.0, 127.5 + in_i))));
            out->push_back((uint8_t)std::lrint(std::min(255.0, std::max(0.0, 127.5 + in_q))));
        }
    }
};

int
record_event(dsd_iq_capture_writer* writer, dsd_iq_event_kind kind, uint64_t mute_bytes, uint32_t rate_hz,
             uint64_t center_hz, const char* reason) {
    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = kind;
    if (kind == DSD_IQ_EVENT_MUTE) {
        ev.duration_bytes = mute_bytes;
    } else {
        ev.center_frequency_hz = center_hz;
        ev.capture_center_frequency_hz = center_hz;
        ev.sample_rate_hz = rate_hz;
    }
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", reason);
    return dsd_iq_capture_record_event(writer, &ev) == DSD_IQ_OK ? 0 : 1;
}

int
write_segments(dsd_iq_capture_writer* writer, const Segment* segs, size_t nsegs, uint32_t rate_hz, uint64_t center_hz) {
    Synth synth;
    int rc = 0;
    for (size_t s = 0; s < nsegs && rc == 0; s++) {
        const Segment& seg = segs[s];
        if (seg.kind == kOverflow) {
            rc |= record_event(writer, DSD_IQ_EVENT_MUTE, seg.mute_bytes, rate_hz, center_hz,
                               RTL_CAPTURE_MUTE_REASON_DRIVER_OVERFLOW);
            continue;
        }
        if (seg.kind == kRetune) {
            rc |= record_event(writer, DSD_IQ_EVENT_RETUNE, 0U, rate_hz, center_hz + 500000ULL, "frequency");
            rc |= record_event(writer, DSD_IQ_EVENT_RESET, 0U, rate_hz, center_hz + 500000ULL, "frequency");
            continue;
        }
        std::vector<uint8_t> bytes;
        const size_t n = (size_t)(seg.seconds * (double)rate_hz);
        synth.emit(&bytes, n, rate_hz, seg.deviation, seg.kind == kTone);
        rc |= dsd_iq_capture_submit(writer, bytes.data(), bytes.size()) == DSD_IQ_OK ? 0 : 1;
    }
    return rc;
}

int
write_capture(const char* tag, uint32_t rate_hz, uint64_t center_hz, const Segment* segs, size_t nsegs, char* out_meta,
              size_t out_size) {
    char dir[DSD_TEST_PATH_MAX];
    char data_path[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof(dir), tag)) {
        DSD_FPRINTF(stderr, "FAIL: could not create a fixture directory\n");
        return 1;
    }
    g_dirs.push_back(dir);
    if (dsd_test_path_join(data_path, sizeof(data_path), dir, "capture.iq") != 0
        || dsd_test_path_join(out_meta, out_size, dir, "capture.iq.json") != 0) {
        return 1;
    }
    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", out_meta);
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = rate_hz;
    cfg.center_frequency_hz = center_hz;
    /* The carrier at the capture's centre: no fs/4 offset to take back out. */
    cfg.capture_center_frequency_hz = center_hz;
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = (int)(rate_hz / 1000U);
    cfg.base_decimation = 1U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = rate_hz;
    cfg.fs4_shift_enabled = 0;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=0");
    dsd_iq_capture_writer* writer = NULL;
    char err[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: capture writer: %s\n", err);
        return 1;
    }
    if (write_segments(writer, segs, nsegs, rate_hz, center_hz) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not write the capture\n");
        dsd_iq_capture_abort(writer);
        return 1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    return 0;
}

/* What every capture holds: tone, a gap, tone, a retune, tone, noise, then the tone 20 dB down from 3.4 s. */
const Segment kSegments[] = {
    {1.0, 3000.0, 0U, kTone}, {0.0, 0.0, 3074U, kOverflow}, {1.0, 3000.0, 0U, kTone}, {0.0, 0.0, 0U, kRetune},
    {1.0, 3000.0, 0U, kTone}, {0.4, 0.0, 0U, kNoise},       {1.5, 300.0, 0U, kTone},
};

/* A capture's rate, and whether the stream resamples it to 48 kHz. */
struct Spec {
    const char* name;
    uint32_t rate_hz;
    int resample;
};

/* One read's batch: where it starts in the run's samples, how many, whether the lane was asked for while the demod made
   it, and the de-emphasis gain the stream published by then. */
struct Batch {
    size_t at;
    size_t n;
    int lane;
    double gain;
    uint32_t rate_hz;
};

struct Run {
    std::vector<float> norm;
    std::vector<float> voice;
    std::vector<uint8_t> flags;
    std::vector<Batch> batches;
    size_t resumed_at = 0U; /* replay_resumed(): the first batch of the resumed stream */
};

/* Whether the lane is asked for while the demod makes batch @p k: never, always, or on, off and on again. */
enum LanePlan { kLaneNever, kLaneAlways, kLaneToggled };

int
lane_wanted(LanePlan plan, size_t k) {
    switch (plan) {
        case kLaneAlways: return 1;
        case kLaneToggled: return (k >= 2U && k < 9U) || k >= 13U;
        case kLaneNever:
        default: return 0;
    }
}

/* The decode mode a fixture replays under, by name: the frame flags that pick its FSK profile. */
int
apply_mode(dsd_opts* opts, const char* mode) {
    struct Mode {
        const char* name;
        int dsd_opts::* flag;
    };

    static const Mode kModes[] = {
        {"provoice", &dsd_opts::frame_provoice}, {"dmr", &dsd_opts::frame_dmr},   {"nxdn48", &dsd_opts::frame_nxdn48},
        {"nxdn96", &dsd_opts::frame_nxdn96},     {"dpmr", &dsd_opts::frame_dpmr}, {"dstar", &dsd_opts::frame_dstar},
        {"ysf", &dsd_opts::frame_ysf},           {"m17", &dsd_opts::frame_m17},   {"p25p1", &dsd_opts::frame_p25p1},
        {"x2tdma", &dsd_opts::frame_x2tdma},
    };
    for (const Mode& m : kModes) {
        if (std::string(mode) == m.name) {
            opts->*m.flag = 1;
            return 0;
        }
    }
    return -1;
}

/* Replays @p meta to its end, reading each batch whole with its voice lane and flags: the demod makes a batch only
   once the decoder waits on the empty ring (the replay's pacing), so a lane switched before a read applies to the
   whole batch that read returns. */
int replay_meta(const char* meta, const char* mode, int resample_mode, int auto_squelch, LanePlan plan, Run* out);

int
replay(const Spec& spec, LanePlan plan, Run* out) {
    char meta[DSD_TEST_PATH_MAX];
    const uint64_t center_hz = g_center_hz;
    g_center_hz += 1000000ULL;
    if (write_capture("dsdneo_voice_lane", spec.rate_hz, center_hz, kSegments, sizeof kSegments / sizeof kSegments[0],
                      meta, sizeof meta)
        != 0) {
        return 1;
    }
    /* The 9600 bit/s FSK discriminator EDACS runs, its auto squelch on. */
    return replay_meta(meta, "provoice", spec.resample ? DSD_DIGITAL_RESAMPLE_ON : DSD_DIGITAL_RESAMPLE_OFF, 1, plan,
                       out);
}

/* How one stream of a replay runs: the decode mode, the digital resampler, the squelch, an explicit DSP bandwidth (0:
   the capture's) and a checkpoint to resume from; it stops after @c stop_after reads (0: at the capture's end). The
   lane plan counts the run's reads from @c first_read on, so a resumed stream goes on with the plan. */
struct StreamCfg {
    const char* mode;
    int resample_mode;
    int auto_squelch;
    int bw_khz;
    const dsd_iq_replay_checkpoint* resume;
    size_t stop_after;
    size_t first_read;
};

/* One stream's reads into @p out; returns 0, or 1 on a failure, with the stream's log printed only then. */
int
replay_stream(const char* meta, const StreamCfg& cfg, LanePlan plan, Run* out) {
    std::unique_ptr<dsd_opts> opts(new dsd_opts());
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = DSD_IQ_REPLAY_RATE_FAST;
    if (apply_mode(opts.get(), cfg.mode) != 0) {
        DSD_FPRINTF(stderr, "FAIL: unknown mode %s\n", cfg.mode);
        return 1;
    }
    opts->rtl_squelch_mode = cfg.auto_squelch ? DSD_SQUELCH_MODE_AUTO : DSD_SQUELCH_MODE_LEVEL;
    opts->rtl_squelch_margin_db = 10;
    opts->digital_resample_mode = cfg.resample_mode;
    if (cfg.bw_khz > 0) {
        dsd_opts_set_rtl_dsp_bw(opts.get(), cfg.bw_khz, DSD_RTL_DSP_BW_SOURCE_EXPLICIT);
    }
    if (cfg.resume) {
        opts->iq_replay_resume_pending = 1;
        opts->iq_replay_resume = *cfg.resume;
    }
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", meta);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", meta);
    /* The stream logs what it opens and replays: kept, and shown only for a failing run. */
    dsd_test_capture_stderr cap;
    const int captured = dsd_test_capture_stderr_begin(&cap, "dsdneo_voice_lane_log") == 0;
    rtl_stream_set_voice_lane(lane_wanted(plan, cfg.first_read));
    RtlSdrContext* ctx = NULL;
    int rc = 0;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not start the replay of %s\n", meta);
        rc = 1;
    } else {
        static float norm[65536];
        static float voice[65536];
        static uint8_t flags[65536];
        const uint64_t start_ns = dsd_time_monotonic_ns();
        for (size_t k = cfg.first_read; cfg.stop_after == 0U || k < cfg.first_read + cfg.stop_after; k++) {
            const int lane = lane_wanted(plan, k);
            rtl_stream_set_voice_lane(lane);
            int got = 0;
            if (rtl_stream_read_lanes(ctx, norm, voice, flags, sizeof(norm) / sizeof(norm[0]), &got, 0) != 0) {
                break;
            }
            const Batch b = {out->norm.size(), (size_t)got, lane, rtl_stream_monitor_deemph_gain(),
                             rtl_stream_output_rate(ctx)};
            out->batches.push_back(b);
            out->norm.insert(out->norm.end(), norm, norm + got);
            out->voice.insert(out->voice.end(), voice, voice + got);
            out->flags.insert(out->flags.end(), flags, flags + got);
            if (dsd_time_monotonic_ns() - start_ns > 60000000000ULL) {
                DSD_FPRINTF(stderr, "FAIL: timed out reading the replay of %s\n", meta);
                rc = 1;
                break;
            }
        }
        (void)rtl_stream_stop(ctx);
    }
    if (ctx) {
        (void)rtl_stream_destroy(ctx);
    }
    if (captured) {
        (void)dsd_test_capture_stderr_end(&cap);
        static char log[1 << 16];
        if (dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0 && rc != 0) {
            DSD_FPRINTF(stderr, "%s", log);
        }
    }
    return rc;
}

int
replay_meta(const char* meta, const char* mode, int resample_mode, int auto_squelch, LanePlan plan, Run* out) {
    rtl_stream_set_voice_lane(0);
    const StreamCfg cfg = {mode, resample_mode, auto_squelch, 0, nullptr, 0U, 0U};
    const int rc = replay_stream(meta, cfg, plan, out);
    rtl_stream_set_voice_lane(0);
    return rc;
}

/* The replay of a fresh 48 kHz capture stopped after @p stop_after reads and resumed from its checkpoint at an
   explicit 24 kHz DSP bandwidth, the digital resampler on (it takes the resumed stream back up to 48 kHz), as a run-time
   bandwidth change re-plans a running replay (issue #701). The lane plan runs on across the two streams. */
int
replay_resumed(LanePlan plan, size_t stop_after, Run* out) {
    char meta[DSD_TEST_PATH_MAX];
    const uint64_t center_hz = g_center_hz;
    g_center_hz += 1000000ULL;
    if (write_capture("dsdneo_voice_lane", 48000U, center_hz, kSegments, sizeof kSegments / sizeof kSegments[0], meta,
                      sizeof meta)
        != 0) {
        return 1;
    }
    rtl_stream_set_voice_lane(0);
    StreamCfg cfg = {"provoice", DSD_DIGITAL_RESAMPLE_ON, 1, 0, nullptr, stop_after, 0U};
    int rc = replay_stream(meta, cfg, plan, out);
    dsd_iq_replay_checkpoint checkpoint{};
    if (rc == 0 && rtl_stream_replay_checkpoint(&checkpoint) != 0) {
        DSD_FPRINTF(stderr, "FAIL: the stopped replay left no checkpoint\n");
        rc = 1;
    }
    out->resumed_at = out->batches.size();
    cfg.bw_khz = 24;
    cfg.resume = &checkpoint;
    cfg.stop_after = 0U;
    cfg.first_read = stop_after;
    if (rc == 0) {
        rc = replay_stream(meta, cfg, plan, out);
    }
    rtl_stream_set_voice_lane(0);
    return rc;
}

int
same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        return 0;
    }
    for (size_t i = 0; i < a.size(); i++) {
        uint32_t x = 0U;
        uint32_t y = 0U;
        DSD_MEMCPY(&x, &a[i], sizeof(x));
        DSD_MEMCPY(&y, &b[i], sizeof(y));
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

/* The 1 kHz tone's peak in @p n samples from @p at at @p rate_hz (whole cycles), least squares. */
double
tone_peak(const std::vector<float>& y, size_t at, size_t n, uint32_t rate_hz) {
    const size_t cycle = rate_hz / 1000U;
    const size_t count = cycle > 0U ? (n / cycle) * cycle : 0U;
    if (count == 0U) {
        return 0.0;
    }
    double a = 0.0;
    double b = 0.0;
    for (size_t k = 0; k < count; k++) {
        const double w = 2.0 * kPi * 1000.0 * (double)k / (double)rate_hz;
        a += (double)y[at + k] * std::cos(w);
        b += (double)y[at + k] * std::sin(w);
    }
    a *= 2.0 / (double)count;
    b *= 2.0 / (double)count;
    return std::sqrt(a * a + b * b);
}

double
median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2U];
}

/* The share of samples where lane b is lane a times one slowly moving gain: each sample's ratio within 3% of the median
   ratio around it, @p lag samples apart (0: at the same position). Samples near the normaliser's clip, or too small to
   weigh, are left out. */
double
ratio_agreement(const Run& run, long lag) {
    size_t agree = 0U;
    size_t total = 0U;
    const long n = (long)run.norm.size();
    for (long i = 256; i + 256 < n; i += 3) {
        const float x = run.norm[(size_t)i];
        /* The tone only: in noise the normaliser's gain jumps at every new peak. */
        if (std::fabs(x) < 3000.0f || std::fabs(x) > 28000.0f || (run.flags[(size_t)i] & DSD_SQUELCH_FLAG_CLOSED)) {
            continue;
        }
        std::vector<double> local;
        for (long j = i - 240; j <= i + 240; j += 16) {
            if (std::fabs(run.norm[(size_t)j]) > 3000.0f && std::fabs(run.norm[(size_t)j]) < 28000.0f) {
                local.push_back((double)run.voice[(size_t)j] / (double)run.norm[(size_t)j]);
            }
        }
        const double m = median(local);
        const double r = (double)run.voice[(size_t)(i + lag)] / (double)x;
        total++;
        if (std::fabs(m) > 1e-12 && std::fabs(r / m - 1.0) < 0.03) {
            agree++;
        }
    }
    return total > 0U ? (double)agree / (double)total : 0.0;
}

/* The lane's level over each whole batch the lane ran for at the reference deviation, made up by the gain the stream
   published (the median over those batches), and the same for the batches 20 dB down. */
void
lane_levels(const Run& run, size_t first_quiet_batch, double* ref, double* quiet) {
    std::vector<double> loud;
    std::vector<double> soft;
    for (size_t k = 0; k < run.batches.size(); k++) {
        const Batch& b = run.batches[k];
        if (!b.lane || b.n < (size_t)4U * (b.rate_hz / 1000U)) {
            continue;
        }
        const double level = tone_peak(run.voice, b.at, b.n, b.rate_hz) * b.gain;
        (k >= first_quiet_batch ? soft : loud).push_back(level);
    }
    *ref = median(loud);
    *quiet = median(soft);
}

void
check_capture(const Spec& spec, double tau_gain) {
    const char* name = spec.name;
    const int resample = spec.resample;
    const uint32_t first_rate_hz = spec.rate_hz;
    Run never;
    Run toggled;
    expect("replay without the lane", replay(spec, kLaneNever, &never) == 0);
    expect("replay with the lane switched", replay(spec, kLaneToggled, &toggled) == 0);
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "%s: the normalised samples are bit for bit the same with the lane", name);
    expect(label, !never.norm.empty() && same_bits(never.norm, toggled.norm));
    DSD_SNPRINTF(label, sizeof label, "%s: the flags are the same with the lane", name);
    expect(label, never.flags == toggled.flags);
    size_t closed = 0U;
    for (uint8_t f : never.flags) {
        closed += (f & DSD_SQUELCH_FLAG_CLOSED) != 0U ? 1U : 0U;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: the auto squelch closed the noise (flags carried)", name);
    expect(label, closed > 0U && closed < never.flags.size());

    int off_silent = 1;
    int on_heard = 0;
    int gain_follows = 1;
    for (const Batch& b : toggled.batches) {
        double sum = 0.0;
        for (size_t i = 0; i < b.n; i++) {
            sum += std::fabs((double)toggled.voice[b.at + i]);
        }
        if (!b.lane) {
            off_silent &= sum < 1e-30 ? 1 : 0;
            gain_follows &= std::fabs(b.gain - 1.0) < 1e-6 ? 1 : 0;
        } else {
            on_heard += sum > 1e-30 ? 1 : 0;
        }
    }
    for (const Batch& b : never.batches) {
        double sum = 0.0;
        for (size_t i = 0; i < b.n; i++) {
            sum += std::fabs((double)never.voice[b.at + i]);
        }
        off_silent &= sum < 1e-30 ? 1 : 0;
    }
    DSD_SNPRINTF(label, sizeof label, "%s: a batch made with the lane off reads silence on it", name);
    expect(label, off_silent != 0);
    DSD_SNPRINTF(label, sizeof label, "%s: batches made with the lane on carry it", name);
    expect(label, on_heard > 4);
    DSD_SNPRINTF(label, sizeof label, "%s: the published gain is 1 while the lane is off", name);
    expect(label, gain_follows != 0);
    /* The first lane-on batch publishes the lane's de-emphasis gain at the first rate. */
    double first_gain = 0.0;
    for (const Batch& b : toggled.batches) {
        if (b.lane && b.rate_hz == (resample ? 48000U : first_rate_hz)) {
            first_gain = b.gain;
            break;
        }
    }
    DSD_SNPRINTF(label, sizeof label, "%s: the lane's published de-emphasis gain", name);
    expect(label, std::fabs(first_gain - tau_gain) < 1e-4);
    if (std::fabs(first_gain - tau_gain) >= 1e-4) {
        DSD_FPRINTF(stderr, "  %s: published %.6f, want %.6f\n", name, first_gain, tau_gain);
    }

    Run always;
    expect("replay with the lane on", replay(spec, kLaneAlways, &always) == 0);
    DSD_SNPRINTF(label, sizeof label, "%s: the normalised samples are the same with the lane on throughout", name);
    expect(label, same_bits(never.norm, always.norm) && never.flags == always.flags);
    double ref = 0.0;
    double quiet = 0.0;
    /* The quiet tone starts with the capture's fourth tone segment; the batches before it are counted by samples. */
    size_t first_quiet = always.batches.size();
    const size_t quiet_at = (size_t)(3.6 * (double)(resample ? 48000U : first_rate_hz));
    for (size_t k = 0; k < always.batches.size(); k++) {
        if (always.batches[k].at >= quiet_at) {
            first_quiet = k;
            break;
        }
    }
    lane_levels(always, first_quiet, &ref, &quiet);
    DSD_SNPRINTF(label, sizeof label, "%s: the lane reads the reference tone at the monitor's level", name);
    expect(label, std::fabs(ref - kReferencePeak) < 0.03 * kReferencePeak);
    DSD_SNPRINTF(label, sizeof label, "%s: the tone 20 dB down reads 20 dB down on the lane", name);
    const double drop_db = 20.0 * std::log10(ref / (quiet > 0.0 ? quiet : 1e-12));
    expect(label, std::fabs(drop_db - 20.0) < 1.0);
    if (std::fabs(ref - kReferencePeak) >= 0.03 * kReferencePeak || std::fabs(drop_db - 20.0) >= 1.0) {
        DSD_FPRINTF(stderr, "  %s: reference %.5f peak, quiet tone %.2f dB down\n", name, ref, drop_db);
    }
}

/* With no de-emphasis the lane is the normalised lane times the normaliser's slowly moving gain, at every position. */
void
check_alignment(const Spec& spec) {
    const char* name = spec.name;
    (void)dsd_test_setenv("DSD_NEO_DEEMPH", "off", 1);
    dsd_neo_config_init();
    Run run;
    expect("replay with the lane on, no de-emphasis", replay(spec, kLaneAlways, &run) == 0);
    (void)dsd_test_unsetenv("DSD_NEO_DEEMPH");
    dsd_neo_config_init();
    const double at = ratio_agreement(run, 0);
    const double before = ratio_agreement(run, -1);
    const double after = ratio_agreement(run, 1);
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "%s: the lane sits at the normalised samples' positions", name);
    expect(label, at > 0.97 && before < 0.6 && after < 0.6);
    if (!(at > 0.97 && before < 0.6 && after < 0.6)) {
        DSD_FPRINTF(stderr, "  %s: ratio agreement %.3f at 0, %.3f at -1, %.3f at +1\n", name, at, before, after);
    }
}

/* The lane's de-emphasis gain at 1 kHz for the default 75 us at @p rate_hz, as the demod computes it. */
double
default_lane_gain(int rate_hz) {
    /* demod_state carries 64-byte-aligned members, so it needs the aligned allocator. */
    demod_state* d = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!d) {
        return 0.0;
    }
    DSD_MEMSET(d, 0, sizeof(*d));
    d->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    d->complex_rate = rate_hz;
    d->voice_lane_enabled = 1;
    d->voice_lane_deemph_a = dsd_demod_deemph_alpha(rate_hz, 75);
    const double g = dsd_demod_deemph_compensation(d);
    dsd_neo_aligned_free(d);
    return g;
}

/* The closest a replay comes to a rate change in one stream: a replay holds one rate chain per stream (a RETUNE at
   another rate seals the capture writer), and a run-time bandwidth or mode change re-plans it by stopping the stream
   and resuming the capture from its checkpoint on the new chain (issue #701). The lane stays asked for across that, as
   it does through an EDACS call: the resumed stream runs it from its first block at the new rate (24 kHz, resampled back
   to 48 kHz), its de-emphasis gain the new rate's, the reference at its level, and the normalised samples and flags
   the same as with the lane off. */
void
check_resumed(void) {
    Run never;
    Run always;
    const size_t stop_after = 300U;
    expect("resumed replay without the lane", replay_resumed(kLaneNever, stop_after, &never) == 0);
    expect("resumed replay with the lane", replay_resumed(kLaneAlways, stop_after, &always) == 0);
    expect("resumed at 24 kHz: the normalised samples and flags are the same with the lane",
           !never.norm.empty() && same_bits(never.norm, always.norm) && never.flags == always.flags);
    expect("resumed at 24 kHz: both streams read", always.resumed_at > 0U && always.resumed_at < always.batches.size());
    const double gain48 = default_lane_gain(48000);
    const double gain24 = default_lane_gain(24000);
    int gains_ok = always.resumed_at > 0U && always.resumed_at < always.batches.size();
    std::vector<double> levels;
    for (size_t k = 0; k < always.batches.size(); k++) {
        const Batch& b = always.batches[k];
        const double want = k < always.resumed_at ? gain48 : gain24;
        gains_ok &= std::fabs(b.gain - want) < 1e-4 && b.rate_hz == 48000U ? 1 : 0;
        /* The resumed stream's reference tone, past its first 0.2 s and before the carrier drops. */
        if (k >= always.resumed_at && b.at >= always.batches[always.resumed_at].at + 9600U && b.at < (size_t)144000U
            && b.n >= (size_t)192U) {
            levels.push_back(tone_peak(always.voice, b.at, b.n, b.rate_hz) * b.gain);
        }
    }
    expect("resumed at 24 kHz: the published gain is the lane's at each stream's rate", gains_ok != 0);
    const double level = median(levels);
    expect("resumed at 24 kHz: the lane reads the reference at the monitor's level",
           !levels.empty() && std::fabs(level - kReferencePeak) < 0.03 * kReferencePeak);
    if (levels.empty() || std::fabs(level - kReferencePeak) >= 0.03 * kReferencePeak || !gains_ok) {
        DSD_FPRINTF(stderr, "  resumed: %zu batches (%zu before the resume), level %.5f over %zu batches\n",
                    always.batches.size(), always.resumed_at, level, levels.size());
    }
}

} // namespace

/* The evidence run: @p meta replayed under @p mode with the lane off, then on throughout, the squelch off (the auto
   squelch keeps a channel's floor for the process, so the second replay of one centre would start from another floor:
   the synthetic captures above cover the flags). */
static int
fixture_identity(const char* meta, const char* mode) {
    Run off;
    Run on;
    if (replay_meta(meta, mode, DSD_DIGITAL_RESAMPLE_AUTO, 0, kLaneNever, &off) != 0
        || replay_meta(meta, mode, DSD_DIGITAL_RESAMPLE_AUTO, 0, kLaneAlways, &on) != 0) {
        return 1;
    }
    size_t lane_samples = 0U;
    for (float v : on.voice) {
        lane_samples += std::fabs(v) > 0.0f ? 1U : 0U;
    }
    const int same = same_bits(off.norm, on.norm) && off.flags == on.flags;
    std::printf("%s %s: %zu samples, normalised and flags %s with the lane on (%zu lane samples)\n", meta, mode,
                off.norm.size(), same ? "bit-identical" : "DIFFER", lane_samples);
    return same && lane_samples > 0U ? 0 : 1;
}

int
main(int argc, char** argv) {
    (void)dsd_test_unsetenv("DSD_NEO_DEEMPH");
    dsd_neo_config_init();
    if (argc == 3) {
        return fixture_identity(argv[1], argv[2]);
    }
    static const Spec kSpecs[] = {
        {"48 kHz", 48000U, 0},
        {"60 kHz resampled to 48 kHz", 60000U, 1},
        {"24 kHz", 24000U, 0},
    };
    for (const Spec& spec : kSpecs) {
        check_capture(spec, default_lane_gain((int)spec.rate_hz));
        check_alignment(spec);
    }
    check_resumed();
    for (const std::string& dir : g_dirs) {
        static const char* const kFiles[] = {"capture.iq", "capture.iq.json", nullptr};
        (void)dsd_test_remove_temp_dir(dir.c_str(), kFiles);
    }
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "IO_RTL_VOICE_LANE: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "IO_RTL_VOICE_LANE: OK\n");
    return 0;
}
