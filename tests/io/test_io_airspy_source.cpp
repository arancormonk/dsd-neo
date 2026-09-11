// SPDX-License-Identifier: GPL-3.0-or-later
#include <airspy.h>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "airspy_source.h"
/* Link the real native adapter against an in-process SDK double. No USB access. */

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            DSD_FPRINTF(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                        \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

struct airspy_source;

struct airspy_device {
    int unused;
};

static airspy_device device;
static airspy_sample_block_cb_fn rx;
static void* rx_context;
static bool streaming;
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
    *out = &device;
    return 0;
}

int
airspy_close(airspy_device*) {
    ++closes;
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
    if (failure == 2) {
        return AIRSPY_ERROR_THREAD;
    }
    CHECK(!streaming);
    rx = cb;
    rx_context = context;
    streaming = true;
    ++starts;
    return 0;
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
    std::vector<float> received;
    airspy_source* s = airspy_source_open(&cfg, capture, &received);
    CHECK(s && selected == 0x123456789ABCDEF0ULL && sensitivity == 10 && !bias);
    dsd_airspy_info info{};
    CHECK(airspy_source_info(s, &info) == 0 && info.sample_rate == 10000000 && info.rate_count == 2);
    CHECK(strcmp(info.serial, "123456789ABCDEF0") == 0 && cfg.sample_rate == 0);
    CHECK(airspy_source_frequency(s, 851375000) == 0);
    CHECK(airspy_source_start(s) == 0 && airspy_source_running(s));
    float iq[] = {0.125f, -0.75f, 0.5f, 0.25f};
    airspy_transfer transfer{&device, rx_context, iq, 2, 7, AIRSPY_SAMPLE_FLOAT32_IQ};
    CHECK(rx(&transfer) == 0 && received == std::vector<float>(iq, iq + 4));
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
    s = airspy_source_open(&cfg, capture, &received);
    CHECK(s && rate == 3000000);
    failure = 2;
    CHECK(airspy_source_start(s) != 0 && !airspy_source_running(s));
    airspy_source_close(s);
    failure = 0;
    cfg.sample_rate = 2500000;
    CHECK(!airspy_source_open(&cfg, capture, &received));
    cfg.sample_rate = 0;
    DSD_SNPRINTF(cfg.serial, sizeof cfg.serial, "%s", "0000000000000001");
    int previous_opens = opens;
    CHECK(!airspy_source_open(&cfg, capture, &received) && opens == previous_opens + 1);
    CHECK(airspy_source_set_fd(42) != 0 && !airspy_source_fd_in_use());
    return 0;
}
