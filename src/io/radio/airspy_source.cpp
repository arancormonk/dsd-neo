// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/runtime/log.h>
#include <stdint.h>
#include "airspy_source.h"

#ifdef USE_AIRSPY
#include <airspy.h>
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/airspy_config.h>
#include <dsd-neo/runtime/input_failure.h>
#include <new>
struct airspy_device;

#if defined(__ANDROID__) && defined(USE_AIRSPY_OPEN_FD)
static std::atomic<int> usb_fd{-1};
#endif
static std::atomic<int> usb_in_use{0};

struct airspy_source {
    airspy_device* device = nullptr;
    dsd_mutex_t lock{};
    airspy_source_callback callback = nullptr;
    void* context = nullptr;
    dsd_airspy_config config{};
    dsd_airspy_info info{};
    std::atomic<uint64_t> drops{0};
    uint32_t frequency = 0;
    bool started = false;
    bool owns_fd = false;
};

namespace {
class AirspyLock {
    dsd_mutex_t* mutex;

  public:
    explicit AirspyLock(dsd_mutex_t* m) : mutex(m) { dsd_mutex_lock(mutex); }

    ~AirspyLock() { dsd_mutex_unlock(mutex); }

    AirspyLock(const AirspyLock&) = delete;
    AirspyLock& operator=(const AirspyLock&) = delete;
};
} // namespace

static int
checked(int result, const char* operation) {
    if (result != AIRSPY_SUCCESS) {
        LOG_ERROR("Airspy %s: %s (%d)\n", operation, airspy_error_name((airspy_error)result), result);
    }
    return result;
}

static int
receive(airspy_transfer* transfer) {
    if (!transfer || !transfer->ctx || !transfer->samples || transfer->sample_count <= 0
        || transfer->sample_type != AIRSPY_SAMPLE_FLOAT32_IQ) {
        return -1;
    }
    auto* s = static_cast<airspy_source*>(transfer->ctx);
    s->drops.fetch_add(transfer->dropped_samples, std::memory_order_relaxed);
    s->callback(s->context, static_cast<const float*>(transfer->samples), (size_t)transfer->sample_count,
                transfer->dropped_samples);
    return 0;
}

static int
controls(airspy_source* s, const dsd_airspy_config* c) {
    int rc;
    if (c->gain_mode == DSD_AIRSPY_SENSITIVITY) {
        rc = airspy_set_sensitivity_gain(s->device, (uint8_t)c->sensitivity_gain);
    } else if (c->gain_mode == DSD_AIRSPY_LINEARITY) {
        rc = airspy_set_linearity_gain(s->device, (uint8_t)c->linearity_gain);
    } else {
        rc = airspy_set_lna_agc(s->device, (uint8_t)c->lna_agc);
        if (rc == 0) {
            rc = airspy_set_mixer_agc(s->device, (uint8_t)c->mixer_agc);
        }
        if (rc == 0 && !c->lna_agc) {
            rc = airspy_set_lna_gain(s->device, (uint8_t)c->lna_gain);
        }
        if (rc == 0 && !c->mixer_agc) {
            rc = airspy_set_mixer_gain(s->device, (uint8_t)c->mixer_gain);
        }
        if (rc == 0) {
            rc = airspy_set_vga_gain(s->device, (uint8_t)c->vga_gain);
        }
    }
    if (rc == 0) {
        rc = airspy_set_rf_bias(s->device, (uint8_t)c->bias_tee);
    }
    return checked(rc, "controls");
}

int
airspy_source_list(uint64_t* serials, int capacity) {
    return airspy_list_devices(serials, capacity);
}

static int
open_device(airspy_source* s, uint64_t serial) {
#if defined(__ANDROID__) && defined(USE_AIRSPY_OPEN_FD)
    (void)serial;
    const int fd = usb_fd.load(std::memory_order_acquire);
    if (fd < 0) {
        return AIRSPY_ERROR_NOT_FOUND;
    }
    usb_in_use.store(1, std::memory_order_release);
    s->owns_fd = true;
    return airspy_open_fd(&s->device, fd);
#else
    if (s->config.serial[0]) {
        return airspy_open_sn(&s->device, serial);
    }
    uint64_t serials[256];
    int count = airspy_list_devices(serials, 256);
    return count > 0 ? airspy_open_sn(&s->device, serials[0]) : AIRSPY_ERROR_NOT_FOUND;
#endif
}

