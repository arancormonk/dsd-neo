// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The verified position feed: one plain record per received position report, the admission every report
 * passes before it reaches `-L`, history or the feed, and the session's ring of kept reports.
 *
 * Producers (the protocol decoders that read a position) fill a dsd_position_fix (start from
 * dsd_position_fix_init(), which sets every "unknown" sentinel), range-check each field before they convert it and
 * leave a field they cannot read unknown, then call dsd_position_admit() with the verdicts of the checks they actually
 * ran. Only an admitted report may be written to `-L` or to a history row; a refused one goes nowhere.
 *
 * Admission (dsd_position_admit()), in order:
 * 1. Gate: the position check passed (pos_verified) and the receive policy (dsd_receive_allows()) admits the report's
 *    protocol and slot; else DSD_POSITION_REFUSED.
 * 2. Identity: without source_verified (or with DSD_POS_FLAG_SOURCE_HASHED) the source, callsign, target and
 *    DSD_POS_FLAG_TARGET_GROUP are cleared; the report format (kind) is kept.
 * 3. Origin: a multi-fragment report is refused when its first fragment was heard under another reception generation
 *    (dsd_position_origin_capture() / dsd_position_origin_matches()).
 * 4. Values: |lat| <= 90, |lon| <= 180, exactly (0,0) refused; an out-of-range heading becomes unknown.
 * 5. Freeze: the reception scope, the sender scope, the identity discriminator and the label at acceptance.
 * 6. Stamp: the process-wide revision (rev), proc_instance, rx_time_s and rx_clock.
 * 8. Feed: a repeat of the same track's last kept report (every observable field equal, kept less than
 *    DSD_POSITION_REPEAT_MS ago on the decode clock) is not kept; anything else is appended to the ring with the next
 *    seq. Without a ring this step is skipped.
 *
 * The ring (DSD_POSITION_FEED_CAPACITY records) lives in the state extension slot DSD_STATE_EXT_CORE_POSITION_FEED.
 * The engine creates it at the start of every run (dsd_position_feed_start()); it is freed with every other extension
 * when the run ends, so every run is a new stream (stream_id, never 0, never repeated in the process) whose seq starts
 * at 1. It is not part of a trunk-scan target's saved state, and no carrier loss or decoder reset clears it.
 *
 * Threading: the live ring is written and read on the decoder thread only. Frontends read the copies app-control
 * publishes (dsd_position_feed_copy_since() is the publish hop's read).
 *
 * Fixed point everywhere (degrees x 1e7, decimetres, 0.1 km/h, 0.1 degree), so no reader compares floats.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_POSITION_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_POSITION_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Records the feed ring holds; the oldest is overwritten first. */
#define DSD_POSITION_FEED_CAPACITY    1024
/** Tracks whose last kept report the repeat rule remembers; the least recently kept track is forgotten first. */
#define DSD_POSITION_FEED_TRACKS      256
/** A report equal to its track's last kept one is a repeat while that one was kept less than this long ago (decode
 * clock, dsd_decode_now_mono_ms()). */
#define DSD_POSITION_REPEAT_MS        30000U

/** dsd_position_fix::alt_dm when the altitude is unknown. */
#define DSD_POSITION_ALT_UNKNOWN      INT32_MIN
/** dsd_position_fix::err_m when the position error is unknown. */
#define DSD_POSITION_ERR_UNKNOWN      0U
/** dsd_position_fix::speed_dkmh when the speed is unknown. */
#define DSD_POSITION_SPEED_UNKNOWN    UINT16_MAX
/** dsd_position_fix::heading_ddeg when the heading is unknown; a known heading is 0..DSD_POSITION_HEADING_MAX. */
#define DSD_POSITION_HEADING_UNKNOWN  UINT16_MAX
/** The largest known heading: 359.9 degrees. */
#define DSD_POSITION_HEADING_MAX      3599U
/** dsd_position_fix::age_class when the report sent none (LIP time elapsed). */
#define DSD_POSITION_AGE_CLASS_NONE   0xFFU
/** dsd_position_fix::sats when the report sent no satellite count. */
#define DSD_POSITION_SATS_NONE        0xFFU
/** dsd_position_fix::protocol before the producer sets it: dsd_position_admit() refuses it. */
#define DSD_POSITION_PROTOCOL_NONE    0xFFU
/** Bytes of dsd_position_fix::source_text, the NUL included. */
#define DSD_POSITION_SOURCE_TEXT_SIZE 16
/** Bytes of dsd_position_fix::label, the NUL included. */
#define DSD_POSITION_LABEL_SIZE       32

/** dsd_position_fix::kind: the report format. The values are stable: never renumber. */
typedef enum {
    DSD_POS_DMR_LRRP = 1,     /**< DMR LRRP over an assembled data packet. */
    DSD_POS_DMR_LOCN = 2,     /**< DMR LOCN text report. */
    DSD_POS_DMR_EMBEDDED = 3, /**< DMR embedded GPS LC (5-bit checksum: DSD_POS_FLAG_WEAK_CHECK). */
    DSD_POS_LIP_USBD = 4,     /**< DMR USBD LIP (source hashed). */
    DSD_POS_LIP = 5,          /**< LIP over DMR UDT or UDP. */
    DSD_POS_NMEA = 6,         /**< NMEA (binary UDT, or ASCII RMC/GGA). */
    DSD_POS_P25_APX = 7,      /**< P25 Motorola APX embedded GPS (MFID $90 LCO $06). */
    DSD_POS_P25_HARRIS = 8,   /**< P25 L3Harris GPS (Phase 1 LC or Phase 2 MAC). */
    DSD_POS_NXDN_GPS = 9,     /**< NXDN GPS data. */
    DSD_POS_DSTAR_DPRS = 10,  /**< D-STAR DPRS (uncompressed APRS). */
    DSD_POS_M17_GNSS = 11,    /**< M17 GNSS META or packet. */
    DSD_POS_TETRA_LIP = 12,   /**< TETRA LIP short report over SDS. */
    DSD_POS_KIND_LAST = DSD_POS_TETRA_LIP,
} dsd_position_kind;

/** dsd_position_fix::flags. The values are stable. */
typedef enum {
    /** The source ID is a hash (DMR USBD), never a radio ID: the identity is cleared at admission. */
    DSD_POS_FLAG_SOURCE_HASHED = 1U << 0,
    /** No checked provenance named the source: source, callsign, target and TARGET_GROUP were cleared. */
    DSD_POS_FLAG_SOURCE_UNVERIFIED = 1U << 1,
    /** target_id is a talkgroup; clear: a radio (private). */
    DSD_POS_FLAG_TARGET_GROUP = 1U << 2,
    /** The position check is weak (DMR embedded GPS: 5-bit sum mod 31). */
    DSD_POS_FLAG_WEAK_CHECK = 1U << 3,
    /** speed_dkmh is a lower bound (LIP speed code 126; binary UDT NMEA 127 kn). */
    DSD_POS_FLAG_SPEED_LOWER_BOUND = 1U << 4,
    /** err_m is a lower bound (LIP "> 200 km"; M17 radius code 7, "> 64 m"). */
    DSD_POS_FLAG_ERR_LOWER_BOUND = 1U << 5,
    /** The heading is ambiguous (LIP direction of travel 15). */
    DSD_POS_FLAG_HEADING_AMBIGUOUS = 1U << 6,
    /** The radio may have failed to get a fix: the position may be old or invalid (position error 7, decision D11). */
    DSD_POS_FLAG_VALIDITY_UNKNOWN = 1U << 7,
    /** The format rests on fewer than two sources (decision D12). */
    DSD_POS_FLAG_FORMAT_PROVISIONAL = 1U << 8,
    /** The report carried an emergency indication (L3Harris E bit). */
    DSD_POS_FLAG_EMERGENCY = 1U << 9,
    /** Every defined flag; admission clears any other bit. */
    DSD_POS_FLAG_ALL = (1U << 10) - 1U,
} dsd_position_flag;

/** dsd_position_scope::kind. 0..5 are the alias scope kinds (dsd_alias_scope_kind, <dsd-neo/core/alias_learn.h>),
 * with the same a and b; the feed-only kinds name a reception that has no alias scope (decision D7). Stable values. */
typedef enum {
    DSD_POS_SCOPE_NONE = 0,    /**< No identity (M17 and D-STAR name their sources by callsign). */
    DSD_POS_SCOPE_P25 = 1,     /**< a = WACN, b = SYSID. */
    DSD_POS_SCOPE_DMR = 2,     /**< As dsd_alias_scope for DMR. */
    DSD_POS_SCOPE_NXDN = 3,    /**< As dsd_alias_scope for NXDN. */
    DSD_POS_SCOPE_TETRA = 4,   /**< a = MCC, b = MNC. */
    DSD_POS_SCOPE_P25_NAC = 5, /**< a = NAC, b = 0; not unique: the identity adds the tuned frequency. */
    /** Feed only: an EDACS system. a = 0x10000 | site when the site is known, else 0; b = 0x10000 | system when the
     * system is known, else 0. */
    DSD_POS_SCOPE_FEED_EDACS = 0x40,
    /** Feed only: the control channel the system was received on. a = its frequency in Hz, b = 0. */
    DSD_POS_SCOPE_FEED_CC_FREQ = 0x41,
    /** Feed only: the tuned frequency. a = the frequency in Hz, b = 0. */
    DSD_POS_SCOPE_FEED_TUNED_FREQ = 0x42,
} dsd_position_scope_kind;

/** A system identity: an alias scope or a feed-only one (dsd_position_scope_kind). DSD_POS_SCOPE_NONE has every field
 * 0, so two scopes compare by their fields. */
typedef struct {
    uint8_t kind; /**< dsd_position_scope_kind */
    uint8_t pad[3];
    uint32_t a, b;
} dsd_position_scope;

/**
 * @brief One position report.
 *
 * The producer fills the report's own fields (marked "producer"); admission fills the rest (marked "admission").
 * A track is the report's typed source: the protocol family, the sender scope with the identity discriminator, and the
 * numeric source or the callsign (dsd_position_same_track()).
 */
typedef struct dsd_position_fix {
    uint64_t stream_id;     /**< admission: the feed stream that kept it; 0 when not kept */
    uint64_t seq;           /**< admission: 1-based within the stream; 0 when not kept */
    uint64_t proc_instance; /**< admission: dsd_position_proc_instance() */
    uint64_t rev;           /**< admission: process-wide revision, increasing with every admitted report */
    uint64_t origin_gen;    /**< admission: the reception generation the report came from (dsd_position_origin) */
    int64_t rx_time_s;      /**< admission: dsd_decode_time() at acceptance */
    int64_t fix_time_s;     /**< producer: the report's own full UTC time (Unix seconds); 0 = none */
    int64_t freq_hz;        /**< admission: the tuned frequency the report was heard on; 0 = unknown */
    /** admission: the identity discriminator (D7): the tuned frequency for a DSD_POS_SCOPE_P25_NAC sender scope (two
     * conventional systems can share a NAC), else 0. */
    int64_t identity_freq_hz;
    int32_t lat_e7;        /**< producer: latitude, degrees x 1e7, north positive */
    int32_t lon_e7;        /**< producer: longitude, degrees x 1e7, east positive */
    int32_t alt_dm;        /**< producer: altitude, decimetres; DSD_POSITION_ALT_UNKNOWN */
    uint32_t err_m;        /**< producer: position error, metres; DSD_POSITION_ERR_UNKNOWN (0) */
    uint32_t source_id;    /**< producer: radio ID; 0 = none (cleared without checked provenance) */
    uint32_t target_id;    /**< producer: target ID; 0 = none; group with DSD_POS_FLAG_TARGET_GROUP */
    uint16_t speed_dkmh;   /**< producer: speed, 0.1 km/h; DSD_POSITION_SPEED_UNKNOWN */
    uint16_t heading_ddeg; /**< producer: heading, 0.1 degree, 0..3599; DSD_POSITION_HEADING_UNKNOWN */
    uint16_t flags;        /**< producer and admission: dsd_position_flag */
    uint8_t rx_clock;      /**< admission: dsd_decode_clock_source() at acceptance */
    uint8_t age_class;     /**< producer: LIP time elapsed code; DSD_POSITION_AGE_CLASS_NONE */
    uint8_t sats;          /**< producer: satellites in use; DSD_POSITION_SATS_NONE */
    uint8_t protocol;      /**< producer: the DSD_SYNC_* value of the carrier (<dsd-neo/core/synctype_ids.h>) */
    uint8_t kind;          /**< producer: dsd_position_kind; never cleared */
    uint8_t slot;          /**< producer: the lane (0 or 1) of a two-slot protocol, else 0 */
    /** admission: where the report was heard (D7: the alias scope, else a feed-only scope; none for M17/D-STAR). */
    dsd_position_scope reception_scope;
    /** producer (TETRA only: the calling party's own network when sent) or admission: the sender's system, equal to
     * the reception scope unless the producer set it. The track key uses it. */
    dsd_position_scope sender_scope;
    char source_text[DSD_POSITION_SOURCE_TEXT_SIZE]; /**< producer: callsign (D-STAR MY 8+4, M17); "" = none */
    char label[DSD_POSITION_LABEL_SIZE];             /**< admission: the source's label at acceptance; "" = none */
} dsd_position_fix;

/** dsd_position_admit() results. */
typedef enum {
    DSD_POSITION_REFUSED = 0,           /**< Nothing may use the report: no `-L`, no history, no feed. */
    DSD_POSITION_ADMITTED_NOT_KEPT = 1, /**< Admitted (`-L` and history may use it); not appended to the feed. */
    DSD_POSITION_ADMITTED_KEPT = 2,     /**< Admitted and appended to the feed (stream_id and seq set). */
} dsd_position_admit_result;

/**
 * @brief The reception a report came from: captured with a multi-fragment report's first fragment, so its completion
 * keeps the reception the report began on and is refused when the receiver moved on between fragments.
 */
typedef struct dsd_position_origin {
    dsd_position_scope scope; /**< the reception scope (D7) */
    int64_t freq_hz;          /**< the tuned frequency (dsd_opts_tuned_freq_hz()); 0 = unknown */
    /** The reception generation: the received identity's epoch (dsd_received_identity::epoch) in the low 32 bits, the
     * trunk tuning generation (dsd_trunk_tuning_generation(), advanced by every completed retune) in the high 32. */
    uint64_t gen;
    uint8_t valid; /**< 1 once captured; a zeroed origin never matches */
} dsd_position_origin;

/** Where a reader of the feed stands (dsd_position_feed_copy_since()). */
typedef struct {
    uint64_t stream_id;  /**< the ring's stream; 0 without a ring */
    uint64_t oldest_seq; /**< the oldest seq held; 0 while empty */
    uint64_t newest_seq; /**< the newest seq held; 0 while empty */
    uint64_t last_seq;   /**< the last seq returned (the next after_seq); after_seq when none was (0 without a ring) */
    uint8_t more;        /**< entries remain past the ones returned */
    uint8_t lost;        /**< entries after after_seq were overwritten before this read: a gap before the first one */
} dsd_position_feed_meta;

/** Set every field of @p fix to its "unknown" value: zeroes, DSD_POSITION_ALT_UNKNOWN, DSD_POSITION_SPEED_UNKNOWN,
 * DSD_POSITION_HEADING_UNKNOWN, DSD_POSITION_AGE_CLASS_NONE, DSD_POSITION_SATS_NONE and DSD_POSITION_PROTOCOL_NONE.
 * NULL does nothing. */
void dsd_position_fix_init(dsd_position_fix* fix);

/** This process's instance: 64 random bits (never 0) drawn once, at the first call. Any thread. */
uint64_t dsd_position_proc_instance(void);

/** The latest revision admission stamped in this process (0 before the first admitted report). Any thread. */
uint64_t dsd_position_last_rev(void);

/**
 * @brief Capture the current reception into @p out: its scope (D7: the alias scope of @p state's received identity;
 * else, for EDACS, its site and system; else the control channel frequency the identity holds; else the tuned
 * frequency; else none), the tuned frequency and the reception generation. Decoder thread.
 *
 * A NULL @p opts reads no tuned frequency; a NULL @p state captures an empty origin; a NULL @p out does nothing.
 */
void dsd_position_origin_capture(const dsd_opts* opts, const dsd_state* state, dsd_position_origin* out);

/** 1 when @p origin was captured (valid) under the reception generation @p state is in now; 0 otherwise (a retune,
 * a trunk-scan target switch or a received identity epoch change since, or NULL). */
int dsd_position_origin_matches(const dsd_state* state, const dsd_position_origin* origin);

/**
 * @brief Admit one position report (the file comment lists the steps).
 *
 * @param opts Decoder options (receive policy, tuned frequency); may be NULL.
 * @param state Decoder state; NULL refuses. The feed it appends to lives in the state's extension slot.
 * @param fix In: the producer's report. Out, when admitted: the admitted record (identity cleared without checked
 *        provenance, scopes, label, stamps; stream_id and seq when kept). Left unchanged when refused.
 * @param pos_verified 1 when the producer's own check of the position passed.
 * @param source_verified 1 when the source, target and group/private kind came from checked provenance.
 * @param origin The reception captured with a multi-fragment report's first fragment (refused unless it still
 *        matches, dsd_position_origin_matches()); NULL for a single-burst report, which takes the current reception.
 * @return a dsd_position_admit_result. A report with no kind or protocol is refused.
 */
dsd_position_admit_result dsd_position_admit(const dsd_opts* opts, const dsd_state* state, dsd_position_fix* fix,
                                             int pos_verified, int source_verified, const dsd_position_origin* origin);

/** 1 when @p a and @p b are reports of the same track: neither anonymous (hashed, unverified, or neither a source ID
 * nor a callsign), the same protocol family, sender scope and identity discriminator, and the same source ID and
 * callsign. */
int dsd_position_same_track(const dsd_position_fix* a, const dsd_position_fix* b);

/**
 * @brief Start the run's feed: a new stream (a new stream_id, seq restarting at 1, no remembered tracks) in @p state's
 * extension slot, allocated when the slot has none. The engine calls it at the start of every run, never on the
 * decode path. Returns 0, or -1 for NULL @p state or a failed allocation (the feed then stays off for the run and
 * admission still works).
 */
int dsd_position_feed_start(dsd_state* state);

/** The stream of @p state's ring; 0 without a ring. */
uint64_t dsd_position_feed_stream_id(const dsd_state* state);

/**
 * @brief Copy the kept records with seq above @p after_seq, oldest first, up to @p cap of them, into @p out.
 *
 * No allocation. @p meta (required) is written on every return >= 0: the stream, the seqs held, the last seq returned,
 * whether more remain and whether entries after @p after_seq were already overwritten (lost). Without a ring it
 * returns 0 with a zeroed meta (stream 0). A reader that follows another stream starts over from after_seq 0.
 *
 * @return the number of records copied, or -1 for a NULL @p state or @p meta, or a NULL @p out with @p cap > 0.
 */
int dsd_position_feed_copy_since(const dsd_state* state, uint64_t after_seq, dsd_position_fix* out, size_t cap,
                                 dsd_position_feed_meta* meta);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_POSITION_H_ */
