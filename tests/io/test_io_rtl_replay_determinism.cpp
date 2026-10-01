// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Issue #572: under --iq-replay the decoder paces the demod. The demod starts a block only once the decoder waits on
 * an empty output ring with every block it published acknowledged, and publishes the block's output in one step with
 * its batch tag. A replayed event (RETUNE, MUTE, RESET, a loop rewind) is applied only once the pipeline is idle. The
 * decoder's output then depends on the capture alone: fast or realtime, read greedily or slowly, in any read size.
 * These tests pin that, the publication step, the events' boundary, and a bounded shutdown at every wait it adds. */

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "rtl_stream_test_support.h"
#include "test_support.h"

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_u64_eq(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

/* ---------------- Captures ---------------- */

/* One replay reader chunk (rtl_device.cpp): a capture reaches the demod as blocks of this many bytes, the last one
 * shorter. */
static const size_t kChunkBytes = 65536U;
/* The capture: 1.536 Msps cu8 with the fs/4 offset, decimated by 32 to the 48 kHz demod rate. */
static const uint32_t kCaptureRateHz = 1536000U;
static const uint32_t kDecimation = 32U;

static std::vector<std::string> g_fixture_dirs;
static const char* const kFixtureFiles[] = {"capture.iq", "capture.iq.json", nullptr};

static int
remove_fixture_dirs(void) {
    int rc = 0;
    for (const std::string& dir : g_fixture_dirs) {
        if (dsd_test_remove_temp_dir(dir.c_str(), kFixtureFiles) != 0) {
            DSD_FPRINTF(stderr, "FAIL: could not remove fixture directory %s: %s\n", dir.c_str(), std::strerror(errno));
            rc = 1;
        }
    }
    g_fixture_dirs.clear();
    return rc;
}

namespace {
/* An event a capture records (make_capture_with_events()). */
struct CaptureEvent {
    uint64_t offset; /* the capture bytes before it */
    dsd_iq_event_kind kind;
    uint64_t value; /* a RETUNE's or RESET's channel centre in Hz; a MUTE's omitted bytes */
};
} // namespace

/* The channel centres the captures retune between; each capture centre sits the fs/4 offset above its channel. */
static const uint64_t kFirstCenterHz = 851375000ULL;
static const uint64_t kSecondCenterHz = 851500000ULL;
static const uint64_t kFs4OffsetHz = kCaptureRateHz / 4U;

static int
record_capture_event(dsd_iq_capture_writer* writer, const CaptureEvent& event) {
    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = event.kind;
    if (event.kind == DSD_IQ_EVENT_MUTE) {
        ev.duration_bytes = event.value;
        DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "retune_mute");
    } else {
        ev.center_frequency_hz = event.value;
        ev.capture_center_frequency_hz = event.value + kFs4OffsetHz;
        ev.sample_rate_hz = kCaptureRateHz;
        DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "frequency");
    }
    return dsd_iq_capture_record_event(writer, &ev) == DSD_IQ_OK ? 0 : 1;
}

/* Write @p payload as a cu8 capture with its sidecar and @p events (sorted by offset); @p out_metadata_path receives the
 * sidecar's path. */
static int
make_capture_with_events(const std::vector<uint8_t>& payload, const std::vector<CaptureEvent>& events,
                         char* out_metadata_path, size_t out_metadata_path_size) {
    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_determinism")) {
        DSD_FPRINTF(stderr, "FAIL: could not create a fixture directory\n");
        return 1;
    }
    g_fixture_dirs.push_back(temp_dir);
    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "capture.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "capture.iq.json") != 0) {
        return 1;
    }

    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", metadata_path);
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = kCaptureRateHz;
    cfg.center_frequency_hz = kFirstCenterHz;
    cfg.capture_center_frequency_hz = kFirstCenterHz + kFs4OffsetHz;
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = kDecimation;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = kCaptureRateHz / kDecimation;
    cfg.fs4_shift_enabled = 1;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=0");

    dsd_iq_capture_writer* writer = NULL;
    char err[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open the capture writer: %s\n", err[0] ? err : "unknown");
        return 1;
    }
    size_t written = 0U;
    for (size_t i = 0; i <= events.size(); i++) {
        const size_t upto = i < events.size() ? (size_t)events[i].offset : payload.size();
        if (upto > written && dsd_iq_capture_submit(writer, payload.data() + written, upto - written) != DSD_IQ_OK) {
            dsd_iq_capture_abort(writer);
            DSD_FPRINTF(stderr, "FAIL: could not write the capture\n");
            return 1;
        }
        written = upto > written ? upto : written;
        if (i < events.size() && record_capture_event(writer, events[i]) != 0) {
            dsd_iq_capture_abort(writer);
            DSD_FPRINTF(stderr, "FAIL: could not record capture event %zu\n", i + 1U);
            return 1;
        }
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    if (DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        return 1;
    }
    return 0;
}

/* Write @p payload as a cu8 capture with its sidecar and no events. */
static int
make_capture(const std::vector<uint8_t>& payload, char* out_metadata_path, size_t out_metadata_path_size) {
    return make_capture_with_events(payload, std::vector<CaptureEvent>(), out_metadata_path, out_metadata_path_size);
}

/* @p complex_samples of a tone 5 kHz off the channel centre (the capture carries the fs/4 offset the replay rotates
 * out) with LCG noise, as cu8. */
static std::vector<uint8_t>
tone_and_noise_payload(size_t complex_samples) {
    std::vector<uint8_t> payload(complex_samples * 2U);
    uint32_t lcg = 0x2545F491U;
    const double tone_hz = (double)kCaptureRateHz / 4.0 + 5000.0;
    for (size_t i = 0; i < complex_samples; i++) {
        const double phase = 2.0 * M_PI * tone_hz * (double)i / (double)kCaptureRateHz;
        lcg = lcg * 1664525U + 1013904223U;
        const int noise_i = (int)((lcg >> 24) & 0x1FU) - 16;
        lcg = lcg * 1664525U + 1013904223U;
        const int noise_q = (int)((lcg >> 24) & 0x1FU) - 16;
        const long in_phase = lrint(127.5 + 70.0 * std::cos(phase)) + noise_i;
        const long quadrature = lrint(127.5 + 70.0 * std::sin(phase)) + noise_q;
        payload[i * 2U + 0U] = (uint8_t)(in_phase < 0 ? 0 : (in_phase > 255 ? 255 : in_phase));
        payload[i * 2U + 1U] = (uint8_t)(quadrature < 0 ? 0 : (quadrature > 255 ? 255 : quadrature));
    }
    return payload;
}

namespace {
/* A chunk the replay reader hands the demod as one block: its bytes, the capture time of its first sample in complex
 * samples, the samples a MUTE omitted included, and whether a RESET (or the start) comes before it. */
struct LayoutChunk {
    uint64_t bytes;
    uint64_t media_start;
    int epoch_start;
};
} // namespace

/* The chunks a replay of @p capture_bytes with @p events delivers, from the capture layout alone: the reader reads 64
 * KiB at a time, cut short at the next event and at the end, a MUTE moves the media timeline on by what it omitted,
 * and a RESET starts the chunk after it on a new filter epoch. */
static std::vector<LayoutChunk>
capture_layout(size_t capture_bytes, const std::vector<CaptureEvent>& events) {
    std::vector<LayoutChunk> chunks;
    uint64_t offset = 0U;
    uint64_t media = 0U;
    int epoch_start = 1;
    std::vector<CaptureEvent>::const_iterator next = events.begin();
    while (offset < capture_bytes) {
        for (; next != events.end() && next->offset <= offset; ++next) {
            if (next->kind == DSD_IQ_EVENT_MUTE) {
                media += next->value / 2U;
            } else if (next->kind == DSD_IQ_EVENT_RESET) {
                epoch_start = 1;
            }
        }
        uint64_t bytes = kChunkBytes;
        if (next != events.end() && next->offset - offset < bytes) {
            bytes = next->offset - offset;
        }
        if (capture_bytes - offset < bytes) {
            bytes = capture_bytes - offset;
        }
        chunks.push_back(LayoutChunk{bytes, media, epoch_start});
        epoch_start = 0;
        offset += bytes;
        media += bytes / 2U;
    }
    return chunks;
}

/* Half the channel FIR's span at the 48 kHz demod rate: every channel design there, whatever the profile, is the
 * 135-tap Blackman filter with the 1200 Hz transition, so c = (135 - 1) / 2. */
static const uint64_t kChannelFirHalfSpan = 67U;

/* The oracle for how many samples each chunk's block delivers, from the capture layout alone. The half-band cascade
 * decimates the block's complex samples by 32, floor(n / 32) (five nested halvings). The streaming channel FIR then
 * makes max(0, pending + N - c) outputs and holds the rest back, pending' = pending + N - outputs, so a filter epoch
 * delivers all its samples but its last c. A RESET starts an epoch: pending goes back to 0 and what the last one held
 * is dropped. The FSK discriminator makes one output per sample, with no resampling at 48 kHz. */
static std::vector<uint64_t>
oracle_chunk_outputs(const std::vector<LayoutChunk>& chunks) {
    std::vector<uint64_t> outputs;
    uint64_t pending = 0U;
    for (const LayoutChunk& chunk : chunks) {
        if (chunk.epoch_start) {
            pending = 0U;
        }
        const uint64_t held = pending + (chunk.bytes / 2U) / kDecimation;
        const uint64_t made = held > kChannelFirHalfSpan ? held - kChannelFirHalfSpan : 0U;
        pending = held - made;
        outputs.push_back(made);
    }
    return outputs;
}

