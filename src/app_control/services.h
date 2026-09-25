// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief App-control service helpers invoked by frontend command handlers.
 *
 * These helpers mutate runtime options/state in response to UI actions,
 * handling validation and any required side effects (file opens, socket
 * connects, RTL restarts, etc.). Unless noted, functions return 0 on success
 * and a negative value on invalid inputs or failures.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SERVICES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SERVICES_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <stddef.h>

#ifdef USE_RADIO
#include <dsd-neo/core/airspy_config.h>
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Toggle all encrypted-audio mute flags (P25 + DMR left/right).
 */
int svc_toggle_all_mutes(dsd_opts* opts);
/**
 * @brief Enable per-call WAV capture, creating the output directory if needed.
 */
int svc_enable_per_call_wav(dsd_opts* opts, dsd_state* state);

/**
 * @brief Set the symbol capture output filename and open it for writing.
 */
int svc_open_symbol_out(dsd_opts* opts, dsd_state* state, const char* filename);
/**
 * @brief Open a captured symbol file for playback and switch input type.
 */
int svc_open_symbol_in(dsd_opts* opts, dsd_state* state, const char* filename);
/**
 * @brief Connect to a PCM16LE audio stream over TCP and configure libsndfile.
 */
int svc_tcp_connect_audio(dsd_opts* opts, const char* host, int port);
/**
 * @brief Connect to a rigctl server and enable rigctl control if successful.
 */
int svc_rigctl_connect(dsd_opts* opts, const char* host, int port);

// LRRP output file helpers
/**
 * @brief Enable LRRP logging to `$HOME/lrrp.txt`.
 */
int svc_lrrp_set_home(dsd_opts* opts); // ~/lrrp.txt
/**
 * @brief Enable LRRP logging to `./DSDPlus.LRRP`.
 */
int svc_lrrp_set_dsdp(dsd_opts* opts); // ./DSDPlus.LRRP
/**
 * @brief Enable LRRP logging to a user-specified file.
 */
int svc_lrrp_set_custom(dsd_opts* opts, const char* filename);
/**
 * @brief Disable LRRP logging and clear the configured output path.
 */
void svc_lrrp_disable(dsd_opts* opts);

// Misc toggles/actions
/**
 * @brief Reset both event-history rings to their initial empty state.
 */
void svc_reset_event_history(dsd_state* state);
/**
 * @brief Override Phase 2 system identifiers (WACN, SYSID, CC), clamped to valid ranges.
 */
void svc_set_p2_params(dsd_state* state, unsigned long long wacn, unsigned long long sysid, unsigned long long cc);

// Logging & file outputs
/** @brief Set the event log output path (enables logging). */
int svc_set_event_log(dsd_opts* opts, const char* path);
/** @brief Disable event logging and clear the path. */
void svc_disable_event_log(dsd_opts* opts);
/** @brief Configure static WAV output (single file, no stereo split) and open it. */
int svc_open_static_wav(dsd_opts* opts, dsd_state* state, const char* path);
/** @brief Configure RAW WAV output and open it. */
int svc_open_raw_wav(dsd_opts* opts, dsd_state* state, const char* path);
/** @brief Enable DSP debug output to the given filename under ./DSP/. */
int svc_set_dsp_output_file(dsd_opts* opts, const char* filename);

// Pulse/UDP output helpers
/** @brief Switch audio output to PulseAudio and select a device index/name. */
int svc_set_pulse_output(dsd_opts* opts, const char* index);
/** @brief Switch audio input to PulseAudio and select a device index/name. */
int svc_set_pulse_input(dsd_opts* opts, const char* index);
/** @brief Configure UDP audio output endpoint and enable it. */
int svc_udp_output_config(dsd_opts* opts, dsd_state* state, const char* host, int port);

