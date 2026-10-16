// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A run-time re-plan reopens an I/Q replay where the stopped stream left it (issue #701).
 *
 * A 48 kHz cu8 capture with the fs/4 shift, a MUTE that omits an odd number of samples, a RETUNE and RESET, and a
 * second MUTE. A replay is stopped part way through; rtl_stream_replay_checkpoint() then gives where its reader stood
 * after the last chunk it handed the demodulator, and a new stream started with that checkpoint
 * (dsd_opts::iq_replay_resume) goes on from it:
 *
 * - Stopped after the first MUTE: the checkpoint lies past the MUTE and before the RETUNE, with the MUTE applied (event
 *   index 1) and its omitted samples on the media timeline. The resumed replay's blocks are the straight replay's
 *   blocks from the checkpoint on, sample for sample (the fs/4 phase goes on as well), with the same media spans, so
 *   its media time continues and never goes behind what the stopped stream delivered; it applies the RETUNE, the RESET
 *   and the second MUTE, and opens on the opening centre.
 * - Stopped after the RETUNE: the resumed replay opens on the centre the RETUNE moved to.
 * - Under --iq-loop, stopped on the second pass: the checkpoint counts the completed pass, and the resumed replay's
 *   blocks, pass count included, are the straight looping replay's.
 * - Resumed at another rate: an explicit 24 kHz DSP bandwidth on the 48 kHz capture runs at base decimation 2 (the
 *   demod at 24 kHz) on the same capture blocks, and TETRA resamples to 72 kHz; both go on from the checkpoint's media
 *   time.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "dsd-neo/core/iq_replay_checkpoint.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "rtl_stream_test_support.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr uint32_t kRateHz = 48000U;
constexpr uint64_t kCenterHz = 851375000ULL;
constexpr uint64_t kSecondCenterHz = 852000000ULL;
constexpr size_t kCaptureSamples = 48000U;
constexpr uint64_t kMute1At = 20002U;
constexpr uint64_t kMute1Bytes = 3074U; /* 1537 samples */
constexpr uint64_t kRetuneAt = 50010U;
constexpr uint64_t kMute2At = 70000U;
constexpr uint64_t kMute2Bytes = 2048U;

struct Block {
    uint64_t sequence;
    uint64_t media_start_ns;
    uint64_t media_end_ns;
    uint32_t loop_pass;
    std::vector<float> floats;
};

struct BlockLog {
    std::vector<Block> blocks;
};

std::vector<std::string> g_dirs;

int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

void
block_hook(const rtl_stream_test_replay_block* block, void* ctx) {
    BlockLog* log = static_cast<BlockLog*>(ctx);
    Block b;
    b.sequence = block->sequence;
    b.media_start_ns = block->media_start_ns;
    b.media_end_ns = block->media_end_ns;
    rtl_stream_test_replay_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    (void)dsd_rtl_stream_test_get_replay_state(&state);
    b.loop_pass = state.replay_loop_restart_count;
    b.floats.assign(block->p1, block->p1 + block->n1);
    if (block->p2 && block->n2 > 0U) {
        b.floats.insert(b.floats.end(), block->p2, block->p2 + block->n2);
    }
    log->blocks.push_back(std::move(b));
}

std::vector<uint8_t>
tone_payload(size_t samples) {
    std::vector<uint8_t> payload(samples * 2U);
    uint32_t lcg = 0x7654321U;
    for (size_t i = 0; i < samples; i++) {
        const double phase = 2.0 * M_PI * 9100.0 * (double)i / (double)kRateHz;
        lcg = lcg * 1664525U + 1013904223U;
        const int ni = (int)((lcg >> 24) & 0xFU) - 8;
        lcg = lcg * 1664525U + 1013904223U;
        const int nq = (int)((lcg >> 24) & 0xFU) - 8;
        const long vi = lrint(127.5 + 80.0 * std::cos(phase)) + ni;
        const long vq = lrint(127.5 + 80.0 * std::sin(phase)) + nq;
        payload[2U * i] = (uint8_t)(vi < 0 ? 0 : (vi > 255 ? 255 : vi));
        payload[2U * i + 1U] = (uint8_t)(vq < 0 ? 0 : (vq > 255 ? 255 : vq));
    }
    return payload;
}

