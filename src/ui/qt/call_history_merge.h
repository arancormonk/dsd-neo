// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Merge and re-ingest policy for the Qt call history, free of Qt.
 *
 * The Qt Quick frontend only builds for Android, so the decisions that make or
 * break the history — when two committed ring rows are one conversation, which
 * fragment's source label, frequency and access code a merged row shows, and
 * when an already-ingested row has learned enough to be worth re-reading — live
 * here as plain functions the host test suite can exercise. The site a call was
 * heard on is compared and merged by the core's own rules
 * (<dsd-neo/core/received_identity_site.h>, libc only).
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_MERGE_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_MERGE_H_

#include <cstring>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/received_identity_site.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>

namespace dsd_qt {

/* Trunk-following mints a committed row per tune attempt, so one keyed-up
 * talkgroup can shed several near-simultaneous fragments. Within this window a
 * same-target row is the same conversation, not a new call. (Plain constexpr,
 * not C++17 inline variables: the host test suite builds as C++14.) */
constexpr int64_t kCallMergeWindowSrcMatchedSecs = 20;

/* When either side never learned its source id the window has to be tight:
 * src==0 is treated as a wildcard, and on a busy talkgroup two genuinely
 * distinct back-to-back calls routinely land within 20 s of each other. A
 * retune fragment chains off the previous fragment's end within a few seconds;
 * a different unit answering usually does not. */
constexpr int64_t kCallMergeWindowSrcUnknownSecs = 6;

/**
 * @brief Whether two same-target voice rows are close enough to be one call.
 *
 * The window extends off each fragment's end, not its start: one keyed-up
 * talkgroup sheds a fragment per retune, and a fixed window off the first
 * start would leak a duplicate row every window-length of activity.
 *
 * @param src_known_match Both rows carry the same nonzero source id.
 */
inline bool
call_history_merge_within_window(int64_t existing_start, int64_t existing_end, int64_t row_start, int64_t row_end,
                                 bool src_known_match) {
    const int64_t window = src_known_match ? kCallMergeWindowSrcMatchedSecs : kCallMergeWindowSrcUnknownSecs;
    return row_start <= existing_end + window && existing_start <= row_end + window;
}

/**
 * @brief Whether a ring row already ingested has since learned something.
 *
 * The core merges a reacquired segment into its committed row in place: the end
 * stamp extends, a late-decoded source id fills 0 -> real, and the crypto verdict
 * and emergency can flip on. The row's key does not change when that happens, so
 * this comparison — against what was last read, not against presence in a seen set —
 * is the only way those merges ever reach the display and the persisted log.
 */
inline bool
call_history_seen_row_advanced(int64_t stored_end, uint64_t stored_src, bool stored_enc, int64_t end, uint64_t src,
                               bool enc, bool stored_emergency = false, bool emergency = false) {
    return end > stored_end || (stored_src == 0U && src != 0U) || (!stored_enc && enc)
           || (!stored_emergency && emergency);
}

/**
 * @brief Fold a fresh read of a seen ring row into what was last recorded.
 *
 * One-way ratchets, matching the core's own merge semantics: the end never
 * retreats, a learned source id never un-learns, the crypto verdict never
 * clears; emergency also latches on. This is the single definition both the live
 * noteSeen() path and its tests share, so the ratchet cannot drift from the
 * advance test above.
 *
 * @return true when the row had advanced and the stored values were updated.
 */
inline bool
call_history_seen_absorb(int64_t* stored_end, uint64_t* stored_src, bool* stored_enc, int64_t end, uint64_t src,
                         bool enc, bool* stored_emergency = nullptr, bool emergency = false) {
    if (!call_history_seen_row_advanced(*stored_end, *stored_src, *stored_enc, end, src, enc,
                                        stored_emergency ? *stored_emergency : false, emergency)) {
        return false;
    }
    if (end > *stored_end) {
        *stored_end = end;
    }
    if (*stored_src == 0U && src != 0U) {
        *stored_src = src;
    }
    *stored_enc = *stored_enc || enc;
    if (stored_emergency) {
        *stored_emergency = *stored_emergency || emergency;
    }
    return true;
}

/**
 * @brief Which fragment a merged row's field came from: its start, push stamp and slot.
 *
 * Fragments reach the merge in whichever order a refresh happens to collect them -- a
 * backlog walks newest-first, a live session arrives oldest-first -- so a field has to be
 * chosen by the fragment it came from rather than by arrival. Each field keeps its own:
 * the newest fragment that knew a source label need not be the newest that knew the
 * frequency.
 */
struct CallHistoryProvenance {
    int64_t when; /**< The fragment's start. */
    uint64_t seq; /**< Its push stamp, which breaks a same-second tie. */
    int slot;     /**< Its TDMA slot, which breaks a tie between the slots' rings. */
};

/**
 * @brief Order two fragments: by start, then push stamp, then slot.
 * @return -1 when @p a is older than @p b, 1 when newer, 0 for the same fragment.
 */
inline int
call_history_provenance_compare(const CallHistoryProvenance& a, const CallHistoryProvenance& b) {
    if (a.when != b.when) {
        return a.when < b.when ? -1 : 1;
    }
    if (a.seq != b.seq) {
        return a.seq < b.seq ? -1 : 1;
    }
    if (a.slot != b.slot) {
        return a.slot < b.slot ? -1 : 1;
    }
    return 0;
}

/**
 * @brief Whether a fragment's value of one field replaces the merged row's.
 *
 * The per-field fold behind the source label, the frequency and the access code: the
 * newest fragment with a known value wins, unknown never erases known, and a known value
 * fills an unknown one whatever its age. Equal provenance adopts, since that is the same
 * fragment filling itself in (an in-place update). So fragments folded in any order give
 * the same row. The caller takes the incoming provenance with the value.
 *
 * @param stored_known   The row holds a known value.
 * @param stored         The fragment the row's value came from.
 * @param incoming_known The fragment carries a known value.
 * @param incoming       The fragment.
 */
inline bool
call_history_fold_adopts(bool stored_known, const CallHistoryProvenance& stored, bool incoming_known,
                         const CallHistoryProvenance& incoming) {
    if (!incoming_known) {
        return false;
    }
    return !stored_known || call_history_provenance_compare(incoming, stored) >= 0;
}

/** @brief Whether a row's frequency is known: 0 (and anything not positive) is unknown. */
inline bool
call_history_freq_known(int64_t freq_hz) {
    return freq_hz > 0;
}

/** @brief Whether a row's access code is known: any kind but DSD_ACCESS_CODE_NONE. */
inline bool
call_history_access_code_known(int kind) {
    return kind != static_cast<int>(DSD_ACCESS_CODE_NONE);
}

/**
 * @brief Fold a fresh read of a seen ring row's frequency and access code into what was last recorded.
 *
 * The core fills a committed row's frequency or code only while it is unknown (a
 * reacquisition merge learning one), so unknown -> known is the one change that makes the
 * row worth re-reading, and it is recorded. A read that changes a known value into another
 * is ignored and recorded nowhere, and unknown never erases.
 *
 * @return true when either field was learned.
 */
inline bool
call_history_seen_absorb_fill(int64_t* stored_freq_hz, int* stored_ac_kind, int* stored_ac, int64_t freq_hz,
                              int ac_kind, int ac) {
    bool learned = false;
    if (!call_history_freq_known(*stored_freq_hz) && call_history_freq_known(freq_hz)) {
        *stored_freq_hz = freq_hz;
        learned = true;
    }
    if (!call_history_access_code_known(*stored_ac_kind) && call_history_access_code_known(ac_kind)) {
        *stored_ac_kind = ac_kind;
        *stored_ac = ac;
        learned = true;
    }
    return learned;
}

/** @brief Whether a call channel is known: a DMR numbering (LSN, LPCN or LCN) and a channel number. */
inline bool
call_history_call_channel_known(uint8_t kind, uint32_t channel) {
    return kind >= static_cast<uint8_t>(DSD_CALL_CHANNEL_LSN) && kind <= static_cast<uint8_t>(DSD_CALL_CHANNEL_LCN)
           && channel != 0U;
}

/**
 * @brief Fold a fragment's site into the row's, unless the two conflict.
 *
 * The rows a merge joins already agree (dsd_call_site_compatible(), which also holds the channel kinds to each other
 * and to a DMR trunking system), and then the row keeps the union of their parts, which needs no provenance: no part
 * both know differs, so any order gives the same identity. An update of a fragment the row already holds can bring
 * parts that conflict with the row's (the core filled the fragment later, with a site the row's other fragments never
 * shared): then the fragment's site is discarded whole, and the caller discards its channel with it, since a part-wise
 * pick could pair one fragment's network with another's site. The fragment is not split back out of the row.
 *
 * A limit of folding by compatibility: with a split on conflicting parts, a fragment that knows fewer parts than its
 * neighbours (none, or the system without the site) joins whichever compatible row it meets first, so a live session
 * (oldest first) and a backlog (newest first) can group the same three fragments differently.
 *
 * @param site          The row's site, folded in place.
 * @param kind          The row's channel kind (dsd_call_channel_kind; NONE when it has no channel).
 * @param incoming      The fragment's site.
 * @param incoming_kind The fragment's channel kind.
 * @return true when the sites agree and @p site now holds their union; false when they conflict and @p site is as it
 *         was.
 */
inline bool
call_history_site_fold(dsd_received_identity* site, uint8_t kind, const dsd_received_identity* incoming,
                       uint8_t incoming_kind) {
    if (dsd_call_site_compatible(site, kind, incoming, incoming_kind) == 0) {
        return false;
    }
    dsd_received_identity merged;
    dsd_rxid_site_union(site, incoming, &merged);
    *site = merged;
    return true;
}

/**
 * @brief Fold a fresh read of a seen ring row's site parts and call channel into what was last recorded.
 *
 * The core adds site parts to a committed row in place (a reacquisition merge's union) and fills its channel, without
 * changing the row's key. A part gained is an advance; so is any change of the channel or its kind, which the core
 * never makes to a known one, so the change is read rather than second-guessed. Parts are never forgotten.
 *
 * @return true when the row advanced and the stored values were updated.
 */
inline bool
call_history_seen_absorb_site(uint32_t* stored_mask, uint32_t* stored_channel, uint8_t* stored_kind, uint32_t mask,
                              uint32_t channel, uint8_t kind) {
    const bool gained = (mask & ~*stored_mask) != 0U;
    const bool moved = channel != *stored_channel || kind != *stored_kind;
    if (!gained && !moved) {
        return false;
    }
    *stored_mask |= mask;
    *stored_channel = channel;
    *stored_kind = kind;
    return true;
}

/**
 * @brief A digest of a site's DMR site text: FNV-1a (32 bits) over its bytes up to the NUL, bounded by the field;
 *        0 when the text is not valid.
 *
 * The seen store keeps this rather than the text: the core can change a committed row's text in place and nothing else
 * (a reacquisition merge's union taking the text that names Capacity Max), and only the change matters.
 */
inline uint32_t
call_history_site_text_digest(const dsd_received_identity* site) {
    if ((site->valid & DSD_RXID_DMR_SITE_TEXT) == 0U) {
        return 0U;
    }
    uint32_t hash = 2166136261U;
    for (size_t i = 0U; i < sizeof(site->dmr_site_text) && site->dmr_site_text[i] != '\0'; i++) {
        hash ^= static_cast<uint8_t>(site->dmr_site_text[i]);
        hash *= 16777619U;
    }
    return hash;
}

/**
 * @brief Fold a fresh read of a seen ring row's site text digest into what was last recorded.
 *
 * Any change from the digest last read is an advance. A digest not known on either side (a seen entry an older build
 * stored, which kept none) is no advance: an unknown stored digest takes the one read, and an unknown read leaves the
 * stored one as it is.
 *
 * @return true when the row advanced and the stored digest was updated.
 */
inline bool
call_history_seen_absorb_site_text(uint32_t* stored_digest, bool* stored_known, uint32_t digest, bool known) {
    if (!known) {
        return false;
    }
    const bool advanced = *stored_known && digest != *stored_digest;
    *stored_digest = digest;
    *stored_known = true;
    return advanced;
}

/**
 * @brief A system identity as a history row keeps it (issue #800, D7): a dsd_position_scope and its identity
 *        discriminator (the tuned frequency of a P25 NAC scope, else 0). Kind DSD_POS_SCOPE_NONE is no identity.
 *
 * A row keeps two: where it was heard (Event_History::rx_scope) and its sender's (Event_History::pos.scope).
 */
struct CallHistoryScope {
    uint8_t kind = DSD_POS_SCOPE_NONE; /**< dsd_position_scope_kind */
    uint32_t a = 0U;
    uint32_t b = 0U;
    int64_t freqHz = 0; /**< the identity discriminator */
};

/** @brief Whether a row knows the identity: any kind but DSD_POS_SCOPE_NONE. */
inline bool
call_history_scope_known(const CallHistoryScope& scope) {
    return scope.kind != static_cast<uint8_t>(DSD_POS_SCOPE_NONE);
}

/**
 * @brief How strongly an identity names a system (issue #800, B5): 0 none; 1 a feed-only fallback (the control
 *        channel's or the tuned frequency); 2 a weak scope that is not unique (a conventional P25 NAC with its tuned
 *        frequency, a Capacity Plus site and colour code, an EDACS site or system); 3 a full system scope (P25 WACN and
 *        SYSID, a Tier III or Connect Plus network, NXDN, TETRA).
 */
inline int
call_history_scope_strength(const CallHistoryScope& scope) {
    switch (scope.kind) {
        case DSD_POS_SCOPE_NONE: return 0;
        case DSD_POS_SCOPE_FEED_CC_FREQ:
        case DSD_POS_SCOPE_FEED_TUNED_FREQ: return 1;
        case DSD_POS_SCOPE_P25_NAC:
        case DSD_POS_SCOPE_FEED_EDACS: return 2;
        case DSD_POS_SCOPE_DMR:
            // a = (dsd_rxid_dmr_trunk << 8) | Tier III model (<dsd-neo/core/alias_learn.h>).
            return (scope.a >> 8) == static_cast<uint32_t>(DSD_RXID_DMR_TRUNK_CAPACITY_PLUS) ? 2 : 3;
        default: return 3;
    }
}

/** @brief The system family an identity belongs to: P25 for both P25 kinds; 0 for a feed-only fallback, which any
 *         family can have. */
inline int
call_history_scope_family(const CallHistoryScope& scope) {
    if (call_history_scope_strength(scope) <= 1) {
        return 0;
    }
    return scope.kind == static_cast<uint8_t>(DSD_POS_SCOPE_P25_NAC) ? static_cast<int>(DSD_POS_SCOPE_P25)
                                                                     : static_cast<int>(scope.kind);
}

/**
 * @brief Whether identity @p y names everything identity @p x names: the same kind, and each part @p x knows with the
 *        value @p x gives it (issue #800, B5).
 *
 * Two kinds can know part of themselves: an EDACS scope its site and its system, each 0 until known
 * (DSD_POS_SCOPE_FEED_EDACS), and a P25 NAC scope its discriminator, the tuned frequency, 0 when none was known. Every
 * other kind is known whole or not at all, so for it this is equality.
 */
inline bool
call_history_scope_covers(const CallHistoryScope& x, const CallHistoryScope& y) {
    if (x.kind != y.kind) {
        return false;
    }
    if (x.kind == static_cast<uint8_t>(DSD_POS_SCOPE_FEED_EDACS)) {
        return (x.a == 0U || x.a == y.a) && (x.b == 0U || x.b == y.b) && x.freqHz == y.freqHz;
    }
    if (x.kind == static_cast<uint8_t>(DSD_POS_SCOPE_P25_NAC)) {
        return x.a == y.a && x.b == y.b && (x.freqHz == 0 || x.freqHz == y.freqHz);
    }
    return x.a == y.a && x.b == y.b && x.freqHz == y.freqHz;
}

/**
 * @brief Whether two identities may be one system (issue #800, B5).
 *
 * Only two identities known at the same strength (call_history_scope_strength()) neither of which names everything the
 * other does (call_history_scope_covers()) are two systems: two conventional DMR systems on two frequencies, two
 * conventional P25 channels of one NAC, two EDACS systems on one site. One that knows part of what the other knows
 * fully is the same system, so an EDACS call whose first fragment knew only the site joins the fragment that also
 * learned the system. A weaker identity is compatible with a stronger one of the same family, so a trunked call whose
 * first fragment knew only the NAC still joins the fragment that learned the WACN and SYSID. A row with no recorded
 * identity (one an older build stored, a protocol that has none) is compatible with any, which keeps the merge it
 * always had.
 */
inline bool
call_history_scopes_compatible(const CallHistoryScope& x, const CallHistoryScope& y) {
    const int sx = call_history_scope_strength(x);
    const int sy = call_history_scope_strength(y);
    if (sx == 0 || sy == 0) {
        return true;
    }
    if (sx == sy) {
        return call_history_scope_covers(x, y) || call_history_scope_covers(y, x);
    }
    const int fx = call_history_scope_family(x);
    const int fy = call_history_scope_family(y);
    return fx == 0 || fy == 0 || fx == fy;
}

/**
 * @brief Whether a row that knows identity @p held learns more of its system from a compatible fragment's @p seen
 *        (issue #800, B5): @p seen is stronger, or as strong and names everything @p held does and more (an EDACS
 *        system learned beside the site, a P25 NAC's tuned frequency). The row then keeps @p seen.
 */
inline bool
call_history_scope_learns(const CallHistoryScope& held, const CallHistoryScope& seen) {
    const int sh = call_history_scope_strength(held);
    const int ss = call_history_scope_strength(seen);
    if (ss != sh) {
        return ss > sh;
    }
    return ss != 0 && call_history_scope_covers(held, seen) && !call_history_scope_covers(seen, held);
}

/**
 * @brief The protocol family of a row's sync type (Event_History::systype), or -1 when it is not known.
 *
 * The core's family (dsd_sync_protocol_family(), as the position feed groups tracks), extended for History: the YSF,
 * dPMR and X2-TDMA sync values, which the core leaves one family each, are grouped per system too. A track never mixes
 * them, but one History call can be decoded in either polarity, and the merge only asks whether two fragments can be
 * one call. Rows older builds stored keep no sync type (-1).
 */
inline int
call_history_protocol_family(int protocol) {
    if (protocol < 0 || protocol >= static_cast<int>(DSD_POSITION_PROTOCOL_NONE)) {
        return -1;
    }
    if (DSD_SYNC_IS_YSF(protocol)) {
        return 0x200 + DSD_SYNC_YSF_POS;
    }
    if (DSD_SYNC_IS_DPMR(protocol)) {
        return 0x200 + DSD_SYNC_DPMR_FS1_POS;
    }
    if (DSD_SYNC_IS_X2TDMA(protocol)) {
        return 0x200 + DSD_SYNC_X2TDMA_DATA_POS;
    }
    return dsd_sync_protocol_family(protocol);
}

/**
 * @brief Whether a protocol's calls are named by text (issue #800): M17, D-STAR and YSF callsigns and dPMR dial
 * strings. Every other protocol names its calls by number, and the text the core writes beside the number
 * (Event_History::src_str/tgt_str, e.g. EDACS `LID_<id>`, `LID_UNK` and `<afs>_AFS_..`) is a display rendering of
 * that number or of its absence: no raw identity of its own. An unknown protocol (a row an older build stored) keeps
 * whatever text it has.
 */
inline bool
call_history_protocol_names_by_text(int protocol) {
    if (protocol < 0 || protocol >= static_cast<int>(DSD_POSITION_PROTOCOL_NONE)) {
        return protocol < 0;
    }
    return DSD_SYNC_IS_M17(protocol) || DSD_SYNC_IS_DSTAR(protocol) || DSD_SYNC_IS_YSF(protocol)
           || DSD_SYNC_IS_DPMR(protocol);
}

/**
 * @brief Whether @p text (simplified) is a raw source or target identity of a @p protocol call, rather than a display
 * placeholder: only a protocol that names calls by text has one (call_history_protocol_names_by_text()), and M17's
 * `UNKNOWN` source (the broadcast address where a callsign belongs) names nobody.
 */
inline bool
call_history_identity_text_is_raw(int protocol, bool source, const char* text) {
    if (!call_history_protocol_names_by_text(protocol) || text == nullptr || text[0] == '\0') {
        return false;
    }
    return !(source && protocol >= 0 && DSD_SYNC_IS_M17(protocol) && std::strcmp(text, "UNKNOWN") == 0);
}

/** @brief Whether two sync types may be one call: the same family, or either not known. */
inline bool
call_history_protocols_compatible(int x, int y) {
    const int fx = call_history_protocol_family(x);
    const int fy = call_history_protocol_family(y);
    return fx < 0 || fy < 0 || fx == fy;
}

/**
 * @brief The class a position's protocol belongs to as the core's position writers apply it
 * (dsd_position_attribution_class()): P25 Phase 1 and Phase 2 apart, every other protocol its core family; -1 when the
 * sync type is not known.
 *
 * Narrower than call_history_protocol_family(), which lets the two P25 phases (and both polarities of YSF, dPMR and
 * X2-TDMA) be one conversation: a row may merge them, but a fix is one class's only (issue #800).
 */
inline int
call_history_position_class(int protocol) {
    if (protocol < 0 || protocol >= static_cast<int>(DSD_POSITION_PROTOCOL_NONE)) {
        return -1;
    }
    if (DSD_SYNC_IS_P25P1(protocol)) {
        return 0x1001;
    }
    if (DSD_SYNC_IS_P25P2(protocol)) {
        return 0x1002;
    }
    return dsd_sync_protocol_family(protocol);
}

/**
 * @brief Where a History fragment was heard, as the core's reception rule reads a row (dsd_position_reception,
 * issue #800): the scope (Event_History::rx_scope once resolved, with its discriminator), and the received identity
 * epoch it was read under (rx_epoch, epoch_known) with the ring that epoch counts in (Event_History_I::instance; 0 when
 * not known). An epoch means nothing outside its ring: a fragment of another decoder run shares none with this one.
 */
struct CallHistoryReception {
    CallHistoryScope scope;
    uint64_t ring = 0U;
    uint32_t epoch = 0U;
    bool epochKnown = false;
};

/** @brief The tuned frequency a reception names: a tuned-frequency scope's, a P25 NAC scope's discriminator; else 0. */
inline int64_t
call_history_reception_tuned_hz(const CallHistoryScope& scope) {
    if (scope.kind == static_cast<uint8_t>(DSD_POS_SCOPE_FEED_TUNED_FREQ)) {
        return static_cast<int64_t>(scope.a);
    }
    return scope.kind == static_cast<uint8_t>(DSD_POS_SCOPE_P25_NAC) ? scope.freqHz : 0;
}

/** @brief Whether two known scopes are equal, discriminator included. */
inline bool
call_history_scopes_equal(const CallHistoryScope& x, const CallHistoryScope& y) {
    return x.kind == y.kind && x.a == y.a && x.b == y.b && x.freqHz == y.freqHz;
}

/** @brief Whether two receptions were read under one received identity epoch of one ring: both epochs known, in the
 * same known ring, and equal. */
inline bool
call_history_same_epoch(const CallHistoryReception& x, const CallHistoryReception& y) {
    return x.epochKnown && y.epochKnown && x.ring != 0U && x.ring == y.ring && x.epoch == y.epoch;
}

/** @brief Whether two scopes of one kind agree on every part both know: an EDACS scope knows its site and its system
 * apart (0 until known); every other kind is known whole. */
inline bool
call_history_scope_parts_agree(const CallHistoryScope& x, const CallHistoryScope& y) {
    if (x.kind == static_cast<uint8_t>(DSD_POS_SCOPE_FEED_EDACS)) {
        return (x.a == 0U || y.a == 0U || x.a == y.a) && (x.b == 0U || y.b == 0U || x.b == y.b);
    }
    return x.a == y.a && x.b == y.b;
}

/** @brief Within one epoch, whether one of two known scopes only refines the other: no two families, no two tuned
 * frequencies, and one kind only with agreeing parts. */
inline bool
call_history_reception_refines(const CallHistoryScope& x, const CallHistoryScope& y) {
    const int fx = call_history_scope_family(x);
    const int fy = call_history_scope_family(y);
    if (fx != 0 && fy != 0 && fx != fy) {
        return false;
    }
    const int64_t hx = call_history_reception_tuned_hz(x);
    const int64_t hy = call_history_reception_tuned_hz(y);
    if (hx != 0 && hy != 0 && hx != hy) {
        return false;
    }
    return x.kind != y.kind || call_history_scope_parts_agree(x, y);
}

/**
 * @brief Whether two receptions are one (issue #800, fail closed), the core's reception rule
 * (dsd_position_same_reception()) on History's fragments: both scopes known and equal, with one discriminator; or the
 * same received identity epoch of one ring and nothing that says otherwise (an unknown scope, or one scope only
 * refining the other: a NAC then the WACN and SYSID, an EDACS site then its system). Two families, two tuned frequencies
 * or two values of one kind are two receptions within one epoch, and an unknown side never matches across epochs or
 * rings.
 */
inline bool
call_history_same_reception(const CallHistoryReception& x, const CallHistoryReception& y) {
    const bool knownX = call_history_scope_known(x.scope);
    const bool knownY = call_history_scope_known(y.scope);
    if (knownX && knownY && call_history_scopes_equal(x.scope, y.scope)) {
        return true;
    }
    if (!call_history_same_epoch(x, y)) {
        return false;
    }
    return !knownX || !knownY || call_history_reception_refines(x.scope, y.scope);
}

/**
 * @brief Where a fragment's GPS text and position stand in the order that picks a merged row's one (issue #800, 2.7).
 *
 * A total order across decoder runs: the run's session ordinal (a persisted, strictly increasing number the model
 * gives each ring the first time it reads it; 0 when not known), then the position's rev within that run. Keyed only
 * while the fragment carries an admitted position and both are known.
 *
 * Apart from the key, @c version orders two readings of one fragment, with or without a position: the ring's
 * Event_History_I::commit_rev when the fragment was read. Every change of a committed row moves it and nothing resets
 * it, so of two readings of a fragment from one ring the greater version is the later content.
 */
struct CallHistoryPositionKey {
    bool keyed;       /**< an admitted position with its session ordinal and rev */
    int64_t ord;      /**< the session ordinal of the fragment's run */
    uint64_t rev;     /**< the position's rev in that run */
    uint64_t version; /**< the ring's commit_rev when the fragment was read; 0 when not known (older stores) */
};

/** @brief Order two keyed positions: -1 when @p a is older than @p b, 1 when newer, 0 for the same. */
inline int
call_history_position_key_compare(const CallHistoryPositionKey& a, const CallHistoryPositionKey& b) {
    if (a.ord != b.ord) {
        return a.ord < b.ord ? -1 : 1;
    }
    if (a.rev != b.rev) {
        return a.rev < b.rev ? -1 : 1;
    }
    return 0;
}

/**
 * @brief Whether two readings are of one ring fragment (issue #800, 2.7): the same run (session ordinal),
 * slot and push stamp, the fragment's stable ring identity (call_history_fragment_key()). Its start is not part of it:
 * a committed fragment with no start reads its end as its start, and the core can fill the start in or move the end
 * later (watchdog_event_merge_stamps()), so two readings of one fragment can carry different starts. Only when either
 * run is not known (ordinal 0: an older store) does the start guard the push stamp, which a restarted run counts from
 * zero again, as the seen key's does (seen_key()); fail closed, two such readings are one fragment only when every part
 * of their provenance agrees.
 *
 * @param a     One reading's provenance.
 * @param a_ord Its run's session ordinal, 0 when not known.
 * @param b     The other's.
 * @param b_ord Its run's session ordinal, 0 when not known.
 */
inline bool
call_history_same_fragment(const CallHistoryProvenance& a, int64_t a_ord, const CallHistoryProvenance& b,
                           int64_t b_ord) {
    if (a_ord != b_ord || a.seq != b.seq || a.slot != b.slot) {
        return false;
    }
    return a_ord > 0 || a.when == b.when;
}

/**
 * @brief Whether a fragment's GPS text and position replace the merged row's (issue #800, 2.7). They move together: the
 * text and the coordinates always come from one fragment.
 *
 * Two readings of the same fragment (call_history_same_fragment()) are ordered by their versions when both are
 * known: the later reading wins, with or without a position, so the core's text-only GPS write (which drops the
 * position) still applies while an older spill line replayed over a newer save never does. Otherwise the fragment with
 * the greatest key wins, whatever order the fragments arrive in. A keyed fragment beats one with no key (a row an older
 * build stored, a text-only GPS line, a ring of unknown run); between two with none, the newer fragment wins
 * (call_history_fold_adopts()), the rule every other field follows. Nothing never erases something here; a later
 * reading of one fragment that holds nothing withdraws that fragment's unit instead (call_history_gps_withdraws()). A
 * reading an older store kept without a version: the same fragment read again with no key takes what it says now.
 *
 * @param stored_known   The row holds GPS text or a position.
 * @param stored_key     Its key.
 * @param stored         The fragment it came from.
 * @param incoming_known The fragment carries GPS text or a position.
 * @param incoming_key   Its key.
 * @param incoming       The fragment.
 */
inline bool
call_history_gps_adopts(bool stored_known, const CallHistoryPositionKey& stored_key,
                        const CallHistoryProvenance& stored, bool incoming_known,
                        const CallHistoryPositionKey& incoming_key, const CallHistoryProvenance& incoming) {
    if (!incoming_known) {
        return false;
    }
    if (!stored_known) {
        return true;
    }
    const int order = call_history_provenance_compare(incoming, stored);
    const bool same_fragment = call_history_same_fragment(incoming, incoming_key.ord, stored, stored_key.ord);
    if (same_fragment && incoming_key.version != 0U && stored_key.version != 0U) {
        return incoming_key.version >= stored_key.version;
    }
    if (stored_key.keyed && incoming_key.keyed) {
        // The start orders distinct fragments only: the same fragment read again takes what it says now.
        const int keys = call_history_position_key_compare(incoming_key, stored_key);
        return keys > 0 || (keys == 0 && (same_fragment || order >= 0));
    }
    if (same_fragment && !incoming_key.keyed) {
        return true;
    }
    if (stored_key.keyed != incoming_key.keyed) {
        return incoming_key.keyed;
    }
    return order >= 0;
}

/**
 * @brief Whether a reading of a fragment with no GPS text and no position withdraws the GPS unit a row holds of that
 * fragment (issue #800, 2.7): the core cleared both (a report that no longer qualifies for the row), so the row no
 * longer shows or saves them.
 *
 * Only a reading of the same fragment (call_history_same_fragment()), and only one at least as late as the held one
 * by the rule call_history_gps_adopts() orders two readings of a fragment: by their versions when both are known, else
 * the fragment read again takes what it says now. A reading of another fragment never withdraws anything.
 *
 * @param stored_key The held unit's key.
 * @param stored     The fragment the held unit came from.
 * @param reading_key The empty reading's key.
 * @param reading     The fragment the empty reading came from.
 */
inline bool
call_history_gps_withdraws(const CallHistoryPositionKey& stored_key, const CallHistoryProvenance& stored,
                           const CallHistoryPositionKey& reading_key, const CallHistoryProvenance& reading) {
    if (!call_history_same_fragment(reading, reading_key.ord, stored, stored_key.ord)) {
        return false;
    }
    if (reading_key.version != 0U && stored_key.version != 0U) {
        return reading_key.version >= stored_key.version;
    }
    return true;
}

/** @brief FNV-1a (32 bits) of @p len bytes, folded into @p hash. */
inline uint32_t
call_history_fnv1a(uint32_t hash, const void* data, size_t len) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0U; i < len; i++) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

