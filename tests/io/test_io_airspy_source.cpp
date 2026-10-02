// SPDX-License-Identifier: GPL-3.0-or-later
/* Link the real native adapter against the in-process SDK double (airspy_sdk_double.h). No USB access. */

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/safe_api.h>
#include <memory>
#include "airspy_sdk_double.h"

#ifndef DSD_TEST_AIRSPY_STREAM
#include <airspy.h>
#include <cstring>
#include <dsd-neo/runtime/input_failure.h>
#include <stdint.h>
#include <vector>
#include "airspy_source.h"

using namespace airspy_double;

struct airspy_source;

static void
capture(void* context, const float* samples, size_t count, uint64_t dropped) {
    auto* received = static_cast<std::vector<float>*>(context);
    CHECK(dropped == 7);
    received->assign(samples, samples + count * 2);
}

int
main() {
    dsd_airspy_config cfg;
    dsd_airspy_config_defaults(&cfg);
    // The adapter retains this context until close joins the receive callback.
    auto received = std::make_unique<std::vector<float>>();
    airspy_source* s = airspy_source_open(&cfg, capture, received.get());
    CHECK(s && selected == 0x123456789ABCDEF0ULL && sensitivity == 10 && !bias);
    dsd_airspy_info info{};
    CHECK(airspy_source_info(s, &info) == 0 && info.sample_rate == 10000000 && info.rate_count == 2);
    CHECK(strcmp(info.serial, "123456789ABCDEF0") == 0 && cfg.sample_rate == 0);
    CHECK(airspy_source_frequency(s, 851375000) == 0);
    CHECK(airspy_source_start(s) == 0 && airspy_source_running(s));
    float iq[] = {0.125f, -0.75f, 0.5f, 0.25f};
    airspy_transfer transfer{&device, rx_context, iq, 2, 7, AIRSPY_SAMPLE_FLOAT32_IQ};
    CHECK(rx(&transfer) == 0 && *received == std::vector<float>(iq, iq + 4));
    CHECK(airspy_source_info(s, &info) == 0 && info.dropped_samples == 7);
    int previous_starts = starts;
    CHECK(airspy_source_frequency(s, 852000000) == 0 && frequency == 852000000);
    CHECK(starts == previous_starts + 1 && stops == 1 && streaming);
    CHECK(airspy_source_rate(s, 10000000) == 0 && starts == previous_starts + 1);
    CHECK(airspy_source_rate(s, 2500000) == 0 && rate == 2500000 && streaming);
    CHECK(airspy_source_rate(s, 1536000) != 0 && rate == 2500000);
    cfg.gain_mode = DSD_AIRSPY_MANUAL;
    cfg.lna_gain = 15;
    cfg.mixer_gain = 8;
    cfg.vga_gain = 7;
    cfg.bias_tee = 1;
    CHECK(airspy_source_controls(s, &cfg) == 0 && lna == 15 && mixer == 8 && vga == 7 && bias);
    cfg.lna_agc = cfg.mixer_agc = 1;
    CHECK(airspy_source_controls(s, &cfg) == 0 && lna_agc && mixer_agc);
    cfg.gain_mode = DSD_AIRSPY_LINEARITY;
    cfg.linearity_gain = 21;
    CHECK(airspy_source_controls(s, &cfg) == 0 && linearity == 21 && !lna_agc && !mixer_agc);
    failure = 4;
    CHECK(airspy_source_frequency(s, 853000000) != 0 && streaming && frequency == 852000000);
    failure = 3;
    CHECK(airspy_source_frequency(s, 853000000) != 0 && !streaming);
    failure = 0;
    streaming = false; // USB removal is visible to the stream monitor.
    CHECK(!airspy_source_running(s));
    airspy_source_close(s);
    CHECK(!streaming && closes == 1 && !bias);
    cfg.sample_rate = 3000000;
    rates[0] = 6000000;
    rates[1] = 3000000;
    s = airspy_source_open(&cfg, capture, received.get());
    CHECK(s && rate == 3000000);
    failure = 2;
    int previous_stops = stops;
    CHECK(airspy_source_start(s) == AIRSPY_ERROR_THREAD && !airspy_source_running(s));
    CHECK(stops == previous_stops + 1 && !streaming);
    failure = 0;
    CHECK(airspy_source_start(s) == 0 && airspy_source_running(s));
    for (bool change_rate : {false, true}) {
        failure = 2;
        previous_stops = stops;
        int rc = change_rate ? airspy_source_rate(s, 6000000) : airspy_source_frequency(s, 854000000);
        CHECK(rc == AIRSPY_ERROR_THREAD && !airspy_source_running(s));
        CHECK(stops == previous_stops + 2 && !streaming);
        failure = 0;
        CHECK(airspy_source_start(s) == 0 && airspy_source_running(s));
    }
    airspy_source_close(s);
    cfg.sample_rate = 2500000;
    CHECK(!airspy_source_open(&cfg, capture, received.get()));
    cfg.sample_rate = 0;
    DSD_SNPRINTF(cfg.serial, sizeof cfg.serial, "%s", "0000000000000001");
    int previous_opens = opens;
    CHECK(!airspy_source_open(&cfg, capture, received.get()) && opens == previous_opens + 1);
    dsd_input_failure latched{};
    dsd_input_failure_get(&latched);
    CHECK(latched.kind == DSD_INPUT_FAILURE_DEVICE);
    cfg.serial[0] = '\0';
    s = airspy_source_open(&cfg, capture, received.get());
    CHECK(s);
    dsd_input_failure_get(&latched);
    CHECK(latched.kind == DSD_INPUT_FAILURE_NONE);
    airspy_source_close(s);
    cfg.sample_rate = 2500000; // Configuration failures also clear on recovery.
    CHECK(!airspy_source_open(&cfg, capture, received.get()));
    dsd_input_failure_get(&latched);
    CHECK(latched.kind == DSD_INPUT_FAILURE_DEVICE);
    cfg.sample_rate = 0;
    s = airspy_source_open(&cfg, capture, received.get());
    CHECK(s);
    dsd_input_failure_get(&latched);
    CHECK(latched.kind == DSD_INPUT_FAILURE_NONE);
    airspy_source_close(s);
    CHECK(airspy_source_set_fd(42) != 0 && !airspy_source_fd_in_use());
    return 0;
}

