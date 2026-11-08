// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The --trunk-scan target lists of this session, as every frontend shows and switches them (issue #697).
 *
 * The coordinator publishes its lists on dsd_state (trunk_scan_lists[], trunk_scan_list_count, the list of the target
 * on air, trunk_scan_lists_seq). This view reads those fields, and the scan state view's running predicate, from a
 * published snapshot pair or the decoder's own structs, for the terminal's Scan lists modal and status rows, the
 * Qt/Android Scan lists sheet and the control API: one entry per list, the counts, and whether a toggle can be
 * offered now. It reads only the fields it names (no lock, no walk of the targets), so a
 * frontend reads it every frame and acts only when @c lists_seq or @c session moves.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_LIST_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_LIST_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/trunk_scan_lists.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One target list. */
typedef struct {
    uint8_t number;     /**< 1-based, CLI/INI order: what DSD_APP_CMD_SCAN_LIST_SET names. */
    uint8_t enabled;    /**< 1 = switched on: its targets are in the rotation. */
    uint8_t has_active; /**< 1 = the target on air is one of this list's. */
    uint8_t pad;
    uint16_t targets;                         /**< Targets the list holds. */
    char name[DSD_TRUNK_SCAN_LIST_NAME_SIZE]; /**< File name without folder and extension. */
} dsd_app_scan_list_entry;

/** @brief The session's target lists. With no trunk scan, or before its coordinator starts, every count is 0. */
typedef struct {
    uint32_t session;        /**< dsd_state::trunk_scan_session: what a toggle names. 0 with no scan. */
    uint32_t lists_seq;      /**< dsd_state::trunk_scan_lists_seq: moves whenever a list changes. */
    uint8_t list_count;      /**< Lists loaded (at most DSD_TRUNK_SCAN_MAX_LISTS). */
    uint8_t lists_on;        /**< Lists switched on. */
    uint16_t targets_on;     /**< Targets of the lists that are on, avoided ones included (the "n of m" m). */
    uint8_t active_list;     /**< The list (1-based) of the target on air; 0 when none is on air (paused included). */
    uint8_t active_list_off; /**< 1 = that list is off: the receiver leaves as soon as another target can take it. */
    uint8_t running;         /**< 1 = the trunk scan runs (not paused). */
    uint8_t paused;          /**< 1 = the operator paused it. */
    uint8_t toggleable;      /**< 1 = lists can be switched now: running, not paused, 2+ lists. */
    dsd_app_scan_list_entry lists[DSD_TRUNK_SCAN_MAX_LISTS]; /**< lists[0 .. list_count - 1]. */
} dsd_app_scan_list_view;

/**
 * @brief Fill @p out from decoder state or a frontend snapshot pair. Zeroes @p out first.
 *
 * Reads the coordinator's published list fields only while --trunk-scan is configured; a list count past
 * DSD_TRUNK_SCAN_MAX_LISTS is clamped.
 *
 * @return 0, or -1 for NULL arguments.
 */
int dsd_app_scan_list_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_list_view* out);

/**
 * @brief The distinct --trunk-scan targets the lists of @p on_mask hold together (issue #697), avoided ones included:
 * the total every frontend shows for a set of lists on, a pending switch's included.
 *
 * Bit N-1 of @p on_mask is list N; bits past DSD_TRUNK_SCAN_MAX_LISTS mean nothing. A target several lists hold
 * counts once: the sum of dsd_state::trunk_scan_membership_counts over every membership mask that meets @p on_mask,
 * never a sum of the lists' own counts. Reads only those published counts (no lock, no walk of the targets), from
 * decoder state or a frontend snapshot.
 *
 * @return The total; 0 for a NULL @p state, an empty mask, or no trunk scan.
 */
unsigned int dsd_app_scan_lists_targets_on(const dsd_state* state, unsigned int on_mask);

/** @brief Which --trunk-scan targets a frequency typed by hand could pick now (issue #644), as the engine's entry match
 * decides it and the coordinator publishes it: one answer per target in the coordinator's target order (the lists in
 * order, each in its file's order), so a frontend names the row the scan will take without deciding it again. */
typedef struct {
    uint64_t revision; /**< dsd_state::trunk_scan_entry_rev: moves whenever an answer or the count changes. */
    size_t count;      /**< Targets answered; 0 with no trunk scan, or before the coordinator first publishes. */
    int32_t on_air;    /**< The target the scan would take as on air (confirmed there: parked, its park landed, not
                            paused), its index in the same order; -1 for none. */
} dsd_app_scan_entry_summary;

/** @brief Fill @p out from decoder state or a frontend snapshot pair (a few loads, no walk). Zeroes @p out first.
 * @return 0, or -1 for NULL arguments. */
int dsd_app_scan_entry_summary_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_entry_summary* out);

/** @brief Whether target @p index (0-based, the coordinator's order) could be picked: 1 when it is eligible (on a list
 * switched on, not avoided, its width fitting the published DSP rate), 0 when it is not, -1 for an index past the
 * published count or with no trunk scan. */
int dsd_app_scan_entry_eligible(const dsd_opts* opts, const dsd_state* state, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SCAN_LIST_VIEW_H_ */
