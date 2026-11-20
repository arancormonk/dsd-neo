// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * dsd_gps.c
 * GPS Handling Functions for Various Protocols
 *
 * LWVMOBILE
 * 2023-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/position_attribution.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/protocol/pdu.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/unicode.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nmea_sentence.h"

/* Enrich the slot's ACTIVE call's row with GPS text: with an admitted position report (@p fix, from
 * dsd_position_admit()) the text and the position go in together under the writer's typed-source rule
 * (dsd_event_enrich_position()); with @p fix NULL the text alone (dsd_event_enrich_gps()), on a call of @p protocol
 * (DSD_SYNC_*, the protocol whose signalling carried the report) only. A nonzero @p expected_source must be the call's
 * source. */
static int
gps_enrich_active_call(const dsd_opts* opts, dsd_state* state, uint8_t slot, int protocol, uint32_t expected_source,
                       const char* text, const dsd_position_fix* fix, dsd_call_snapshot* call_out) {
    dsd_call_snapshot call;
    const int has_active_call = dsd_call_state_get(state, slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
    if (has_active_call && expected_source != 0U && call.ota_source_id != expected_source) {
        return 0;
    }
    if (!has_active_call) {
        return 0;
    }
    if (call_out) {
        *call_out = call;
    }
    if (fix != NULL) {
        return dsd_event_enrich_position(opts, state, slot, call.epoch, fix, text);
    }
    return dsd_event_enrich_gps(state, slot, protocol, call.epoch, text);
}

static const uint32_t POSITION_ERROR_POW10[8] = {
    1U, 10U, 100U, 1000U, 10000U, 100000U, 1000000U, 10000000U,
};

// Location files feed mapping tools that cannot see a CRC verdict. A CRC-failed burst can carry a plausible
// but wrong fix (issue #550); mirror the pdu_crc_ok gate in dmr_lrrp_write_file_if_needed.
// Callers must run inside an event_crc_invalid save/set/restore scope indexed by the same slot they pass;
// outside such a scope the gate is inert.
static int
gps_lrrp_row_allowed(const dsd_opts* opts, const dsd_state* state, int slot) {
    if (opts == NULL || opts->lrrp_file_output != 1) {
        return 0;
    }
    return state == NULL
           || (state->event_crc_invalid[slot & 1] == 0 && dsd_receive_current_allows(opts, state, slot & 1));
}

// One location-file row, ungated: the caller decided the report may be written.
static void
gps_write_lrrp_compact_row(const dsd_opts* opts, uint32_t src, double latitude, double longitude, int speed_kph,
                           int azimuth) {
    char datestr[9];
    char timestr[7];
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_DATE_COMPACT, datestr, sizeof datestr);
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COMPACT, timestr, sizeof timestr);

    FILE* p_file = dsd_fopen_private(opts->lrrp_out_file, "a");
    if (p_file == NULL) {
        return;
    }

    DSD_FPRINTF(p_file, "%s\t", datestr);
    DSD_FPRINTF(p_file, "%s\t", timestr);
    DSD_FPRINTF(p_file, "%08u\t", src);
    DSD_FPRINTF(p_file, "%.6lf\t", latitude);
    DSD_FPRINTF(p_file, "%.6lf\t", longitude);
    DSD_FPRINTF(p_file, "%d\t ", speed_kph);
    DSD_FPRINTF(p_file, "%d\t ", azimuth);
    DSD_FPRINTF(p_file, "\n");
    fclose(p_file);
}

static void
gps_write_lrrp_compact(const dsd_opts* opts, const dsd_state* state, int slot, uint32_t src, double latitude,
                       double longitude, int speed_kph, int azimuth) {
    if (!gps_lrrp_row_allowed(opts, state, slot)) {
        return;
    }
    gps_write_lrrp_compact_row(opts, src, latitude, longitude, speed_kph, azimuth);
}

/* The location-file row of a report dsd_position_admit() admitted, from the admitted record: its source (0 when
 * admission cleared it), coordinates, and the speed and heading columns the caller derived (0 where unknown, the
 * file's placeholder). Admission already applied the CRC verdict and the receive policy; only -L itself is checked. */
static void
gps_write_lrrp_admitted(const dsd_opts* opts, const dsd_position_fix* fix, int speed_kph, int azimuth) {
    if (opts == NULL || fix == NULL || opts->lrrp_file_output != 1) {
        return;
    }
    gps_write_lrrp_compact_row(opts, fix->source_id, (double)fix->lat_e7 / 1e7, (double)fix->lon_e7 / 1e7, speed_kph,
                               azimuth);
}

static void DSD_ATTR_USED
gps_write_lrrp_slash_colon(const dsd_opts* opts, const dsd_state* state, int slot, uint32_t src, double latitude,
                           double longitude, int speed_kph, int azimuth) {
    if (!gps_lrrp_row_allowed(opts, state, slot)) {
        return;
    }

    char datestr[11];
    char timestr[9];
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_DATE_SLASH, datestr, sizeof datestr);
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);

    FILE* p_file = dsd_fopen_private(opts->lrrp_out_file, "a");
    if (p_file == NULL) {
        return;
    }

    DSD_FPRINTF(p_file, "%s\t", datestr);
    DSD_FPRINTF(p_file, "%s\t", timestr);
    DSD_FPRINTF(p_file, "%08u\t", src);
    DSD_FPRINTF(p_file, "%.6lf\t", latitude);
    DSD_FPRINTF(p_file, "%.6lf\t", longitude);
    DSD_FPRINTF(p_file, "%d\t ", speed_kph);
    DSD_FPRINTF(p_file, "%d\t ", azimuth);
    DSD_FPRINTF(p_file, "\n");
    fclose(p_file);
}

/* A two's-complement coordinate of @p bits bits whose full scale is @p span_e7 (degrees x 1e7 for 2^bits units), as
 * degrees x 1e7 rounded to the nearest unit. LIP and the DMR embedded GPS LC share it (TS 100 392-18-1 §6.3.30/6.3.50,
 * TS 102 361-2 §7.2.16/7.2.17): latitude 24 bits over 180 degrees, longitude 25 bits over 360 degrees. */
static int32_t
gps_twos_complement_e7(uint32_t raw, unsigned bits, int64_t span_e7) {
    const int64_t denom = (int64_t)1 << bits;
    int64_t value = (int64_t)(raw & (uint32_t)(denom - 1));
    if (value >= denom / 2) {
        value -= denom;
    }
    const int64_t num = value * span_e7;
    const int64_t scaled = num >= 0 ? (num + denom / 2) / denom : -((-num + denom / 2) / denom);
    return (int32_t)scaled;
}

#define GPS_LAT_SPAN_E7 1800000000LL
#define GPS_LON_SPAN_E7 3600000000LL

/* The position error code of LIP (TS 100 392-18-1 Table 6.93) and the DMR embedded GPS LC (TS 102 361-2 Table 7.24),
 * in metres: 0..5 are "less than" 2 x 10^n m (5: up to 200 km), 6 is "more than 200 km" (a lower bound) and 7 "not
 * known", which may mean the radio failed to get a fix (Table 6.1 Note 4, Table 7.24 NOTE): its position may be old or
 * invalid (decision D11). */
static uint32_t
gps_position_error_m(uint8_t code, uint16_t* flags) {
    if (code <= 5U) {
        return 2U * POSITION_ERROR_POW10[code];
    }
    if (code == 6U) {
        *flags |= (uint16_t)DSD_POS_FLAG_ERR_LOWER_BOUND;
        return 200000U;
    }
    *flags |= (uint16_t)DSD_POS_FLAG_VALIDITY_UNKNOWN;
    return DSD_POSITION_ERR_UNKNOWN;
}

/* LIP horizontal velocity (TS 100 392-18-1 §6.3.17, Table 6.51) in 0.1 km/h: 1 km/h steps to 28, then
 * v = 16 x 1.038^(K - 13); 126 is "more than 1 043 km/h" (a lower bound), 127 "not known". */
static uint16_t
lip_speed_dkmh(uint8_t code, uint16_t* flags) {
    if (code >= 127U) {
        return DSD_POSITION_SPEED_UNKNOWN;
    }
    if (code == 126U) {
        *flags |= (uint16_t)DSD_POS_FLAG_SPEED_LOWER_BOUND;
        return 10430U;
    }
    if (code <= 28U) {
        return (uint16_t)(code * 10U);
    }
    return (uint16_t)lround(160.0 * pow(1.038, (double)code - 13.0));
}

/* LIP direction of travel (Table 6.45) in 0.1 degree: code x 22.5 degrees; 15 is "337,5 or direction of travel is
 * not known". */
static uint16_t
lip_heading_ddeg(uint8_t code, uint16_t* flags) {
    if (code >= 15U) {
        *flags |= (uint16_t)DSD_POS_FLAG_HEADING_AMBIGUOUS;
        return 3375U;
    }
    return (uint16_t)(code * 225U);
}

static void
lip_print_time_elapsed(uint8_t time_elapsed) {
    switch (time_elapsed) {
        case 0: DSD_FPRINTF(stderr, " TE: < 5s;"); break;
        case 1: DSD_FPRINTF(stderr, " TE: < 5m;"); break;
        case 2: DSD_FPRINTF(stderr, " TE: < 30m;"); break;
        default: DSD_FPRINTF(stderr, " TE: NA or UNK;"); break;
    }
}

/* The raw fields of a LIP location report: the short report (TS 100 392-18-1 Table 6.1) or the DMR USBD Poll Response
 * (TS 102 361-4 Table 6.79), which carries the same fields with a 3-bit reason. */
typedef struct {
    uint8_t time_elapsed;
    uint32_t lon; /* 25-bit two's complement */
    uint32_t lat; /* 24-bit two's complement */
    uint8_t pos_err;
    uint8_t hor_vel;
    uint8_t dir_tra;
    uint8_t reason;
    uint8_t reason_is_user_data;
} lip_fields;

/* TS 100 392-18-1 Table 6.94: "Emergency condition is detected". */
enum { LIP_REASON_EMERGENCY = 2U };

static void
lip_fields_from_usbd(const uint8_t* input, lip_fields* f) {
    f->time_elapsed = (uint8_t)convert_bits_into_output(input + 4, 2);
    f->lon = (uint32_t)convert_bits_into_output(input + 6, 25);
    f->lat = (uint32_t)convert_bits_into_output(input + 31, 24);
    f->pos_err = (uint8_t)convert_bits_into_output(input + 55, 3);
    f->hor_vel = (uint8_t)convert_bits_into_output(input + 58, 7);
    f->dir_tra = (uint8_t)convert_bits_into_output(input + 65, 4);
    f->reason = (uint8_t)convert_bits_into_output(input + 69, 3);
    f->reason_is_user_data = 0;
}

static void
lip_fields_from_short_report(const uint8_t* bits, lip_fields* f) {
    // TS 100 392-18-1 Table 6.1: fixed 76-bit short report, with no optional-element bits.
    f->time_elapsed = (uint8_t)convert_bits_into_output(bits + 2, 2);
    f->lon = (uint32_t)convert_bits_into_output(bits + 4, 25);
    f->lat = (uint32_t)convert_bits_into_output(bits + 29, 24);
    f->pos_err = (uint8_t)convert_bits_into_output(bits + 53, 3);
    f->hor_vel = (uint8_t)convert_bits_into_output(bits + 56, 7);
    f->dir_tra = (uint8_t)convert_bits_into_output(bits + 63, 4);
    f->reason_is_user_data = bits[67];
    f->reason = (uint8_t)convert_bits_into_output(bits + 68, 8);
}