/* ... and how many the whole replay delivers. Nothing is dropped: a replay delivers every sample of every block. */
static uint64_t
oracle_output_count_of(const std::vector<LayoutChunk>& chunks) {
    const std::vector<uint64_t> outputs = oracle_chunk_outputs(chunks);
    return std::accumulate(outputs.begin(), outputs.end(), (uint64_t)0U);
}

/* ... of a capture of @p capture_bytes with no events. */
static uint64_t
oracle_output_count(size_t capture_bytes) {
    return oracle_output_count_of(capture_layout(capture_bytes, std::vector<CaptureEvent>()));
}

/* Capture time of @p complex_samples in whole ns, rounded down. */
static uint64_t
media_ns(uint64_t complex_samples) {
    return (complex_samples / kCaptureRateHz) * 1000000000ULL
           + ((complex_samples % kCaptureRateHz) * 1000000000ULL) / kCaptureRateHz;
}

/* ---------------- Streams ---------------- */

/* A DMR replay: the FSK discriminator output, at 4800 symbols/s; @p loop: with --iq-loop. @p report: say why a start
 * failed. */
static int
start_replay_reporting(const char* metadata_path, int realtime, int loop, int report,
                       std::unique_ptr<dsd_opts>* out_opts, RtlSdrContext** out_ctx) {
    *out_ctx = NULL;
    out_opts->reset(new dsd_opts());
    dsd_opts* opts = out_opts->get();
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = realtime ? DSD_IQ_REPLAY_RATE_REALTIME : DSD_IQ_REPLAY_RATE_FAST;
    opts->iq_replay_loop = loop ? 1 : 0;
    opts->frame_dmr = 1;
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", metadata_path);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", metadata_path);
    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(opts, &ctx) != 0 || !ctx) {
        DSD_FPRINTF(stderr, "FAIL: rtl_stream_create failed\n");
        return 1;
    }
    if (rtl_stream_start(ctx) != 0) {
        if (report) {
            DSD_FPRINTF(stderr, "FAIL: rtl_stream_start failed\n");
        }
        rtl_stream_destroy(ctx);
        return 1;
    }
    *out_ctx = ctx;
    return 0;
}

static int
start_replay(const char* metadata_path, int realtime, std::unique_ptr<dsd_opts>* out_opts, RtlSdrContext** out_ctx) {
    return start_replay_reporting(metadata_path, realtime, 0, 1, out_opts, out_ctx);
}

/* Stop and destroy a replay; the elapsed time in ms. */
static uint64_t
stop_replay(RtlSdrContext* ctx) {
    const uint64_t start_ns = dsd_time_monotonic_ns();
    if (ctx) {
        (void)rtl_stream_stop(ctx);
        (void)rtl_stream_destroy(ctx);
    }
    return (dsd_time_monotonic_ns() - start_ns) / 1000000ULL;
}

static rtl_stream_test_replay_state
replay_state(void) {
    rtl_stream_test_replay_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    (void)dsd_rtl_stream_test_get_replay_state(&state);
    return state;
}

static int
wait_for_int(const std::atomic<int>* value, int want, unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited <= timeout_ms; waited++) {
        if (value->load(std::memory_order_acquire) >= want) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

/* Wait up to @p timeout_ms for the demod thread to leave (it reports the replay drained as it goes). */
static int
wait_for_demod_thread_exit(unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited <= timeout_ms; waited++) {
        if (replay_state().replay_demod_drained) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

/* ---------------- Determinism ---------------- */

namespace {

/* How one leg reads the replay. */
struct Leg {
    const char* name;
    int realtime;
    unsigned int start_delay_ms;
    const size_t* read_sizes; /* cycled */
    size_t read_size_count;
    int lcg_sleeps; /* sleep 0-3 ms between reads, from an LCG */
};

/* One block the demod took, as the block hook reports it. */
struct BlockRecord {
    uint64_t sequence;
    size_t float_count;
    uint64_t media_start_ns;
    uint64_t media_end_ns;
};

/* How many of each event a replay applied. */
struct EventCounts {
    uint32_t retunes;
    uint32_t mutes;
    uint32_t resets;
};

/* One batch the decoder read, as its tag tells it: the chunk, and the capture time of its first sample and of its end,
 * both from the per-sample formula the decoder runs its clock on (dsd_decode_clock_batch_media_ns()). */
struct BatchMedia {
    uint64_t chunk_sequence;
    uint64_t first_ns;
    uint64_t end_ns;
};

/* What a leg delivered, and what the pipeline did on the way. */
struct Signature {
    uint64_t samples = 0U;
    uint64_t fnv = 1469598103934665603ULL;
    std::vector<uint64_t> generation_changes; /* delivered positions where the output generation moved */
    std::vector<uint64_t> batch_starts;       /* delivered position, then media start in ns, of each batch */
    std::vector<uint64_t> media_at;           /* delivered position, then its capture time in ns, at kMediaPositions */
    std::vector<BatchMedia> batch_media;
    std::vector<BlockRecord> blocks;
    int discards = 0;        /* blocks the demod discarded */
    uint64_t truncated = 0U; /* output samples a block could not publish */
    EventCounts events = {0U, 0U, 0U};
    int ended = 0;
    int reacquire_armed = 0;
    int profile_landed = 0; /* the stream ran the requested symbol rate by the end */
};

/* The demod thread's side of a leg (the block and stage hooks). */
struct LegObserver {
    std::vector<BlockRecord> blocks;
    std::atomic<int> discards{0};
};

} // namespace

/* The leg's requests, at fixed delivered positions: an FSK reacquire, then a symbol profile change. */
static const uint64_t kReacquireAt = 6000U;
static const uint64_t kProfileAt = 13000U;

static void
fnv1a_floats(Signature* sig, const float* samples, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t bits = 0U;
        DSD_MEMCPY(&bits, &samples[i], sizeof(bits));
        for (int b = 0; b < 4; b++) {
            sig->fnv ^= (uint64_t)((bits >> (8 * b)) & 0xFFU);
            sig->fnv *= 1099511628211ULL;
        }
    }
}

static void
leg_block_hook(const rtl_stream_test_replay_block* block, void* ctx) {
    LegObserver* observer = static_cast<LegObserver*>(ctx);
    observer->blocks.push_back(
        BlockRecord{block->sequence, block->float_count, block->media_start_ns, block->media_end_ns});
}

static void
leg_stage_hook(int stage, size_t count, void* ctx) {
    (void)count;
    if (stage == RTL_STREAM_TEST_REPLAY_DEMOD_DISCARD_RELEASED) {
        static_cast<LegObserver*>(ctx)->discards.fetch_add(1, std::memory_order_acq_rel);
    }
}

/* Delivered positions at which each leg notes the capture time a decoder reaching that sample runs its clock to:
 * batch edges and insides, the requests' positions, and positions past each event group. */
static const uint64_t kMediaPositions[] = {0U,    1U,     7U,     511U,   1024U,  1535U,  2048U,  6000U,
                                           6001U, 10000U, 13000U, 15003U, 17777U, 20000U, 22222U, 23000U};

/* Note where the batch a read of @p got samples took from starts, when it is a new one, and the capture time of each
 * of kMediaPositions the read delivered. */
static void
note_batch_start(Signature* sig, uint64_t* batch_sequence, int got) {
    rtl_stream_replay_batch tag;
    if (rtl_stream_get_replay_batch(&tag) != 0) {
        return;
    }
    for (uint64_t position : kMediaPositions) {
        if (position >= sig->samples && position < sig->samples + (uint64_t)got) {
            sig->media_at.push_back(position);
            sig->media_at.push_back(
                dsd_decode_clock_batch_media_ns(tag.media_start_ns, tag.media_duration_ns, tag.output_count,
                                                tag.first_index + (uint32_t)(position - sig->samples)));
        }
    }
    if (tag.chunk_sequence == *batch_sequence) {
        return;
    }
    *batch_sequence = tag.chunk_sequence;
    sig->batch_starts.push_back(sig->samples);
    sig->batch_starts.push_back(tag.media_start_ns);
    sig->batch_media.push_back(
        BatchMedia{tag.chunk_sequence,
                   dsd_decode_clock_batch_media_ns(tag.media_start_ns, tag.media_duration_ns, tag.output_count, 0U),
                   dsd_decode_clock_batch_media_ns(tag.media_start_ns, tag.media_duration_ns, tag.output_count,
                                                   tag.output_count)});
}

static int
read_leg(RtlSdrContext* ctx, const Leg& leg, Signature* sig) {
    std::vector<float> buf(512U);
    uint32_t lcg = 0x9E3779B9U;
    uint32_t generation = 0U;
    uint64_t batch_sequence = 0U;
    int have_generation = 0;
    int reacquired = 0;
    int profiled = 0;
    const uint64_t deadline_ns = dsd_time_monotonic_ns() + 20000ULL * 1000000ULL;
    for (size_t read_index = 0U;; read_index++) {
        if (dsd_time_monotonic_ns() > deadline_ns) {
            DSD_FPRINTF(stderr, "FAIL: determinism leg %s: the replay did not end within 20 s\n", leg.name);
            return 1;
        }
        const size_t want = leg.read_sizes[read_index % leg.read_size_count];
        int got = 0;
        if (rtl_stream_read(ctx, buf.data(), want, &got) != 0) {
            sig->ended = 1;
            return 0;
        }
        if (got <= 0) {
            continue;
        }
        const uint32_t now_generation = rtl_stream_output_generation();
        if (have_generation && now_generation != generation) {
            sig->generation_changes.push_back(sig->samples);
        }
        generation = now_generation;
        have_generation = 1;
        note_batch_start(sig, &batch_sequence, got);
        fnv1a_floats(sig, buf.data(), got);
        sig->samples += (uint64_t)got;
        if (!reacquired && sig->samples >= kReacquireAt) {
            sig->reacquire_armed = rtl_stream_request_fsk_reacquire();
            reacquired = 1;
        }
        if (!profiled && sig->samples >= kProfileAt) {
            /* NXDN48's profile: 2400 symbols/s through the 6.25 kHz channel filter. */
            (void)rtl_stream_request_demod_profile(-1, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, -1, 0);
            profiled = 1;
        }
        if (leg.lcg_sleeps) {
            lcg = lcg * 1664525U + 1013904223U;
            dsd_sleep_ms((lcg >> 30) & 0x3U);
        }
    }
}

static int
run_leg(const char* metadata_path, const Leg& leg, Signature* sig) {
    LegObserver observer;
    rtl_stream_test_set_replay_block_hook(leg_block_hook, &observer);
    rtl_stream_test_set_replay_stage_hook(leg_stage_hook, &observer);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    int rc = 1;
    if (start_replay(metadata_path, leg.realtime, &opts, &ctx) == 0) {
        if (leg.start_delay_ms > 0U) {
            dsd_sleep_ms(leg.start_delay_ms);
        }
        rc = read_leg(ctx, leg, sig);
        int symbol_rate_hz = 0;
        (void)rtl_stream_get_symbol_profile_full(&symbol_rate_hz, NULL, NULL);
        sig->profile_landed = symbol_rate_hz == 2400 ? 1 : 0;
        const rtl_stream_test_replay_state state = replay_state();
        sig->truncated = state.replay_output_truncated;
        sig->events =
            EventCounts{state.replay_event_retune_count, state.replay_event_mute_count, state.replay_event_reset_count};
        (void)stop_replay(ctx);
    }
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);
    sig->blocks = observer.blocks;
    sig->discards = observer.discards.load();
    return rc;
}