int
record_event(dsd_iq_capture_writer* writer, dsd_iq_event_kind kind, uint64_t mute_bytes, uint64_t center_hz) {
    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = kind;
    if (kind == DSD_IQ_EVENT_MUTE) {
        ev.duration_bytes = mute_bytes;
        DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "driver_overflow");
    } else {
        ev.center_frequency_hz = center_hz;
        ev.capture_center_frequency_hz = center_hz + kRateHz / 4U;
        ev.sample_rate_hz = kRateHz;
        DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "frequency");
    }
    return dsd_iq_capture_record_event(writer, &ev) == DSD_IQ_OK ? 0 : 1;
}

/* The 48 kHz cu8 capture with its events; @p out_meta gets the sidecar's path. */
int
write_capture(const std::vector<uint8_t>& payload, char* out_meta, size_t out_size) {
    char dir[DSD_TEST_PATH_MAX];
    char data_path[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_replay_resume")) {
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
    cfg.sample_rate_hz = kRateHz;
    cfg.center_frequency_hz = kCenterHz;
    cfg.capture_center_frequency_hz = kCenterHz + kRateHz / 4U;
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 1U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = kRateHz;
    cfg.fs4_shift_enabled = 1;
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
    int rc = dsd_iq_capture_submit(writer, payload.data(), (size_t)kMute1At);
    rc |= record_event(writer, DSD_IQ_EVENT_MUTE, kMute1Bytes, 0U);
    rc |= dsd_iq_capture_submit(writer, payload.data() + kMute1At, (size_t)(kRetuneAt - kMute1At));
    rc |= record_event(writer, DSD_IQ_EVENT_RETUNE, 0U, kSecondCenterHz);
    rc |= record_event(writer, DSD_IQ_EVENT_RESET, 0U, kSecondCenterHz);
    rc |= dsd_iq_capture_submit(writer, payload.data() + kRetuneAt, (size_t)(kMute2At - kRetuneAt));
    rc |= record_event(writer, DSD_IQ_EVENT_MUTE, kMute2Bytes, 0U);
    rc |= dsd_iq_capture_submit(writer, payload.data() + kMute2At, payload.size() - (size_t)kMute2At);
    if (rc != DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "FAIL: could not write the capture\n");
        dsd_iq_capture_abort(writer);
        return 1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    return 0;
}

/* How a replay runs: its mode and bandwidth, where it stops, and where it resumes from. */
struct RunSpec {
    int tetra = 0;
    int bw_khz = 0; /* an explicit DSP bandwidth, or 0 for none */
    int loop = 0;
    int realtime = 0;
    uint64_t stop_after_sequence = 0U; /* stop once the decoder read a batch of this chunk; 0 runs to the end */
    uint32_t stop_after_passes = 0U;   /* under --iq-loop: ... on this pass (passes completed) */
    uint32_t run_passes = 0U;          /* under --iq-loop with no stop: run until this many passes completed */
    const dsd_iq_replay_checkpoint* resume = nullptr;
};

struct Run {
    BlockLog log;
    uint64_t last_read_media_ns = 0U; /* the latest media time the decoder read */
    rtl_stream_test_replay_state state{};
    int demod_rate_hz = 0;
    uint32_t center_hz = 0U;
    int have_checkpoint = 0;
    dsd_iq_replay_checkpoint checkpoint{};
};

int
replay(const char* path, const RunSpec& spec, Run* out) {
    std::unique_ptr<BlockLog> log(new BlockLog());
    std::unique_ptr<dsd_opts> opts(new dsd_opts());
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = spec.realtime ? DSD_IQ_REPLAY_RATE_REALTIME : DSD_IQ_REPLAY_RATE_FAST;
    opts->iq_replay_loop = spec.loop ? 1 : 0;
    opts->frame_tetra = spec.tetra ? 1 : 0;
    if (spec.bw_khz > 0) {
        dsd_opts_set_rtl_dsp_bw(opts.get(), spec.bw_khz, DSD_RTL_DSP_BW_SOURCE_EXPLICIT);
    }
    if (spec.resume) {
        opts->iq_replay_resume_pending = 1;
        opts->iq_replay_resume = *spec.resume;
    }
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", path);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", path);

    rtl_stream_test_set_replay_block_hook(block_hook, log.get());
    RtlSdrContext* ctx = NULL;
    int rc = 0;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not start the replay of %s\n", path);
        rc = 1;
    } else {
        (void)rtl_stream_get_last_applied_freq(&out->center_hz);
        float audio[2048];
        const uint64_t start_ns = dsd_time_monotonic_ns();
        for (;;) {
            int got = 0;
            if (rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got) != 0) {
                break;
            }
            rtl_stream_replay_batch batch;
            if (got > 0 && rtl_stream_get_replay_batch(&batch) == 0) {
                const uint64_t end_ns = batch.media_start_ns + batch.media_duration_ns;
                if (end_ns > out->last_read_media_ns) {
                    out->last_read_media_ns = end_ns;
                }
                rtl_stream_test_replay_state st;
                DSD_MEMSET(&st, 0, sizeof(st));
                (void)dsd_rtl_stream_test_get_replay_state(&st);
                if (spec.stop_after_sequence > 0U && batch.chunk_sequence >= spec.stop_after_sequence
                    && st.replay_loop_restart_count >= spec.stop_after_passes) {
                    break;
                }
                if (spec.run_passes > 0U && st.replay_loop_restart_count >= spec.run_passes) {
                    break;
                }
            }
            if (dsd_time_monotonic_ns() - start_ns > 120000000000ULL) {
                DSD_FPRINTF(stderr, "FAIL: timed out reading the replay of %s\n", path);
                rc = 1;
                break;
            }
        }
        DSD_MEMSET(&out->state, 0, sizeof(out->state));
        (void)dsd_rtl_stream_test_get_replay_state(&out->state);
        out->demod_rate_hz = rtl_stream_get_demod_rate_hz();
        (void)rtl_stream_stop(ctx);
    }
    if (ctx) {
        (void)rtl_stream_destroy(ctx);
    }
    rtl_stream_test_set_replay_block_hook(NULL, NULL);
    out->have_checkpoint = rtl_stream_replay_checkpoint(&out->checkpoint) == 0;
    out->log = std::move(*log);
    return rc;
}