static void
lip_fields_to_report(const lip_fields* f, lip_short_report* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    out->lat_e7 = gps_twos_complement_e7(f->lat, 24U, GPS_LAT_SPAN_E7);
    out->lon_e7 = gps_twos_complement_e7(f->lon, 25U, GPS_LON_SPAN_E7);
    out->err_m = gps_position_error_m(f->pos_err, &out->flags);
    out->speed_dkmh = lip_speed_dkmh(f->hor_vel, &out->flags);
    out->heading_ddeg = lip_heading_ddeg(f->dir_tra, &out->flags);
    out->time_elapsed = (uint8_t)(f->time_elapsed & 3U);
    out->position_error = (uint8_t)(f->pos_err & 7U);
    out->velocity_code = (uint8_t)(f->hor_vel & 0x7FU);
    out->direction_code = (uint8_t)(f->dir_tra & 0x0FU);
    out->reason = f->reason;
    out->reason_is_user_data = f->reason_is_user_data;
}

int
lip_parse_short_report(const uint8_t* bits, size_t bit_count, lip_short_report* out) {
    if (bits == NULL || out == NULL) {
        return -1;
    }
    if (bit_count < LIP_SHORT_REPORT_BITS || convert_bits_into_output(bits, 2) != 0U) {
        return 0;
    }
    lip_fields f;
    lip_fields_from_short_report(bits, &f);
    lip_fields_to_report(&f, out);
    if (!f.reason_is_user_data && f.reason == LIP_REASON_EMERGENCY) {
        /* The short report's own reason for sending (Table 6.94). A DMR USBD reply's 3-bit reason is another table
           (TS 102 361-4 Table 6.80), where no code means an emergency. */
        out->flags = (uint16_t)(out->flags | DSD_POS_FLAG_EMERGENCY);
    }
    return 1;
}

void
lip_short_report_to_fix(const lip_short_report* report, dsd_position_fix* fix) {
    if (report == NULL || fix == NULL) {
        return;
    }
    fix->lat_e7 = report->lat_e7;
    fix->lon_e7 = report->lon_e7;
    fix->err_m = report->err_m;
    fix->speed_dkmh = report->speed_dkmh;
    fix->heading_ddeg = report->heading_ddeg;
    fix->age_class = report->time_elapsed;
    fix->flags = (uint16_t)(fix->flags | report->flags);
}

static void
gps_print_position_error(uint8_t pos_err, unsigned int metres) {
    if (pos_err == 7U) {
        DSD_FPRINTF(stderr, "\n  Position Error: Unknown or Invalid");
    } else if (pos_err == 6U) {
        DSD_FPRINTF(stderr, "\n  Position Error: More than 200km");
    } else if (pos_err == 5U) {
        DSD_FPRINTF(stderr, "\n  Position Error: Less than or equal to 200000m");
    } else {
        DSD_FPRINTF(stderr, "\n  Position Error: Less than %dm", metres);
    }
}

static const char*
lip_reason_label(uint8_t lip_value) {
    // TS 100 392-18-1 Table 6.94; every unlisted value is reserved.
    static const char* const labels[131] = {
        [0] = "Subscriber unit is powered ON",
        [1] = "Subscriber unit is powered OFF",
        [2] = "Emergency condition is detected",
        [3] = "Push-to-talk condition is detected",
        [4] = "Status",
        [5] = "Transmit inhibit mode ON",
        [6] = "Transmit inhibit mode OFF",
        [7] = "System access (TMO ON)",
        [8] = "DMO ON",
        [9] = "Enter service (after being out of service)",
        [10] = "Service loss",
        [11] = "Cell reselection or change of serving cell",
        [12] = "Low battery",
        [13] = "Subscriber unit is connected to a car kit",
        [14] = "Subscriber unit is disconnected from a car kit",
        [15] = "Subscriber unit asks for transfer initialization configuration",
        [16] = "Arrival at destination",
        [17] = "Arrival at a defined location",
        [18] = "Approaching a defined location",
        [19] = "SDS type-1 entered",
        [20] = "User application initiated",
        [21] = "Lost ability to determine location",
        [22] = "Regained ability to determine location",
        [23] = "Leaving point",
        [24] = "Ambience Listening call is detected",
        [25] = "Start of temporary reporting",
        [26] = "Return to normal reporting",
        [27] = "Call setup type 1 detected",
        [28] = "Call setup type 2 detected",
        [29] = "Positioning device in MS ON",
        [30] = "Positioning device in MS OFF",
        [32] = "Response to an immediate location request",
        [129] = "Maximum reporting interval exceeded since the last location information report",
        [130] = "Maximum reporting distance limit travelled since last location information report",
    };
    return lip_value < sizeof labels / sizeof labels[0] && labels[lip_value] != NULL ? labels[lip_value] : "Reserved";
}

static void
lip_print_reason(const lip_short_report* r, int hash_prefix) {
    if (hash_prefix >= 0) {
        // TS 102 361-4 Table 6.80 compresses LIP reason 32 to zero; other codes are reserved.
        DSD_FPRINTF(stderr, " Reason: %s (%u);", r->reason == 0 ? lip_reason_label(32) : "Reserved", r->reason);
    } else if (r->reason_is_user_data) {
        DSD_FPRINTF(stderr, " User data: 0x%02X;", r->reason);
    } else {
        DSD_FPRINTF(stderr, " Reason: %s (%u);", lip_reason_label(r->reason), r->reason);
    }
}

/* A known speed in 0.1 km/h as whole km/h, rounded (Table 6.51 gives code 125 as 1 043 km/h). */
static int
gps_speed_kmh(uint16_t speed_dkmh) {
    return (int)((speed_dkmh + 5U) / 10U);
}

/* "Spd: ..." and "Dir: ..." of a LIP report, the unknown and bounded codes said as such (Tables 6.51, 6.45); a
 * half-degree heading keeps its decimal. */
static void
lip_motion_text(const lip_short_report* r, const char* deg_glyph, char* out, size_t cap) {
    char speed[24];
    char heading[16];
    if (r->speed_dkmh == DSD_POSITION_SPEED_UNKNOWN) {
        DSD_SNPRINTF(speed, sizeof speed, "%s", "unknown");
    } else {
        DSD_SNPRINTF(speed, sizeof speed, "%s%d km/h", (r->flags & DSD_POS_FLAG_SPEED_LOWER_BOUND) != 0U ? ">" : "",
                     gps_speed_kmh(r->speed_dkmh));
    }
    if (r->heading_ddeg % 10U != 0U) {
        DSD_SNPRINTF(heading, sizeof heading, "%u.%u", (unsigned)(r->heading_ddeg / 10U),
                     (unsigned)(r->heading_ddeg % 10U));
    } else {
        DSD_SNPRINTF(heading, sizeof heading, "%u", (unsigned)(r->heading_ddeg / 10U));
    }
    DSD_SNPRINTF(out, cap, "Spd: %s; Dir: %s%s%s", speed, heading, deg_glyph,
                 (r->flags & DSD_POS_FLAG_HEADING_AMBIGUOUS) != 0U ? " or unknown" : "");
}

/* "Err: ..." of a position error code (Tables 6.93 / 7.24); code 7 says the fix may be old or invalid (D11). */
static void
gps_error_text(uint8_t code, uint32_t err_m, char* out, size_t cap) {
    if (code <= 5U) {
        DSD_SNPRINTF(out, cap, "Err: %um", (unsigned)err_m);
    } else if (code == 6U) {
        DSD_SNPRINTF(out, cap, "%s", "Err: >200km");
    } else {
        DSD_SNPRINTF(out, cap, "%s", "Unknown Pos Err (fix may be old or invalid)");
    }
}

/* A coordinate pair as the decoders print it: absolute degrees with hemisphere letters. */
typedef struct {
    double lat;
    double lon;
    const char* latstr;
    const char* lonstr;
} gps_hemisphere_pair;

static gps_hemisphere_pair
gps_hemisphere_pair_from_e7(int32_t lat_e7, int32_t lon_e7) {
    gps_hemisphere_pair p;
    p.lat = fabs((double)lat_e7 / 1e7);
    p.lon = fabs((double)lon_e7 / 1e7);
    p.latstr = lat_e7 < 0 ? "S" : "N";
    p.lonstr = lon_e7 < 0 ? "W" : "E";
    return p;
}

/* Who a LIP report names: a USBD reply's 8-bit hash (hash >= 0, never a radio ID) or the carrier's source. */
typedef struct {
    int hash;
    uint32_t src;
    uint32_t target;      /* the carrier's destination, 0 = none */
    uint8_t target_group; /* @c target is a talkgroup */
} lip_source;

/* Print a LIP report. @p position_note NULL prints its coordinates (an admitted report); otherwise the coordinates are
 * left out and the note says why ("suppressed; CRC ERR", "refused"). @p content 0 (a failed report without -Z,
 * dsd_gps_report_content_printable()) leaves out the rest of the report too: its motion, error, reason and time. */
static void
lip_print_report(const lip_short_report* r, const lip_source* who, const char* position_note, int content) {
    const char* deg_glyph = dsd_degrees_glyph();
    if (who->hash >= 0) {
        DSD_FPRINTF(stderr, "Src(Hash); %03d;  ", who->hash);
    } else {
        DSD_FPRINTF(stderr, "Src: %u; ", who->src);
    }
    if (position_note == NULL) {
        const gps_hemisphere_pair p = gps_hemisphere_pair_from_e7(r->lat_e7, r->lon_e7);
        DSD_FPRINTF(stderr, "Lat: %.5lf%s%s Lon: %.5lf%s%s (%.5lf, %.5lf); ", p.lat, deg_glyph, p.latstr, p.lon,
                    deg_glyph, p.lonstr, (double)r->lat_e7 / 1e7, (double)r->lon_e7 / 1e7);
    } else {
        DSD_FPRINTF(stderr, "Position: (%s); ", position_note);
    }
    if (!content) {
        return;
    }
    char motion[80];
    lip_motion_text(r, deg_glyph, motion, sizeof motion);
    DSD_FPRINTF(stderr, "%s", motion);
    gps_print_position_error(r->position_error, (unsigned int)r->err_m);
    DSD_FPRINTF(stderr, ";");
    lip_print_reason(r, who->hash);
    lip_print_time_elapsed(r->time_elapsed);
}

void
lip_short_report_text(const lip_short_report* report, char* out, size_t cap) {
    if (out == NULL || cap == 0U) {
        return;
    }
    out[0] = '\0';
    if (report == NULL) {
        return;
    }
    const char* deg_glyph = dsd_degrees_glyph();
    const gps_hemisphere_pair p = gps_hemisphere_pair_from_e7(report->lat_e7, report->lon_e7);
    char err[64];
    char motion[80];
    gps_error_text(report->position_error, report->err_m, err, sizeof err);
    lip_motion_text(report, deg_glyph, motion, sizeof motion);
    DSD_SNPRINTF(out, cap, "LIP: %.5lf%s%s %.5lf%s%s; %s; %s", p.lat, deg_glyph, p.latstr, p.lon, deg_glyph, p.lonstr,
                 err, motion);
}