static void
print_signature(const char* name, const Signature& sig) {
    DSD_FPRINTF(stderr,
                "  leg %-8s samples=%llu fnv=%016llx blocks=%zu discards=%d truncated=%llu generation changes at", name,
                (unsigned long long)sig.samples, (unsigned long long)sig.fnv, sig.blocks.size(), sig.discards,
                (unsigned long long)sig.truncated);
    for (uint64_t position : sig.generation_changes) {
        DSD_FPRINTF(stderr, " %llu", (unsigned long long)position);
    }
    DSD_FPRINTF(stderr, "\n");
}

static int
blocks_equal(const BlockRecord& a, const BlockRecord& b) {
    return a.sequence == b.sequence && a.float_count == b.float_count && a.media_start_ns == b.media_start_ns
           && a.media_end_ns == b.media_end_ns;
}

/* The blocks a leg's demod took are the capture's chunks, in order, each whole (a cu8 byte is one float), with its
 * media span. */
static int
expect_blocks_follow_layout(const char* label, const Signature& sig, const std::vector<LayoutChunk>& layout) {
    if (sig.blocks.size() != layout.size()) {
        DSD_FPRINTF(stderr, "FAIL: %s: the demod took %zu blocks, the capture has %zu chunks\n", label,
                    sig.blocks.size(), layout.size());
        return 1;
    }
    for (size_t i = 0; i < layout.size(); i++) {
        const BlockRecord want = {i + 1U, (size_t)layout[i].bytes, media_ns(layout[i].media_start),
                                  media_ns(layout[i].media_start + layout[i].bytes / 2U)};
        if (!blocks_equal(sig.blocks[i], want)) {
            DSD_FPRINTF(stderr,
                        "FAIL: %s: block %zu is chunk %llu of %zu floats, media %llu-%llu ns; want chunk %llu of %zu "
                        "floats, media %llu-%llu ns\n",
                        label, i + 1U, (unsigned long long)sig.blocks[i].sequence, sig.blocks[i].float_count,
                        (unsigned long long)sig.blocks[i].media_start_ns,
                        (unsigned long long)sig.blocks[i].media_end_ns, (unsigned long long)want.sequence,
                        want.float_count, (unsigned long long)want.media_start_ns,
                        (unsigned long long)want.media_end_ns);
            return 1;
        }
    }
    return 0;
}

/* Capture time a decoder runs its clock to at delivered position @p position, from the capture layout alone: the sample
 * sits at its place among its chunk's outputs, evenly across the chunk's capture time (the media time a MUTE omitted
 * included). UINT64_MAX past the last output. */
static uint64_t
oracle_media_at(const std::vector<LayoutChunk>& layout, uint64_t position) {
    const std::vector<uint64_t> outputs = oracle_chunk_outputs(layout);
    uint64_t first = 0U;
    for (size_t i = 0; i < layout.size(); i++) {
        const LayoutChunk& chunk = layout[i];
        const uint64_t count = outputs[i];
        if (position < first + count) {
            const uint64_t start = media_ns(chunk.media_start);
            const uint64_t span = media_ns(chunk.media_start + chunk.bytes / 2U) - start;
            /* (position - first) < count <= 2048 and span < 50 ms: the product stays far inside 64 bits. */
            return start + ((position - first) * span) / count;
        }
        first += count;
    }
    return UINT64_MAX;
}

/* Capture time a MUTE omits at capture byte @p offset: whole ms in the captures here, so exact in ns. */
static uint64_t
muted_ns_at(const std::vector<CaptureEvent>& events, uint64_t offset) {
    uint64_t omitted = 0U;
    for (const CaptureEvent& event : events) {
        if (event.kind == DSD_IQ_EVENT_MUTE && event.offset == offset) {
            omitted += event.value / 2U;
        }
    }
    return media_ns(omitted);
}

/* The leg's media clock: at each of kMediaPositions it reads the layout's capture time, and from one batch to the next
 * it moves by exactly what a MUTE between them omitted, nothing where none did. */
static int
expect_media_follows_layout(const char* label, const Signature& sig, const std::vector<LayoutChunk>& layout,
                            const std::vector<CaptureEvent>& events) {
    int rc = 0;
    char what[200];
    for (size_t i = 0; i + 1U < sig.media_at.size(); i += 2U) {
        DSD_SNPRINTF(what, sizeof(what), "%s: media time at delivered sample %llu", label,
                     (unsigned long long)sig.media_at[i]);
        rc |= expect_u64_eq(what, sig.media_at[i + 1U], oracle_media_at(layout, sig.media_at[i]));
    }
    DSD_SNPRINTF(what, sizeof(what), "%s: media times noted", label);
    rc |= expect_u64_eq(what, (uint64_t)(sig.media_at.size() / 2U),
                        (uint64_t)(sizeof(kMediaPositions) / sizeof(kMediaPositions[0])));
    uint64_t offset = 0U;
    size_t next_chunk = 0U;
    for (size_t i = 0; i < sig.batch_media.size(); i++) {
        const BatchMedia& batch = sig.batch_media[i];
        /* The capture byte offset its chunk starts at. */
        for (; next_chunk + 1U < batch.chunk_sequence && next_chunk < layout.size(); next_chunk++) {
            offset += layout[next_chunk].bytes;
        }
        const uint64_t before = i == 0U ? 0U : sig.batch_media[i - 1U].end_ns;
        DSD_SNPRINTF(what, sizeof(what),
                     "%s: media time a batch moves on from the one before (chunk %llu, at byte %llu)", label,
                     (unsigned long long)batch.chunk_sequence, (unsigned long long)offset);
        if (batch.first_ns < before) {
            DSD_FPRINTF(stderr, "FAIL: %s went backwards\n", what);
            rc = 1;
            continue;
        }
        rc |= expect_u64_eq(what, batch.first_ns - before, muted_ns_at(events, offset));
    }
    return rc;
}

/* One leg on its own: it ended, its requests landed, it applied every event, and it delivered every sample of every
 * chunk, with no block discarded and no output cut short. */
static int
expect_leg_complete(const char* capture, const Leg& leg, const Signature& sig, const std::vector<LayoutChunk>& layout,
                    const EventCounts& want_events) {
    int rc = 0;
    char label[160];
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: the replay ended", capture, leg.name);
    rc |= expect_true(label, sig.ended);
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: the FSK reacquire was armed", capture, leg.name);
    rc |= expect_true(label, sig.reacquire_armed);
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: the reacquire moved the output generation", capture, leg.name);
    rc |= expect_true(label, !sig.generation_changes.empty());
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: the symbol profile change landed", capture, leg.name);
    rc |= expect_true(label, sig.profile_landed);
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: samples delivered (the oracle's count, none dropped)", capture,
                 leg.name);
    rc |= expect_u64_eq(label, sig.samples, oracle_output_count_of(layout));
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: blocks the demod discarded", capture, leg.name);
    rc |= expect_u64_eq(label, (uint64_t)sig.discards, 0U);
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: output samples a block could not publish", capture, leg.name);
    rc |= expect_u64_eq(label, sig.truncated, 0U);
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: RETUNEs applied", capture, leg.name);
    rc |= expect_u64_eq(label, sig.events.retunes, want_events.retunes);
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: MUTEs applied", capture, leg.name);
    rc |= expect_u64_eq(label, sig.events.mutes, want_events.mutes);
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s: RESETs applied", capture, leg.name);
    rc |= expect_u64_eq(label, sig.events.resets, want_events.resets);
    DSD_SNPRINTF(label, sizeof(label), "%s leg %s", capture, leg.name);
    rc |= expect_blocks_follow_layout(label, sig, layout);
    return rc;
}

