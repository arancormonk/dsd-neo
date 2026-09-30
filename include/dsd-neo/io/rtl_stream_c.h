// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief C API for the RTL-SDR orchestrator.
 *
 * Declares a minimal C API mirroring lifecycle, tuning, and I/O operations
 * of the C++ `RtlSdrOrchestrator`, for consumption by C translation units.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_C_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_C_H_H

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/platform/platform.h>

#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/io/rtl_stream_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Decoder/control-pump thread only. */
int rtl_stream_airspy_controls(const dsd_airspy_config* config);
int rtl_stream_airspy_info(dsd_airspy_info* info);

typedef enum DSD_ATTR_PACKED rtl_stream_output_kind {
    RTL_STREAM_OUTPUT_AUDIO_MONITOR = 0,
    RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR = 1,
    RTL_STREAM_OUTPUT_SYMBOL_CQPSK = 2,
} rtl_stream_output_kind;

typedef enum DSD_ATTR_PACKED rtl_stream_channel_profile {
    RTL_STREAM_CHANNEL_PROFILE_WIDE = 0,
    RTL_STREAM_CHANNEL_PROFILE_6K25 = 1,
    RTL_STREAM_CHANNEL_PROFILE_12K5 = 2,
    RTL_STREAM_CHANNEL_PROFILE_PROVOICE = 3,
    RTL_STREAM_CHANNEL_PROFILE_P25_C4FM = 4,
    RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK = 5,
} rtl_stream_channel_profile;

typedef enum DSD_ATTR_PACKED rtl_stream_tune_result {
    RTL_STREAM_TUNE_OK = 0,
    RTL_STREAM_TUNE_DEFERRED = 1,
    RTL_STREAM_TUNE_FAILED = -1,
    RTL_STREAM_TUNE_TIMEOUT = -2,
} rtl_stream_tune_result;

/**
 * @brief Completion notification for a request-tagged RTL tune.
 *
 * The callback runs on the RTL controller thread after the hardware/DSP
 * reconfigure and output boundary have completed. Implementations must not
 * block or call back into the RTL tuning API.
 *
 * @param request_id Caller-provided request ID from rtl_stream_tune_tagged().
 * @param result Terminal result for the controller request.
 * @param user_data Opaque pointer supplied at registration time.
 */
typedef void (*rtl_stream_tune_completion_callback)(uint64_t request_id, rtl_stream_tune_result result,
                                                    void* user_data);

/* Lifecycle */
/**
 * @brief Create a new RTL-SDR stream context mirrored to caller-owned options.
 *
 * The stream owns an internal copy of @p opts, while live requested PPM
 * updates also write back into the caller-owned structure so restarts and
 * config snapshots retain the current correction.
 *
 * @param opts Mutable caller-owned decoder options. Must not be NULL.
 * @param out_ctx [out] On success, receives an opaque context pointer.
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_create(dsd_opts* opts, RtlSdrContext** out_ctx);
/**
 * @brief Start the stream threads and device I/O.
 * @param ctx Stream context created by rtl_stream_create().
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_start(RtlSdrContext* ctx);
/**
 * @brief Whether the last stream start failed at its analog channel check, and what it refused.
 *
 * A start checks the analog channel of the options it opens with once the rate chain is final, at the DSP rate the
 * device actually delivers (a SoapySDR or Airspy device can force one, an I/Q replay runs at its capture's), and fails
 * when it cannot run the width there: a width that rate cannot filter, any explicit width or the AM default while
 * DSD_NEO_CHANNEL_LPF=0 turns the channel filter off, or a replay whose sidecar decimates after the demodulator. The
 * refusal is logged with the validator's text and recorded here. Every stream create and every start forgets the
 * last record, so after a start that failed for any other reason (a missing device, a failed open) this returns 0.
 * The rate is otherwise lost once the start fails: rtl_stream_get_request_rate_hz() reads 0 with no stream, and
 * rtl_stream_get_demod_rate_hz() still reads the stream before it. For the decoder thread that made the start, after
 * it returned.
 *
 * @param out_kind     dsd_analog_demod the start was asked to run. May be NULL.
 * @param out_width_hz Its configured width (0 = the kind's default, as dsd_opts_analog_width_hz() gives it). May be
 *                     NULL.
 * @param out_rate_hz  The DSP rate the start held the width to, in Hz. May be NULL.
 * @return 1 when the last start was refused that way, filling the outputs; 0 otherwise, leaving them untouched.
 */
int rtl_stream_start_analog_refusal(int* out_kind, int* out_width_hz, int* out_rate_hz);
/**
 * @brief Whether the last stream start opened the I/Q capture (--iq-capture) file, writing it anew.
 *
 * A start with the capture requested opens its writer once the device runs, before it starts its workers and the
 * device's streaming. The writer reports whether it opened the data file for writing (dsd_iq_capture_open_ex()),
 * which ends what a stream before it recorded there, and this is that report: a writer that fails after the open, or
 * a start that fails after the writer opened (a worker that did not start, an Airspy SDK that did not stream), has
 * already written over an earlier recording, removing what it wrote (the metadata sidecar, and the data file when the
 * writer itself failed). A start that failed before the open (its analog channel check, a device that did not open, a
 * writer that failed before it opened the file) left that recording as the stream before it closed it. Every stream
 * create and every start forgets the last record. For the decoder thread that made the start, after it returned.
 *
 * @return 1 when the last start opened the capture file for writing, 0 otherwise.
 */
int rtl_stream_start_opened_capture(void);
/**
 * @brief Stop the stream and cleanup resources associated with the run.
 * Safe to call multiple times; subsequent calls are no-ops.
 * @param ctx Stream context created by rtl_stream_create().
 * @return 0 on success; otherwise <0 on error.
 */
int rtl_stream_stop(RtlSdrContext* ctx);
/**
 * @brief Destroy the stream context and free all associated resources.
 * If the stream is running, it is stopped before destruction.
 * @param ctx Stream context to destroy. May be NULL.
 * @return 0 always.
 */
int rtl_stream_destroy(RtlSdrContext* ctx);

/* Control */
/**
 * @brief Tune to a new center frequency.
 *
 * A retune the device settles on another demod rate is refused when that rate cannot realize the explicit width the
 * running analog monitor runs (dsd_analog_width_check()): the refusal is logged with the validator's text, the device
 * goes back to the capture frequency and rate it had, and the tune fails. When the device refuses to be put back, the
 * stream stops (logged, reported as a device input failure with the device's return code); when it does not return to
 * a rate that fits the width, the stream stops as a start at that rate fails (logged, reported as a configuration
 * input failure), rather than run the width without its channel filter.
 *
 * @param ctx Stream context.
 * @param center_freq_hz New center frequency in Hz.
 * @return rtl_stream_tune_result: 0 on success, 1 when deferred, negative on error/timeout.
 */
int rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz);

/**
 * @brief Tune to a new center frequency with correlated completion.
 *
 * A queued tagged tune owns its controller request until completion. A tune
 * from a different owner, including an untagged tune, is deferred instead of
 * replacing the queued target and profile. A zero request ID is invalid.
 *
 * @param ctx Stream context.
 * @param center_freq_hz New center frequency in Hz.
 * @param request_id Non-zero request ID forwarded to the completion callback.
 * @return rtl_stream_tune_result: 0 on success, 1 when deferred, negative on error/timeout.
 */
int rtl_stream_tune_tagged(RtlSdrContext* ctx, uint32_t center_freq_hz, uint64_t request_id);

/**
 * @brief Register the process-wide tagged tune completion callback.
 *
 * Replaces any prior registration. Pass NULL to clear the callback. Replacement
 * and unregistration wait for invocations of the previous callback to finish,
 * so its user data may be released after this function returns. A completion
 * callback must not call this function or another RTL tuning entry point.
 *
 * @param callback Completion callback, or NULL to unregister.
 * @param user_data Opaque callback context.
 */
void rtl_stream_register_tune_completion_callback(rtl_stream_tune_completion_callback callback, void* user_data);

