// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The SoapyAirspyHF module the Windows release builds, from the pinned upstream source with dsd-neo's patch
 * (DESKTOP_SOAPYAIRSPYHF_ERRORS, tests/CMakeLists.txt), linked against the stub libairspyhf entry points below, which
 * fail on request. A frequency, sample-rate, attenuator, LNA or AGC write the device refuses must reach the caller
 * (SoapySDR's C++ API throws; its C API turns that into an error code), and the module must go on reporting the setting
 * the device last accepted. A stream activation whose rate write or start fails must return an error rather than a
 * stream that never delivers, and so must a read that applies a pending rate.
 */

#include <SoapyAirspyHF.hpp>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.hpp>
#include <cmath>
#include <dsd-neo/core/safe_api.h>
#include <exception>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

/* What the stub device was told, and which of its calls fail next. */
struct StubDevice {
    int open_handles;
    int fail_set_freq;
    int fail_set_samplerate;
    int fail_start;
    int fail_att;
    int fail_lna;
    int fail_agc;
    uint32_t freq_hz;
    uint32_t rate_hz;
    int streaming;
    int starts;
};

StubDevice g_stub;
char g_handle;
int g_failures = 0;

/* The firmware's rate list: what airspyhf_get_samplerates() reports. */
const uint32_t kRates[] = {912000, 768000, 384000};
const uint32_t kRateCount = sizeof(kRates) / sizeof(kRates[0]);

int
result(int fail) {
    return fail ? AIRSPYHF_ERROR : AIRSPYHF_SUCCESS;
}

} // namespace

extern "C" {

int ADDCALL
airspyhf_open(airspyhf_device_t** device) {
    *device = reinterpret_cast<airspyhf_device_t*>(&g_handle);
    g_stub.open_handles++;
    return AIRSPYHF_SUCCESS;
}

int ADDCALL
airspyhf_open_sn(airspyhf_device_t** device, uint64_t serial_number) {
    (void)serial_number;
    return airspyhf_open(device);
}

int ADDCALL
airspyhf_close(airspyhf_device_t* device) {
    (void)device;
    g_stub.open_handles--;
    g_stub.streaming = 0;
    return AIRSPYHF_SUCCESS;
}

int ADDCALL
airspyhf_get_samplerates(airspyhf_device_t* device, uint32_t* buffer, const uint32_t len) {
    (void)device;
    if (len == 0) {
        *buffer = kRateCount;
        return AIRSPYHF_SUCCESS;
    }
    if (len > kRateCount) {
        return AIRSPYHF_ERROR;
    }
    DSD_MEMCPY(buffer, kRates, len * sizeof(uint32_t));
    return AIRSPYHF_SUCCESS;
}

int ADDCALL
airspyhf_set_samplerate(airspyhf_device_t* device, uint32_t samplerate) {
    (void)device;
    if (g_stub.fail_set_samplerate) {
        return AIRSPYHF_ERROR;
    }
    g_stub.rate_hz = samplerate;
    return AIRSPYHF_SUCCESS;
}

int ADDCALL
airspyhf_set_freq(airspyhf_device_t* device, const uint32_t freq_hz) {
    (void)device;
    if (g_stub.fail_set_freq) {
        return AIRSPYHF_ERROR;
    }
    g_stub.freq_hz = freq_hz;
    return AIRSPYHF_SUCCESS;
}

int ADDCALL
airspyhf_start(airspyhf_device_t* device, airspyhf_sample_block_cb_fn callback, void* ctx) {
    (void)device;
    (void)callback;
    (void)ctx;
    if (g_stub.fail_start) {
        return AIRSPYHF_ERROR;
    }
    g_stub.starts++;
    g_stub.streaming = 1;
    return AIRSPYHF_SUCCESS;
}

int ADDCALL
airspyhf_stop(airspyhf_device_t* device) {
    (void)device;
    g_stub.streaming = 0;
    return AIRSPYHF_SUCCESS;
}

int ADDCALL
airspyhf_is_streaming(airspyhf_device_t* device) {
    (void)device;
    return g_stub.streaming;
}

int ADDCALL
airspyhf_set_hf_att(airspyhf_device_t* device, uint8_t att_index) {
    (void)device;
    (void)att_index;
    return result(g_stub.fail_att);
}

int ADDCALL
airspyhf_set_hf_lna(airspyhf_device_t* device, uint8_t flag) {
    (void)device;
    (void)flag;
    return result(g_stub.fail_lna);
}

int ADDCALL
airspyhf_set_hf_agc(airspyhf_device_t* device, uint8_t flag) {
    (void)device;
    (void)flag;
    return result(g_stub.fail_agc);
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

std::unique_ptr<SoapyAirspyHF>
open_device(const char* name) {
    g_stub = StubDevice();
    std::unique_ptr<SoapyAirspyHF> dev;
    try {
        dev.reset(new SoapyAirspyHF(SoapySDR::Kwargs()));
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "FAIL %s: the module did not open (%s)\n", name, e.what());
        g_failures++;
    }
    return dev;
}

SoapySDR::Stream*
setup_stream(SoapyAirspyHF& dev) {
    return dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, std::vector<size_t>(1, 0));
}

