// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #806 X12: samples a producer lost before they reached the input ring leave a gap in the stream, and the demod
 * starts its filters over (the half-band cascade, the I/Q rate step, the channel and timing histories) exactly at the
 * first sample after it, as at a stream gap. Before, the stream ran on with every history spanning the gap.
 *
 * - Replay: a capture with driver overflows (MUTE driver_overflow) resets the demod once at each, at the block that
 *   starts at the overflow's offset; an overflow at the offset of a retune's RETUNE and RESET resets once (the RESET);
 *   a retune's omitted span (MUTE retune_mute) is no gap and resets nothing; a block after a gap that is dropped
 *   before the demod enters it (a closing gate) hands the reset it owed to the next block.
 * - Live (an Airspy on the in-process SDK double): transfers of arbitrary short sizes, some reporting samples the SDK
 *   dropped before them, reset the demod at each drop that follows filtered input (a drop before any input follows
 *   the stream start's own reset); the capture records each drop as a driver_overflow MUTE, and its replay resets
 *   exactly as often as the live stream did.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "rtl_stream_test_support.h"
#include "test_support.h"

#ifdef DSD_TEST_AIRSPY_LIVE
#include <cstring>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/rtl_stream.h>
#include "airspy_sdk_double.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr uint32_t kRateHz = 48000U;
constexpr uint64_t kCenterHz = 851375000ULL;
constexpr uint64_t kSecondCenterHz = 852000000ULL;
constexpr size_t kCaptureSamples = 48000U;
/* Event offsets in cu8 capture bytes, each off the chunk grid. */
constexpr uint64_t kOverflow1At = 20002U;
constexpr uint64_t kRetuneAt = 50010U;
constexpr uint64_t kRetuneMuteAt = 70000U;
constexpr uint64_t kOverflow2At = 80000U;

std::vector<std::string> g_dirs;

int
expect_true(const char* label, bool cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

uint64_t
gap_resets_now(void) {
    rtl_stream_test_replay_state st;
    DSD_MEMSET(&st, 0, sizeof(st));
    (void)dsd_rtl_stream_test_get_replay_state(&st);
    return st.input_gap_reset_count;
}

/* Each replay block's capture data offset and the gap resets counted once the demod ran it. */
struct Block {
    uint64_t sequence;
    uint64_t data_offset;
    uint64_t resets_before;
    uint64_t resets_after;
};

struct BlockLog {
    std::vector<Block> blocks;
    uint64_t next_offset = 0U;
    size_t bytes_per_float = 1U;
};

void
block_hook(const rtl_stream_test_replay_block* block, void* ctx) {
    BlockLog* log = static_cast<BlockLog*>(ctx);
    const uint64_t now = gap_resets_now();
    log->blocks.push_back(Block{block->sequence, log->next_offset, now, now});
    log->next_offset += (uint64_t)block->float_count * log->bytes_per_float;
}

void
stage_hook(int stage, size_t count, void* ctx) {
    (void)count;
    BlockLog* log = static_cast<BlockLog*>(ctx);
    if (stage == RTL_STREAM_TEST_REPLAY_DEMOD_BEFORE_OUTPUT_WRITE && !log->blocks.empty()) {
        log->blocks.back().resets_after = gap_resets_now();
    }
}

struct Run {
    BlockLog log;
    rtl_stream_test_replay_state state;
    uint64_t resets = 0U;
};

int
replay(const char* path, size_t bytes_per_float, Run* out) {
    std::unique_ptr<dsd_opts> opts(new dsd_opts());
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = DSD_IQ_REPLAY_RATE_FAST;
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", path);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", path);
    /* Heap-held while the hooks are installed (the stream keeps their context in a global), moved out after. */
    std::unique_ptr<BlockLog> log(new BlockLog());
    log->bytes_per_float = bytes_per_float;
    rtl_stream_test_set_replay_block_hook(block_hook, log.get());
    rtl_stream_test_set_replay_stage_hook(stage_hook, log.get());
    RtlSdrContext* ctx = NULL;
    int rc = 0;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not start the replay of %s\n", path);
        rc = 1;
    } else {
        const uint64_t start = gap_resets_now();
        float audio[2048];
        const uint64_t start_ns = dsd_time_monotonic_ns();
        for (;;) {
            int got = 0;
            if (rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got) != 0) {
                break;
            }
            if (dsd_time_monotonic_ns() - start_ns > 60000000000ULL) {
                DSD_FPRINTF(stderr, "FAIL: timed out reading the replay of %s\n", path);
                rc = 1;
                break;
            }
        }
        DSD_MEMSET(&out->state, 0, sizeof(out->state));
        (void)dsd_rtl_stream_test_get_replay_state(&out->state);
        out->resets = out->state.input_gap_reset_count - start;
        (void)rtl_stream_stop(ctx);
    }
    if (ctx) {
        (void)rtl_stream_destroy(ctx);
    }
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);
    out->log = std::move(*log);
    return rc;
}

