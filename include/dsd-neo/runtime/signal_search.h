// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/** @file Frontend-neutral signal search records and bounded frequency planning. */
#ifndef DSD_NEO_RUNTIME_SIGNAL_SEARCH_H
#define DSD_NEO_RUNTIME_SIGNAL_SEARCH_H

#include <dsd-neo/core/airspy_config.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_SEARCH_MAX_RANGES = 16,
    DSD_SEARCH_MAX_CHANNELS = 100000,
    DSD_SEARCH_MAX_CANDIDATES = 512,
    DSD_SEARCH_MAX_AVOIDS = 1024,
    DSD_SEARCH_HISTORY_LIMIT = 1000,
    DSD_SEARCH_MAX_FFT = 65536
};

typedef enum {
    DSD_SEARCH_STOPPED,
    DSD_SEARCH_SEARCHING,
    DSD_SEARCH_TUNING,
    DSD_SEARCH_IDENTIFYING,
    DSD_SEARCH_LISTENING,
    DSD_SEARCH_HELD,
    DSD_SEARCH_PAUSED,
    DSD_SEARCH_ERROR
} dsd_signal_search_phase;

typedef struct {
    char name[64];
    uint32_t start_hz, end_hz;
    /** Exact rational Hz, e.g. 25000/3. Frequencies are rounded once from the origin. */
    uint32_t step_num, step_den;
    uint32_t bandwidth_hz;
    int enabled;
    int am;
    /** 0: analog only; 1: explicit Auto; 2..9: scan_mode's P25..M17 plus one. */
    int digital;
    int gain_set, gain_tenth_db, gain_auto;
    int airspy_gain_set;
    dsd_airspy_config airspy;
    int sensitivity_db;
    int minimum_set, minimum_dbfs;
    int squelch_mode, squelch_margin_db, squelch_dbfs;
    int probe_ms, max_visit_ms;
} dsd_signal_search_range;

typedef struct {
    uint64_t session_id, hit_id;
    uint64_t started_ms, ended_ms;
    uint32_t frequency_hz, bandwidth_hz;
    uint32_t step_num, step_den;
    uint32_t range_index;
    float power_dbfs, noise_dbfs;
    int confirmed, ended, am, unresolved;
    int gain_tenth_db, gain_auto;
    char range_name[64];
    char protocol[24];
    char access[64];
    char tone[64];
    uint32_t target, source;
    char target_text[64], source_text[64];
    /** Applied source settings, copied at detection time for history and save-as-system. */
    char input_spec[1024];
    int ppm, bias_tee;
    dsd_airspy_config airspy;
} dsd_signal_search_hit;

typedef struct {
    uint64_t session_id, revision, pass;
    dsd_signal_search_phase phase;
    uint32_t range_index, channel_index, channel_count;
    uint32_t capture_rate_hz;
    int identifying;
    /** 1 while a coordinator owns the tuner; 0 for a rejected start or a stopped session. */
    int owned;
    /** Current-channel RF power while listening, on the detector's dBFS scale; detection power stays in hit. */
    int live_power_valid;
    float live_power_dbfs;
    uint64_t measurement_latency_us;
    dsd_signal_search_hit hit;
    char message[192];
} dsd_signal_search_status;

typedef struct {
    uint32_t tune_hz;
    uint32_t first, count, next;
    uint32_t frequencies[DSD_SEARCH_MAX_CANDIDATES];
} dsd_signal_search_span;

typedef struct {
    uint64_t session_id;
    uint32_t range_count, avoid_count;
    dsd_signal_search_range ranges[DSD_SEARCH_MAX_RANGES];
    uint32_t avoids[DSD_SEARCH_MAX_AVOIDS];
} dsd_signal_search_config;

typedef enum {
    DSD_SEARCH_ACTION_STOP,
    DSD_SEARCH_ACTION_PAUSE,
    DSD_SEARCH_ACTION_RESUME,
    DSD_SEARCH_ACTION_HOLD,
    DSD_SEARCH_ACTION_SKIP,
    DSD_SEARCH_ACTION_AVOID,
    DSD_SEARCH_ACTION_AVOID_PERMANENT,
    /** Lift a session avoid by frequency_hz; not tied to a hit. */
    DSD_SEARCH_ACTION_UNAVOID
} dsd_signal_search_action;

typedef struct {
    uint64_t session_id, hit_id;
    int action;
    uint32_t frequency_hz;
} dsd_signal_search_control;

void dsd_signal_search_range_default(dsd_signal_search_range* range);
int dsd_signal_search_validate(const dsd_signal_search_range* range, char* error, size_t capacity);
/** Every range valid, at least one enabled, and at most DSD_SEARCH_MAX_CHANNELS enabled channels in total.
 * Shared by the engine and frontends so a start the engine would reject is refused before submission. */
int dsd_signal_search_validate_config(const dsd_signal_search_config* config, char* error, size_t capacity);
uint32_t dsd_signal_search_channel_count(const dsd_signal_search_range* range);
uint32_t dsd_signal_search_frequency(const dsd_signal_search_range* range, uint32_t index);
int dsd_signal_search_fft_size(uint32_t rate, const dsd_signal_search_range* range);
/** Plan contiguous channels in a usable RF lobe. Hardware DC and edges are never used.
 * capture_offset is the hardware centre minus the requested frequency, including Fs/4/edge tuning.
 * Returns 1 for a span, 0 at end, -1 if the rate/offset cannot cover this range. */
int dsd_signal_search_plan(const dsd_signal_search_range* range, uint32_t first, uint32_t rate, int64_t capture_offset,
                           dsd_signal_search_span* span);

#ifdef __cplusplus
}
#endif
#endif
