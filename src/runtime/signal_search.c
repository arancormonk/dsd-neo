// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/airspy_config.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stdint.h>
#include <string.h>

void
dsd_signal_search_range_default(dsd_signal_search_range* range) {
    if (!range) {
        return;
    }
    DSD_MEMSET(range, 0, sizeof(*range));
    dsd_airspy_config_defaults(&range->airspy);
    DSD_SNPRINTF(range->name, sizeof(range->name), "%s", "UHF");
    range->enabled = 1;
    range->start_hz = 450000000;
    range->end_hz = 470000000;
    range->step_num = 12500;
    range->step_den = 1;
    range->bandwidth_hz = 16000;
    range->digital = 1;
    range->sensitivity_db = 15;
    range->minimum_dbfs = -60;
    range->squelch_mode = DSD_SQUELCH_MODE_NOISE;
    range->squelch_margin_db = 10;
    range->squelch_dbfs = -60;
    range->probe_ms = 2000;
    range->max_visit_ms = 30000;
}

uint32_t
dsd_signal_search_channel_count(const dsd_signal_search_range* range) {
    if (!range || !range->step_num || !range->step_den || range->end_hz < range->start_hz) {
        return 0;
    }
    uint64_t n = ((uint64_t)(range->end_hz - range->start_hz) * range->step_den) / range->step_num + 1;
    return n <= DSD_SEARCH_MAX_CHANNELS ? (uint32_t)n : 0;
}

uint32_t
dsd_signal_search_frequency(const dsd_signal_search_range* range, uint32_t index) {
    if (!range || index >= dsd_signal_search_channel_count(range)) {
        return 0;
    }
    return range->start_hz + (uint32_t)(((uint64_t)index * range->step_num + range->step_den / 2) / range->step_den);
}

static const char*
validate_grid(const dsd_signal_search_range* range) {
    if (!range || !memchr(range->name, '\0', sizeof(range->name))) {
        return "Invalid range name";
    }
    if (!range->start_hz || range->end_hz < range->start_hz) {
        return "End frequency must follow start frequency";
    }
    if (!range->step_den || range->step_den > 1000 || !range->step_num || range->step_num > 100000000
        || range->step_num < range->step_den || !dsd_signal_search_channel_count(range)) {
        return "Range must contain 1 to 100000 channels with a positive rational step";
    }
    if (!dsd_analog_width_in_range(range->am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM, (int)range->bandwidth_hz)) {
        return "Channel width must be 8000 to 25000 Hz for NFM, or 5000 to 20000 Hz for AM";
    }
    return NULL;
}

static const char*
validate_receiver(const dsd_signal_search_range* range) {
    if (range->sensitivity_db < 3 || range->sensitivity_db > 30 || range->minimum_dbfs < -160
        || range->minimum_dbfs > 3) {
        return "RF sensitivity must be 3 to 30 dB and minimum level -160 to 3 dBFS";
    }
    if (range->gain_tenth_db < -100 || range->gain_tenth_db > 1000) {
        return "Invalid tuner gain";
    }
    if (range->airspy_gain_set && !dsd_airspy_config_valid(&range->airspy)) {
        return "Invalid Airspy gain controls";
    }
    if (range->digital < 0 || range->digital > 9 || (range->am && range->digital)) {
        return "AM requires analog-only monitoring";
    }
    return NULL;
}

static const char*
validate_listening(const dsd_signal_search_range* range) {
    if (range->probe_ms < 2000 || range->probe_ms > 10000 || range->max_visit_ms < 0 || range->max_visit_ms > 600000) {
        return "Probe must be 2 to 10 seconds; maximum visit 0 to 600 seconds";
    }
    if (range->squelch_mode < DSD_SQUELCH_MODE_LEVEL || range->squelch_mode > DSD_SQUELCH_MODE_NOISE
        || (range->am && range->squelch_mode == DSD_SQUELCH_MODE_NOISE) || range->squelch_margin_db < 0
        || range->squelch_margin_db > DSD_SQUELCH_MARGIN_MAX_DB || range->squelch_dbfs < -160
        || range->squelch_dbfs > 0) {
        return "Invalid listening squelch for this modulation";
    }
    /* The receiver clamps a dynamic squelch's margin into this range; a saved margin outside it would not be the one
       applied. Level mode has no margin. */
    if (dsd_squelch_mode_is_dynamic(range->squelch_mode) && range->squelch_margin_db < DSD_SQUELCH_MARGIN_MIN_DB) {
        return "Squelch margin must be 3 to 30 dB";
    }
    return NULL;
}

