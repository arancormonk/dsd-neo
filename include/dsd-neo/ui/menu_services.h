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

#ifdef __cplusplus
}
#endif
