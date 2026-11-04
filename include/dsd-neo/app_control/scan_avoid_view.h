// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The avoided -Y rows or --trunk-scan targets of this session, as every frontend lists them (issue #672).
 *
 * The scan avoids live in one of two stores: the -Y channel map's per-row flags (KIND_LCN), or the list the trunk-scan
 * coordinator publishes (KIND_TRUNK_SCAN, which wins while --trunk-scan is configured). This view answers, from a
 * published snapshot pair or the decoder's own structs, the questions the terminal's Avoids rows, the Qt/Android sheet
 * and the control API ask: which store, how many entries, whether the copy is whole enough to act on, and which
 * entries, each with the identity DSD_APP_CMD_SCAN_AVOID_REMOVE takes and a label.
 *
 * The summary costs a constant amount (no walk of either store), so a frontend reads it every frame and rebuilds its
 * list only when the summary's key moves (dsd_app_scan_avoid_summary_same_key()). The list build and the raw
 * collection walk the store once and allocate; they run on the caller's thread, the raw collection being the half the
 * API feed runs on the decoder thread (no formatting there).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_AVOID_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_AVOID_VIEW_H_

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Room for a row's name or a target's label column, as the decoder stores them. */
enum { DSD_APP_SCAN_AVOID_NAME_SIZE = 64 };

/** Room for any entry label: "#12  county-p25  851.012500 MHz  County Fire" with a 63-byte id and label. */
enum { DSD_APP_SCAN_AVOID_LABEL_SIZE = 192 };

/** @brief Which store the session's avoids live in, how many there are, and what the frontends may offer. */
typedef struct {
    uint32_t kind;       /**< dsd_app_scan_avoid_kind: TRUNK_SCAN while --trunk-scan is configured, else LCN while
                              -Y runs or is paused or a channel map has rows, else NONE. */
    uint8_t available;   /**< 1 = the published store is whole (dsd_state::scan_avoids_complete, and for a trunk scan
                              trunk_scan_avoids_ok): entries can be listed and put back. NONE reads 1. */
    uint8_t total_known; /**< 1 = @c total is exact; it is while @c available. */
    uint8_t can_avoid;   /**< 1 = a rotation runs and is not paused: Avoid current channel is offered. */
    uint8_t can_manage;  /**< 1 = the list and Clear are offered: a store exists and a rotation runs, entries exist,
                              or the copy is incomplete (so Clear stays reachable while the list is not ready). */
    size_t total;        /**< Avoided entries (exact, never saturated); 0 unless @c total_known. */
    uint64_t instance;   /**< dsd_state::state_instance. */
    uint64_t list_rev;   /**< LCN: dsd_state::lcn_list_rev; 0 otherwise. */
    uint32_t session;    /**< TRUNK_SCAN: dsd_state::trunk_scan_session; 0 otherwise. */
    uint64_t revision;   /**< LCN: lcn_avoid_rev; TRUNK_SCAN: trunk_scan_avoid_rev; 0 for NONE. Moves whenever what
                              the list shows could change. */
} dsd_app_scan_avoid_summary;

/** @brief One avoided entry before any formatting: its identity and the stored text a label is made from. */
typedef struct {
    dsd_app_scan_avoid_remove_payload identity; /**< What DSD_APP_CMD_SCAN_AVOID_REMOVE takes to put it back. */
    uint32_t ordinal; /**< TRUNK_SCAN: the target's 1-based position in the target list; LCN: 0. */
    uint32_t pad;
    char name[DSD_APP_SCAN_AVOID_NAME_SIZE]; /**< The row's name or the target's label column; "" for none. */
} dsd_app_scan_avoid_raw_entry;

/** @brief An owned raw collection (dsd_app_scan_avoid_raw_free()). */
typedef struct {
    dsd_app_scan_avoid_summary summary;
    dsd_app_scan_avoid_raw_entry* entries;
    size_t count;
} dsd_app_scan_avoid_raw;

/** @brief One avoided entry as a list shows it. */
typedef struct {
    dsd_app_scan_avoid_remove_payload identity;
    char label[DSD_APP_SCAN_AVOID_LABEL_SIZE];
} dsd_app_scan_avoid_entry;

/** @brief An owned list (dsd_app_scan_avoid_list_free()). */
typedef struct {
    dsd_app_scan_avoid_summary summary;
    dsd_app_scan_avoid_entry* entries;
    size_t count;
} dsd_app_scan_avoid_list;

/**
 * @brief Fill @p out from decoder state or a frontend snapshot pair, at a constant cost (no walk of either store).
 * Zeroes @p out first.
 *
 * @return 0, or -1 for NULL arguments.
 */
int dsd_app_scan_avoid_summary_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_avoid_summary* out);

/**
 * @brief 1 when @p a and @p b have the same key -- kind, instance, revision and availability -- so a list built for
 * one still holds for the other; 0 otherwise or for a NULL argument.
 */
int dsd_app_scan_avoid_summary_same_key(const dsd_app_scan_avoid_summary* a, const dsd_app_scan_avoid_summary* b);

/**
 * @brief Collect the avoided entries in list order (rows ascending, targets in target-list order), identities and
 * stored text only. Zeroes @p out first, then fills its summary.
 *
 * An unavailable store yields no entries (the caller says the list is not ready). On success the summary's @c total is
 * the number of entries collected, which the constant-cost summary can overstate when the -Y list shrank without a
 * recount.
 *
 * @return 0 (also with no entries), or -1 for NULL arguments or a failed allocation (no entries).
 */
int dsd_app_scan_avoid_raw_collect(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_avoid_raw* out);

/** @brief Free @p raw's entries and zero it. NULL is ignored. */
void dsd_app_scan_avoid_raw_free(dsd_app_scan_avoid_raw* raw);

/**
 * @brief The label of one entry, every distinguishing part before the free text a narrow list cuts:
 *
 * - a -Y row: "Row 3  851.012500 MHz  Fire Dispatch", or "Row 3  851.012500 MHz" without a name;
 * - a trunk-scan target: "#2  county-p25  851.012500 MHz  County Fire", or without its label column
 *   "#2  county-p25  851.012500 MHz".
 *
 * A row or target with no frequency yet says "frequency unknown" in place of its MHz.
 *
 * @return 0, or -1 for a NULL argument or an empty @p out.
 */
int dsd_app_scan_avoid_raw_label(const dsd_app_scan_avoid_raw_entry* entry, char* out, size_t out_size);

/**
 * @brief Build the list a frontend shows: dsd_app_scan_avoid_raw_collect() with each entry labelled. Zeroes @p out
 * first, then fills its summary.
 *
 * @return 0 (also with no entries, or none because the store is unavailable), or -1 for NULL arguments or a failed
 * allocation (no entries).
 */
int dsd_app_scan_avoid_list_build(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_avoid_list* out);

/** @brief Free @p list's entries and zero it. NULL is ignored. */
void dsd_app_scan_avoid_list_free(dsd_app_scan_avoid_list* list);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_AVOID_VIEW_H_ */