/* The row's and the display's text of an admitted LIP report, into dsd_state::dmr_embedded_gps[slot]. */
static void
lip_store_text(dsd_state* state, int slot, const lip_short_report* r, int hash) {
    char text[192]; /* the longest text is about 120 bytes */
    lip_short_report_text(r, text, sizeof text);
    if (hash >= 0) {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "%03d; %s", hash, text);
    } else {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "%s", text);
    }
}

/* Why a report of the slot's scope is unchecked, for the console: a failed check, or a RAS acceptance. */
static const char*
gps_suppressed_label(uint8_t crc_flags) {
    return dsd_crc_is_failed(crc_flags) ? "suppressed; CRC ERR" : "suppressed; RAS";
}

/* The reception the data packet carrying a report began on, captured with its header: the DMR data header's
 * (dmr_dheader()) for a DMR packet, the P25 Phase 1 MDPU header's (processMPDU()) for a P25 one (decode_ip_pdu() takes
 * both); NULL when none was captured, which takes the current reception. dmr_pdu.c chooses its LRRP and LOCN origin
 * the same way (dmr_pdu_position_origin()). */
static const dsd_position_origin*
gps_packet_origin(const dsd_state* state, int slot) {
    const dsd_position_origin* origin = NULL;
    if (DSD_SYNC_IS_DMR(state->lastsynctype)) {
        origin = &state->dmr_data_origin[slot & 1];
    } else if (DSD_SYNC_IS_P25P1(state->lastsynctype)) {
        origin = &state->p25_mdpu_origin;
    }
    return (origin != NULL && origin->valid != 0U) ? origin : NULL;
}

/* The destination a checked carrier named, with its group qualifier (a group flag needs a destination). */
static void
gps_fill_target(dsd_position_fix* fix, uint32_t target, uint8_t target_group) {
    fix->target_id = target;
    if (target != 0U && target_group != 0U) {
        fix->flags = (uint16_t)(fix->flags | DSD_POS_FLAG_TARGET_GROUP);
    }
}

/* The destination the checked data header of @p slot named (dmr_lrrp_target), with that header's own group qualifier
 * (dmr_data_checked_group: the DMR data header's G/I bit, an NXDN data call's call type; a P25 LLID is a radio). */
static void
gps_fill_header_target(const dsd_state* state, uint8_t slot, dsd_position_fix* fix) {
    const unsigned long long target = state->dmr_lrrp_target[slot & 1U];
    gps_fill_target(fix, target <= UINT32_MAX ? (uint32_t)target : 0U, state->dmr_data_checked_group[slot & 1U]);
}

/* Admit a checked LIP report: the record, its kind and source (a USBD reply's hash is never an identity), and the
 * reception it came from (the origin of the DMR or P25 data packet that carried it, gps_packet_origin()). */
static int
lip_admit(const dsd_opts* opts, dsd_state* state, const lip_short_report* r, const lip_source* who,
          dsd_position_fix* fix) {
    const int slot = state->currentslot & 1;
    dsd_position_fix_init(fix);
    lip_short_report_to_fix(r, fix);
    fix->slot = (uint8_t)slot;
    const dsd_position_origin* origin = NULL;
    int source_verified = 0;
    if (who->hash >= 0) {
        fix->kind = DSD_POS_LIP_USBD;
        fix->protocol = (uint8_t)DSD_SYNC_DMR_MS_DATA; // only an inbound (MS-sourced) burst is a Poll Response
        fix->flags = (uint16_t)(fix->flags | DSD_POS_FLAG_SOURCE_HASHED);
    } else {
        fix->kind = DSD_POS_LIP;
        fix->protocol = (uint8_t)state->lastsynctype;
        fix->source_id = who->src;
        gps_fill_target(fix, who->target, who->target_group);
        source_verified = 1; // the same checked packet named the source and the destination
        origin = gps_packet_origin(state, slot);
    }
    return dsd_position_admit(opts, state, fix, 1, source_verified, origin) != DSD_POSITION_REFUSED;
}

/* Print a LIP report and publish it when its carrier's checks passed (the slot's event_crc_invalid scope, set by the
 * caller) and admission admitted it: stage it with its text for the slot's next data notice (the carrier's own: a UDT,
 * a UDP datagram or a USBD reply), and write the location file unless the source is a hash (decision D8). A report
 * that is unchecked or refused prints no coordinates, leaves no text and stages nothing (spec 2.11). */
static void
lip_report(const dsd_opts* opts, dsd_state* state, const lip_short_report* r, const lip_source* who) {
    const int slot = state->currentslot & 1;
    const uint8_t crc_flags = state->event_crc_invalid[slot];
    DSD_FPRINTF(stderr, "Location Information Protocol; ");
    if (who->hash < 0) {
        DSD_FPRINTF(stderr, "Short Location Report; ");
    }
    dsd_position_fix fix;
    const int admitted = crc_flags == 0U && lip_admit(opts, state, r, who, &fix);
    lip_print_report(r, who, crc_flags != 0U ? gps_suppressed_label(crc_flags) : (admitted ? NULL : "refused"),
                     dsd_gps_report_content_printable(crc_flags == 0U, opts->payload));
    if (!admitted) {
        return;
    }
    lip_store_text(state, slot, r, who->hash);
    (void)dsd_event_stage_position(state, (uint8_t)slot, &fix, state->dmr_embedded_gps[slot]);
    if (who->hash < 0) {
        lip_write_location_row(opts, &fix);
    }
}

void
lip_write_location_row(const dsd_opts* opts, const dsd_position_fix* fix) {
    if (fix == NULL) {
        return;
    }
    const int speed = fix->speed_dkmh == DSD_POSITION_SPEED_UNKNOWN ? 0 : gps_speed_kmh(fix->speed_dkmh);
    const int azimuth = (fix->flags & DSD_POS_FLAG_HEADING_AMBIGUOUS) != 0U ? 0 : (int)(fix->heading_ddeg / 10U);
    gps_write_lrrp_admitted(opts, fix, speed, azimuth);
}

void
lip_protocol_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* input) {
    // ETSI TS 102 361-4 V1.12.1 Table 6.79: service 0..3, time 4..5, longitude 6..30,
    // latitude 31..54, position error 55..57, velocity 58..64, direction 65..68,
    // reason 69..71 and source hash 72..79. The 16-bit burst CRC follows at bit 80.
    lip_fields f;
    lip_fields_from_usbd(input, &f);
    lip_short_report r;
    lip_fields_to_report(&f, &r);
    const lip_source who = {(int)convert_bits_into_output(input + 72, 8), 0U, 0U, 0U};
    lip_report(opts, state, &r, &who);
}

static const char*
lip_extension_label(uint8_t extension) {
    // TS 100 392-18-1 Table 6.92.
    static const char* const labels[16] = {
        "Reserved",
        "Immediate location report request",
        "Reserved",
        "Long location report",
        "Location report acknowledgement",
        "Basic location parameters request/response",
        "Add/modify trigger request/response",
        "Remove trigger request/response",
        "Report trigger request/response",
        "Report basic location parameters request/response",
        "Location reporting enable/disable request/response",
        "Location reporting temporary control request/response",
        "Backlog request/response",
        "Reserved",
        "Reserved",
        "Reserved",
    };
    return labels[extension & 15U];
}

void
lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count,
                const dsd_gps_address* addr) {
    if (bit_count < 2) {
        DSD_FPRINTF(stderr, "LIP: truncated;");
        return;
    }
    uint8_t type = (uint8_t)convert_bits_into_output(bits, 2);
    if (type == 0) {
        lip_short_report r;
        if (lip_parse_short_report(bits, bit_count, &r) != 1) {
            DSD_FPRINTF(stderr, "LIP: short location report truncated (%zu bits);", bit_count);
            return;
        }
        const lip_source who = {-1, addr ? addr->source_id : 0U, addr ? addr->target_id : 0U,
                                addr ? addr->target_group : 0U};
        lip_report(opts, state, &r, &who);
    } else if (type == 1) {
        if (bit_count < 6) {
            DSD_FPRINTF(stderr, "LIP: PDU type extension truncated (%zu bits);", bit_count);
            return;
        }
        uint8_t extension = (uint8_t)convert_bits_into_output(bits + 2, 4);
        DSD_FPRINTF(stderr, "LIP: %s (PDU type extension %u); not decoded;", lip_extension_label(extension), extension);
    } else {
        DSD_FPRINTF(stderr, "LIP: reserved PDU type %u; not decoded;", type);
    }
}

/* The fields of a binary UDT NMEA report (TS 102 361-4 Annex B.3.6, Figures B.22-B.24, Table B.7). */
typedef struct {
    uint8_t ns;     /* 1 = North, 0 = South */
    uint8_t ew;     /* 1 = East, 0 = West */
    uint8_t q;      /* 1 = fix valid, 0 = no fix */
    uint8_t speed;  /* knots 0..126; 127 = more than 126 knots */
    uint8_t ndeg;   /* 0..89 */
    uint8_t nmin;   /* 0..59 */
    uint16_t nminf; /* ten-thousandths of a minute, 0..9999 */
    uint8_t edeg;   /* 0..179 */
    uint8_t emin;
    uint16_t eminf;
    uint8_t utc_hh;  /* 0..23 */
    uint8_t utc_mm;  /* 0..59 */
    uint8_t utc_ss3; /* short format: seconds / 10, 0..5 */
    uint8_t utc_ss6; /* long format: seconds, 0..59 */
    uint16_t cog;    /* long format: course over ground, degrees 0..359 */
} udt_nmea_fields;

/* The fields of @p type's format only: a short report (NMEA_UDT_SHORT_BITS) ends with its seconds code, so the long
 * format's seconds and course are not read from it (they would lie past its user data). */
static void
udt_nmea_read(const uint8_t* input, int type, udt_nmea_fields* f) {
    // uint8_t nmea_c = input[0];  //encrypted -- checked before we get here
    DSD_MEMSET(f, 0, sizeof(*f));
    f->ns = input[1];
    f->ew = input[2];
    f->q = input[3];
    f->speed = (uint8_t)convert_bits_into_output(&input[4], 7);
    f->ndeg = (uint8_t)convert_bits_into_output(&input[11], 7);
    f->nmin = (uint8_t)convert_bits_into_output(&input[18], 6);
    f->nminf = (uint16_t)convert_bits_into_output(&input[24], 14);
    f->edeg = (uint8_t)convert_bits_into_output(&input[38], 8);
    f->emin = (uint8_t)convert_bits_into_output(&input[46], 6);
    f->eminf = (uint16_t)convert_bits_into_output(&input[52], 14);
    f->utc_hh = (uint8_t)convert_bits_into_output(&input[66], 5);
    f->utc_mm = (uint8_t)convert_bits_into_output(&input[71], 6);
    if (type == 1) {
        f->utc_ss3 = (uint8_t)convert_bits_into_output(&input[77], 3);
        return;
    }
    f->utc_ss6 = (uint8_t)convert_bits_into_output(&input[77], 6);
    f->cog = (uint16_t)convert_bits_into_output(&input[103], 9);
}

/* Table B.7 ranges of the position fields; anything else is no position at all (never normalised). */
static int
udt_nmea_position_in_range(const udt_nmea_fields* f) {
    return f->ndeg <= 89U && f->nmin <= 59U && f->nminf <= 9999U && f->edeg <= 179U && f->emin <= 59U
           && f->eminf <= 9999U;
}