#else
#include <atomic>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rtl_stream.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/rtl_stream_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/exitflag.h>
#include <initializer_list>
#include <stdio.h>
#include "rtl_stream_test_support.h"
#include "test_support.h"

using namespace airspy_double;

namespace {
struct Worker {
    dsd_thread_fn entry;
    void* context;
};
} // namespace

static Worker workers[2];
static int create_calls;
static int fail_create;
static std::atomic<int> workers_entered{0};
static std::atomic<int> workers_exited{0};

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    tracked_worker(void* context) {
    auto* worker = static_cast<Worker*>(context);
    workers_entered.fetch_add(1);
    auto result = worker->entry(worker->context);
    workers_exited.fetch_add(1);
    return result;
}

/* The size of the file at @p path, or -1 when it cannot be read. */
static long
file_size(const char* path) {
    FILE* fp = dsd_fopen_private(path, "rb");
    if (!fp) {
        return -1;
    }
    long size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) {
        size = ftell(fp);
    }
    (void)fclose(fp);
    return size;
}

/* Stand in for what an earlier stream recorded at @p path: 1024 bytes. */
static void
write_recording(const char* path) {
    FILE* recording = dsd_fopen_private(path, "wb");
    CHECK(recording);
    static const unsigned char kRecorded[1024] = {0};
    CHECK(fwrite(kRecorded, 1, sizeof kRecorded, recording) == sizeof kRecorded);
    CHECK(fclose(recording) == 0);
}

/* Start a stream on @p opts that fails, with no worker or SDK failure injected unless the caller set one. */
static void
start_failing_stream(dsd_opts& opts) {
    fail_create = create_calls = 0;
    workers_entered.store(0);
    workers_exited.store(0);
    {
        RtlSdrOrchestrator stream(opts);
        CHECK(stream.start() != 0);
    }
    CHECK(!rtl_stream_test_has_resources() && !device_open);
}

static int
create_worker(dsd_thread_t* thread, dsd_thread_fn entry, void* context) {
    int index = create_calls++;
    if (create_calls == fail_create) {
        return -1;
    }
    CHECK(index < 2);
    workers[index] = {entry, context};
    int rc = dsd_thread_create(thread, tracked_worker, &workers[index]);
    if (rc == 0) {
        // Ensure the real worker has entered before injecting the next failure.
        while (workers_entered.load() <= index) {
            dsd_sleep_ms(1);
        }
    }
    return rc;
}

