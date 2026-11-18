// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The SoapyAirspy module the Windows release builds, from the pinned upstream source with dsd-neo's patch
 * (DESKTOP_SOAPYAIRSPY_ERRORS, tests/CMakeLists.txt), linked against the stub libairspy entry points below, which fail
 * on request. A frequency, sample-rate, gain or AGC write the device refuses must reach the caller (SoapySDR's C++ API
 * throws; its C API turns that into an error code), and the module must go on reporting the setting the device last
 * accepted. A stream activation whose rate write or start fails must return an error rather than a stream that never
 * delivers, and so must a read that applies a pending rate.
 */

#include <SoapyAirspy.hpp>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.hpp>
#include <cmath>
#include <dsd-neo/core/safe_api.h>
#include <exception>
#include <libairspy/airspy.h>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

namespace SoapySDR {
class Stream;
}

namespace {

/* What the stub device was told, and which of its calls fail next. */
struct StubDevice {
    int fail_set_freq;
    int fail_set_samplerate;
    int fail_start;
    int fail_lna_gain;
    int fail_mixer_gain;
    int fail_vga_gain;
    int fail_lna_agc;
    int fail_mixer_agc;
    uint32_t freq_hz;
    uint32_t rate_hz;
    uint8_t lna_gain;
    uint8_t mixer_gain;
    uint8_t vga_gain;
    uint8_t lna_agc;
    uint8_t mixer_agc;
    int streaming;
    int starts;
};

StubDevice g_stub;
char g_handle;
int g_failures = 0;

/* The firmware's rate list: what airspy_get_samplerates() reports. */
const uint32_t kRates[] = {10000000, 2500000};
const uint32_t kRateCount = sizeof(kRates) / sizeof(kRates[0]);

/* Stores @p value in @p field unless @p fail asks for a refusal, as the device would. */
int
write_or_refuse(int fail, uint8_t* field, uint8_t value) {
    if (fail) {
        return AIRSPY_ERROR_LIBUSB;
    }
    *field = value;
    return AIRSPY_SUCCESS;
}

} // namespace

extern "C" {

int ADDCALL
airspy_open(struct airspy_device** device) {
    *device = reinterpret_cast<struct airspy_device*>(&g_handle);
    return AIRSPY_SUCCESS;
}

int ADDCALL
airspy_open_sn(struct airspy_device** device, uint64_t serial_number) {
    (void)serial_number;
    return airspy_open(device);
}

int ADDCALL
airspy_close(struct airspy_device* device) {
    (void)device;
    g_stub.streaming = 0;
    return AIRSPY_SUCCESS;
}

int ADDCALL
airspy_get_samplerates(struct airspy_device* device, uint32_t* buffer, const uint32_t len) {
    (void)device;
    if (len == 0) {
        *buffer = kRateCount;
        return AIRSPY_SUCCESS;
    }
    if (len > kRateCount) {
        return AIRSPY_ERROR_INVALID_PARAM;
    }
    DSD_MEMCPY(buffer, kRates, len * sizeof(uint32_t));
    return AIRSPY_SUCCESS;
}

int ADDCALL
airspy_set_samplerate(struct airspy_device* device, uint32_t samplerate) {
    (void)device;
    if (g_stub.fail_set_samplerate) {
        return AIRSPY_ERROR_LIBUSB;
    }
    g_stub.rate_hz = samplerate;
    return AIRSPY_SUCCESS;
}

int ADDCALL
airspy_set_sample_type(struct airspy_device* device, enum airspy_sample_type sample_type) {
    (void)device;
    (void)sample_type;
    return AIRSPY_SUCCESS;
}

int ADDCALL
airspy_set_freq(struct airspy_device* device, const uint32_t freq_hz) {
    (void)device;
    if (g_stub.fail_set_freq) {
        return AIRSPY_ERROR_LIBUSB;
    }
    g_stub.freq_hz = freq_hz;
    return AIRSPY_SUCCESS;
}

int ADDCALL
airspy_start_rx(struct airspy_device* device, airspy_sample_block_cb_fn callback, void* rx_ctx) {
    (void)device;
    (void)callback;
    (void)rx_ctx;
    if (g_stub.streaming) {
        return AIRSPY_ERROR_BUSY;
    }
    if (g_stub.fail_start) {
        return AIRSPY_ERROR_LIBUSB;
    }
    g_stub.starts++;
    g_stub.streaming = 1;
    return AIRSPY_SUCCESS;
}

int ADDCALL
airspy_stop_rx(struct airspy_device* device) {
    (void)device;
    g_stub.streaming = 0;
    return AIRSPY_SUCCESS;
}

int ADDCALL
airspy_is_streaming(struct airspy_device* device) {
    (void)device;
    return g_stub.streaming;
}

int ADDCALL
airspy_set_lna_gain(struct airspy_device* device, uint8_t value) {
    (void)device;
    return write_or_refuse(g_stub.fail_lna_gain, &g_stub.lna_gain, value);
}

int ADDCALL
airspy_set_mixer_gain(struct airspy_device* device, uint8_t value) {
    (void)device;
    return write_or_refuse(g_stub.fail_mixer_gain, &g_stub.mixer_gain, value);
}

int ADDCALL
airspy_set_vga_gain(struct airspy_device* device, uint8_t value) {
    (void)device;
    return write_or_refuse(g_stub.fail_vga_gain, &g_stub.vga_gain, value);
}

int ADDCALL
airspy_set_lna_agc(struct airspy_device* device, uint8_t value) {
    (void)device;
    return write_or_refuse(g_stub.fail_lna_agc, &g_stub.lna_agc, value);
}

int ADDCALL
airspy_set_mixer_agc(struct airspy_device* device, uint8_t value) {
    (void)device;
    return write_or_refuse(g_stub.fail_mixer_agc, &g_stub.mixer_agc, value);
}

int ADDCALL
airspy_set_rf_bias(struct airspy_device* dev, uint8_t value) {
    (void)dev;
    (void)value;
    return AIRSPY_SUCCESS;
}

int ADDCALL
airspy_set_packing(struct airspy_device* device, uint8_t value) {
    (void)device;
    (void)value;
    return AIRSPY_SUCCESS;
}

} // extern "C"