/**
 * @brief Publish a live RTL PPM request and assign it a fresh request generation.
 *
 * Runtime writers should use this helper instead of mutating
 * `opts->rtlsdr_ppm_error` directly so failed applies and same-value retries
 * remain distinguishable.
 *
 * @param opts Decoder options to update. Must not be NULL.
 * @param ppm Requested correction in PPM; clamped to [-200, 200].
 * @return 0 on success; otherwise <0 on invalid input.
 */
int rtl_stream_request_ppm(dsd_opts* opts, int ppm);
/**
 * @brief Adjust the live RTL PPM request relative to its current published value.
 *
 * Uses the same synchronized publication path as rtl_stream_request_ppm() so
 * UI increment/decrement commands do not race a concurrent rollback or retry.
 *
 * @param opts Decoder options to update. Must not be NULL.
 * @param delta Signed delta in PPM; result is clamped to [-200, 200].
 * @return 0 on success; otherwise <0 on invalid input.
 */
int rtl_stream_adjust_ppm(dsd_opts* opts, int delta);
/**
 * @brief Read the live requested RTL PPM value using the synchronized runtime snapshot.
 *
 * Runtime UI code should prefer this helper over reading
 * `opts->rtlsdr_ppm_error` directly while the RTL stream is active.
 *
 * @param opts Decoder options to query. May be NULL.
 * @return The currently requested RTL PPM value, or 0 when `opts` is NULL.
 */
int rtl_stream_get_requested_ppm(const dsd_opts* opts);

/* I/O */
/**
 * @brief Read up to `count` interleaved audio samples into `out`.
 * @param ctx Stream context.
 * @param out Destination buffer for samples. Must not be NULL.
 * @param count Maximum number of samples to read.
 * @param out_got [out] Set to the number of samples actually read.
 * @return 0 on success; otherwise <0 on error (e.g., shutdown).
 */
int rtl_stream_read(RtlSdrContext* ctx, float* out, size_t count, int* out_got);
/**
 * @brief Get the current output sample rate in Hz.
 * @param ctx Stream context.
 * @return Output sample rate in Hz; returns 0 if `ctx` is invalid.
 */
uint32_t rtl_stream_output_rate(const RtlSdrContext* ctx);
/**
 * @brief Return the RTL output stream generation.
 *
 * Increments whenever the RTL output stream can contain a different logical
 * sample/symbol sequence, such as fresh stream setup, retune, restart, or
 * explicit output clear.
 */
uint32_t rtl_stream_output_generation(void);
/**
 * @brief Return 1 when an RTL-family stream context is currently running.
 *
 * The predicate is false before startup, after soft/hard stop, and during
 * cooperative shutdown. Use it before consuming stream-local metadata that may
 * retain its last configured value after cleanup.
 */
int rtl_stream_is_active(void);
/**
 * @brief Return the active RTL stream output kind.
 *
 * Digital RTL-family paths, including SoapySDR, return symbol kinds. Analog
 * monitor paths return AUDIO_MONITOR.
 */
int rtl_stream_get_output_kind(void);
int rtl_stream_get_symbol_profile_full(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile);

/**
 * @brief Update symbol modem profile for RTL-family digital modes.
 *
 * @param symbol_rate_hz Symbol rate in Hz, e.g. 4800, 6000, 2400.
 * @param levels Number of FSK levels (2 or 4). CQPSK remains selected by modulation.
 * @param channel_profile rtl_stream_channel_profile profile id.
 * @return 0 on success, negative on invalid input.
 */
int rtl_stream_set_symbol_profile(int symbol_rate_hz, int levels, int channel_profile);

/**
 * @brief Queue a demod profile change for the demod thread to apply.
 *
 * Unlike calling rtl_stream_toggle_cqpsk()/rtl_stream_set_symbol_profile()
 * directly, this is safe while the demod thread is running: parameters are
 * validated and queued, and the demod thread applies them between blocks
 * (a newer request overwrites an unconsumed older one). Application is
 * therefore deferred by up to one demod block; a rejected symbol profile is
 * logged by the demod thread rather than returned to the caller. When the
 * stream pipeline is not running there is no thread to race with, so the
 * request is applied immediately.
 *
 * @param cqpsk_enable 1/0 to switch the demod family, -1 to leave unchanged.
 * @param symbol_rate_hz Symbol rate in Hz (e.g. 4800, 6000, 2400); <=0 leaves
 *                       the symbol profile (rate/levels/channel) unchanged.
 * @param levels Number of FSK levels (2 or 4); ignored when symbol_rate_hz<=0.
 * @param channel_profile rtl_stream_channel_profile profile id; ignored when
 *                        symbol_rate_hz<=0.
 * @param ted_sps Timing SPS: >0 clears the override then applies the value,
 *                0 clears the override only, <0 leaves timing untouched.
 * @param ted_sps_is_override When ted_sps>0: nonzero applies it as a sticky
 *                            override (rtl_stream_set_ted_sps), zero applies
 *                            it without override so later rate changes may
 *                            recalculate SPS.
 * @return 0 on success, negative on invalid input.
 */
int rtl_stream_request_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps,
                                     int ted_sps_is_override);

/**
 * @brief Queue a receive-family / analog profile change for the demod thread.
 *
 * Covers a width-only change, an FM<->AM switch and an analog<->digital switch. The request is validated on the
 * caller's thread, then applied by the demod thread between blocks, before any demod profile queued after it; a newer
 * request overwrites an unconsumed older one, which then reads replaced (rtl_stream_receive_request_outcome()), and
 * drops any demod profile queued before it (for a digital request, the symbol profile that follows is the one the
 * switch lands on). An analog request, a width or kind change on the running monitor and a switch onto the monitor
 * alike, is validated against the published demod rate, and checked again by the demod thread against the rate the
 * stream is on when it applies it (a retune may have moved it). A width that rate cannot realize is refused either
 * time, never clamped, replaced or run without its channel filter: the refusal is logged with the validator's text and
 * the front end keeps its current receive profile (a digital session asked to switch stays on the digital family). The
 * unset NFM default is never refused for its rate. A refusal at the demod thread reaches the caller through the
 * request's number, which then reads refused (rtl_stream_receive_request_outcome(), with what the stream kept from
 * rtl_stream_receive_request_refusal()): a decoder that has already committed to Analog, or to the width, puts itself
 * back from that. A decode-mode change also asks rtl_stream_check_analog_profile() before it commits, so only a retune
 * in between gets this far. With no pipeline running there is nothing to switch and no demod rate to check against:
 * only the kind, range and DSD_NEO_CHANNEL_LPF rules apply, and the next stream open configures the front end from the
 * options and checks the width against the rate it actually delivers.
 *
 * Entering the analog family (or leaving it) re-applies the defaults a fresh stream open of that family would choose,
 * resets the filter state, clears the output ring and bumps the output generation. The stream remembers the family it
 * switched to: the options it runs on are the copy taken before the open, so that record, not those options, decides
 * from then on whether a symbol profile without CQPSK runs the FSK discriminator or monitor audio (and on the digital
 * family, the modes noted with rtl_stream_set_digital_decode_modes() pick its FSK channel profile). Leaving the analog
 * family for digital expects the digital symbol profile to follow through rtl_stream_request_demod_profile(), and waits
 * for it: that profile decides the digital resampler and output rate, so a digital family request the demod thread
 * finds without a symbol profile stays queued until one arrives, and the two apply at the same block boundary. A
 * digital family request leaves the analog family whenever the stream runs it (rtl_stream_analog_family_active()),
 * including after a symbol profile applied on its own (a typed digital scan row, a CQPSK toggle) has moved the front
 * end off the monitor output: such a profile never leaves the family itself, and the decoder asks for the digital
 * family only when its configured mode is digital, not for a typed digital scan row on an analog session.
 *
 * @param family   dsd_rx_family: DSD_RX_FAMILY_ANALOG or DSD_RX_FAMILY_DIGITAL.
 * @param kind     dsd_analog_demod for the analog family: FM (the discriminator) or AM (the envelope detector).
 * @param width_hz Explicit analog channel width in Hz, or 0 for the kind's default (ignored for digital).
 * @return 0 when queued or applied; -1 when refused (unknown family/kind, a width outside the kind's range, a
 *         width the running stream's published rate cannot realize (the unset AM default is held to it like an
 *         explicit width), or, while DSD_NEO_CHANNEL_LPF=0, an explicit width or AM). A refusal is
 *         logged as an error with the validator's text (for a width the rate cannot realize: the width, the DSP rate,
 *         the largest width that rate fits and the DSP bandwidths that would fit), once per kind, width and rate until
 *         an analog request is accepted.
 */
