// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR Tier III channel frequencies: where each row of the channel map (dsd_state::trunk_chan_map, LPCN 1..4094) came
 * from, and the one writer every over-the-air frequency goes through.
 *
 * A row's origin decides what may replace it. A row from a channel map file (FILE) or typed by the user (USER) is never
 * overwritten over the air. A frequency a checked CSBK or MBC names (SITE) replaces a RAS-accepted one (UNVERIFIED), a
 * neighbour site's (NEIGHBOUR) and a heuristic estimate (ESTIMATE) always, and another SITE one when the frequency
 * differs. UNVERIFIED, NEIGHBOUR and ESTIMATE only fill empty rows. A nonzero row with no recorded origin (a map loaded
 * before any of this was recorded) counts as FILE.
 *
 * A row another writer (an import, another protocol) sets afterwards is no longer the air's: dsd_state_set_trunk_chan_freq()
 * clears its bit in dsd_state::dmr_lcn_ota, and the row then reads as FILE, which DMR neither replaces nor drops.
 *
 * The origins, a network generation and a revision live in the DMR module's state extension
 * (DSD_STATE_EXT_PROTO_DMR_RC); with dsd_state::dmr_lcn_trust and dsd_state::dmr_lcn_ota they are saved per trunk-scan
 * target and per Signal search session. Decoder thread only.
 *
 * Beside them sits the missing-channel ledger: the channels a grant named that have no frequency, so the receiver
 * cannot follow their calls. Only a standard outbound grant that passed its CRC, or that -F accepted as RAS on a
 * RAS-confirmed carrier (then marked unchecked), feeds it, never a synthesized PDU; a grant whose channel has a
 * frequency never reaches it. A map change clears the channels it gave a frequency, a network change empties it, and
 * it is saved with the provenance.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_CHANNELS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_CHANNELS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/dmr/dmr_t3_grant.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where a channel map row came from. The values are stored per row and saved per trunk-scan target. */
typedef enum {
    DMR_CHAN_ORIGIN_NONE = 0,
    DMR_CHAN_ORIGIN_FILE = 1,
    DMR_CHAN_ORIGIN_USER = 2,
    DMR_CHAN_ORIGIN_SITE = 3,
    DMR_CHAN_ORIGIN_UNVERIFIED = 4,
    DMR_CHAN_ORIGIN_NEIGHBOUR = 5,
    DMR_CHAN_ORIGIN_ESTIMATE = 6
} dmr_chan_origin_e;

/**
 * The only over-the-air writer of the channel map: record @p freq_hz for @p lpcn (1..4094) as @p origin (SITE,
 * UNVERIFIED, NEIGHBOUR or ESTIMATE; FILE, USER and NONE are refused) under the precedence above. A written row's trust
 * byte (dsd_state::dmr_lcn_trust) is 2 when learned on the control channel and 1 otherwise, NEIGHBOUR always 1 and
 * ESTIMATE 0. A new row or a SITE change posts a notice on the receiving burst's slot with that burst's CRC flags; with
 * the heuristic fill on, a changed anchor drops every estimate and fills again. Returns 1 when the row or its origin
 * changed.
 */
int dmr_t3_chan_learn(dsd_opts* opts, dsd_state* state, uint16_t lpcn, long freq_hz, dmr_chan_origin_e origin);

/** The origin of @p lpcn's row: NONE for an empty row, FILE for a nonzero row with none recorded or one another writer
 * set after the air. */
dmr_chan_origin_e dmr_t3_chan_origin(const dsd_state* state, uint16_t lpcn);

/** dmr_t3_chan_origin() of LPCN 0..@p count - 1 into @p out (at most 0x1000 entries); 0 and 4095 read NONE. */
void dmr_t3_chan_origins_copy(const dsd_state* state, uint8_t* out, size_t count);

/** After a channel map import or reload replaced the map: nonzero rows become FILE, the others NONE. */
void dmr_t3_chan_note_map_loaded(dsd_state* state);

/** After the channel map was emptied: every origin is NONE. */
void dmr_t3_chan_note_map_cleared(dsd_state* state);