namespace {

void
expect(const char* name, bool ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL %s: %s\n", name, what);
        g_failures++;
    }
}

bool
close_to(double got, double want) {
    return std::fabs(got - want) < 0.5;
}

/* Runs @p call and reports whether it threw, as SoapySDR's C API would turn into an error code. */
template <typename Call>
bool
throws(Call call) {
    try {
        call();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

std::unique_ptr<SoapyAirspy>
open_device(const char* name) {
    g_stub = StubDevice();
    std::unique_ptr<SoapyAirspy> dev;
    try {
        dev.reset(new SoapyAirspy(SoapySDR::Kwargs()));
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "FAIL %s: the module did not open (%s)\n", name, e.what());
        g_failures++;
    }
    return dev;
}

SoapySDR::Stream*
setup_stream(SoapyAirspy& dev) {
    return dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, std::vector<size_t>(1, 0));
}

/* One read of a few complex float samples. */
int
read_some(SoapyAirspy& dev, SoapySDR::Stream* stream) {
    const size_t elems = 64;
    const size_t floats_per_elem = 2; /* I and Q */
    std::vector<float> samples(elems * floats_per_elem);
    void* buffs[] = {samples.data()};
    int flags = 0;
    long long time_ns = 0;
    return dev.readStream(stream, buffs, elems, flags, time_ns, 1000);
}

/* A refused tune throws, and the module keeps reporting the frequency the device has. */
void
refused_frequency_is_reported(void) {
    const char* name = "refused_frequency_is_reported";
    std::unique_ptr<SoapyAirspy> dev = open_device(name);
    if (!dev) {
        return;
    }
    expect(name, !throws([&] { dev->setFrequency(SOAPY_SDR_RX, 0, "RF", 100000000.0); }), "an accepted tune");
    expect(name, g_stub.freq_hz == 100000000U, "the device was tuned");
    g_stub.fail_set_freq = 1;
    expect(name, throws([&] { dev->setFrequency(SOAPY_SDR_RX, 0, "RF", 200000000.0); }), "a refused tune throws");
    expect(name, close_to(dev->getFrequency(SOAPY_SDR_RX, 0, "RF"), 100000000.0),
           "the frequency read back is the one the device has");
}