std::vector<uint8_t>
tone_payload(size_t samples) {
    std::vector<uint8_t> payload(samples * 2U);
    for (size_t i = 0; i < samples; i++) {
        const double phase = 2.0 * M_PI * 3100.0 * (double)i / (double)kRateHz;
        payload[2U * i] = (uint8_t)lrint(127.5 + 80.0 * std::cos(phase));
        payload[2U * i + 1U] = (uint8_t)lrint(127.5 + 80.0 * std::sin(phase));
    }
    return payload;
}

int
make_dir(const char* tag, char* out, size_t out_size) {
    if (!dsd_test_mkdtemp(out, out_size, tag)) {
        DSD_FPRINTF(stderr, "FAIL: could not create a fixture directory\n");
        return 1;
    }
    g_dirs.push_back(out);
    return 0;
}

int
record_event(dsd_iq_capture_writer* writer, dsd_iq_event_kind kind, uint64_t mute_bytes, const char* reason) {
    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = kind;
    if (kind == DSD_IQ_EVENT_MUTE) {
        ev.duration_bytes = mute_bytes;
    } else {
        ev.center_frequency_hz = kSecondCenterHz;
        ev.capture_center_frequency_hz = kSecondCenterHz + kRateHz / 4U;
        ev.sample_rate_hz = kRateHz;
    }
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", reason);
    return dsd_iq_capture_record_event(writer, &ev) == DSD_IQ_OK ? 0 : 1;
}

