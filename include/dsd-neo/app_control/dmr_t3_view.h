// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The DMR Tier III channels as the frontends read them (discussion #900 F3).
 *
 * The UI snapshot (<dsd-neo/app_control/snapshot.h>) carries an owned copy of the DMR module's channel record: where
 * each channel map row came from, the frequency DMR uses for each channel (the user's entry over the shared map row),
 * the channels granted without a frequency (the missing-channel ledger), the user's channels over the map (set and
 * hidden), whether the site was named Tier III and whether the carrier names another system now, whether the
 * --dmr-chan-overrides file applied to this session, and the control channel frequency checked evidence names on the
 * current carrier. Both snapshot hops copy it. Each copies the ledger only when the state instance, the parked
 * trunk-scan target or the ledger's revision moved (missing_seq), and the channel rows (origins, frequencies, overlay
 * rows) only when the instance, the target, the channel revision or the channel map's sequence moved (content_seq): a
 * grant heard again for a channel with no frequency moves the ledger alone. Only the decoder thread writes the
 * source.
 *
 * dsd_app_dmr_t3_view() reads it from a snapshot; a live decoder state carries none.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_DMR_T3_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_DMR_T3_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sizes of the view (they match <dsd-neo/protocol/dmr/dmr_t3_channels.h>). */
enum {
    DSD_UI_DMR_T3_ROWS = 0x1000,          /**< origin[] entries, LPCN 0..4095 (1..4094 are channels) */
    DSD_UI_DMR_T3_SET_BYTES = 512,        /**< bytes of each ledger bit set */
    DSD_UI_DMR_T3_MISSING_DETAILS = 16,   /**< the most recent channels the ledger details */
    DSD_UI_DMR_T3_OVERLAY_MAX_ROWS = 256, /**< the most channels the user's overlay holds */
};

/** The values origin[] holds, dmr_chan_origin_e's, for a frontend that reads no protocol header. */
enum {
    DSD_UI_DMR_T3_ORIGIN_NONE = 0,       /**< no frequency */
    DSD_UI_DMR_T3_ORIGIN_FILE = 1,       /**< the channel map file */
    DSD_UI_DMR_T3_ORIGIN_USER = 2,       /**< the user's overlay (set, or hidden) */
    DSD_UI_DMR_T3_ORIGIN_SITE = 3,       /**< checked over the air on this site */
    DSD_UI_DMR_T3_ORIGIN_UNVERIFIED = 4, /**< over the air, accepted as RAS only */
    DSD_UI_DMR_T3_ORIGIN_NEIGHBOUR = 5,  /**< a neighbour site's announcement */
    DSD_UI_DMR_T3_ORIGIN_ESTIMATE = 6,   /**< the heuristic fill */
};

/** DSD_APP_CMD_DMR_CHANNEL_SET's operations, dmr_t3_overlay_op's values, and the frequencies a SET takes. */
enum {
    DSD_UI_DMR_T3_OP_SET = 1,
    DSD_UI_DMR_T3_OP_HIDE = 2,
    DSD_UI_DMR_T3_OP_RESTORE = 3,
    DSD_UI_DMR_T3_MIN_HZ = 100000,
    DSD_UI_DMR_T3_MAX_HZ = 2147483647,
};

/** One ledger channel with the latest grant that named it (dmr_t3_missing_detail). */
typedef struct {
    uint16_t lpcn;          /**< 1..4094 */
    uint8_t slot;           /**< destination slot, 0 or 1 */
    uint8_t group;          /**< 1 for a group grant, 0 for a private one */
    uint8_t data;           /**< 1 for a data grant */
    uint8_t unverified;     /**< 1 while no checked grant named the channel (RAS-accepted only) */
    uint32_t caller;        /**< the calling radio */
    uint32_t recipient;     /**< the talkgroup (group) or the called radio (private) */
    uint64_t last_heard_ms; /**< dsd_decode_now_mono_ms() when the grant was heard */
} dsd_ui_dmr_t3_missing_detail;

/**
 * The missing-channel ledger (dmr_t3_missing_ledger): bit LPCN & 7 of byte LPCN >> 3 of each set. details holds the
 * detail_count most recent channels, newest first; a channel past them is in the sets only.
 */
typedef struct {
    uint8_t missing[DSD_UI_DMR_T3_SET_BYTES];
    uint8_t checked[DSD_UI_DMR_T3_SET_BYTES];
    uint8_t reported[DSD_UI_DMR_T3_SET_BYTES];
    uint32_t revision;
    uint8_t detail_count;
    dsd_ui_dmr_t3_missing_detail details[DSD_UI_DMR_T3_MISSING_DETAILS];
} dsd_ui_dmr_t3_missing;

/** One channel of the user's overlay (dmr_t3_overlay_get()). */
typedef struct {
    uint16_t lpcn;  /**< 1..4094 */
    uint8_t hidden; /**< 1 for a hidden channel (no frequency, listed "hidden"), 0 for one the user set */
    long hz;        /**< the user's frequency; 0 when hidden */
    long base_hz;   /**< the shared channel map row under the entry, what RESTORE shows (0 for none) */
} dsd_ui_dmr_t3_overlay_row;

