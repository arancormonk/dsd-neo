// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RTL-SDR device I/O public API.
 *
 * Declares the opaque `rtl_device` handle and functions for configuring and
 * streaming samples from an RTL-SDR, including optional offset tuning, gain
 * control, PPM correction, and asynchronous USB ingestion into an input ring.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEVICE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEVICE_H_

#include <dsd-neo/core/airspy_config.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle for RTL-SDR device */
struct rtl_device;
struct input_ring_state;
struct dsd_iq_capture_writer;

/**
 * @brief Optional SoapySDR-specific startup controls.
 *
 * String pointers may be NULL or empty. bandwidth_hz uses -1 for profile/default
 * behavior, 0 for driver automatic/no explicit bandwidth request, and positive
 * values for a requested hardware bandwidth in Hz.
 *
 * center_freq_hz is the intended startup center frequency. Configuration runs before the
 * tuner is programmed, so profiles that pick an antenna per band (for example the SDDC
 * HF/VHF ports) need it here; 0 disables antenna auto-selection.
 */
struct rtl_soapy_config {
    const char* profile;
    const char* antenna;
    const char* clock_source;
    const char* gains;
    const char* stream_format;
    int bandwidth_hz;
    const char* settings;
    uint32_t center_freq_hz;
};

/**
 * @brief Create and initialize a local RTL-SDR device over USB (librtlsdr).
 *
 * @param dev_index Device index to open.
 * @param input_ring Pointer to input ring for incoming I/Q data.
 * @return Pointer to rtl_device handle, or NULL on failure.
 */
struct rtl_device* rtl_device_create(int dev_index, struct input_ring_state* input_ring);

/** Last local USB open/claim error, preserving librtlsdr/libusb's negative code
 * (including LIBUSB_ERROR_BUSY). Atomic, safe to read after engine cleanup.
 * Clear at session start so a non-USB run cannot inherit a previous USB failure. */
int rtl_device_last_open_error(void);
void rtl_device_clear_open_error(void);

/**
 * @brief Hand the next USB open an already-open device file descriptor.
 *
 * Android applications cannot open `/dev/bus/usb` nodes themselves: the descriptor
 * comes from `UsbDeviceConnection.getFileDescriptor()` in Java. While one is set,
 * USB device discovery is bypassed — the engine skips enumeration (which would
 * report no devices) and rtl_device_create() wraps this descriptor instead of
 * opening by index. The caller keeps ownership and must keep it open for the
 * lifetime of the device.
 *
 * Recording the descriptor and bypassing enumeration are platform-uniform, which
 * keeps the engine's decision reachable from a host test. Actually opening from it
 * is not: only builds where rtl_device_preopened_fd_supported() is true can, and
 * anywhere else the open falls through to opening by index — which, with
 * enumeration bypassed, means opening whichever device sits at the configured
 * index. Callers that can be built either way must check support first.
 *
 * @param sys_fd Open USB file descriptor, or -1 to clear.
 */
void rtl_device_set_preopened_fd(int sys_fd);

/**
 * @brief Whether a pre-opened USB descriptor is currently set.
 *
 * @return 1 when set, 0 otherwise.
 */
int rtl_device_preopened_fd_is_set(void);

/**
 * @brief Whether the engine currently has the pre-opened descriptor wrapped.
 *
 * Distinct from rtl_device_preopened_fd_is_set(), which only reports that one was
 * recorded. This is what the descriptor's owner must poll before closing the
 * connection behind it: the engine takes the descriptor part way into a run and
 * gives it back before the run ends, so neither "a descriptor is set" nor "the
 * engine is running" brackets the period during which closing it would pull the
 * file out from under an in-flight USB transfer.
 *
 * Raised before the wrap is attempted and lowered only after the device is closed,
 * so it is never clear while libusb still holds the descriptor. Clearing the slot
 * with rtl_device_set_preopened_fd(-1) does not lower it -- a device already open
 * keeps working, by design.
 *
 * @return 1 while the descriptor is in use, 0 otherwise.
 */
