// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The libairspy SDK double; airspy_sdk_double.h describes it. */

#include "airspy_sdk_double.h"
#include <airspy.h>
#include <stdint.h>

namespace airspy_double {

airspy_device device;
airspy_sample_block_cb_fn rx;
void* rx_context;
bool streaming;
bool device_open;
int starts;
int stops;
int closes;
int opens;
int failure;
uint32_t frequency;
uint32_t rate;
uint64_t selected;
int sensitivity;
int linearity;
int lna;
int mixer;
int vga;
int lna_agc;
int mixer_agc;
int bias;
uint32_t rates[2] = {10000000, 2500000};

} // namespace airspy_double

using namespace airspy_double;

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