/* The media time of @p samples capture samples at 48 kHz, as the replay reader counts it. */
uint64_t
media_ns(uint64_t samples) {
    return (samples / kRateHz) * 1000000000ULL + ((samples % kRateHz) * 1000000000ULL) / kRateHz;
}

/* The media time a replay resumed from @p c starts at: the checkpoint's, past what a MUTE at the checkpoint's offset
 * omits (the resumed reader applies it first). */
uint64_t
resume_media_ns(const dsd_iq_replay_checkpoint& c) {
    uint64_t samples = c.timeline_samples;
    if (c.data_offset == kMute1At && c.event_index == 0U) {
        samples += kMute1Bytes / 2U;
    } else if (c.data_offset == kMute2At && c.event_index == 3U) {
        samples += kMute2Bytes / 2U;
    }
    return media_ns(samples);
}

/* The index of the straight replay's block that starts at @p media_start_ns on pass @p pass, or -1. */
long
find_block(const std::vector<Block>& blocks, uint64_t media_start_ns, uint32_t pass) {
    for (size_t i = 0; i < blocks.size(); i++) {
        if (blocks[i].media_start_ns == media_start_ns && blocks[i].loop_pass == pass) {
            return (long)i;
        }
    }
    return -1;
}

/* The resumed replay's blocks are the straight replay's from @p from on: samples (with @p compare_floats), media spans
 * and passes; with @p whole, to the straight replay's end. */