/** The DMR Tier III channel record a snapshot carries. */
typedef struct {
    uint8_t present;           /**< 1 when the DMR module recorded anything for the session; all else 0 when not */
    uint8_t tier3;             /**< a checked C_ALOHA, C_SYS_Parms, P_SYS_Parms, C_BCAST or grant named the site Tier
                                    III (this session, or on this trunk-scan target), and the receiver is not on
                                    another system now (other_system_here 0) */
    uint8_t cc_confirmed_here; /**< checked TSCC evidence (C_ALOHA or C_SYS_Parms, no call followed) on the current
                                    carrier: cc_hz is this carrier's TSCC */
    uint8_t tier3_unchecked;   /**< a message -F accepted as RAS named the site Tier III: display only (channel numbers
                                    show in decimal), never identity; 0 too while other_system_here */
    uint8_t other_system_here; /**< the carrier's received identity (dsd_state::rx_id) names a system that is not Tier
                                    III: another DMR trunking system (Capacity Plus, Connect Plus, XPT;
                                    dsd_rxid_dmr_trunk_named()) or another protocol's. Its map is that system's, so
                                    no Tier III numbering or editor applies; tier3 and tier3_unchecked read 0. 0 while
                                    the carrier named nothing yet */
    uint8_t overrides_applied; /**< the --dmr-chan-overrides file applied to this session (dmr_t3_overrides_applied()):
                                    0 with no file, and when the engine skipped it (a session that does not decode
                                    DMR, or a trunk scan), whatever dsd_opts::dmr_chan_overrides_file holds */
    uint8_t dmr_decoding;      /**< the options last published decode DMR (dsd_opts::frame_dmr): 0 after a mode change
                                    left DMR off, when no Tier III lookup reads the user's channels and the decoder
                                    refuses every channel edit (DSD_APP_DMR_CHANNEL_SET_NO_DMR), so a frontend offers
                                    no editor, as for other_system_here. Set at every publish */
    uint64_t state_id;         /**< dsd_state::state_instance of the source */
    uint32_t target_id;        /**< moves whenever the parked trunk-scan target changes; never repeats in a process */
    uint32_t revision;         /**< dmr_t3_chan_revision(): bumps on any row, origin or overlay change */
    uint32_t generation;       /**< dmr_t3_chan_generation(): the network generation the channel-set command expects */
    uint64_t content_seq;      /**< moves whenever the channel rows (origin, overlay, channel_hz) were copied anew;
                                    never repeats in a process */
    uint64_t missing_seq;      /**< moves whenever the ledger (missing) was copied anew; never repeats in a process */
    dsd_ui_dmr_t3_missing missing;      /**< the ledger */
    uint8_t origin[DSD_UI_DMR_T3_ROWS]; /**< dmr_t3_chan_origin() per LPCN (a dmr_chan_origin_e; USER with an entry) */
    uint16_t overlay_count;             /**< overlay rows in use, by LPCN ascending */
    dsd_ui_dmr_t3_overlay_row overlay[DSD_UI_DMR_T3_OVERLAY_MAX_ROWS];
    /** The frequency DMR uses per LPCN (dmr_t3_chan_effective_hz()): the user's entry over the shared map row (a set
     * channel its frequency, a hidden one 0), else the map row; 0 for LPCN 0 and 4095. What a frontend shows and
     * sends as DSD_APP_CMD_DMR_CHANNEL_SET's expected frequency: the snapshot's trunk_chan_map row is the shared one,
     * which another protocol's writer can empty or fill under an entry. Copied with the body. */
    long channel_hz[DSD_UI_DMR_T3_ROWS];
    long cc_hz; /**< the TSCC frequency when cc_confirmed_here, else 0 */
} dsd_ui_dmr_t3_view;

/** The record @p snapshot carries, or NULL when it carries none (a live decoder state, or NULL). */
const dsd_ui_dmr_t3_view* dsd_app_dmr_t3_view(const dsd_state* snapshot);

/** Whether the ledger in @p view (may be NULL) holds @p lpcn. */
int dsd_app_dmr_t3_view_missing_has(const dsd_ui_dmr_t3_view* view, uint16_t lpcn);

/** How many channels the ledger in @p view holds (0 for NULL), the ones past its details included. */
size_t dsd_app_dmr_t3_view_missing_count(const dsd_ui_dmr_t3_view* view);

/**
 * How many ledger channels the frontends list and count (the terminal's Channels panel, Qt's NO FREQ): the ledger's
 * count while the view records the session, the carrier names no other system and the session decodes DMR, else 0. A
 * carrier naming another system (other_system_here) has that system's map, and a session a live mode change left
 * without DMR (dmr_decoding 0) follows no Tier III grant, so no Tier III channel is missing a frequency there. 0 for
 * NULL.
 */
size_t dsd_app_dmr_t3_view_missing_shown(const dsd_ui_dmr_t3_view* view);

/**
 * Whether the channel map shows as Tier III LPCNs, in decimal: the site was named Tier III (checked or RAS-accepted),
 * or the ledger or the user's overlay holds a channel, the receiver is not on another system now (other_system_here),
 * and the session decodes DMR (dmr_decoding: with DMR off no Tier III lookup reads the user's channels, so the shared
 * map shows in its own form). 0 for NULL.
 */
int dsd_app_dmr_t3_view_shows_lpcns(const dsd_ui_dmr_t3_view* view);

/**
 * Whether the channel map in @p snapshot shows as Tier III LPCNs now: dsd_app_dmr_t3_view_shows_lpcns() for @p view, and
 * the system the receiver is on is DMR (the snapshot's synctype, else its lastsynctype; before any sync, DMR is the only
 * trunking protocol @p opts decodes). The Tier III flags hold for the session, so after a move to another system its
 * map keeps that system's numbering (P25's IDEN form, its channels past 4094 included). 0 for any NULL.
 */
int dsd_app_dmr_t3_view_lists_lpcns(const dsd_ui_dmr_t3_view* view, const dsd_opts* opts, const dsd_state* snapshot);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_DMR_T3_VIEW_H_ */