/* A refused tune throws, and the module keeps reporting the frequency the device has. */
void
refused_frequency_is_reported(void) {
    const char* name = "refused_frequency_is_reported";
    std::unique_ptr<SoapyAirspyHF> dev = open_device(name);
    if (!dev) {
        return;
    }
    expect(name, !throws([&] { dev->setFrequency(SOAPY_SDR_RX, 0, "RF", 1000000.0); }), "an accepted tune");
    expect(name, g_stub.freq_hz == 1000000U, "the device was tuned");
    g_stub.fail_set_freq = 1;
    expect(name, throws([&] { dev->setFrequency(SOAPY_SDR_RX, 0, "RF", 2000000.0); }), "a refused tune throws");
    expect(name, close_to(dev->getFrequency(SOAPY_SDR_RX, 0, "RF"), 1000000.0),
           "the frequency read back is the one the device has");
}

/* With no stream running, a rate is written to the device at once: a refused one throws and the rate read back stays
 * the one the device has; an accepted one is the rate read back. */
void
idle_rate_is_applied_or_reported(void) {
    const char* name = "idle_rate_is_applied_or_reported";
    std::unique_ptr<SoapyAirspyHF> dev = open_device(name);
    if (!dev) {
        return;
    }
    expect(name, !throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 912000.0); }), "an accepted rate");
    expect(name, g_stub.rate_hz == 912000U, "the device was given the rate");
    g_stub.fail_set_samplerate = 1;
    expect(name, throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 384000.0); }), "a refused rate throws");
    expect(name, close_to(dev->getSampleRate(SOAPY_SDR_RX, 0), 912000.0),
           "the rate read back is the one the device has");
}

/* An activation whose rate write or start fails returns an error, and a later one works. */
void
failed_activation_is_reported(void) {
    const char* name = "failed_activation_is_reported";
    std::unique_ptr<SoapyAirspyHF> dev = open_device(name);
    if (!dev) {
        return;
    }
    SoapySDR::Stream* stream = setup_stream(*dev);
    g_stub.fail_set_samplerate = 1;
    expect(name, dev->activateStream(stream) < 0, "an activation whose rate write fails returns an error");
    expect(name, g_stub.starts == 0, "no stream started on a rate the device refused");
    g_stub.fail_set_samplerate = 0;
    g_stub.fail_start = 1;
    expect(name, dev->activateStream(stream) < 0, "an activation whose start fails returns an error");
    g_stub.fail_start = 0;
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
    std::unique_ptr<SoapyAirspyHF> dev = open_device(name);
    if (!dev) {
        return;
    }
    SoapySDR::Stream* stream = setup_stream(*dev);
    expect(name, dev->activateStream(stream) == 0, "activation");
    const uint32_t streaming_rate = g_stub.rate_hz;
    g_stub.fail_set_samplerate = 1;
    expect(name, throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 384000.0); }), "a refused rate throws");
    expect(name, close_to(dev->getSampleRate(SOAPY_SDR_RX, 0), (double)streaming_rate),
           "the rate read back is the one the stream runs at");
    expect(name, g_stub.streaming == 1, "the stream runs on");
    g_stub.fail_set_samplerate = 0;
    expect(name, !throws([&] { dev->setSampleRate(SOAPY_SDR_RX, 0, 384000.0); }), "an accepted rate");
    expect(name, g_stub.rate_hz == 384000U && g_stub.streaming == 1, "the stream runs at the new rate");
    expect(name, close_to(dev->getSampleRate(SOAPY_SDR_RX, 0), 384000.0), "the new rate is read back");
    expect(name, dev->deactivateStream(stream) == 0, "deactivation");
    dev->closeStream(stream);
}