/* With no stream running, a rate is written to the device at once: a refused one throws and the rate read back stays
 * the one the device has; an accepted one is the rate read back. */
void
idle_rate_is_applied_or_reported(void) {
    const char* name = "idle_rate_is_applied_or_reported";
    std::unique_ptr<SoapyAirspy> dev = open_device(name);
    if (!dev) {
        return;
    }
    expect(name, !throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 10000000.0); }), "an accepted rate");
    expect(name, g_stub.rate_hz == 10000000U, "the device was given the rate");
    g_stub.fail_set_samplerate = 1;
    expect(name, throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 2500000.0); }), "a refused rate throws");
    expect(name, close_to(dev->getSampleRate(SOAPY_SDR_RX, 0), 10000000.0),
           "the rate read back is the one the device has");
}

/* An activation whose rate write or start fails returns an error, and a later one works. */
void
failed_activation_is_reported(void) {
    const char* name = "failed_activation_is_reported";
    std::unique_ptr<SoapyAirspy> dev = open_device(name);
    if (!dev) {
        return;
    }
    SoapySDR::Stream* stream = setup_stream(*dev);
    g_stub.fail_set_samplerate = 1;
    expect(name, dev->activateStream(stream) < 0, "an activation whose rate write fails returns an error");
    expect(name, g_stub.starts == 0, "no stream started on a rate the device refused");
    g_stub.fail_set_samplerate = 0;
    if (g_stub.streaming) {
        (void)dev->deactivateStream(stream);
    }
    g_stub.fail_start = 1;
    expect(name, dev->activateStream(stream) < 0, "an activation whose start fails returns an error");
    g_stub.fail_start = 0;
    if (g_stub.streaming) {
        (void)dev->deactivateStream(stream);
    }
    expect(name, dev->activateStream(stream) == 0, "a later activation works");
    expect(name, g_stub.streaming == 1, "the device streams");
    expect(name, dev->deactivateStream(stream) == 0, "deactivation");
    dev->closeStream(stream);
}

/* A rate change on a running stream restarts it on the new rate. A refused one throws and the stream runs on at the
 * rate it had, which is the rate read back. */
void
live_rate_change_is_applied_or_reported(void) {
    const char* name = "live_rate_change_is_applied_or_reported";
    std::unique_ptr<SoapyAirspy> dev = open_device(name);
    if (!dev) {
        return;
    }
    expect(name, !throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 10000000.0); }), "an accepted rate");
    SoapySDR::Stream* stream = setup_stream(*dev);
    expect(name, dev->activateStream(stream) == 0, "activation");
    expect(name, g_stub.rate_hz == 10000000U, "the stream runs at the rate set");
    g_stub.fail_set_samplerate = 1;
    expect(name, throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 2500000.0); }), "a refused rate throws");
    expect(name, close_to(dev->getSampleRate(SOAPY_SDR_RX, 0), 10000000.0),
           "the rate read back is the one the stream runs at");
    expect(name, g_stub.streaming == 1, "the stream runs on");
    g_stub.fail_set_samplerate = 0;
    expect(name, !throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 2500000.0); }), "an accepted rate change");
    expect(name, g_stub.rate_hz == 2500000U && g_stub.streaming == 1, "the stream runs at the new rate");
    expect(name, close_to(dev->getSampleRate(SOAPY_SDR_RX, 0), 2500000.0), "the new rate is read back");
    g_stub.fail_start = 1;
    expect(name, throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 10000000.0); }),
           "a rate change whose restart fails throws");
    g_stub.fail_start = 0;
    (void)dev->deactivateStream(stream);
    dev->closeStream(stream);
}

/* A stream set up again while it runs leaves its rate for the next read to apply; a read whose rate write or restart
 * fails returns an error instead of waiting for samples. */
