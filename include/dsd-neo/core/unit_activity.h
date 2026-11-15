// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The session's unit activity log: registrations, deregistrations, talkgroup joins, denies, queued requests,
 * radio commands and status reports heard on a trunking control channel, kept in a fixed ring of plain records.
 *
 * Protocol producers push one record per decoded message that passed its CRC/FEC gate (never content that `-F`
 * relaxed). The ring stores numbers only: labels, reason text, scope text and current names are resolved later by the
 * app-control view. The ring lives in the state extension slot DSD_STATE_EXT_CORE_UNIT_ACTIVITY and is created by
 * the first push, which also draws its instance (never 0, never repeated in the process).
 *
 * Revisions: every insert or merge stamps the record with the ring's next revision (rev), so a reader that keeps the
 * last revision it read (its cursor) and asks for rows after it never skips one: rows come back in revision order, not
 * seq order. seq names a row for its whole life (assigned once, at insert); presentation order is the reader's.
 *
 * Merges (dedup): a push whose record equals one of the DSD_UNIT_ACTIVITY_DEDUP_LOOKBACK most recently revised rows on
 * every key field (see dsd_unit_activity_push()) merges into that row when the decode clock has not gone back past the
 * row's last_ms and less than DSD_UNIT_ACTIVITY_DEDUP_MS passed since its first_ms (an anchored window, so a message
 * repeated forever still starts a new row every window). A merge bumps repeat (saturating), last_ms and rev.
 *
 * Eviction: a full ring evicts its least recently revised row (the lowest rev), so every row it holds has a higher
 * revision than every row it evicted (evicted_rev_max). A reader whose cursor is below that revision missed a row:
 * dsd_unit_activity_read() reports it once (meta lost), and the first page it reads moves its cursor past it.
 *
 * Budget: DSD_UNIT_ACTIVITY_CAPACITY rows of at most 192 bytes (static-asserted) plus 4 KiB of links: about 196 KiB
 * per ring. The decoder holds one; each snapshot hop (published, consumer) holds one more, allocated at its first copy
 * and reused afterwards. The ring is not part of a trunk-scan target's saved state: one log spans the session.
 *
 * Threading: the live ring is written and read on the decoder thread only (the P25 watchdog's tick runs producers
 * under the P25 SM tick guard). Frontends read snapshot copies (dsd_unit_activity_copy_snapshot()).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_UNIT_ACTIVITY_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_UNIT_ACTIVITY_H_

#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** dsd_unit_activity::protocol. The values are stable. */
typedef enum {
    DSD_UA_PROTO_P25P1 = 1,
    DSD_UA_PROTO_P25P2,
    DSD_UA_PROTO_DMR,
    DSD_UA_PROTO_NXDN,
    DSD_UA_PROTO_TETRA,
} dsd_ua_protocol;

/** dsd_unit_activity::kind: what the message is about. The values are stable. */
typedef enum {
    DSD_UA_REGISTRATION = 1,
    DSD_UA_LOCATION_REGISTRATION,
    DSD_UA_DEREGISTRATION,
    DSD_UA_GROUP_AFFILIATION,
    DSD_UA_DENY,
    DSD_UA_QUEUED,
    DSD_UA_WAITING,
    DSD_UA_RADIO_CHECK,
    DSD_UA_INHIBIT,
    DSD_UA_UNINHIBIT,
    DSD_UA_STUN,
    DSD_UA_REVIVE,
    DSD_UA_KILL,
    DSD_UA_DETACH,
    DSD_UA_RADIO_MONITOR,
    DSD_UA_STATUS,
    DSD_UA_MESSAGE,
    DSD_UA_CALL_ALERT,
    DSD_UA_ANSWER_REQUEST,
    DSD_UA_AUTHENTICATION,
} dsd_ua_kind;

/** dsd_unit_activity::role: the message's place in its exchange. A COMMAND or ACK says a command was sent or
 * acknowledged, never that it took effect. The values are stable. */
typedef enum {
    DSD_UA_ROLE_COMMAND = 1,
    DSD_UA_ROLE_RESPONSE,
    DSD_UA_ROLE_ACK,
    DSD_UA_ROLE_REPORT,
} dsd_ua_role;

/** dsd_unit_activity::result: the outcome the message itself states (NA when it states none). The values are stable. */
typedef enum {
    DSD_UA_RESULT_NA = 0,
    DSD_UA_RESULT_ACCEPTED,
    DSD_UA_RESULT_FAILED,
    DSD_UA_RESULT_DENIED,
    DSD_UA_RESULT_REFUSED,
    DSD_UA_RESULT_QUEUED,
    DSD_UA_RESULT_ACKNOWLEDGED,
    DSD_UA_RESULT_UNKNOWN,
} dsd_ua_result;

