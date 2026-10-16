// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A replay whose capture rate cannot reach the mode's DSP rate is resampled ahead of the half-band chain (issue #701).
 *
 * A 48 kHz cu8 capture with the fs/4 shift, a MUTE that omits an odd number of samples, a RETUNE and RESET, and a
 * second MUTE, replayed without TETRA (its recorded 48 kHz chain, no resampling) and under TETRA (resampled to 72 kHz):
 *
 * - The TETRA replay's chain runs from 72 kHz, before and after the events, and the native one from 48 kHz.
 * - Each TETRA block is the native block of the same capture span, resampled: between events, the concatenated TETRA
 *   blocks equal what a resampler of the same design makes of the concatenated native blocks, started fresh at the
 *   capture's start and at every event. So the conversion, the fs/4 phase carried across a MUTE, and the samples are
 *   those of an unresampled replay, and the resampler starts over at each discontinuity.
 * - Every TETRA block spans the native block's capture time, so media spans stay contiguous but for what a MUTE omits,
 *   and the MUTE is accounted in capture bytes; no block holds more than one live transfer.
 * - Each block publishes the input level of its raw capture chunk: the same snapshot as the native replay's.
 * - Fast, realtime, and fast with short reads deliver identical blocks.
 *
 * A WAV recording with no centre loops under --iq-loop at 30 kHz, resampled to 48 kHz: it opens with no frequency,
 * and every pass hands the demodulator the same blocks.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr uint32_t kRateHz = 48000U;
constexpr uint32_t kTetraHz = 72000U;
constexpr uint64_t kCenterHz = 851375000ULL;
constexpr uint64_t kSecondCenterHz = 852000000ULL;
constexpr size_t kCaptureSamples = 48000U;
/* Event offsets in capture bytes (cu8: 2 per sample), each at a chunk-unaligned place. */
constexpr uint64_t kMute1At = 20002U;
constexpr uint64_t kMute1Bytes = 3074U; /* 1537 samples: the fs/4 phase moves on by 1 */
constexpr uint64_t kRetuneAt = 50010U;
constexpr uint64_t kMute2At = 70000U;
constexpr uint64_t kMute2Bytes = 2048U;

struct Block {
    uint64_t sequence;
    uint64_t media_start_ns;
    uint64_t media_end_ns;
    uint32_t loop_pass;
    std::vector<float> floats;
    int have_level;
    dsd_input_level_snapshot level;
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
    b.have_level = 0;
    DSD_MEMSET(&b.level, 0, sizeof(b.level));
    log->blocks.push_back(std::move(b));
}

/* The demod publishes the block's input level as it starts the block, before its output. */
void
level_stage(int stage, size_t count, void* ctx) {
    (void)count;
    if (stage != RTL_STREAM_TEST_REPLAY_DEMOD_BEFORE_OUTPUT_WRITE) {
        return;
    }
    BlockLog* log = static_cast<BlockLog*>(ctx);
    if (!log->blocks.empty()) {
        log->blocks.back().have_level = rtl_stream_get_input_level(&log->blocks.back().level) == 0;
    }
}