int rtl_stream_request_analog_profile(int family, int kind, int width_hz);

/**
 * @brief Queue a switch to the digital family that lands where rtl_stream_output_rate_for_family() predicts, whichever
 * family the front end runs where the demod thread applies it (issue #583).
 *
 * rtl_stream_request_analog_profile(DSD_RX_FAMILY_DIGITAL, ...) switches only a front end that runs the analog family;
 * on the digital family its symbol profile applies the CQPSK state it asks for and keeps the output chain the stream
 * runs. A caller that timed the decoder for the digital family's landing because the front end runs the analog family,
 * or because work already outstanding lands a family (rtl_stream_family_landing_after_pending(): a retune that carries
 * the digital family lands that prediction even on a front end already digital), asks with this instead, so the front
 * end lands where the decoder is timed in either order: this request is the newer word on the family, so a retune that
 * carries a family, attached before it, lands only its centre if it lands after this was queued, and one that landed
 * the prediction before leaves this request landing the same prediction again.
 *
 * It is the same family request otherwise: counted as a live family request (rtl_stream_live_family_request_count()),
 * numbered (rtl_stream_receive_request_seq()), replaced by a later family request while still queued, which lands as
 * that one says, and followed by the symbol profile the caller queues right after it
 * (rtl_stream_request_demod_profile()), which the demod thread waits for and applies at the same block boundary. That
 * profile lands as a switch out of the analog family would: the CQPSK state and channel filter an open of the mode
 * would pick, which DSD_NEO_CQPSK decides when set unless @p cqpsk_explicit (rtl_demod_landing_cqpsk(),
 * rtl_demod_open_channel_profile()), and a TED timed for the demod rate it lands on unless the profile overrides it. On
 * a front end already on the digital family the resampler and output rate are designed again for that CQPSK state and
 * symbol rate, as the switch and a retune's finalize design them, and nothing else of the switch happens: no family
 * switch is recorded (RtlSdrInternals::rx_family_switch keeps deciding the FSK channel profile source) and no loops are
 * reset beyond a CQPSK change's. While it is queued, rtl_stream_family_landing_after_pending() counts it: a digital
 * retune queued behind it lands after that landing, and a timing made before it lands is made for it.
 *
 * @param cqpsk_explicit Non-zero when the CQPSK state of the symbol profile that follows is a trunk-scan target's own
 *                       choice, which stands over DSD_NEO_CQPSK (the rule rtl_stream_output_rate_for_family() and a
 *                       retune's rtl_stream_retune_analog_profile::cqpsk_explicit follow); 0 lands where an open of the
 *                       mode would.
 * @return 0 when queued, or with no pipeline running settled at once like any family request (the next stream open
 *         configures the front end from the options).
 */
int rtl_stream_request_digital_family_landing(int cqpsk_explicit);

/** @brief What became of a queued receive request (rtl_stream_receive_request_outcome()). */
enum rtl_stream_rx_request_outcome {
    RTL_STREAM_RX_REQUEST_PENDING = 0,  /**< Queued: the demod thread has not taken it yet. */
    RTL_STREAM_RX_REQUEST_SETTLED = 1,  /**< Taken and its result published, or dropped otherwise than REPLACED. */
    RTL_STREAM_RX_REQUEST_REFUSED = 2,  /**< An analog profile request refused at the demod rate it landed at. */
    RTL_STREAM_RX_REQUEST_REPLACED = 3, /**< Never taken: an analog profile request a later one replaced in the queue,
                                             or a retune's receive family retired, or a request dropped with it. */
};

/**
 * @brief Number of the last receive request queued for the demod thread (rtl_stream_request_demod_profile(),
 * rtl_stream_request_analog_profile()), for rtl_stream_receive_request_outcome().
 *
 * Read it on the thread that queued the request, right after the request: the decoder thread queues them all. Numbers
 * rise by one per queued request and are never 0. A request applied at once, with no pipeline running, is not queued
 * and gets no number of its own; it settles every request queued before it, since no demod thread will take them.
 */
uint32_t rtl_stream_receive_request_seq(void);

/**
 * @brief What became of the receive request numbered @p seq (RTL_STREAM_RX_REQUEST_*).
 *
 * While a request is pending, what the stream publishes (the CQPSK state rtl_stream_get_cqpsk_status() reports, the
 * analog profile) can still describe the stream before it: the demod thread clears the output, which moves the output
 * generation, before it publishes what it applied, and a retune moves the generation without taking a request. Once
 * the request is settled, refused or replaced, what the stream publishes includes its effect, or what replaced it. The
 * demod thread settles every request it took at a block boundary, a later request that replaced an earlier one
 * settles that one with it, and a stream open settles whatever the previous stream left queued. An analog profile
 * request whose width the demod rate it landed at cannot filter (a retune moved the rate after the request was
 * checked) is refused there, logged with the validator's text, and the front end keeps the receive profile it had; its
 * number then reads REFUSED until the next stream open, which opens on the options as they are and forgets it (it
 * reads settled from then).
 *
 * An analog profile request (rtl_stream_request_analog_profile(), either family) the demod thread never took reads
 * REPLACED once it settles, not SETTLED like one the front end ran (issue #578): a later analog profile request
 * replaced it while it was still queued, or a retune's receive family landed before the demod thread took it and
 * retired it (rtl_stream_prepare_retune_analog_profile_for_target(): a scan going on to its next row). What replaced it
 * decides the front end. So it reads for every analog request of a chain dropped that way, each queued over the one
 * before it, and for a demod profile queued between two of them, or after the last one and dropped with it. Any other
 * request the stream dropped without taking it (a demod profile a later request replaced, a queue a stream open
 * dropped, or one a request made with no pipeline running settled) reads SETTLED. The stream keeps the last such run
 * only: a request of an earlier run reads SETTLED once an analog request queued after that run ended is dropped in
 * turn, and the next stream open forgets the run as it forgets a refusal.
 */
int rtl_stream_receive_request_outcome(uint32_t seq);

/**
 * @brief The receive profile the stream kept when it refused request @p seq where it landed.
 *
 * Recorded before the refusal becomes visible, so a caller that reads @p seq as refused reads what the refusing
 * stream kept, not what the request asked for: two width requests queued back to back, the first taken and the second
 * refused, leave the first one's width.
 *
 * @param seq               A number rtl_stream_receive_request_seq() returned.
 * @param out_analog_family 1 when the stream stayed on the analog family (a width or kind change on the monitor, or a
 *                          return to the monitor from a symbol profile applied under it), 0 when it stayed on the
 *                          digital family (a switch onto the analog family). May be NULL.
 * @param out_width_hz      The configured analog width that family runs, as the request that set it carried it (0 =
 *                          the kind's default, AM's included, so an explicit 6000 Hz AM width reads 6000 and the unset
 *                          default 0; meaningful on the analog family only). May be NULL.
 * @param out_kind          The dsd_analog_demod that family runs (meaningful on the analog family only): the kind
 *                          before a refused switch between FM and AM. May be NULL.
 * @param out_monitor       1 when the stream kept the analog monitor output (dsd_demod_analog_monitor_active(): the
 *                          analog family on its own channel, with no CQPSK), which then runs @p out_width_hz; 0 on the
 *                          digital family, or on the analog family under a symbol profile applied on its own (a typed
 *                          digital scan row's channel profile, CQPSK toggled on), where @p out_width_hz is the setting
 *                          the monitor would return to, not a width that runs. May be NULL.
 * @return 1 when @p seq reads RTL_STREAM_RX_REQUEST_REFUSED, filling the outputs; 0 otherwise, leaving them untouched.
 */
