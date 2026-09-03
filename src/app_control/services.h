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
#include <dsd-neo/runtime/decode_mode.h>

#ifdef USE_RADIO
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
/** @brief Restart the RTL stream if active, tearing down any existing context. */
int svc_rtl_restart(dsd_opts* opts, dsd_state* state);
/** @brief Set RTL device index and mark stream for restart (applied immediately if active). */
int svc_rtl_set_dev_index(dsd_opts* opts, dsd_state* state, int index);
/** @brief Tune RTL center frequency (Hz), applying live when stream active. */
int svc_rtl_set_freq(dsd_opts* opts, dsd_state* state, uint32_t hz);
/** @brief Set RTL manual gain (0–49), clamping and restarting if needed. */
int svc_rtl_set_gain(dsd_opts* opts, dsd_state* state, int value);
/** @brief Set RTL DSP baseband bandwidth (kHz: 4,6,8,12,16,24,48), clamping and restarting if needed. */
int svc_rtl_set_bandwidth(dsd_opts* opts, dsd_state* state, int khz);
/**
 * @brief Set the RTL squelch threshold from a decibel value.
 *
 * Negative values are a threshold in dB. Zero or above switches the squelch off,
 * the same meaning 0 carries in the `sql` field of an input string and in the
 * `rtl_sql` config key; a 0 dB threshold is full scale and would never open.
 */
int svc_rtl_set_sql_db(dsd_opts* opts, double dB);
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