std::vector<uint8_t>
tone_payload(size_t samples) {
    std::vector<uint8_t> payload(samples * 2U);
    uint32_t lcg = 0x1234567U;
    for (size_t i = 0; i < samples; i++) {
        const double phase = 2.0 * M_PI * (12000.0 + 3100.0) * (double)i / (double)kRateHz;
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
make_dir(const char* tag, char* out, size_t out_size) {
    if (!dsd_test_mkdtemp(out, out_size, tag)) {
        DSD_FPRINTF(stderr, "FAIL: could not create a fixture directory\n");
        return 1;
    }
    g_dirs.push_back(out);
    return 0;
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
    if (make_dir("dsdneo_replay_resample", dir, sizeof(dir)) != 0
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

void
put_u16(std::vector<uint8_t>* b, unsigned v) {
    b->push_back((uint8_t)(v & 0xFFU));
    b->push_back((uint8_t)((v >> 8) & 0xFFU));
}

void
put_u32(std::vector<uint8_t>* b, uint32_t v) {
    put_u16(b, v & 0xFFFFU);
    put_u16(b, (v >> 16) & 0xFFFFU);
}

/* A 16-bit 2-channel WAV at 30 kHz, with no centre anywhere; @p out_path gets its path. */
int
write_loop_wav(char* out_path, size_t out_size) {
    char dir[DSD_TEST_PATH_MAX];
    if (make_dir("dsdneo_replay_resample_wav", dir, sizeof(dir)) != 0
        || dsd_test_path_join(out_path, out_size, dir, "loop.wav") != 0) {
        return 1;
    }
    const uint32_t rate = 30000U;
    const size_t frames = 9000U;
    std::vector<uint8_t> b;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put_u32(&b, (uint32_t)(36U + frames * 4U));
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(&b, 16U);
    put_u16(&b, 1U);
    put_u16(&b, 2U);
    put_u32(&b, rate);
    put_u32(&b, rate * 4U);
    put_u16(&b, 4U);
    put_u16(&b, 16U);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put_u32(&b, (uint32_t)(frames * 4U));
    for (size_t i = 0; i < frames; i++) {
        const double ph = 2.0 * M_PI * 2500.0 * (double)i / (double)rate;
        put_u16(&b, (unsigned)(uint16_t)(int16_t)lrint(12000.0 * std::cos(ph)));
        put_u16(&b, (unsigned)(uint16_t)(int16_t)lrint(12000.0 * std::sin(ph)));
    }
    FILE* fp = dsd_fopen_private(out_path, "wb");
    if (!fp) {
        return 1;
    }
    const int ok = fwrite(b.data(), 1, b.size(), fp) == b.size();
    return (fclose(fp) == 0 && ok) ? 0 : 1;
}

struct Run {
    BlockLog log;
    /* The demod's output the decoder read, by the capture chunk it came from (rtl_stream_get_replay_batch()). */
    std::vector<std::pair<uint64_t, std::vector<float>>> output;
    uint32_t rate_hz_before_stop;
    int demod_rate_before_stop;
    rtl_stream_test_replay_state state;
};

/* Replay @p path to its end (or, under --iq-loop, until @p loop_passes passes completed), logging every block. */
int
replay(const char* path, int tetra, int realtime, size_t read_limit, int loop_passes, Run* out) {
    std::unique_ptr<BlockLog> log(new BlockLog());
    std::unique_ptr<dsd_opts> opts(new dsd_opts());
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = realtime ? DSD_IQ_REPLAY_RATE_REALTIME : DSD_IQ_REPLAY_RATE_FAST;
    opts->iq_replay_loop = loop_passes > 0 ? 1 : 0;
    opts->frame_tetra = tetra ? 1 : 0;
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", path);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", path);

    rtl_stream_test_set_replay_block_hook(block_hook, log.get());
    rtl_stream_test_set_replay_stage_hook(level_stage, log.get());
    rtl_device_test_replay_limit_read(read_limit);
    RtlSdrContext* ctx = NULL;
    int rc = 0;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not start the replay of %s\n", path);
        rc = 1;
    } else {
        float audio[2048];
        const uint64_t start_ns = dsd_time_monotonic_ns();
        for (;;) {
            int got = 0;
            if (rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got) != 0) {
                break;
            }
            rtl_stream_replay_batch batch;
            if (got > 0 && rtl_stream_get_replay_batch(&batch) == 0) {
                /* A replay read stays inside one batch: the demod publishes the next only once this one is read. */
                if (out->output.empty() || out->output.back().first != batch.chunk_sequence) {
                    out->output.emplace_back(batch.chunk_sequence, std::vector<float>());
                }
                out->output.back().second.insert(out->output.back().second.end(), audio, audio + got);
            }
            if (loop_passes > 0) {
                rtl_stream_test_replay_state st;
                DSD_MEMSET(&st, 0, sizeof(st));
                (void)dsd_rtl_stream_test_get_replay_state(&st);
                if (st.replay_loop_restart_count >= (uint32_t)loop_passes) {
                    break;
                }
            }
            if (dsd_time_monotonic_ns() - start_ns > 60000000000ULL) {
                DSD_FPRINTF(stderr, "FAIL: timed out reading the replay of %s\n", path);
                rc = 1;
                break;
            }
        }
        DSD_MEMSET(&out->state, 0, sizeof(out->state));
        (void)dsd_rtl_stream_test_get_replay_state(&out->state);
        out->rate_hz_before_stop = out->state.dongle_rate_hz;
        out->demod_rate_before_stop = rtl_stream_get_demod_rate_hz();
        (void)rtl_stream_stop(ctx);
    }
    if (ctx) {
        (void)rtl_stream_destroy(ctx);
    }
    rtl_device_test_replay_limit_read(0U);
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);
    out->log = std::move(*log);
    return rc;
}

std::vector<float>
concat(const std::vector<Block>& blocks, size_t from, size_t to) {
    std::vector<float> out;
    for (size_t i = from; i < to; i++) {
        out.insert(out.end(), blocks[i].floats.begin(), blocks[i].floats.end());
    }
    return out;
}

/* The native blocks' data offsets (capture bytes before each block, MUTEd bytes not counted): where each segment
 * between events starts. */
