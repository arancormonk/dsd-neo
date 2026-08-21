// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Dynamic label generators and visibility predicates for menu items.
 *
 * Labels follow one grammar: `Noun [State]` for toggles (On/Off), `Noun... [current]`
 * for rows that open a prompt, a bare `Prefix: value` for read-only status rows.
 *
 * This header is internal to src/ui/terminal/ and should NOT be installed.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_MENU_LABELS_H_
#define DSD_NEO_SRC_UI_TERMINAL_MENU_LABELS_H_

#include <stdbool.h>
#include <stddef.h>

// ---- Visibility/predicate functions ----
bool io_always_on(const void* ctx);
bool io_rtl_active(const void* ctx);
bool trunk_enabled(const void* ctx);
bool provoice_active(const void* ctx);
bool rr_feature_available(const void* ctx);
bool rr_key_prompt_offered(const void* ctx);
bool rr_imports_available(const void* ctx);
bool rr_imports_available_and_feature(const void* ctx);
bool rr_key_prompt_offered_and_feature(const void* ctx);

#ifdef USE_RADIO
bool is_mod_qpsk(const void* v);
bool is_not_qpsk(const void* v);
bool is_ted_allowed(const void* v);
int ui_current_mod(const void* v);
#endif

// ---- Label functions ----
// Decoder
const char* lbl_decode_mode(const void* v, char* b, size_t n);
const char* lbl_modulation(const void* v, char* b, size_t n);
const char* lbl_p25p2_mod_lock(const void* v, char* b, size_t n);
const char* lbl_lpf(const void* v, char* b, size_t n);
const char* lbl_hpf(const void* v, char* b, size_t n);
const char* lbl_pbf(const void* v, char* b, size_t n);
const char* lbl_hpf_d(const void* v, char* b, size_t n);
const char* lbl_crc_relax(const void* v, char* b, size_t n);
const char* lbl_dmr_le(const void* v, char* b, size_t n);
const char* lbl_slot1(const void* v, char* b, size_t n);
const char* lbl_slot2(const void* v, char* b, size_t n);
const char* lbl_slotpref(const void* v, char* b, size_t n);
const char* lbl_slots_on(const void* v, char* b, size_t n);
const char* lbl_provoice_esk(const void* v, char* b, size_t n);
const char* lbl_provoice_mode(const void* v, char* b, size_t n);
const char* lbl_invert_all(const void* v, char* b, size_t n);
const char* lbl_inv_x2(const void* v, char* b, size_t n);
const char* lbl_inv_dmr(const void* v, char* b, size_t n);
const char* lbl_inv_dpmr(const void* v, char* b, size_t n);
const char* lbl_inv_m17(const void* v, char* b, size_t n);
const char* lbl_m17_user_data(const void* v, char* b, size_t n);

// Trunking
const char* lbl_trunk(const void* v, char* b, size_t n);
const char* lbl_scan(const void* v, char* b, size_t n);
const char* lbl_lcw(const void* v, char* b, size_t n);
const char* lbl_allow(const void* v, char* b, size_t n);
const char* lbl_tune_group(const void* v, char* b, size_t n);
const char* lbl_tune_priv(const void* v, char* b, size_t n);
const char* lbl_tune_data(const void* v, char* b, size_t n);
const char* lbl_tg_hold(const void* v, char* b, size_t n);
const char* lbl_hangtime(const void* v, char* b, size_t n);
const char* lbl_rev_mute(const void* v, char* b, size_t n);
const char* lbl_pref_cc(const void* v, char* b, size_t n);
const char* lbl_rigctl(const void* vctx, char* b, size_t n);
const char* lbl_rr_account(const void* v, char* b, size_t n);
const char* lbl_p25_vc_grace(const void* v, char* b, size_t n);
const char* lbl_p25_min_follow(const void* v, char* b, size_t n);
const char* lbl_p25_grant_voice(const void* v, char* b, size_t n);
const char* lbl_p25_cc_grace(const void* v, char* b, size_t n);
const char* lbl_p25_force_extra(const void* v, char* b, size_t n);
const char* lbl_p25_force_margin(const void* v, char* b, size_t n);
const char* lbl_p25_p1_err_pct(const void* v, char* b, size_t n);
const char* lbl_p25_p1_err_sec(const void* v, char* b, size_t n);

// Encryption
const char* lbl_muting(const void* v, char* b, size_t n);
const char* lbl_p25_enc_lockout(const void* v, char* b, size_t n);
const char* lbl_enc_lockout_clear(const void* v, char* b, size_t n);
const char* lbl_key_force_bp(const void* v, char* b, size_t n);
const char* lbl_key_force_rc4(const void* v, char* b, size_t n);
const char* lbl_key_hytera(const void* v, char* b, size_t n);

