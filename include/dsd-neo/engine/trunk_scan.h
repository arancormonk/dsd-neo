// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine-owned single-tuner trunk scan coordinator.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_

#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>
#include <stdint.h>

struct p25_bandplan_row;

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_TRUNK_SCAN_DWELL_MIN_MS = 250,
    DSD_TRUNK_SCAN_DWELL_MAX_MS = 600000,
    DSD_TRUNK_SCAN_IDLE_DWELL_DEFAULT_MS = 3000,
    DSD_TRUNK_SCAN_ACTIVITY_HOLD_DEFAULT_MS = 1200,
    /* Ceiling on the memory the parked-target snapshots may occupy. See
     * dsd_trunk_scan_max_targets(). */
    DSD_TRUNK_SCAN_TARGET_MEMORY_BUDGET_BYTES = 256 * 1024 * 1024,
};

/**
 * @brief Largest number of trunk scan targets that fits in the target memory budget.
 *
 * There is no target-count limit as such: each parked target reserves a snapshot of decoder
 * state, and the cap is DSD_TRUNK_SCAN_TARGET_MEMORY_BUDGET_BYTES divided by that snapshot's
 * size, so it tracks the struct instead of being a constant that goes stale. Enforced while the
 * targets CSV is parsed - an unbounded row count would otherwise ask for a single allocation
 * large enough that Linux overcommit grants it and the process is killed while faulting the
 * pages in, rather than failing with a message the operator can act on.
 */
size_t dsd_trunk_scan_max_targets(void);

typedef enum {
    DSD_TRUNK_SCAN_TARGET_P25_TRUNK = 0,
    DSD_TRUNK_SCAN_TARGET_DMR_TRUNK = 1,
    DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL = 2,
    DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK = 3,
    DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL = 4,
    DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL = 5,
} dsd_trunk_scan_target_type;

typedef enum {
    DSD_TRUNK_SCAN_MODULATION_UNSET = DSD_SCAN_MODULATION_INHERIT,
    DSD_TRUNK_SCAN_MODULATION_AUTO = DSD_SCAN_MODULATION_AUTO,
    DSD_TRUNK_SCAN_MODULATION_C4FM = DSD_SCAN_MODULATION_C4FM,
    DSD_TRUNK_SCAN_MODULATION_CQPSK = DSD_SCAN_MODULATION_CQPSK,
    DSD_TRUNK_SCAN_MODULATION_GFSK = DSD_SCAN_MODULATION_GFSK,
} dsd_trunk_scan_modulation;

typedef struct {
    char id[64];
    dsd_trunk_scan_target_type type;
    uint32_t frequency_hz;
    char chan_csv[1024];
    /* Per-target key files, resolved relative to the targets CSV at parse time.
     * Loaded into the target's key set at init; empty means the global keys. */
    char keys_hex_csv[1024];
    char keys_dec_csv[1024];
    /* Parsed direct `-H`/`-b` equivalents. Kept as fixed metadata so raw key
     * text never survives parsing or reaches a diagnostic. */
    dsd_key_scalars single_key_scalars;
    uint8_t single_keys_present;
    /** Nonsecret, validated per-target overrides. Group path is relative-resolved at import. */
    dsd_scan_option_values row_options;
    /* Per-target P25 band plan CSV (trunk targets only), resolved relative to the targets CSV at
     * parse time and loaded into the target's own band-plan store at init; empty means none. */
    char p25_bandplan_csv[1024];
    int dwell_ms;
    int activity_hold_ms;
    dsd_trunk_scan_modulation modulation;
    int rtl_gain_is_set;
    int rtl_gain_db;
} dsd_trunk_scan_target;

typedef struct {
    dsd_trunk_scan_target* targets; /**< Owned heap array; NULL when empty. */
    size_t count;
    size_t capacity;
} dsd_trunk_scan_target_list;

/**
 * @brief Release a target list's owned storage and zero it.
 *
 * The list returned by a successful dsd_trunk_scan_load_targets_csv() owns its
 * `targets` array; callers must reset it once the coordinator has copied the
 * targets out. A failed load never writes `*out`, so nothing to reset there.
 *
 * @param list List to clear; NULL is ignored.
 */
void dsd_trunk_scan_target_list_reset(dsd_trunk_scan_target_list* list);

int dsd_trunk_scan_load_targets_csv(const char* path, const dsd_opts* opts, dsd_trunk_scan_target_list* out, char* err,
                                    size_t err_sz);

