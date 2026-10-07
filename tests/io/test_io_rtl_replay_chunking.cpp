// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * An I/Q replay hands the demodulator one capture chunk per block (issue #572), so the chunk is the replay's demod
 * block, and every per-block decision (the level squelch among them) is made at its size. Issue #626: a chunk is at
 * most one live RTL transfer and no longer than such a transfer lasts at 1.536 Msps (dsd_iq_replay_chunk_bytes()), so
 * a replay runs those decisions at a live receiver's cadence. A fixed 64 KiB chunk was 683 ms of a 48 kHz capture, and
 * a replayed level squelch opened and closed on that grid.
 *
 * - Framing: at 48 kHz a cu8 block is 512 floats (256 samples, 5.33 ms), a cf32 one 512 floats from 2048 bytes; at
 *   1.536 Msps a cu8 block is 16384 floats. A MUTE cuts the chunk it falls in and moves the media time on by the time
 *   it omits; the capture's end cuts the last chunk.
 * - Level edges: nfm_burst_synth (receiver noise, a carrier from 0.30 to 0.55 s, noise) under a level between its
 *   noise and its carrier opens on exactly one run of blocks, starting within one block of the carrier's key and
 *   ending within one block of its unkey, as the channel filter delays them.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>
#include <memory>
#include <vector>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "rtl_stream_test_support.h"
#include "test_support.h"

namespace {

/* One demod block as the block hook reported it, and the channel power the demod published after it. */
struct LoggedBlock {
    uint64_t sequence;
    uint64_t submit_gen;
    uint64_t media_start_ns;
    uint64_t media_end_ns;
    size_t float_count;
    int have_power;
    double power;
};

/* The demod thread writes it; the test reads it once the stream is stopped. */
struct BlockLog {
    std::vector<LoggedBlock> blocks;
};

/* A capture chunk the replay reader hands the demod as one block. */
struct ExpectedChunk {
    size_t float_count;
    uint64_t start_complex; /* capture position of its first sample, samples a MUTE omitted included */
};

struct FixtureDir {
    char dir[DSD_TEST_PATH_MAX];
};

std::vector<FixtureDir> g_fixture_dirs;

} // namespace

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static void
block_log_hook(const rtl_stream_test_replay_block* block, void* ctx) {
    BlockLog* log = static_cast<BlockLog*>(ctx);
    LoggedBlock entry = {
        block->sequence, block->submit_gen, block->media_start_ns, block->media_end_ns, block->float_count, 0, 0.0};
    log->blocks.push_back(entry);
}

/* The demod publishes the block's channel power before it writes the block's output: pair it with the block it just
 * took. A block the demod discards never gets here and keeps no power. */
static void
block_power_stage(int stage, size_t count, void* ctx) {
    (void)count;
    if (stage != RTL_STREAM_TEST_REPLAY_DEMOD_BEFORE_OUTPUT_WRITE) {
        return;
    }
    BlockLog* log = static_cast<BlockLog*>(ctx);
    if (!log->blocks.empty()) {
        log->blocks.back().have_power = 1;
        log->blocks.back().power = rtl_stream_return_pwr(NULL);
    }
}

static int
write_fixture(const char* tag, dsd_iq_sample_format format, uint32_t rate_hz, const std::vector<uint8_t>& head,
              uint64_t mute_bytes, const std::vector<uint8_t>& tail, char* out_metadata_path, size_t out_size) {
    FixtureDir fixture = {};
    if (!dsd_test_mkdtemp(fixture.dir, sizeof(fixture.dir), tag)) {
        DSD_FPRINTF(stderr, "FAIL: %s: could not create a fixture directory\n", tag);
        return 1;
    }
    g_fixture_dirs.push_back(fixture);
    char data_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), fixture.dir, "chunks.iq") != 0
        || dsd_test_path_join(out_metadata_path, out_size, fixture.dir, "chunks.iq.json") != 0) {
        return 1;
    }

    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", out_metadata_path);
    cfg.format = format;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s",
                 format == DSD_IQ_FORMAT_CF32 ? "post_driver_cf32_pre_ring" : "post_mute_pre_widen");
    cfg.sample_rate_hz = rate_hz;
    cfg.center_frequency_hz = 851375000ULL;
    cfg.capture_center_frequency_hz = 851375000ULL;
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = rate_hz / 48000U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = 48000U;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=0");

    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: %s: could not open the capture writer: %s\n", tag, err_buf);
        return 1;
    }
    int rc = dsd_iq_capture_submit(writer, head.data(), head.size());
    if (rc == DSD_IQ_OK && mute_bytes > 0U) {
        dsd_iq_event ev;
        DSD_MEMSET(&ev, 0, sizeof(ev));
        ev.kind = DSD_IQ_EVENT_MUTE;
        ev.duration_bytes = mute_bytes;
        DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "driver_overflow");
        rc = dsd_iq_capture_record_event(writer, &ev);
    }
    if (rc == DSD_IQ_OK && !tail.empty()) {
        rc = dsd_iq_capture_submit(writer, tail.data(), tail.size());
    }
    if (rc != DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "FAIL: %s: could not write the capture (rc=%d)\n", tag, rc);
        dsd_iq_capture_abort(writer);
        return 1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    return 0;
}