static int
read_identity(airspy_source* s, uint64_t requested_serial) {
    airspy_read_partid_serialno_t id{};
    int rc = airspy_board_partid_serialno_read(s->device, &id);
    if (rc != 0) {
        return rc;
    }
    uint64_t serial = ((uint64_t)id.serial_no[2] << 32) | id.serial_no[3];
    DSD_SNPRINTF(s->info.serial, sizeof s->info.serial, "%016" PRIX64, serial);
    return s->config.serial[0] && serial != requested_serial ? AIRSPY_ERROR_NOT_FOUND : 0;
}

static int
read_rates(airspy_source* s) {
    uint32_t count = 0;
    int rc = airspy_get_samplerates(s->device, &count, 0);
    if (rc != 0) {
        return rc;
    }
    if (count == 0 || count > DSD_AIRSPY_MAX_RATES) {
        return AIRSPY_ERROR_INVALID_PARAM;
    }
    uint32_t rates[DSD_AIRSPY_MAX_RATES];
    rc = airspy_get_samplerates(s->device, rates, count);
    if (rc != 0) {
        return rc;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (rates[i] >= 225000 && rates[i] <= DSD_AIRSPY_MAX_RATE) {
            s->info.rates[s->info.rate_count++] = rates[i];
        }
    }
    std::sort(s->info.rates, s->info.rates + s->info.rate_count);
    return s->info.rate_count ? 0 : AIRSPY_ERROR_INVALID_PARAM;
}

static int
configure_device(airspy_source* s, uint64_t serial) {
    int rc = read_identity(s, serial);
    if (rc == 0) {
        rc = read_rates(s);
    }
    if (rc != 0) {
        return rc;
    }
    uint32_t rate = s->config.sample_rate;
    if (!rate) {
#ifdef __ANDROID__
        rate = s->info.rates[0];
#else
        rate = s->info.rates[s->info.rate_count - 1];
#endif
    }
    rc = airspy_set_sample_type(s->device, AIRSPY_SAMPLE_FLOAT32_IQ);
    if (rc == 0) {
        rc = airspy_source_rate(s, rate);
    }
    if (rc == 0) {
        rc = controls(s, &s->config);
    }
    return rc;
}

airspy_source*
// cppcheck-suppress funcArgNamesDifferentUnnamed -- Both declarations name the context; callback typedef confuses cppcheck.
airspy_source_open(const dsd_airspy_config* config, airspy_source_callback callback, void* context) {
    if (!dsd_airspy_config_valid(config) || !callback) {
        return nullptr;
    }
    uint64_t serial = 0;
    if (config->serial[0] && dsd_parse_hex_u64_n(config->serial, 16, &serial) != 0) {
        return nullptr;
    }
    auto* s = new (std::nothrow) airspy_source;
    if (!s) {
        return nullptr;
    }
    if (dsd_mutex_init(&s->lock) != 0) {
        delete s;
        return nullptr;
    }
    s->callback = callback;
    s->context = context;
    s->config = *config;
    int rc = checked(open_device(s, serial), "open");
    if (rc == 0) {
        rc = checked(configure_device(s, serial), "configure");
    }
    if (rc != 0) {
        dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, rc);
        airspy_source_close(s);
        return nullptr;
    }
    dsd_input_failure_clear();
    LOG_INFO("Airspy %s: %u samples/s, float IQ\n", s->info.serial, s->info.sample_rate);
    return s;
}

int
airspy_source_start(airspy_source* s) {
    if (!s) {
        return -1;
    }
    AirspyLock guard(&s->lock);
    if (s->started) {
        return -1;
    }
    int rc = checked(airspy_start_rx(s->device, receive, s), "start");
    if (rc != 0) {
        // The SDK can leave submitted transfers active after thread creation fails.
        (void)checked(airspy_stop_rx(s->device), "stop after failed start");
    }
    s->started = rc == 0;
    return rc;
}

int
airspy_source_stop(airspy_source* s) {
    if (!s) {
        return -1;
    }
    AirspyLock guard(&s->lock);
    int rc = s->started ? checked(airspy_stop_rx(s->device), "stop") : 0;
    s->started = false;
    return rc;
}

void
airspy_source_close(airspy_source* s) {
    if (!s) {
        return;
    }
    if (s->device) {
        (void)airspy_source_stop(s);
        (void)airspy_set_rf_bias(s->device, 0);
        (void)airspy_close(s->device);
    }
    if (s->owns_fd) {
        usb_in_use.store(0, std::memory_order_release);
    }
    dsd_mutex_destroy(&s->lock);
    delete s;
}

int
airspy_source_running(airspy_source* s) {
    if (!s) {
        return 0;
    }
    AirspyLock guard(&s->lock);
    return s->started && airspy_is_streaming(s->device) == AIRSPY_TRUE;
}

/* libairspy queues raw USB transfers internally. Stop joins its consumer before
 * programming a new frequency, and start resets those queues and its IQ filter. */