void
read_with_refused_pending_rate_is_reported(void) {
    const char* name = "read_with_refused_pending_rate_is_reported";
    for (int fail_start = 0; fail_start < 2; ++fail_start) {
        std::unique_ptr<SoapyAirspy> dev = open_device(name);
        if (!dev) {
            return;
        }
        SoapySDR::Stream* stream = setup_stream(*dev);
        expect(name, dev->activateStream(stream) == 0, "activation");
        stream = setup_stream(*dev);
        g_stub.fail_set_samplerate = !fail_start;
        g_stub.fail_start = fail_start;
        expect(name, read_some(*dev, stream) == SOAPY_SDR_STREAM_ERROR,
               fail_start ? "a read whose restart fails returns a stream error"
                          : "a read whose rate write fails returns a stream error");
        g_stub.fail_set_samplerate = 0;
        g_stub.fail_start = 0;
        (void)dev->deactivateStream(stream);
        dev->closeStream(stream);
    }
}

/* A refused LNA, mixer or VGA gain throws, and the gain read back stays the one the device has. */
void
refused_gain_is_reported(void) {
    const char* name = "refused_gain_is_reported";
    std::unique_ptr<SoapyAirspy> dev = open_device(name);
    if (!dev) {
        return;
    }
    const char* const stages[] = {"LNA", "MIX", "VGA"};
    for (int i = 0; i < 3; ++i) {
        const char* stage = stages[i];
        expect(name, !throws([&] { dev->setGain(SOAPY_SDR_RX, 0, stage, 5.0); }), "an accepted gain");
        g_stub.fail_lna_gain = i == 0;
        g_stub.fail_mixer_gain = i == 1;
        g_stub.fail_vga_gain = i == 2;
        expect(name, throws([&] { dev->setGain(SOAPY_SDR_RX, 0, stage, 9.0); }), "a refused gain throws");
        expect(name, close_to(dev->getGain(SOAPY_SDR_RX, 0, stage), 5.0), "the gain read back is the device's");
        g_stub.fail_lna_gain = g_stub.fail_mixer_gain = g_stub.fail_vga_gain = 0;
    }
    expect(name, g_stub.lna_gain == 5 && g_stub.mixer_gain == 5 && g_stub.vga_gain == 5,
           "the device kept the gains it accepted");
    /* The overall gain is spread across the stages, so a stage that refuses fails it too. */
    g_stub.fail_vga_gain = 1;
    expect(name, throws([&] { dev->setGain(SOAPY_SDR_RX, 0, 30.0); }), "a refused overall gain throws");
    g_stub.fail_vga_gain = 0;
}

/* A refused AGC write throws, the mode read back stays the device's, and a half-applied change is put back. */
void
refused_gain_mode_is_reported(void) {
    const char* name = "refused_gain_mode_is_reported";
    std::unique_ptr<SoapyAirspy> dev = open_device(name);
    if (!dev) {
        return;
    }
    expect(name, !throws([&] { dev->setGainMode(SOAPY_SDR_RX, 0, false); }), "AGC off");
    g_stub.fail_lna_agc = 1;
    expect(name, throws([&] { dev->setGainMode(SOAPY_SDR_RX, 0, true); }), "a refused LNA AGC throws");
    expect(name, !dev->getGainMode(SOAPY_SDR_RX, 0), "the AGC mode read back is the device's");
    g_stub.fail_lna_agc = 0;
    g_stub.fail_mixer_agc = 1;
    expect(name, throws([&] { dev->setGainMode(SOAPY_SDR_RX, 0, true); }), "a refused mixer AGC throws");
    expect(name, !dev->getGainMode(SOAPY_SDR_RX, 0), "the AGC mode read back is the device's");
    expect(name, g_stub.lna_agc == 0 && g_stub.mixer_agc == 0, "the LNA AGC is put back off");
    g_stub.fail_mixer_agc = 0;
    expect(name, !throws([&] { dev->setGainMode(SOAPY_SDR_RX, 0, true); }), "AGC on");
    expect(name, dev->getGainMode(SOAPY_SDR_RX, 0) && g_stub.lna_agc == 1 && g_stub.mixer_agc == 1, "AGC is on");
}

} // namespace

int
main(void) {
    refused_frequency_is_reported();
    idle_rate_is_applied_or_reported();
    failed_activation_is_reported();
    live_rate_change_is_applied_or_reported();
    read_with_refused_pending_rate_is_reported();
    refused_gain_is_reported();
    refused_gain_mode_is_reported();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d expectation(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