/**
 * The receiver is on another network (an operator move, or a checked system code whose MODEL or NET differs from the
 * last checked one's; another site of the same network is none): the over-the-air rows (SITE, UNVERIFIED, NEIGHBOUR,
 * ESTIMATE) and their trust bytes are dropped, FILE and USER rows stay, the missing-channel ledger empties, and the
 * network generation bumps. A trip from the control channel to a voice channel and back is none.
 */
void dmr_t3_chan_network_changed(dsd_opts* opts, dsd_state* state);

/** The network generation: bumps on every network change. */
uint32_t dmr_t3_chan_generation(const dsd_state* state);

/** The revision: bumps on any row or origin change, another writer taking a row at its frequency included. */
uint32_t dmr_t3_chan_revision(const dsd_state* state);

/**
 * One channel of the ledger with the latest grant that named it: its destination slot (0 or 1), the grant's classes and
 * parties, when it was heard (dsd_decode_now_mono_ms()), and whether no checked grant named the channel yet.
 */
typedef struct {
    uint16_t lpcn;
    uint8_t slot;
    uint8_t group;
    uint8_t data;
    uint8_t unverified;
    uint32_t caller;
    uint32_t recipient;
    uint64_t last_heard_ms;
} dmr_t3_missing_detail;

/**
 * The missing-channel ledger. Bit LPCN & 7 of byte LPCN >> 3 of each set: missing (granted, still no frequency),
 * checked (a checked grant named it), reported (its notice was logged; only a network change clears it). The revision
 * bumps on every change. details holds the detail_count most recent channels, newest first; a channel past them is in
 * the sets only.
 */
typedef struct {
    uint8_t missing[512];
    uint8_t checked[512];
    uint8_t reported[512];
    uint32_t revision;
    uint8_t detail_count;
    dmr_t3_missing_detail details[16];
} dmr_t3_missing_ledger;

/**
 * A grant of class @p cls for @p lpcn (1..4094) on destination slot @p slot named no frequency: record it, and log its
 * notice the first time this network names the channel. @p unverified is nonzero for a grant -F accepted as RAS; the
 * caller judges the provenance and records nothing else. A channel with a frequency is ignored.
 */
void dmr_t3_missing_note_grant(dsd_opts* opts, dsd_state* state, uint16_t lpcn, const dmr_t3_grant_class* cls,
                               uint8_t slot, int unverified);

/**
 * After the channel map or the user's channels changed: the channels that now have a frequency, and the ones the user
 * hid, leave the ledger; none join it. It writes the ledger in @p state's DMR extension, so decoder thread only.
 */
void dmr_t3_missing_reconcile(const dsd_state* state);

/** How many channels the ledger holds, the ones past its details included. */
size_t dmr_t3_missing_count(const dsd_state* state);

/** The ledger's revision (0 when nothing was recorded). */
uint32_t dmr_t3_missing_revision(const dsd_state* state);

/** Copy the ledger to @p out (all zero when nothing was recorded). */
void dmr_t3_missing_copy(const dsd_state* state, dmr_t3_missing_ledger* out);

/** Log the exit summary of the ledger's channels, if any. */
void dmr_t3_missing_log_summary(const dsd_state* state);

/** Log the exit summary of @p ledger, a trunk-scan target's, naming that target @p target (NULL or empty: none). */
void dmr_t3_missing_log_summary_for(const dmr_t3_missing_ledger* ledger, const char* target);

/* ---- The user's channels (discussion #900 F3) ----
 *
 * An overlay over the channel map, kept in the same extension: per LPCN, the user's frequency (SET), or the channel
 * hidden (HIDE: no frequency in the live map, so no grant for it is followed; it stays listed, and a grant for it is
 * not recorded as missing). The live row then reads as USER, which nothing over the air overwrites, and the row it
 * replaced (its frequency, origin and trust) is kept beside it, so RESTORE puts that back. A channel map import, reload
 * or clear replaces the base and dmr_t3_overlay_reapply() lays the overlay over the new one; a network change drops
 * only what the air taught, from the base too, and the overlay stays. An edit takes effect from the next grant that
 * names the channel: a call already followed keeps its carrier. At most DMR_T3_OVERLAY_MAX_ROWS channels have an
 * entry. The overlay belongs to the one global channel map: a trunk scan refuses every edit and the start-up file, so
 * a trunk-scan target never has one, and the target switch empties it (dmr_t3_overlay_discard()). Decoder thread
 * only. */