static int
signatures_equal(const Signature& a, const Signature& b) {
    if (a.samples != b.samples || a.fnv != b.fnv || a.generation_changes != b.generation_changes
        || a.batch_starts != b.batch_starts || a.media_at != b.media_at || a.blocks.size() != b.blocks.size()) {
        return 0;
    }
    for (size_t i = 0; i < a.blocks.size(); i++) {
        if (!blocks_equal(a.blocks[i], b.blocks[i])) {
            return 0;
        }
    }
    if (a.batch_media.size() != b.batch_media.size()) {
        return 0;
    }
    for (size_t i = 0; i < a.batch_media.size(); i++) {
        if (a.batch_media[i].chunk_sequence != b.batch_media[i].chunk_sequence
            || a.batch_media[i].first_ns != b.batch_media[i].first_ns
            || a.batch_media[i].end_ns != b.batch_media[i].end_ns) {
            return 0;
        }
    }
    return 1;
}

/* Replay @p metadata_path (the chunks @p layout, the events @p events, @p want_events of them applied) with a greedy
 * fast reader, a slow reader that starts late and reads 1, 7 and 512 samples at a time with sleeps between, and a greedy
 * realtime reader, each asking for an FSK reacquire and a symbol profile change at the same delivered positions. Each
 * must get every sample of every chunk, bit for bit the same, with the output generation moving and every batch
 * starting at the same positions, and the demod must take the same blocks. The media time a decoder runs its clock on
 * must be the same at the same delivered positions, the layout's, and move by exactly what a MUTE omitted at each
 * event boundary. */
static int
expect_legs_agree(const char* capture, const char* metadata_path, const std::vector<LayoutChunk>& layout,
                  const std::vector<CaptureEvent>& events, const EventCounts& want_events) {
    static const size_t kGreedy[] = {512U};
    static const size_t kSlow[] = {1U, 7U, 512U};
    const Leg legs[] = {
        {"fast", 0, 0U, kGreedy, 1U, 0},
        {"slow", 0, 80U, kSlow, 3U, 1},
        {"realtime", 1, 0U, kGreedy, 1U, 0},
    };
    const size_t leg_count = sizeof(legs) / sizeof(legs[0]);
    std::vector<Signature> sigs(leg_count);
    int rc = 0;
    for (size_t i = 0; i < leg_count; i++) {
        rc |= run_leg(metadata_path, legs[i], &sigs[i]);
    }
    int differ = 0;
    for (size_t i = 0; i < leg_count; i++) {
        rc |= expect_leg_complete(capture, legs[i], sigs[i], layout, want_events);
        char label[96];
        DSD_SNPRINTF(label, sizeof(label), "%s leg %s", capture, legs[i].name);
        rc |= expect_media_follows_layout(label, sigs[i], layout, events);
        if (i > 0U && !signatures_equal(sigs[i], sigs[0])) {
            DSD_FPRINTF(stderr, "FAIL: %s: leg %s delivered a different stream from leg %s\n", capture, legs[i].name,
                        legs[0].name);
            differ = 1;
        }
    }
    if (differ || rc != 0) {
        for (size_t i = 0; i < leg_count; i++) {
            print_signature(legs[i].name, sigs[i]);
        }
        rc = 1;
    }
    return rc;
}

/* Issue #572: an event-free replay decodes the same whoever reads it (expect_legs_agree()). A front end that runs ahead
 * of its decoder lands the requests on whatever block it reached, and a reacquire clears output the decoder has not
 * read. */
static int
test_replay_output_does_not_depend_on_the_reader(void) {
    /* About 0.5 s: 23 whole chunks and a short last one. */
    const size_t complex_samples = 768000U;
    const std::vector<uint8_t> payload = tone_and_noise_payload(complex_samples);
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_capture(payload, metadata_path, sizeof(metadata_path)) != 0) {
        return 1;
    }
    const std::vector<CaptureEvent> no_events;
    return expect_legs_agree("determinism", metadata_path, capture_layout(payload.size(), no_events), no_events,
                             EventCounts{0U, 0U, 0U});
}

/* The eventful capture: a MUTE at offset 0; a RETUNE, MUTE and RESET together, a lone MUTE, and a RETUNE, MUTE, RESET
 * and MUTE together, none on a chunk boundary (the first cuts a chunk to an odd count of complex samples); and a MUTE
 * at the end. Every MUTE omits a whole number of ms, so media time moves by exactly that. */
static const CaptureEvent kEventfulEvents[] = {
    {0U, DSD_IQ_EVENT_MUTE, 6144U},       {200002U, DSD_IQ_EVENT_RETUNE, kSecondCenterHz},
    {200002U, DSD_IQ_EVENT_MUTE, 3072U},  {200002U, DSD_IQ_EVENT_RESET, kSecondCenterHz},
    {700000U, DSD_IQ_EVENT_MUTE, 9216U},  {1000006U, DSD_IQ_EVENT_RETUNE, kFirstCenterHz},
    {1000006U, DSD_IQ_EVENT_MUTE, 3072U}, {1000006U, DSD_IQ_EVENT_RESET, kFirstCenterHz},
    {1000006U, DSD_IQ_EVENT_MUTE, 6144U}, {1536000U, DSD_IQ_EVENT_MUTE, 12288U},
};

/* Issue #572: a replay's RETUNE, MUTE and RESET events land on the same samples whoever reads it. Each waits for an
 * idle pipeline, so the demod applies it between the same two chunks, and a RESET drops nothing a slow decoder had not
 * read yet. */
static int
test_replay_events_do_not_depend_on_the_reader(void) {
    const size_t complex_samples = 768000U;
    const std::vector<uint8_t> payload = tone_and_noise_payload(complex_samples);
    const std::vector<CaptureEvent> events(std::begin(kEventfulEvents), std::end(kEventfulEvents));
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_capture_with_events(payload, events, metadata_path, sizeof(metadata_path)) != 0) {
        return 1;
    }
    return expect_legs_agree("eventful", metadata_path, capture_layout(payload.size(), events), events,
                             EventCounts{2U, 6U, 2U});
}

/* ---------------- Publication ---------------- */

namespace {
/* test_replay_publishes_a_block_whole_with_its_tag(). */
struct PublishWindow {
    std::atomic<uint64_t> delivered{0U};        /* the decoder's samples so far */
    std::atomic<uint64_t> published_before{0U}; /* samples in the blocks before the one the demod holds */
    std::atomic<int> holds{0};
    std::atomic<int> violations{0};
    std::atomic<uint64_t> violation_block{0U};
};
} // namespace

static void
publish_window_stage(int stage, size_t count, void* ctx) {
    PublishWindow* window = static_cast<PublishWindow*>(ctx);
    if (stage != RTL_STREAM_TEST_REPLAY_DEMOD_BEFORE_PUBLISH) {
        return;
    }
    /* The block's output is ready and not yet published: for 100 ms, the decoder must not get any of it. */
    const int hold = window->holds.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint64_t before = window->published_before.load(std::memory_order_acquire);
    for (unsigned int waited = 0U; waited < 100U; waited++) {
        if (window->delivered.load(std::memory_order_acquire) > before) {
            window->violation_block.store((uint64_t)hold, std::memory_order_relaxed);
            window->violations.fetch_add(1, std::memory_order_acq_rel);
            break;
        }
        dsd_sleep_ms(1U);
    }
    window->published_before.fetch_add((uint64_t)count, std::memory_order_acq_rel);
}

namespace {
/* The decoder side of test_replay_publishes_a_block_whole_with_its_tag(). */
struct TaggedReader {
    RtlSdrContext* ctx = nullptr;
    PublishWindow* window = nullptr;
    std::atomic<int> done{0};
    int tag_failures = 0;
    char first_failure[256] = {0};
    uint64_t batches = 0U;
    uint64_t delivered = 0U;
    uint64_t batch_sequence = 0U; /* the batch being read: its chunk, what the reads took of it, and its size */
    uint64_t batch_taken = 0U;
    uint64_t batch_count = 0U;
};
} // namespace

static void
tagged_reader_fail(TaggedReader* reader, const char* what) {
    if (reader->tag_failures++ == 0) {
        DSD_SNPRINTF(reader->first_failure, sizeof(reader->first_failure), "%s at delivered sample %llu", what,
                     (unsigned long long)reader->delivered);
    }
}

/* Whether @p tag describes a batch of the four-chunk capture as the block made it: its capture time (whole ns, rounded
 * down, from the capture's first sample), its size (the oracle's count for its chunk), and the DMR output it ran on. */
static int
tag_matches_its_chunk(const rtl_stream_replay_batch& tag) {
    const uint64_t chunk_complex = kChunkBytes / 2U;
    const std::vector<uint64_t> outputs =
        oracle_chunk_outputs(capture_layout(4U * kChunkBytes, std::vector<CaptureEvent>()));
    if (tag.chunk_sequence < 1U || tag.chunk_sequence > outputs.size()) {
        return 0;
    }
    const uint64_t start_ns = ((tag.chunk_sequence - 1U) * chunk_complex * 1000000000ULL) / kCaptureRateHz;
    const uint64_t end_ns = (tag.chunk_sequence * chunk_complex * 1000000000ULL) / kCaptureRateHz;
    return tag.output_count == outputs[tag.chunk_sequence - 1U] && tag.media_start_ns == start_ns
           && tag.media_duration_ns == end_ns - start_ns && tag.output_kind == RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR
           && tag.symbol_rate_hz == 4800 && tag.output_rate_hz == (int)(kCaptureRateHz / kDecimation)
           && tag.output_generation == rtl_stream_output_generation();
}

/* Check the tag after a read of @p got samples: a new batch is the next chunk's, read from its start, and every read
 * takes the samples that follow the last one's in the batch. */
