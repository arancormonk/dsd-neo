// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The unit activity log as every frontend shows it: display-ready rows read from a snapshot's ring
 * (<dsd-neo/core/unit_activity.h>), with their labels, filters, reason, scope and radio names resolved in one place, so
 * the Qt Activity screen and the terminal panel say the same thing.
 *
 * Labels say what was heard, never more: a command row says the command was sent ("Inhibit sent", "Kill sent",
 * "Authenticated disable command"), an acknowledgement that it was acknowledged ("Inhibit acknowledged"); no label
 * says a radio is inhibited, stunned, disabled or killed, because a listener never sees that happen.
 *
 * Names: a row's subject is named from the snapshot's current lists only while the row's scope is the snapshot's
 * current reception scope (the learned-alias scope, <dsd-neo/core/alias_learn.h>), looked up in that scope; a row of
 * another system shows the name its producer stored when it was heard (dsd_unit_activity::name_frozen). So a later
 * alias renames rows of the current system, never another system's, and an A -> B -> A trunk-scan hop shows each
 * system's own name. dsd_app_unit_activity_name_key() changes whenever those names could change: a frontend that holds
 * rows reads them again from {instance, 0} then, and updates each in place by seq (that re-read reports lost again
 * when the ring has evicted rows: a gap already shown stays one gap).
 *
 * Threading: reads a snapshot (a frontend's deep copy), never the live decoder state.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_UNIT_ACTIVITY_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_UNIT_ACTIVITY_VIEW_H_

#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** dsd_app_unit_activity_row::filters: the Activity screen's filters a row belongs to (it can be in several). */
enum {
    /** Registration, location registration and deregistration, whatever their result. */
    DSD_UA_FILTER_LOGINS = 1U << 0,
    /** Talkgroup affiliation and subscription. */
    DSD_UA_FILTER_TALKGROUPS = 1U << 1,
    /** Deny, queued and waiting responses, and any row whose result is refused, denied or failed. */
    DSD_UA_FILTER_REFUSED = 1U << 2,
    /** Inhibit, uninhibit, stun, revive, kill, detach, radio check and monitor, with their acknowledgements and
     * responses. */
    DSD_UA_FILTER_COMMANDS = 1U << 3,
    /** Status, message, call alert, answer request and authentication. */
    DSD_UA_FILTER_OTHER = 1U << 4,
};

/** dsd_app_unit_activity_row::tone. */
enum {
    DSD_UA_TONE_NORMAL = 0,
    DSD_UA_TONE_WARNING = 1, /**< An inhibit or kill command (TETRA permanent disable included) or its ack. */
};

/** A buffer for dsd_app_unit_activity_headline(): a kind label, ": " and a reason text. */
#define DSD_APP_UNIT_ACTIVITY_HEADLINE_SIZE 112

/** One display-ready row. Every text is well-formed UTF-8, NUL-terminated, cut at a character boundary, without C0 or
 * C1 control characters; an empty text means the row has none. */
typedef struct {
    uint64_t seq;     /**< The row's identity for its whole life: a merge comes back with the same seq. */
    uint64_t rev;     /**< The ring revision of the row's last insert or merge. */
    int64_t wall_s;   /**< The decode wall clock at the first message, whole seconds. */
    uint16_t repeat;  /**< How many messages the row holds (1 at insert). */
    uint32_t filters; /**< DSD_UA_FILTER_* bits; every row has at least one. */
    uint8_t tone;     /**< DSD_UA_TONE_* */
    /** What was heard: "Registration accepted", "Joined talkgroup", "Denied", "Queued", "Inhibit sent", ... A frontend
     * shows it through dsd_app_unit_activity_headline(), which adds a deny or queued row's reason. */
    char kind_label[48];
    /** The affected radio or group as an ID ("1234567", "TG 100"); a status or message report's reporting unit. */
    char subject_text[64];
    /** The subject's name (see the file comment), else "". */
    char subject_name[64];
    /** The rest of the message, ", "-separated: its talkgroup (with its label), value, assigned identity ("as n"),
     * recipient ("to n") and requester ("from n"). Each part is whole: a long talkgroup label is shortened first
     * (ending "…"), and a part that still does not fit is left out with those after it (the text ends ", …"), so an
     * ID is never cut. */
    char target_text[128];
    /** The reason text and its code ("Target Unit Not Valid (0x20)"; "Reason 0x7A" when the table names none). */
    char reason_text[64];
    /** The system the row was heard on ("WACN BEE00 SYS 123 RFSS 1 SITE 2"), else "". */
    char scope_text[48];
    /** "P25 Phase 1", "P25 Phase 2", "DMR", "NXDN" or "TETRA". */
    char protocol_text[12];
} dsd_app_unit_activity_row;

/** A reader's place in the log. Start at {0, 0}; dsd_app_unit_activity_rows() moves it. */
typedef struct {
    uint64_t instance; /**< The ring instance the cursor belongs to (0 before the first read). */
    uint64_t rev;      /**< The revision of the last row read. */
} dsd_app_unit_activity_cursor;

/** What a dsd_app_unit_activity_rows() call found beside its rows. */
typedef struct {
    uint8_t held;  /**< 1 when the snapshot holds a log; 0: nothing was read and the cursor stayed. */
    uint8_t more;  /**< 1 when rows after the cursor remain: read again. */
    uint8_t lost;  /**< 1 when rows after the cursor were dropped before they were read (show a gap). */
    uint8_t reset; /**< 1 when the cursor belonged to another log (or none): drop held rows; these start a new one. */
} dsd_app_unit_activity_meta;

/**
 * @brief Read up to @p cap rows of @p snapshot's log revised after @p cursor, oldest revision first, and move the
 * cursor past them.
 *
 * Rows come in revision order: a merged row comes back with its seq and a higher rev, so a reader updates it in place.
 * Pages never skip a row: call again while @p meta more is set. @p meta lost is reported until a read returns a row:
 * that first row moves the cursor past the loss (a @p cap 0 read moves nothing and reports it again). With no log held, nothing is read and @p cursor is unchanged.
 *
 * @return the number of rows written, or -1 on a NULL @p snapshot, @p cursor or @p meta, a negative @p cap, or a NULL
 *         @p out with a positive @p cap.
 */
int dsd_app_unit_activity_rows(const dsd_state* snapshot, dsd_app_unit_activity_cursor* cursor,
                               dsd_app_unit_activity_row* out, int cap, dsd_app_unit_activity_meta* meta);

/**
 * @brief A key that changes whenever a row's resolved name could change (dsd_alias_name_key(): the group and radio
 * lists and the current scope). 0 only for NULL.
 */
uint64_t dsd_app_unit_activity_name_key(const dsd_state* snapshot);

/**
 * @brief The row's one-line headline, as every frontend shows it: @p row's kind label, and for a deny or queued row
 * with a reason, ": " and its reason text ("Denied: Target Unit Not Valid (0x20)", "Queued: ..."; "Denied" alone when
 * the row has no reason). A row of any other kind keeps its reason on its own line.
 *
 * Writes at most @p cap bytes (DSD_APP_UNIT_ACTIVITY_HEADLINE_SIZE holds any), cut at a character boundary; nothing
 * for a NULL @p out or a @p cap of 0, "" for a NULL @p row.
 */
void dsd_app_unit_activity_headline(const dsd_app_unit_activity_row* row, char* out, size_t cap);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_UNIT_ACTIVITY_VIEW_H_ */