/* Table B.7 ranges of the time: hours 0..23, minutes 0..59, short seconds code 0..5 (x 10 s), long seconds 0..59. */
static int
udt_nmea_time_valid(const udt_nmea_fields* f, int type) {
    const int seconds_ok = type == 1 ? f->utc_ss3 <= 5U : f->utc_ss6 <= 59U;
    return f->utc_hh <= 23U && f->utc_mm <= 59U && seconds_ok;
}

/* Degrees, minutes and ten-thousandths of a minute as degrees x 1e7, rounded: 1/600000 degree per fraction count. */
static int32_t
udt_nmea_e7(uint8_t deg, uint8_t min, uint16_t frac, uint8_t positive) {
    const int64_t minutes_e4 = (int64_t)min * 10000 + (int64_t)frac;
    const int64_t e7 = (int64_t)deg * 10000000 + (minutes_e4 * 100 + 3) / 6;
    return (int32_t)(positive ? e7 : -e7);
}

static void
udt_nmea_print_time(const udt_nmea_fields* f, int type, const char* deg_glyph) {
    const int time_ok = udt_nmea_time_valid(f, type);
    char utc[24];
    if (time_ok) {
        DSD_SNPRINTF(utc, sizeof utc, "%02u:%02u:%02u UTC", f->utc_hh, f->utc_mm,
                     type == 1 ? (unsigned)f->utc_ss3 * 10U : (unsigned)f->utc_ss6);
    } else {
        DSD_SNPRINTF(utc, sizeof utc, "%s", "time unknown");
    }
    if (type == 1) {
        DSD_FPRINTF(stderr, " FIX: %d; %s; Short Format;", f->q, utc);
    } else if (type == 2) {
        if (f->cog <= 359U) {
            DSD_FPRINTF(stderr, " FIX: %d; %s; COG: %d%s; Long Format;", f->q, utc, f->cog, deg_glyph);
        } else {
            DSD_FPRINTF(stderr, " FIX: %d; %s; COG: unknown; Long Format;", f->q, utc);
        }
    }
}

/* The report as a feed record (before admission): Q set and every position field in range, checked by the caller. */
static void
udt_nmea_fill_fix(const dsd_state* state, const udt_nmea_fields* f, int type, const dsd_gps_address* addr, int slot,
                  dsd_position_fix* fix) {
    dsd_position_fix_init(fix);
    fix->kind = DSD_POS_NMEA;
    fix->protocol = (uint8_t)state->lastsynctype;
    fix->slot = (uint8_t)slot;
    if (addr != NULL) {
        // The UDT header names the source and the destination under the same CRC.
        fix->source_id = addr->source_id;
        gps_fill_target(fix, addr->target_id, addr->target_group);
    }
    fix->lat_e7 = udt_nmea_e7(f->ndeg, f->nmin, f->nminf, f->ns);
    fix->lon_e7 = udt_nmea_e7(f->edeg, f->emin, f->eminf, f->ew);
    const uint8_t knots = f->speed >= 127U ? 126U : f->speed;
    fix->speed_dkmh = (uint16_t)(((uint32_t)knots * 1852U + 50U) / 100U);
    if (f->speed >= 127U) {
        fix->flags = (uint16_t)(fix->flags | DSD_POS_FLAG_SPEED_LOWER_BOUND);
    }
    if (type == 2 && f->cog <= 359U) {
        fix->heading_ddeg = (uint16_t)(f->cog * 10U);
    }
}

/* Admit a checked, in-range UDT NMEA report (the UDT's addressing, from the same checked header) against the reception
 * its UDT began on. */
static int
udt_nmea_admit(const dsd_opts* opts, dsd_state* state, int slot, dsd_position_fix* fix) {
    return dsd_position_admit(opts, state, fix, 1, 1, gps_packet_origin(state, slot)) != DSD_POSITION_REFUSED;
}

/* The console's position of a UDT NMEA report: coordinates only for an admitted one. */
static void
udt_nmea_print_position(uint8_t crc_flags, const udt_nmea_fields* f, int in_range, int admitted,
                        const dsd_position_fix* fix, const char* deg_glyph) {
    if (crc_flags != 0U) {
        DSD_FPRINTF(stderr, " Position: (%s);", gps_suppressed_label(crc_flags));
    } else if (!f->q) {
        DSD_FPRINTF(stderr, " GPS: no fix;");
    } else if (!in_range) {
        DSD_FPRINTF(stderr, " GPS: invalid position;");
    } else if (!admitted) {
        DSD_FPRINTF(stderr, " GPS: (refused);");
    } else {
        DSD_FPRINTF(stderr, " GPS: %f%s, %f%s;", (double)fix->lat_e7 / 1e7, deg_glyph, (double)fix->lon_e7 / 1e7,
                    deg_glyph);
    }
}

void
nmea_iec_61162_1(const dsd_opts* opts, dsd_state* state, const uint8_t* input, size_t bit_count,
                 const dsd_gps_address* addr, int type) {
    const int slot = state->currentslot & 1;
    const size_t needed = type == 1 ? (size_t)NMEA_UDT_SHORT_BITS : (size_t)NMEA_UDT_LONG_BITS;
    if (bit_count < needed) {
        /* The pad nibbles are not user data: a field they would hold was never sent. */
        DSD_FPRINTF(stderr, " truncated (%zu of %zu bits);", bit_count, needed);
        return;
    }

    //NOTE: The Only difference between Short (type == 1) and Long Format (type == 2)
    //is the utc_ss3 on short vs utc_ss6 and inclusion of COG value on long

    //NOTE: MFID Specific Formats are not handled here, unknown, could be added if worked out
    //they could share most of the same elements and use the large spare bits in block 2 for extra
    // uint8_t mfid = (uint8_t)convert_bits_into_output(&input[88], 8); //on type 3, last octet of first block carries MFID

    udt_nmea_fields f;
    udt_nmea_read(input, type, &f);
    const char* deg_glyph = dsd_degrees_glyph();
    const uint8_t crc_flags = state->event_crc_invalid[slot];
    const int in_range = udt_nmea_position_in_range(&f);

    // Q = 0 is "No fix" and the Table B.7 ranges bound every field: neither is a position (spec 2.5).
    dsd_position_fix fix;
    udt_nmea_fill_fix(state, &f, type, addr, slot, &fix);
    const int admitted = crc_flags == 0U && f.q && in_range && udt_nmea_admit(opts, state, slot, &fix);
    udt_nmea_print_position(crc_flags, &f, in_range, admitted, &fix, deg_glyph);

    // A failed report's other fields print only with -Z (dsd_gps_report_content_printable()).
    if (dsd_gps_report_content_printable(crc_flags == 0U, opts->payload)) {
        //Speed in Knots
        if (f.speed > 126) {
            DSD_FPRINTF(stderr, " SPD > 126 knots;");
        } else {
            DSD_FPRINTF(stderr, " SPD: %d knots; %f kph;", f.speed, (double)f.speed * 1.852);
        }
        udt_nmea_print_time(&f, type, deg_glyph);
    }
    if (!admitted) {
        return;
    }
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "GPS: (%f%s, %f%s)",
                 (double)fix.lat_e7 / 1e7, deg_glyph, (double)fix.lon_e7 / 1e7, deg_glyph);
    (void)dsd_event_stage_position(state, (uint8_t)slot, &fix, state->dmr_embedded_gps[slot]);
    const int azimuth = fix.heading_ddeg == DSD_POSITION_HEADING_UNKNOWN ? 0 : (int)(fix.heading_ddeg / 10U);
    gps_write_lrrp_admitted(opts, &fix, gps_speed_kmh(fix.speed_dkmh), azimuth);
}

/* The provenance of a report decoded without a caller's context: nothing checked, so nothing is admitted. */
static const dsd_gps_report_ctx GPS_REPORT_CTX_NONE = {0};

/* Print the report's source as the context vouches for it: a Phase 2 report names its voice channel first. */
static void
gps_report_print_prefix(const dsd_gps_report_ctx* ctx) {
    DSD_FPRINTF(stderr, "\n");
    if (DSD_SYNC_IS_P25P2(ctx->protocol)) {
        DSD_FPRINTF(stderr, " VCH: %u -", (unsigned int)ctx->slot);
    }
    if (ctx->source_verified != 1U) {
        DSD_FPRINTF(stderr, " SRC: unverified;");
    } else if (ctx->source_id == 0U) {
        DSD_FPRINTF(stderr, " SRC: none;");
    } else {
        DSD_FPRINTF(stderr, " SRC: %08u;", ctx->source_id);
    }
}

/* Write an admitted report into its call's history row: the call the terminator just ended (spec N2), the call of the
 * epoch the report was bound to, or else the slot's ACTIVE call. The writer's typed-source rule decides. A report shown
 * to belong to no call (ctx->no_call_row: a TDULC that ended none) writes no row: the call still open is not its. */
static void
gps_report_write_history(const dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_gps_report_ctx* ctx,
                         const dsd_position_fix* fix, const char* text) {
    if (ctx->no_call_row) {
        return;
    }
    if (ctx->closing != NULL && ctx->closing->valid) {
        (void)dsd_event_enrich_position_closing(opts, state, ctx->closing, fix, text);
    } else if (ctx->call_epoch != 0U) {
        (void)dsd_event_enrich_position(opts, state, slot, ctx->call_epoch, fix, text);
    } else {
        (void)gps_enrich_active_call(opts, state, slot, ctx->protocol, 0U, text, fix, NULL);
    }
}

/* Fill the provenance fields every P25 link-control/MAC report shares: the call's identity, which admission clears
 * unless the context verified it. */
static void
gps_report_fill_identity(const dsd_gps_report_ctx* ctx, dsd_position_fix* fix) {
    fix->protocol = (uint8_t)ctx->protocol;
    fix->slot = ctx->slot >= 1U ? 1U : 0U;
    fix->source_id = ctx->source_id;
    fix->target_id = ctx->target_id;
    if (ctx->target_group == 1U && ctx->target_id != 0U) {
        fix->flags |= DSD_POS_FLAG_TARGET_GROUP;
    }
}

/* L3Harris GPS update (TIA-102.AABF-D-1 §7.3.35-36). Body octet n is Part A octet n + 2 for n < 7, else Part B octet
 * n - 5. */
typedef struct {
    uint32_t lat_frac; /* ten-thousandths of a minute */
    uint32_t lon_frac;
    uint8_t lat_south;
    uint8_t lon_west;
    uint8_t lat_min;
    uint8_t lon_min;
    uint8_t lat_deg;
    uint8_t lon_deg;
    uint32_t time_s;       /* 17 bits: seconds since midnight; HARRIS_GPS_TIME_UNKNOWN */
    uint16_t heading_ddeg; /* 12 bits, tenths of a degree */
    uint16_t velocity_dkn; /* 12 bits, tenths of a knot */
    uint8_t emergency;
    uint8_t quality; /* %00 no fix, %01 fix, %10 differential fix, %11 GPS not enabled */
    uint8_t sats;    /* satellites in view */
} harris_gps_fields;

#define HARRIS_GPS_TIME_UNKNOWN 0x1FFFFU
#define GPS_SECONDS_PER_DAY     86400