int
expect_continues(const char* label, const std::vector<Block>& straight, long from, const std::vector<Block>& resumed,
                 int compare_floats, int whole) {
    if (from < 0 || resumed.empty()) {
        DSD_FPRINTF(stderr,
                    "FAIL: %s: the resumed replay (first block at %llu ns, pass %u) starts at no block of the "
                    "straight one\n",
                    label, resumed.empty() ? 0ULL : (unsigned long long)resumed[0].media_start_ns,
                    resumed.empty() ? 0U : (unsigned)resumed[0].loop_pass);
        return 1;
    }
    const size_t left = straight.size() - (size_t)from;
    const size_t n = resumed.size() < left ? resumed.size() : left;
    if (n == 0U || (whole && resumed.size() != left)) {
        DSD_FPRINTF(stderr, "FAIL: %s: %zu blocks resumed, %zu left in the straight replay\n", label, resumed.size(),
                    left);
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        const Block& a = straight[(size_t)from + i];
        const Block& b = resumed[i];
        if (a.media_start_ns != b.media_start_ns || a.media_end_ns != b.media_end_ns || a.loop_pass != b.loop_pass
            || (compare_floats && a.floats != b.floats)) {
            DSD_FPRINTF(stderr, "FAIL: %s: resumed block %zu (%llu..%llu ns, pass %u) is not straight block %zu\n",
                        label, i, (unsigned long long)b.media_start_ns, (unsigned long long)b.media_end_ns,
                        (unsigned)b.loop_pass, (size_t)from + i);
            return 1;
        }
    }
    return 0;
}

int
test_resume_after_mute(const char* meta) {
    int rc = 0;
    Run straight;
    Run stopped;
    RunSpec spec;
    rc |= replay(meta, spec, &straight);
    spec.stop_after_sequence = 60U; /* 60 chunks of 512 bytes: past the first MUTE, before the RETUNE */
    rc |= replay(meta, spec, &stopped);
    if (rc != 0) {
        return rc;
    }
    const dsd_iq_replay_checkpoint& c = stopped.checkpoint;
    rc |= expect_true("a stopped replay leaves a checkpoint", stopped.have_checkpoint);
    rc |= expect_true("the checkpoint lies past the first MUTE and before the RETUNE",
                      c.data_offset > kMute1At && c.data_offset < kRetuneAt);
    rc |= expect_true("the checkpoint's next event is the RETUNE", c.event_index == 1U);
    rc |= expect_true("the checkpoint's media timeline counts the MUTE's omitted samples",
                      c.timeline_samples == c.data_offset / 2U + kMute1Bytes / 2U);
    rc |= expect_true("the checkpoint's pass position counts them too", c.pass_samples == c.timeline_samples);
    rc |= expect_true("the checkpoint is on the first pass", c.loop_pass == 0U);
    rc |= expect_true("the checkpoint's fs/4 phase follows the samples and the MUTE",
                      c.fs4_phase == (int32_t)(c.pass_samples & 3U));
    rc |= expect_true("the stopped stream's decoder read no further than the checkpoint",
                      stopped.last_read_media_ns <= media_ns(c.timeline_samples));
    if (rc != 0) {
        return rc;
    }

    Run resumed;
    spec = RunSpec();
    spec.resume = &c;
    rc |= replay(meta, spec, &resumed);
    if (rc != 0) {
        return rc;
    }
    rc |= expect_true("the resumed replay opens on the opening centre", resumed.center_hz == (uint32_t)kCenterHz);
    rc |= expect_true("the resumed replay's media time starts at the checkpoint",
                      resumed.log.blocks[0].media_start_ns == resume_media_ns(c));
    rc |= expect_true("the decode time never goes behind what the stopped stream delivered",
                      resumed.log.blocks[0].media_start_ns >= stopped.last_read_media_ns);
    rc |= expect_true("the resumed replay applies the RETUNE, the RESET and the second MUTE only",
                      resumed.state.replay_event_retune_count == 1U && resumed.state.replay_event_reset_count == 1U
                          && resumed.state.replay_event_mute_count == 1U
                          && resumed.state.replay_event_last_mute_bytes == kMute2Bytes);
    const long from = find_block(straight.log.blocks, resume_media_ns(c), 0U);
    rc |= expect_continues("resumed after the MUTE", straight.log.blocks, from, resumed.log.blocks, 1, 1);
    if (from >= 0) {
        uint64_t bytes = 0U;
        for (const Block& b : resumed.log.blocks) {
            bytes += b.floats.size(); /* cu8: one byte per float */
        }
        rc |= expect_true("the resumed replay delivered the capture's bytes from the checkpoint on",
                          bytes == kCaptureSamples * 2U - c.data_offset);
    }

    /* The same checkpoint at another rate: an explicit 24 kHz DSP bandwidth decimates the same capture blocks by 2,
       and TETRA resamples them to 72 kHz; the media time goes on from the checkpoint either way. */
    Run narrow;
    spec = RunSpec();
    spec.bw_khz = 24;
    spec.resume = &c;
    rc |= replay(meta, spec, &narrow);
    rc |= expect_true("24 kHz runs the chain from the capture's 48 kHz", narrow.state.dongle_rate_hz == kRateHz);
    rc |= expect_true("24 kHz runs the demodulator at 24 kHz", narrow.demod_rate_hz == 24000);
    rc |= expect_continues("resumed at 24 kHz", straight.log.blocks, from, narrow.log.blocks, 1, 1);

    Run tetra;
    spec = RunSpec();
    spec.tetra = 1;
    spec.resume = &c;
    rc |= replay(meta, spec, &tetra);
    rc |= expect_true("TETRA resamples the resumed replay to 72 kHz", tetra.state.dongle_rate_hz == 72000U);
    rc |= expect_true("TETRA's resumed media time starts at the checkpoint",
                      !tetra.log.blocks.empty() && tetra.log.blocks[0].media_start_ns == resume_media_ns(c));
    rc |= expect_continues("resumed under TETRA", straight.log.blocks, from, tetra.log.blocks, 0, 1);
    return rc;
}

