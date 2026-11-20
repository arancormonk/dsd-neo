// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief ASCII NMEA 0183 sentences: framing and checksum per sentence, and the RMC / GGA measured fix
 * (nmea_sentence.h).
 */

#include "nmea_sentence.h"

#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/position.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Framing ---- */

static int
nmea_is_line_end(uint8_t c) {
    return c == (uint8_t)'\r' || c == (uint8_t)'\n';
}

/* A character a sentence body may hold: printable, and none of the characters that start a sentence. */
static int
nmea_is_body_char(uint8_t c) {
    return c >= 0x20U && c < 0x7FU && c != (uint8_t)'$' && c != (uint8_t)'!';
}

static int
nmea_hex_value(uint8_t c) {
    if (c >= (uint8_t)'0' && c <= (uint8_t)'9') {
        return c - (uint8_t)'0';
    }
    if (c >= (uint8_t)'A' && c <= (uint8_t)'F') {
        return c - (uint8_t)'A' + 10;
    }
    if (c >= (uint8_t)'a' && c <= (uint8_t)'f') {
        return c - (uint8_t)'a' + 10;
    }
    return -1;
}

/* After a sentence whose framing failed: the next line end, sentence start or NUL from @p i, so that the sentences
 * after it are still read. */
static size_t
nmea_resync(const uint8_t* bytes, size_t len, size_t i) {
    while (i < len && !nmea_is_line_end(bytes[i]) && bytes[i] != (uint8_t)'$' && bytes[i] != (uint8_t)'!'
           && bytes[i] != 0U) {
        i++;
    }
    return i;
}

/* The sentence's end from @p star (where its body stopped): the '*', the two checksum digits and the verdict. Returns
 * where the next search starts. */
static size_t
nmea_close_sentence(const uint8_t* bytes, size_t len, size_t star, nmea_sentence_span* out) {
    if (star >= len || bytes[star] != (uint8_t)'*') {
        out->status = NMEA_SENTENCE_NO_STAR;
        out->len = star - out->start;
        return nmea_resync(bytes, len, star);
    }
    const int hi = (star + 1U < len) ? nmea_hex_value(bytes[star + 1U]) : -1;
    const int lo = (star + 2U < len) ? nmea_hex_value(bytes[star + 2U]) : -1;
    if (hi < 0 || lo < 0) {
        out->status = NMEA_SENTENCE_BAD_HEX;
        out->len = star + 1U - out->start;
        return nmea_resync(bytes, len, star + 1U);
    }
    out->received = (uint8_t)((hi << 4) | lo);
    out->status = (out->received == out->calc) ? NMEA_SENTENCE_OK : NMEA_SENTENCE_CHECKSUM;
    out->len = star + 3U - out->start;
    return star + 3U;
}

int
nmea_next_sentence(const uint8_t* bytes, size_t len, size_t* pos, nmea_sentence_span* out) {
    if (bytes == NULL || pos == NULL || out == NULL) {
        return 0;
    }
    size_t i = *pos;
    while (i < len && nmea_is_line_end(bytes[i])) {
        i++;
    }
    *pos = i;
    if (i >= len || bytes[i] == 0U) {
        return 0;
    }
    if (bytes[i] != (uint8_t)'$' && bytes[i] != (uint8_t)'!') {
        return -1;
    }
    out->start = i;
    out->calc = 0U;
    out->received = 0U;
    i++;
    while (i < len && nmea_is_body_char(bytes[i]) && bytes[i] != (uint8_t)'*') {
        out->calc ^= bytes[i];
        i++;
    }
    *pos = nmea_close_sentence(bytes, len, i, out);
    return 1;
}

/* ---- Fields ---- */

enum { NMEA_MAX_FIELDS = 24 };

typedef struct {
    const char* p;
    size_t n;
} nmea_field;

typedef struct {
    nmea_field f[NMEA_MAX_FIELDS];
    size_t count;
} nmea_fields;