/**
 * @brief A digest of a ring row's text (its GPS text or text message): FNV-1a over its bytes up to the NUL, bounded by
 *        @p capacity; 0 for no text, and never 0 for some.
 *
 * The seen store keeps it rather than the text: the core writes both into a committed voice row in place (a late
 * enrichment), and only the change matters (issue #800, B2).
 */
inline uint32_t
call_history_text_digest(const char* text, size_t capacity) {
    size_t len = 0U;
    while (len < capacity && text[len] != '\0') {
        len++;
    }
    if (len == 0U) {
        return 0U;
    }
    const uint32_t hash = call_history_fnv1a(2166136261U, text, len);
    return hash != 0U ? hash : 1U;
}

/**
 * @brief A digest of an admitted position (Event_History::pos): its coordinates, error, flags, kind, rev and process
 *        instance; 0 when the row carries none, and never 0 for one.
 */
inline uint32_t
call_history_position_digest(bool valid, int32_t lat_e7, int32_t lon_e7, uint32_t err_m, uint16_t flags, uint8_t kind,
                             uint64_t rev, uint64_t proc_instance) {
    if (!valid) {
        return 0U;
    }
    uint32_t hash = 2166136261U;
    hash = call_history_fnv1a(hash, &lat_e7, sizeof lat_e7);
    hash = call_history_fnv1a(hash, &lon_e7, sizeof lon_e7);
    hash = call_history_fnv1a(hash, &err_m, sizeof err_m);
    hash = call_history_fnv1a(hash, &flags, sizeof flags);
    hash = call_history_fnv1a(hash, &kind, sizeof kind);
    hash = call_history_fnv1a(hash, &rev, sizeof rev);
    hash = call_history_fnv1a(hash, &proc_instance, sizeof proc_instance);
    return hash != 0U ? hash : 1U;
}

