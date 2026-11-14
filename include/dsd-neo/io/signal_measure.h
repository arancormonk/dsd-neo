// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/** @file Capture-rate search and scan quiet measurements. No rendering or decoder state. */
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
    /** A scan quiet request was lost before it was taken: a Signal search request, a cancel, a reset or a newer quiet
     * request replaced it, or its result was already taken. */
    DSD_SIGNAL_MEASURE_SUPERSEDED,
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
 * A refused request returns -1 with the reason in @p out_failure (optional). A request, refused or not, supersedes a
 * scan quiet request (rtl_signal_quiet_request()). */
int rtl_signal_measure_request(const dsd_signal_measure_request* request, dsd_signal_request_failure* out_failure);
/** 1 with a complete report, -1 with a failed one (its reason and ids: the first failure since the request), 0 with
 * none yet (@p report untouched). */
int rtl_signal_measure_get(dsd_signal_measure_report* report);
void rtl_signal_measure_cancel(void);
/** The stream that fed the tap has stopped: forget its capture and the time of its latest samples, and retire any
 * request as a cancel does, a scan quiet request included. Until the next stream's first block there is no capture, so
 * a search started on that stream waits for its samples and never reads the stopped stream's age as a stall. */
void rtl_signal_measure_reset(void);
void rtl_signal_search_bypass(int enabled);
int rtl_signal_search_bypassed(void);
/** Reserve tagged controller requests for one search session; zero releases the tuner. */
void rtl_signal_search_set_owner(uint64_t session);
uint64_t rtl_signal_search_owner(void);
int rtl_signal_capture_get(dsd_signal_capture* capture);
uint64_t rtl_signal_capture_last_ms(void);
void rtl_signal_measure_feed(const float* iq, size_t pairs, const dsd_signal_capture* capture);

/* Scan quiet measurement: whether one landed channel carries no power above its local noise floor over
 * DSD_SIGNAL_QUIET_WINDOWS back-to-back sub-windows, each the smallest whole number of transforms of at least 10 ms
 * (at least 90 ms in all), judged in every requested width class. */

/** Width classes a quiet request judges (bit mask): 12.5 kHz, 6.25 kHz, and the analog width of the request. */
enum {
    DSD_SIGNAL_QUIET_CLASS_12K5 = 1,
    DSD_SIGNAL_QUIET_CLASS_6K25 = 2,
    DSD_SIGNAL_QUIET_CLASS_ANALOG = 4,
};

/** The widths of the two digital classes, in Hz. */
enum {
    DSD_SIGNAL_QUIET_WIDTH_12K5_HZ = 12500,
    DSD_SIGNAL_QUIET_WIDTH_6K25_HZ = 6250,
};

/** Sub-windows per quiet request. */
#define DSD_SIGNAL_QUIET_WINDOWS 9

/** How a quiet measurement estimates the local noise floor from the eight sideband blocks beside the channel, the
 * floor the guard compares with the capture-wide noise reference. */
typedef enum {
    /** Signal search's estimate: the mean of the blocks within 6 dB of the third lowest. */
    DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN = 0,
    /** The lowest block. */
    DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK = 1,
} dsd_signal_quiet_floor_estimator;

typedef struct {
    /** A sub-window is below when its channel power over the capture-wide noise reference across the same bins,
     * (S+N)/N in dB, is less than this. */
    float threshold_db;
    /** A dsd_signal_quiet_floor_estimator value. */
    int floor_estimator;
    /** A sub-window is invalid (never quiet) when a class's sideband floor stands more than this many dB over the
     * capture-wide noise reference of the same transforms: the reference then does not stand for the noise round the
     * channel (a front-end floor that is far from flat across the capture, or strong transmitters crowding the
     * sidebands). Finite. */
    float guard_db;
} dsd_signal_quiet_params;

typedef struct {
    /** Owner-local, nonzero. */
    uint64_t request_id;
    /** rtl_signal_capture_get() after the landing. */
    dsd_signal_capture capture;
    /** The landed channel frequency. */
    uint32_t frequency_hz;
    /** DSD_SIGNAL_QUIET_CLASS_* bits to judge; at least one. */
    uint8_t classes;
    /** The analog class's width, DSD_ANALOG_AM_WIDTH_MIN_HZ to DSD_ANALOG_NFM_WIDTH_MAX_HZ, with
     * DSD_SIGNAL_QUIET_CLASS_ANALOG; 0 without it. */
    uint32_t analog_width_hz;
} dsd_signal_quiet_request;