/** dsd_unit_activity::stage: the step of a command that has more than one (a TETRA D-DISABLE / D-ENABLE is sent as an
 * intent, then as its authenticated confirm stage, EN 300 392-7 A.8.35). Both stages are commands: neither says the
 * radio acted on it. The values are stable. */
typedef enum {
    DSD_UA_STAGE_NONE = 0,          /**< The message has no stages. */
    DSD_UA_STAGE_INTENT = 1,        /**< The command's first (intent) stage. */
    DSD_UA_STAGE_AUTHENTICATED = 2, /**< The command's authenticated (confirm) stage. */
} dsd_ua_stage;

/** dsd_ua_identity::subtype. Only RADIO identities are ever named as radios; TEI_PARTIAL (a TETRA equipment identity
 * piece) and UNKNOWN (an address of unknown kind, a TETRA SMI or event label) are never named. The values are
 * stable. */
typedef enum {
    DSD_UA_ID_RADIO = 1,
    DSD_UA_ID_GROUP,
    DSD_UA_ID_INFRASTRUCTURE,
    DSD_UA_ID_TEI_PARTIAL,
    DSD_UA_ID_UNKNOWN,
} dsd_ua_id_subtype;

/**
 * dsd_ua_scope::kind, and dsd_ua_identity::home_kind (a home system in the same encoding). The values, and a and b
 * of each kind, are those of the learned-alias scope (dsd_alias_scope, <dsd-neo/core/alias_learn.h>), so a row's
 * scope reads as an alias scope by its kind, a and b:
 * - P25: a WACN, b SYSID (a network status was received).
 * - P25_NAC: a the NAC of a conventional (or not yet identified) P25 carrier, b 0.
 * - DMR: Tier III a = (trunk << 8) | model, b the raw network; Connect Plus a = trunk << 8, b the network; Capacity
 *   Plus a = (trunk << 8) | colour code, b the site (trunk: dsd_rxid_dmr_trunk).
 * - NXDN: Type-C a = 0x100 | category, b the system code (category is also in dsd_ua_scope::category); Type-D
 *   a = 0x200, b the SRV_INFO system ID.
 * - TETRA: a MCC, b MNC.
 */
typedef enum {
    DSD_UA_SCOPE_NONE = 0,
    DSD_UA_SCOPE_P25 = 1,
    DSD_UA_SCOPE_DMR = 2,
    DSD_UA_SCOPE_NXDN = 3,
    DSD_UA_SCOPE_TETRA = 4,
    DSD_UA_SCOPE_P25_NAC = 5,
} dsd_ua_scope_kind;

/** Set in dsd_ua_scope::c when it holds a site: P25 (RFSS << 8) | site, DMR the raw site number, NXDN the Type-C site
 * code or the Type-D SRV_INFO site code, TETRA the location area. Clear: no site was received. */
#define DSD_UA_SCOPE_SITE_KNOWN 0x80000000U

/** One identity of a record. Read id and subtype only while valid; home_kind/home_a/home_b (a dsd_ua_scope_kind
 * encoding: the P25 home WACN/SYSID of a fully qualified SUID, the NXDN subscriber's home location, the TETRA
 * subscriber's MCC/MNC) only while home_valid. An invalid identity is all zero. */
typedef struct {
    uint32_t id;
    uint8_t subtype; /**< dsd_ua_id_subtype */
    uint8_t valid;
    uint8_t home_kind; /**< dsd_ua_scope_kind */
    uint8_t home_valid;
    uint32_t home_a, home_b;
} dsd_ua_identity;

/** The reception scope a record was heard in, from dsd_state::rx_id at push unless the producer gave one. Read kind,
 * a, b and c only while valid; an unknown scope is all zero (valid 0), never a zero-valued valid scope. category is
 * the NXDN Type-C location category (0 Global, 1 Regional, 2 Local), 0 otherwise. */
typedef struct {
    uint8_t kind; /**< dsd_ua_scope_kind */
    uint8_t category;
    uint8_t valid;
    uint32_t a, b, c; /**< c: a site with DSD_UA_SCOPE_SITE_KNOWN, else 0 */
} dsd_ua_scope;

