// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief ASCII NMEA 0183 sentences in a data payload: one sentence at a time with its own checksum, and the measured
 * fix an RMC or GGA sentence carries (issue #800, 2.4 and the RMC/GGA rule in 2.2). Pure: no decoder state, no output.
 *
 * Sources: gpsd "NMEA Revealed" and the u-blox 8 / M8 receiver description (UBX-13003221, section 31): the checksum is
 * the XOR of the characters between the '$' or '!' and the '*', sent as two hex digits; any two-letter talker
 * (GP, GL, GA, GB, BD, GN, ...) sends RMC and GGA, while '$P' starts a proprietary sentence; latitude is ddmm.m...,
 * longitude dddmm.m...; RMC speed is in knots; GGA has no date; HDOP is a ratio, not metres (never read).
 */

#ifndef DSD_NEO_SRC_CORE_GPS_NMEA_SENTENCE_H_
#define DSD_NEO_SRC_CORE_GPS_NMEA_SENTENCE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What one sentence's own framing and checksum said (nmea_sentence_span::status). */
typedef enum {
    NMEA_SENTENCE_OK = 0,       /**< the transmitted checksum matches the computed one */
    NMEA_SENTENCE_NO_STAR = 1,  /**< the line ended before a '*' */
    NMEA_SENTENCE_BAD_HEX = 2,  /**< the '*' is not followed by two hex digits */
    NMEA_SENTENCE_CHECKSUM = 3, /**< the checksums differ */
} nmea_sentence_status;

/** One sentence of a payload: bytes [start, start + len) run from the '$' or '!' through the two checksum digits (or
 * through what was read when the framing failed). */
typedef struct {
    size_t start;
    size_t len;
    uint8_t status;   /**< nmea_sentence_status */
    uint8_t calc;     /**< the computed checksum */
    uint8_t received; /**< the transmitted checksum (NMEA_SENTENCE_OK and NMEA_SENTENCE_CHECKSUM only) */
} nmea_sentence_span;

/**
 * @brief Find the next sentence of @p bytes at or after @p *pos.
 *
 * CR and LF between sentences are skipped. The payload ends at its last byte or at a NUL (padding). A sentence starts
 * with '$' or '!' and runs over printable characters to its '*' and two hex digits. After a sentence whose framing
 * failed (no '*', or no two hex digits after it) the search goes on at the next CR, LF, '$', '!' or NUL, so the
 * sentences after it are still read.
 *
 * @return 1 with @p out filled and @p *pos moved past the sentence (whatever its status); 0 at the end of the payload;
 *         -1 when the byte at @p *pos can start no sentence (@p *pos is left on it).
 */
int nmea_next_sentence(const uint8_t* bytes, size_t len, size_t* pos, nmea_sentence_span* out);

/** The sentence formatter of a parsed sentence (nmea_fix::type). */
typedef enum {
    NMEA_TYPE_OTHER = 0,
    NMEA_TYPE_RMC = 1,
    NMEA_TYPE_GGA = 2,
} nmea_sentence_type;

/** What an RMC or GGA sentence says about the position. Unknown values carry the <dsd-neo/core/position.h>
 * sentinels. */
typedef struct {
    uint8_t type; /**< nmea_sentence_type */
    /** 1 when the sentence's own status says it is a measured fix: GGA quality 1-5; RMC status A and, when the
     * NMEA 2.3 mode field is sent, mode A, D, F, R or P. */
    uint8_t measured_fix;
    uint8_t has_position;  /**< 1 when latitude and longitude were sent, well formed and in range */
    uint8_t sats;          /**< GGA satellites in use; DSD_POSITION_SATS_NONE */
    int32_t lat_e7;        /**< degrees x 1e7, north positive (has_position) */
    int32_t lon_e7;        /**< degrees x 1e7, east positive (has_position) */
    int32_t alt_dm;        /**< GGA altitude above mean sea level, decimetres; DSD_POSITION_ALT_UNKNOWN */
    uint16_t speed_dkmh;   /**< RMC speed over ground, 0.1 km/h; DSD_POSITION_SPEED_UNKNOWN */
    uint16_t heading_ddeg; /**< RMC course over ground, 0.1 degree true (0-3599); DSD_POSITION_HEADING_UNKNOWN */
    int64_t fix_time_s; /**< RMC date and time as Unix seconds (years 2000-2099); 0 when either is missing or invalid */
    /** Why the sentence gives no position ("" when measured_fix and has_position are both 1). */
    const char* refusal;
} nmea_fix;

/**
 * @brief Read the fix of one checked sentence.
 *
 * @param sentence The sentence from its '$' through its checksum digits (a span nmea_next_sentence() returned with
 *        NMEA_SENTENCE_OK); only the text before the '*' is read.
 * @param len Its length in bytes.
 * @param out Filled whatever the sentence is (NMEA_TYPE_OTHER for any sentence but RMC and GGA).
 *
 * Every field is range-checked before it is converted, and an invalid one is unknown, never normalised: latitude
 * degrees <= 90, longitude degrees <= 180, minutes <= 59, the boundary degree only with zero minutes; hour <= 23,
 * minute <= 59, second <= 59, month 1-12, day within its month; a course of 360 degrees or more is unknown.
 */
void nmea_parse_fix(const char* sentence, size_t len, nmea_fix* out);

/** 1 when @p year-@p month-@p day is a calendar date (month 1-12, day within its month), else 0. */
int nmea_civil_date_valid(int64_t year, uint32_t month, uint32_t day);

/** Unix seconds of a UTC date and time, or -1 when the date is no calendar date (nmea_civil_date_valid()) or the time
 * is out of range (hour <= 23, minute <= 59, second <= 59). */
int64_t nmea_utc_seconds(int64_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute, uint32_t second);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_CORE_GPS_NMEA_SENTENCE_H_ */
