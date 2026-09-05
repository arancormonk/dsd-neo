// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/** @file @brief Scoped scanner decoder classes, independent of global CLI preset IDs. */
#ifndef DSD_NEO_RUNTIME_SCAN_MODE_H
#define DSD_NEO_RUNTIME_SCAN_MODE_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
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

/** Scalar snapshot of the exact configured decoder settings; owns no pointers. */
typedef struct {
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
/** Select a row from the saved baseline, keeping the open audio sink layout fixed. */
int dsd_scan_mode_enter(dsd_opts* opts, dsd_state* state, dsd_scan_mode mode);
/** Restore the exact configured baseline and release the scope. */
void dsd_scan_mode_leave(dsd_opts* opts, dsd_state* state);
/** Temporarily restore configuration for an operator update, retaining the row constraint. */
int dsd_scan_mode_suspend(dsd_opts* opts, dsd_state* state);
/** Save the updated configuration and reapply the suspended constraint. */
int dsd_scan_mode_resume(dsd_opts* opts, dsd_state* state);
/** Nonzero between suspend and resume; side effects must wait until effective settings are known. */
int dsd_scan_mode_updating(const dsd_state* state);
/** Trunk target modulation precedence: 0 inherit, 1 auto, 2 C4FM, 3 CQPSK, 4 GFSK. */
void dsd_scan_mode_target_modulation(const dsd_state* state, int modulation);
/** Snapshot configured settings, even while a row override is active. */
void dsd_scan_mode_configured(const dsd_opts* opts, const dsd_state* state, dsd_scan_settings* out);
/** Copy only configured option fields into an already initialized options copy. */
void dsd_scan_mode_configured_opts(const dsd_state* state, dsd_opts* opts);
/** Deep-copy scalar scope metadata for frontend snapshots. No live extension pointer is shared. */
void dsd_scan_mode_copy_snapshot(dsd_state* dst, const dsd_state* src);
/** Current class profile; combined P25 follows the active Phase 1/2 profile. */
dsd_decode_mode_profile dsd_scan_mode_effective_profile(const dsd_opts* opts, const dsd_state* state);
#ifdef __cplusplus
}
#endif
#endif
