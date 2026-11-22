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
/** The clip level of a source whose float samples are scaled to a full scale of 1.0 (SoapySDR, a CF32 or CS16 replay),
 * of a native Airspy, and of a capture that sets none. On an Airspy the clip level is not a reliable indication of ADC
 * overload: libairspy's real-to-complex conversion keeps an in-band signal at about half of 1.0 even at the ADC's
 * rails (derived from its code), so its samples rarely reach this level. */
#define DSD_SIGNAL_CLIP_LEVEL_DEFAULT 0.999f

/** How a measurement treats a capture's geometry (dsd_signal_capture.geometry), a discrete policy of the source. */
typedef enum {
    /** Every source but a native Airspy: a hardware-DC guard round the hardware centre (no channel within it is
     * measured), a capture-wide noise reference over +-0.45 Fs of hardware offset, and a quiet ratio judged against
     * the lower of that reference and the channel's sideband floor. */
    DSD_SIGNAL_GEOMETRY_DEFAULT = 0,
    /** A native Airspy, tuned on the channel (no rotation, no offset): no hardware-DC exclusion, a capture-wide
     * reference over a per-rate span, and a quiet ratio judged against the lower of that reference and the channel's
     * sideband floor. It claims nothing about spurs or overload. */
    DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED = 1,
} dsd_signal_capture_geometry;

/** The receiver behind a capture, where the quiet measurement treats it apart (dsd_signal_capture.receiver). */
typedef enum {
    /** Any receiver the measurement's policy covers as its geometry says. */
    DSD_SIGNAL_RECEIVER_OTHER = 0,
    /** An Airspy opened through SoapySDR (its driver or hardware key names an Airspy; an Airspy HF+ is not one). Its
     * captures run the default policy at the Fs/4 shift, but its passband leaves the reference under or over the
     * channel's noise by an amount nobody has measured, so a quiet request on one is refused at every rate. */
    DSD_SIGNAL_RECEIVER_SOAPY_AIRSPY = 1,
    /** A SoapySDR device whose identity is unknown: no lookup of its keys or arguments named any device (failed,
     * empty, or opaque such as a proxy's). It cannot be ruled out as an Airspy, so a quiet request on one is refused
     * at the rates an Airspy runs (dsd_signal_quiet_receiver_refused()); at any other rate it is not an Airspy. */
    DSD_SIGNAL_RECEIVER_SOAPY_UNKNOWN = 2,
} dsd_signal_capture_receiver;

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
    /** A dsd_signal_capture_geometry value, set by the source (rtl_device_signal_geometry()); 0 is the default policy.
     * A capture change: a request armed or a report waiting under one policy fails on a capture of another. Last, so a
     * positional initialiser of the fields before it keeps its meaning. */
    uint8_t geometry;
    /** A dsd_signal_capture_receiver value, set by the source (rtl_device_signal_receiver()); 0 for any other. A capture
     * change, as the geometry is. */
    uint8_t receiver;
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
    /** The capture changed: retune, rate, rotation, offset, gain, geometry policy or output generation; or a completed
     * report waiting to be read was invalidated by any capture change. */
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
/** The tap's capture word (lock-free, acquire): it moves whenever a fed block's capture differs from the published one
 * in generation, centre or capture offset, and at a reset (rtl_signal_measure_reset()). Drops and gain do not move it.
 * A reader that remembers the word rtl_signal_capture_get_seq() gave knows, with one load, whether the capture may
 * have changed since (issue #834). */
uint32_t rtl_signal_capture_seq(void);
/** rtl_signal_capture_get() and the capture word in one snapshot, so the word read never belongs to a newer capture
 * than the one returned. Returns rtl_signal_capture_get()'s result; @p seq (optional) takes the word either way. */
int rtl_signal_capture_get_seq(dsd_signal_capture* capture, uint32_t* seq);
uint64_t rtl_signal_capture_last_ms(void);
void rtl_signal_measure_feed(const float* iq, size_t pairs, const dsd_signal_capture* capture);

