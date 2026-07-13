// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral public types shared by core config storage and app-control.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_FRONTEND_TYPES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_FRONTEND_TYPES_H_

#include <dsd-neo/platform/platform.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DSD_ATTR_PACKED {
    DSD_FRONTEND_NONE = 0,
    DSD_FRONTEND_TERMINAL = 1,
    DSD_FRONTEND_NATIVE = 2
} dsd_frontend_kind;

typedef struct dsd_frontend_common_display_opts {
    int constellation;
    float const_gate_qpsk;
    float const_gate_other;
    uint8_t const_norm_mode;
    uint8_t eye_view;
    uint8_t fsk_hist_view;
    uint8_t spectrum_view;
    uint8_t show_dsp_panel;
    uint8_t show_p25_metrics;
    uint8_t show_p25_neighbors;
    uint8_t show_p25_iden_plan;
    uint8_t show_p25_cc_candidates;
    uint8_t show_channels;
    uint8_t show_p25_affiliations;
    uint8_t show_p25_group_affiliations;
    uint8_t show_p25_callsign_decode;
} dsd_frontend_common_display_opts;

typedef struct dsd_frontend_terminal_display_opts {
    uint8_t terminal_compact;
    uint8_t terminal_history;
    uint8_t eye_unicode;
    uint8_t eye_color;
} dsd_frontend_terminal_display_opts;

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_FRONTEND_TYPES_H_ */