// Trunking & control helpers
/** @brief Import a channel map CSV into runtime state. */
int svc_import_channel_map(dsd_opts* opts, dsd_state* state, const char* path);
/** @brief Import a group list CSV into runtime state. */
int svc_import_group_list(dsd_opts* opts, dsd_state* state, const char* path);
/** Adopt a nonempty source alias CSV; failure preserves the live list and path. */
int svc_import_src_list(dsd_opts* opts, dsd_state* state, const char* path);
/**
 * @brief Import a P25 band plan CSV (IDEN table) into the live state.
 *
 * Dry-runs the file first and refuses one that yields no usable row, so a
 * mispicked CSV cannot replace the stored plan. Refused under trunk scan, where
 * band plans come per target (p25_bandplan_csv). On success the path is
 * recorded in opts->p25_bandplan_in_file and pending P25 announcements are
 * re-resolved against the newly seeded tables.
 * @return 0 on success, -1 otherwise.
 */
int svc_import_p25_bandplan(dsd_opts* opts, dsd_state* state, const char* path);
/**
 * @brief Export the learned P25 band plan (live and, under trunk scan, parked IDEN tables) to @p path.
 * @return The number of rows written (>= 1), or -1 when there was nothing to write or the write failed.
 */
int svc_export_p25_bandplan(const dsd_opts* opts, const dsd_state* state, const char* path);
/** @brief Import keys from a decimal CSV. */
int svc_import_keys_dec(dsd_opts* opts, dsd_state* state, const char* path);
/** @brief Import keys from a hexadecimal CSV. */
int svc_import_keys_hex(dsd_opts* opts, dsd_state* state, const char* path);

/*
 * Unload counterparts. The importers all take a path and reject an empty one,
 * so a frontend whose system can deselect a CSV has no way to say "none"
 * without these; the previous file would otherwise stay live for the session.
 * Each returns 0 when the data is gone afterwards, -1 when it could not be.
 */
/** @brief Drop the runtime channel map, its LCN list and its trust bytes. */
int svc_clear_channel_map(dsd_opts* opts, dsd_state* state);
/** @brief Drop every loaded talkgroup entry. */
int svc_clear_group_list(dsd_opts* opts, dsd_state* state);
/** Clear the source alias list and its recorded path. */
int svc_clear_src_list(dsd_opts* opts, dsd_state* state);
/** @brief Drop the keyring and disarm the key loader (covers dec and hex). */
int svc_clear_keys(dsd_opts* opts, dsd_state* state);
/** @brief Set the current talkgroup hold value. */
void svc_set_tg_hold(dsd_state* state, unsigned tg);
/** @brief Set trunking hang time (seconds, clamped to >=0). */
void svc_set_hangtime(dsd_opts* opts, double seconds);
/** @brief Set rigctl setmod bandwidth (clamped to 0..25 kHz). */
void svc_set_rigctl_setmod_bw(dsd_opts* opts, int hz);
/** @brief Toggle reverse mute (mute when unmuted, unmute when muted). */
void svc_toggle_reverse_mute(dsd_opts* opts);
/** @brief Toggle P25 LCW retune helper. */
void svc_toggle_lcw_retune(dsd_opts* opts);
/** @brief Toggle little-endian DMR symbol ordering. */
void svc_toggle_dmr_le(dsd_opts* opts);
/** @brief Set slot preference (0=slot 1, 1=slot 2, 2=auto). */
void svc_set_slot_pref(dsd_opts* opts, int pref);
/** @brief Enable/disable slots using bitmask (bit0=slot1, bit1=slot2). */
void svc_set_slots_onoff(dsd_opts* opts, int mask);
/** @brief Gate trunk scan on voice: leave a signal that carries no voice. */
void svc_set_scan_voice_only(dsd_opts* opts, int on);
/** @brief Voice qualify window in ms (clamped to 100..600000). */
void svc_set_scan_voice_qualify_ms(dsd_opts* opts, int ms);
/** @brief Voice hold time in ms (clamped to 100..600000). */
void svc_set_scan_voice_hold_ms(dsd_opts* opts, int ms);

// Symbol profile
/**
 * @brief Make the SPS hunt and the front end agree with the decoder's timing.
 *
 * The caller has already put @c samplesPerSymbol / @c symbolCenter on the timing
 * @p profile calls for; this publishes that decision to everything downstream of
 * it. The hunt is restarted from the profile's index, because left on the
 * previous mode's it overwrites the freshly computed timing on its next pass, and
 * on an RTL front end the demodulator family and channel filter are queued to
 * match — otherwise the decoder is looking for one protocol through the filter of
 * another.
 *
 * For handlers that change the decoder in place. Ones that retune are already
 * served by the trunk tuning hook, which stages a profile with the retune, and
 * a second request from here would fight it.
 */