static std::vector<uint8_t>
cu8_payload(size_t bytes) {
    std::vector<uint8_t> payload(bytes);
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<uint8_t>((i * 7U) & 0xFFU);
    }
    return payload;
}

static std::vector<uint8_t>
cf32_payload(size_t complex_count) {
    std::vector<float> samples(complex_count * 2U);
    for (size_t i = 0; i < complex_count; i++) {
        samples[i * 2U + 0U] = 0.25f * (float)((i % 7U)) - 0.75f;
        samples[i * 2U + 1U] = 0.125f * (float)((i % 5U)) - 0.25f;
    }
    std::vector<uint8_t> bytes(samples.size() * sizeof(float));
    DSD_MEMCPY(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

/* Replays @p metadata_path to its end with @p squelch_level in force (0: off), logging every demod block. */
static int
replay_and_log(const char* metadata_path, double squelch_level, BlockLog* log) {
    std::unique_ptr<dsd_opts> opts(new dsd_opts());
    DSD_MEMSET(opts.get(), 0, sizeof(dsd_opts));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = DSD_IQ_REPLAY_RATE_FAST;
    opts->rtl_squelch_level = squelch_level;
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", metadata_path);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", metadata_path);

    rtl_stream_test_set_replay_block_hook(block_log_hook, log);
    rtl_stream_test_set_replay_stage_hook(block_power_stage, log);
    RtlSdrContext* ctx = NULL;
    int rc = 0;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not start the replay of %s\n", metadata_path);
        rc = 1;
    } else {
        float audio[2048];
        const uint64_t start_ns = dsd_time_monotonic_ns();
        for (;;) {
            int got = 0;
            if (rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got) != 0) {
                break;
            }
            if (dsd_time_monotonic_ns() - start_ns > 60000000000ULL) {
                DSD_FPRINTF(stderr, "FAIL: timed out reading the replay of %s\n", metadata_path);
                rc = 1;
                break;
            }
        }
        (void)rtl_stream_stop(ctx);
    }
    if (ctx) {
        (void)rtl_stream_destroy(ctx);
    }
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);
    return rc;
}

static uint64_t
media_ns(uint64_t complex_samples, uint32_t rate_hz) {
    return (complex_samples / rate_hz) * 1000000000ULL + ((complex_samples % rate_hz) * 1000000000ULL) / rate_hz;
}

/* One block a chunk: each block has its chunk's floats, a sequence and submit generation counting from 1, and the
 * chunk's media span; no block spans more than a live transfer at 1.536 Msps (16/3 ms). */
static int
expect_blocks(const char* label, const BlockLog& log, const std::vector<ExpectedChunk>& chunks, uint32_t rate_hz) {
    int rc = 0;
    if (log.blocks.size() != chunks.size()) {
        DSD_FPRINTF(stderr, "FAIL: %s: the demod took %zu blocks for the capture's %zu chunks\n", label,
                    log.blocks.size(), chunks.size());
        rc = 1;
    }
    const size_t n = log.blocks.size() < chunks.size() ? log.blocks.size() : chunks.size();
    for (size_t i = 0; i < n; i++) {
        const LoggedBlock& got = log.blocks[i];
        const ExpectedChunk& want = chunks[i];
        const uint64_t want_start = media_ns(want.start_complex, rate_hz);
        const uint64_t want_end = media_ns(want.start_complex + want.float_count / 2U, rate_hz);
        if (got.float_count != want.float_count || got.sequence != i + 1U || got.submit_gen != i + 1U
            || got.media_start_ns != want_start || got.media_end_ns != want_end
            || got.media_end_ns - got.media_start_ns > 5333334U) {
            DSD_FPRINTF(stderr,
                        "FAIL: %s: block %zu has %zu floats, sequence %llu, generation %llu, media %llu-%llu ns; "
                        "chunk %zu has %zu floats, media %llu-%llu ns\n",
                        label, i + 1U, got.float_count, (unsigned long long)got.sequence,
                        (unsigned long long)got.submit_gen, (unsigned long long)got.media_start_ns,
                        (unsigned long long)got.media_end_ns, i + 1U, want.float_count, (unsigned long long)want_start,
                        (unsigned long long)want_end);
            rc = 1;
        }
    }
    return rc;
}