static int
reconfigure(airspy_source* s, uint32_t value, bool rate) {
    AirspyLock guard(&s->lock);
    uint32_t previous = rate ? s->info.sample_rate : s->frequency;
    if (value == previous) {
        return 0;
    }
    bool restart = s->started;
    int rc = restart ? airspy_stop_rx(s->device) : 0;
    if (rc != 0) {
        return checked(rc, "stop for retune");
    }
    rc = rate ? airspy_set_samplerate(s->device, value) : airspy_set_freq(s->device, value);
    if (rc == 0) {
        if (rate) {
            s->info.sample_rate = value;
        } else {
            s->frequency = value;
        }
    } else if (previous) {
        int rollback = rate ? airspy_set_samplerate(s->device, previous) : airspy_set_freq(s->device, previous);
        if (rollback != 0) {
            /* The stream monitor must fail the session rather than accept frames
             * whose hardware frequency/rate can no longer be established. */
            s->started = false;
            return checked(rollback, "retune rollback");
        }
    }
    if (restart) {
        int start_rc = checked(airspy_start_rx(s->device, receive, s), "restart");
        if (start_rc != 0) {
            (void)checked(airspy_stop_rx(s->device), "stop after failed restart");
            s->started = false;
            rc = start_rc;
        }
    }
    return checked(rc, rate ? "sample rate" : "frequency");
}

int
airspy_source_frequency(airspy_source* s, uint32_t frequency) {
    if (!s || frequency < 24000000U || frequency > 1700000000U) {
        return -1;
    }
    return reconfigure(s, frequency, false);
}

int
airspy_source_rate(airspy_source* s, uint32_t rate) {
    if (!s
        || std::find(s->info.rates, s->info.rates + s->info.rate_count, rate) == s->info.rates + s->info.rate_count) {
        return -1;
    }
    return reconfigure(s, rate, true);
}

int
airspy_source_controls(airspy_source* s, const dsd_airspy_config* config) {
    if (!s || !dsd_airspy_config_valid(config)) {
        return -1;
    }
    AirspyLock guard(&s->lock);
    int rc = controls(s, config);
    if (rc == 0) {
        s->config = *config;
    } else if (controls(s, &s->config) != 0) {
        (void)airspy_stop_rx(s->device);
        s->started = false;
    }
    return rc;
}

int
airspy_source_info(airspy_source* s, dsd_airspy_info* info) {
    if (!s || !info) {
        return -1;
    }
    AirspyLock guard(&s->lock);
    *info = s->info;
    info->dropped_samples = s->drops.load(std::memory_order_relaxed);
    return 0;
}

int
airspy_source_set_fd(int fd) {
#if defined(__ANDROID__) && defined(USE_AIRSPY_OPEN_FD)
    usb_fd.store(fd, std::memory_order_release);
    return 0;
#else
    return fd < 0 ? 0 : -1;
#endif
}

int
airspy_source_fd_in_use(void) {
    return usb_in_use.load(std::memory_order_acquire);
}

#else
airspy_source*
// cppcheck-suppress funcArgNamesDifferentUnnamed -- Both declarations name the context; callback typedef confuses cppcheck.
airspy_source_open(const dsd_airspy_config* config, airspy_source_callback callback, void* context) {
    (void)config;
    (void)callback;
    (void)context;
    LOG_ERROR("Native Airspy support is unavailable in this build. Enable DSD_ENABLE_AIRSPY and install libairspy.\n");
    return nullptr;
}

void
airspy_source_close(airspy_source* s) {
    (void)s;
}

int
airspy_source_start(airspy_source* s) {
    (void)s;
    return -1;
}

int
airspy_source_stop(airspy_source* s) {
    (void)s;
    return -1;
}

int
airspy_source_running(airspy_source* s) {
    (void)s;
    return 0;
}

int
airspy_source_frequency(airspy_source* s, uint32_t frequency) {
    (void)s;
    (void)frequency;
    return -1;
}

int
airspy_source_rate(airspy_source* s, uint32_t rate) {
    (void)s;
    (void)rate;
    return -1;
}

int
airspy_source_controls(airspy_source* s, const dsd_airspy_config* config) {
    (void)s;
    (void)config;
    return -1;
}

int
airspy_source_info(airspy_source* s, dsd_airspy_info* info) {
    (void)s;
    (void)info;
    return -1;
}

int
airspy_source_set_fd(int fd) {
    return fd < 0 ? 0 : -1;
}

int
airspy_source_fd_in_use(void) {
    return 0;
}

int
airspy_source_list(uint64_t* serials, int capacity) {
    (void)serials;
    (void)capacity;
    return -1;
}
#endif