void svc_publish_symbol_profile(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile);

/**
 * @brief Ask a running RTL front end, before a decode-mode change commits, whether it takes the receive profile
 * @p mode will publish.
 *
 * Only a mode that moves the front end onto the analog family can be refused (an analog width the running demod rate
 * cannot realize, for one): rtl_stream_check_analog_profile() holds it to the running stream's rate and logs a
 * refusal with the validator's text. A caller that gets -1 leaves the decoder's mode as it was, so the decoder and the
 * front end agree about the family. The request svc_publish_symbol_profile() makes once the caller has committed is
 * held to the same rules again, at the rate the stream runs when it is made and when it lands: only a retune that
 * moves the rate in between gets it refused there, logged, with the front end kept on its receive profile rather than
 * running the width without its channel filter, and with the decoder left on the mode it committed to.
 *
 * @return 0 when the front end would take it, or when @p mode publishes no analog profile here (a digital mode, the
 *         M17 encoder, no running RTL stream, or a scope update that defers the publish); -1 when it would refuse it.
 */
int svc_check_mode_receive_profile(const dsd_opts* opts, const dsd_state* state, dsdneoUserDecodeMode mode);

/**
 * @brief Note the configured digital decode modes with a running RTL front end.
 *
 * Once a live switch has moved the front end onto the digital family, the options its stream opened with no longer name
 * the modes it runs, and the modes noted here pick the FSK channel profile its CQPSK toggle returns to
 * (rtl_stream_set_digital_decode_modes()). svc_publish_symbol_profile() notes them with every profile it publishes; a
 * handler that changes the digital modes without publishing one (a config apply whose [mode] stays digital) calls this.
 * Nothing is noted for the analog family, off an RTL front end, or from a running scan row's options, which carry the
 * row's constraint rather than the configured modes.
 */
void svc_note_digital_decode_modes(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Check an NFM channel width against the receive front end it would run on, before anything changes.
 *
 * @p width_hz is the full RF channel-filter width in Hz, or 0 for the default (runtime/analog_channel.h). A width
 * outside 8000..25000 Hz is refused. An explicit width is also held to the DSP rate it would run at: with a running
 * RTL-family stream, the front end's own check at its published demod rate (rtl_stream_check_analog_profile(), which
 * also logs a refusal with the validator's text); without one, the rate an RTL-SDR or rtl_tcp input's DSP bandwidth
 * (rtl_dsp_bw_khz) gives. Other inputs are checked by their next stream start, against the rate the device delivers.
 * The unset default is never refused for its rate. Callers decide whether the width is in use; this only says whether
 * the front end would take it. A rate refusal's reason names the width, the rate, the widest width that rate filters
 * and the fix: the DSP bandwidths that would fit on an RTL-SDR or rtl_tcp input, or narrowing the width where a running
 * SoapySDR, Airspy or I/Q replay stream's device or capture forces its demod rate (the rate named is then the one the
 * stream publishes). The validator's full text is logged (by the front end, with a stream running).
 *
 * @param why      Receives a short reason on refusal, for a toast (may be NULL).
 * @param why_size Size of @p why.
 * @return 0 when the width may be applied, -1 otherwise.
 */
int svc_check_nfm_bandwidth(const dsd_opts* opts, const dsd_state* state, int width_hz, char* why, size_t why_size);

/**
 * @brief Check an NFM channel width against an RTL-SDR or rtl_tcp input reopened at DSP bandwidth @p rtl_bw_khz.
 *
 * For a change that reopens the device at another DSP bandwidth (a config apply whose [input] sets rtl_bw_khz), where
 * the running stream's rate says nothing about the rate the width will run at. The range is checked as
 * svc_check_nfm_bandwidth() checks it; an explicit width must then fit the rate @p rtl_bw_khz gives, and a refusal is
 * reported and logged the same way. The unset default (0) and @p rtl_bw_khz <= 0 are never refused for a rate.
 *
 * @return 0 when the width may be applied, -1 otherwise (reason in @p why, may be NULL).
 */