int rtl_device_preopened_fd_in_use(void);

/**
 * @brief Whether this build can open a device from a pre-opened descriptor.
 *
 * Requires Android and a librtlsdr providing `rtlsdr_open_fd()`. Where this is 0,
 * rtl_device_set_preopened_fd() records a descriptor that nothing will consume.
 *
 * @return 1 when supported, 0 otherwise.
 */
int rtl_device_preopened_fd_supported(void);

/**
 * @brief Set how long a USB receiver's failure waits before it ends the session.
 *
 * When a USB receiver stops delivering for a reason other than a stop (its reader returned: unplugged, or its transfers
 * gave out; or it sent no samples for 5 s, which the sample-progress monitor notices), the session ends for it as a
 * device failure. Where something else owns an unplug's outcome -- on Android, the USB detach
 * broadcast, which stops the engine itself -- that owner needs a moment to act first: during the grace the failure
 * polls the session, and a stop or an exit request inside it keeps the failure silent. Only once the grace runs out is
 * the failure reported and the receiver's ending committed (dsd_input_failure_receiver_ended()).
 *
 * Process-wide, read when a failure is raised. The Android host sets 2000 ms at startup; every other build keeps the
 * default 0, which reports at once. An allocation failure (-ENOMEM) is the host's, not the receiver's, and never waits.
 *
 * @param ms Grace in milliseconds; 0 for none.
 */
void rtl_device_set_unplug_owner_grace_ms(uint32_t ms);

/**
 * @brief The grace rtl_device_set_unplug_owner_grace_ms() set (default 0).
 *
 * @return Grace in milliseconds.
 */
uint32_t rtl_device_unplug_owner_grace_ms(void);

/**
 * @brief Create and initialize a remote RTL-SDR stream via rtl_tcp.
 *
 * Connects to an rtl_tcp server (default port 1234) and configures the
 * receiver via protocol commands. Sample bytes from the TCP stream are
 * widened and pushed into the same input ring used by the USB backend.
 *
 * @param host Remote hostname or IP (e.g., "127.0.0.1").
 * @param port Remote TCP port (e.g., 1234).
 * @param input_ring Pointer to input ring for incoming I/Q data.
 * @param autotune_enabled Enable rtl_tcp adaptive buffering/autotune at startup (1=on, 0=off).
 * @return Pointer to rtl_device handle, or NULL on failure.
 */
struct rtl_device* rtl_device_create_tcp(const char* host, int port, struct input_ring_state* input_ring,
                                         int autotune_enabled);

/** Native Airspy factory. Copies configuration; the ring must outlive the device.
 * Returns NULL on unavailable support, invalid settings, or driver failure. */
struct rtl_device* rtl_device_create_airspy(const dsd_airspy_config* config, struct input_ring_state* ring);
int rtl_device_airspy_info(struct rtl_device* dev, dsd_airspy_info* info);
int rtl_device_airspy_controls(struct rtl_device* dev, const dsd_airspy_config* config);
int rtl_device_airspy_list(uint64_t* serials, int capacity);
int rtl_device_airspy_set_preopened_fd(int fd);
int rtl_device_airspy_preopened_fd_in_use(void);

/**
 * @brief Create and initialize an RX source through SoapySDR.
 *
 * @param soapy_args Opaque Soapy device arguments string (may be empty).
 * @param input_ring Pointer to input ring for incoming I/Q data.
 * @return Pointer to rtl_device handle, or NULL on failure.
 */
struct rtl_device* rtl_device_create_soapy(const char* soapy_args, struct input_ring_state* input_ring);

/**
 * @brief Apply SoapySDR-specific profile and capability-aware startup options.
 *
 * Must be called after rtl_device_create_soapy() and before rtl_device_start_async().
 * Returns 0 for success, negative for failure/unsupported backend.
 */