/* Chunks of @p chunk_floats floats over @p floats floats starting at capture sample @p start_complex. */
static void
append_chunks(std::vector<ExpectedChunk>* chunks, size_t floats, size_t chunk_floats, uint64_t start_complex) {
    for (size_t done = 0; done < floats; done += chunk_floats) {
        const size_t len = floats - done < chunk_floats ? floats - done : chunk_floats;
        chunks->push_back({len, start_complex + done / 2U});
    }
}

/* 48 kHz cu8, the committed fixtures' format: 256 samples (512 bytes, 5.33 ms) a block, as a live block decimates to
 * at the default 1.536 Msps, where the 64 KiB chunk was 683 ms. A MUTE 700 bytes in cuts the second chunk at 188
 * floats and omits 500 samples of media time; the capture's end cuts the last chunk. */
static int
test_cu8_48k_blocks_are_one_live_transfer(void) {
    char metadata_path[DSD_TEST_PATH_MAX];
    if (write_fixture("dsdneo_chunks_cu8", DSD_IQ_FORMAT_CU8, 48000U, cu8_payload(700U), 1000U, cu8_payload(2000U),
                      metadata_path, sizeof(metadata_path))
        != 0) {
        return 1;
    }
    BlockLog log;
    int rc = replay_and_log(metadata_path, 0.0, &log);
    std::vector<ExpectedChunk> chunks;
    append_chunks(&chunks, 700U, 512U, 0U);
    append_chunks(&chunks, 2000U, 512U, 350U + 500U);
    rc |= expect_blocks("cu8 48 kHz", log, chunks, 48000U);
    return rc;
}

/* 48 kHz cf32: 256 samples a block too, 2048 bytes, 512 floats. */
static int
test_cf32_48k_blocks_are_one_live_transfer(void) {
    char metadata_path[DSD_TEST_PATH_MAX];
    if (write_fixture("dsdneo_chunks_cf32", DSD_IQ_FORMAT_CF32, 48000U, cf32_payload(1000U), 0U, {}, metadata_path,
                      sizeof(metadata_path))
        != 0) {
        return 1;
    }
    BlockLog log;
    int rc = replay_and_log(metadata_path, 0.0, &log);
    std::vector<ExpectedChunk> chunks;
    append_chunks(&chunks, 2000U, 512U, 0U);
    rc |= expect_blocks("cf32 48 kHz", log, chunks, 48000U);
    return rc;
}

/* 1.536 Msps cu8: exactly a live transfer, 16384 bytes, where the chunk was 65536. */
static int
test_cu8_1536k_blocks_are_one_live_transfer(void) {
    char metadata_path[DSD_TEST_PATH_MAX];
    if (write_fixture("dsdneo_chunks_rtl", DSD_IQ_FORMAT_CU8, 1536000U, cu8_payload(40000U), 0U, {}, metadata_path,
                      sizeof(metadata_path))
        != 0) {
        return 1;
    }
    BlockLog log;
    int rc = replay_and_log(metadata_path, 0.0, &log);
    std::vector<ExpectedChunk> chunks;
    append_chunks(&chunks, 40000U, 16384U, 0U);
    rc |= expect_blocks("cu8 1.536 Msps", log, chunks, 1536000U);
    return rc;
}

/* The 48 kHz capture sample a media time falls on: media times are rounded down from whole samples. */
static uint64_t
sample_at_48k(uint64_t ns) {
    return (ns * 48ULL + 999999ULL) / 1000000ULL;
}