int rtl_stream_receive_request_refusal(uint32_t seq, int* out_analog_family, int* out_width_hz, int* out_kind,
                                       int* out_monitor);

/**
 * @brief The CQPSK state the RTL front end runs once the receive requests queued so far have applied.
 *
 * While a request is unsettled (rtl_stream_receive_request_outcome()), what the stream publishes
 * (rtl_stream_get_cqpsk_status()) can still describe the stream before it. This answers for the requests instead,
 * whoever queued them: the DSP menu's CQPSK toggle, a decode-mode change, a scan row's profile or its leave. An analog
 * family request turns CQPSK off (it enters the monitor), a demod profile request sets the state it names or leaves
 * it, and a digital family request leaves it to the symbol profile queued after it. Once every request has settled,
 * the published state answers. An analog request later refused where it landed was counted as turning CQPSK off
 * until it settles.
 *
 * @return 1 for CQPSK, 0 otherwise.
 */
int rtl_stream_requested_cqpsk(void);

/**
 * @brief Note the digital decode modes the decoder is configured for.
 *
 * An open picks the FSK channel profile a symbol profile without one of its own lands on (the DSP menu's CQPSK toggle
 * turning CQPSK off) from the decode modes in the options it opens with. Those options are the copy taken before the
 * open, and a live switch can move the stream onto the digital family for modes they do not name (a -fA session names
 * none), so once a live switch has done that, the modes noted here pick that profile instead, as an open with them
 * would. A stream still on the family it opened on keeps picking from its own options. Called on the decoder thread
 * with its configured options whenever it publishes a digital mode's symbol profile (svc_publish_symbol_profile()); the
 * latest note stands until the next stream open drops it. Does nothing for NULL.
 *
 * @param opts Decoder options whose frame flags name the configured digital decode modes.
 */
void rtl_stream_set_digital_decode_modes(const dsd_opts* opts);

/**
 * @brief Check a receive-family / analog profile request without queuing it.
 *
 * Applies the family and kind, the width's range, DSD_NEO_CHANNEL_LPF, and while a stream runs its published demod
 * rate and a replay's post-demod decimation, and logs a refusal the way rtl_stream_request_analog_profile() does. For
 * a caller that must change nothing else when the front end refuses, such as a decode-mode change: asked before the
 * caller commits its decoder, it keeps the decoder and the front end on the same family whenever the rate holds until
 * the request that follows lands (a retune that moves the rate in between gets that request refused, logged, with
 * the front end left on its current receive profile).
 *
 * @return 0 when the front end takes the profile at the rate it runs now; -1 when it refuses it.
 */
int rtl_stream_check_analog_profile(int family, int kind, int width_hz);

/**
 * @brief Report the demod (DSP) rate an analog profile request is held to on the caller's thread.
 *
 * The rate the running stream published, from its start on, which rtl_stream_check_analog_profile(),
 * rtl_stream_request_analog_profile() and rtl_stream_prepare_retune_analog_profile_for_target() check a width against.
 * Unlike rtl_stream_get_demod_rate_hz(), a metrics value the demod thread publishes with its first processed block,
 * it needs no block to have run.
 *
 * @return The rate in Hz, or 0 with no stream running.
 */
int rtl_stream_get_request_rate_hz(void);

/**
 * @brief Report the published analog receive profile.
 *
 * @param out_kind     dsd_analog_demod of the active analog family (0 otherwise). May be NULL.
 * @param out_width_hz Effective channel width in Hz: the configured width while the width-driven channel filter runs,
 *                     otherwise the width the DSP rate leaves: the passband of the legacy WIDE plan an unset default
 *                     runs where the rate cannot fit it (dsd_channel_lpf_legacy_wide_width_hz()), or the DSP rate
 *                     with no channel filter (0 outside the analog family). An I/Q replay that
 *                     decimates after the demodulator runs the filter at post_downsample times its design rate, and
 *                     reports that many times the designed width. May be NULL.
 * @param out_lpf_on   1 when the width-driven channel filter sets the width, 0 when the DSP rate limits it (including
 *                     a replay's post-demod decimation). May be NULL.
 * @return 1 while the analog family is active, 0 otherwise (digital output, or the M17 encoder's monitor path).
 */
int rtl_stream_get_analog_profile(int* out_kind, int* out_width_hz, int* out_lpf_on);

/**
 * @brief Report whether the stream runs the analog receive family.
 *
 * Unlike rtl_stream_get_analog_profile(), this stays 1 while a symbol profile applied without a family request (a
 * CQPSK toggle, a typed digital scan row's profile) has moved the front end off the analog monitor output. A digital
 * family request leaves the analog family from there too, so a caller that times the decoder for that switch reads
 * this rather than the published analog profile.
 *
 * @return 1 while the stream runs the analog family, 0 otherwise (the digital family, the M17 encoder's monitor
 *         path, or before a stream has published its profile).
 */
int rtl_stream_analog_family_active(void);

/**
 * @brief Report whether a digital retune queued now lands on a receive family's landing rather than on the profile the
 * stream runs: the stream runs the analog family now, or the receive requests and retunes already queued or in flight
 * land a family once they have landed (issue #583).
 *
 * rtl_stream_analog_family_active() answers for the family published now. A retune the controller has taken, or one
 * queued for it, lands its family later, and so does a live request the demod thread has not taken yet (a width edit
 * made while an analog target's retune was still in flight). A caller deciding whether a digital retune it queues now
 * must attach the digital family, or timing the decoder for where that retune lands, reads this instead: the retune
 * lands after that outstanding work, so a symbol profile without the family would land on the analog family it leaves
 * behind, an older analog request would take the front end back to the monitor, and a digital-family retune still
 * outstanding moves the output rate to the digital family's landing, not the rate the stream runs now.
 *
 * The answer is a union, 1 when any of these holds:
 * - the published family is analog (rtl_stream_analog_family_active());
 * - a retune the controller has taken, or the one queued for it, carries a receive family, lands on the target it
 *   was queued for, and has not been superseded by a later live family request
 *   (rtl_stream_prepare_retune_analog_profile_for_target()): the analog family, or the digital family, which lands
 *   where rtl_stream_output_rate_for_family() predicts even on a front end already on the digital family;
 * - a live receive request is still unsettled and the requests queued so far leave the stream on the analog family
 *   (rtl_stream_receive_request_outcome()), or end with a digital landing the demod thread has not applied
 *   (rtl_stream_request_digital_family_landing(), which lands that prediction on either family too).
 *
 * The retunes are read first, then the live requests, then the published family, and each publishes what it landed
 * before it stops counting as outstanding, so a landing between two reads is still seen. A union rather than the newest
 * word on the family: a queued retune coalesces with a later one and takes its profile, so a retune that carries a
 * family, queued behind another, can be replaced by one that carries none. The answer can only err towards 1 (an analog
 * retune refused where it lands, a request replaced later), which attaches the digital family to a retune that finds
 * the digital family running where it lands: it retires older live requests and lands its symbol profile as a switch to
 * the digital family would, where the decoder, timed by this same answer, expects it
 * (rtl_stream_prepare_retune_analog_profile_for_target()). A digital-only session never attaches a family, so it
 * answers 0 throughout, as the published family alone does. Only the decoder thread queues receive requests and
 * retunes, and the controller and demod threads only resolve them, so between two reads on the decoder thread the answer
 * can only fall from 1 to 0, unless the decoder thread queued work that lands a family in between. A scan row's timing
 * records the decision it made from it (dsd_scan_mode_timed_digital_family()), which its retune's family follows, and a
 * timing of the row made while that retune is outstanding reads 1 from the retune itself. A live republish decides by
 * the same rule (svc_publish_symbol_profile()): when it times the decoder for the landing it asks for the landing too
 * (rtl_stream_request_digital_family_landing()), which supersedes such a retune.
 *
 * With no stream running there is no controller to read, but the live requests and the published family still are:
 * a stream that closed on the analog family keeps publishing it, so the answer is then 1, as
 * rtl_stream_analog_family_active() is.
 *
 * @return 1 when the analog family runs now or outstanding work lands a family, or, with no stream running, when the
 *         family last published is analog; 0 otherwise.
 */