int rtl_device_configure_soapy(struct rtl_device* dev, const struct rtl_soapy_config* config);

/**
 * @brief Destroy an RTL-SDR device and free resources.
 *
 * @param dev Pointer to rtl_device handle.
 */
void rtl_device_destroy(struct rtl_device* dev);

/**
 * @brief Set device center frequency.
 *
 * @param dev RTL-SDR device handle.
 * @param frequency Frequency in Hz.
 * @return 0 on success, negative on failure.
 */
int rtl_device_set_frequency(struct rtl_device* dev, uint32_t frequency);

/**
 * @brief Set device sample rate.
 *
 * @param dev RTL-SDR device handle.
 * @param samp_rate Sample rate in Hz.
 * @return 0 on success, negative on failure.
 */
int rtl_device_set_sample_rate(struct rtl_device* dev, uint32_t samp_rate);

/**
 * @brief Report the sample rate the backend would deliver for a requested rate.
 *
 * Pure query; the device is not reconfigured. Backends with a fixed rate grid answer
 * with the nearest supported entry, so the rate chain can choose decimation for the
 * rate that will actually arrive instead of the one that was asked for.
 *
 * @param dev RTL-SDR device handle.
 * @param requested Desired rate in Hz.
 * @param out_actual Receives the deliverable rate in Hz.
 * @return 0 on success, negative when the backend cannot answer.
 */
int rtl_device_nearest_supported_rate(struct rtl_device* dev, uint32_t requested, uint32_t* out_actual);

/**
 * @brief Query the current device sample rate.
 *
 * USB backend returns the actual rate reported by librtlsdr
 * (which may differ slightly from the requested value). The rtl_tcp
 * backend returns the last programmed value.
 *
 * @param dev RTL-SDR device handle.
 * @return Sample rate in Hz (non-negative) or negative on error.
 */
int rtl_device_get_sample_rate(struct rtl_device* dev);

/**
 * @brief Set tuner gain mode and value.
 *
 * On USB a gain change is one transaction over the RTL digital AGC, the tuner's gain mode and its gain: a change that
 * fails part way puts back the gain state the device last took, and when that fails too the gain reads as unknown
 * (rtl_device_is_auto_gain() -1, no rtl_device_get_tuner_gain() reading) until a later change takes. Test mode, which
 * shares a register with the digital AGC, stays on through gain changes.
 *
 * @param dev RTL-SDR device handle.
 * @param gain Gain in tenths of dB, or AUTO_GAIN for automatic.
 * @return 0 on success, negative on failure.
 */
int rtl_device_set_gain(struct rtl_device* dev, int gain);

/**
 * @brief Set tuner gain to the nearest supported value (manual mode).
 *
 * For USB (librtlsdr) backend, queries the list of supported gains and
 * applies the nearest value to the requested target; a gain table that
 * cannot be read is an error and changes nothing. For rtl_tcp, sends
 * the target value directly and lets the server handle quantization.
 *
 * @param dev RTL-SDR device handle.
 * @param target_tenth_db Target gain in tenths of dB (e.g., 180 for 18.0 dB).
 * @param out_selected_tenth_db Optional: the gain applied, written on success. It may be negative (an E4K's lowest is
 *        -10, an FC0012's -99), so only the return value says whether the change took.
 * @return 0 on success, negative on failure.
 */
int rtl_device_set_gain_nearest(struct rtl_device* dev, int target_tenth_db, int* out_selected_tenth_db);

/**
 * @brief Get current tuner gain in tenths of dB as reported by the driver.
 *
 * Writes the gain to @p out_tenth_db (270 for 27.0 dB; negative gains are real: an E4K's lowest is -10, an FC0012's
 * -99) and returns 0, or returns a negative value on error (DSD_ERR_NOT_SUPPORTED when the device or capture reports
 * none) and leaves @p out_tenth_db alone. When auto-gain is enabled, the gain may be undefined; prefer checking
 * rtl_device_is_auto_gain().
 */