int dsd_engine_trunk_scan_init(dsd_opts* opts, dsd_state* state, char* err, size_t err_sz);
void dsd_engine_trunk_scan_shutdown(dsd_opts* opts, dsd_state* state);
void dsd_engine_trunk_scan_tick(dsd_opts* opts, dsd_state* state);
/**
 * @brief Apply an operator scan control (a dsd_trunk_scan_control_op from
 * runtime/trunk_scan_hooks.h) to the parked target list.
 *
 * Hold pauses the idle dwell on the active target without stopping its state machine;
 * avoid flags the active target for the session and moves on; clear puts every avoided
 * target back; advance moves now. Returns the codes documented beside the op enum.
 * Decoder thread only: avoid and advance retune.
 */
int dsd_engine_trunk_scan_control(dsd_opts* opts, dsd_state* state, int op);
void* dsd_engine_trunk_scan_active_p25_ctx(void);
void* dsd_engine_trunk_scan_active_dmr_ctx(void);
/**
 * @brief Channel-map path of the currently parked scan target, or NULL when it has none.
 *
 * Backs the `active_chan_csv` runtime hook: per-target `chan_csv` files are imported through
 * throwaway options, so this is the only place the path survives for protocol code to name.
 */
const char* dsd_engine_trunk_scan_active_chan_csv(const dsd_state* state);
/**
 * @brief Report decoded conventional activity so the active target keeps its park.
 *
 * Each entry point only acts when the target currently parked belongs to its own
 * conventional family; anything else (including trunk targets, or trunk scan not
 * being installed) is ignored. The NXDN entry point serves both NXDN conventional
 * types: NXDN48 and NXDN96 share a sync word and every decoded element, so the
 * protocol layer cannot tell them apart and must not have to. The call identity is run through the global
 * talkgroup policy, and only an allowed call refreshes the target's
 * activity hold, so allow/block lists, private-call, data-call and
 * encrypted-call tuning controls all apply to the park decision.
 *
 * Callers must pass identity that has already cleared the protocol's own
 * FEC/CRC gate: an unverified header would park the coordinator on noise.
 *
 * @param opts       Decoder options (policy and hold controls).
 * @param state      Decoder state owning the scan coordinator.
 * @param target     Destination talkgroup (group call) or destination unit (private call).
 * @param source     Source unit id, or 0 when unknown.
 * @param is_private Non-zero for a private/individual call, zero for a group call.
 * @param encrypted  Non-zero when the call is encrypted; pass the protocol's corroborated
 *                   classification, not a single frame's raw cipher field.
 * @param data_call  Non-zero for data traffic rather than voice.
 */
void dsd_engine_trunk_scan_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                     uint32_t source, int is_private, int encrypted, int data_call);
/** @copydoc dsd_engine_trunk_scan_dmr_conventional_activity */
void dsd_engine_trunk_scan_nxdn_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                      uint32_t source, int is_private, int encrypted, int data_call);
size_t dsd_engine_trunk_scan_target_count(const dsd_state* state);
int dsd_engine_trunk_scan_saved_tuner_autogain(const dsd_state* state, int* out_on);
int dsd_engine_trunk_scan_active_p25_cqpsk_request(const dsd_state* state, int* out_enable);
/**
 * @brief Symbol rate of the parked four-level GFSK scan target, or 0 when there is none.
 *
 * The tuning layer cannot derive this on its own. `state->rf_mod == 2` is true for both the
 * 4800 sym/s targets (DMR, NXDN96) and the 2400 sym/s ones (NXDN48), and `state->sps_hunt_idx`
 * is rotated by the no-sync SPS hunt, so a plain `-T` session can be sitting on the 2400 profile
 * during dead air. The coordinator's own target type is the only stable answer.
 *
 * The tuning layer treats a non-zero answer as authoritative for the RTL chain regardless of
 * `rf_mod`, so a target whose modulation column is empty under a global `-m` lock still gets its
 * own symbol rate and channel filter.
 *
 * @param state Decoder state owning the scan coordinator.
 * @return 2400 or 4800 for a parked GFSK-family target; 0 when trunk scan is not installed or the
 *         parked target is P25.
 */
int dsd_engine_trunk_scan_active_gfsk_symbol_rate(const dsd_state* state);
/**
 * @brief Append the parked targets' learned IDEN entries as band-plan rows.
 *
 * Walks every target except the active one (whose tables are live in @p state and are the
 * caller's to collect) and appends each parked snapshot's ready FDMA/TDMA entries through
 * dsd_p25_bandplan_append_tables(), which de-duplicates by identifier, table and WACN/SYS.
 * Serves the band-plan export; a session without a coordinator returns @p count unchanged.
 *
 * @param state Decoder state owning the scan coordinator.
 * @param rows  Row array of at least @p cap entries.
 * @param count Rows already in @p rows.
 * @param cap   Capacity of @p rows.
 * @return The new row count.
 */
int dsd_engine_trunk_scan_append_p25_idens(const dsd_state* state, struct p25_bandplan_row* rows, int count, int cap);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_ */