/** @brief The length of @p text up to its NUL, at most @p capacity. */
inline size_t
call_history_text_len(const char* text, size_t capacity) {
    size_t len = 0U;
    while (len < capacity && text[len] != '\0') {
        len++;
    }
    return len;
}

/** @brief @p scope's fields folded into FNV-1a @p hash, one by one (no padding is read). */
inline uint32_t
call_history_scope_fnv1a(uint32_t hash, const CallHistoryScope& scope) {
    hash = call_history_fnv1a(hash, &scope.kind, sizeof scope.kind);
    hash = call_history_fnv1a(hash, &scope.a, sizeof scope.a);
    hash = call_history_fnv1a(hash, &scope.b, sizeof scope.b);
    return call_history_fnv1a(hash, &scope.freqHz, sizeof scope.freqHz);
}

/**
 * @brief A digest of a ring row's raw identity (issue #800): its sync type, where it was heard (Event_History::rx_scope,
 *        and whether it was read), its sender's system (pos.scope with its discriminator) and its source and target
 *        text, each text up to its NUL within its capacity; never 0.
 *
 * The core can resolve a committed voice row's identity in place (a reacquired segment that learned its system), and a
 * fragment that then names another system than the row it joined must take back what it gave that row, so any change
 * is an advance.
 */
