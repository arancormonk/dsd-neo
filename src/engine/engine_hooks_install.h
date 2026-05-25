// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Internal hook-installer entrypoints used by engine.c.
 *
 * These are intentionally private to the engine module.
 */

#ifndef DSD_NEO_SRC_ENGINE_ENGINE_HOOKS_INSTALL_H
#define DSD_NEO_SRC_ENGINE_ENGINE_HOOKS_INSTALL_H

void dsd_engine_frame_sync_hooks_install(void);
void dsd_engine_trunk_tuning_hooks_install(void);
void dsd_engine_rtl_stream_io_hooks_install(void);
void dsd_engine_rtl_stream_metrics_hooks_install(void);
void dsd_engine_rigctl_query_hooks_install(void);
void dsd_engine_net_audio_input_hooks_install(void);
void dsd_engine_udp_audio_hooks_install(void);
void dsd_engine_m17_udp_hooks_install(void);
void dsd_engine_p25_optional_hooks_install(void);

#endif
