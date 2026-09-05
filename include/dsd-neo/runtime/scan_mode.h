// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Scoped scanner decoder classes, independent of global CLI preset IDs. */
#ifndef DSD_NEO_RUNTIME_SCAN_MODE_H
#define DSD_NEO_RUNTIME_SCAN_MODE_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_SCAN_MODE_INHERIT = 0,
    DSD_SCAN_MODE_P25,
    DSD_SCAN_MODE_DMR,
    DSD_SCAN_MODE_NXDN96,
    DSD_SCAN_MODE_NXDN48,
    DSD_SCAN_MODE_DPMR,
    DSD_SCAN_MODE_DSTAR,
    DSD_SCAN_MODE_YSF,
    DSD_SCAN_MODE_M17
} dsd_scan_mode;

/** Target modulation precedence shared by scope reapplication and trunk entry. */
typedef enum {
    DSD_SCAN_MODULATION_INHERIT = 0,
    DSD_SCAN_MODULATION_AUTO,
    DSD_SCAN_MODULATION_C4FM,
    DSD_SCAN_MODULATION_CQPSK,
    DSD_SCAN_MODULATION_GFSK
} dsd_scan_modulation;

/** Scalar snapshot of the exact configured decoder settings; owns no pointers. */
typedef struct {
    int force_key;
    int aggressive_framesync;
    int dmr_crc_relaxed_default;
    int scan_voice_only;
    int scan_voice_qualify_ms;
    int scan_voice_hold_ms;
    int dmr_mute_encL;
    int dmr_mute_encR;
    int unmute_encrypted_p25;
    char group_in_file[1024];
    int frame_dstar;
    int frame_x2tdma;
    int frame_p25p1;
    int frame_p25p2;
    int frame_nxdn48;
    int frame_nxdn96;
    int frame_dmr;
    int frame_dpmr;
    int frame_provoice;
    int frame_ysf;
    int frame_m17;
    int mod_c4fm;
    int mod_qpsk;
    int mod_gfsk;
    int mod_cli_lock;
    int mod_p25p2_c4fm;
    int mod_p25p2_profile_lock;
    int inverted_p2;
    int inverted_x2tdma;
    int inverted_dmr;
    int inverted_dpmr;
    int inverted_ysf;
    int inverted_m17;
    int dmr_stereo;
    int dmr_mono;
    int use_cosine_filter;
    int ssize;
    int msize;
    int analog_only;
    int monitor_input_audio;
    char output_name[1024];
    int state_rf_mod;
    int state_samplesPerSymbol;
    int state_symbolCenter;
    int state_dmr_stereo;
    int state_sps_hunt_idx;
} dsd_scan_settings;

/** Parse a trimmed, case-insensitive class; empty means inherit. Returns -1 on invalid input. */
int dsd_scan_mode_parse(const char* text, dsd_scan_mode* mode);
const char* dsd_scan_mode_name(dsd_scan_mode mode);
dsd_decode_mode_profile dsd_scan_mode_profile(dsd_scan_mode mode);
/** Active class, including combined P25; INHERIT when no override is installed. */
dsd_scan_mode dsd_scan_mode_active(const dsd_state* state);
/** Capture/restore effective fields for a staged tune; no pointers or audio sink fields are changed. */
void dsd_scan_settings_capture(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out);
void dsd_scan_settings_restore(const dsd_scan_settings* saved, dsd_opts* opts, dsd_state* state);
/** Compare setting values, ignoring unused label bytes; optionally include live timing/modulation. */
int dsd_scan_settings_equal(const dsd_scan_settings* a, const dsd_scan_settings* b, int include_timing);
/** Prepare production row settings without committing the row or baseline. */
int dsd_scan_mode_prepare(dsd_opts* opts, dsd_state* state, dsd_scan_mode mode, dsd_scan_settings* out);
/** Reserve scope storage before staging a tune or building trunk-target snapshots. */
int dsd_scan_mode_begin(const dsd_opts* opts, dsd_state* state);
/** Configured preset for mode selectors; active combined P25 remains a separate scan class. */
dsdneoUserDecodeMode dsd_scan_mode_configured_preset(const dsd_opts* opts, const dsd_state* state);
/** Configured preset for persistence; UNSET preserves custom decoder combinations. */
dsdneoUserDecodeMode dsd_scan_mode_configured_preset_exact(const dsd_opts* opts, const dsd_state* state);
/** Select a row from the saved baseline, keeping the open audio sink layout fixed.
 * INHERIT restores the baseline for a blank row while retaining scan ownership. */
int dsd_scan_mode_enter(dsd_opts* opts, dsd_state* state, dsd_scan_mode mode);
/** Install nonsecret row overrides after mode entry; NULL restores baseline row options.
 * No allocation. The caller must already own a scan scope. Reapplied after operator updates. */
void dsd_scan_mode_options(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* values);
/** Restore the exact configured baseline and release the scope. */
void dsd_scan_mode_leave(dsd_opts* opts, dsd_state* state);
/** Temporarily restore configuration for an operator update, retaining the row constraint. */
int dsd_scan_mode_suspend(dsd_opts* opts, dsd_state* state);
/** Save updated configuration and reapply the constraint. Return nonzero when decoder
 * settings changed and acquisition must reset; audio-routing-only updates return zero. */
int dsd_scan_mode_resume(dsd_opts* opts, dsd_state* state);
/** Nonzero between suspend and resume; side effects must wait until effective settings are known. */
int dsd_scan_mode_updating(const dsd_state* state);
/** Retain target modulation precedence across configured-setting updates. */
void dsd_scan_mode_target_modulation(const dsd_state* state, dsd_scan_modulation modulation);
/** Apply target flags/locks only; AUTO starts P25 on C4FM and other trunk classes on GFSK. */
void dsd_scan_mode_apply_modulation(dsd_opts* opts, dsd_scan_mode mode, dsd_scan_modulation modulation);
/** Snapshot configured settings, even while a row override is active. */
void dsd_scan_mode_configured(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out);
/** Borrow saved settings, or NULL without a scope/during an update. Use only on
 * the decoder thread or a consumer-owned snapshot; invalidated by scope updates. */
const dsd_scan_settings* dsd_scan_mode_configured_view(const dsd_state* state);
/** Deep-copy scalar scope metadata for frontend snapshots. No live extension pointer is shared. */
void dsd_scan_mode_copy_snapshot(dsd_state* dst, const dsd_state* src);
/** Current class profile; combined P25 and inherited settings follow the active hunt index. */
dsd_decode_mode_profile dsd_scan_mode_effective_profile(const dsd_opts* opts, const dsd_state* state);
#ifdef __cplusplus
}
#endif
#endif