static void
harris_gps_parse(const uint8_t* body, harris_gps_fields* f) {
    f->lat_frac = ((uint32_t)body[0] << 8) | body[1];
    f->lat_south = (uint8_t)(body[2] >> 7);
    f->lat_min = (uint8_t)(body[2] & 0x7FU);
    f->lat_deg = body[3];
    f->lon_frac = ((uint32_t)body[4] << 8) | body[5];
    f->lon_west = (uint8_t)(body[6] >> 7);
    f->lon_min = (uint8_t)(body[6] & 0x7FU);
    f->lon_deg = body[7];
    f->time_s = ((uint32_t)(body[13] >> 7) << 16) | ((uint32_t)body[8] << 8) | body[9];
    f->heading_ddeg = (uint16_t)(((uint16_t)(body[12] & 0x0FU) << 8) | body[10]);
    f->velocity_dkn = (uint16_t)(((uint16_t)(body[12] >> 4) << 8) | body[11]);
    f->emergency = (uint8_t)((body[13] >> 6) & 1U);
    f->quality = (uint8_t)((body[13] >> 4) & 3U);
    f->sats = (uint8_t)(body[13] & 0x0FU);
}

/* Degrees, minutes and ten-thousandths of a minute that name a real coordinate (spec V1): checked before any
 * conversion, so 22°60' is invalid rather than 23°; the boundary degree only with zero minutes. */
static int
gps_dm_valid(uint32_t deg, uint32_t max_deg, uint32_t min, uint32_t frac) {
    if (deg > max_deg || min > 59U || frac > 9999U) {
        return 0;
    }
    return deg < max_deg || (min == 0U && frac == 0U);
}

/* Validated degrees/minutes to degrees x 1e7, rounded half up: 1e-4 minute = 1e7 / 600000 units. */
static int32_t
gps_dm_to_e7(uint32_t deg, uint32_t min, uint32_t frac, uint8_t negative) {
    const uint32_t ten_thousandths = (min * 10000U) + frac;
    const uint32_t e7 = (deg * 10000000U) + (((ten_thousandths * 100U) + 3U) / 6U);
    return negative ? -(int32_t)e7 : (int32_t)e7;
}

/* The report's full UTC time from its time of day and the receive day (AABF-D-1: "The receiver is expected to fill in
 * the date based upon the day the location update is received"), across midnight: the day that puts the report within
 * half a day of its reception. Unknown ($1FFFF) and invalid (any other value above 86,399) give 0, never a time
 * folded into the day. */
static int64_t
harris_gps_fix_time(uint32_t time_s) {
    if (time_s >= (uint32_t)GPS_SECONDS_PER_DAY) {
        return 0;
    }
    const int64_t rx = (int64_t)dsd_decode_time();
    if (rx <= 0) {
        return 0;
    }
    int64_t t = rx - (rx % GPS_SECONDS_PER_DAY) + (int64_t)time_s;
    if (t - rx > GPS_SECONDS_PER_DAY / 2) {
        t -= GPS_SECONDS_PER_DAY;
    } else if (rx - t > GPS_SECONDS_PER_DAY / 2) {
        t += GPS_SECONDS_PER_DAY;
    }
    return t;
}

static void
harris_gps_build_fix(const harris_gps_fields* f, const dsd_gps_report_ctx* ctx, dsd_position_fix* fix) {
    dsd_position_fix_init(fix);
    fix->kind = DSD_POS_P25_HARRIS;
    gps_report_fill_identity(ctx, fix);
    fix->lat_e7 = gps_dm_to_e7(f->lat_deg, f->lat_min, f->lat_frac, f->lat_south);
    fix->lon_e7 = gps_dm_to_e7(f->lon_deg, f->lon_min, f->lon_frac, f->lon_west);
    fix->speed_dkmh = (uint16_t)((((uint32_t)f->velocity_dkn * 1852U) + 500U) / 1000U);
    fix->heading_ddeg = f->heading_ddeg <= DSD_POSITION_HEADING_MAX ? f->heading_ddeg : DSD_POSITION_HEADING_UNKNOWN;
    fix->sats = f->sats;
    fix->fix_time_s = harris_gps_fix_time(f->time_s);
    if (f->emergency) {
        fix->flags |= DSD_POS_FLAG_EMERGENCY;
    }
}

static void
harris_gps_print_details(const harris_gps_fields* f, const dsd_position_fix* fix, const char* deg_glyph) {
    if (fix->heading_ddeg <= DSD_POSITION_HEADING_MAX) {
        DSD_FPRINTF(stderr, " HEADING: %03u.%u%s;", (unsigned int)(fix->heading_ddeg / 10U),
                    (unsigned int)(fix->heading_ddeg % 10U), deg_glyph);
    } else {
        DSD_FPRINTF(stderr, " HEADING: unknown;");
    }
    DSD_FPRINTF(stderr, " SPD: %u.%u knots;", (unsigned int)(f->velocity_dkn / 10U),
                (unsigned int)(f->velocity_dkn % 10U));
    if (f->time_s < (uint32_t)GPS_SECONDS_PER_DAY) {
        DSD_FPRINTF(stderr, " TIME: %02u:%02u:%02u UTC;", (unsigned int)(f->time_s / 3600U),
                    (unsigned int)((f->time_s % 3600U) / 60U), (unsigned int)(f->time_s % 60U));
    } else {
        DSD_FPRINTF(stderr, " TIME: %s;", f->time_s == HARRIS_GPS_TIME_UNKNOWN ? "unknown" : "invalid");
    }
    DSD_FPRINTF(stderr, " %s; SATS: %u;", f->quality == 2U ? "DGPS Fix" : "GPS Fix", (unsigned int)f->sats);
    if (f->emergency) {
        DSD_FPRINTF(stderr, " EMERGENCY;");
    }
}

static void
harris_gps_publish(const dsd_opts* opts, dsd_state* state, const harris_gps_fields* f, const dsd_position_fix* fix,
                   const dsd_gps_report_ctx* ctx) {
    const char* deg_glyph = dsd_degrees_glyph();
    const uint8_t slot = fix->slot;
    const double latitude = (double)fix->lat_e7 / 1e7;
    const double longitude = (double)fix->lon_e7 / 1e7;
    const int heading_known = fix->heading_ddeg <= DSD_POSITION_HEADING_MAX;

    DSD_FPRINTF(stderr, " Harris GPS: %.6lf%s, %.6lf%s;", latitude, deg_glyph, longitude, deg_glyph);
    harris_gps_print_details(f, fix, deg_glyph);

    if (heading_known) {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                     "(%.6lf%s, %.6lf%s) %03u.%u%s", latitude, deg_glyph, longitude, deg_glyph,
                     (unsigned int)(fix->heading_ddeg / 10U), (unsigned int)(fix->heading_ddeg % 10U), deg_glyph);
    } else {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "(%.6lf%s, %.6lf%s)",
                     latitude, deg_glyph, longitude, deg_glyph);
    }
    gps_report_write_history(opts, state, slot, ctx, fix, state->dmr_embedded_gps[slot]);

    // -L: the -L placeholder 0 stands for an unknown heading, as before.
    const int speed_kph = (int)((fix->speed_dkmh + 5U) / 10U);
    const int azimuth = heading_known ? (int)(fix->heading_ddeg / 10U) : 0;
    gps_write_lrrp_compact(opts, state, slot, fix->source_id, latitude, longitude, speed_kph, azimuth);
}

int
nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* body, const dsd_gps_report_ctx* ctx) {
    if (!opts || !state || !body) {
        return 0;
    }
    if (!ctx) {
        ctx = &GPS_REPORT_CTX_NONE;
    }

    harris_gps_fields f;
    harris_gps_parse(body, &f);
    gps_report_print_prefix(ctx);

    /* A report whose own check did not pass says nothing by default, its quality included: only that it was unchecked
       (dsd_gps_report_content_printable(), issue #800, fail closed). With -Z it is read as far as admission. */
    if (!dsd_gps_report_content_printable(ctx->pos_verified == 1U, opts->payload)) {
        DSD_FPRINTF(stderr, " Harris GPS: not used (unchecked);");
        return 0;
    }
    // Quality %00 (no fix) and %11 (GPS not enabled): the coordinates are not a fix.
    if (f.quality == 0U || f.quality == 3U) {
        DSD_FPRINTF(stderr, " Harris GPS: %s;", f.quality == 0U ? "No Fix" : "GPS Not Enabled");
        return 0;
    }
    if (!gps_dm_valid(f.lat_deg, 90U, f.lat_min, f.lat_frac) || !gps_dm_valid(f.lon_deg, 180U, f.lon_min, f.lon_frac)) {
        DSD_FPRINTF(stderr, " Harris GPS: Invalid Position;");
        return 0;
    }

    dsd_position_fix fix;
    harris_gps_build_fix(&f, ctx, &fix);
    const dsd_position_admit_result admitted =
        dsd_position_admit(opts, state, &fix, ctx->pos_verified == 1U, ctx->source_verified == 1U, ctx->origin);
    if (admitted == DSD_POSITION_REFUSED) {
        DSD_FPRINTF(stderr, " Harris GPS: %s;", ctx->pos_verified == 1U ? "not used" : "not used (unchecked)");
        return 0;
    }
    harris_gps_publish(opts, state, &f, &fix, ctx);
    return (int)admitted;
}

//externalize embedded GPS - Confirmed working now on NE, NW, SE, and SW coordinates
/* The DMR burst that carried the embedded LC: its own sync type, which the receive policy judges the report by. */
static int
dmr_embedded_gps_protocol(const dsd_state* state) {
    if (DSD_SYNC_IS_DMR(state->synctype)) {
        return state->synctype;
    }
    return DSD_SYNC_IS_DMR(state->lastsynctype) ? state->lastsynctype : DSD_SYNC_DMR_BS_VOICE_POS;
}

/* The embedded GPS LC's attribution (spec 2.2): the slot's ACTIVE DMR call (a call of another protocol still open on
 * the slot is not the one this burst belongs to), and the source, target and group flag a checked full LC of that call's
 * transmission (a voice LC header or a late-entry LC, not a RAS acceptance) named under this reception, by the
 * attribution rule (dsd_position_attribution_grant()). */
static int
dmr_embedded_gps_call(const dsd_state* state, uint8_t slot, int report_protocol, dsd_call_snapshot* call,
                      dsd_position_attribution* granted) {
    dsd_position_attribution_clear(granted);
    if (dsd_call_state_get(state, slot, call) <= 0 || call->phase != DSD_CALL_PHASE_ACTIVE
        || !DSD_SYNC_IS_DMR(call->protocol)) {
        return 0;
    }
    dsd_position_attribution_call view;
    (void)dsd_position_attribution_call_from_slot(state, slot, &view);
    (void)dsd_position_attribution_grant(state, &state->dmr_lc_attribution[slot], &view, report_protocol, granted);
    return 1;
}

/* An embedded GPS fix (5-bit checksum: WEAK_CHECK), admitted with the attribution of the slot's active call. */
typedef struct {
    dsd_position_fix fix;
    dsd_call_snapshot call;
    int has_call;
} dmr_embedded_gps_admission;