int
test_resume_after_retune(const char* meta) {
    int rc = 0;
    Run stopped;
    RunSpec spec;
    spec.stop_after_sequence = 110U; /* past the RETUNE and RESET, before the second MUTE */
    rc |= replay(meta, spec, &stopped);
    if (rc != 0) {
        return rc;
    }
    const dsd_iq_replay_checkpoint& c = stopped.checkpoint;
    rc |= expect_true("the checkpoint lies past the RETUNE and before the second MUTE",
                      c.data_offset > kRetuneAt && c.data_offset < kMute2At && c.event_index == 3U);
    Run resumed;
    spec = RunSpec();
    spec.resume = &c;
    rc |= replay(meta, spec, &resumed);
    rc |= expect_true("the resumed replay opens on the centre the RETUNE moved to",
                      resumed.center_hz == (uint32_t)kSecondCenterHz);
    rc |= expect_true("the resumed replay applies the second MUTE only",
                      resumed.state.replay_event_retune_count == 0U && resumed.state.replay_event_mute_count == 1U);
    return rc;
}

int
test_resume_after_loop(const char* meta) {
    int rc = 0;
    Run straight;
    Run stopped;
    RunSpec spec;
    spec.loop = 1;
    spec.run_passes = 3U;
    rc |= replay(meta, spec, &straight);
    spec = RunSpec();
    spec.loop = 1;
    spec.stop_after_passes = 1U;
    spec.stop_after_sequence = 230U; /* some 40 chunks into the second pass */
    rc |= replay(meta, spec, &stopped);
    if (rc != 0) {
        return rc;
    }
    const dsd_iq_replay_checkpoint& c = stopped.checkpoint;
    rc |= expect_true("the checkpoint counts the completed pass", c.loop_pass == 1U);
    rc |= expect_true("the checkpoint's media timeline counts the whole first pass",
                      c.timeline_samples > kCaptureSamples + (kMute1Bytes + kMute2Bytes) / 2U
                          && c.timeline_samples == kCaptureSamples + (kMute1Bytes + kMute2Bytes) / 2U + c.pass_samples);
    Run resumed;
    spec = RunSpec();
    spec.loop = 1;
    spec.run_passes = 3U;
    spec.resume = &c;
    rc |= replay(meta, spec, &resumed);
    if (rc != 0) {
        return rc;
    }
    rc |= expect_true("the resumed loop goes on counting passes",
                      !resumed.log.blocks.empty() && resumed.log.blocks[0].loop_pass == 1U);
    const long from = find_block(straight.log.blocks, resume_media_ns(c), 1U);
    rc |= expect_continues("resumed on the second pass", straight.log.blocks, from, resumed.log.blocks, 1, 0);
    return rc;
}

/* A replay opened on an explicit bandwidth request (a menu, API or config bandwidth, the startup INI's included) runs
 * at it, not at the 48 kHz its sidecar recorded. */