inline uint32_t
call_history_identity_digest(int systype, bool rx_resolved, const CallHistoryScope& rx, const CallHistoryScope& sender,
                             const char* src_text, size_t src_capacity, const char* tgt_text, size_t tgt_capacity) {
    const uint8_t resolved = rx_resolved ? 1U : 0U;
    uint32_t hash = call_history_fnv1a(2166136261U, &systype, sizeof systype);
    hash = call_history_fnv1a(hash, &resolved, sizeof resolved);
    hash = call_history_scope_fnv1a(hash, rx);
    hash = call_history_scope_fnv1a(hash, sender);
    hash = call_history_fnv1a(hash, src_text, call_history_text_len(src_text, src_capacity));
    const uint8_t separator = 0U;
    hash = call_history_fnv1a(hash, &separator, sizeof separator);
    hash = call_history_fnv1a(hash, tgt_text, call_history_text_len(tgt_text, tgt_capacity));
    return hash != 0U ? hash : 1U;
}

/**
 * @brief Whether an admitted position's values are ones a report could hold: latitude within +/-90 and longitude within
 *        +/-180 degrees (x 1e7), not exactly (0, 0), a known kind and only defined flags (position.h).
 *
 * A stored row's position is read through it, so a corrupt or hand-edited store loads as no position rather than one
 * nobody received.
 */