/* Split the text between the '$' and the '*' at its commas (field 0 is the address). */
static void
nmea_split(const char* sentence, size_t len, nmea_fields* out) {
    out->count = 0U;
    size_t end = 1U;
    while (end < len && sentence[end] != '*') {
        end++;
    }
    size_t start = 1U;
    for (size_t i = 1U; i <= end && out->count < NMEA_MAX_FIELDS; i++) {
        if (i == end || sentence[i] == ',') {
            out->f[out->count].p = sentence + start;
            out->f[out->count].n = i - start;
            out->count++;
            start = i + 1U;
        }
    }
}

static const nmea_field*
nmea_get(const nmea_fields* fields, size_t index) {
    static const nmea_field empty = {"", 0U};
    return index < fields->count ? &fields->f[index] : &empty;
}

static int
nmea_is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* A number written as digits, an optional '.', and more digits: its integer part (at most @p max_int digits, at least
 * one) and its first @p keep fraction digits as an integer (zero-padded; further digits are dropped, but whether any
 * fraction digit at all is nonzero is kept). */
typedef struct {
    uint32_t whole;
    uint32_t frac;
    size_t int_digits;
    uint8_t frac_nonzero; /* some fraction digit, kept or dropped, is not 0 */
} nmea_number;

static int
nmea_parse_number(const nmea_field* f, size_t max_int, size_t keep, nmea_number* out) {
    size_t i = 0U;
    out->whole = 0U;
    out->frac = 0U;
    out->frac_nonzero = 0U;
    while (i < f->n && nmea_is_digit(f->p[i])) {
        if (i >= max_int) {
            return 0;
        }
        out->whole = (out->whole * 10U) + (uint32_t)(f->p[i] - '0');
        i++;
    }
    out->int_digits = i;
    if (i == 0U) {
        return 0;
    }
    size_t kept = 0U;
    if (i < f->n) {
        if (f->p[i] != '.') {
            return 0;
        }
        for (i++; i < f->n; i++) {
            if (!nmea_is_digit(f->p[i])) {
                return 0;
            }
            if (f->p[i] != '0') {
                out->frac_nonzero = 1U;
            }
            if (kept < keep) {
                out->frac = (out->frac * 10U) + (uint32_t)(f->p[i] - '0');
                kept++;
            }
        }
    }
    for (; kept < keep; kept++) {
        out->frac *= 10U;
    }
    return 1;
}

/* A count written as digits only (1 to @p max_digits of them): a field with a fraction, a sign or anything else is no
 * count, so it stays unknown. */
static int
nmea_parse_count(const nmea_field* f, size_t max_digits, uint32_t* out) {
    if (f->n == 0U || f->n > max_digits) {
        return 0;
    }
    uint32_t value = 0U;
    for (size_t i = 0U; i < f->n; i++) {
        if (!nmea_is_digit(f->p[i])) {
            return 0;
        }
        value = (value * 10U) + (uint32_t)(f->p[i] - '0');
    }
    *out = value;
    return 1;
}

static int
nmea_field_is(const nmea_field* f, char c) {
    return f->n == 1U && f->p[0] == c;
}

/* ---- Position ---- */

enum {
    NMEA_E7 = 10000000,
    NMEA_FRAC_DIGITS = 7, /* fraction-of-minute digits kept: 1e-7 minute */
};

/* ddmm.m... (latitude, @p deg_digits 2, @p max_deg 90) or dddmm.m... (longitude, 3, 180) and its hemisphere letter. The
 * components are checked before anything is converted: degrees <= max, minutes <= 59, the boundary degree only with
 * zero minutes and a fraction whose every digit is 0, those past the kept precision included. */
static int
nmea_parse_coordinate(const nmea_field* value, const nmea_field* hemi, size_t deg_digits, uint32_t max_deg,
                      const char* signs, int32_t* out_e7) {
    nmea_number num;
    if (!nmea_parse_number(value, deg_digits + 2U, NMEA_FRAC_DIGITS, &num) || num.int_digits != deg_digits + 2U) {
        return 0;
    }
    const uint32_t deg = num.whole / 100U;
    const uint32_t min = num.whole % 100U;
    if (deg > max_deg || min > 59U || (deg == max_deg && (min != 0U || num.frac_nonzero))) {
        return 0;
    }
    int sign = 0;
    if (nmea_field_is(hemi, signs[0])) {
        sign = 1;
    } else if (nmea_field_is(hemi, signs[1])) {
        sign = -1;
    } else {
        return 0;
    }
    const int64_t min_e7 = ((int64_t)min * NMEA_E7) + (int64_t)num.frac;
    const int64_t e7 = ((int64_t)deg * NMEA_E7) + ((min_e7 + 30) / 60);
    *out_e7 = (int32_t)(sign * e7);
    return 1;
}