std::vector<size_t>
segment_starts(const std::vector<Block>& native) {
    const uint64_t cuts[3] = {kMute1At, kRetuneAt, kMute2At};
    std::vector<size_t> starts(1, 0U);
    uint64_t offset = 0U;
    size_t cut = 0U;
    for (size_t i = 0; i < native.size(); i++) {
        if (cut < 3U && offset == cuts[cut]) {
            starts.push_back(i);
            cut++;
        }
        offset += (uint64_t)native[i].floats.size(); /* cu8: one byte per float */
    }
    starts.push_back(native.size());
    return starts;
}

int
test_resampled_events(void) {
    int rc = 0;
    const std::vector<uint8_t> payload = tone_payload(kCaptureSamples);
    char meta[DSD_TEST_PATH_MAX];
    if (write_capture(payload, meta, sizeof(meta)) != 0) {
        return 1;
    }
    Run native;
    Run fast;
    Run realtime;
    Run short_reads;
    rc |= replay(meta, 0, 0, 0U, 0, &native);
    rc |= replay(meta, 1, 0, 0U, 0, &fast);
    rc |= replay(meta, 1, 1, 37U, 0, &realtime);
    rc |= replay(meta, 1, 0, 5U, 0, &short_reads);
    if (rc != 0) {
        return rc;
    }

    rc |= expect_true("native replay runs from 48 kHz", native.rate_hz_before_stop == kRateHz);
    rc |= expect_true("native demod at 48 kHz", native.demod_rate_before_stop == (int)kRateHz);
    rc |= expect_true("TETRA replay runs from 72 kHz after the RETUNE and RESET", fast.rate_hz_before_stop == kTetraHz);
    rc |= expect_true("TETRA demod at 72 kHz", fast.demod_rate_before_stop == (int)kTetraHz);
    rc |= expect_true("TETRA replay applied both MUTEs, the RETUNE and the RESET",
                      fast.state.replay_event_mute_count == 2U && fast.state.replay_event_retune_count == 1U
                          && fast.state.replay_event_reset_count == 1U);
    rc |= expect_true("the last MUTE is counted in capture bytes",
                      fast.state.replay_event_last_mute_bytes == kMute2Bytes);

    const std::vector<Block>& nb = native.log.blocks;
    const std::vector<Block>& tb = fast.log.blocks;
    size_t native_floats = 0U;
    for (const Block& b : nb) {
        native_floats += b.floats.size();
    }
    rc |= expect_true("the native replay delivered the whole capture unresampled",
                      native_floats == (size_t)(payload.size()));
    rc |= expect_true("one TETRA block per native block", tb.size() == nb.size());
    if (tb.size() != nb.size()) {
        return 1;
    }
    for (size_t i = 0; i < nb.size(); i++) {
        if (tb[i].media_start_ns != nb[i].media_start_ns || tb[i].media_end_ns != nb[i].media_end_ns) {
            DSD_FPRINTF(stderr, "FAIL: block %zu spans %llu..%llu ns, the native one %llu..%llu ns\n", i,
                        (unsigned long long)tb[i].media_start_ns, (unsigned long long)tb[i].media_end_ns,
                        (unsigned long long)nb[i].media_start_ns, (unsigned long long)nb[i].media_end_ns);
            rc = 1;
            break;
        }
        if (tb[i].floats.size() > 2U * 8192U) {
            DSD_FPRINTF(stderr, "FAIL: block %zu holds %zu floats, past one live transfer\n", i, tb[i].floats.size());
            rc = 1;
        }
        if (!tb[i].have_level || !nb[i].have_level || tb[i].level.sample_count != nb[i].level.sample_count
            || std::fabs(tb[i].level.rms_dbfs - nb[i].level.rms_dbfs) > 1e-9
            || std::fabs(tb[i].level.peak_dbfs - nb[i].level.peak_dbfs) > 1e-9
            || tb[i].level.status != nb[i].level.status) {
            DSD_FPRINTF(stderr,
                        "FAIL: block %zu's input level (%llu samples, %.3f dBFS) is not its raw chunk's (%llu, %.3f)\n",
                        i, (unsigned long long)tb[i].level.sample_count, tb[i].level.rms_dbfs,
                        (unsigned long long)nb[i].level.sample_count, nb[i].level.rms_dbfs);
            rc = 1;
            break;
        }
    }
    /* Media spans: contiguous, but for the two MUTEs' omitted time. */
    int gaps = 0;
    for (size_t i = 1; i < tb.size(); i++) {
        if (tb[i].media_start_ns != tb[i - 1U].media_end_ns) {
            gaps++;
        }
    }
    rc |= expect_true("media spans are contiguous but for the two MUTEs", gaps == 2);

    /* Each segment between events, resampled from scratch, is what the TETRA replay delivered. */
    const std::vector<size_t> starts = segment_starts(nb);
    rc |= expect_true("the capture has four segments between its events", starts.size() == 5U);
    for (size_t s = 0; s + 1U < starts.size(); s++) {
        const std::vector<float> in = concat(nb, starts[s], starts[s + 1U]);
        const std::vector<float> got = concat(tb, starts[s], starts[s + 1U]);
        dsd_iq_resampler r;
        dsd_iq_resampler_init(&r);
        if (dsd_iq_resampler_configure(&r, (int)kRateHz, (int)kTetraHz, (int)(0.45 * (double)kTetraHz), 256U)
            != DSD_IQ_RESAMPLER_OK) {
            DSD_FPRINTF(stderr, "FAIL: reference resampler\n");
            return 1;
        }
        std::vector<float> want(2U * dsd_iq_resampler_max_out(&r, in.size() / 2U));
        const int n = dsd_iq_resampler_process(&r, in.data(), in.size() / 2U, want.data(), want.size() / 2U);
        dsd_iq_resampler_free(&r);
        want.resize(n > 0 ? 2U * (size_t)n : 0U);
        if (got.size() != want.size() || std::memcmp(got.data(), want.data(), got.size() * sizeof(float)) != 0) {
            DSD_FPRINTF(stderr, "FAIL: segment %zu: %zu resampled floats delivered, %zu expected, or they differ\n", s,
                        got.size(), want.size());
            rc = 1;
        }
    }

    /* Realtime with short reads, and fast with short reads, deliver the same blocks. */
    const Run* others[2] = {&realtime, &short_reads};
    const char* names[2] = {"realtime with 37-byte reads", "fast with 5-byte reads"};
    for (int k = 0; k < 2; k++) {
        const std::vector<Block>& ob = others[k]->log.blocks;
        int same = ob.size() == tb.size();
        for (size_t i = 0; same && i < tb.size(); i++) {
            same = ob[i].floats == tb[i].floats && ob[i].media_start_ns == tb[i].media_start_ns;
        }
        char label[128];
        DSD_SNPRINTF(label, sizeof(label), "%s delivers the fast replay's blocks", names[k]);
        rc |= expect_true(label, same);
    }
    return rc;
}