/* A stream set up again while it runs leaves its rate for the next read to apply; a read whose rate write fails
 * returns an error instead of waiting for samples. */
void
read_with_refused_pending_rate_is_reported(void) {
    const char* name = "read_with_refused_pending_rate_is_reported";
    std::unique_ptr<SoapyAirspyHF> dev = open_device(name);
    if (!dev) {
        return;
    }
    SoapySDR::Stream* stream = setup_stream(*dev);
    expect(name, dev->activateStream(stream) == 0, "activation");
    stream = setup_stream(*dev);
    g_stub.fail_set_samplerate = 1;
    std::vector<float> samples(2 * 64);
    void* buffs[] = {samples.data()};
    int flags = 0;
    long long time_ns = 0;
    expect(name, dev->readStream(stream, buffs, 64, flags, time_ns, 1000) == SOAPY_SDR_STREAM_ERROR,
           "a read whose rate write fails returns a stream error");
    g_stub.fail_set_samplerate = 0;
    expect(name, dev->deactivateStream(stream) == 0, "deactivation");
    dev->closeStream(stream);
}

/* A refused attenuator, LNA or AGC write throws, and the gain read back stays the one the device has. */
void
refused_gain_is_reported(void) {
    const char* name = "refused_gain_is_reported";
    std::unique_ptr<SoapyAirspyHF> dev = open_device(name);
    if (!dev) {
        return;
    }
    expect(name, dev->listGains(SOAPY_SDR_RX, 0).size() == 2, "the gain controls are there");
    expect(name, !throws([&] { dev->setGain(SOAPY_SDR_RX, 0, "RF", -12.0); }), "an accepted attenuation");
    g_stub.fail_att = 1;
    expect(name, throws([&] { dev->setGain(SOAPY_SDR_RX, 0, "RF", -24.0); }), "a refused attenuation throws");
    expect(name, close_to(dev->getGain(SOAPY_SDR_RX, 0, "RF"), -12.0), "the attenuation read back is the device's");
    expect(name, !throws([&] { dev->setGain(SOAPY_SDR_RX, 0, "LNA", 6.0); }), "an accepted LNA setting");
    g_stub.fail_lna = 1;
    expect(name, throws([&] { dev->setGain(SOAPY_SDR_RX, 0, "LNA", 0.0); }), "a refused LNA setting throws");
    expect(name, close_to(dev->getGain(SOAPY_SDR_RX, 0, "LNA"), 6.0), "the LNA gain read back is the device's");
    expect(name, !throws([&] { dev->setGainMode(SOAPY_SDR_RX, 0, false); }), "AGC off");
    g_stub.fail_agc = 1;
    expect(name, throws([&] { dev->setGainMode(SOAPY_SDR_RX, 0, true); }), "a refused AGC setting throws");
    expect(name, !dev->getGainMode(SOAPY_SDR_RX, 0), "the AGC mode read back is the device's");
}

/* The open sets the LNA and AGC once the attenuator answers. When either is refused the open fails and closes the
 * device, rather than reporting gains the device does not have. */
void
refused_open_gain_closes_the_device(void) {
    const char* name = "refused_open_gain_closes_the_device";
    for (int which = 0; which < 2; ++which) {
        g_stub = StubDevice();
        g_stub.fail_lna = which == 0;
        g_stub.fail_agc = which == 1;
        expect(name, throws([] {
                   const SoapySDR::Kwargs args;
                   SoapyAirspyHF dev(args);
               }),
               "the open throws");
        expect(name, g_stub.open_handles == 0, "the device was closed");
    }
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
    refused_open_gain_closes_the_device();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d expectation(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