int rtl_device_get_tuner_gain(struct rtl_device* dev, int* out_tenth_db);

/**
 * @brief Return 1 if auto-gain is enabled (requested), 0 if manual, -1 with no device or, on USB, while a failed gain
 * change left the gain unknown.
 */
int rtl_device_is_auto_gain(const struct rtl_device* dev);

/**
 * @brief Set frequency correction (PPM error).
 *
 * @param dev RTL-SDR device handle.
 * @param ppm_error PPM correction value.
 * @return 0 on success, negative on failure.
 */
int rtl_device_set_ppm(struct rtl_device* dev, int ppm_error);

/**
 * @brief Report whether the backend can apply a frequency (PPM) correction.
 *
 * RTL USB/TCP always can. SoapySDR devices are asked via `hasFrequencyCorrection`,
 * and IQ replay never can. Callers use this to skip auto-PPM on hardware where the
 * correction would be silently discarded.
 *
 * @param dev RTL-SDR device handle.
 * @return 1 when supported, 0 otherwise.
 */
int rtl_device_supports_ppm(struct rtl_device* dev);

/**
 * @brief Set direct sampling mode.
 *
 * @param dev RTL-SDR device handle.
 * @param on Direct sampling selection: 0=off, 1=I input, 2=Q input.
 * @return 0 on success, negative on failure.
 */
int rtl_device_set_direct_sampling(struct rtl_device* dev, int on);

/**
 * @brief Enable/disable offset tuning mode.
 *
 * @param dev RTL-SDR device handle.
 * @param on  1 to enable, 0 to disable.
 * @return 0 on success, negative on failure.
 */
int rtl_device_set_offset_tuning_enabled(struct rtl_device* dev, int on);

/**
 * @brief Report whether hardware offset tuning took on the device.
 *
 * @param dev RTL-SDR device handle.
 * @return 1 when it took, 0 when it is off or was refused.
 */
int rtl_device_get_offset_tuning(const struct rtl_device* dev);

/**
 * @brief Report how far librtlsdr displaces the LO while hardware offset tuning is on.
 *
 * @param dev RTL-SDR device handle.
 * @return The offset in Hz, or 0 when offset tuning did not take or the backend does not report it.
 */
uint32_t rtl_device_get_offset_tuning_hz(const struct rtl_device* dev);

/**
 * @brief Set tuner IF bandwidth if supported by the driver.
 * @param dev RTL-SDR device handle.
 * @param bw_hz Target bandwidth in Hz (e.g., 200000).
 * @return 0 on success, negative on failure.
 */
int rtl_device_set_tuner_bandwidth(struct rtl_device* dev, uint32_t bw_hz);

/**
 * @brief The tuner bandwidth to request while hardware offset tuning may be active.
 *
 * @param wanted_hz Bandwidth the configuration asks for (0 = driver automatic).
 * @param offset_active Whether hardware offset tuning is active.
 * @param offs_freq_hz LO offset while it is active (rtl_device_get_offset_tuning_hz()).
 * @return @p wanted_hz, or at least 2 * @p offs_freq_hz while offset tuning is active.
 */
uint32_t rtl_offset_aware_bandwidth_hz(uint32_t wanted_hz, int offset_active, uint32_t offs_freq_hz);

/**
 * @brief Reset device buffer.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success, negative on failure.
 */
int rtl_device_reset_buffer(struct rtl_device* dev);

/**
 * @brief Start asynchronous reading from the device.
 *
 * @param dev RTL-SDR device handle.
 * @param buf_len Buffer length for async read.
 * @return 0 on success, negative on failure.
 */
int rtl_device_start_async(struct rtl_device* dev, uint32_t buf_len);

/**
 * @brief Stop asynchronous reading and join the device thread.
 *
 * On a USB device it first latches the stop (whether or not a reader runs), so a recovery in progress gives up and
 * no reader starts afterwards (rtl_device_usb_resume_reader()); it then waits for that recovery to return.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success, negative on failure.
 */