int
dsd_signal_search_validate(const dsd_signal_search_range* range, char* error, size_t capacity) {
    const char* why = validate_grid(range);
    if (!why) {
        why = validate_receiver(range);
    }
    if (!why) {
        why = validate_listening(range);
    }
    if (error && capacity) {
        DSD_SNPRINTF(error, capacity, "%s", why ? why : "");
    }
    return why ? -1 : 0;
}

static int
search_config_error(char* error, size_t capacity, const char* why) {
    if (error && capacity) {
        DSD_SNPRINTF(error, capacity, "%s", why);
    }
    return -1;
}

int
dsd_signal_search_validate_config(const dsd_signal_search_config* config, char* error, size_t capacity) {
    if (!config || !config->range_count || config->range_count > DSD_SEARCH_MAX_RANGES
        || config->avoid_count > DSD_SEARCH_MAX_AVOIDS) {
        return search_config_error(error, capacity, "Invalid signal search configuration");
    }
    int enabled = 0;
    uint64_t total_channels = 0;
    for (uint32_t i = 0; i < config->range_count; ++i) {
        if (dsd_signal_search_validate(&config->ranges[i], error, capacity)) {
            return -1;
        }
        if (config->ranges[i].enabled) {
            ++enabled;
            total_channels += dsd_signal_search_channel_count(&config->ranges[i]);
        }
    }
    if (!enabled) {
        return search_config_error(error, capacity, "Enable at least one range");
    }
    if (total_channels > DSD_SEARCH_MAX_CHANNELS) {
        return search_config_error(error, capacity, "A search supports up to 100,000 enabled channels");
    }
    if (error && capacity) {
        error[0] = '\0';
    }
    return 0;
}

int
dsd_signal_search_fft_size(uint32_t rate, const dsd_signal_search_range* range) {
    if (!rate || dsd_signal_search_validate(range, NULL, 0)) {
        return 0;
    }
    double width = (double)range->step_num / range->step_den;
    if (width > range->bandwidth_hz) {
        width = range->bandwidth_hz;
    }
    int n = 32;
    while ((double)rate * 8 > width * n && n < DSD_SEARCH_MAX_FFT) {
        n *= 2;
    }
    return (double)rate * 8 <= width * n ? n : 0;
}

int
dsd_signal_search_plan(const dsd_signal_search_range* range, uint32_t first, uint32_t rate, int64_t capture_offset,
                       dsd_signal_search_span* span) {
    if (!span || !dsd_signal_search_fft_size(rate, range)) {
        return -1;
    }
    DSD_MEMSET(span, 0, sizeof(*span));
    uint32_t total = dsd_signal_search_channel_count(range);
    if (first >= total) {
        return 0;
    }
    /* Four channel widths leave room for the noise sidebands; an additional 1% protects DC.
       Always place the first unvisited channel in this lobe, including DC/edge holes of earlier spans. */
    int64_t margin = (int64_t)range->bandwidth_hz * 4;
    int64_t width = (int64_t)rate * 4 / 10 - 2 * margin - rate / 100;
    if (width < 0) {
        return -1;
    }
    int64_t low = dsd_signal_search_frequency(range, first);
    int64_t capture = low + (int64_t)rate * 4 / 10 - margin;
    int64_t tune = capture - capture_offset;
    if (tune <= 0 || tune > UINT32_MAX || capture > UINT32_MAX) {
        return -1;
    }
    span->tune_hz = (uint32_t)tune;
    span->first = first;
    while (first < total && span->count < DSD_SEARCH_MAX_CANDIDATES) {
        uint32_t f = dsd_signal_search_frequency(range, first);
        if ((int64_t)f > low + width) {
            break;
        }
        span->frequencies[span->count++] = f;
        ++first;
    }
    span->next = first;
    return 1;
}