int
main() {
    auto opts = std::make_unique<dsd_opts>();
    *opts = {};
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 851375000;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_volume_multiplier = 1;
    opts->frame_p25p1 = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "airspy");
    dsd_airspy_config_defaults(&opts->airspy);
    rtl_stream_test_set_thread_create(create_worker);
    // 4: initial frequency programming fails after the device opened (configure stage);
    // 3: SDK start fails after both workers exist; 2/1: the second/first worker fails.
    for (int stage : {4, 3, 2, 1}) {
        fail_create = stage < 3 ? stage : 0;
        failure = stage == 3 ? 2 : (stage == 4 ? 3 : 0);
        create_calls = 0;
        workers_entered.store(0);
        workers_exited.store(0);
        int previous_closes = closes;
        RtlSdrOrchestrator stream(*opts);
        CHECK(stream.start() != 0);
        const int expected_workers = stage == 4 ? 0 : stage - 1;
        CHECK(workers_entered.load() == expected_workers);
        CHECK(workers_exited.load() == expected_workers);
        CHECK(!rtl_stream_test_has_resources());
        CHECK(!device_open && closes == previous_closes + 1 && !streaming);
        CHECK(!dsd_exitflag_load());

        fail_create = failure = create_calls = 0;
        workers_entered.store(0);
        workers_exited.store(0);
        CHECK(stream.start() == 0);
        CHECK(device_open && streaming);
        CHECK(stream.stop() == 0);
        CHECK(workers_entered.load() == 2 && workers_exited.load() == 2);
        CHECK(!rtl_stream_test_has_resources());
        CHECK(!device_open && !streaming && closes == previous_closes + 2);
        CHECK(!dsd_exitflag_load());
        CHECK(rtl_stream_start_analog_refusal(nullptr, nullptr, nullptr) == 0);
    }

    // Issue #578: a start that refuses the analog width at the rate the device delivers records the refusal, which
    // names that rate once the stream is gone. The Airspy decimates its capture to 19,531 Hz at a 12 kHz DSP bandwidth,
    // which filters NFM 12.5 kHz but not 25 kHz.
    opts->analog_only = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->rtl_dsp_bw_khz = 12;
    for (int width_hz : {25000, 12500}) {
        opts->analog_nfm_bandwidth_hz = width_hz;
        fail_create = failure = create_calls = 0;
        workers_entered.store(0);
        workers_exited.store(0);
        int previous_closes = closes;
        RtlSdrOrchestrator stream(*opts);
        int kind = -1;
        int refused_width_hz = -1;
        int rate_hz = -1;
        if (width_hz == 25000) {
            CHECK(stream.start() != 0);
            CHECK(rtl_stream_start_analog_refusal(&kind, &refused_width_hz, &rate_hz) == 1);
            CHECK(kind == DSD_ANALOG_DEMOD_FM && refused_width_hz == 25000);
            CHECK(rate_hz == 19531 && !dsd_analog_width_realizable(25000, rate_hz));
            CHECK(!rtl_stream_test_has_resources());
            CHECK(!device_open && closes == previous_closes + 1 && !streaming);
        } else {
            CHECK(stream.start() == 0);
            CHECK(rtl_stream_start_analog_refusal(&kind, &refused_width_hz, &rate_hz) == 0);
            CHECK(kind == -1 && refused_width_hz == -1 && rate_hz == -1);
            CHECK(stream.stop() == 0);
            CHECK(!rtl_stream_test_has_resources());
        }
        CHECK(!dsd_exitflag_load());
    }
    // A create forgets the last start's refusal, so a start that never runs leaves none to report.
    opts->analog_nfm_bandwidth_hz = 25000;
    fail_create = failure = create_calls = 0;
    workers_entered.store(0);
    workers_exited.store(0);
    {
        RtlSdrOrchestrator stream(*opts);
        CHECK(stream.start() != 0);
    }
    CHECK(rtl_stream_start_analog_refusal(nullptr, nullptr, nullptr) == 1);
    RtlSdrContext* ctx = nullptr;
    CHECK(rtl_stream_create(opts.get(), &ctx) == 0 && ctx);
    CHECK(rtl_stream_start_analog_refusal(nullptr, nullptr, nullptr) == 0);
    CHECK(rtl_stream_destroy(ctx) == 0);

    // Issue #578: a start opens the I/Q capture (--iq-capture) once the device runs, before its workers and the Airspy
    // SDK's start, writing the file anew, and records that it did: a start that fails after that point has already
    // written over what a stream before it recorded there, so a rollback cannot say it kept it. A start its analog
    // check refuses fails before, and leaves the recording as it was.
    char capture_dir[DSD_TEST_PATH_MAX];
    char capture_path[DSD_TEST_PATH_MAX];
    CHECK(dsd_test_mkdtemp(capture_dir, sizeof capture_dir, "dsdneo_start_capture") != nullptr);
    CHECK(dsd_test_path_join(capture_path, sizeof capture_path, capture_dir, "cap.iq") == 0);
    write_recording(capture_path);
    opts->iq_capture_requested = 1;
    opts->iq_capture_format = DSD_IQ_FORMAT_CF32;
    DSD_SNPRINTF(opts->iq_capture_path, sizeof opts->iq_capture_path, "%s", capture_path);
    for (int sdk_fails : {0, 1}) {
        // 25 kHz, which the 19,531 Hz rate refuses at the analog check; 12.5 kHz, which it filters, with an SDK start
        // that fails.
        opts->analog_nfm_bandwidth_hz = sdk_fails ? 12500 : 25000;
        fail_create = create_calls = 0;
        failure = sdk_fails ? 2 : 0;
        workers_entered.store(0);
        workers_exited.store(0);
        {
            RtlSdrOrchestrator stream(*opts);
            CHECK(stream.start() != 0);
        }
        CHECK(!rtl_stream_test_has_resources() && !device_open);
        CHECK(rtl_stream_start_analog_refusal(nullptr, nullptr, nullptr) == (sdk_fails ? 0 : 1));
        CHECK(rtl_stream_start_opened_capture() == sdk_fails);
        CHECK(file_size(capture_path) == (sdk_fails ? 0 : 1024));
    }
    // What the start records is the writer's own open of the file. A writer that fails before it opens the file
    // (here a directory that does not exist) wrote nothing, and the start says it did not open the capture; one that
    // opened the file and then failed (its metadata sidecar cannot replace the directory standing at that path) had
    // written the recording anew, and the start says it did.
    opts->analog_nfm_bandwidth_hz = 12500;
    failure = 0;
    char missing_path[DSD_TEST_PATH_MAX];
    CHECK(dsd_test_path_join(missing_path, sizeof missing_path, capture_dir, "missing/cap.iq") == 0);
    DSD_SNPRINTF(opts->iq_capture_path, sizeof opts->iq_capture_path, "%s", missing_path);
    start_failing_stream(*opts);
    CHECK(rtl_stream_start_opened_capture() == 0);
    CHECK(file_size(missing_path) == -1);
    DSD_SNPRINTF(opts->iq_capture_path, sizeof opts->iq_capture_path, "%s", capture_path);
    write_recording(capture_path);
    char sidecar_path[DSD_TEST_PATH_MAX];
    CHECK(dsd_test_path_join(sidecar_path, sizeof sidecar_path, capture_dir, "cap.iq.json") == 0);
    CHECK(dsd_mkdir(sidecar_path, 0700) == 0);
    start_failing_stream(*opts);
    CHECK(rtl_stream_start_opened_capture() == 1);
    CHECK(file_size(capture_path) == -1);
    CHECK(dsd_test_rmdir(sidecar_path) == 0);
    // A create forgets it.
    failure = 0;
    CHECK(rtl_stream_create(opts.get(), &ctx) == 0 && ctx);
    CHECK(rtl_stream_start_opened_capture() == 0);
    CHECK(rtl_stream_destroy(ctx) == 0);
    opts->iq_capture_requested = 0;
    static const char* const kCaptureFiles[] = {"cap.iq", "cap.iq.json", nullptr};
    CHECK(dsd_test_remove_temp_dir(capture_dir, kCaptureFiles) == 0);
    rtl_stream_test_set_thread_create(nullptr);
    return 0;
}
#endif