static int
dmr_embedded_gps_admit(const dsd_opts* opts, dsd_state* state, uint8_t slot, dmr_embedded_gps_admission* a) {
    a->fix.protocol = (uint8_t)dmr_embedded_gps_protocol(state);
    dsd_position_attribution granted;
    a->has_call = dmr_embedded_gps_call(state, slot, a->fix.protocol, &a->call, &granted);
    const int source_verified = granted.source != 0U;
    a->fix.source_id = granted.source;
    gps_fill_target(&a->fix, granted.target, granted.group);
    return dsd_position_admit(opts, state, &a->fix, 1, source_verified, NULL) != DSD_POSITION_REFUSED;
}

/* Publish an admitted embedded GPS fix: the display text, the slot's active call's row (under the writer's
 * typed-source rule) and the location file. */
static void
dmr_embedded_gps_publish(const dsd_opts* opts, dsd_state* state, uint8_t slot, const dmr_embedded_gps_admission* a,
                         uint8_t pos_err) {
    const char* deg_glyph = dsd_degrees_glyph();
    const gps_hemisphere_pair p = gps_hemisphere_pair_from_e7(a->fix.lat_e7, a->fix.lon_e7);
    char err[64];
    gps_error_text(pos_err, a->fix.err_m, err, sizeof err);
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "GPS: %.5lf%s%s %.5lf%s%s %s",
                 p.lat, deg_glyph, p.latstr, p.lon, deg_glyph, p.lonstr, err);
    if (a->has_call) {
        (void)dsd_event_enrich_position(opts, state, slot, a->call.epoch, &a->fix, state->dmr_embedded_gps[slot]);
    }
    gps_write_lrrp_admitted(opts, &a->fix, 0, 0);
}

