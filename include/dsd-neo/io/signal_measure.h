// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/** @file Capture-rate search measurements. No rendering or decoder state. */
#ifndef DSD_NEO_IO_SIGNAL_MEASURE_H
#define DSD_NEO_IO_SIGNAL_MEASURE_H
#include <dsd-neo/runtime/signal_search.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t rate_hz, capture_hz, generation;
    /** Applied complex ingest rotation in Hz (positive j^n is +Fs/4). */
    int32_t rotation_hz;
    int64_t capture_offset_hz;
    uint64_t drops;
    int gain;
} dsd_signal_capture;

typedef struct {
    uint64_t session_id, request_id;
    dsd_signal_capture capture;
    dsd_signal_search_range range;
    uint32_t count;
    uint32_t frequencies[DSD_SEARCH_MAX_CANDIDATES];
} dsd_signal_measure_request;

typedef struct {
    uint32_t frequency_hz;
    float power_dbfs, noise_dbfs;
    int detected, valid;
} dsd_signal_measure_candidate;

typedef struct {
    uint64_t session_id, request_id;
    uint32_t generation, count;
    uint64_t sample_count;
    /** Monotonic arrival of the capture block that completes this report; shared tap only. */
    uint64_t received_realtime_ns;
    dsd_signal_measure_candidate candidates[DSD_SEARCH_MAX_CANDIDATES];
} dsd_signal_measure_report;
typedef struct dsd_signal_detector dsd_signal_detector;
dsd_signal_detector* dsd_signal_detector_create(void);
void dsd_signal_detector_destroy(dsd_signal_detector* detector);
int dsd_signal_detector_begin(dsd_signal_detector* detector, const dsd_signal_measure_request* request);
/** Sample-count accumulation across arbitrary complete IQ-pair blocks. Returns 1 once per request.
 * A discontinuity invalidates the entire request and returns -1, never a partial detection. */
int dsd_signal_detector_feed(dsd_signal_detector* detector, const float* iq, size_t pairs,
                             const dsd_signal_capture* capture, dsd_signal_measure_report* report);
/** Shared radio tap. The demod thread feeds it under the capture reconfiguration gate. Requests and cancels wait for a
 * feed in progress; reading the report and the capture never waits for its analysis. A report is delivered only for the
 * request outstanding when it completed, and never after a cancel or a newer request returns. */
int rtl_signal_measure_request(const dsd_signal_measure_request* request);
int rtl_signal_measure_get(dsd_signal_measure_report* report);
void rtl_signal_measure_cancel(void);
/** The stream that fed the tap has stopped: forget its capture and the time of its latest samples, and retire any
 * request as a cancel does. Until the next stream's first block there is no capture, so a search started on that
 * stream waits for its samples and never reads the stopped stream's age as a stall. */
void rtl_signal_measure_reset(void);
void rtl_signal_search_bypass(int enabled);
int rtl_signal_search_bypassed(void);
/** Reserve tagged controller requests for one search session; zero releases the tuner. */
void rtl_signal_search_set_owner(uint64_t session);
uint64_t rtl_signal_search_owner(void);
int rtl_signal_capture_get(dsd_signal_capture* capture);
uint64_t rtl_signal_capture_last_ms(void);
void rtl_signal_measure_feed(const float* iq, size_t pairs, const dsd_signal_capture* capture);
#ifdef __cplusplus
}
#endif
#endif