int rtl_device_stop_async(struct rtl_device* dev);

/** A USB setter's result while the dongle is being reset and reopened (rtl_device_usb_recover_handle()): nothing was
 * changed, and the setting is not remembered. */
#define RTL_DEVICE_EBUSY (-16)

/**
 * @brief Whether a USB device can be reset and reopened when it stops delivering samples.
 *
 * Decided at open: the device's USB port was identified uniquely (no other device carries its manufacturer, product
 * and serial strings, every candidate could be read, the build has libusb) and the opened handle carries them. When
 * not, the open logged "RTL-SDR: stall recovery is off for this receiver: <reason>".
 *
 * @return 1 when eligible, 0 otherwise (including every non-USB device).
 */
int rtl_device_usb_recovery_eligible(const struct rtl_device* dev);

/**
 * @brief Reset and reopen a stalled USB dongle in place, and restore its settings.
 *
 * Stops the reader (its return is silent) and joins it, retires and closes the librtlsdr handle, resets the device's
 * USB port, finds it again (exactly one device with its strings, or the recovery fails before any open and recovery
 * stays off), reopens it, checks the reopened handle is the same device and replays the settings the
 * dongle had taken, in order: crystal, direct sampling, frequency, sample rate, offset tuning, tuner bandwidth, PPM,
 * frequency, gain (with test mode and digital AGC), IF gains, bias tee; then resets the FIFO. A control that never
 * took is left at the fresh dongle's default; one that took and cannot be restored fails the recovery. Only then is
 * the new handle published. The reader is not restarted (rtl_device_usb_resume_reader()). While it runs, setters
 * return RTL_DEVICE_EBUSY and change nothing, and getters report no reading once the old handle is retired.
 *
 * @param dev RTL-SDR device handle.
 * @param why Receives the reason on failure (may be NULL).
 * @param why_size Capacity of @p why.
 * @return 0 on success; -ECANCELED when a stop came first (silent); -ENOTSUP when the device is not eligible
 *         (nothing touched); another negative code on failure, with the device left without a handle.
 */
int rtl_device_usb_recover_handle(struct rtl_device* dev, char* why, size_t why_size);

/**
 * @brief Restart the USB reader after a successful recovery, unless a stop came first.
 *
 * Records the progress count as the restarted reader's baseline and bumps the progress monitor's rearm sequence
 * before the reader thread starts.
 *
 * @return 0 when the reader started; -ECANCELED when the device was stopped; another negative code on failure.
 */
int rtl_device_usb_resume_reader(struct rtl_device* dev);

/**
 * @brief Asks a stream's controller to recover the device's stalled receiver; called on the progress monitor's thread.
 *
 * It only queues the work: the controller takes the request (rtl_device_usb_recovery_take()), runs the stream boundary
 * and reports its result (rtl_device_usb_recovery_finish()).
 */
typedef void (*rtl_device_recovery_cb)(void* ctx);

/**
 * @brief Register (or, with NULL, clear) the stream's recovery callback.
 *
 * The progress monitor of an eligible USB receiver (rtl_device_usb_recovery_eligible()) posts a recovery request
 * through it once the receiver has sent no samples for 5 s, at most once per stall and not within 60 s of the previous
 * request. Without a callback the silence ends the session as before. Set before the stream starts and clear after it
 * stopped.
 */
void rtl_device_set_recovery_callback(struct rtl_device* dev, rtl_device_recovery_cb cb, void* ctx);

/**
 * @brief Take the recovery request the progress monitor posted, on the controller's thread.
 *
 * Logs that the recovery starts. A request the monitor withdrew (not taken within 2 s; the session then ended) cannot
 * be taken.
 *
 * @return 1 when the caller now owns the recovery and must finish it; 0 when there is nothing to take.
 */