void
dmr_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]) {
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Embedded GPS:");
    // TS 102 361-2 Table 7.3: PF, reserved, FLCO, FID, reserved 4, position error 3 (bit 20), longitude 25 (bit 23),
    // latitude 24 (bit 48); §7.2.16/7.2.17 two's complement, 360/2^25 and 180/2^24 degrees.
    const uint8_t pf = lc_bits[0];
    const uint8_t pos_err = (uint8_t)convert_bits_into_output(&lc_bits[20], 3);
    const uint32_t lon = (uint32_t)convert_bits_into_output(&lc_bits[23], 25);
    const uint32_t lat = (uint32_t)convert_bits_into_output(&lc_bits[48], 24);

    const uint8_t slot = (uint8_t)(state->currentslot & 1);
    // The caller ran the embedded LC checksum; a burst scope that failed (a mono-collapsed slot's) still withholds it.
    const uint8_t crc_flags = state->event_crc_invalid[slot];
    if (pf) {
        /* A withheld LC says nothing of itself by default, its protect flag included (issue #800, fail closed). */
        if (dsd_gps_report_content_printable(crc_flags == 0U, opts->payload)) {
            DSD_FPRINTF(stderr, " Protected");
        } else {
            DSD_FPRINTF(stderr, " Position: (%s)", gps_suppressed_label(crc_flags));
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    dmr_embedded_gps_admission a;
    DSD_MEMSET(&a, 0, sizeof a);
    dsd_position_fix_init(&a.fix);
    a.fix.kind = DSD_POS_DMR_EMBEDDED;
    a.fix.slot = slot;
    a.fix.flags = (uint16_t)DSD_POS_FLAG_WEAK_CHECK;
    a.fix.lat_e7 = gps_twos_complement_e7(lat, 24U, GPS_LAT_SPAN_E7);
    a.fix.lon_e7 = gps_twos_complement_e7(lon, 25U, GPS_LON_SPAN_E7);
    a.fix.err_m = gps_position_error_m(pos_err, &a.fix.flags);
    const int admitted = crc_flags == 0U && dmr_embedded_gps_admit(opts, state, slot, &a);
    if (crc_flags != 0U || !admitted) {
        DSD_FPRINTF(stderr, " Position: (%s)", crc_flags != 0U ? gps_suppressed_label(crc_flags) : "refused");
    } else {
        const char* deg_glyph = dsd_degrees_glyph();
        const gps_hemisphere_pair p = gps_hemisphere_pair_from_e7(a.fix.lat_e7, a.fix.lon_e7);
        DSD_FPRINTF(stderr, " Lat: %.5lf%s%s Lon: %.5lf%s%s (%.5lf, %.5lf)", p.lat, deg_glyph, p.latstr, p.lon,
                    deg_glyph, p.lonstr, (double)a.fix.lat_e7 / 1e7, (double)a.fix.lon_e7 / 1e7);
    }
    if (dsd_gps_report_content_printable(crc_flags == 0U, opts->payload)) {
        gps_print_position_error(pos_err, (unsigned int)a.fix.err_m);
    }
    if (admitted) {
        dmr_embedded_gps_publish(opts, state, slot, &a, pos_err);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

/* P25 Motorola APX GPS, MFID $90 LCO $06 (sign + magnitude). The layout rests on one non-official decoder (SDRTrunk),
 * so its reports are FORMAT_PROVISIONAL (D12); bit 23, which older decoders read as "last fix", has no source and is
 * shown raw only. 23-bit magnitudes scale to 90 and 180 degrees, so every value is in range. */
static int32_t
apx_gps_to_e7(uint32_t magnitude, uint64_t full_scale_e7) {
    return (int32_t)((((uint64_t)magnitude * full_scale_e7) + (0x7FFFFFU / 2U)) / 0x7FFFFFU);
}

static void
apx_gps_print_reserved(const uint8_t lc_bits[]) {
    const uint8_t res_a = lc_bits[1];
    const uint8_t res_b = (uint8_t)convert_bits_into_output(&lc_bits[16], 7);
    if (res_a) {
        DSD_FPRINTF(stderr, "RES_A: %d; ", res_a);
    }
    if (res_b) {
        DSD_FPRINTF(stderr, "RES_B: %02X; ", res_b);
    }
    if (lc_bits[23]) {
        DSD_FPRINTF(stderr, "B23: 1; ");
    }
}

static void
apx_gps_publish(const dsd_opts* opts, dsd_state* state, const dsd_position_fix* fix, const dsd_gps_report_ctx* ctx,
                const uint8_t lc_bits[]) {
    const char* deg_glyph = dsd_degrees_glyph();
    const uint8_t slot = fix->slot;
    const double latitude = (double)fix->lat_e7 / 1e7;
    const double longitude = (double)fix->lon_e7 / 1e7;
    const double lat_abs = fix->lat_e7 < 0 ? -latitude : latitude;
    const double lon_abs = fix->lon_e7 < 0 ? -longitude : longitude;
    const char* latstr = fix->lat_e7 < 0 ? "S" : "N";
    const char* lonstr = fix->lon_e7 < 0 ? "W" : "E";
    DSD_FPRINTF(stderr, " Lat: %.5lf%s%s Lon: %.5lf%s%s (%.5lf, %.5lf) ", lat_abs, deg_glyph, latstr, lon_abs,
                deg_glyph, lonstr, latitude, longitude);
    apx_gps_print_reserved(lc_bits);
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "GPS: %lf%s%s %lf%s%s (%lf, %lf)",
                 lat_abs, deg_glyph, latstr, lon_abs, deg_glyph, lonstr, latitude, longitude);
    gps_report_write_history(opts, state, slot, ctx, fix, state->dmr_embedded_gps[slot]);
    gps_write_lrrp_compact(opts, state, slot, fix->source_id, latitude, longitude, 0, 0);
}

int
apx_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[], const dsd_gps_report_ctx* ctx) {
    if (!opts || !state || !lc_bits) {
        return 0;
    }
    if (!ctx) {
        ctx = &GPS_REPORT_CTX_NONE;
    }
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " GPS:");
    int result = 0;
    if (lc_bits[0]) {
        DSD_FPRINTF(stderr, " Protected");
    } else {
        dsd_position_fix fix;
        dsd_position_fix_init(&fix);
        fix.kind = DSD_POS_P25_APX;
        fix.flags = DSD_POS_FLAG_FORMAT_PROVISIONAL;
        gps_report_fill_identity(ctx, &fix);
        // Latitude: sign bit 24, magnitude 25-47 (0-90 degrees). Longitude: magnitude 49-71 (0-180), sign bit 48
        // offsets it by -180 degrees.
        fix.lat_e7 = apx_gps_to_e7((uint32_t)convert_bits_into_output(&lc_bits[25], 23), 900000000U);
        if (lc_bits[24]) {
            fix.lat_e7 = -fix.lat_e7;
        }
        fix.lon_e7 = apx_gps_to_e7((uint32_t)convert_bits_into_output(&lc_bits[49], 23), 1800000000U);
        if (lc_bits[48]) {
            fix.lon_e7 -= 1800000000;
        }
        const dsd_position_admit_result admitted =
            dsd_position_admit(opts, state, &fix, ctx->pos_verified == 1U, ctx->source_verified == 1U, ctx->origin);
        if (admitted == DSD_POSITION_REFUSED) {
            DSD_FPRINTF(stderr, " %s;", ctx->pos_verified == 1U ? "Not Used" : "Not Used (unchecked)");
        } else {
            apx_gps_publish(opts, state, &fix, ctx, lc_bits);
            result = (int)admitted;
        }
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
    return result;
}

void
decode_cellocator(dsd_opts* opts, dsd_state* state, const uint8_t* input, int len) {
    //TODO: This
    UNUSED(opts);
    UNUSED(state);
    UNUSED(input);
    UNUSED(len);

    // The four-octet signature and the type octet are read below; a shorter payload has neither.
    if (input == NULL || len < 5) {
        DSD_FPRINTF(stderr, " Cellocator: Truncated;");
        return;
    }

    //UTF8 Text: MCGP (0x4D434750) followed by data values
    //(0417D1050000F45FD1DD00010000000097BDD56C81000009AAAABF12864C)
    utf8_to_text(state, 0, 4, input);

    DSD_FPRINTF(stderr, " Cellocator:");

    uint8_t type = input[4];
    if (type == 1) {
        DSD_FPRINTF(stderr, " Platform Manifest Data;");
    } else if (type == 2) {
        DSD_FPRINTF(stderr, " CAN Data;");
    } else if (type == 3) {
        DSD_FPRINTF(stderr, " CAN Trigger Data;");
    } else if (type == 4) {
        DSD_FPRINTF(stderr, " Time and Location Data;");
    } else if (type == 5) {
        DSD_FPRINTF(stderr, " Accelerometer Data;");
    } else if (type == 6) {
        DSD_FPRINTF(stderr, " PSP Alarm System Data;");
    } else if (type == 7) {
        DSD_FPRINTF(stderr, " Usage Counter Data;");
    } else if (type == 8) {
        DSD_FPRINTF(stderr, " Command Authentication Table Data;");
    } else if (type == 9) {
        DSD_FPRINTF(stderr, " GSM Neighbor List Data;");
    } else if (type == 10) {
        DSD_FPRINTF(stderr, " Maintenance Server Platform Manifest Data;");
    } else {
        DSD_FPRINTF(stderr, " Unknown Data;");
    }

    //Data afterwards appears to be an arbitrary len so has variable reporting data
    //will need to establish a len value for data and contents
}

/* ---- ASCII NMEA 0183 (P25 SAP 48, NXDN data call format 0x06) ---- */

enum {
    NMEA_TEXT_MAX = 256, /* the checked sentences kept as text */
};

/* A packet's sentences: the checked ones joined by one space, and its position report (the first RMC with a measured
 * fix, else the first such GGA). */
typedef struct {
    char text[NMEA_TEXT_MAX];
    size_t text_len;
    int checked;
    int have_fix;
    nmea_fix fix;
    const char* refusal; /* why the first RMC or GGA gave no fix; NULL when none was refused */
} nmea_packet;

static void
nmea_print_sentence_error(const nmea_sentence_span* s) {
    if (s->status == NMEA_SENTENCE_NO_STAR) {
        DSD_FPRINTF(stderr, " Possible NMEA Sentence, Missing Ending *;");
    } else if (s->status == NMEA_SENTENCE_BAD_HEX) {
        DSD_FPRINTF(stderr, " NMEA Checksum Digits Invalid;");
    } else {
        DSD_FPRINTF(stderr, " NMEA Checksum Error (%02X / %02X);", s->calc, s->received);
    }
}

static void
nmea_packet_append_text(nmea_packet* pkt, const uint8_t* bytes, const nmea_sentence_span* s) {
    if (pkt->text_len > 0U && pkt->text_len + 1U < sizeof(pkt->text)) {
        pkt->text[pkt->text_len++] = ' ';
    }
    for (size_t i = 0U; i < s->len && pkt->text_len + 1U < sizeof(pkt->text); i++) {
        pkt->text[pkt->text_len++] = (char)bytes[s->start + i];
    }
    pkt->text[pkt->text_len] = '\0';
}

/* One sentence that passed its checksum: its text, and its fix when it is the packet's report. */
static void
nmea_packet_take(nmea_packet* pkt, const uint8_t* bytes, const nmea_sentence_span* s) {
    pkt->checked++;
    nmea_packet_append_text(pkt, bytes, s);
    nmea_fix f;
    nmea_parse_fix((const char*)bytes + s->start, s->len, (int64_t)dsd_decode_time(), &f);
    if (f.type == NMEA_TYPE_OTHER) {
        return;
    }
    if (f.measured_fix && f.has_position && f.lat_e7 == 0 && f.lon_e7 == 0) {
        /* Exactly 0, 0 is never a position (admission refuses it): it must not displace another sentence's fix. One
           zero coordinate is a place like any other. */
        f.measured_fix = 0;
        f.refusal = "position 0, 0";
    }
    if (!f.measured_fix || !f.has_position) {
        if (pkt->refusal == NULL) {
            pkt->refusal = f.refusal;
        }
        return;
    }
    if (!pkt->have_fix || (pkt->fix.type == NMEA_TYPE_GGA && f.type == NMEA_TYPE_RMC)) {
        pkt->fix = f;
        pkt->have_fix = 1;
    }
}

static void
nmea_packet_scan(const uint8_t* bytes, size_t len, nmea_packet* pkt) {
    size_t pos = 0U;
    int seen = 0;
    for (;;) {
        nmea_sentence_span s;
        const int r = nmea_next_sentence(bytes, len, &pos, &s);
        if (r == 0) {
            return;
        }
        if (r < 0) {
            DSD_FPRINTF(stderr, seen ? " Data After NMEA Sentence;" : " Not an NMEA Sentence Structure;");
            return;
        }
        seen = 1;
        if (s.status != NMEA_SENTENCE_OK) {
            nmea_print_sentence_error(&s);
            continue;
        }
        nmea_packet_take(pkt, bytes, &s);
    }
}

static uint8_t
gps_fix_protocol(const dsd_state* state) {
    return (state->lastsynctype >= 0 && state->lastsynctype < (int)DSD_POSITION_PROTOCOL_NONE)
               ? (uint8_t)state->lastsynctype
               : (uint8_t)DSD_POSITION_PROTOCOL_NONE;
}

/* The history text of an admitted position: "(lat, lon)" as the NXDN report has always written it. */
static void
gps_format_position_text(const dsd_position_fix* fix, char* out, size_t out_size) {
    const char* deg = dsd_degrees_glyph();
    DSD_SNPRINTF(out, out_size, "(%.6f%s, %.6f%s)", (double)fix->lat_e7 / 1e7, deg, (double)fix->lon_e7 / 1e7, deg);
}

/* Write the -L row of an admitted report (km/h and degrees as whole numbers; 0 stands for an unknown one). */
static void
gps_write_admitted_row(const dsd_opts* opts, const dsd_state* state, int slot, const dsd_position_fix* fix) {
    if (fix->source_id == 0U) {
        return;
    }
    const int speed = (fix->speed_dkmh != DSD_POSITION_SPEED_UNKNOWN) ? (int)(fix->speed_dkmh / 10U) : 0;
    const int heading = (fix->heading_ddeg <= DSD_POSITION_HEADING_MAX) ? (int)(fix->heading_ddeg / 10U) : 0;
    gps_write_lrrp_slash_colon(opts, state, slot, fix->source_id, (double)fix->lat_e7 / 1e7, (double)fix->lon_e7 / 1e7,
                               speed, heading);
}

static void
nmea_print_fix(const dsd_position_fix* fix) {
    char text[96];
    gps_format_position_text(fix, text, sizeof(text));
    DSD_FPRINTF(stderr, "\n NMEA Fix: %s;", text);
    if (fix->speed_dkmh != DSD_POSITION_SPEED_UNKNOWN) {
        DSD_FPRINTF(stderr, " Speed: %.1f km/h;", (double)fix->speed_dkmh / 10.0);
    }
    if (fix->heading_ddeg <= DSD_POSITION_HEADING_MAX) {
        DSD_FPRINTF(stderr, " Course: %.1f%s;", (double)fix->heading_ddeg / 10.0, dsd_degrees_glyph());
    }
    if (fix->alt_dm != DSD_POSITION_ALT_UNKNOWN) {
        DSD_FPRINTF(stderr, " Altitude: %.1f m;", (double)fix->alt_dm / 10.0);
    }
    if (fix->sats != DSD_POSITION_SATS_NONE) {
        DSD_FPRINTF(stderr, " Satellites: %u;", (unsigned)fix->sats);
    }
}

/* Admit the packet's report: the position and the packet's source and destination (the slot's dmr_lrrp_source and
 * dmr_lrrp_target) only from a packet whose own check passed (both verdicts are the packet's). */
static dsd_position_admit_result
nmea_admit_packet(const dsd_opts* opts, dsd_state* state, uint8_t slot, const nmea_packet* pkt, int packet_verified,
                  const dsd_position_origin* origin, dsd_position_fix* fix) {
    if (!pkt->have_fix) {
        if (pkt->refusal != NULL && packet_verified == 1) {
            DSD_FPRINTF(stderr, "\n NMEA: no measured fix (%s);", pkt->refusal);
        }
        return DSD_POSITION_REFUSED;
    }
    dsd_position_fix_init(fix);
    fix->kind = (uint8_t)DSD_POS_NMEA;
    fix->protocol = gps_fix_protocol(state);
    fix->slot = slot;
    fix->lat_e7 = pkt->fix.lat_e7;
    fix->lon_e7 = pkt->fix.lon_e7;
    fix->alt_dm = pkt->fix.alt_dm;
    fix->speed_dkmh = pkt->fix.speed_dkmh;
    fix->heading_ddeg = pkt->fix.heading_ddeg;
    fix->sats = pkt->fix.sats;
    fix->fix_time_s = pkt->fix.fix_time_s;
    fix->source_id = (uint32_t)state->dmr_lrrp_source[slot];
    gps_fill_header_target(state, slot, fix);
    const dsd_position_admit_result r = dsd_position_admit(opts, state, fix, packet_verified, packet_verified, origin);
    if (r == DSD_POSITION_REFUSED && packet_verified == 1) {
        DSD_FPRINTF(stderr, "\n NMEA: position not admitted;");
    }
    return r;
}

/* The checked sentences' text as History and the console may show it: only a packet whose report admission admitted
 * (issue #800, fail closed). The sentences of any other packet (one whose check failed, one with no measured fix, one
 * admission refused) can hold the very coordinates the decoder rejected, in any sentence form: they are withheld. */
static const char*
nmea_packet_text(const nmea_packet* pkt, int packet_verified, int admitted) {
    if (pkt->checked <= 0 || admitted) {
        return pkt->text;
    }
    return packet_verified != 1 ? "NMEA withheld (CRC ERR)" : "NMEA withheld (no admitted position)";
}

/* The console after admission (a checked packet): the sentences and the fix only of an admitted report
 * (nmea_packet_text()). Labelled, as the sentences can hold a position in any form (issue #800, D10). */
static void
nmea_print_admitted(const nmea_packet* pkt, dsd_position_admit_result result, const dsd_position_fix* fix) {
    if (result == DSD_POSITION_REFUSED) {
        DSD_FPRINTF(stderr, " NMEA Withheld (no admitted position);");
        return;
    }
    DSD_FPRINTF(stderr, " NMEA: %s", pkt->text);
    nmea_print_fix(fix);
}

/* History and -L: the checked sentences' text (only from a packet whose report was admitted, nmea_packet_text()), the
 * data notice, and with an admitted report its position (staged just before the notice) and its -L row. */
static void
nmea_publish(const dsd_opts* opts, dsd_state* state, uint8_t slot, const nmea_packet* pkt, int packet_verified,
             dsd_position_admit_result result, const dsd_position_fix* fix) {
    const int admitted = result != DSD_POSITION_REFUSED;
    if (state->event_history_s != NULL) {
        const char* text = nmea_packet_text(pkt, packet_verified, admitted);
        dsd_event_stage_text(state, slot, text);
        if (pkt->checked > 0) {
            const uint32_t source = admitted ? fix->source_id : (uint32_t)state->dmr_lrrp_source[slot];
            const uint32_t target = (uint32_t)state->dmr_lrrp_target[slot];
            char comp_string[128];
            DSD_SNPRINTF(comp_string, sizeof(comp_string), "NMEA SRC: %u; TGT: %u;", source, target);
            const dsd_call_observation observation =
                dsd_call_observation_data(state->lastsynctype, slot, source, target);
            if (admitted) {
                char gps_text[96];
                gps_format_position_text(fix, gps_text, sizeof(gps_text));
                (void)dsd_event_stage_position(state, slot, fix, gps_text);
            }
            (void)dsd_event_emit_data_notice((dsd_opts*)opts, state, slot, &observation, comp_string);
        }
    }
    if (admitted) {
        gps_write_admitted_row(opts, state, slot, fix);
    }
}

/* The payload's octets (one bit per byte in @p input), every one of them: the caller holds @p len_bytes to
 * NMEA_PAYLOAD_MAX_BYTES. Sentences and the fix chosen among them are read from the whole payload, never a prefix (a
 * prefix could hold a GGA but not the RMC after it). */
static size_t
nmea_payload_octets(const uint8_t* input, int len_bytes, uint8_t* bytes) {
    const size_t n = (size_t)len_bytes;
    for (size_t i = 0U; i < n; i++) {
        bytes[i] = (uint8_t)convert_bits_into_output(input + (i * 8U), 8);
    }
    return n;
}

int
nmea_payload_framed(const uint8_t* input, int len_bytes) {
    if (input == NULL || len_bytes <= 0 || len_bytes > NMEA_PAYLOAD_MAX_BYTES) {
        return 0;
    }
    uint8_t bytes[NMEA_PAYLOAD_MAX_BYTES];
    const size_t n = nmea_payload_octets(input, len_bytes, bytes);
    size_t pos = 0U;
    nmea_sentence_span s;
    return nmea_next_sentence(bytes, n, &pos, &s) == 1;
}

uint8_t
nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot, int len_bytes,
                      int packet_verified, const dsd_position_origin* origin) {
    if (opts == NULL || state == NULL || input == NULL || len_bytes <= 0) {
        return 0U;
    }
    if (len_bytes > NMEA_PAYLOAD_MAX_BYTES) {
        /* Longer than any carrier's payload (P25 SAP 48, NXDN format 0x06): never read in part. */
        DSD_FPRINTF(stderr, " NMEA: payload too long (%d octets); not decoded;", len_bytes);
        return 0U;
    }
    const uint8_t slot_idx = (slot >= 2U) ? 1U : slot;
    uint8_t bytes[NMEA_PAYLOAD_MAX_BYTES];
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    const size_t n = nmea_payload_octets(input, len_bytes, bytes);

    nmea_packet pkt;
    DSD_MEMSET(&pkt, 0, sizeof(pkt));
    nmea_packet_scan(bytes, n, &pkt);
    if (pkt.checked > 0 && origin != NULL && !dsd_position_origin_matches(state, origin)) {
        /* The packet began on another reception (a retune or a trunk scan switch since its header): nothing of it, its
           sentences' text and its notice included, belongs to the one the receiver is on now. */
        DSD_FPRINTF(stderr, " NMEA Withheld (reception changed);");
        state->dmr_lrrp_source[slot_idx] = 0U;
        state->dmr_lrrp_target[slot_idx] = 0U;
        return 1U;
    }
    if (pkt.checked > 0 && packet_verified != 1) {
        DSD_FPRINTF(stderr, " NMEA Withheld (CRC ERR);");
    }

    dsd_position_fix fix;
    dsd_position_fix_init(&fix);
    const dsd_position_admit_result result =
        (pkt.checked > 0) ? nmea_admit_packet(opts, state, slot_idx, &pkt, packet_verified, origin, &fix)
                          : DSD_POSITION_REFUSED;
    if (pkt.checked > 0 && packet_verified == 1) {
        nmea_print_admitted(&pkt, result, &fix);
    }
    nmea_publish(opts, state, slot_idx, &pkt, packet_verified, result, &fix);

    state->dmr_lrrp_source[slot_idx] = 0U;
    state->dmr_lrrp_target[slot_idx] = 0U;
    return (uint8_t)(pkt.checked > 0);
}

/* ---- NXDN GPS ---- */

/* The NXDN GPS report has no official source: its layout is SDRTrunk's (nxdn/layer3/data/GPS.java), whose offsets count
 * from the 0xFFFC marker, so they are 16 more than these: elevation 56 (16 bits, unit unknown), speed 74 (14, 0.1
 * km/h), heading 92 (12, 0.1 degree; 0xFFF none), date 136 (7, year - 2000) / 143 (4, month) / 147 (5, day - 1),
 * longitude 152 (16, DDDMM) / 183 (hemisphere), latitude 184 (16, DDMM) / 200 (15, 1/10000 minute) / 215 (hemisphere),
 * time 247 (5, hour) / 252 (6, minute) / 258 (6, second). The longitude's fraction is not read: its offset rests on
 * that one source (a decided limit), so the longitude keeps whole minutes. */
typedef struct {
    int32_t lat_e7;
    int32_t lon_e7;
    uint16_t speed_dkmh;
    uint16_t heading_ddeg; /* DSD_POSITION_HEADING_UNKNOWN when out of range */
    int16_t elevation;
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    uint8_t date_ok, time_ok;
    int64_t fix_time_s;
} nxdn_gps_fields;

/* DDMM / DDDMM with a 1/10000-minute fraction, checked before it is converted: degrees <= @p max_deg, minutes <= 59,
 * fraction <= 9999, the boundary degree only with zero minutes and fraction. */
static int
nxdn_gps_coordinate(uint16_t degmin, uint16_t frac, uint8_t west_or_south, uint32_t max_deg, int32_t* out_e7) {
    const uint32_t deg = (uint32_t)degmin / 100U;
    const uint32_t min = (uint32_t)degmin % 100U;
    if (deg > max_deg || min > 59U || frac > 9999U || (deg == max_deg && (min != 0U || frac != 0U))) {
        return 0;
    }
    const int64_t min_e7 = ((int64_t)min * 10000000) + ((int64_t)frac * 1000);
    const int64_t e7 = ((int64_t)deg * 10000000) + ((min_e7 + 30) / 60);
    *out_e7 = (int32_t)(west_or_south ? -e7 : e7);
    return 1;
}

static void
nxdn_gps_read_time(const uint8_t* input, nxdn_gps_fields* g) {
    g->year = (uint16_t)(convert_bits_into_output(input + 136, 7) + 2000U);
    g->month = (uint8_t)convert_bits_into_output(input + 143, 4);
    g->day = (uint8_t)(convert_bits_into_output(input + 147, 5) + 1U);
    g->hour = (uint8_t)convert_bits_into_output(input + 247, 5);
    g->minute = (uint8_t)convert_bits_into_output(input + 252, 6);
    g->second = (uint8_t)convert_bits_into_output(input + 258, 6);
    g->date_ok = (uint8_t)nmea_civil_date_valid(g->year, g->month, g->day);
    g->time_ok = (uint8_t)(g->hour <= 23U && g->minute <= 59U && g->second <= 59U);
    const int64_t t = nmea_utc_seconds(g->year, g->month, g->day, g->hour, g->minute, g->second);
    g->fix_time_s = (t > 0) ? t : 0;
}

/* 1 with @p g filled when the position is valid; an invalid date or time only leaves the fix time unknown. */
static int
nxdn_gps_read(const uint8_t* input, nxdn_gps_fields* g) {
    DSD_MEMSET(g, 0, sizeof(*g));
    const uint16_t lon_degmin = (uint16_t)convert_bits_into_output(input + 152, 16);
    const uint8_t lon_hem = (uint8_t)convert_bits_into_output(input + 183, 1);
    const uint16_t lat_degmin = (uint16_t)convert_bits_into_output(input + 184, 16);
    const uint16_t lat_frac = (uint16_t)convert_bits_into_output(input + 200, 15);
    const uint8_t lat_hem = (uint8_t)convert_bits_into_output(input + 215, 1);
    if (!nxdn_gps_coordinate(lat_degmin, lat_frac, lat_hem, 90U, &g->lat_e7)
        || !nxdn_gps_coordinate(lon_degmin, 0U, lon_hem, 180U, &g->lon_e7)) {
        return 0;
    }
    g->elevation = (int16_t)convert_bits_into_output(input + 56, 16);
    g->speed_dkmh = (uint16_t)convert_bits_into_output(input + 74, 14);
    const uint16_t heading = (uint16_t)convert_bits_into_output(input + 92, 12);
    g->heading_ddeg = (heading <= DSD_POSITION_HEADING_MAX) ? heading : DSD_POSITION_HEADING_UNKNOWN;
    nxdn_gps_read_time(input, g);
    return 1;
}

static void
nxdn_gps_print(const nxdn_gps_fields* g, const dsd_position_fix* fix) {
    char text[96];
    gps_format_position_text(fix, text, sizeof(text));
    DSD_FPRINTF(stderr, "\n GPS: %s ", text);
    DSD_FPRINTF(stderr, "Speed: %.1f k/h; ", (double)g->speed_dkmh / 10.0);
    if (g->heading_ddeg <= DSD_POSITION_HEADING_MAX) {
        DSD_FPRINTF(stderr, "COG: %.1f; ", (double)g->heading_ddeg / 10.0);
    } else {
        DSD_FPRINTF(stderr, "COG: unknown; ");
    }
    DSD_FPRINTF(stderr, "Elevation: %d; ", (int)g->elevation);
    if (g->date_ok) {
        DSD_FPRINTF(stderr, "Date: %04u/%02u/%02u; ", (unsigned)g->year, (unsigned)g->month, (unsigned)g->day);
    } else {
        DSD_FPRINTF(stderr, "Date: unknown; ");
    }
    if (g->time_ok) {
        DSD_FPRINTF(stderr, "Time: %02u:%02u:%02u;", (unsigned)g->hour, (unsigned)g->minute, (unsigned)g->second);
    } else {
        DSD_FPRINTF(stderr, "Time: unknown;");
    }
}

/* The report is a data transmission of its own: it goes to its own data notice row only, never onto a voice call's
 * row (controller ruling, as for DMR LIP), with its -L row. */
static void
nxdn_gps_publish(const dsd_opts* opts, dsd_state* state, const dsd_position_fix* fix) {
    if (state->event_history_s != NULL) {
        char gps_string[96];
        gps_format_position_text(fix, gps_string, sizeof(gps_string));

        /* The notice names the checked header bound to this assembly: its source is the record's. */
        const uint32_t source = fix->source_id;
        const uint32_t target = (uint32_t)state->dmr_lrrp_target[0];
        char comp_string[128];
        DSD_SNPRINTF(comp_string, sizeof(comp_string), "GPS SRC: %u; TGT: %u;", source, target);
        const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, 0U, source, target);
        (void)dsd_event_emit_data_notice_with_position((dsd_opts*)opts, state, 0U, &observation,
                                                       DSD_EVENT_CATEGORY_DATA, comp_string, fix, gps_string);
    }
    gps_write_admitted_row(opts, state, 0, fix);
}