/** What an overlay edit does to its channel. */
typedef enum { DMR_T3_OVERLAY_SET = 1, DMR_T3_OVERLAY_HIDE = 2, DMR_T3_OVERLAY_RESTORE = 3 } dmr_t3_overlay_op;

/** What dmr_t3_overlay_apply() did. */
typedef enum {
    DMR_T3_SET_OK = 0,         /**< Applied, or nothing to change */
    DMR_T3_SET_STALE = 1,      /**< The row, its origin or the network generation is not the one expected */
    DMR_T3_SET_TRUNK_SCAN = 2, /**< A trunk scan runs: its targets' maps are not edited */
    DMR_T3_SET_INVALID = 3,    /**< No such channel, operation or frequency */
    DMR_T3_SET_FULL = 4        /**< A new entry past DMR_T3_OVERLAY_MAX_ROWS: remove one first */
} dmr_t3_set_result;

/** The frequencies a SET takes, in Hz (100 kHz .. 2147.483647 MHz). */
enum { DMR_T3_OVERLAY_MIN_HZ = 100000, DMR_T3_OVERLAY_MAX_HZ = 2147483647 };

/** The most channels the overlay holds an entry for (SET or HIDE). */
enum { DMR_T3_OVERLAY_MAX_ROWS = 256 };

/** The overlay, per LPCN: op is 0 (no entry), SET or HIDE; hz the SET frequency; base_* the row it replaced. */
typedef struct {
    uint8_t op[0x1000];
    uint8_t base_origin[0x1000];
    uint8_t base_trust[0x1000];
    long hz[0x1000];
    long base_hz[0x1000];
    uint16_t count;
} dmr_t3_overlay;

/**
 * Edit channel @p lpcn (1..4094): SET it to @p hz, HIDE it, or RESTORE the row the entry replaced (no entry: nothing to
 * do). With @p check nonzero the edit is refused, changing nothing, unless the live row is @p expected_hz with origin
 * @p expected_origin (dmr_t3_chan_origin(), USER for a hidden row) and the network generation is @p generation; with
 * @p check 0 (a start-up file) none of them is compared. Refused under a trunk scan (dsd_opts::trunk_scan_enabled), and
 * a SET or HIDE that would give a channel past DMR_T3_OVERLAY_MAX_ROWS an entry is FULL. An applied change bumps the
 * revision, reconciles the missing-channel ledger and redoes the heuristic estimates.
 */
dmr_t3_set_result dmr_t3_overlay_apply(dsd_opts* opts, dsd_state* state, uint16_t lpcn, dmr_t3_overlay_op op, long hz,
                                       long expected_hz, uint8_t expected_origin, uint32_t generation, int check);

/**
 * Apply the user's channels file @p path (--dmr-chan-overrides: `lpcn,frequency_hz` rows, 0 hides the channel;
 * <dsd-neo/core/dmr_chan_overrides_csv.h>) unchecked, after the channel map loaded. A row the file gets wrong, or one
 * past the DMR_T3_OVERLAY_MAX_ROWS channels the overlay holds, is skipped with a warning. Returns the rows applied, or
 * -1 when the file cannot be read or a trunk scan runs.
 */
int dmr_t3_overlay_load_csv(dsd_opts* opts, dsd_state* state, const char* path);

/**
 * After a channel map import, reload or clear replaced the map: every entry takes the new row as its base and is laid
 * over it again. A row still holding its entry (its origin USER) keeps its base, so a second call changes nothing.
 */
void dmr_t3_overlay_reapply(dsd_state* state);

/**
 * The entry of @p lpcn: its op (0 for none, DMR_T3_OVERLAY_SET or DMR_T3_OVERLAY_HIDE), the user's frequency in
 * @p hz (0 for HIDE) and the frequency RESTORE returns to in @p base_hz; either pointer may be NULL.
 */
