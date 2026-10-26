// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Frontends poll the receiver's telemetry from their own threads (the terminal's UI thread, Qt, Android, the API
 * feed) while the decoder thread soft-stops the stream, replaces the input and starts the next one. What they read
 * must be values the stream published, never the device object, the stream internals or the stream context a UI
 * snapshot copied the pointer of, all of which a soft stop frees (B15, B18).
 *
 * - frontend_metrics_survive_soft_stop: a thread polls the whole frontend metrics getter, with every SNR fallback
 *   and the visualizer getters, against a UI snapshot retained from the first stream, while the main thread starts,
 *   reads and soft-stops replays repeatedly, swapping between two inputs. Under ASan a read of the freed context or
 *   device is a heap-use-after-free; under TSan an unsynchronized read of the stream's pointers is a race. The main
 *   thread also polls the retained snapshot itself right after the first stop, so the freed context is read whatever
 *   the scheduling.
 * - tuner_gain_readback_without_device: the gain getter reports the running input's gain, nothing once the stream has
 *   stopped (never the stopped device's), and the next input's gain after a replacement.
 */

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/wideband_spectrum.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "test_support.h"

extern "C" {
#include "engine_hooks_install.h"
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

/* The files the capture writer puts in a fixture directory. */
static const char* const kFixtureFiles[] = {"fixture.iq", "fixture.iq.json", nullptr};
static std::vector<std::string> g_fixture_dirs;

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

/* A cu8 replay of a few demod blocks whose sidecar records @p tuner_gain_tenth_db as its tuner gain. */
static int
make_replay_fixture(int tuner_gain_tenth_db, char* out_metadata_path, size_t out_size) {
    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_frontend_telemetry")) {
        DSD_FPRINTF(stderr, "FAIL: could not create temporary fixture directory\n");
        return 1;
    }
    g_fixture_dirs.emplace_back(temp_dir);
    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "fixture.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "fixture.iq.json") != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not construct fixture paths\n");
        return 1;
    }
    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", metadata_path);
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = 1536000U;
    cfg.center_frequency_hz = 851375000ULL;
    cfg.capture_center_frequency_hz = 851759000ULL;
    cfg.tuner_gain_tenth_db = tuner_gain_tenth_db;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 32U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = 48000U;
    cfg.fs4_shift_enabled = 1;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=0");
    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open IQ capture writer: %s\n", err_buf[0] ? err_buf : "unknown");
        return 1;
    }
    /* Four live transfers of a ramp: enough for the input level to have something to measure. */
    std::vector<uint8_t> payload(16384U * 4U);
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<uint8_t>(i & 0xFFU);
    }
    if (dsd_iq_capture_submit(writer, payload.data(), payload.size()) != DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "FAIL: could not submit fixture payload\n");
        dsd_iq_capture_abort(writer);
        return 1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    if (DSD_SNPRINTF(out_metadata_path, out_size, "%s", metadata_path) >= (int)out_size) {
        DSD_FPRINTF(stderr, "FAIL: metadata path overflow\n");
        return 1;
    }
    return 0;
}

/* A looping fast replay of @p metadata_path, so the stream keeps running until it is stopped. */
static void
prepare_replay_opts(dsd_opts* opts, const char* metadata_path) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = DSD_IQ_REPLAY_RATE_FAST;
    opts->iq_replay_loop = 1;
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", metadata_path);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", metadata_path);
}

/* Create and start a stream on @p opts; NULL when either fails. */
static RtlSdrContext*
start_stream(dsd_opts* opts) {
    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(opts, &ctx) != 0 || !ctx) {
        DSD_FPRINTF(stderr, "FAIL: rtl_stream_create\n");
        return NULL;
    }
    if (rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: rtl_stream_start\n");
        (void)rtl_stream_destroy(ctx);
        return NULL;
    }
    return ctx;
}

/* Read a few blocks, as the decoder would, so the demod runs and the stream publishes its input level. */
static int
read_some(RtlSdrContext* ctx) {
    float block[1024];
    for (int i = 0; i < 4; i++) {
        int got = 0;
        if (rtl_stream_read(ctx, block, sizeof(block) / sizeof(block[0]), &got) != 0) {
            DSD_FPRINTF(stderr, "FAIL: rtl_stream_read\n");
            return 1;
        }
    }
    return 0;
}

/* The soft stop an input replacement or a menu restart runs: stop, then free the context. */
static void
stop_stream(RtlSdrContext* ctx) {
    (void)rtl_stream_stop(ctx);
    (void)rtl_stream_destroy(ctx);
}

/* Everything a frontend polls: the metrics with every fallback, and the visualizer copies. */
static void
poll_frontend(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* m) {
    (void)dsd_app_frontend_get_metrics_for_snapshot(opts, state, m, DSD_FRONTEND_SNR_FALLBACK_ALL);
    static thread_local float buf[DSD_WIDEBAND_SPECTRUM_BINS * 2];
    int sps = 0;
    int rate = 0;
    uint32_t center = 0;
    uint32_t span = 0;
    uint32_t serial = 0;
    (void)dsd_app_frontend_constellation_get(buf, DSD_WIDEBAND_SPECTRUM_BINS);
    (void)dsd_app_frontend_eye_get(buf, DSD_WIDEBAND_SPECTRUM_BINS, &sps);
    (void)dsd_app_frontend_spectrum_get(buf, DSD_WIDEBAND_SPECTRUM_BINS, &rate);
    (void)dsd_app_frontend_wideband_spectrum_get(buf, DSD_WIDEBAND_SPECTRUM_BINS, &center, &span, &serial);
}