static void
nmea_read_position(const nmea_fields* fields, size_t lat_index, nmea_fix* out) {
    int32_t lat_e7 = 0;
    int32_t lon_e7 = 0;
    if (nmea_parse_coordinate(nmea_get(fields, lat_index), nmea_get(fields, lat_index + 1U), 2U, 90U, "NS", &lat_e7)
        && nmea_parse_coordinate(nmea_get(fields, lat_index + 2U), nmea_get(fields, lat_index + 3U), 3U, 180U, "EW",
                                 &lon_e7)) {
        out->lat_e7 = lat_e7;
        out->lon_e7 = lon_e7;
        out->has_position = 1U;
    }
}

/* ---- Time ---- */

static int
nmea_is_leap(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int
nmea_days_in_month(int64_t year, uint32_t month) {
    static const uint8_t days[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    if (month == 2U && nmea_is_leap(year)) {
        return 29;
    }
    return days[month - 1U];
}

/* Days from 1970-01-01 to @p y-@p m-@p d (proleptic Gregorian, H. Hinnant's days_from_civil). */
static int64_t
nmea_days_from_civil(int64_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2U) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - (era * 400);
    const int64_t mp = (m > 2U) ? (int64_t)m - 3 : (int64_t)m + 9;
    const int64_t doy = ((153 * mp) + 2) / 5 + (int64_t)d - 1;
    const int64_t doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
    return (era * 146097) + doe - 719468;
}

int
nmea_civil_date_valid(int64_t year, uint32_t month, uint32_t day) {
    return month >= 1U && month <= 12U && day >= 1U && (int)day <= nmea_days_in_month(year, month);
}

int64_t
nmea_utc_seconds(int64_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute, uint32_t second) {
    if (!nmea_civil_date_valid(year, month, day) || hour > 23U || minute > 59U || second > 59U) {
        return -1;
    }
    return (nmea_days_from_civil(year, month, day) * 86400) + ((int64_t)hour * 3600) + ((int64_t)minute * 60)
           + (int64_t)second;
}

/* hhmmss with an optional fraction: seconds into the day, or -1 when malformed or out of range. */
static int64_t
nmea_parse_time_of_day(const nmea_field* f) {
    nmea_number num;
    if (!nmea_parse_number(f, 6U, 0U, &num) || num.int_digits != 6U) {
        return -1;
    }
    const uint32_t hh = num.whole / 10000U;
    const uint32_t mm = (num.whole / 100U) % 100U;
    const uint32_t ss = num.whole % 100U;
    if (hh > 23U || mm > 59U || ss > 59U) {
        return -1;
    }
    return ((int64_t)hh * 3600) + ((int64_t)mm * 60) + (int64_t)ss;
}

static int64_t
nmea_floor_div(int64_t a, int64_t b) {
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
}

int
dsd_gps_two_digit_year_date(uint32_t yy, uint32_t month, uint32_t day, int64_t receive_utc_s, int64_t* year_out,
                            int64_t* days_out) {
    if (yy > 99U) {
        return 0;
    }

    /* Within 50 years (of 365.25 days) of the reception: one of two neighbouring centuries' dates always is, unless the
       day is a calendar date in only one of them (29 February), which then may be none. */
    enum { NMEA_TWO_DIGIT_YEAR_WINDOW_DAYS = 18262 };

    const int64_t ref_days = nmea_floor_div(receive_utc_s, 86400);
    /* The receive year, near enough to pick the centuries around it (146097 days are 400 Gregorian years). */
    const int64_t ref_year = 1970 + nmea_floor_div(ref_days * 400, 146097);
    const int64_t base = ref_year - (((ref_year % 100) + 100) % 100);
    int found = 0;
    int64_t best_year = 0;
    int64_t best_days = 0;
    int64_t best_gap = 0;
    for (int64_t c = base - 100; c <= base + 100; c += 100) {
        const int64_t year = c + (int64_t)yy;
        if (!nmea_civil_date_valid(year, month, day)) {
            continue;
        }
        const int64_t days = nmea_days_from_civil(year, month, day);
        const int64_t gap = days >= ref_days ? days - ref_days : ref_days - days;
        if (gap > NMEA_TWO_DIGIT_YEAR_WINDOW_DAYS) {
            continue;
        }
        if (!found || gap < best_gap) {
            found = 1;
            best_year = year;
            best_days = days;
            best_gap = gap;
        }
    }
    if (found && year_out != NULL) {
        *year_out = best_year;
    }
    if (found && days_out != NULL) {
        *days_out = best_days;
    }
    return found;
}

/* ddmmyy: 1 with the days since 1970-01-01 in @p days_out, the year in the century closest to @p receive_utc_s; 0 when
   malformed or out of range. */
static int
nmea_parse_date(const nmea_field* f, int64_t receive_utc_s, int64_t* days_out) {
    if (f->n != 6U) {
        return 0;
    }
    nmea_number num;
    if (!nmea_parse_number(f, 6U, 0U, &num) || num.int_digits != 6U) {
        return 0;
    }
    const uint32_t dd = num.whole / 10000U;
    const uint32_t mo = (num.whole / 100U) % 100U;
    return dsd_gps_two_digit_year_date(num.whole % 100U, mo, dd, receive_utc_s, NULL, days_out);
}

/* ---- RMC and GGA ---- */

static int
nmea_rmc_mode_measured(const nmea_field* mode) {
    /* Before NMEA 2.3 there is no mode field: status A alone counts (a decided limit). A field that is present but
       empty says no more than an absent one, so it is read the same way (controller ruling). */
    if (mode->n == 0U) {
        return 1;
    }
    return nmea_field_is(mode, 'A') || nmea_field_is(mode, 'D') || nmea_field_is(mode, 'F') || nmea_field_is(mode, 'R')
           || nmea_field_is(mode, 'P');
}

static void
nmea_read_rmc_motion(const nmea_fields* fields, nmea_fix* out) {
    nmea_number num;
    if (nmea_parse_number(nmea_get(fields, 7U), 5U, 3U, &num)) {
        /* knots x 1000 -> km/h x 10: x 1.852 x 10 / 1000 */
        const uint64_t dkmh = (((uint64_t)num.whole * 1000U + num.frac) * 1852U + 50000U) / 100000U;
        if (dkmh < DSD_POSITION_SPEED_UNKNOWN) {
            out->speed_dkmh = (uint16_t)dkmh;
        }
    }
    if (nmea_parse_number(nmea_get(fields, 8U), 3U, 1U, &num)) {
        const uint32_t ddeg = (num.whole * 10U) + num.frac;
        if (ddeg <= DSD_POSITION_HEADING_MAX) {
            out->heading_ddeg = (uint16_t)ddeg;
        }
    }
}

/* $--RMC,time,status,lat,N/S,lon,E/W,speed kn,course,date,magvar,E/W[,mode[,nav status]] */
static void
nmea_read_rmc(const nmea_fields* fields, int64_t receive_utc_s, nmea_fix* out) {
    out->type = NMEA_TYPE_RMC;
    if (fields->count < 10U) {
        out->refusal = "RMC too short";
        return;
    }
    if (!nmea_field_is(nmea_get(fields, 2U), 'A')) {
        out->refusal = "RMC status not valid";
        return;
    }
    if (!nmea_rmc_mode_measured(nmea_get(fields, 12U))) {
        out->refusal = "RMC mode not a measured fix";
        return;
    }
    out->measured_fix = 1U;
    nmea_read_position(fields, 3U, out);
    if (!out->has_position) {
        out->refusal = "RMC position invalid";
        return;
    }
    nmea_read_rmc_motion(fields, out);
    const int64_t tod = nmea_parse_time_of_day(nmea_get(fields, 1U));
    int64_t days = 0;
    if (tod >= 0 && nmea_parse_date(nmea_get(fields, 9U), receive_utc_s, &days)) {
        out->fix_time_s = (days * 86400) + tod;
    }
}

static void
nmea_read_gga_extras(const nmea_fields* fields, nmea_fix* out) {
    uint32_t sats = 0U;
    if (nmea_parse_count(nmea_get(fields, 7U), 3U, &sats) && sats < DSD_POSITION_SATS_NONE) {
        out->sats = (uint8_t)sats;
    }
    nmea_number num;
    const nmea_field* alt = nmea_get(fields, 9U);
    if (!nmea_field_is(nmea_get(fields, 10U), 'M') || alt->n == 0U) {
        return;
    }
    const int negative = alt->p[0] == '-';
    const nmea_field magnitude = {alt->p + (negative ? 1 : 0), alt->n - (negative ? 1U : 0U)};
    if (nmea_parse_number(&magnitude, 6U, 1U, &num)) {
        const int32_t dm = (int32_t)((num.whole * 10U) + num.frac);
        out->alt_dm = negative ? -dm : dm;
    }
}

/* $--GGA,time,lat,N/S,lon,E/W,quality,sats,HDOP,alt,M,sep,M,age,station. HDOP is a ratio: never read as an error. */
static void
nmea_read_gga(const nmea_fields* fields, nmea_fix* out) {
    out->type = NMEA_TYPE_GGA;
    const nmea_field* quality = nmea_get(fields, 6U);
    if (quality->n != 1U || quality->p[0] < '1' || quality->p[0] > '5') {
        out->refusal = "GGA quality not a measured fix";
        return;
    }
    out->measured_fix = 1U;
    nmea_read_position(fields, 2U, out);
    if (!out->has_position) {
        out->refusal = "GGA position invalid";
        return;
    }
    nmea_read_gga_extras(fields, out);
}

/* The address: a two-letter talker (upper case) and the formatter. A 'P' first is a proprietary sentence ($P + a
 * manufacturer code, NMEA 0183), never an RMC or GGA whatever letters follow. */
static nmea_sentence_type
nmea_formatter(const nmea_field* address) {
    if (address->n != 5U || address->p[0] < 'A' || address->p[0] > 'Z' || address->p[0] == 'P' || address->p[1] < 'A'
        || address->p[1] > 'Z') {
        return NMEA_TYPE_OTHER;
    }
    const char* f = address->p + 2;
    if (f[0] == 'R' && f[1] == 'M' && f[2] == 'C') {
        return NMEA_TYPE_RMC;
    }
    if (f[0] == 'G' && f[1] == 'G' && f[2] == 'A') {
        return NMEA_TYPE_GGA;
    }
    return NMEA_TYPE_OTHER;
}

void
nmea_parse_fix(const char* sentence, size_t len, int64_t receive_utc_s, nmea_fix* out) {
    if (out == NULL) {
        return;
    }
    out->type = NMEA_TYPE_OTHER;
    out->measured_fix = 0U;
    out->has_position = 0U;
    out->sats = DSD_POSITION_SATS_NONE;
    out->lat_e7 = 0;
    out->lon_e7 = 0;
    out->alt_dm = DSD_POSITION_ALT_UNKNOWN;
    out->speed_dkmh = DSD_POSITION_SPEED_UNKNOWN;
    out->heading_ddeg = DSD_POSITION_HEADING_UNKNOWN;
    out->fix_time_s = 0;
    out->refusal = "not an RMC or GGA sentence";
    if (sentence == NULL || len < 2U || sentence[0] != '$') {
        return;
    }
    nmea_fields fields;
    nmea_split(sentence, len, &fields);
    switch (nmea_formatter(nmea_get(&fields, 0U))) {
        case NMEA_TYPE_RMC: nmea_read_rmc(&fields, receive_utc_s, out); break;
        case NMEA_TYPE_GGA: nmea_read_gga(&fields, out); break;
        default: return;
    }
    if (out->measured_fix && out->has_position) {
        out->refusal = "";
    }
}