int
write_capture(char* out_meta, size_t out_size) {
    char dir[DSD_TEST_PATH_MAX];
    char data_path[DSD_TEST_PATH_MAX];
    if (make_dir("dsdneo_input_gap", dir, sizeof(dir)) != 0
        || dsd_test_path_join(data_path, sizeof(data_path), dir, "capture.iq") != 0
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
    const std::vector<uint8_t> p = tone_payload(kCaptureSamples);
    int rc = dsd_iq_capture_submit(writer, p.data(), (size_t)kOverflow1At);
    rc |= record_event(writer, DSD_IQ_EVENT_MUTE, 3074U, RTL_CAPTURE_MUTE_REASON_DRIVER_OVERFLOW);
    rc |= dsd_iq_capture_submit(writer, p.data() + kOverflow1At, (size_t)(kRetuneAt - kOverflow1At));
    /* A driver overflow inside a retune: its RETUNE, the overflow, then its RESET, all at one offset. */
    rc |= record_event(writer, DSD_IQ_EVENT_RETUNE, 0U, "frequency");
    rc |= record_event(writer, DSD_IQ_EVENT_MUTE, 1000U, RTL_CAPTURE_MUTE_REASON_DRIVER_OVERFLOW);
    rc |= record_event(writer, DSD_IQ_EVENT_RESET, 0U, "frequency");
    rc |= dsd_iq_capture_submit(writer, p.data() + kRetuneAt, (size_t)(kRetuneMuteAt - kRetuneAt));
    rc |= record_event(writer, DSD_IQ_EVENT_MUTE, 2048U, "retune_mute");
    rc |= dsd_iq_capture_submit(writer, p.data() + kRetuneMuteAt, (size_t)(kOverflow2At - kRetuneMuteAt));
    rc |= record_event(writer, DSD_IQ_EVENT_MUTE, 512U, RTL_CAPTURE_MUTE_REASON_DRIVER_OVERFLOW);
    rc |= dsd_iq_capture_submit(writer, p.data() + kOverflow2At, p.size() - (size_t)kOverflow2At);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not write the capture\n");
        dsd_iq_capture_abort(writer);
        return 1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    return 0;
}

int
test_replay_resets_at_overflows(void) {
    int rc = 0;
    char meta[DSD_TEST_PATH_MAX];
    if (write_capture(meta, sizeof(meta)) != 0) {
        return 1;
    }
    Run run;
    rc |= replay(meta, 1U, &run);
    if (rc != 0) {
        return rc;
    }
    rc |= expect_true("replay applied every MUTE and the RESET",
                      run.state.replay_event_mute_count == 4U && run.state.replay_event_reset_count == 1U);
    rc |=
        expect_true("two gap resets: the overflows, not the one a RESET covers nor a retune's span", run.resets == 2U);
    std::vector<uint64_t> reset_at;
    for (const Block& b : run.log.blocks) {
        if (b.resets_after != b.resets_before) {
            rc |= expect_true("one reset a block", b.resets_after == b.resets_before + 1U);
            reset_at.push_back(b.data_offset);
        }
    }
    rc |= expect_true("each reset at the first sample after its overflow",
                      reset_at == std::vector<uint64_t>({kOverflow1At, kOverflow2At}));

    /* The block after the first overflow is dropped before the demod enters it, as a gate closing on it would: the
       reset it owed runs at the next block, and no gap goes without one. */
    uint64_t refused = 0U;
    uint64_t next_offset = 0U;
    for (size_t i = 0; i + 1U < run.log.blocks.size(); i++) {
        if (run.log.blocks[i].data_offset == kOverflow1At) {
            refused = run.log.blocks[i].sequence;
            next_offset = run.log.blocks[i + 1U].data_offset;
        }
    }
    rc |= expect_true("the overflow's block found", refused != 0U);
    rtl_stream_test_replay_refuse_chunk(refused);
    Run dropped;
    rc |= replay(meta, 1U, &dropped);
    rtl_stream_test_replay_refuse_chunk(0U);
    rc |= expect_true("a dropped block keeps the reset it owed", dropped.resets == 2U);
    reset_at.clear();
    for (const Block& b : dropped.log.blocks) {
        if (b.resets_after != b.resets_before) {
            reset_at.push_back(b.data_offset);
        }
    }
    rc |= expect_true("the owed reset runs at the next block",
                      reset_at == std::vector<uint64_t>({next_offset, kOverflow2At}));
    return rc;
}

#ifdef DSD_TEST_AIRSPY_LIVE
/* Waits (5 s at most) for the demod to have taken everything delivered: the gap resets of what it took are counted
 * by then, each before its block. */
int
wait_input_drained(void) {
    for (int i = 0; i < 5000; i++) {
        rtl_stream_test_replay_state st;
        DSD_MEMSET(&st, 0, sizeof(st));
        if (dsd_rtl_stream_test_get_replay_state(&st) == 0 && st.input_ring_used == 0U) {
            return 0;
        }
        dsd_sleep_ms(1);
    }
    DSD_FPRINTF(stderr, "FAIL: the demod did not take the delivered samples\n");
    return 1;
}

int
test_live_resets_match_replay(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    char data_path[DSD_TEST_PATH_MAX];
    char meta_path[DSD_TEST_PATH_MAX];
    if (make_dir("dsdneo_input_gap_live", dir, sizeof(dir)) != 0
        || dsd_test_path_join(data_path, sizeof(data_path), dir, "live.iq") != 0
        || dsd_test_path_join(meta_path, sizeof(meta_path), dir, "live.iq.json") != 0) {
        return 1;
    }
    auto opts = std::make_unique<dsd_opts>();
    *opts = {};
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 851375000;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_volume_multiplier = 1;
    opts->frame_p25p1 = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "airspy");
    dsd_airspy_config_defaults(&opts->airspy);
    opts->iq_capture_requested = 1;
    opts->iq_capture_format = DSD_IQ_FORMAT_CF32;
    DSD_SNPRINTF(opts->iq_capture_path, sizeof opts->iq_capture_path, "%s", data_path);

    static float samples[2 * 4096];
    for (size_t i = 0; i < 4096U; i++) {
        samples[2U * i] = 0.3f * (float)std::cos(0.01 * (double)i);
        samples[2U * i + 1U] = 0.3f * (float)std::sin(0.01 * (double)i);
    }
    uint64_t live_resets = 0U;
    int drops = 0;
    {
        RtlSdrOrchestrator stream(*opts);
        if (stream.start() != 0) {
            DSD_FPRINTF(stderr, "FAIL: the live stream did not start\n");
            return 1;
        }
        const uint64_t start = gap_resets_now();
        uint32_t lcg = 0x2468U;
        for (int i = 0; i < 40; i++) {
            lcg = lcg * 1664525U + 1013904223U;
            const int pairs = 1 + (int)((lcg >> 8) % 3000U);
            /* The first transfer reports a drop before anything was filtered; then every fifth. */
            const uint64_t dropped = (i == 0 || i % 5 == 3) ? (uint64_t)(17 + i) : 0U;
            rc |= expect_true("delivered", airspy_double::deliver_dropped(samples, pairs, dropped));
            drops += dropped ? 1 : 0;
            rc |= wait_input_drained();
        }
        live_resets = gap_resets_now() - start;
        rc |= expect_true("stop", stream.stop() == 0);
    }
    rc |= expect_true("live: a reset at every drop after filtered input", live_resets == (uint64_t)(drops - 1));

    dsd_iq_replay_config meta;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    char err[256] = {0};
    rc |= expect_true("live capture sidecar reads",
                      dsd_iq_replay_read_metadata(meta_path, &meta, err, sizeof(err)) == DSD_IQ_OK);
    /* The live rate chain the capture records (issue #806): the Airspy's own rate as the nominal one, forced, and the
       DSP rate the step makes, so the replay below runs the same step. */
    const dsd_iq_rate_chain* chain = &meta.rate_chain;
    rc |=
        expect_true("live capture records its rate chain", chain->has_nominal && chain->has_forced && chain->has_step);
    rc |= expect_true("live capture: the nominal rate is the device's", chain->nominal_rate_hz == meta.sample_rate_hz);
    rc |= expect_true("live capture: the DSP rate is the bandwidth's", chain->dsp_rate_hz == 48000U);
    rc |= expect_true("live capture: a forced rate the half-band chain does not reach steps",
                      chain->device_forced == 1
                          && (chain->step_l != 0U) == (meta.sample_rate_hz / meta.base_decimation != 48000U));
    /* The double's 10 MS/s over 2^7 is 78,125 Hz: the chain steps 384/625, so the replay below resets at each
       driver_overflow MUTE on a stepped chain, the step included (dsd_demod_reset_filter_state()). */
    rc |= expect_true("live capture: the chain steps", chain->step_l == 384U && chain->step_m == 625U);
    uint32_t overflows = 0U;
    for (uint32_t i = 0; i < meta.event_count; i++) {
        if (meta.events[i].kind == DSD_IQ_EVENT_MUTE
            && strcmp(meta.events[i].reason, RTL_CAPTURE_MUTE_REASON_DRIVER_OVERFLOW) == 0) {
            overflows++;
        }
    }
    dsd_iq_replay_config_clear(&meta);
    rc |= expect_true("the capture records each drop", overflows == (uint32_t)drops);

    Run run;
    rc |= replay(meta_path, sizeof(float), &run);
    rc |= expect_true("replay resets exactly as the live stream did", run.resets == live_resets);
    return rc;
}
#endif

} // namespace

int
main(void) {
    int rc = 0;
    rc |= test_replay_resets_at_overflows();
#ifdef DSD_TEST_AIRSPY_LIVE
    rc |= test_live_resets_match_replay();
#endif
    for (const std::string& dir : g_dirs) {
        static const char* const kFiles[] = {"capture.iq", "capture.iq.json", "live.iq", "live.iq.json", nullptr};
        (void)dsd_test_remove_temp_dir(dir.c_str(), kFiles);
    }
    if (rc == 0) {
        printf("IO_RTL_STREAM_INPUT_GAP: OK\n");
    }
    return rc ? 1 : 0;
}
