// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The rate chain a radio or replay runs from its device rate to the DSP bandwidth rate (issue #806).
 *
 * dsd-neo asks a device for an RTL-shaped rate (the DSP bandwidth rate times a power of two) and halves it with the
 * half-band filters. A device that forces its own rate (an Airspy's 2.5 MS/s, say) leaves the half-band filters at a
 * rate that is not the DSP bandwidth rate, often not even a whole number of Hz (2.5 MS/s over 2^6 is 39062.5 Hz). The
 * live I/Q rate step then resamples that to the DSP bandwidth rate exactly. dsd_rate_plan_compute() decides the
 * chain from the rates alone, with no device, DSP or runtime dependency, so a live open, a read-back, a retune and a
 * replay all plan the same way.
 *
 * The rates, in the order they are known:
 *
 * - request: the rate dsd-neo asks for (the DSP bandwidth rate times 2^p, RTL-shaped).
 * - programmed: the rate written to the device after it picked the nearest one it has (the request on an RTL-SDR).
 * - readback: the rate the device reports after the write; 0 when it has not been read back yet.
 * - nominal: programmed when the read-back is within DSD_RATE_PLAN_READBACK_TOLERANCE_PPM of it, else the read-back.
 *   It is formed before the passes are chosen, so a read-back of 1535999 Hz plans like 1536000 Hz.
 * - hb_out: nominal / 2^p, the rate leaving the half-band filters, kept as an exact fraction.
 * - target: the DSP bandwidth rate.
 * - step_out: the rate after the step: target when it steps, else hb_out.
 *
 * The passes p are the most (up to max_passes) that keep hb_out at or above target. The step runs only where the
 * backend can take it (an Airspy, a SoapySDR device, or a replay that keeps a recorded forced chain) and hb_out is not
 * already target; its ratio is L/M = target x 2^p / nominal in lowest terms when both terms fit
 * DSD_RATE_PLAN_MAX_RATIO_TERM, else the closest ratio whose terms fit, accepted within DSD_RATE_PLAN_MAX_APPROX_PPM.
 * An RTL-SDR or rtl_tcp stream never steps: in tolerance its chain is exact by construction, and a read-back out of
 * tolerance is refused.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RATE_PLAN_H
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RATE_PLAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How far a read-back may be from the programmed rate and still count as it, in ppm (inclusive). */
#define DSD_RATE_PLAN_READBACK_TOLERANCE_PPM 100
/** The largest L or M of an exact step ratio; the same as DSD_IQ_RESAMPLER_MAX_RATIO_TERM, which a DSP test pins. */
#define DSD_RATE_PLAN_MAX_RATIO_TERM         4096
/** How far an approximated step ratio may leave the target, in ppm (inclusive); the same as
    DSD_IQ_RESAMPLER_MAX_APPROX_PPM, which a DSP test pins. */
#define DSD_RATE_PLAN_MAX_APPROX_PPM         5.0
/** The most passes a plan takes; the same as DSD_IQ_RESAMPLER_MAX_RATE_SHIFT, which a DSP test pins. */
#define DSD_RATE_PLAN_MAX_PASSES             30

/** Where the samples come from, which decides whether the chain may step. */
typedef enum {
    DSD_RATE_PLAN_BACKEND_RTL_USB = 0,
    DSD_RATE_PLAN_BACKEND_RTL_TCP,
    DSD_RATE_PLAN_BACKEND_AIRSPY,
    DSD_RATE_PLAN_BACKEND_SOAPY,
    /** A replay that keeps the chain its sidecar recorded: it steps only when that chain was device-forced. */
    DSD_RATE_PLAN_BACKEND_REPLAY_KEEP,
    /** A replay whose reader resamples the capture to a rate the half-band filters reach exactly. */
    DSD_RATE_PLAN_BACKEND_REPLAY_RESAMPLED,
    /** A replay whose capture rate the half-band filters reach exactly. */
    DSD_RATE_PLAN_BACKEND_REPLAY_EXACT,
    DSD_RATE_PLAN_BACKEND_WAV,
} dsd_rate_plan_backend;

/** Why dsd_rate_plan_compute() refused. */
typedef enum {
    DSD_RATE_PLAN_OK = 0,
    /** A NULL argument, an unknown backend, a request, programmed rate or target of 0 or past INT32_MAX, a read-back past
        INT32_MAX, or max_passes outside 0..DSD_RATE_PLAN_MAX_PASSES. */
    DSD_RATE_PLAN_ERR_ARGS = -1,
    /** An RTL-SDR or rtl_tcp read-back out of tolerance: the chain would run at a fractional rate it cannot step. */
    DSD_RATE_PLAN_ERR_READBACK = -2,
    /** The nominal rate is below the target: the chain never upsamples. */
    DSD_RATE_PLAN_ERR_BELOW_TARGET = -3,
    /** The step ratio's exact terms pass DSD_RATE_PLAN_MAX_RATIO_TERM and the closest ratio that fits is further than
        DSD_RATE_PLAN_MAX_APPROX_PPM from the target. */
    DSD_RATE_PLAN_ERR_RATIO = -4,
    /** A backend that cannot step would leave the half-band filters at a rate that is not a whole number of Hz. */
    DSD_RATE_PLAN_ERR_FRACTIONAL = -5,
} dsd_rate_plan_status;

/** What dsd_rate_plan_compute() plans from. */
typedef struct {
    dsd_rate_plan_backend backend;
    /** The rate asked for. A replay passes the request its recorded chain made, so a chain the device forced (a
        request other than the capture rate) still plans as forced. */
    uint32_t request_hz;
    uint32_t programmed_hz;
    uint32_t readback_hz; /**< 0: not read back yet (a provisional plan), so nominal is the programmed rate. */
    uint32_t target_hz;
    int max_passes; /**< The longest half-band chain the caller runs (10 live). */
} dsd_rate_plan_input;

/** The I/Q rate step a plan runs. All 0 (and enabled 0) when it does not step. */
typedef struct {
    int enabled;
    int L;
    int M;
    /** How far the step's output rate is from the target: ((hb_out x L / M) / target - 1) x 1e6. 0 for an exact ratio;
        an approximated one is within DSD_RATE_PLAN_MAX_APPROX_PPM, and step_out is still reported as the target. */
    double approx_ppm;
    int protect_hz; /**< The protected band: 0.45 x target (dsd_rate_plan_step_edges()). */
    int stop_hz;    /**< The stopband edge: target less the protected band. */
} dsd_rate_plan_step;

/** A planned rate chain. On a refusal, the fields worked out before it stay set (the read-back, nominal and
    device_forced of a refused RTL-SDR read-back, say) and the step is off. */
typedef struct {
    dsd_rate_plan_backend backend;
    uint32_t request_hz;
    uint32_t programmed_hz;
    uint32_t readback_hz;
    uint32_t target_hz;
    uint32_t nominal_hz;
    int passes; /**< p */
    /** hb_out = hb_out_num / 2^hb_out_shift Hz, in lowest terms: a shift of 0 is a whole number of Hz. */
    int64_t hb_out_num;
    unsigned hb_out_shift;
    /** The device decided the rate: programmed differs from the request, or the read-back was out of tolerance. */
    int device_forced;
    /** The backend may step: an Airspy, a SoapySDR device, or a REPLAY_KEEP of a device-forced chain. */
    int step_eligible;
    dsd_rate_plan_step step;
    /** step_out in whole Hz: the target when stepping, else hb_out. 0 on a refusal. */
    uint32_t step_out_hz;
    int status; /**< dsd_rate_plan_status */
    /** Why it was refused, in plain words for the log and the user (the rates and the DSP bandwidth, no filter
        terms); empty on success. */
    char message[192];
} dsd_rate_plan;

/** The I/Q rate step's delay, for EOF padding and retune-boundary expectations. All 0 for a plan that does not step. */
typedef struct {
    int taps_per_phase;        /**< T, from the same Kaiser estimate the I/Q resampler designs with. */
    double group_delay_inputs; /**< (L x T - 1) / (2 x L) input samples */
    double group_delay_s;      /**< group_delay_inputs / hb_out */
    uint32_t tail_inputs;      /**< T - 1 input samples: how long the step rings after its last input */
    double tail_s;             /**< tail_inputs / hb_out */
} dsd_rate_plan_bounds;

/**
 * @brief Plan the chain for @p in into @p out.
 *
 * @p out is always filled (zeroed first). Refusals: an RTL-SDR or rtl_tcp read-back out of tolerance, a nominal rate
 * below the target, a step ratio with no fit within DSD_RATE_PLAN_MAX_APPROX_PPM, and a backend that cannot step left
 * at a fractional rate; each names the rates in @p out->message.
 *
 * The plan does not bound the step's taps per phase. A step the I/Q resampler cannot design (past its 256 taps per
 * phase, which no Airspy or SoapySDR grid rate reaches) is refused when the step is designed: a live stream then
 * refuses the open or the retune that read the rate back, and puts the device back on the capture it ran.
 *
 * @return DSD_RATE_PLAN_OK, or a negative dsd_rate_plan_status (also in @p out->status).
 */
int dsd_rate_plan_compute(const dsd_rate_plan_input* in, dsd_rate_plan* out);

/** The step's design edges for @p target_hz: the protected band 0.45 x target (rounded down to whole Hz) and the
    stopband edge target less that. */
void dsd_rate_plan_step_edges(uint32_t target_hz, int* protect_hz, int* stop_hz);

/**
 * @brief The delay and tail of @p plan's step.
 *
 * T comes from the Kaiser estimate dsd_iq_resampler_configure_ratio() designs with (65 dB, for the 60 dB the resampler
 * promises; taps per phase rounded up to a multiple of 4), duplicated here because io_iq cannot depend on the DSP
 * module; a DSP test pins the two equal at every planned grid point.
 *
 * @return DSD_RATE_PLAN_OK (bounds of 0 for a plan that does not step), or DSD_RATE_PLAN_ERR_ARGS for a NULL argument.
 */
int dsd_rate_plan_step_bounds(const dsd_rate_plan* plan, dsd_rate_plan_bounds* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RATE_PLAN_H */