/**
 * One activity row. A producer fills the event fields (protocol through name_frozen, seq/rev/first_ms/last_ms/wall_s
 * and repeat aside) and leaves the rest zero; dsd_unit_activity_push() stamps the others.
 *
 * - seq: assigned once, at insert (1, 2, ... per ring). rev: the ring's revision at the row's last insert or merge.
 * - first_ms/last_ms: the decode clock's monotonic ms (dsd_decode_now_mono_ms()) at the first and the latest push.
 * - wall_s: the decode clock's wall reading at the first push, floored to the second (the history rows' convention).
 * - repeat: how many pushes the row holds (1 at insert), saturating at UINT16_MAX.
 * - subject: the affected radio or group (a status or message report: the reporting unit); actor: the requester,
 *   gateway or other party when the message carries one (a gateway is INFRASTRUCTURE, never the subject); assigned:
 *   the identity assigned to the subject (P25 assigned WUID, TETRA assigned SSI); recipient: a report's target.
 * - group/group_valid, value/value_valid, value2/value2_valid: payload fields (the message's group, its status value,
 *   command argument, accept type or service type, and a second field such as deny additional info, an NXDN visitor
 *   group or a TETRA assigned flag). Read a value only while its valid flag is set.
 * - reason_code with reason_set (a dsd_ua_reason_set, <dsd-neo/core/unit_activity_reason.h>): text is resolved at
 *   view time.
 * - stage: a step within one command (a dsd_ua_stage: TETRA intent vs authenticated confirm), 0 when the message has
 *   none.
 * - slot: the TDMA slot heard on (0/1); freq_hz: the carrier heard on (0 unknown).
 * - scope_epoch: the trunk-scan target generation the row was heard under, or 0; the producer fills it.
 * - name_frozen: the latest pushed name: the subject's name as the producer resolved it at the push that inserted
 *   the row, replaced by each merge that pushes a non-empty one (UTF-8, NUL-terminated; push cuts it at a character
 *   boundary when it fills the field). It is "frozen" only against later alias changes: nothing rewrites it but a
 *   push. Rows of another scope than the current one show it.
 */
typedef struct dsd_unit_activity {
    uint64_t seq, rev;
    uint64_t first_ms, last_ms;
    int64_t wall_s;
    int64_t freq_hz;
    dsd_ua_identity subject, actor, assigned, recipient;
    dsd_ua_scope scope;
    uint32_t group;
    uint32_t value, value2;
    uint32_t scope_epoch;
    uint16_t reason_code;
    uint16_t repeat;
    uint8_t protocol; /**< dsd_ua_protocol */
    uint8_t kind;     /**< dsd_ua_kind */
    uint8_t role;     /**< dsd_ua_role */
    uint8_t result;   /**< dsd_ua_result */
    uint8_t stage, reason_set, slot, value_valid;
    uint8_t group_valid;
    uint8_t value2_valid;
    char name_frozen[32];
} dsd_unit_activity;

#ifdef __cplusplus
static_assert(sizeof(dsd_unit_activity) <= 192, "a unit activity row fits the ring budget (192 bytes)");
#else
_Static_assert(sizeof(dsd_unit_activity) <= 192, "a unit activity row fits the ring budget (192 bytes)");
#endif

enum {
    DSD_UNIT_ACTIVITY_CAPACITY = 1024,
    DSD_UNIT_ACTIVITY_DEDUP_LOOKBACK = 16,
    DSD_UNIT_ACTIVITY_DEDUP_MS = 2000,
};

/** What a dsd_unit_activity_read() found beside the rows it returned. */
typedef struct dsd_unit_activity_read_meta {
    /** The ring's instance; the caller's own @p instance when no ring is held. Pass it back on the next read. */
    uint64_t instance;
    /** The cursor to pass back as @p after_rev: the revision of the last row returned, else the cursor read from. */
    uint64_t last_rev;
    /** The highest revision the ring evicted (0: none). Every row it holds has a higher one. */
    uint64_t evicted_rev;
    uint8_t held;  /**< 1 when a ring is held; 0: nothing was read and the caller's cursor stays as it was. */
    uint8_t more;  /**< 1 when rows after last_rev remain (read again). */
    uint8_t lost;  /**< 1 when a row after the caller's cursor was evicted before it was read. */
    uint8_t reset; /**< 1 when @p instance was not the ring's: the read started from the ring's oldest revision. */
} dsd_unit_activity_read_meta;