typedef struct {
    uint64_t request_id;
    /** The capture the samples came from (the request's). */
    dsd_signal_capture capture;
    /** DSD_SIGNAL_MEASURE_OK for a complete result. A failed one keeps its request's id and capture and is neither
     * valid nor quiet. */
    dsd_signal_measure_failure failure;
    /** valid: the channel and its noise sidebands fit the capture in every judged class, and no sub-window's floor
     * stands over the guard. quiet: valid, and every sub-window of every judged class is below the threshold. */
    uint8_t valid, quiet;
    /** The highest (S+N)/N over every valid sub-window and class, with that measurement's channel power and the
     * capture-wide noise reference over the same bins it was judged against (0 when nothing was valid). */
    float max_ratio_db, power_dbfs, floor_dbfs;
    /** The highest sideband floor over the capture-wide noise reference in any sub-window and class whose geometry
     * fits, in dB: above the guard (dsd_signal_quiet_params.guard_db) the result is not valid. 0 when no class fits or
     * no bin of the capture qualified for the reference; the result is then not valid either. */
    float floor_rise_db;
    /** dsd_realtime_mono_ns() when the analysis that completed (or failed) the request finished, after the feed held
     * the tap; shared tap only, else 0. */
    uint64_t completed_realtime_ns;
} dsd_signal_quiet_result;

/** The shipped threshold, floor estimator and floor guard the shared tap measures with (calibrated:
 * docs/signal-search.md "Quiet-channel calibration"). */
dsd_signal_quiet_params dsd_signal_quiet_params_default(void);

/** 1 when the calibration rendered and validated capture rate @p rate_hz (exactly; every class was judged at it), else
 * 0: at any other rate a scan visit makes no quiet check. The rates are those the capture paths run: the RTL rate
 * planner's, an Airspy's, and the common RTL-SDR rates a SoapySDR device may be given. */
int dsd_signal_quiet_rate_supported(uint32_t rate_hz);

/** 1 when an analog class of @p width_hz for analog @p kind (dsd_analog_demod: DSD_ANALOG_DEMOD_FM or
 * DSD_ANALOG_DEMOD_AM, as dsd_opts.analog_demod carries it) at capture rate @p rate_hz was rendered and validated as
 * that kind (exactly: the Radio sheet's NFM width steps for FM, its AM steps for AM, at a
 * dsd_signal_quiet_rate_supported() rate), else 0: any other kind or width gets no quiet check. */
int dsd_signal_quiet_analog_supported(int kind, uint32_t width_hz, uint32_t rate_hz);

/** Stand-alone quiet detector, the one the shared tap uses, for calibration and tests. */
typedef struct dsd_signal_quiet_detector dsd_signal_quiet_detector;
dsd_signal_quiet_detector* dsd_signal_quiet_detector_create(void);
void dsd_signal_quiet_detector_destroy(dsd_signal_quiet_detector* detector);
/** Arm @p detector for @p request, measured with @p params (NULL: dsd_signal_quiet_params_default()); the previous
 * request is dropped either way. Returns 0, or -1 with the reason in @p out_failure (optional): UNSUPPORTED for a
 * request, width class, rate or parameter it cannot measure, NO_MEMORY when its transform cannot be prepared. Buffers
 * are prepared once per transform size, so a request at an unchanged size allocates nothing. */
int dsd_signal_quiet_detector_begin(dsd_signal_quiet_detector* detector, const dsd_signal_quiet_request* request,
                                    const dsd_signal_quiet_params* params, dsd_signal_request_failure* out_failure);
/** Accumulate complete IQ pairs. Returns 1 once the request is complete, -1 once it failed (a capture change, clipping
 * over 1% of the pairs so far, a non-finite sample, the transform), else 0. Samples after either are ignored. */
int dsd_signal_quiet_detector_feed(dsd_signal_quiet_detector* detector, const float* iq, size_t pairs,
                                   const dsd_signal_capture* capture);
/** The result of the request last begun, once it finished: 1 complete, -1 failed (reason in @p result->failure), 0
 * not finished or never begun (@p result untouched). */
int dsd_signal_quiet_detector_result(const dsd_signal_quiet_detector* detector, dsd_signal_quiet_result* result);

/** Shared radio tap quiet request. The tap holds one request at a time and records its owner: a quiet request is
 * refused (UNSUPPORTED) while Signal search owns the tuner or holds a request or an unread report, and a Signal search
 * request supersedes a quiet one. Returns 0 and the request's ticket in @p ticket, or -1 with the reason in
 * @p out_failure (optional). Every search request, every quiet request not refused for Signal search, a quiet cancel
 * and a reset move the tap's ticket, so an earlier ticket reads lost at once, with no further samples. */
int rtl_signal_quiet_request(const dsd_signal_quiet_request* request, uint32_t* ticket,
                             dsd_signal_request_failure* out_failure);
/** Non-consuming and lock-free: 1 done (a result or failure waiting), 0 pending, -1 lost (superseded, cancelled,
 * reset, already taken, or never a quiet ticket). */
int rtl_signal_quiet_status(uint32_t ticket);
/** Consuming: 1 with a complete result, -1 with a failure (reason in @p result->failure; SUPERSEDED when the ticket
 * is lost), 0 pending (@p result untouched). */
int rtl_signal_quiet_take(uint32_t ticket, dsd_signal_quiet_result* result);
/** Retire the quiet request; a no-op unless @p ticket is still the tap's quiet request. rtl_signal_measure_cancel()
 * cancels only a Signal search request. */
void rtl_signal_quiet_cancel(uint32_t ticket);
#ifdef __cplusplus
}
#endif
#endif