int svc_check_nfm_bandwidth_for_rtl_bw(int width_hz, int rtl_bw_khz, char* why, size_t why_size);

/**
 * @brief Set the configured NFM channel width (DSD_APP_CMD_NFM_BANDWIDTH_SET), live when the analog monitor runs.
 *
 * Refuses, and changes nothing, a width outside 0 or 8000..25000 Hz, and, while the NFM preset uses the width, one the
 * front end would refuse (svc_check_nfm_bandwidth()). An accepted width is stored and handed to a running RTL front end
 * (svc_publish_nfm_bandwidth()): on the analog monitor a width-only change redesigns the channel filter from empty
 * histories at the next block. Anywhere else (a digital session, a typed digital scan row on an analog session, CQPSK
 * toggled on under -fA, a stopped stream) the stored width applies the next time the analog profile is requested or
 * the stream opens. Decoder thread only.
 *
 * @return 0 when stored, -1 when refused (reason in @p why).
 */
int svc_set_nfm_bandwidth(dsd_opts* opts, const dsd_state* state, int width_hz, char* why, size_t why_size);

/**
 * @brief Hand the configured NFM width to a running RTL front end, as a live analog profile request.
 *
 * Does nothing unless the options in force run the -fA NFM preset with a stream running. A switch onto the analog
 * family, or a CQPSK toggle back to it, that the demod thread has not taken yet has its queued width replaced. Under a
 * scan row's suspended scope (dsd_scan_mode_updating()) it waits: the scoped command dispatcher calls it again once the
 * row's constraint is back, so a typed digital row keeps its own profile. CQPSK toggled on under -fA keeps the front
 * end off the monitor, whether the demod thread has taken that toggle yet or not; svc_toggle_rtl_cqpsk() turning it
 * off requests the analog profile with the configured width. For callers that changed the width (the width command, a
 * config apply). Decoder thread only: it reads and keeps the record of the receive requests queued from it.
 */