/* Scan quiet measurement: whether one landed channel carries no power above its local noise floor over
 * back-to-back sub-windows (DSD_SIGNAL_QUIET_WINDOWS unless the request asks for another count), each the smallest
 * whole number of transforms of at least 10 ms (at least 90 ms in all at the calibrated count), judged in every
 * requested width class. */

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

/** Sub-windows per quiet request: the calibrated count, the one a request with windows 0 judges. */
#define DSD_SIGNAL_QUIET_WINDOWS     9

/** The most sub-windows a quiet request may ask for (dsd_signal_quiet_request.windows). */
#define DSD_SIGNAL_QUIET_WINDOWS_MAX 50

/** How a quiet measurement estimates the local noise floor from the eight sideband blocks beside the channel, the
 * floor the guard compares with the capture-wide noise reference. */
typedef enum {
    /** Signal search's estimate: the mean of the blocks within 6 dB of the third lowest. */
    DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN = 0,
    /** The lowest block. */
    DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK = 1,
} dsd_signal_quiet_floor_estimator;

typedef struct {
    /** A sub-window is below when its channel power over the noise it is judged against across the same bins,
     * (S+N)/N in dB, is less than this (centred_threshold_db on a DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED capture when set):
     * the lower of the capture-wide noise reference and the class's sideband floor, on every geometry. */
    float threshold_db;
    /** A dsd_signal_quiet_floor_estimator value. */
    int floor_estimator;
    /** A sub-window is invalid (never quiet) when a class's sideband floor stands more than this many dB over the
     * capture-wide noise reference of the same transforms: the reference then does not stand for the noise round the
     * channel (a front-end floor that is far from flat across the capture, or strong transmitters crowding the
     * sidebands). Finite. */
    float guard_db;
    /** A sub-window is invalid (never quiet) when more of the capture-wide reference's bins stand over the noise than
     * noise alone puts there: when the share of them above the 98th percentile the reference implies for noise, less
     * the 2% noise puts there, is over this. A capture partly filled by transmitters or a raised floor lifts the
     * reference's lower quartile off the noise (it lands on a higher quantile of the noise that is left). The ratio is
     * judged against the sideband floor where that is lower, so this matters most when the raise covers the sidebands
     * too; it voids such a reading rather than judging it. 0 turns it off; at most 1. */
    float occupancy_max_share;
    /** The threshold for a DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED capture in place of threshold_db; 0 means threshold_db.
     * Finite, not negative. */
    float centred_threshold_db;
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
    /** Sub-windows to judge: 0 for DSD_SIGNAL_QUIET_WINDOWS (the calibrated reading), else 1 to
     * DSD_SIGNAL_QUIET_WINDOWS_MAX. Each keeps its length (the smallest whole number of transforms of at least
     * 10 ms). */
    uint32_t windows;
    /** Sample pairs dropped before the first transform: where the reading starts in the stream, nothing else. */
    uint32_t lead_pairs;
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
    /** The highest (S+N)/N over every valid sub-window and class, with that measurement's channel power and the noise
     * over the same bins it was judged against (dsd_signal_quiet_params.threshold_db: the lower of the capture-wide
     * reference and the sideband floor), so power_dbfs - floor_dbfs is max_ratio_db (0 each when nothing was valid). */
    float max_ratio_db, power_dbfs, floor_dbfs;
    /** The highest sideband floor over the capture-wide noise reference in any sub-window and class whose geometry
     * fits, in dB, under either geometry policy: above the guard (dsd_signal_quiet_params.guard_db) the result is not
     * valid. 0 when no class fits or no bin of the capture qualified for the reference; the result is then not valid
     * either. */
    float floor_rise_db;
    /** The highest share over any sub-window of the reference's bins standing over its noise, beyond what noise alone
     * puts there (dsd_signal_quiet_params.occupancy_max_share); 0 when no bin of the capture qualified. Reported
     * whether or not the check is on. */
    float occupancy_share;
    /** dsd_realtime_mono_ns() when the analysis that completed (or failed) the request finished, after the feed held
     * the tap; shared tap only, else 0. */
    uint64_t completed_realtime_ns;
    /** Why a complete result is not valid, DSD_SIGNAL_QUIET_VOID_* bits over every sub-window and class (0 when it is
     * valid, and for a failed result). */
    uint8_t void_causes;
} dsd_signal_quiet_result;