/* Let the poller run at least @p polls more times (bounded, so a stuck poller fails the checks instead of hanging). */
static void
wait_for_polls(const std::atomic<unsigned>& count, unsigned polls) {
    const unsigned target = count.load(std::memory_order_acquire) + polls;
    for (unsigned spins = 0; spins < 50000000U && count.load(std::memory_order_acquire) < target; spins++) {
        std::this_thread::yield();
    }
}

static int
test_frontend_metrics_survive_soft_stop(const char* input_a, const char* input_b) {
    int rc = 0;
    std::unique_ptr<dsd_opts> stream_opts = std::make_unique<dsd_opts>();
    prepare_replay_opts(stream_opts.get(), input_a);
    /* The UI's copies: an options snapshot, and a state snapshot whose stream context is the first stream's, which
       the frontend keeps polling against after that stream is gone (a UI snapshot copies the pointer, not the
       stream). */
    std::unique_ptr<dsd_opts> ui_opts = std::make_unique<dsd_opts>();
    prepare_replay_opts(ui_opts.get(), input_a);
    std::unique_ptr<dsd_state> ui_state = std::make_unique<dsd_state>();

    RtlSdrContext* first = start_stream(stream_opts.get());
    if (!first) {
        return 1;
    }
    ui_state->rtl_ctx = first;

    std::atomic<int> stop{0};
    std::atomic<unsigned> polls{0};
    std::thread poller([&] {
        dsd_frontend_metrics m;
        while (stop.load(std::memory_order_acquire) == 0) {
            poll_frontend(ui_opts.get(), ui_state.get(), &m);
            polls.fetch_add(1U, std::memory_order_acq_rel);
        }
    });

    rc |= read_some(first);
    wait_for_polls(polls, 2U);
    stop_stream(first);

    dsd_frontend_metrics after_stop;
    poll_frontend(ui_opts.get(), ui_state.get(), &after_stop);
    rc |= expect_int("stopped stream reads inactive", after_stop.stream_active, 0);
    rc |= expect_int("stopped stream has no tuner gain", after_stop.tuner_gain_valid, 0);

    /* Restart, and replace the input with another, several times over, with the poller running throughout. */
    for (int cycle = 0; cycle < 8 && rc == 0; cycle++) {
        prepare_replay_opts(stream_opts.get(), (cycle & 1) ? input_a : input_b);
        RtlSdrContext* ctx = start_stream(stream_opts.get());
        if (!ctx) {
            rc = 1;
            break;
        }
        wait_for_polls(polls, 1U);
        rc |= read_some(ctx);
        wait_for_polls(polls, 1U);
        stop_stream(ctx);
        wait_for_polls(polls, 1U);
    }

    stop.store(1, std::memory_order_release);
    poller.join();
    rc |= expect_true("poller ran", polls.load(std::memory_order_acquire) > 0U);

    dsd_frontend_metrics m;
    poll_frontend(ui_opts.get(), ui_state.get(), &m);
    rc |= expect_int("final: stream inactive", m.stream_active, 0);
    rc |= expect_int("final: no tuner gain", m.tuner_gain_valid, 0);
    rc |= expect_int("final: no input level", (int)m.input_level.sample_count, 0);
    rc |= expect_int("final: no decode health", m.decode_health.valid, 0);
    return rc;
}

static int
test_tuner_gain_readback_without_device(const char* input_a, const char* input_b) {
    int rc = 0;
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    prepare_replay_opts(opts.get(), input_a);
    RtlSdrContext* ctx = start_stream(opts.get());
    if (!ctx) {
        return 1;
    }
    int gain = 12345;
    int is_auto = -1;
    rc |= expect_int("running: gain read", rtl_stream_get_gain(&gain, &is_auto), 0);
    rc |= expect_int("running: recorded gain", gain, 270);
    rc |= expect_int("running: manual", is_auto, 0);
    dsd_frontend_metrics m;
    poll_frontend(opts.get(), NULL, &m);
    rc |= expect_int("running: metrics gain valid", m.tuner_gain_valid, 1);
    rc |= expect_int("running: metrics gain", m.tuner_gain_tenth_db, 270);
    rc |= expect_int("running: stream active", m.stream_active, 1);
    stop_stream(ctx);

    gain = 12345;
    is_auto = -1;
    rc |= expect_int("stopped: no reading", rtl_stream_get_gain(&gain, &is_auto), -1);
    rc |= expect_int("stopped: no value", gain, 0);
    rc |= expect_int("stopped: mode reads auto", is_auto, 1);
    poll_frontend(opts.get(), NULL, &m);
    rc |= expect_int("stopped: metrics gain invalid", m.tuner_gain_valid, 0);

    /* The replacement input's own gain, not the stopped one's. */
    prepare_replay_opts(opts.get(), input_b);
    ctx = start_stream(opts.get());
    if (!ctx) {
        return 1;
    }
    rc |= expect_int("replaced: gain read", rtl_stream_get_gain(&gain, &is_auto), 0);
    rc |= expect_int("replaced: its recorded gain", gain, -10);
    rc |= expect_int("replaced: manual", is_auto, 0);
    stop_stream(ctx);
    return rc;
}

int
main(void) {
    dsd_engine_rtl_stream_metrics_hooks_install();
    char input_a[DSD_TEST_PATH_MAX];
    char input_b[DSD_TEST_PATH_MAX];
    int rc = 0;
    if (make_replay_fixture(270, input_a, sizeof(input_a)) != 0
        || make_replay_fixture(-10, input_b, sizeof(input_b)) != 0) {
        rc = 1;
    }
    if (rc == 0) {
        rc |= test_frontend_metrics_survive_soft_stop(input_a, input_b);
        rc |= test_tuner_gain_readback_without_device(input_a, input_b);
    }
    rc |= remove_fixture_dirs();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "PASS: frontend telemetry\n");
    }
    return rc;
}