void svc_publish_nfm_bandwidth(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief The DSP menu's CQPSK toggle on a running RTL front end (DSD_APP_DSP_OP_TOGGLE_CQ).
 *
 * Flips the CQPSK state the front end was last asked for (by an earlier toggle or a published symbol profile the demod
 * thread may not have taken yet, otherwise the state it publishes) and queues it for the demod thread, leaving the
 * symbol profile and timing alone. Turning CQPSK off under -fA returns to the analog monitor through the analog profile
 * with the configured channel width. Decoder thread only.
 */
void svc_toggle_rtl_cqpsk(const dsd_opts* opts);

// Per-protocol inversion toggles
/** @brief Toggle X2-TDMA symbol inversion. */
void svc_toggle_inv_x2(dsd_opts* opts);
/** @brief Toggle DMR symbol inversion. */
void svc_toggle_inv_dmr(dsd_opts* opts);
/** @brief Toggle dPMR symbol inversion. */
void svc_toggle_inv_dpmr(dsd_opts* opts);
/** @brief Toggle M17 symbol inversion. */
void svc_toggle_inv_m17(dsd_opts* opts);

#ifdef USE_RADIO
// RTL-SDR configuration and lifecycle helpers
/** @brief Switch active input to RTL-SDR and restart the stream. */
int svc_rtl_enable_input(dsd_opts* opts, dsd_state* state);
/**
 * @brief Check the configured explicit analog width against the RTL-SDR input DSD_APP_CMD_RTL_ENABLE_INPUT would open.
 *
 * Asked before the switch rewrites the input and tears down the running stream. The configured analog preset's
 * explicit width (as RTL_SET_BW holds it, a typed digital scan row included) must fit the rate the RTL DSP bandwidth
 * (rtl_dsp_bw_khz) gives the device the switch opens: an RTL-SDR from an Airspy spec, "pulse" or any other device
 * string, rtl_tcp from an rtl_tcp spec. A SoapySDR or I/Q replay input is reopened at a rate its device or capture
 * sets, which its start checks. The unset default is never refused for a rate. A refusal's reason names the width,
 * the rate, the widest width it filters and the DSP bandwidths that would fit; the validator's text is logged.
 *
 * @return 0 when the switch may go ahead, -1 otherwise (reason in @p why, may be NULL).
 */
int svc_check_rtl_input_analog_width(const dsd_opts* opts, const dsd_state* state, char* why, size_t why_size);
/** @brief Restart the RTL stream if active, tearing down any existing context. */
int svc_rtl_restart(dsd_opts* opts, dsd_state* state);
/** Restart without acquiring; caller holds the P25 SM tick guard. */
int svc_rtl_restart_locked(dsd_opts* opts, dsd_state* state);
int svc_airspy_apply(dsd_opts* opts, dsd_state* state, const dsd_airspy_config* config);

typedef struct {
    uint32_t frequency;
    int bandwidth;
    double squelch;
    int volume;
} svc_airspy_tuning;

/** Apply native settings and shared tuning together; restore prior tuning on failure.
 * opts holds the requested tuning and the previous native settings on entry. */
int svc_airspy_apply_config(dsd_opts* opts, dsd_state* state, const dsd_airspy_config* config,
                            const svc_airspy_tuning* previous_tuning);
/** Apply and roll back without acquiring; caller holds the P25 SM tick guard. */
int svc_airspy_apply_config_locked(dsd_opts* opts, dsd_state* state, const dsd_airspy_config* config,
                                   const svc_airspy_tuning* previous_tuning);
/** @brief Set RTL device index and mark stream for restart (applied immediately if active). */
int svc_rtl_set_dev_index(dsd_opts* opts, dsd_state* state, int index);
/** @brief Tune receiver frequency (Hz); caller owns trunking and call bookkeeping. */
int svc_rtl_set_freq(dsd_opts* opts, dsd_state* state, uint32_t hz);
/** @brief Set RTL manual gain (0–49), clamping and restarting if needed. */
int svc_rtl_set_gain(dsd_opts* opts, dsd_state* state, int value);
/**
 * @brief Set RTL DSP baseband bandwidth (kHz: 4,6,8,12,16,24,48), restarting if needed.
 *
 * An unsupported value becomes 48. A bandwidth the explicit analog channel width in use cannot run at (the analog
 * preset on an RTL-SDR or rtl_tcp input, whose DSP rate this sets) is refused and nothing changes: the width is never
 * clamped to fit. @p why receives a short reason naming both values, the widest width the bandwidth filters and the
 * fix (narrow the width first) on that refusal (may be NULL); the validator's full text is logged.
 */
int svc_rtl_set_bandwidth(dsd_opts* opts, dsd_state* state, int khz, char* why, size_t why_size);
/**
 * @brief Set the RTL squelch threshold from a decibel value.
 *
 * Negative values are a threshold in dB. Zero or above switches the squelch off,
 * the same meaning 0 carries in the `sql` field of an input string and in the
 * `rtl_sql` config key; a 0 dB threshold is full scale and would never open.
 * The value is the configured default: while a scan row or target overrides the
 * squelch, the row's threshold stays in force (dsd_scan_mode_set_configured_squelch()).
 * @p state may be NULL when no scan scope can exist. Although @p state is const, the
 * call writes the scan scope attached to it: call it only on the decoder thread with
 * the live state, never with a frontend snapshot (dsd_app_get_latest_snapshot()).
 */
int svc_rtl_set_sql_db(dsd_opts* opts, const dsd_state* state, double dB);
/** @brief Set RTL monitor/non-symbol gain multiplier (clamped to 0–3). */
int svc_rtl_set_volume_mult(dsd_opts* opts, int mult);
/** @brief Toggle RTL bias tee (applied live when stream active). */
int svc_rtl_set_bias_tee(dsd_opts* opts, const dsd_state* state, int on);
/** @brief Toggle RTL-TCP adaptive networking and propagate to env/stream. */
int svc_rtltcp_set_autotune(dsd_opts* opts, const dsd_state* state, int on);
/** @brief Toggle carrier/error-based auto PPM and propagate to env/stream. */
int svc_rtl_set_auto_ppm(dsd_opts* opts, const dsd_state* state, int on);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SERVICES_H_ */