int rtl_device_usb_recovery_take(struct rtl_device* dev);

/**
 * @brief Finish a recovery taken with rtl_device_usb_recovery_take().
 *
 * @p rc 0: the receiver was reset, reopened and its reader restarted; the progress monitor watches it afresh.
 * -ECANCELED (or any result once the device was stopped): silent. Anything else ends the session for the receiver
 * as a device failure, with @p why (the step that failed) in the logged cause.
 */
void rtl_device_usb_recovery_finish(struct rtl_device* dev, int rc, const char* why);

/**
 * @brief Let the device's producer deliver again after a stream boundary (USB: restart the reader,
 * rtl_device_usb_resume_reader()).
 *
 * @return 0 when it delivers again (or has nothing to resume); -ECANCELED when the device was stopped; another
 *         negative code on failure.
 */
int rtl_device_resume_stream(struct rtl_device* dev);

/**
 * @brief Whether the device has a stream at all (issue #518 D4), as against one that is only delayed.
 *
 * A USB receiver has none from the moment its progress monitor asks for a stall recovery until that recovery finished
 * (rtl_device_usb_recovery_take(), rtl_device_usb_recovery_finish()). rtl_tcp and every other backend report one.
 * Safe from any thread.
 *
 * @return 1 when the device has a stream, 0 when it has none or @p dev is NULL.
 */
int rtl_device_has_stream(const struct rtl_device* dev);

/**
 * @brief Mute the incoming raw USB/TCP byte stream for a short duration.
 *
 * Note: The argument is in raw input BYTES (u8 I/Q interleaved), not int16
 * samples. The requested mute span is rounded up to whole I/Q pairs, but the
 * internal remaining byte count may still go odd between rtl_tcp callbacks
 * because TCP read boundaries are arbitrary. If a prior chunk ended on an odd
 * byte boundary, the implementation may discard one additional future byte
 * internally so unmuted processing resumes on an I/Q boundary.
 *
 * @param dev RTL-SDR device handle.
 * @param bytes Number of input bytes to discard while muted. Odd values are
 *              rounded up so the muted span always covers whole I/Q pairs.
 */
void rtl_device_mute(struct rtl_device* dev, int bytes);

/**
 * @brief Enable or disable the RTL-SDR bias tee.
 *
 * USB (librtlsdr) backend uses `rtlsdr_set_bias_tee` when available at
 * build time. The rtl_tcp backend forwards the request to the server via
 * protocol command 0x0E.
 *
 * @param dev RTL-SDR device handle.
 * @param on  Non-zero to enable, zero to disable.
 * @return 0 on success, negative on failure.
 */
int rtl_device_set_bias_tee(struct rtl_device* dev, int on);

/**
 * @brief Print tuner type and whether this librtlsdr build is expected to support
 * hardware offset tuning for the detected tuner. For rtl_tcp backend, prints that
 * the capability is determined by the server.
 *
 * This is a non-invasive probe based on tuner type heuristics (e.g., upstream
 * librtlsdr disables offset tuning for R820T/R828D). Actual enablement is still
 * attempted later in the normal initialization sequence.
 *
 * @param dev RTL-SDR device handle.
 */
void rtl_device_print_offset_capability(struct rtl_device* dev);

/**
 * @brief Enable or disable rtl_tcp adaptive buffering at runtime.
 *
 * No-op for the USB backend. Updates are applied immediately for rtl_tcp.
 *
 * @param dev RTL-SDR device handle.
 * @param onoff Non-zero to enable autotune; zero to disable.
 * @return 0 on success; negative on error.
 */
int rtl_device_set_tcp_autotune(struct rtl_device* dev, int onoff);
/**
 * @brief Attach or detach an optional IQ capture writer.
 *
 * The stream/orchestrator owns the writer lifetime. The device stores only a
 * borrowed pointer used by ingestion callbacks.
 *
 * @param dev RTL-SDR device handle.
 * @param writer Borrowed writer pointer, or NULL to detach.
 */
