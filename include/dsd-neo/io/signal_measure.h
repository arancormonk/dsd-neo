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
/** The clip level of a source whose float samples reach full scale at 1.0 (Airspy, SoapySDR, a CF32 or CS16 replay),
 * and of a capture that sets none. */
#define DSD_SIGNAL_CLIP_LEVEL_DEFAULT 0.999f

typedef struct {
    uint32_t rate_hz, capture_hz, generation;
    /** Applied complex ingest rotation in Hz (positive j^n is +Fs/4). */
    int32_t rotation_hz;
    int64_t capture_offset_hz;
    uint64_t drops;
    int gain;
    /** A sample pair counts as clipped when either component's magnitude is at or above this level. Set by the source
     * that knows its sample conversion (rtl_device_signal_clip_level()); 0 means DSD_SIGNAL_CLIP_LEVEL_DEFAULT. A
     * property of the conversion, not a capture change: reports are not invalidated by it. */
    float clip_level;
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

/** Why a measurement failed. */
typedef enum {
    DSD_SIGNAL_MEASURE_OK = 0,
    /** More than 1% of the sample pairs measured so far reached the capture's clip level. */
    DSD_SIGNAL_MEASURE_CLIPPED,
    /** The input ring dropped samples (dsd_signal_capture.drops moved) and nothing else about the capture changed. */
    DSD_SIGNAL_MEASURE_DROPPED,
    /** The capture changed: retune, rate, rotation, offset, gain or output generation; or a completed report waiting
     * to be read was invalidated by any capture change. */
    DSD_SIGNAL_MEASURE_CAPTURE_CHANGED,
    /** A sample was NaN or infinite. */
    DSD_SIGNAL_MEASURE_NONFINITE,
    /** The transform could not run. */
    DSD_SIGNAL_MEASURE_FAILED,
} dsd_signal_measure_failure;

typedef struct {
    uint64_t session_id, request_id;
    uint32_t generation, count;
    uint64_t sample_count;
    /** Monotonic arrival of the capture block that completes (or fails) this report; shared tap only. */
    uint64_t received_realtime_ns;
    /** DSD_SIGNAL_MEASURE_OK for a complete report. A failed one keeps its request's ids and generation, the samples
     * taken before the failure, and no candidates (count 0). */
    dsd_signal_measure_failure failure;
    dsd_signal_measure_candidate candidates[DSD_SEARCH_MAX_CANDIDATES];
} dsd_signal_measure_report;

/** Why rtl_signal_measure_request() refused a request. */
typedef enum {
    DSD_SIGNAL_REQUEST_OK = 0,
    /** The request's FFT plan is not supported (rate, bandwidth or candidate count). */
    DSD_SIGNAL_REQUEST_UNSUPPORTED,
    /** The detector could not be allocated. */
    DSD_SIGNAL_REQUEST_NO_MEMORY,
} dsd_signal_request_failure;
typedef struct dsd_signal_detector dsd_signal_detector;
dsd_signal_detector* dsd_signal_detector_create(void);
void dsd_signal_detector_destroy(dsd_signal_detector* detector);
int dsd_signal_detector_begin(dsd_signal_detector* detector, const dsd_signal_measure_request* request);
/** Sample-count accumulation across arbitrary complete IQ-pair blocks. Returns 1 once per request.
 * A discontinuity, clipping or a non-finite sample invalidates the entire request and returns -1, never a partial
 * detection; @p report then carries the request's ids and the reason (dsd_signal_measure_report.failure). */
int dsd_signal_detector_feed(dsd_signal_detector* detector, const float* iq, size_t pairs,
                             const dsd_signal_capture* capture, dsd_signal_measure_report* report);
/** Shared radio tap. The demod thread feeds it under the capture reconfiguration gate. Requests and cancels wait for a
 * feed in progress; reading the report and the capture never waits for its analysis. A report is delivered only for the
 * request outstanding when it completed, and never after a cancel or a newer request returns.
 * A refused request returns -1 with the reason in @p out_failure (optional). */
int rtl_signal_measure_request(const dsd_signal_measure_request* request, dsd_signal_request_failure* out_failure);
/** 1 with a complete report, -1 with a failed one (its reason and ids: the first failure since the request), 0 with
 * none yet (@p report untouched). */
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
