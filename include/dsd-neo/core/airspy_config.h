// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_CORE_AIRSPY_CONFIG_H
#define DSD_NEO_CORE_AIRSPY_CONFIG_H

#include <stdint.h>

/* Value types only: safe to copy into configuration and frontend snapshots. */
enum { DSD_AIRSPY_SENSITIVITY = 0, DSD_AIRSPY_LINEARITY = 1, DSD_AIRSPY_MANUAL = 2 };

enum { DSD_AIRSPY_MAX_RATES = 64, DSD_AIRSPY_MAX_RATE = 10000000 };

typedef struct {
    char serial[17];      /* Empty selects the first device; otherwise 16 hexadecimal digits. */
    uint32_t sample_rate; /* 0 = platform automatic policy; never replaced by the effective rate. */
    int gain_mode;
    int sensitivity_gain;
    int linearity_gain;
    int lna_gain;
    int mixer_gain;
    int vga_gain;
    int lna_agc;
    int mixer_agc;
    int bias_tee;
} dsd_airspy_config;

typedef struct {
    char serial[17];
    uint32_t sample_rate;
    uint32_t rates[DSD_AIRSPY_MAX_RATES];
    uint32_t rate_count;
    uint64_t dropped_samples;
} dsd_airspy_info;

static inline void
dsd_airspy_config_defaults(dsd_airspy_config* cfg) {
    const dsd_airspy_config defaults = {{0}, 0, DSD_AIRSPY_SENSITIVITY, 10, 10, 1, 5, 5, 0, 0, 0};
    *cfg = defaults;
}

#endif