static void
print_block_powers(const BlockLog& log, double level) {
    for (size_t i = 0; i < log.blocks.size(); i++) {
        const LoggedBlock& b = log.blocks[i];
        DSD_FPRINTF(stderr, "  block %3zu samples %6llu-%6llu power %s%7.2f dB%s\n", i + 1U,
                    (unsigned long long)sample_at_48k(b.media_start_ns),
                    (unsigned long long)sample_at_48k(b.media_end_ns), b.have_power ? "" : "(none) ",
                    b.have_power && b.power > 0.0 ? 10.0 * log10(b.power) : -200.0,
                    b.have_power && b.power >= level ? " open" : "");
    }
}

/* nfm_burst_synth's carrier is keyed at 0.30 s (sample 14400) and unkeyed at 0.55 s (26400), after a 2 ms ramp. */
static const uint64_t kBurstKeySample = 14400U;
static const uint64_t kBurstUnkeySample = 26400U;
/* The channel filter's delay at 48 kHz in the replay's profile: its first output is centred on its first input, and
 * each block's outputs end this many samples before the block does, so a block opens on input it took that long ago. */
static const uint64_t kChannelFilterDelaySamples = 67U;
/* A level 9.5 dB over the burst's noise and about 10 dB under its carrier. */
static const double kBurstLevelDb = -28.0;

/* The acceptance of issue #626: a replayed level squelch opens within one live block of a burst's start and closes
 * within one live block of its end. The gate decision is the channel power the demod published for each block against
 * the level (zeroed blocks leave filter tails in the audio after them, so the decision is read where it is made). On
 * the 64 KiB chunks both of the burst's chunks start on noise and no block opens. */
static int
test_level_squelch_edges_within_one_block(void) {
    char metadata_path[DSD_TEST_PATH_MAX];
    DSD_SNPRINTF(metadata_path, sizeof(metadata_path), "%s/nfm_burst_synth.iq.json", DSD_NEO_TEST_IQ_FIXTURE_DIR);
    const double level = dsd_squelch_level_from_sql(kBurstLevelDb);
    BlockLog log;
    int rc = replay_and_log(metadata_path, level, &log);
    size_t first_open = 0U;
    size_t last_open = 0U;
    size_t runs = 0U;
    int was_open = 0;
    for (size_t i = 0; i < log.blocks.size(); i++) {
        const int open = log.blocks[i].have_power && log.blocks[i].power >= level;
        if (open && !was_open) {
            runs++;
            if (runs == 1U) {
                first_open = i;
            }
        }
        if (open) {
            last_open = i;
        }
        was_open = open;
    }
    rc |= expect_true("level edges: the replay was cut into 256-sample blocks",
                      log.blocks.size() > 100U && log.blocks[0].float_count == 512U);
    rc |= expect_true("level edges: the gate opened on exactly one run of blocks", runs == 1U);
    if (runs >= 1U) {
        const uint64_t open_start = sample_at_48k(log.blocks[first_open].media_start_ns);
        const uint64_t open_end = sample_at_48k(log.blocks[last_open].media_end_ns);
        const uint64_t key = kBurstKeySample + kChannelFilterDelaySamples;
        const uint64_t unkey = kBurstUnkeySample + kChannelFilterDelaySamples;
        rc |= expect_true("level edges: the first open block starts within one block of the key",
                          open_start + 256U > key && open_start < key + 256U);
        rc |= expect_true("level edges: the last open block ends within one block of the unkey",
                          open_end + 256U > unkey && open_end < unkey + 256U);
    }
    if (rc != 0 || getenv("DSD_NEO_TEST_VERBOSE") != NULL) {
        print_block_powers(log, level);
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_cu8_48k_blocks_are_one_live_transfer();
    rc |= test_cf32_48k_blocks_are_one_live_transfer();
    rc |= test_cu8_1536k_blocks_are_one_live_transfer();
    rc |= test_level_squelch_edges_within_one_block();
    for (const FixtureDir& fixture : g_fixture_dirs) {
        static const char* const kFiles[] = {"chunks.iq", "chunks.iq.json", NULL};
        dsd_test_remove_temp_dir(fixture.dir, kFiles);
    }
    if (rc == 0) {
        DSD_FPRINTF(stdout, "IO_RTL_REPLAY_CHUNKING: OK\n");
    }
    return rc;
}