/** dsd_signal_quiet_result.void_causes. */
enum {
    /** A judged class's channel or its noise sidebands do not fit the capture. */
    DSD_SIGNAL_QUIET_VOID_GEOMETRY = 1,
    /** No bin of the capture qualified for the noise reference. */
    DSD_SIGNAL_QUIET_VOID_REFERENCE = 2,
    /** A sideband floor stood over the reference by more than the guard (dsd_signal_quiet_params.guard_db). */
    DSD_SIGNAL_QUIET_VOID_GUARD = 4,
    /** The occupancy check: too many of the reference's bins stood over its noise
     * (dsd_signal_quiet_params.occupancy_max_share). */
    DSD_SIGNAL_QUIET_VOID_OCCUPANCY = 8,
};

/** The shipped thresholds, floor estimator, floor guard and occupancy check the shared tap measures with (calibrated:
 * docs/signal-search.md "Quiet-channel calibration"). */
dsd_signal_quiet_params dsd_signal_quiet_params_default(void);

/** 1 when the calibration rendered and validated capture rate @p rate_hz (exactly; every class was judged at it), else
 * 0: at any other rate a scan visit makes no quiet check. The rates are those the capture paths run: the RTL rate
 * planner's, an Airspy's, and the common RTL-SDR rates a SoapySDR device may be given. */
int dsd_signal_quiet_rate_supported(uint32_t rate_hz);
/** How many capture rates dsd_signal_quiet_rate_supported() admits. */
#define DSD_SIGNAL_QUIET_RATE_COUNT 16

/** 1 when @p capture's receiver gets no quiet check: a SoapySDR Airspy at any rate, a SoapySDR device of unknown
 * identity at a rate an Airspy runs (2.5, 3, 6 or 10 MS/s), or a receiver value this build does not know; else 0. The
 * detector refuses such a request (dsd_signal_quiet_detector_begin()), and the scan refuses the visit first: inline so
 * both read this one rule. */
static inline int
dsd_signal_quiet_receiver_refused(const dsd_signal_capture* capture) {
    if (!capture) {
        return 1;
    }
    if (capture->receiver == DSD_SIGNAL_RECEIVER_OTHER) {
        return 0;
    }
    if (capture->receiver == DSD_SIGNAL_RECEIVER_SOAPY_UNKNOWN) {
        return capture->rate_hz == 2500000U || capture->rate_hz == 3000000U || capture->rate_hz == 6000000U
               || capture->rate_hz == 10000000U;
    }
    return 1; /* a SoapySDR Airspy, or a value this build does not know */
}

/** 1 when the calibration validated capture rate @p rate_hz (exactly) for a DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED capture,
 * whose capture-wide reference spans a per-rate share of the capture, else 0: the native Airspy rates only. A quiet
 * request under that policy at any other rate is refused (dsd_signal_quiet_detector_begin()). */
int dsd_signal_quiet_airspy_centred_rate_supported(uint32_t rate_hz);

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
 * request, width class, rate, geometry policy (an unknown one, or AIRSPY_CENTRED at a rate
 * dsd_signal_quiet_airspy_centred_rate_supported() refuses) or parameter it cannot measure, NO_MEMORY when its
 * transform cannot be prepared. Buffers are prepared once per transform size, so a request at an unchanged size
 * allocates nothing. */
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