static void
check_read_tag(TaggedReader* reader, int got) {
    rtl_stream_replay_batch tag;
    if (rtl_stream_get_replay_batch(&tag) != 0) {
        tagged_reader_fail(reader, "no batch tag");
        return;
    }
    if (reader->batch_taken >= reader->batch_count || tag.chunk_sequence != reader->batch_sequence) {
        if (tag.chunk_sequence != reader->batch_sequence + 1U || tag.first_index != 0U) {
            tagged_reader_fail(reader, "a batch out of chunk order, or not read from its start");
        }
        reader->batch_sequence = tag.chunk_sequence;
        reader->batch_taken = 0U;
        reader->batch_count = tag.output_count;
        reader->batches++;
    }
    if (tag.first_index != reader->batch_taken || reader->batch_taken + (uint64_t)got > reader->batch_count
        || !tag_matches_its_chunk(tag)) {
        tagged_reader_fail(reader, "a tag that does not describe the samples read");
    }
    reader->batch_taken += (uint64_t)got;
}

/* Read the replay 1, 7 and 512 samples at a time, checking each read's tag. */
static DSD_THREAD_RETURN_TYPE
tagged_reader_fn(void* arg) {
    TaggedReader* reader = static_cast<TaggedReader*>(arg);
    static const size_t kSizes[] = {1U, 7U, 512U};
    float buf[512];
    for (size_t i = 0U;; i++) {
        int got = 0;
        if (rtl_stream_read(reader->ctx, buf, kSizes[i % 3U], &got) != 0) {
            break;
        }
        if (got <= 0) {
            continue;
        }
        check_read_tag(reader, got);
        reader->delivered += (uint64_t)got;
        reader->window->delivered.store(reader->delivered, std::memory_order_release);
    }
    reader->done.store(1, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/* Issue #572: the demod publishes a block's output in one step with its batch tag and the count of blocks published,
 * under the lock the decoder's empty check and acknowledgement take. Output the decoder could take before the count
 * moved would be read under the previous batch's tag, and the decoder's acknowledgement of it would be one block
 * short. The demod is held with each block's output ready and not yet counted: the decoder must get none of it until
 * the demod publishes, and every read's tag must describe the samples it returned. */
static int
test_replay_publishes_a_block_whole_with_its_tag(void) {
    const size_t chunks = 4U;
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_capture(tone_and_noise_payload(chunks * kChunkBytes / 2U), metadata_path, sizeof(metadata_path)) != 0) {
        return 1;
    }
    PublishWindow window;
    rtl_stream_test_set_replay_stage_hook(publish_window_stage, &window);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay(metadata_path, 0, &opts, &ctx) != 0) {
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        return 1;
    }
    TaggedReader reader;
    reader.ctx = ctx;
    reader.window = &window;
    dsd_thread_t thread;
    int rc = 0;
    if (dsd_thread_create(&thread, tagged_reader_fn, &reader) != 0) {
        rc = expect_true("publication: the decoder thread started", 0);
    } else {
        if (!wait_for_int(&reader.done, 1, 10000U)) {
            rc |= expect_true("publication: the replay reached its end within 10 s", 0);
            dsd_exitflag_store(1);
        }
        (void)dsd_thread_join(thread);
    }
    (void)stop_replay(ctx);
    dsd_exitflag_store(0);
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);

    if (window.violations.load() != 0) {
        DSD_FPRINTF(stderr,
                    "FAIL: publication: the decoder read output of %d blocks before the demod published them (first: "
                    "block %llu)\n",
                    window.violations.load(), (unsigned long long)window.violation_block.load());
        rc = 1;
    }
    rc |= expect_u64_eq("publication: blocks held before publication", (uint64_t)window.holds.load(), chunks);
    rc |= expect_u64_eq("publication: batches read", reader.batches, chunks);
    rc |= expect_u64_eq("publication: samples delivered", window.delivered.load(),
                        oracle_output_count(chunks * kChunkBytes));
    if (reader.tag_failures != 0) {
        DSD_FPRINTF(stderr, "FAIL: publication: %d reads had a wrong batch tag (first: %s)\n", reader.tag_failures,
                    reader.first_failure);
        rc = 1;
    }
    return rc;
}

/* ---------------- Bounded shutdown ---------------- */

namespace {
/* The demod's pacing, as its stages report it. */
struct Pacing {
    std::atomic<int> blocks{0};          /* blocks the demod took */
    std::atomic<int> demand_waits{0};    /* times it waited for the decoder's demand */
    std::atomic<uint64_t> published{0U}; /* blocks published before the last demand wait, the virtual block 0 in */
    std::atomic<int> hold_block{0};      /* hold this block (from 1) at reserve until released; 0: none */
    std::atomic<int> holding{0};
    std::atomic<int> release{0};
    std::atomic<int> reader_starting{0};  /* the opening thread got to the reader's start */
    std::atomic<int> start_saw_demand{0}; /* ... with the demod already waiting for demand */
};
} // namespace

static void
pacing_stage(int stage, size_t count, void* ctx) {
    Pacing* pacing = static_cast<Pacing*>(ctx);
    switch (stage) {
        case RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_RESERVE: {
            const int block = pacing->blocks.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (block == pacing->hold_block.load(std::memory_order_acquire)) {
                pacing->holding.store(1, std::memory_order_release);
                (void)wait_for_int(&pacing->release, 1, 3000U);
            }
            break;
        }
        case RTL_STREAM_TEST_REPLAY_DEMOD_WAIT_FOR_DEMAND:
            pacing->published.store((uint64_t)count, std::memory_order_release);
            pacing->demand_waits.fetch_add(1, std::memory_order_acq_rel);
            break;
        case RTL_STREAM_TEST_REPLAY_READER_START:
            /* A start that fails once the demod waits for the decoder's first read. */
            pacing->reader_starting.store(1, std::memory_order_release);
            pacing->start_saw_demand.store(wait_for_int(&pacing->demand_waits, 1, 2000U), std::memory_order_release);
            break;
        default: break;
    }
}

