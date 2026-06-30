// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Forward declaration of core decoder options type (`dsd_opts`).
 *
 * Provides an incomplete type for headers that only need pointers/
 * references.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_OPTS_FWD_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_OPTS_FWD_H_H

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

typedef struct dsd_frontend_display_opts {
    int constellation;                   // common: constellation view (0=off, 1=on)
    float const_gate_qpsk;               // common: constellation magnitude gate for QPSK
    float const_gate_other;              // common: constellation gate for non-QPSK (FSK)
    uint8_t const_norm_mode;             // common: 0=radial percentile norm, 1=unit-circle norm
    uint8_t terminal_compact;            // terminal-only: compact terminal layout
    uint8_t terminal_history;            // terminal-only: event history display mode
    uint8_t eye_view;                    // common: timing/eye diagram for C4FM/FSK (0=off)
    uint8_t fsk_hist_view;               // common: 4-level histogram for C4FM/FSK (0=off)
    uint8_t spectrum_view;               // common: spectrum analyzer for complex baseband (0=off)
    uint8_t eye_unicode;                 // terminal-only: use Unicode block glyphs in eye diagram (0=ASCII)
    uint8_t eye_color;                   // terminal-only: use colorized density in eye diagram (0=mono)
    uint8_t show_dsp_panel;              // common: show compact DSP status panel (0=hidden)
    uint8_t show_p25_metrics;            // common
    uint8_t show_p25_neighbors;          // common
    uint8_t show_p25_iden_plan;          // common
    uint8_t show_p25_cc_candidates;      // common
    uint8_t show_channels;               // common
    uint8_t show_p25_affiliations;       // common
    uint8_t show_p25_group_affiliations; // common
    uint8_t show_p25_callsign_decode;    // common
} dsd_frontend_display_opts;

typedef struct dsd_opts dsd_opts;
/* Opaque TCP audio input context referenced by dsd_opts without requiring IO-layer includes. */
typedef struct tcp_input_ctx tcp_input_ctx;

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_OPTS_FWD_H_H */
