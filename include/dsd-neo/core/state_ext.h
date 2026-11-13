// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Extension slots for per-module `dsd_state` data.
 *
 * Provides a small mechanism for features/modules to attach per-state
 * allocations without continually expanding the core `dsd_state` struct.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_STATE_EXT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_STATE_EXT_H_H

#include <dsd-neo/platform/platform.h>

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum number of extension slots.
 *
 * Keep this value stable: increasing it changes the size/layout of `dsd_state`.
 */
enum DSD_ATTR_PACKED { DSD_STATE_EXT_MAX = 32 };

/**
 * @brief Stable IDs for extension slots.
 *
 * Values must be in the range [0, DSD_STATE_EXT_MAX).
 *
 * ID allocation policy (keep stable):
 * - 0-7: engine
 * - 8-11: core
 * - 12-15: io
 * - 16-23: ui
 * - 24-31: protocols
 *
 * When adding a new ID:
 * - Assign an explicit numeric value.
 * - Never renumber existing IDs.
 * - Keep within your module's reserved range and < DSD_STATE_EXT_MAX.
 */
typedef enum DSD_ATTR_PACKED dsd_state_ext_id {
    DSD_STATE_EXT_ENGINE_START_MS = 0,
    DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES = 1,
    /*
     * Core facilities use dedicated slots rather than expanding `dsd_state`.
     * Engine owns 0, 1, 3 and 7; core owns 2, 4, 5 and 8, and 13 (taken from the io range); runtime owns 6; DSP
     * owns 9, 10 and 11 (taken from the core range, as runtime took 6 from the engine range) and 12 (taken from the io
     * range); app-control owns 16, from the ui range.
     */
    DSD_STATE_EXT_CORE_TG_POLICY = 2,
    DSD_STATE_EXT_ENGINE_TRUNK_SCAN = 3,
    DSD_STATE_EXT_CORE_CALL_STATE = 4,
    DSD_STATE_EXT_CORE_CHANNEL_MODES = 5,
    DSD_STATE_EXT_RUNTIME_SCAN_MODE = 6,
    DSD_STATE_EXT_ENGINE_CHANNEL_SCAN = 7,
    DSD_STATE_EXT_CORE_SOURCE_ALIAS = 8,
    /* Analog receive working state (sub-audible front end and tone detectors, issue #522).
     * Decoder-thread private: frontends read dsd_state::analog_rx, never this slot. */
    DSD_STATE_EXT_DSP_ANALOG_RX = 9,
    /* The analog monitor's audio chains: voice band-pass and AGC state (issue #518). Decoder-thread private. */
    DSD_STATE_EXT_DSP_ANALOG_AUDIO = 10,
    /* The analog sinks' converters to the sink rate (issue #633). Decoder-thread private. */
    DSD_STATE_EXT_DSP_ANALOG_SINK = 11,
    /* The symbol grid's sync anchor: recent samples and symbol centres (src/dsp/symbol_anchor.c, issue #729).
     * Decoder-thread private. */
    DSD_STATE_EXT_DSP_SYMBOL_ANCHOR = 12,
    /* The digital voice chain's speech level per slot: each slot's speech AGC and the reception it runs for, the M17
     * encoder monitor's fixed-unity limiter and the DMR superframe mixer's one-channel limiter
     * (src/core/audio/voice_level.c, issue #518). Decoder-thread private: frontends read the applied gain from
     * dsd_state::aout_gain/aout_gainR. */
    DSD_STATE_EXT_CORE_VOICE_LEVEL = 13,
    /* App-control's frequency entry that sent a scan to one of its rows and still waits on its tune (issue #644,
     * src/app_control/scan_entry_toast.c): what its landing's toast names. Decoder-thread private. */
    DSD_STATE_EXT_APP_SCAN_ENTRY = 16,
    DSD_STATE_EXT_PROTO_NXDN_TRUNK_DIAG = 24,
    DSD_STATE_EXT_PROTO_DMR_RC = 25,
    DSD_STATE_EXT_PROTO_P25_CC_SELECTION = 26,
    /* TETRA decoder state (src/protocol/tetra/tetra_state.h). Decoder-thread private. */
    DSD_STATE_EXT_PROTO_TETRA = 27,
    /* NXDN control-channel hunt (src/protocol/nxdn/nxdn_cc_hunt.c). Decoder-thread private. */
    DSD_STATE_EXT_PROTO_NXDN_CC_HUNT = 28,
    /* P25 Phase 2 scrambling code recovery: the candidate waiting for its confirmation
     * (src/protocol/p25/phase2/p25p2_seed_recover.c). Decoder-thread private. */
    DSD_STATE_EXT_PROTO_P25P2_SEED_RECOVER = 29,
} dsd_state_ext_id;

typedef void (*dsd_state_ext_cleanup_fn)(void*);

#define DSD_STATE_EXT_GET_AS(type, state, id) ((type*)dsd_state_ext_get((state), (id)))

void* dsd_state_ext_get(const dsd_state* state, dsd_state_ext_id id);

const void* dsd_state_ext_get_const(const dsd_state* state, dsd_state_ext_id id);

int dsd_state_ext_set(dsd_state* state, dsd_state_ext_id id, void* ptr, dsd_state_ext_cleanup_fn cleanup);

void dsd_state_ext_free_all(dsd_state* state);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_STATE_EXT_H_H */