// Input / Audio
const char* lbl_current_input(const void* vctx, char* b, size_t n);
const char* lbl_current_output(const void* vctx, char* b, size_t n);
const char* lbl_input_volume(const void* vctx, char* b, size_t n);
const char* lbl_input_warn(const void* v, char* b, size_t n);
const char* lbl_tcp(const void* vctx, char* b, size_t n);
const char* lbl_out_mute(const void* vctx, char* b, size_t n);
const char* lbl_gain_dig(const void* v, char* b, size_t n);
const char* lbl_gain_ana(const void* v, char* b, size_t n);
const char* lbl_monitor(const void* v, char* b, size_t n);
const char* lbl_cosine(const void* v, char* b, size_t n);
const char* lbl_deemph(const void* v, char* b, size_t n);
const char* lbl_audio_lpf(const void* v, char* b, size_t n);
const char* lbl_call_alert(const void* v, char* b, size_t n);
const char* lbl_call_alert_events(const void* v, char* b, size_t n);

// Recording & logs
const char* lbl_sym_save(const void* vctx, char* b, size_t n);
const char* lbl_stop_symbol_capture(const void* vctx, char* b, size_t n);
const char* lbl_replay_last(const void* vctx, char* b, size_t n);
const char* lbl_stop_symbol_playback(const void* vctx, char* b, size_t n);
const char* lbl_per_call_wav(const void* vctx, char* b, size_t n);
const char* lbl_event_log(const void* v, char* b, size_t n);
const char* lbl_toggle_payload(const void* v, char* b, size_t n);
const char* lbl_lrrp_current(const void* vctx, char* b, size_t n);

// Display
const char* lbl_ui_compact(const void* v, char* b, size_t n);
const char* lbl_ui_channels(const void* v, char* b, size_t n);
const char* lbl_ui_p25_metrics(const void* v, char* b, size_t n);
const char* lbl_ui_p25_affil(const void* v, char* b, size_t n);
const char* lbl_ui_p25_ga(const void* v, char* b, size_t n);
const char* lbl_ui_p25_neighbors(const void* v, char* b, size_t n);
const char* lbl_ui_p25_iden(const void* v, char* b, size_t n);
const char* lbl_ui_p25_ccc(const void* v, char* b, size_t n);
const char* lbl_ui_p25_callsign(const void* v, char* b, size_t n);
const char* lbl_vis_const(const void* v, char* b, size_t n);
const char* lbl_vis_const_norm(const void* v, char* b, size_t n);
const char* lbl_vis_eye(const void* v, char* b, size_t n);
const char* lbl_vis_eye_unicode(const void* v, char* b, size_t n);
const char* lbl_vis_eye_color(const void* v, char* b, size_t n);
const char* lbl_vis_fsk(const void* v, char* b, size_t n);
const char* lbl_vis_spectrum(const void* v, char* b, size_t n);
const char* lbl_history_mode(const void* v, char* b, size_t n);
const char* lbl_history_slot(const void* v, char* b, size_t n);

// Advanced
const char* lbl_ftz_daz(const void* v, char* b, size_t n);
const char* lbl_window_freeze(const void* v, char* b, size_t n);
const char* lbl_rt_sched(const void* v, char* b, size_t n);
const char* lbl_mt(const void* v, char* b, size_t n);
const char* lbl_auto_ppm_snr(const void* v, char* b, size_t n);
const char* lbl_auto_ppm_pwr(const void* v, char* b, size_t n);
const char* lbl_auto_ppm_zeroppm(const void* v, char* b, size_t n);
const char* lbl_auto_ppm_zerohz(const void* v, char* b, size_t n);
const char* lbl_auto_ppm_freeze(const void* v, char* b, size_t n);
const char* lbl_tcp_prebuf(const void* v, char* b, size_t n);
const char* lbl_tcp_rcvbuf(const void* v, char* b, size_t n);
const char* lbl_tcp_rcvtimeo(const void* v, char* b, size_t n);
const char* lbl_tcp_waitall(const void* v, char* b, size_t n);

// RTL-SDR and DSP (USE_RADIO only)
#ifdef USE_RADIO
const char* lbl_rtl_freq(const void* v, char* b, size_t n);
const char* lbl_rtl_gain(const void* v, char* b, size_t n);
const char* lbl_rtl_ppm(const void* v, char* b, size_t n);
const char* lbl_rtl_bw(const void* v, char* b, size_t n);
const char* lbl_rtl_vol(const void* v, char* b, size_t n);
const char* lbl_rtl_bias(const void* v, char* b, size_t n);
const char* lbl_rtl_rtltcp_autotune(const void* v, char* b, size_t n);
const char* lbl_rtl_auto_ppm(const void* v, char* b, size_t n);
const char* lbl_rtl_tuner_autogain(const void* v, char* b, size_t n);
const char* lbl_onoff_cq(const void* v, char* b, size_t n);
const char* lbl_onoff_iqbal(const void* v, char* b, size_t n);
const char* lbl_iq_dc(const void* v, char* b, size_t n);
const char* lbl_iq_dc_k(const void* v, char* b, size_t n);
const char* lbl_ted_gain(const void* v, char* b, size_t n);
const char* lbl_cqpsk_timing_bias(const void* v, char* b, size_t n);
const char* lbl_dsp_panel(const void* v, char* b, size_t n);
#endif

#endif /* DSD_NEO_SRC_UI_TERMINAL_MENU_LABELS_H_ */
