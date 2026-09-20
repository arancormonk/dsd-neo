// SPDX-License-Identifier: GPL-3.0-or-later
#include <airspy.h>
#include <cstdlib>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/safe_api.h>
#include <initializer_list>
#include <memory>
#include <stdint.h>
#include <stdio.h>
/* Link the real native adapter against an in-process SDK double. No USB access. */

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            DSD_FPRINTF(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                        \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

struct airspy_device {
    int unused;
};

static airspy_device device;
static airspy_sample_block_cb_fn rx;
static void* rx_context;
static bool streaming;
static bool device_open;
static int starts, stops, closes, opens, failure;
static uint32_t frequency, rate;
static uint64_t selected;
static int sensitivity, linearity, lna, mixer, vga, lna_agc, mixer_agc, bias;
static uint32_t rates[2] = {10000000, 2500000};

extern "C" {
int
airspy_list_devices(uint64_t* serials, int count) {
    if (count > 0) {
        serials[0] = 0x123456789ABCDEF0ULL;
    }
    return 1;
}

int
airspy_open_sn(airspy_device** out, uint64_t serial) {
    ++opens;
    selected = serial;
    if (serial != 0x123456789ABCDEF0ULL || failure == 1) {
        return AIRSPY_ERROR_NOT_FOUND;
    }
    CHECK(!device_open);
    device_open = true;
    *out = &device;
    return 0;
}

int
airspy_close(airspy_device*) {
    ++closes;
    device_open = false;
    streaming = false;
    return 0;
}

int
airspy_board_partid_serialno_read(airspy_device*, airspy_read_partid_serialno_t* id) {
    id->serial_no[2] = 0x12345678;
    id->serial_no[3] = 0x9ABCDEF0;
    return 0;
}

int
airspy_get_samplerates(airspy_device*, uint32_t* out, uint32_t count) {
    if (!count) {
        *out = (uint32_t)2U;
        return 0;
    }
    CHECK(count == 2U);
    for (size_t i = 0; i < 2U; ++i) {
        out[i] = rates[i];
    }
    return 0;
}

int
airspy_set_sample_type(airspy_device*, airspy_sample_type type) {
    CHECK(type == AIRSPY_SAMPLE_FLOAT32_IQ);
    return 0;
}

int
airspy_set_samplerate(airspy_device*, uint32_t value) {
    CHECK(!streaming);
    rate = value;
    return 0;
}

int
airspy_set_freq(airspy_device*, uint32_t value) {
    CHECK(!streaming);
    if (failure == 3 || (failure == 4 && value != frequency)) {
        return AIRSPY_ERROR_LIBUSB;
    }
    frequency = value;
    return 0;
}

int
airspy_start_rx(airspy_device*, airspy_sample_block_cb_fn cb, void* context) {
    if (streaming) {
        return AIRSPY_ERROR_BUSY;
    }
    rx = cb;
    rx_context = context;
    streaming = true;
    ++starts;
    // Simulate submitted transfers surviving a later SDK thread creation failure.
    return failure == 2 ? AIRSPY_ERROR_THREAD : 0;
}

int
airspy_stop_rx(airspy_device*) {
    streaming = false;
    ++stops;
    return 0;
}

int
airspy_is_streaming(airspy_device*) {
    return streaming ? AIRSPY_TRUE : 0;
}

int
airspy_set_sensitivity_gain(airspy_device*, uint8_t value) {
    sensitivity = value;
    lna_agc = mixer_agc = 0;
    return 0;
}

int
airspy_set_linearity_gain(airspy_device*, uint8_t value) {
    linearity = value;
    lna_agc = mixer_agc = 0;
    return 0;
}

int
airspy_set_lna_gain(airspy_device*, uint8_t value) {
    lna = value;
    return 0;
}

int
airspy_set_mixer_gain(airspy_device*, uint8_t value) {
    mixer = value;
    return 0;
}

int
airspy_set_vga_gain(airspy_device*, uint8_t value) {
    vga = value;
    return 0;
}

int
airspy_set_lna_agc(airspy_device*, uint8_t value) {
    lna_agc = value;
    return 0;
}

int
airspy_set_mixer_agc(airspy_device*, uint8_t value) {
    mixer_agc = value;
    return 0;
}

int
airspy_set_rf_bias(airspy_device*, uint8_t value) {
    bias = value;
    return 0;
}

const char*
airspy_error_name(airspy_error) {
    return "injected error";
}
}

#ifndef DSD_TEST_AIRSPY_STREAM
#include <cstring>
#include <dsd-neo/runtime/input_failure.h>
#include <vector>
#include "airspy_source.h"

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
#include <dsd-neo/io/rtl_stream.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/exitflag.h>
#include "rtl_stream_test_support.h"

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
    }
    rtl_stream_test_set_thread_create(nullptr);
    return 0;
}
#endif