int
test_wav_loop(void) {
    char wav[DSD_TEST_PATH_MAX];
    if (write_loop_wav(wav, sizeof(wav)) != 0) {
        return 1;
    }
    Run run;
    int rc = replay(wav, 0, 0, 0U, 3, &run);
    if (rc != 0) {
        return rc;
    }
    rc |= expect_true("the centreless WAV runs from 48 kHz", run.rate_hz_before_stop == 48000U);
    std::vector<std::vector<float>> passes(3);
    for (const Block& b : run.log.blocks) {
        if (b.loop_pass < 3U) {
            passes[b.loop_pass].insert(passes[b.loop_pass].end(), b.floats.begin(), b.floats.end());
        }
    }
    /* 9000 samples at 30 kHz resample to 14400 at 48 kHz. */
    rc |= expect_true("the first pass delivered the recording resampled", passes[0].size() == 2U * 14400U);
    rc |= expect_true("the second pass delivered the first pass's blocks", passes[1] == passes[0]);
    rc |= expect_true("the third pass delivered the first pass's blocks", passes[2] == passes[0]);
    /* The rewind finalizes the demodulator afresh, centre or none: each pass demodulates to the same output. */
    std::vector<uint32_t> pass_of_sequence(run.log.blocks.size() + 2U, 99U);
    for (const Block& b : run.log.blocks) {
        if (b.sequence < pass_of_sequence.size()) {
            pass_of_sequence[b.sequence] = b.loop_pass;
        }
    }
    std::vector<std::vector<float>> outputs(3);
    for (const auto& chunk : run.output) {
        const uint32_t pass = chunk.first < pass_of_sequence.size() ? pass_of_sequence[chunk.first] : 99U;
        if (pass < 3U) {
            outputs[pass].insert(outputs[pass].end(), chunk.second.begin(), chunk.second.end());
        }
    }
    rc |= expect_true("the first pass demodulated to output", !outputs[0].empty());
    rc |= expect_true("the second pass demodulated as the first", outputs[1] == outputs[0]);
    rc |= expect_true("the third pass demodulated as the first", outputs[2] == outputs[0]);
    return rc;
}

} // namespace

int
main(void) {
    int rc = 0;
    rc |= test_resampled_events();
    rc |= test_wav_loop();
    static const char* const kFiles[] = {"capture.iq", "capture.iq.json", "loop.wav", NULL};
    for (const std::string& dir : g_dirs) {
        if (dsd_test_remove_temp_dir(dir.c_str(), kFiles) != 0) {
            DSD_FPRINTF(stderr, "FAIL: could not remove %s\n", dir.c_str());
            rc = 1;
        }
    }
    if (rc == 0) {
        printf("IO_RTL_REPLAY_RESAMPLE: OK\n");
    }
    return rc;
}