inline bool
call_history_position_values_valid(int64_t lat_e7, int64_t lon_e7, int64_t kind, int64_t flags) {
    return lat_e7 >= -900000000LL && lat_e7 <= 900000000LL && lon_e7 >= -1800000000LL && lon_e7 <= 1800000000LL
           && !(lat_e7 == 0 && lon_e7 == 0) && kind >= 1 && kind <= static_cast<int64_t>(DSD_POS_KIND_LAST)
           && flags >= 0 && (flags & ~static_cast<int64_t>(DSD_POS_FLAG_ALL)) == 0;
}

/* Sanity bound on a row's start/end stamps: a span longer than this is a corrupt
 * or clock-shifted row, not a measured call, so it renders as unknown. */
constexpr int64_t kCallHistoryMaxPlausibleDurationSecs = 3600;

/**
 * @brief Measured duration from a row's stamped ends, or -1 when unknown.
 *
 * The single definition of "plausible" shared by first ingest and every later
 * merge, so a duration a fresh row would refuse cannot sneak in via a merge.
 */
inline int
call_history_duration_secs(int64_t start, int64_t end) {
    if (start <= 0 || end < start || end - start > kCallHistoryMaxPlausibleDurationSecs) {
        return -1;
    }
    return static_cast<int>(end - start);
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_MERGE_H_ */
