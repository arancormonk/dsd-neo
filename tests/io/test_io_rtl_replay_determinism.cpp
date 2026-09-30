// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Issue #572: under --iq-replay the decoder paces the demod. The demod starts a block only once the decoder waits on
 * an empty output ring with every block it published acknowledged, and publishes the block's output in one step with
 * its batch tag. The decoder's output then depends on the capture alone: fast or realtime, read greedily or slowly,
 * in any read size. These tests pin that, the publication step, and a bounded shutdown at every wait it adds. */

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
#include <dsd-neo/runtime/exitflag.h>
#include <memory>
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

/* Write @p payload as a cu8 capture with its sidecar; @p out_metadata_path receives the sidecar's path. */
static int
make_capture(const std::vector<uint8_t>& payload, char* out_metadata_path, size_t out_metadata_path_size) {
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
    cfg.center_frequency_hz = 851375000ULL;
    cfg.capture_center_frequency_hz = 851759000ULL;
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
    if (!payload.empty() && dsd_iq_capture_submit(writer, payload.data(), payload.size()) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        DSD_FPRINTF(stderr, "FAIL: could not write the capture\n");
        return 1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    if (DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        return 1;
    }
    return 0;
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

/* The oracle for how many samples a replay of @p capture_bytes delivers, from the capture layout alone: the reader
 * hands the demod 64 KiB chunks, the last one shorter, and each chunk's block decimates its complex samples by 32 to
 * the 48 kHz FSK discriminator output, floor(n / 32), with no resampling at 48 kHz. (A channel FIR that carries its
 * look-ahead across blocks moves this.) Nothing is dropped: a replay delivers every sample of every block. */
static uint64_t
oracle_output_count(size_t capture_bytes) {
    uint64_t total = 0U;
    for (size_t offset = 0U; offset < capture_bytes; offset += kChunkBytes) {
        const size_t bytes = (capture_bytes - offset < kChunkBytes) ? capture_bytes - offset : kChunkBytes;
        total += (uint64_t)(bytes / 2U) / kDecimation;
    }
    return total;
}

/* ---------------- Streams ---------------- */

/* A DMR replay: the FSK discriminator output, at 4800 symbols/s. @p report: say why a start failed. */
static int
start_replay_reporting(const char* metadata_path, int realtime, int report, std::unique_ptr<dsd_opts>* out_opts,
                       RtlSdrContext** out_ctx) {
    *out_ctx = NULL;
    out_opts->reset(new dsd_opts());
    dsd_opts* opts = out_opts->get();
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = realtime ? DSD_IQ_REPLAY_RATE_REALTIME : DSD_IQ_REPLAY_RATE_FAST;
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
    return start_replay_reporting(metadata_path, realtime, 1, out_opts, out_ctx);
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

/* What a leg delivered. */
struct Signature {
    uint64_t samples = 0U;
    uint64_t fnv = 1469598103934665603ULL;
    std::vector<uint64_t> generation_changes; /* delivered positions where the output generation moved */
    int ended = 0;
    int reacquire_armed = 0;
    int profile_landed = 0; /* the stream ran the requested symbol rate by the end */
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

static int
run_leg(const char* metadata_path, const Leg& leg, Signature* sig) {
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay(metadata_path, leg.realtime, &opts, &ctx) != 0) {
        return 1;
    }
    if (leg.start_delay_ms > 0U) {
        dsd_sleep_ms(leg.start_delay_ms);
    }
    std::vector<float> buf(512U);
    uint32_t lcg = 0x9E3779B9U;
    uint32_t generation = 0U;
    int have_generation = 0;
    int reacquired = 0;
    int profiled = 0;
    const uint64_t deadline_ns = dsd_time_monotonic_ns() + 20000ULL * 1000000ULL;
    int rc = 0;
    for (size_t read_index = 0U;; read_index++) {
        if (dsd_time_monotonic_ns() > deadline_ns) {
            DSD_FPRINTF(stderr, "FAIL: determinism leg %s: the replay did not end within 20 s\n", leg.name);
            rc = 1;
            break;
        }
        const size_t want = leg.read_sizes[read_index % leg.read_size_count];
        int got = 0;
        if (rtl_stream_read(ctx, buf.data(), want, &got) != 0) {
            sig->ended = 1;
            break;
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
    int symbol_rate_hz = 0;
    (void)rtl_stream_get_symbol_profile_full(&symbol_rate_hz, NULL, NULL);
    sig->profile_landed = symbol_rate_hz == 2400 ? 1 : 0;
    (void)stop_replay(ctx);
    return rc;
}

static void
print_signature(const char* name, const Signature& sig) {
    DSD_FPRINTF(stderr, "  leg %-8s samples=%llu fnv=%016llx generation changes at", name,
                (unsigned long long)sig.samples, (unsigned long long)sig.fnv);
    for (uint64_t position : sig.generation_changes) {
        DSD_FPRINTF(stderr, " %llu", (unsigned long long)position);
    }
    DSD_FPRINTF(stderr, "\n");
}

/* Issue #572: an event-free replay decodes the same whoever reads it. A greedy fast reader, a slow reader that starts
 * late and reads 1, 7 and 512 samples at a time with sleeps between, and a greedy realtime reader each ask for an FSK
 * reacquire and a symbol profile change at the same delivered positions. Each must get every sample the capture
 * holds, bit for bit the same, with the output generation moving at the same positions. A front end that runs ahead
 * of its decoder lands those requests on whatever block it reached, and a reacquire clears output the decoder has not
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
    const uint64_t want_samples = oracle_output_count(payload.size());

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
        char label[128];
        DSD_SNPRINTF(label, sizeof(label), "determinism leg %s: the replay ended", legs[i].name);
        rc |= expect_true(label, sigs[i].ended);
        DSD_SNPRINTF(label, sizeof(label), "determinism leg %s: the FSK reacquire was armed", legs[i].name);
        rc |= expect_true(label, sigs[i].reacquire_armed);
        DSD_SNPRINTF(label, sizeof(label), "determinism leg %s: the reacquire moved the output generation",
                     legs[i].name);
        rc |= expect_true(label, !sigs[i].generation_changes.empty());
        DSD_SNPRINTF(label, sizeof(label), "determinism leg %s: the symbol profile change landed", legs[i].name);
        rc |= expect_true(label, sigs[i].profile_landed);
        DSD_SNPRINTF(label, sizeof(label), "determinism leg %s: samples delivered (the oracle's count, none dropped)",
                     legs[i].name);
        rc |= expect_u64_eq(label, sigs[i].samples, want_samples);
        if (i > 0U
            && (sigs[i].samples != sigs[0].samples || sigs[i].fnv != sigs[0].fnv
                || sigs[i].generation_changes != sigs[0].generation_changes)) {
            DSD_FPRINTF(stderr, "FAIL: determinism: leg %s delivered a different stream from leg %s\n", legs[i].name,
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
 * down, from the capture's first sample), its size, and the DMR output it ran on. */
static int
tag_matches_its_chunk(const rtl_stream_replay_batch& tag) {
    const uint64_t chunk_complex = kChunkBytes / 2U;
    const uint64_t start_ns = ((tag.chunk_sequence - 1U) * chunk_complex * 1000000000ULL) / kCaptureRateHz;
    const uint64_t end_ns = (tag.chunk_sequence * chunk_complex * 1000000000ULL) / kCaptureRateHz;
    return tag.output_count == chunk_complex / kDecimation && tag.media_start_ns == start_ns
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
        const int start_rc = start_replay_reporting(metadata_path, 0, !empty, &opts, &ctx);
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

int
main(void) {
    int rc = 0;
    rc |= test_replay_output_does_not_depend_on_the_reader();
    rc |= test_replay_publishes_a_block_whole_with_its_tag();
    rc |= test_replay_demand_waits_stop_in_bounded_time();
    rc |= test_replay_decoder_wait_stops_in_bounded_time();
    rc |= test_replay_without_output_ends();
    rc |= test_replay_reader_start_failure_at_the_first_demand_wait();
    rc |= remove_fixture_dirs();
    return rc ? 1 : 0;
}