int
test_explicit_request_beats_the_recording(const char* meta) {
    int rc = 0;
    Run run;
    RunSpec spec;
    spec.bw_khz = 24;
    spec.stop_after_sequence = 4U;
    rc |= replay(meta, spec, &run);
    rc |= expect_true("an explicit 24 kHz request runs the demodulator at 24 kHz", run.demod_rate_hz == 24000);
    return rc;
}

/* The running replay's re-plan check (rtl_stream_replay_replan_check()): the options it runs plan the chain it runs;
 * TETRA plans 72 kHz, resampled; an explicit 24 kHz plans 24 kHz by decimation; with the stream stopped there is no
 * replay to re-plan. */
int
test_replan_check(const char* meta) {
    std::unique_ptr<dsd_opts> opts(new dsd_opts());
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = DSD_IQ_REPLAY_RATE_FAST;
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", meta);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", meta);
    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not start the replay of %s\n", meta);
        if (ctx) {
            (void)rtl_stream_destroy(ctx);
        }
        return 1;
    }
    int rc = 0;
    uint32_t rate = 0U;
    int resampled = -1;
    char why[160];
    rc |= expect_true("the options the replay runs plan its chain",
                      rtl_stream_replay_replan_check(opts.get(), &rate, &resampled, why, sizeof(why))
                          == RTL_STREAM_REPLAN_SAME);
    std::unique_ptr<dsd_opts> tetra(new dsd_opts(*opts));
    tetra->frame_tetra = 1;
    rc |= expect_true("TETRA plans another chain",
                      rtl_stream_replay_replan_check(tetra.get(), &rate, &resampled, why, sizeof(why))
                          == RTL_STREAM_REPLAN_DIFFERS);
    rc |= expect_true("TETRA's chain is resampled to 72 kHz", rate == 72000U && resampled == 1);
    std::unique_ptr<dsd_opts> narrow(new dsd_opts(*opts));
    dsd_opts_set_rtl_dsp_bw(narrow.get(), 24, DSD_RTL_DSP_BW_SOURCE_EXPLICIT);
    rc |= expect_true("an explicit 24 kHz plans another chain",
                      rtl_stream_replay_replan_check(narrow.get(), &rate, &resampled, why, sizeof(why))
                          == RTL_STREAM_REPLAN_DIFFERS);
    rc |= expect_true("24 kHz decimates without resampling", rate == 24000U && resampled == 0);
    std::unique_ptr<dsd_opts> preset(new dsd_opts(*opts));
    dsd_opts_set_rtl_dsp_bw(preset.get(), 24, DSD_RTL_DSP_BW_SOURCE_PRESET);
    rc |= expect_true("a preset's 24 kHz keeps the recorded chain",
                      rtl_stream_replay_replan_check(preset.get(), &rate, &resampled, why, sizeof(why))
                          == RTL_STREAM_REPLAN_SAME);
    (void)rtl_stream_stop(ctx);
    (void)rtl_stream_destroy(ctx);
    rc |= expect_true("a stopped stream has no replay to re-plan",
                      rtl_stream_replay_replan_check(opts.get(), &rate, &resampled, why, sizeof(why))
                          == RTL_STREAM_REPLAN_NO_REPLAY);
    return rc;
}

} // namespace

int
main(void) {
    int rc = 0;
    const std::vector<uint8_t> payload = tone_payload(kCaptureSamples);
    char meta[DSD_TEST_PATH_MAX];
    if (write_capture(payload, meta, sizeof(meta)) != 0) {
        return 1;
    }
    rc |= test_resume_after_mute(meta);
    rc |= test_resume_after_retune(meta);
    rc |= test_resume_after_loop(meta);
    rc |= test_explicit_request_beats_the_recording(meta);
    rc |= test_replan_check(meta);
    static const char* const kFiles[] = {"capture.iq", "capture.iq.json", NULL};
    for (const std::string& dir : g_dirs) {
        if (dsd_test_remove_temp_dir(dir.c_str(), kFiles) != 0) {
            DSD_FPRINTF(stderr, "FAIL: could not remove %s\n", dir.c_str());
            rc = 1;
        }
    }
    if (rc == 0) {
        printf("IO_RTL_REPLAY_RESUME: OK\n");
    }
    return rc;
}