/* Wait up to @p timeout_ms for the demod to wait for demand with @p published blocks published. */
static int
wait_for_demand_wait(const Pacing* pacing, uint64_t published, unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited <= timeout_ms; waited++) {
        if (pacing->demand_waits.load(std::memory_order_acquire) > 0
            && pacing->published.load(std::memory_order_acquire) == published) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

/* Start a four-chunk replay with the pacing hook, and read @p reads single samples. */
static int
start_paced_replay(const char* label, Pacing* pacing, int reads, std::unique_ptr<dsd_opts>* opts, RtlSdrContext** ctx) {
    static std::string metadata;
    if (metadata.empty()) {
        char path[DSD_TEST_PATH_MAX];
        if (make_capture(tone_and_noise_payload(4U * kChunkBytes / 2U), path, sizeof(path)) != 0) {
            return 1;
        }
        metadata = path;
    }
    rtl_stream_test_set_replay_stage_hook(pacing_stage, pacing);
    if (start_replay(metadata.c_str(), 0, opts, ctx) != 0) {
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        return 1;
    }
    for (int i = 0; i < reads; i++) {
        float sample = 0.0f;
        int got = 0;
        if (rtl_stream_read(*ctx, &sample, 1U, &got) != 0 || got != 1) {
            DSD_FPRINTF(stderr, "FAIL: %s: read %d of the replay failed\n", label, i + 1);
            return 1;
        }
    }
    return 0;
}

/* Issue #572: the demod waits for the decoder's demand before it starts a block, and each wait ends in bounded time
 * on a stop. Before the decoder's first read (the virtual block 0) it takes no block; once the decoder has read part
 * of a block it takes no other until the decoder acknowledges that one. At both waits, a soft stop returns within 2 s
 * and the global exit flag ends the demod thread within 1 s, without a read. */
static int
test_replay_demand_waits_stop_in_bounded_time(void) {
    int rc = 0;
    for (int reads = 0; reads <= 1; reads++) {
        for (int global_exit = 0; global_exit <= 1; global_exit++) {
            char label[160];
            DSD_SNPRINTF(label, sizeof(label), "%s, %s",
                         reads ? "awaiting an acknowledgement" : "before the first read",
                         global_exit ? "global exit" : "soft stop");
            Pacing pacing;
            std::unique_ptr<dsd_opts> opts;
            RtlSdrContext* ctx = NULL;
            if (start_paced_replay(label, &pacing, reads, &opts, &ctx) != 0) {
                (void)stop_replay(ctx);
                rtl_stream_test_set_replay_stage_hook(NULL, NULL);
                return 1;
            }
            /* Published so far: the virtual block 0, and the block the read took part of. */
            const uint64_t published = 1U + (uint64_t)reads;
            int waiting = wait_for_demand_wait(&pacing, published, 2000U);
            dsd_sleep_ms(100U);
            char what[256];
            DSD_SNPRINTF(what, sizeof(what), "%s: the demod waits for the decoder's demand", label);
            rc |= expect_true(what, waiting);
            DSD_SNPRINTF(what, sizeof(what), "%s: blocks the demod took", label);
            rc |= expect_u64_eq(what, (uint64_t)pacing.blocks.load(), (uint64_t)reads);
            const rtl_stream_test_replay_state state = replay_state();
            DSD_SNPRINTF(what, sizeof(what), "%s: batches the decoder acknowledged (the virtual block 0 once it read)",
                         label);
            rc |= expect_u64_eq(what, state.replay_out_acked, (uint64_t)reads);
            if (global_exit) {
                dsd_exitflag_store(1);
                DSD_SNPRINTF(what, sizeof(what), "%s: the demod thread left within 1 s", label);
                rc |= expect_true(what, wait_for_demod_thread_exit(1000U));
            }
            const uint64_t stop_ms = stop_replay(ctx);
            dsd_exitflag_store(0);
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            if (stop_ms > 2000U) {
                DSD_FPRINTF(stderr, "FAIL: %s: the stop took %llu ms (want <= 2000)\n", label,
                            (unsigned long long)stop_ms);
                rc = 1;
            }
        }
    }
    return rc;
}

namespace {
struct BlockedRead {
    RtlSdrContext* ctx = nullptr;
    std::atomic<uint64_t> delivered{0U};
    std::atomic<int> rc{0};
    std::atomic<int> done{0};
};
} // namespace

/* Read until a read fails. */
static DSD_THREAD_RETURN_TYPE
blocked_read_fn(void* arg) {
    BlockedRead* reader = static_cast<BlockedRead*>(arg);
    float buf[512];
    for (;;) {
        int got = 0;
        const int read_rc = rtl_stream_read(reader->ctx, buf, 512U, &got);
        if (read_rc != 0) {
            reader->rc.store(read_rc, std::memory_order_release);
            break;
        }
        reader->delivered.fetch_add((uint64_t)(got > 0 ? got : 0), std::memory_order_acq_rel);
    }
    reader->done.store(1, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/* Issue #572: a decoder waiting for output while the demod works on a block, and the demod's wait for demand after
 * it, both end within 1 s of a forced stop (the replay device's stop) or the global exit flag. The demod holds its
 * second block until the decoder's read has been waiting for 100 ms. */
static int
test_replay_decoder_wait_stops_in_bounded_time(void) {
    int rc = 0;
    for (int global_exit = 0; global_exit <= 1; global_exit++) {
        const char* label = global_exit ? "decoder wait, global exit" : "decoder wait, forced stop";
        Pacing pacing;
        pacing.hold_block.store(2, std::memory_order_release);
        std::unique_ptr<dsd_opts> opts;
        RtlSdrContext* ctx = NULL;
        if (start_paced_replay(label, &pacing, 0, &opts, &ctx) != 0) {
            (void)stop_replay(ctx);
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            return 1;
        }
        BlockedRead reader;
        reader.ctx = ctx;
        dsd_thread_t thread;
        if (dsd_thread_create(&thread, blocked_read_fn, &reader) != 0) {
            pacing.release.store(1, std::memory_order_release);
            (void)stop_replay(ctx);
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            return expect_true("the decoder thread started", 0);
        }
        char what[256];
        DSD_SNPRINTF(what, sizeof(what), "%s: the demod holds its second block", label);
        rc |= expect_true(what, wait_for_int(&pacing.holding, 1, 2000U));
        dsd_sleep_ms(100U);
        DSD_SNPRINTF(what, sizeof(what), "%s: the decoder read the first block and waits", label);
        rc |= expect_true(what, reader.delivered.load() == oracle_output_count(kChunkBytes) && !reader.done.load());
        if (global_exit) {
            dsd_exitflag_store(1);
        } else {
            rtl_stream_test_replay_force_stop();
        }
        DSD_SNPRINTF(what, sizeof(what), "%s: the decoder's read ended within 1 s", label);
        rc |= expect_true(what, wait_for_int(&reader.done, 1, 1000U));
        pacing.release.store(1, std::memory_order_release);
        DSD_SNPRINTF(what, sizeof(what), "%s: the demod thread left within 1 s", label);
        rc |= expect_true(what, wait_for_demod_thread_exit(1000U));
        if (!reader.done.load()) {
            dsd_exitflag_store(1);
        }
        (void)dsd_thread_join(thread);
        const uint64_t stop_ms = stop_replay(ctx);
        dsd_exitflag_store(0);
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        if (stop_ms > 2000U) {
            DSD_FPRINTF(stderr, "FAIL: %s: the stop took %llu ms (want <= 2000)\n", label, (unsigned long long)stop_ms);
            rc = 1;
        }
    }
    return rc;
}

/* Issue #572: a replay with no output ends. A capture shorter than one output sample ends the decoder's first read
 * within 2 s: the demod's only block publishes nothing, so no batch waits for an acknowledgement. An empty capture is
 * refused at the start, which returns within 2 s. */
static int
test_replay_without_output_ends(void) {
    int rc = 0;
    for (int empty = 0; empty <= 1; empty++) {
        const char* label = empty ? "empty replay" : "zero-output replay";
        char metadata_path[DSD_TEST_PATH_MAX];
        const size_t complex_samples = empty ? 0U : kDecimation / 2U;
        if (make_capture(tone_and_noise_payload(complex_samples), metadata_path, sizeof(metadata_path)) != 0) {
            return 1;
        }
        Pacing pacing;
        rtl_stream_test_set_replay_stage_hook(pacing_stage, &pacing);
        std::unique_ptr<dsd_opts> opts;
        RtlSdrContext* ctx = NULL;
        const uint64_t start_ns = dsd_time_monotonic_ns();
        const int start_rc = start_replay_reporting(metadata_path, 0, 0, !empty, &opts, &ctx);
        const uint64_t start_ms = (dsd_time_monotonic_ns() - start_ns) / 1000000ULL;
        if (empty) {
            rc |= expect_true("empty replay: the start is refused", start_rc != 0);
            rc |= expect_true("empty replay: the refused start returned within 2 s", start_ms <= 2000U);
            (void)stop_replay(ctx);
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            continue;
        }
        if (start_rc != 0) {
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            rc = 1;
            continue;
        }
        BlockedRead reader;
        reader.ctx = ctx;
        dsd_thread_t thread;
        if (dsd_thread_create(&thread, blocked_read_fn, &reader) != 0) {
            (void)stop_replay(ctx);
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            return expect_true("the decoder thread started", 0);
        }
        char what[256];
        DSD_SNPRINTF(what, sizeof(what), "%s: the decoder's read ended within 2 s", label);
        const int ended = wait_for_int(&reader.done, 1, 2000U);
        rc |= expect_true(what, ended);
        if (!ended) {
            dsd_exitflag_store(1);
        }
        (void)dsd_thread_join(thread);
        DSD_SNPRINTF(what, sizeof(what), "%s: samples delivered", label);
        rc |= expect_u64_eq(what, reader.delivered.load(), 0U);
        DSD_SNPRINTF(what, sizeof(what), "%s: blocks the demod took", label);
        rc |= expect_u64_eq(what, (uint64_t)pacing.blocks.load(), 1U);
        (void)stop_replay(ctx);
        dsd_exitflag_store(0);
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    }
    return rc;
}

namespace {
struct ReplayStart {
    RtlSdrContext* ctx = nullptr;
    std::atomic<int> done{0};
    int rc = 0;
};
} // namespace

static DSD_THREAD_RETURN_TYPE
replay_start_fn(void* arg) {
    ReplayStart* start = static_cast<ReplayStart*>(arg);
    start->rc = rtl_stream_start(start->ctx);
    start->done.store(1, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/* Issue #572: the replay reader failing to start once the demod waits for the decoder's first read unwinds the start
 * within 5 s, with no decoder to read and without the global exit flag. */
static int
test_replay_reader_start_failure_at_the_first_demand_wait(void) {
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_capture(tone_and_noise_payload(kChunkBytes / 2U), metadata_path, sizeof(metadata_path)) != 0) {
        return 1;
    }
    std::unique_ptr<dsd_opts> opts(new dsd_opts());
    DSD_MEMSET(opts.get(), 0, sizeof(dsd_opts));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = DSD_IQ_REPLAY_RATE_FAST;
    opts->frame_dmr = 1;
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", metadata_path);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", metadata_path);
    ReplayStart start;
    if (rtl_stream_create(opts.get(), &start.ctx) != 0 || !start.ctx) {
        return expect_true("reader start failure: rtl_stream_create", 0);
    }
    Pacing pacing;
    rtl_stream_test_set_replay_stage_hook(pacing_stage, &pacing);
    rtl_device_test_replay_fail_start(1);
    int rc = 0;
    dsd_thread_t thread;
    if (dsd_thread_create(&thread, replay_start_fn, &start) != 0) {
        rc = expect_true("reader start failure: the start thread started", 0);
    } else {
        const int finished = wait_for_int(&start.done, 1, 5000U);
        rc |= expect_true("reader start failure: the start returned within 5 s", finished);
        if (finished) {
            rc |= expect_true("reader start failure: no global exit", !dsd_exitflag_load());
        } else {
            dsd_exitflag_store(1);
        }
        (void)dsd_thread_join(thread);
        rc |= expect_true("reader start failure: the start failed", start.rc != 0);
    }
    rtl_device_test_replay_fail_start(0);
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    rtl_stream_destroy(start.ctx);
    dsd_exitflag_store(0);
    rc |= expect_true("reader start failure: the opening thread got to the reader's start",
                      pacing.reader_starting.load());
    rc |= expect_true("reader start failure: the demod waited for the decoder's first read when the start failed",
                      pacing.start_saw_demand.load());
    rc |= expect_u64_eq("reader start failure: blocks the demod took", (uint64_t)pacing.blocks.load(), 0U);
    rc |= expect_true("reader start failure: no stream resources left", !rtl_stream_test_has_resources());
    return rc;
}

/* ---------------- Event boundaries ---------------- */

/* A short chunk: shorter than the reader's 64 KiB, so a capture of a few runs fast. */
static const size_t kShortChunkBytes = 40000U;

/* A capture of @p chunks short chunks with a RETUNE and a RESET between each two, as a live retune records them,
 * hopping from the first channel centre to the second and back; @p out_layout gets its chunks. */
static int
make_retune_capture(size_t chunks, std::vector<LayoutChunk>* out_layout, char* out_metadata_path,
                    size_t out_metadata_path_size) {
    std::vector<CaptureEvent> events;
    for (size_t i = 1U; i < chunks; i++) {
        const uint64_t center_hz = (i % 2U) != 0U ? kSecondCenterHz : kFirstCenterHz;
        events.push_back(CaptureEvent{i * kShortChunkBytes, DSD_IQ_EVENT_RETUNE, center_hz});
        events.push_back(CaptureEvent{i * kShortChunkBytes, DSD_IQ_EVENT_RESET, center_hz});
    }
    const std::vector<uint8_t> payload = tone_and_noise_payload(chunks * kShortChunkBytes / 2U);
    *out_layout = capture_layout(payload.size(), events);
    return make_capture_with_events(payload, events, out_metadata_path, out_metadata_path_size);
}

/* Read the replay to its end on a decoder thread, 512 samples at a time, within 10 s (else end it with the exit flag);
 * @p out_delivered gets the samples read. */
static int
read_replay_to_end(const char* label, RtlSdrContext* ctx, uint64_t* out_delivered) {
    *out_delivered = 0U;
    BlockedRead reader;
    reader.ctx = ctx;
    dsd_thread_t thread;
    if (dsd_thread_create(&thread, blocked_read_fn, &reader) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: the decoder thread started\n", label);
        return 1;
    }
    int rc = 0;
    if (!wait_for_int(&reader.done, 1, 10000U)) {
        DSD_FPRINTF(stderr, "FAIL: %s: the replay reached its end within 10 s\n", label);
        dsd_exitflag_store(1);
        rc = 1;
    }
    (void)dsd_thread_join(thread);
    dsd_exitflag_store(0);
    *out_delivered = reader.delivered.load();
    return rc;
}

namespace {
/* test_replay_boundary_waits_for_a_stalled_decoder(). */
struct DecoderStall {
    std::atomic<int> holds{0};
    std::atomic<int> resets_at_hold_end{-1};   /* RESETs applied by the time the decoder went on */
    std::atomic<int> restarts_at_hold_end{-1}; /* ... and loop rewinds */
};

/* A decoder that reads until it gets samples of chunk stop_sequence's batch. */
struct BatchReader {
    RtlSdrContext* ctx = nullptr;
    uint64_t stop_sequence = 0U;
    std::atomic<uint64_t> before_stop{0U}; /* samples it read from the batches before that one */
    std::atomic<int> reached{0};
    std::atomic<int> done{0};
};
} // namespace

static void
decoder_stall_stage(int stage, size_t count, void* ctx) {
    (void)count;
    DecoderStall* stall = static_cast<DecoderStall*>(ctx);
    if (stage != RTL_STREAM_TEST_REPLAY_DECODER_OUTPUT_FOUND
        || stall->holds.fetch_add(1, std::memory_order_acq_rel) != 0) {
        return;
    }
    /* The decoder found its first batch, the chunk before the first RESET or the rewind, and stalls four times as long
       as the boundary used to wait for it (50 ms) before it cleared what was left. */
    dsd_sleep_ms(200U);
    const rtl_stream_test_replay_state state = replay_state();
    stall->resets_at_hold_end.store((int)state.replay_event_reset_count, std::memory_order_release);
    stall->restarts_at_hold_end.store((int)state.replay_loop_restart_count, std::memory_order_release);
}

static DSD_THREAD_RETURN_TYPE
batch_reader_fn(void* arg) {
    BatchReader* reader = static_cast<BatchReader*>(arg);
    float buf[512];
    for (;;) {
        int got = 0;
        if (rtl_stream_read(reader->ctx, buf, 512U, &got) != 0) {
            break;
        }
        rtl_stream_replay_batch tag;
        if (got <= 0 || rtl_stream_get_replay_batch(&tag) != 0) {
            continue;
        }
        if (tag.chunk_sequence >= reader->stop_sequence) {
            reader->reached.store(1, std::memory_order_release);
            break;
        }
        reader->before_stop.fetch_add((uint64_t)got, std::memory_order_acq_rel);
    }
    reader->done.store(1, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/* Issue #572: a RESET, and a loop rewind, wait with no deadline for the decoder to read every sample before them. A
 * decoder that stalls on the batch before one (200 ms, the forced interleaving) gets all of it once it goes on, where
 * the boundary used to clear it after 50 ms. The RESET case reads the capture to its end; the rewind case reads until
 * the second pass's first batch. */
static int
test_replay_boundary_waits_for_a_stalled_decoder(void) {
    int rc = 0;
    for (int rewind = 0; rewind <= 1; rewind++) {
        const char* label = rewind ? "stalled decoder at a rewind" : "stalled decoder at a RESET";
        std::vector<LayoutChunk> layout;
        char metadata_path[DSD_TEST_PATH_MAX];
        const std::vector<uint8_t> one_chunk = tone_and_noise_payload(kShortChunkBytes / 2U);
        const int made = rewind ? make_capture(one_chunk, metadata_path, sizeof(metadata_path))
                                : make_retune_capture(3U, &layout, metadata_path, sizeof(metadata_path));
        if (made != 0) {
            return 1;
        }
        if (rewind) {
            layout = capture_layout(one_chunk.size(), std::vector<CaptureEvent>());
        }
        DecoderStall stall;
        rtl_stream_test_set_replay_stage_hook(decoder_stall_stage, &stall);
        std::unique_ptr<dsd_opts> opts;
        RtlSdrContext* ctx = NULL;
        if (start_replay_reporting(metadata_path, 0, rewind, 1, &opts, &ctx) != 0) {
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            return 1;
        }
        /* The RESET case reads to the end (a chunk the capture never reaches); the rewind case to chunk 2, the second
           pass's first. */
        BatchReader reader;
        reader.ctx = ctx;
        reader.stop_sequence = rewind ? 2U : UINT64_MAX;
        dsd_thread_t thread;
        char what[256];
        if (dsd_thread_create(&thread, batch_reader_fn, &reader) != 0) {
            DSD_SNPRINTF(what, sizeof(what), "%s: the decoder thread started", label);
            rc |= expect_true(what, 0);
        } else {
            if (!wait_for_int(&reader.done, 1, 10000U)) {
                DSD_SNPRINTF(what, sizeof(what), "%s: the decoder read what it wanted within 10 s", label);
                rc |= expect_true(what, 0);
                dsd_exitflag_store(1);
            }
            (void)dsd_thread_join(thread);
        }
        const rtl_stream_test_replay_state state = replay_state();
        (void)stop_replay(ctx);
        dsd_exitflag_store(0);
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        DSD_SNPRINTF(what, sizeof(what), "%s: the decoder stalled on its first batch", label);
        rc |= expect_true(what, stall.holds.load() >= 1);
        DSD_SNPRINTF(what, sizeof(what), "%s: nothing was applied while the decoder stalled before it", label);
        rc |= expect_true(what, (rewind ? stall.restarts_at_hold_end.load() : stall.resets_at_hold_end.load()) == 0);
        if (rewind) {
            DSD_SNPRINTF(what, sizeof(what), "%s: the decoder got to the second pass", label);
            rc |= expect_true(what, reader.reached.load());
        } else {
            DSD_SNPRINTF(what, sizeof(what), "%s: RESETs applied", label);
            rc |= expect_u64_eq(what, state.replay_event_reset_count, 2U);
        }
        DSD_SNPRINTF(what, sizeof(what), "%s: samples delivered (the oracle's count, none dropped)", label);
        rc |= expect_u64_eq(what, reader.before_stop.load(), oracle_output_count_of(layout));
        DSD_SNPRINTF(what, sizeof(what), "%s: output samples a block could not publish", label);
        rc |= expect_u64_eq(what, state.replay_output_truncated, 0U);
    }
    return rc;
}

namespace {
/* test_replay_purge_the_demod_takes_keeps_the_next_chunk(). */
struct PurgeRace {
    std::atomic<int> top_holds{0};              /* holds of the demod at the top of its loop after the first block */
    std::atomic<int> held_until_purge_wait{0};  /* ... that lasted until the reader waited for the RESET's purge */
    std::atomic<int> purge_waits{0};            /* the reader's waits for a boundary's purge */
    std::atomic<int> purges_taken{0};           /* purge flags the demod took */
    std::atomic<int> reader_waited_for_take{0}; /* the reader's first purge wait went on only once the demod took it */
};
} // namespace

static void
purge_race_stage(int stage, size_t count, void* ctx) {
    PurgeRace* race = static_cast<PurgeRace*>(ctx);
    switch (stage) {
        case RTL_STREAM_TEST_REPLAY_DEMOD_WAIT_FOR_DEMAND:
            /* The first block is published (the virtual block 0 and it): hold the demod at the top of its loop, ahead
               of its purge check, until the reader waits for the RESET's purge, so that the demod takes the flag. */
            if (count == 2U && race->top_holds.fetch_add(1, std::memory_order_acq_rel) == 0) {
                race->held_until_purge_wait.store(wait_for_int(&race->purge_waits, 1, 3000U),
                                                  std::memory_order_release);
            }
            break;
        case RTL_STREAM_TEST_REPLAY_READER_PURGE_WAIT:
            /* The reader waits for the RESET's purge once the demod took the flag... */
            if (race->purge_waits.fetch_add(1, std::memory_order_acq_rel) == 0) {
                race->reader_waited_for_take.store(wait_for_int(&race->purges_taken, 1, 3000U),
                                                   std::memory_order_release);
            }
            break;
        case RTL_STREAM_TEST_REPLAY_DEMOD_PURGE_TAKEN:
            /* ... and the demod discards the input ring 100 ms after it took the flag. */
            if (race->purges_taken.fetch_add(1, std::memory_order_acq_rel) == 0) {
                dsd_sleep_ms(100U);
            }
            break;
        default: break;
    }
}

/* Issue #572: the replay reader goes on past a RESET only once its input purge is applied. When the demod takes the
 * purge flag, the reader waits until the demod has discarded the input ring, so the chunk after the RESET, which the
 * reader commits next, is never the one discarded. The forced interleaving: the demod takes the flag while the reader
 * waits, and holds its discard 100 ms. */
static int
test_replay_purge_the_demod_takes_keeps_the_next_chunk(void) {
    std::vector<LayoutChunk> layout;
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_retune_capture(3U, &layout, metadata_path, sizeof(metadata_path)) != 0) {
        return 1;
    }
    PurgeRace race;
    LegObserver observer;
    rtl_stream_test_set_replay_stage_hook(purge_race_stage, &race);
    rtl_stream_test_set_replay_block_hook(leg_block_hook, &observer);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    int rc = 1;
    uint64_t delivered = 0U;
    if (start_replay(metadata_path, 0, &opts, &ctx) == 0) {
        rc = read_replay_to_end("purge race", ctx, &delivered);
        (void)stop_replay(ctx);
    }
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);
    rc |= expect_true("purge race: the demod waited at the top of its loop until the reader waited for the purge",
                      race.held_until_purge_wait.load());
    rc |= expect_true("purge race: the demod took the purge flag while the reader waited for the purge",
                      race.reader_waited_for_take.load());
    Signature sig;
    sig.blocks = observer.blocks;
    rc |= expect_blocks_follow_layout("purge race (the chunk after the RESET reaches the demod)", sig, layout);
    rc |= expect_u64_eq("purge race: samples delivered (the oracle's count, none dropped)", delivered,
                        oracle_output_count_of(layout));
    return rc;
}

namespace {
/* test_replay_reset_after_a_retune_resets_from_the_old_centre(): the reset plan in force as the reader goes on past
 * each RESET. */
struct ResetPlans {
    rtl_stream_test_reset_plan plan[2] = {};
    std::atomic<int> have[2] = {{0}, {0}};
};
} // namespace

static void
reset_plan_stage(int stage, size_t count, void* ctx) {
    ResetPlans* plans = static_cast<ResetPlans*>(ctx);
    /* The reader read chunk 2 or 3, after the first or second RESET. */
    if (stage != RTL_STREAM_TEST_REPLAY_READER_WAIT_FOR_EMPTY_INPUT || count < 2U || count > 3U) {
        return;
    }
    const size_t which = count - 2U;
    plans->have[which].store(rtl_stream_test_get_last_reset_plan(&plans->plan[which]) == 0 ? 1 : 0,
                             std::memory_order_release);
}

static int
expect_reset_plan(const char* label, const ResetPlans& plans, size_t which, uint32_t previous_hz, uint32_t next_hz,
                  int reset_fll, int restored_fll) {
    const rtl_stream_test_reset_plan& plan = plans.plan[which];
    if (!plans.have[which].load() || std::strcmp(plan.reason, "frequency") != 0
        || plan.previous_center_hz != previous_hz || plan.next_center_hz != next_hz
        || plan.reset_retained_fll != reset_fll || plan.restored_cached_fll != restored_fll) {
        DSD_FPRINTF(stderr,
                    "FAIL: %s: reset plan %s %u -> %u Hz, FLL reset %d, restored %d; want frequency %u -> %u Hz, FLL "
                    "reset %d, restored %d\n",
                    label, plans.have[which].load() ? plan.reason : "(none)", plan.previous_center_hz,
                    plan.next_center_hz, plan.reset_retained_fll, plan.restored_cached_fll, previous_hz, next_hz,
                    reset_fll, restored_fll);
        return 1;
    }
    return 0;
}

/* Issue #572: a replayed RESET resets the demod from the centre the RETUNE before it left, as the live retune it
 * records did: a hop to a new centre starts its band-edge FLL fresh, and the hop back restores the seed cached for the
 * old centre. Resetting from the centre the RETUNE moved to, both hops kept the old channel's FLL. */
static int
test_replay_reset_after_a_retune_resets_from_the_old_centre(void) {
    std::vector<LayoutChunk> layout;
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_retune_capture(3U, &layout, metadata_path, sizeof(metadata_path)) != 0) {
        return 1;
    }
    ResetPlans plans;
    rtl_stream_test_set_replay_stage_hook(reset_plan_stage, &plans);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    int rc = 1;
    uint64_t delivered = 0U;
    if (start_replay(metadata_path, 0, &opts, &ctx) == 0) {
        rc = read_replay_to_end("reset plan", ctx, &delivered);
        (void)stop_replay(ctx);
    }
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    rc |= expect_reset_plan("reset plan: the RESET after the hop to the second centre", plans, 0U,
                            (uint32_t)kFirstCenterHz, (uint32_t)kSecondCenterHz, 1, 0);
    rc |= expect_reset_plan("reset plan: the RESET after the hop back", plans, 1U, (uint32_t)kSecondCenterHz,
                            (uint32_t)kFirstCenterHz, 0, 1);
    return rc;
}

namespace {
/* test_replay_event_boundary_waits_stop_in_bounded_time(). */
struct BoundaryWait {
    std::atomic<int> boundaries{0};
    std::atomic<int> first_kind{-1}; /* the event the reader first waited for (DSD_IQ_EVENT_*), 0: a rewind */
};
} // namespace

static void
boundary_wait_stage(int stage, size_t count, void* ctx) {
    BoundaryWait* wait = static_cast<BoundaryWait*>(ctx);
    if (stage == RTL_STREAM_TEST_REPLAY_READER_EVENT_BOUNDARY
        && wait->boundaries.fetch_add(1, std::memory_order_acq_rel) == 0) {
        wait->first_kind.store((int)count, std::memory_order_release);
    }
}

/* Wait up to @p timeout_ms for the replay reader thread to leave. */
static int
wait_for_reader_exit(unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited <= timeout_ms; waited++) {
        if (replay_state().replay_reader_exited) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

/* Issue #572: the replay reader waits at an event, or at a loop rewind, until the decoder has read everything before
 * it, and that wait ends in bounded time on a stop. The decoder reads one sample of the first chunk and no more. The
 * reader must wait at the RETUNE (or the rewind) without applying it; then a soft stop returns within 2 s, and a forced
 * stop or the global exit flag ends the reader within 1 s. */
static int
test_replay_event_boundary_waits_stop_in_bounded_time(void) {
    static const char* const kHow[] = {"soft stop", "forced stop", "global exit"};
    int rc = 0;
    for (int rewind = 0; rewind <= 1; rewind++) {
        for (int how = 0; how < 3; how++) {
            char label[160];
            DSD_SNPRINTF(label, sizeof(label), "%s, %s", rewind ? "loop rewind" : "RETUNE boundary", kHow[how]);
            char metadata_path[DSD_TEST_PATH_MAX];
            std::vector<LayoutChunk> layout;
            const int made = rewind ? make_capture(tone_and_noise_payload(kShortChunkBytes / 2U), metadata_path,
                                                   sizeof(metadata_path))
                                    : make_retune_capture(2U, &layout, metadata_path, sizeof(metadata_path));
            if (made != 0) {
                return 1;
            }
            BoundaryWait wait;
            rtl_stream_test_set_replay_stage_hook(boundary_wait_stage, &wait);
            std::unique_ptr<dsd_opts> opts;
            RtlSdrContext* ctx = NULL;
            if (start_replay_reporting(metadata_path, 0, rewind, 1, &opts, &ctx) != 0) {
                rtl_stream_test_set_replay_stage_hook(NULL, NULL);
                return 1;
            }
            float sample = 0.0f;
            int got = 0;
            char what[256];
            DSD_SNPRINTF(what, sizeof(what), "%s: the decoder read a sample of the first chunk", label);
            rc |= expect_true(what, rtl_stream_read(ctx, &sample, 1U, &got) == 0 && got == 1);
            const int waiting = wait_for_int(&wait.boundaries, 1, 2000U);
            /* Four times the 50 ms a RESET or rewind used to wait for the decoder. */
            dsd_sleep_ms(200U);
            const rtl_stream_test_replay_state state = replay_state();
            DSD_SNPRINTF(what, sizeof(what), "%s: the reader waits at the boundary", label);
            rc |= expect_true(what, waiting && wait.first_kind.load() == (rewind ? 0 : (int)DSD_IQ_EVENT_RETUNE));
            DSD_SNPRINTF(what, sizeof(what), "%s: nothing is applied while the decoder has not read the chunk", label);
            rc |= expect_true(what,
                              rewind ? state.replay_loop_restart_count == 0U : state.replay_event_retune_count == 0U);
            if (how == 1) {
                rtl_stream_test_replay_force_stop();
            } else if (how == 2) {
                dsd_exitflag_store(1);
            }
            if (how != 0) {
                DSD_SNPRINTF(what, sizeof(what), "%s: the replay reader left within 1 s", label);
                rc |= expect_true(what, wait_for_reader_exit(1000U));
            }
            const uint64_t stop_ms = stop_replay(ctx);
            dsd_exitflag_store(0);
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            if (stop_ms > 2000U) {
                DSD_FPRINTF(stderr, "FAIL: %s: the stop took %llu ms (want <= 2000)\n", label,
                            (unsigned long long)stop_ms);
                rc = 1;
            }
        }
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_replay_output_does_not_depend_on_the_reader();
    rc |= test_replay_events_do_not_depend_on_the_reader();
    rc |= test_replay_publishes_a_block_whole_with_its_tag();
    rc |= test_replay_demand_waits_stop_in_bounded_time();
    rc |= test_replay_decoder_wait_stops_in_bounded_time();
    rc |= test_replay_without_output_ends();
    rc |= test_replay_reader_start_failure_at_the_first_demand_wait();
    rc |= test_replay_boundary_waits_for_a_stalled_decoder();
    rc |= test_replay_purge_the_demod_takes_keeps_the_next_chunk();
    rc |= test_replay_reset_after_a_retune_resets_from_the_old_centre();
    rc |= test_replay_event_boundary_waits_stop_in_bounded_time();
    rc |= remove_fixture_dirs();
    return rc ? 1 : 0;
}