int rtl_stream_family_landing_after_pending(void);

/**
 * @brief Report the analog kind and configured channel width the stream runs on the analog family, whether or not its
 * monitor output runs.
 *
 * rtl_stream_get_analog_profile() publishes a kind and width only while the monitor output runs. A symbol profile
 * applied under the family without a family request (a CQPSK toggle, a typed digital scan row's profile) moves the
 * front end off that output with the family's kind and width setting left as they were: they are what the monitor
 * returns to, and what a refused request leaves the stream on (rtl_stream_receive_request_refusal() records the same
 * two). A caller that reads what the front end kept once its requests have settled reads this, so the kind a mode
 * change under a typed row asked for is never taken for the one the family runs.
 *
 * @param out_kind     dsd_analog_demod the analog family runs (0 outside the family). May be NULL.
 * @param out_width_hz Configured width the family was asked for (demod_state::analog_width_setting_hz: 0 for the
 *                     kind's default, AM's included; 0 outside the family). May be NULL.
 * @return 1 while the stream runs the analog family (as rtl_stream_analog_family_active()), 0 otherwise.
 */
int rtl_stream_get_analog_setting(int* out_kind, int* out_width_hz);

/**
 * @brief Report whether the stream runs its channel filter where no width requests one: the unset NFM default on the
 * analog monitor, and a digital profile.
 *
 * The stream's configuration decides it once, from DSD_NEO_CHANNEL_LPF or else from the DSP rate the configuration
 * starts from (20 kHz or more), and keeps it when the device delivers another rate (a forced rate, a replay's capture
 * rate) or a retune moves it. So the width the monitor returns to from a CQPSK toggle or a typed digital scan row
 * follows this decision, not the rate it runs at then.
 *
 * @return 1 when it runs the filter, 0 when it does not, -1 before a stream has published its receive profile.
 */
int rtl_stream_channel_lpf_default(void);

/**
 * @brief Predict the output rate the stream will have once it runs @p family.
 *
 * A family switch is deferred to the demod thread, so a caller that sets symbol timing for the new family must not
 * read the current output rate (the analog monitor resamples to its audio rate; a digital stream usually does not).
 * A retune that carries the digital family lands on this rate too, whether the front end still runs the analog family
 * where it lands or already runs the digital family (rtl_stream_prepare_retune_analog_profile_for_target()), and so
 * does a live digital landing (rtl_stream_request_digital_family_landing()).
 *
 * @param family         dsd_rx_family.
 * @param cqpsk_enable   Non-zero for the CQPSK symbol output (digital family only). DSD_NEO_CQPSK overrides it when
 *                       set, as it does at stream open and at the switch itself, unless @p cqpsk_explicit.
 * @param symbol_rate_hz Digital symbol rate, which decides the digital resampling policy.
 * @param cqpsk_explicit Non-zero when @p cqpsk_enable is a trunk-scan target's own choice, which stands over
 *                       DSD_NEO_CQPSK at the switch (issue #583): what a retune that carries the digital family says
 *                       (rtl_stream_retune_analog_profile::cqpsk_explicit), or a live digital landing
 *                       (rtl_stream_request_digital_family_landing(), which a republish of a trunk-scan row asks with
 *                       the scope's flag). 0 for a plain live family request (rtl_stream_request_analog_profile()) and
 *                       a landing whose CQPSK state is no target's own, which land where an open of the mode would.
 * @return Predicted output rate in Hz, derived from the last published demod rate (48000 until a stream has opened
 *         and published its own), or 0 when that rate is not positive.
 */
unsigned int rtl_stream_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz, int cqpsk_explicit);

typedef struct rtl_stream_retune_gain_profile {
    int tuner_gain_is_set;
    int tuner_gain_tenth_db;
    int tuner_gain_is_auto;
    int tuner_autogain_is_set;
    int tuner_autogain_on;
} rtl_stream_retune_gain_profile;

/**
 * @brief Queue a symbol/CQPSK timing profile plus optional tuner gain for a retune target.
 *
 * Gain fields are consumed at the same retune boundary as the demodulator
 * profile. Pass NULL or set tuner_gain_is_set to zero to leave tuner gain
 * unchanged. When set, tuner_gain_is_auto requests device auto gain; otherwise
 * tuner_gain_tenth_db is applied as nearest manual gain.
 *
 * @param target_freq_hz Intended center frequency in Hz; zero leaves the profile unbound.
 * @param cqpsk_enable 0/1 to force CQPSK off/on, negative to leave unchanged.
 * @param symbol_rate_hz Symbol rate in Hz, e.g. 4800 or 6000.
 * @param levels Number of symbol levels, 2 or 4.
 * @param channel_profile rtl_stream_channel_profile profile id.
 * @param ted_sps CQPSK timing samples-per-symbol to apply; <=0 leaves the SPS unchanged.
 * @param persist_ted_override Non-zero keeps ted_sps as an override after retune.
 * @param gain_profile Optional tuner gain/autogain profile to apply at retune.
 */
void rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t target_freq_hz, int cqpsk_enable,
                                                            int symbol_rate_hz, int levels, int channel_profile,
                                                            int ted_sps, int persist_ted_override,
                                                            const rtl_stream_retune_gain_profile* gain_profile);

/** @brief Receive family, analog demodulator and channel width to apply at a retune. */
typedef struct rtl_stream_retune_analog_profile {
    int family;   /**< dsd_rx_family to switch to: DSD_RX_FAMILY_DIGITAL or DSD_RX_FAMILY_ANALOG. */
    int kind;     /**< dsd_analog_demod (analog family only). */
    int width_hz; /**< Explicit analog channel width in Hz; 0 selects the kind's default. */
    /** Digital family only: non-zero when the CQPSK state of the symbol profile queued for the target is the target's
     *  own choice (a trunk-scan target's `modulation`, or a DMR/NXDN target's FSK), which stands over DSD_NEO_CQPSK
     *  where the retune lands (issue #583). 0 lands where an open of the mode would. Ignored with the analog family,
     *  and for a symbol profile that leaves the CQPSK state alone (cqpsk_enable < 0). */
    int cqpsk_explicit;
} rtl_stream_retune_analog_profile;

/**
 * @brief Attach a receive-family / analog profile to the retune queued for @p target_freq_hz.
 *
 * Applied at the same retune boundary as the symbol profile and before it. Queue any symbol profile for the target
 * first (rtl_stream_prepare_retune_profile_for_target_with_gain() replaces the whole queued profile); with none
 * queued for this target, an analog-only profile is queued that leaves the CQPSK family, symbol profile and timing
 * alone. A DSD_RX_FAMILY_DIGITAL switch then applies the queued symbol profile; a DSD_RX_FAMILY_ANALOG switch
 * applies none of it (the analog family has no symbol clock), only the gain profile.
 *
 * A retune that carries DSD_RX_FAMILY_DIGITAL lands as a switch out of the analog family would: the CQPSK family and
 * channel filter an open of the mode would, which DSD_NEO_CQPSK decides when set (rtl_demod_open_cqpsk_request()),
 * unless @p analog's cqpsk_explicit says the queued symbol profile's CQPSK state is the target's own: that state then
 * stands, with the channel filter the profile names, as it does on a retune that carries no family. The resampler and
 * output rate follow that CQPSK state, and the TED is timed for the demod rate it lands on, so the retune lands where
 * rtl_stream_output_rate_for_family() predicts, the rate a caller that attaches the family times the decoder for. It
 * lands there whether the front end still runs the analog family where the retune lands or already runs the digital
 * family (an analog retune refused where it landed, or a request replaced, can leave it there); the family itself is
 * switched, with the loop resets of a switch, only from the analog family. A front end already on the digital family
 * gets its output chain designed again where the stream retunes, and where an external backend's retune lands
 * (rtl_stream_apply_pending_retune_profile_for_target()), which has no retune of the stream's own to finalize it. A
 * retune that carries no family applies its symbol profile as queued, over the output chain it finds, unless a live
 * analog request or a scan leave counted after the profile was queued superseded it
 * (rtl_stream_familyless_retune_supersedes()): then it lands its centre only. Otherwise it retires a live analog
 * request still queued from before it was queued, which then reads replaced.
 *
 * A live rtl_stream_request_analog_profile() or rtl_stream_request_digital_family_landing() accepted after this call is
 * the newer word on the family: the retune then lands on its target with neither this family nor the symbol profile
 * queued with it, so the front end stays on the family and profile the live requests chose. A scanner that leaves
 * while its row's retune is still in flight puts the configured family back that way, and the late retune does not
 * switch it back to the row's.
 *
 * @return 0 when attached; -1 when refused (same rules and refusal log as rtl_stream_request_analog_profile()).
 */