int dmr_t3_overlay_get(const dsd_state* state, uint16_t lpcn, long* hz, long* base_hz);

/** How many channels have an entry. */
size_t dmr_t3_overlay_count(const dsd_state* state);

/* ---- Tier III identity and control channel evidence (discussion #900 F3) ---- */

/**
 * A message named the site Tier III (a C_ALOHA, C_SYS_Parms, P_SYS_Parms, C_BCAST or standard grant): with @p checked
 * nonzero its CRC passed (dmr_t3_tier3_seen()), with 0 -F accepted it as RAS (dmr_t3_tier3_unchecked_seen(), which
 * only decides how channel numbers show). Both flags hold for the session and are saved per trunk-scan target. The
 * caller judges the provenance. Decoder thread only.
 */
void dmr_t3_note_tier3(dsd_state* state, int checked);

/**
 * dmr_t3_note_tier3() that never allocates: nothing is recorded until the extension exists. For the grant path. It
 * writes the flag in @p state's DMR extension, so decoder thread only.
 */
void dmr_t3_note_tier3_if_recorded(const dsd_state* state, int checked);

/** Whether a checked message named the site Tier III this session (or on this trunk-scan target). */
int dmr_t3_tier3_seen(const dsd_state* state);

/** Whether a message -F accepted as RAS named the site Tier III this session (or on this trunk-scan target). */
int dmr_t3_tier3_unchecked_seen(const dsd_state* state);

/**
 * A checked C_ALOHA or C_SYS_Parms was received on the carrier the receiver is on: while no call is followed
 * (dsd_opts::trunk_is_tuned 0) and the tuned frequency is known (dsd_opts_tuned_freq_hz()), that frequency is this
 * carrier's TSCC until the receiver leaves it (dsd_state::carrier_seq moves). Also notes Tier III. The caller judges
 * the provenance. Decoder thread only.
 */
void dmr_t3_cc_note_evidence(const dsd_opts* opts, dsd_state* state);

/** The TSCC frequency dmr_t3_cc_note_evidence() recorded on the current carrier, or 0 when none or the receiver left
 * it. */
long dmr_t3_cc_confirmed_hz(const dsd_state* state);

/** Whether the DMR module recorded anything for @p state (its extension exists). */
int dmr_t3_chan_recorded(const dsd_state* state);

/**
 * Forget every entry, leaving the live rows as they are: a trunk-scan target's map and origins were just restored
 * (dmr_t3_chan_restore()), and no user's channel crosses to a target.
 */
void dmr_t3_overlay_discard(dsd_state* state);

/**
 * One trunk-scan target's (or Signal search session's) channel provenance: the origins, the trust bytes
 * (dsd_state::dmr_lcn_trust), the rows the air holds (dsd_state::dmr_lcn_ota), the generation and revision, the
 * missing-channel ledger and the Tier III flags. syscode is the last checked system code (valid when syscode_valid),
 * the baseline a later checked one is compared against.
 */
typedef struct {
    uint8_t origin[0x1000];
    uint8_t trust[0x1000];
    uint8_t ota[0x1000 / 8];
    uint32_t generation;
    uint32_t revision;
    uint16_t syscode;
    uint8_t syscode_valid;
    uint8_t tier3;
    uint8_t tier3_unchecked;
    dmr_t3_missing_ledger missing;
} dmr_t3_chan_target_snapshot;

/** Save the parked target's provenance, ledger and Tier III flags (never the overlay, which no target has, nor the
 * control channel evidence, which the retune ends). */
void dmr_t3_chan_save(const dsd_state* state, dmr_t3_chan_target_snapshot* out);

/**
 * Restore a target's provenance, ledger and Tier III flags, the map rows being restored beside it, and drop the control
 * channel evidence. Each revision goes past both the current one and the saved one, so a reader comparing revisions
 * sees the switch. The hunt's candidate list is not part of it, and neither is the overlay, which stays the live
 * session's: a Signal search session's comes back over the session's map rows; trunk scan empties it with
 * dmr_t3_overlay_discard().
 */
void dmr_t3_chan_restore(dsd_state* state, const dmr_t3_chan_target_snapshot* in);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_CHANNELS_H_ */