/**
 * Add @p row to the log of @p state (decoder thread), creating the ring at the first push.
 *
 * The record is checked and normalised first: a valid identity needs a dsd_ua_id_subtype, a valid home and a valid
 * scope a dsd_ua_scope_kind other than NONE; the fields of an invalid identity, home, scope or value are zeroed,
 * name_frozen is terminated, and a row whose scope is not valid takes the reception scope from dsd_state::rx_id (see dsd_ua_scope).
 * seq, rev, first_ms, last_ms, wall_s and repeat are stamped here; what @p row holds in them is ignored.
 *
 * Merge key: every field but seq, rev, first_ms, last_ms, wall_s, repeat, slot and name_frozen (protocol, kind,
 * role, result, stage, reason set and code, the four identities with their home systems, group, value, value2 and
 * their valid flags, scope, scope_epoch and freq_hz). A merge keeps the row's slot and takes the pushed name when it
 * is not empty, so name_frozen holds the latest pushed name.
 *
 * Returns 1 when a new row was inserted, 0 when the push merged into a row, -1 on a NULL argument, a protocol, kind,
 * role or result outside its enum, a valid identity, home or scope whose subtype or kind is outside its enum, or an
 * allocation failure (nothing is logged).
 */
int dsd_unit_activity_push(dsd_state* state, const dsd_unit_activity* row);

/**
 * Copy into @p out (room for @p cap rows, @p cap >= 0) the rows of @p state's ring revised after @p after_rev, in
 * revision order, and describe the read in @p meta (required).
 *
 * @p instance is the ring instance the cursor belongs to (0 before the first read). Another instance than the ring's
 * reads from the ring's oldest revision and sets meta reset. meta lost is set when the ring evicted a row revised
 * after the cursor read from; every row it holds is newer than those, so the first page returned moves the cursor
 * past the loss and the next read does not report it again. No ring held: nothing is returned, meta held is 0, and
 * meta instance and last_rev give back the caller's cursor. A cursor past the ring's revision in its own instance
 * reads nothing and keeps the cursor.
 *
 * Returns the number of rows written, or -1 on a NULL @p state or @p meta, a negative @p cap, or a NULL @p out with a
 * positive @p cap.
 */
int dsd_unit_activity_read(const dsd_state* state, uint64_t instance, uint64_t after_rev, dsd_unit_activity* out,
                           int cap, dsd_unit_activity_read_meta* meta);

/**
 * Bring @p dst's copy of the log up to @p src's (a snapshot hop: decoder to published, published to consumer).
 *
 * @p dst keeps its own ring, allocated at its first copy and reused afterwards: never an alias of @p src's (an alias
 * left by a struct copy is detached, not freed). A copy at the same instance and revision returns at once; one at the
 * same instance copies only the rows revised since; another instance copies every row. @p src holding no ring frees
 * @p dst's. On an allocation failure @p dst keeps what it held (no ring, at the first copy) and -1 is returned: a
 * reader of @p dst then reads nothing new and keeps its cursor until a later copy succeeds.
 *
 * Returns 0 on success (nothing to copy included), -1 on a NULL argument or an allocation failure.
 */
int dsd_unit_activity_copy_snapshot(dsd_state* dst, const dsd_state* src);

/**
 * @brief 1 when identity @p id is at home in the reception @p state decodes now: it is valid and has no home of its own
 * (dsd_ua_identity::home_valid 0), or its home system is the current reception's scope (dsd_alias_scope_current(),
 * whose kinds and a / b are those of dsd_ua_scope_kind, static-asserted in unit_activity.c). 0 for a NULL argument, an
 * invalid identity, or a home elsewhere (a roaming radio: its number may be another local radio's).
 */
int dsd_unit_activity_identity_at_home(const dsd_state* state, const dsd_ua_identity* id);

/**
 * @brief Fill @p row's name_frozen with the name the receiver knows its subject by now, for producers to call just
 * before dsd_unit_activity_push() (P25, DMR, NXDN and TETRA use this one rule, so the frozen names agree with the
 * view's live ones).
 *
 * The identity named is the row's assigned identity when it is a valid RADIO (the local ID a visiting radio was given),
 * else the subject when it is a valid RADIO at home (dsd_unit_activity_identity_at_home()). Its name is the current
 * reception's (dsd_source_label_lookup_scoped() with the current scope: a learned alias of another system never names
 * it). Groups, infrastructure, TEI pieces and identities of unknown kind are never named. A producer that knows a
 * subject number belongs to another network without a home to say so (a TETRA USSI) does not call this. name_frozen is
 * left as it is when nothing is named; a name longer than the field is cut by the push at a character boundary.
 */
void dsd_unit_activity_freeze_name(const dsd_state* state, dsd_unit_activity* row);

#ifdef DSD_NEO_TEST_HOOKS
/** Fail ring allocations after @p successful_allocations more succeed; -1 disables the seam. */
void dsd_unit_activity_test_alloc_fail_after(long successful_allocations);
/** Rows dsd_unit_activity_copy_snapshot() copied since the process started (every hop). */
uint64_t dsd_unit_activity_test_rows_copied(void);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_UNIT_ACTIVITY_H_ */