int rtl_stream_prepare_retune_analog_profile_for_target(uint32_t target_freq_hz,
                                                        const rtl_stream_retune_analog_profile* analog);

/**
 * @brief Number of live receive-family requests accepted so far (rtl_stream_request_analog_profile(),
 * rtl_stream_request_digital_family_landing()).
 *
 * Rises by one per accepted request, running stream or not; a refused request does not count. A retune profile whose
 * family was attached before the number last moved no longer lands that family or the symbol profile queued with it
 * (rtl_stream_prepare_retune_analog_profile_for_target()). A scanner that queued a row's retune compares the number
 * when the retune completes: a request made meanwhile (a command acting for the row still on air) chose the family
 * the front end runs, so the row it tuned for must be restaged.
 */
uint32_t rtl_stream_live_family_request_count(void);

/**
 * @brief Number of supersedes counted so far for retune profiles queued with no receive family (issue #582).
 *
 * Rises by one per accepted rtl_stream_request_analog_profile() for DSD_RX_FAMILY_ANALOG (it asks for the monitor),
 * running stream or not, and per rtl_stream_supersede_familyless_retunes(); a refused request, and a digital one,
 * does not count. A retune profile queued with no receive family before the number last moved no longer lands its
 * symbol profile: the retune moves its centre only. A scanner that queued such a row's retune compares the number when
 * the retune completes, and restages the row when it moved.
 */
uint32_t rtl_stream_familyless_retune_supersedes(void);

/**
 * @brief Supersede the retune profiles queued with no receive family so far (issue #582).
 *
 * Counts one supersede (rtl_stream_familyless_retune_supersedes()). The -Y scan leave calls it before its own
 * requests: a typed row's retune, which carries no family, is otherwise not superseded by the leave's digital family
 * request, and the controller can still land it after the demod thread took the leave's requests, putting the row's
 * symbol profile back over the configured decoder's for good. Superseded, it lands its centre only.
 */
void rtl_stream_supersede_familyless_retunes(void);

/**
 * @brief Apply and clear a queued retune profile for a specific external retune target.
 *
 * Use this when an external backend, such as rigctl, has completed a frequency
 * change and the queued profile was bound to that target.
 *
 * Called on the decoder thread while the demod thread runs. The landing holds the stream's reconfigure gate throughout,
 * which parks the demod thread between blocks: it waits while the controller thread is reconfiguring the stream (a PPM
 * correction, a hop), and the next reconfiguration waits until the landing has finished. Under the gate the gain
 * profile goes to the device, then the receive family, the symbol profile and its TED apply, in the order the stream's
 * own retune applies them under that gate. A profile that carries the digital family lands as it does on the stream's
 * own retune (rtl_stream_prepare_retune_analog_profile_for_target()): the output chain is designed again for the CQPSK
 * state and symbol rate it lands, so the stream runs at the rate rtl_stream_output_rate_for_family() predicted (issue
 * #583). A profile without a family keeps the output chain.
 */
void rtl_stream_apply_pending_retune_profile_for_target(uint32_t target_freq_hz);

/**
 * @brief Clear any queued retune profile that has not yet been consumed.
 */
void rtl_stream_clear_pending_retune_profile(void);

/**
 * @brief Request fresh acquisition for the active RTL FSK demod path.
 *
 * The request is consumed by the demod thread before the next FSK block so the
 * modem state is not mutated from decoder/control threads. Returns 1 when a
 * request was queued, 0 when RTL FSK output is inactive.
 */
int rtl_stream_request_fsk_reacquire(void);

/**
 * @brief Request fresh acquisition for the active RTL CQPSK demod path.
 *
 * The request is consumed by the demod thread before the next CQPSK block so
 * carrier/timing state is not mutated from decoder/control threads. The
 * coarse FLL frequency estimate is retained while phase, timing, and filter
 * history are reset. Returns 1 when a request was queued, 0 when RTL CQPSK
 * output is inactive.
 */
int rtl_stream_request_cqpsk_reacquire(void);

/**
 * @brief Return mean power approximation (RMS^2 proxy) for soft squelch.
 * The computation uses a small fixed sample window and matches the reference implementation.
 * @param ctx Stream context (unused).
 * @return Mean power value (approximate RMS squared, normalized to full scale 1.0).
 */
double rtl_stream_return_pwr(const RtlSdrContext* ctx);

/**
 * @brief Set the channel squelch level in the demod state.
 *
 * Call this whenever opts->rtl_squelch_level changes to keep the demod
 * state in sync for channel-based squelching.
 *
 * @param level Linear power threshold (same units as rtl_squelch_level).
 */
void rtl_stream_set_channel_squelch(float level);

/**
 * @brief Enable or disable RTL-SDR bias tee at runtime.
 *
 * Applies to the active device if present. For rtl_tcp sources, forwards the
 * request to the server (protocol cmd 0x0E). For USB sources, requires
 * librtlsdr built with bias tee API support.
 *
 * @param on Non-zero to enable, zero to disable.
 * @return 0 on success; negative on failure or when no device is active.
 */
int rtl_stream_set_bias_tee(int on);

/**
 * @brief Get the currently applied tuner gain.
 *
 * Returns the driver-reported tuner gain in tenths of a dB (e.g., 270 for
 * 27.0 dB) via out_tenth_db, and whether auto-gain is active via out_is_auto
 * (1=auto, 0=manual). Any pointer may be NULL. Returns 0 on success; <0 if
 * the RTL stream/device is not available.
 */
int rtl_stream_get_gain(int* out_tenth_db, int* out_is_auto);
/**
 * @brief Enable or disable rtl_tcp adaptive buffering/autotune logic.
 * @param onoff Non-zero to enable; zero to disable.
 */
int rtl_stream_set_rtltcp_autotune(int onoff);

/**
 * @brief Return the last RF center frequency applied by the controller thread.
 *
 * This reports the effective tuned frequency after any pending-retune coalescing.
 *
 * @param out_freq_hz [out] Applied center frequency in Hz.
 * @return 0 on success; negative on error.
 */
int rtl_stream_get_last_applied_freq(uint32_t* out_freq_hz);

/**
 * @brief Get smoothed CQPSK timing residual from the demod pipeline in Q14 units.
 *
 * Positive values indicate persistent "sample early" bias (nudge center right),
 * negative values indicate "sample late" bias (nudge center left).
 * Returns 0 when unavailable.
 *
 * @param ctx Stream context (unused).
 * @return Signed Q14 residual (approximately float residual * 16384).
 */
int rtl_stream_cqpsk_timing_bias(const RtlSdrContext* ctx);

/**
 * @brief Get the configured CQPSK Gardner timing samples-per-symbol.
 *
 * @return Nominal SPS (>=2); 0 when unavailable.
 */
int rtl_stream_get_ted_sps(void);

/**
 * @brief Get the pending CQPSK Gardner timing samples-per-symbol override.
 *
 * @return Override SPS when set; 0 when normal rate-derived SPS is active.
 */
int rtl_stream_get_ted_sps_override(void);

/**
 * @brief Set the CQPSK Gardner timing samples-per-symbol.
 *
 * Use when switching between symbol rates (e.g., P25P1 4800 sym/s vs P25P2 6000 sym/s).
 * CQPSK timing will reinitialize its internal state (omega bounds, delay line) on SPS change.
 * Also sets an override flag that persists the value across rate-change refreshes.
 *
 * @param sps Nominal samples per symbol (clamped to [2, 64]).
 */