void rtl_device_set_iq_capture_writer(struct rtl_device* dev, struct dsd_iq_capture_writer* writer);

/**
 * @brief Hold IQ capture submission while a live reconfigure timeline is being stamped.
 *
 * Samples received during the hold are omitted from capture data and recorded as mute spans.
 *
 * @param dev RTL-SDR device handle.
 */
void rtl_device_begin_capture_reconfigure(struct rtl_device* dev);

/**
 * @brief Release a capture reconfigure hold after reset/post-reset events are armed.
 *
 * @param dev RTL-SDR device handle.
 */
void rtl_device_end_capture_reconfigure(struct rtl_device* dev);

/**
 * @brief Increment capture retune diagnostics when a capture writer is attached.
 *
 * @param dev RTL-SDR device handle.
 */
void rtl_device_note_capture_retune(struct rtl_device* dev);
void rtl_device_record_capture_retune(struct rtl_device* dev, uint64_t center_frequency_hz,
                                      uint64_t capture_center_frequency_hz, uint32_t sample_rate_hz,
                                      const char* reason);
void rtl_device_record_capture_reset(struct rtl_device* dev, uint64_t center_frequency_hz,
                                     uint64_t capture_center_frequency_hz, uint32_t sample_rate_hz, const char* reason);

/**
 * @brief Snapshot replay/capture retune count accumulated by the device.
 *
 * @param dev RTL-SDR device handle.
 * @return Capture retune count.
 */
uint32_t rtl_device_get_capture_retune_count(struct rtl_device* dev);

/**
 * @brief Return backend-native sample format.
 *
 * Returns @ref dsd_iq_sample_format integral values (e.g., DSD_IQ_FORMAT_CU8).
 * Returns 0 when unknown/not yet selected.
 *
 * @param dev RTL-SDR device handle.
 * @return Sample format code or 0.
 */
int rtl_device_get_native_sample_format(const struct rtl_device* dev);

/**
 * @brief Set (or clear) RTL and tuner crystal reference frequencies.
 *
 * Pass 0 to leave either value unchanged at the driver default.
 * Applies to USB via librtlsdr and to rtl_tcp via protocol commands 0x0B/0x0C.
 *
 * @param dev RTL-SDR device handle.
 * @param rtl_xtal_hz RTL crystal frequency in Hz (0 to leave unchanged).
 * @param tuner_xtal_hz Tuner crystal frequency in Hz (0 to leave unchanged).
 * @return 0 on success; negative on failure.
 */
int rtl_device_set_xtal_freq(struct rtl_device* dev, uint32_t rtl_xtal_hz, uint32_t tuner_xtal_hz);

/**
 * @brief Enable or disable librtlsdr test mode (ramp signal instead of I/Q samples).
 *
 * Applies to USB (rtlsdr_set_testmode) and rtl_tcp (protocol cmd 0x07). On USB, test mode and the RTL digital AGC share
 * demod register 0x19: test mode wins while on, and turning it off puts the digital AGC back as the gain mode wants it.
 *
 * @param dev RTL-SDR device handle.
 * @param on Non-zero to enable test mode; zero to disable.
 * @return 0 on success; negative on failure.
 */
int rtl_device_set_testmode(struct rtl_device* dev, int on);

/**
 * @brief Set intermediate frequency (IF) gain for a specific stage.
 *
 * Stage index and gain units follow librtlsdr: gain in tenths of a dB.
 * Applies to USB (rtlsdr_set_tuner_if_gain) and rtl_tcp (protocol cmd 0x06).
 *
 * @param dev RTL-SDR device handle.
 * @param stage IF stage index (0-based).
 * @param gain_tenth_db Gain in tenths of a dB.
 * @return 0 on success; negative on failure.
 */
int rtl_device_set_if_gain(struct rtl_device* dev, int stage, int gain_tenth_db);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEVICE_H_ */
