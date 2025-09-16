// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <dsd-neo/core/dsd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Generic service helpers used by UI (and reusable by other front-ends).

int svc_toggle_all_mutes(dsd_opts* opts);
int svc_toggle_call_alert(dsd_opts* opts);

int svc_enable_per_call_wav(dsd_opts* opts, dsd_state* state);

int svc_open_symbol_out(dsd_opts* opts, dsd_state* state, const char* filename);
int svc_open_symbol_in(dsd_opts* opts, dsd_state* state, const char* filename);
int svc_replay_last_symbol(dsd_opts* opts, dsd_state* state);
void svc_stop_symbol_playback(dsd_opts* opts);
void svc_stop_symbol_saving(dsd_opts* opts, dsd_state* state);

int svc_tcp_connect_audio(dsd_opts* opts, const char* host, int port);
int svc_rigctl_connect(dsd_opts* opts, const char* host, int port);

// LRRP output file helpers
int svc_lrrp_set_home(dsd_opts* opts); // ~/lrrp.txt
int svc_lrrp_set_dsdp(dsd_opts* opts); // ./DSDPlus.LRRP
int svc_lrrp_set_custom(dsd_opts* opts, const char* filename);
void svc_lrrp_disable(dsd_opts* opts);

// Decode mode presets
int svc_mode_auto(dsd_opts* opts, dsd_state* state);
int svc_mode_tdma(dsd_opts* opts, dsd_state* state);
int svc_mode_dstar(dsd_opts* opts, dsd_state* state);
int svc_mode_m17(dsd_opts* opts, dsd_state* state);
int svc_mode_edacs(dsd_opts* opts, dsd_state* state);
int svc_mode_p25p2(dsd_opts* opts, dsd_state* state);
int svc_mode_dpmr(dsd_opts* opts, dsd_state* state);
int svc_mode_nxdn48(dsd_opts* opts, dsd_state* state);
int svc_mode_nxdn96(dsd_opts* opts, dsd_state* state);
int svc_mode_dmr(dsd_opts* opts, dsd_state* state);
int svc_mode_ysf(dsd_opts* opts, dsd_state* state);

// Misc toggles/actions
void svc_toggle_inversion(dsd_opts* opts);
void svc_reset_event_history(dsd_state* state);
void svc_toggle_payload(dsd_opts* opts);
void svc_set_p2_params(dsd_state* state, unsigned long long wacn, unsigned long long sysid, unsigned long long cc);

// Logging & file outputs
int svc_set_event_log(dsd_opts* opts, const char* path);
void svc_disable_event_log(dsd_opts* opts);
int svc_open_static_wav(dsd_opts* opts, dsd_state* state, const char* path);
int svc_open_raw_wav(dsd_opts* opts, dsd_state* state, const char* path);
int svc_set_dsp_output_file(dsd_opts* opts, const char* filename);

// Pulse/UDP output helpers
int svc_set_pulse_output(dsd_opts* opts, const char* index);
int svc_set_pulse_input(dsd_opts* opts, const char* index);
int svc_udp_output_config(dsd_opts* opts, dsd_state* state, const char* host, int port);

// Trunking & control helpers
void svc_toggle_trunking(dsd_opts* opts);
void svc_toggle_scanner(dsd_opts* opts);
int svc_import_channel_map(dsd_opts* opts, dsd_state* state, const char* path);
int svc_import_group_list(dsd_opts* opts, dsd_state* state, const char* path);
int svc_import_keys_dec(dsd_opts* opts, dsd_state* state, const char* path);
int svc_import_keys_hex(dsd_opts* opts, dsd_state* state, const char* path);
void svc_toggle_tune_group(dsd_opts* opts);
void svc_toggle_tune_private(dsd_opts* opts);
void svc_toggle_tune_data(dsd_opts* opts);
void svc_set_tg_hold(dsd_state* state, unsigned tg);
void svc_set_hangtime(dsd_opts* opts, double seconds);
void svc_set_rigctl_setmod_bw(dsd_opts* opts, int hz);
void svc_toggle_reverse_mute(dsd_opts* opts);
void svc_toggle_crc_relax(dsd_opts* opts);
void svc_toggle_lcw_retune(dsd_opts* opts);
void svc_toggle_dmr_le(dsd_opts* opts);
void svc_set_slot_pref(dsd_opts* opts, int pref01);
void svc_set_slots_onoff(dsd_opts* opts, int mask);

// Per-protocol inversion toggles
void svc_toggle_inv_x2(dsd_opts* opts);
void svc_toggle_inv_dmr(dsd_opts* opts);
void svc_toggle_inv_dpmr(dsd_opts* opts);
void svc_toggle_inv_m17(dsd_opts* opts);

#ifdef USE_RTLSDR
// RTL-SDR configuration and lifecycle helpers
int svc_rtl_enable_input(dsd_opts* opts);
int svc_rtl_restart(dsd_opts* opts);
int svc_rtl_set_dev_index(dsd_opts* opts, int index);
int svc_rtl_set_freq(dsd_opts* opts, uint32_t hz);
int svc_rtl_set_gain(dsd_opts* opts, int value);
int svc_rtl_set_ppm(dsd_opts* opts, int ppm);
int svc_rtl_set_bandwidth(dsd_opts* opts, int khz);
int svc_rtl_set_sql_db(dsd_opts* opts, double dB);
int svc_rtl_set_volume_mult(dsd_opts* opts, int mult);
#endif

#ifdef __cplusplus
}
#endif