void rtl_stream_set_ted_sps(int sps);

/**
 * @brief Clear the CQPSK timing SPS override.
 *
 * Call when returning to control channel to allow normal SPS calculation
 * based on opts mode flags. Without clearing, the voice channel SPS would
 * persist incorrectly.
 */
void rtl_stream_clear_ted_sps_override(void);

/**
 * @brief Set the CQPSK Gardner timing SPS without asserting the override.
 *
 * Sets ted_sps but leaves ted_sps_override unchanged (typically 0 after
 * clearing). Use when returning to CC or switching protocols where the
 * rate-change refresh should be allowed to recalculate SPS later.
 *
 * @param sps Nominal samples per symbol (clamped to [2, 64]).
 */
void rtl_stream_set_ted_sps_no_override(int sps);

/**
 * @brief Set the CQPSK Gardner timing loop gain (native float).
 *
 * @param gain Loop gain; typical 0.01..0.1, default ~0.05.
 */
void rtl_stream_set_ted_gain(float gain);

/**
 * @brief Get the current CQPSK Gardner timing loop gain.
 *
 * @return Native float loop gain.
 */
float rtl_stream_get_ted_gain(void);

/**
 * @brief Capture a snapshot of the eye diagram buffer (timing helper).
 *
 * Copies up to `max_samples` real I-channel samples from the decimated complex
 * baseband into `out` and returns the number of samples copied. Also writes
 * the current nominal SPS into out_sps when available.
 *
 * @param out Destination buffer for I-channel samples (must not be NULL).
 * @param max_samples Maximum number of samples to copy.
 * @param out_sps [out] Receives nominal samples-per-symbol (may be NULL).
 * @return Number of samples written; 0 if unavailable.
 */
int rtl_stream_eye_get(float* out, int max_samples, int* out_sps);

/**
 * @brief Get smoothed demod SNR estimate in dB (post-filter, center-of-symbol).
 *
 * Computed on the demod thread for digital modes using the active demodulation
 * domain: discriminator samples for FSK output, and symbol/constellation-domain
 * samples for CQPSK.
 * Returns a negative value when unavailable.
 *
 * @return SNR in dB, or negative when unavailable.
 */
double rtl_stream_get_snr_c4fm(void);
/**
 * @brief Get smoothed CQPSK/LSM demod SNR estimate in dB.
 * @return SNR in dB, or negative when unavailable.
 */
double rtl_stream_get_snr_cqpsk(void);
/**
 * @brief Get smoothed GFSK demod SNR estimate in dB.
 * @return SNR in dB, or negative when unavailable.
 */
double rtl_stream_get_snr_gfsk(void);

/**
 * @brief Get the current C4FM (4-level FSK) SNR estimator bias in dB.
 *
 * This bias accounts for both the statistical estimator bias and the
 * noise bandwidth correction based on current DSP settings (sample rate,
 * samples per symbol, and channel LPF profile).
 *
 * @return Bias value in dB to subtract from raw SNR estimate.
 */
double rtl_stream_get_snr_bias_c4fm(void);

/**
 * @brief Get the current EVM/GFSK/QPSK SNR estimator bias in dB.
 *
 * This bias accounts for both the statistical estimator bias and the
 * noise bandwidth correction based on current DSP settings.
 *
 * @return Bias value in dB to subtract from raw SNR estimate.
 */
double rtl_stream_get_snr_bias_evm(void);

/**
 * @brief Estimate C4FM SNR from the eye buffer as a lightweight fallback.
 *
 * Uses quartile clustering over eye-diagram I-channel samples near symbol
 * centers to approximate signal and noise variances, returning SNR in dB.
 * Returns a negative value (<= -50 dB) when insufficient data is available.
 */
double rtl_stream_estimate_snr_c4fm_eye(void);

/**
 * @brief Estimate QPSK SNR from the constellation snapshot as a fallback.
 *
 * Uses recent equalized I/Q points to estimate amplitude and EVM vs ideal
 * QPSK targets; returns SNR in dB. Returns <= -50 dB when insufficient data.
 */
double rtl_stream_estimate_snr_qpsk_const(void);

/**
 * @brief Estimate GFSK SNR from the eye buffer as a fallback.
 *
 * Uses a two-level (median split) clustering on eye-diagram I-channel
 * samples near symbol centers; returns SNR in dB. Returns <= -50 dB when
 * insufficient data.
 */
double rtl_stream_estimate_snr_gfsk_eye(void);

/**
 * @brief Get supervisory tuner auto-gain enable flag.
 * @return 1 when auto-gain supervisor is enabled; 0 when disabled.
 */
int rtl_stream_get_tuner_autogain(void);
/**
 * @brief Enable or disable supervisory tuner auto-gain.
 * @param onoff Non-zero to enable; zero to disable.
 */
void rtl_stream_set_tuner_autogain(int onoff);

/**
 * @brief Get auto PPM status and last measurements.
 *
 * @param enabled [out] Current enable flag (0/1); may be NULL.
 * @param snr_db [out] Latest SNR estimate in dB; may be NULL.
 * @param df_hz [out] Latest residual frequency offset in Hz; may be NULL.
 * @param est_ppm [out] Estimated PPM error relative to center; may be NULL.
 * @param last_dir [out] Last applied step direction (-1,0,+1); may be NULL.
 * @param cooldown [out] Remaining cooldown iterations before next step; may be NULL.
 * @param locked [out] 1 if locked; 0 if training/idle; may be NULL.
 * @return 0 on success; negative on error or when unavailable.
 */
int rtl_stream_auto_ppm_get_status(int* enabled, double* snr_db, double* df_hz, double* est_ppm, int* last_dir,
                                   int* cooldown, int* locked);

/**
 * @brief Get locked auto-PPM value and lock-time snapshot, if available.
 *
 * @param ppm [out] Locked PPM value; may be NULL.
 * @param snr_db [out] SNR at lock time in dB; may be NULL.
 * @param df_hz [out] Residual frequency offset at lock time in Hz; may be NULL.
 * @return 0 on success; negative when unavailable.
 */
int rtl_stream_auto_ppm_get_lock(int* ppm, double* snr_db, double* df_hz);
/**
 * @brief Runtime toggle for auto-PPM (0/1).
 *
 * @param onoff Non-zero to enable; zero to disable.
 */
void rtl_stream_set_auto_ppm(int onoff);
/**
 * @brief Return the current runtime auto-PPM toggle value (0/1).
 *
 * @return 1 when auto-PPM is enabled; 0 when disabled.
 */
int rtl_stream_get_auto_ppm(void);

/**
 * @brief Toggle generic IQ balance prefilter (mode-aware image cancel).
 *
 * @param onoff Non-zero to enable; zero to disable.
 */
void rtl_stream_toggle_iq_balance(int onoff);
/** Get generic IQ balance prefilter state; returns 1 if enabled. */
int rtl_stream_get_iq_balance(void);
/**
 * @brief Provide P25P1 FEC OK/ERR deltas to drive BER-adaptive tuning.
 * Call with positive deltas (not totals). No-ops when RTL stream inactive.
 *
 * @param fec_ok_delta Incremental count of successful FEC codewords.
 * @param fec_err_delta Incremental count of failed FEC codewords.
 */
void rtl_stream_p25p1_ber_update(int fec_ok_delta, int fec_err_delta);

typedef struct rtl_stream_costas_metrics {
    int err_smooth_avg_q14;
    int err_raw_avg_q14;
    int confidence_avg_q14;
    int zero_conf_pct;
} rtl_stream_costas_metrics;

typedef struct rtl_stream_decode_health {
    int valid;
    uint32_t generation;
    unsigned int p25p1_fec_ok;
    unsigned int p25p1_fec_err;
    unsigned int p25p2_facch_ok;
    unsigned int p25p2_facch_err;
    unsigned int p25p2_sacch_ok;
    unsigned int p25p2_sacch_err;
    unsigned int p25p2_voice_err;
} rtl_stream_decode_health;