void
nxdn_gps_report(const dsd_opts* opts, dsd_state* state, const uint8_t* input, size_t input_bits, uint32_t src,
                const dsd_position_origin* origin) {
    if (opts == NULL || state == NULL || input == NULL) {
        return;
    }
    nxdn_gps_fields g;
    if (input_bits < DSD_NXDN_GPS_REPORT_BITS) {
        DSD_FPRINTF(stderr, " GPS: NXDN position report too short;");
    } else if (!nxdn_gps_read(input, &g)) {
        DSD_FPRINTF(stderr, " GPS: Invalid NXDN position report;");
    } else {
        dsd_position_fix fix;
        dsd_position_fix_init(&fix);
        fix.kind = (uint8_t)DSD_POS_NXDN_GPS;
        fix.protocol = gps_fix_protocol(state);
        fix.slot = 0U;
        fix.lat_e7 = g.lat_e7;
        fix.lon_e7 = g.lon_e7;
        fix.speed_dkmh = g.speed_dkmh;
        fix.heading_ddeg = g.heading_ddeg;
        fix.fix_time_s = g.fix_time_s;
        fix.source_id = src;
        fix.flags = (uint16_t)DSD_POS_FLAG_FORMAT_PROVISIONAL;
        gps_fill_header_target(state, 0U, &fix); // the same bound header's destination
        /* Called only for a data call whose CRC32 passed, with the header bound to its assembly (nxdn_element.c). */
        if (dsd_position_admit(opts, state, &fix, 1, 1, origin) == DSD_POSITION_REFUSED) {
            DSD_FPRINTF(stderr, " GPS: NXDN position report not admitted;");
        } else {
            nxdn_gps_print(&g, &fix);
            nxdn_gps_publish(opts, state, &fix);
        }
    }
    state->dmr_lrrp_source[0] = 0U;
    state->dmr_lrrp_target[0] = 0U;
}