/**
 * @brief Toggle CQPSK path pre-processing on/off (0=off, nonzero=on).
 *
 * @param onoff Non-zero to enable; zero to disable.
 */
void rtl_stream_toggle_cqpsk(int onoff);
/**
 * @brief Get current CQPSK recovery status; any pointer may be NULL.
 *
 * @param cqpsk_enable [out] CQPSK enable flag.
 * @param cqpsk_timing_active [out] CQPSK Gardner timing active flag.
 * @return 0 on success; negative on error.
 */
int rtl_stream_get_cqpsk_status(int* cqpsk_enable, int* cqpsk_timing_active);

/**
 * @brief Get RTL-path decode-health counters for the current output generation.
 *
 * Counters are reset on output generation changes and accumulate protocol
 * health callbacks such as P25 Phase 1 FEC and Phase 2 RS/voice deltas.
 *
 * @param out [out] Decode-health snapshot. Must not be NULL.
 * @return 0 on success; negative on invalid input.
 */
int rtl_stream_get_decode_health(rtl_stream_decode_health* out);

/**
 * @brief Get recent raw receiver input-level health metrics.
 *
 * The snapshot observes backend-native samples before demodulation (CU8 for
 * RTL/rtl_tcp, CS16 or CF32 for Soapy). When raw receiver metrics are not yet
 * available, the RTL path may return an invalid/unknown snapshot or a
 * soft-symbol clipping diagnostic.
 *
 * @param out [out] Input-level snapshot. Must not be NULL.
 * @return 0 on success; negative on invalid input.
 */
int rtl_stream_get_input_level(dsd_input_level_snapshot* out);

/**
 * @brief Provide P25 Phase 2 RS/voice error deltas for runtime helpers.
 *
 * Pass positive deltas (not totals). Slot is 0 or 1. Any delta may be 0 when
 * not applicable. No-ops when RTL stream inactive.
 *
 * @param slot Slot index (0 or 1).
 * @param facch_ok_delta Incremental FACCH success count.
 * @param facch_err_delta Incremental FACCH error count.
 * @param sacch_ok_delta Incremental SACCH success count.
 * @param sacch_err_delta Incremental SACCH error count.
 * @param voice_err_delta Incremental voice error count.
 */
void rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                 int sacch_err_delta, int voice_err_delta);

/**
 * @brief Capture a snapshot of recent constellation points after DSP.
 *
 * Copies up to `max_points` I/Q pairs into `out_xy` as interleaved floats
 * [I0,Q0,I1,Q1,...] on the normalized float amplitude scale used by the DSP.
 * Returns the number of pairs copied (0 if unavailable).
 *
 * @param out_xy Destination buffer for interleaved I/Q pairs (must not be NULL).
 * @param max_points Maximum number of pairs to write.
 * @return Number of pairs written; 0 if unavailable.
 */
int rtl_stream_constellation_get(float* out_xy, int max_points);

/**
 * @brief Get a snapshot of the current baseband power spectrum (magnitude, dBFS-like).
 *
 * Returns up to max_bins bins in out_db, equally spaced across the complex
 * baseband Nyquist span, with DC-centered ordering (i.e., out_db[0] ~ -Fs/2,
 * mid ~ DC, last ~ +Fs/2). Values are smoothed and approximately in dBFS.
 * Optionally returns the current demod output sample rate via out_rate.
 *
 * @param out_db Destination buffer for spectrum bins (float dB). Must not be NULL.
 * @param max_bins Maximum number of bins to write.
 * @param out_rate Optional pointer to receive current output sample rate in Hz.
 * @return Number of bins written (0 if unavailable).
 */
int rtl_stream_spectrum_get(float* out_db, int max_bins, int* out_rate);

/**
 * @brief Set desired spectrum FFT size (power-of-two, clamped to allowed range).
 *
 * @param n Requested FFT size (power of two within supported bounds).
 * @return 0 on success; negative on error.
 */
int rtl_stream_spectrum_set_size(int n);
/** @brief Get current spectrum FFT size. */
int rtl_stream_spectrum_get_size(void);

/**
 * @brief Get a snapshot of the wideband power spectrum across the capture span.
 *
 * Unlike rtl_stream_spectrum_get(), which reports the narrow post-decimation
 * span used for tuner diagnostics, this covers the full SDR capture bandwidth
 * (typically ~1.536 MHz) so a UI can draw a panorama around the tuned
 * frequency. Bins are DC-centered: out_db[0] ~ center - span/2, the middle bin
 * ~ center, and the last ~ center + span/2. Values are smoothed and
 * approximately in dBFS.
 *
 * Production is off by default and costs nothing until
 * rtl_stream_wideband_spectrum_set_enabled(1) is called. The center, span and
 * serial number are published atomically with the bins, so the axis always
 * matches the data.
 *
 * Every frame is exactly DSD_WIDEBAND_SPECTRUM_BINS wide. A buffer shorter than
 * that is refused rather than filled with a prefix, which would be the low end
 * of the span carrying a label for the whole of it.
 *
 * @param out_db Destination buffer, at least DSD_WIDEBAND_SPECTRUM_BINS floats
 *               (from <dsd-neo/core/wideband_spectrum.h>). Must not be NULL.
 * @param max_bins Capacity of @p out_db in floats.
 * @param out_center_freq_hz Optional pointer to receive the tuned center in Hz.
 * @param out_span_hz Optional pointer to receive the covered span in Hz.
 * @param out_frame_serial Optional pointer to receive the frame's serial number.
 *                         It changes only when the producer publishes a new
 *                         frame, so a consumer polling on its own clock can tell
 *                         a fresh frame from a re-read of the last one.
 * @return Number of bins written; 0 when disabled, not yet published,
 *         invalidated by a retune, or when @p out_db is too small. On 0 the
 *         buffer is left exactly as the caller passed it, so a consumer that
 *         holds its last frame across a gap is holding the frame it drew.
 */
int rtl_stream_wideband_spectrum_get(float* out_db, int max_bins, uint32_t* out_center_freq_hz, uint32_t* out_span_hz,
                                     uint32_t* out_frame_serial);

/** @brief Enable or disable wideband spectrum production (off = zero DSP cost). */
void rtl_stream_wideband_spectrum_set_enabled(int on);
/** @brief Return 1 when wideband spectrum production is enabled. */
int rtl_stream_wideband_spectrum_enabled(void);

/* Carrier/Costas diagnostics and control */
/** Return current NCO frequency used for carrier rotation (Costas/FLL), in Hz. */
double rtl_stream_get_cfo_hz(void);
/** Return 1 when carrier loop appears locked (CQPSK active, CFO/residual small, SNR ok), else 0. */
int rtl_stream_get_carrier_lock(void);
/** Return last average absolute smoothed Costas error magnitude (Q14, pi==1<<14). */
int rtl_stream_get_costas_err_q14(void);
/** Return Costas discriminator health metrics for the latest DSP block. */
int rtl_stream_get_costas_metrics(rtl_stream_costas_metrics* out);
/** Return raw NCO frequency control (Q15 cycles per sample). */
int rtl_stream_get_nco_q15(void);
/** Return current demod output sample rate (Hz). */
int rtl_stream_get_demod_rate_hz(void);

/** Return FLL band-edge frequency estimate in Hz (coarse freq offset for CQPSK). */
double rtl_stream_get_fll_band_edge_freq_hz(void);

/**
 * @brief Get complex I/Q DC blocker state and shift k.
 *
 * Any pointer may be NULL.
 *
 * @param out_shift_k [out] Current shift exponent k; may be NULL.
 * @return 1 if enabled; 0 otherwise.
 */
int rtl_stream_get_iq_dc(int* out_shift_k);
/**
 * @brief Set DC blocker enable (0/1) and/or shift k (>=6..<=15).
 *
 * @param enable Non-zero to enable; zero to disable.
 * @param shift_k Shift exponent k; pass negative to leave unchanged.
 */
void rtl_stream_set_iq_dc(int enable, int shift_k);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_STREAM_C_H_H */
