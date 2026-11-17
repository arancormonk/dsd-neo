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
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/position.h>
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

/* Enrich the slot's ACTIVE call's row with GPS text: with an admitted position report (@p fix, from
 * dsd_position_admit()) the text and the position go in together under the writer's typed-source rule
 * (dsd_event_enrich_position()); with @p fix NULL the text alone (dsd_event_enrich_gps()). A nonzero
 * @p expected_source must be the call's source. */
static int
gps_enrich_active_call(dsd_state* state, uint8_t slot, uint32_t expected_source, const char* text,
                       const dsd_position_fix* fix, dsd_call_snapshot* call_out) {
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
        return dsd_event_enrich_position(state, slot, call.epoch, fix, text);
    }
    return dsd_event_enrich_gps(state, slot, call.epoch, text);
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

static void
nmea_harris_print_src_prefix(uint16_t header, uint32_t src, int slot) {
    if (header == 0x2AA4U) {
        DSD_FPRINTF(stderr, " SRC: %08u;", src);
    } else {
        DSD_FPRINTF(stderr, " VCH: %d - SRC: %08u;", slot, src);
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
} lip_source;

/* Print a LIP report. @p position_note NULL prints its coordinates (an admitted report); otherwise the coordinates are
 * left out and the note says why ("suppressed; CRC ERR", "refused"). */
static void
lip_print_report(const lip_short_report* r, const lip_source* who, const char* position_note) {
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
    char motion[80];
    lip_motion_text(r, deg_glyph, motion, sizeof motion);
    DSD_FPRINTF(stderr, "%s", motion);
    gps_print_position_error(r->position_error, (unsigned int)r->err_m);
    DSD_FPRINTF(stderr, ";");
    lip_print_reason(r, who->hash);
    lip_print_time_elapsed(r->time_elapsed);
}

/* The row's and the display's text of an admitted LIP report, into dsd_state::dmr_embedded_gps[slot]. */
static void
lip_store_text(dsd_state* state, int slot, const lip_short_report* r, int hash) {
    const char* deg_glyph = dsd_degrees_glyph();
    const gps_hemisphere_pair p = gps_hemisphere_pair_from_e7(r->lat_e7, r->lon_e7);
    char prefix[16] = "";
    if (hash >= 0) {
        DSD_SNPRINTF(prefix, sizeof prefix, "%03d; ", hash);
    }
    char err[64];
    char motion[80];
    gps_error_text(r->position_error, r->err_m, err, sizeof err);
    lip_motion_text(r, deg_glyph, motion, sizeof motion);
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                 "%sLIP: %.5lf%s%s %.5lf%s%s; %s; %s", prefix, p.lat, deg_glyph, p.latstr, p.lon, deg_glyph, p.lonstr,
                 err, motion);
}

/* Why a report of the slot's scope is unchecked, for the console: a failed check, or a RAS acceptance. */
static const char*
gps_suppressed_label(uint8_t crc_flags) {
    return dsd_crc_is_failed(crc_flags) ? "suppressed; CRC ERR" : "suppressed; RAS";
}

/* Admit a checked LIP report: the record, its kind and source (a USBD reply's hash is never an identity), and the
 * reception it came from (a DMR data assembly's origin, captured with its header). */
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
        source_verified = 1; // the same checked packet named it
        if (DSD_SYNC_IS_DMR(state->lastsynctype) && state->dmr_data_origin[slot].valid) {
            origin = &state->dmr_data_origin[slot];
        }
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
    lip_print_report(r, who, crc_flags != 0U ? gps_suppressed_label(crc_flags) : (admitted ? NULL : "refused"));
    if (!admitted) {
        return;
    }
    lip_store_text(state, slot, r, who->hash);
    (void)dsd_event_stage_position(state, (uint8_t)slot, &fix, state->dmr_embedded_gps[slot]);
    if (who->hash < 0) {
        const int speed = fix.speed_dkmh == DSD_POSITION_SPEED_UNKNOWN ? 0 : gps_speed_kmh(fix.speed_dkmh);
        const int azimuth = (fix.flags & DSD_POS_FLAG_HEADING_AMBIGUOUS) != 0U ? 0 : (int)(fix.heading_ddeg / 10U);
        gps_write_lrrp_admitted(opts, &fix, speed, azimuth);
    }
}

static uint8_t
nmea_validate_checksum(const uint8_t* input, int len_bytes, uint8_t* end_value, uint8_t* checksum_calc,
                       uint8_t* checksum_ext) {
    uint8_t start_value = (uint8_t)convert_bits_into_output(input, 8);
    if (start_value != (uint8_t)'$' && start_value != (uint8_t)'!') {
        return 0U;
    }

    int star_pos = -1;
    *end_value = 0U;
    *checksum_calc = 0U;
    *checksum_ext = 0U;

    for (int i = 1; i < len_bytes; i++) {
        uint8_t value = (uint8_t)convert_bits_into_output(input + ((size_t)i * 8U), 8);
        if (value == (uint8_t)'*') {
            *end_value = value;
            star_pos = i;
            break;
        }
        if (value >= 0x20U && value < 0x7FU) {
            *checksum_calc ^= value;
        } else {
            break;
        }
    }

    if (star_pos < 0 || (star_pos + 2) >= len_bytes) {
        return 0U;
    }

    uint8_t h0 = (uint8_t)convert_bits_into_output(input + ((size_t)(star_pos + 1) * 8U), 8);
    uint8_t h1 = (uint8_t)convert_bits_into_output(input + ((size_t)(star_pos + 2) * 8U), 8);
    int n0 = dsd_hex_nibble_value(h0);
    int n1 = dsd_hex_nibble_value(h1);
    if (n0 < 0 || n1 < 0) {
        return 0U;
    }
    *checksum_ext = (uint8_t)((n0 << 4) | n1);
    return (uint8_t)(*checksum_ext == *checksum_calc);
}

static void
nmea_copy_printable_sentence(const uint8_t* input, int len_bytes, char* out, size_t out_cap) {
    size_t w = 0U;
    int i = 0;
    while (i < len_bytes && w + 1U < out_cap) {
        uint8_t ascii = (uint8_t)convert_bits_into_output(input + ((size_t)i * 8U), 8);
        uint16_t crlf = 0xFFFFU;
        if ((i + 1) < len_bytes) {
            crlf = (uint16_t)convert_bits_into_output(input + ((size_t)i * 8U), 16);
        }

        if (ascii >= 0x20U && ascii < 0x7FU) {
            out[w++] = (char)ascii;
            i++;
        } else if (crlf == 0x0D0AU) {
            out[w++] = ' ';
            i += 2;
        } else {
            break;
        }
    }
    out[w] = '\0';
}

static void
nmea_print_invalid_reason(uint8_t start_value, uint8_t end_value, uint8_t checksum_calc, uint8_t checksum_ext) {
    if (start_value != (uint8_t)'$' && start_value != (uint8_t)'!') {
        DSD_FPRINTF(stderr, " Not an NMEA Sentence Structure;");
    } else if (end_value != (uint8_t)'*') {
        DSD_FPRINTF(stderr, " Possible NMEA Sentence, Missing Ending *;");
    } else {
        DSD_FPRINTF(stderr, " NMEA Checksum Error (%02X / %02X);", checksum_calc, checksum_ext);
    }
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
    const lip_source who = {(int)convert_bits_into_output(input + 72, 8), 0U};
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
lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count, uint32_t src) {
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
        const lip_source who = {-1, src};
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

static void
udt_nmea_read(const uint8_t* input, udt_nmea_fields* f) {
    // uint8_t nmea_c = input[0];  //encrypted -- checked before we get here
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
    f->utc_ss3 = (uint8_t)convert_bits_into_output(&input[77], 3);
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
udt_nmea_fill_fix(const dsd_state* state, const udt_nmea_fields* f, int type, uint32_t src, int slot,
                  dsd_position_fix* fix) {
    dsd_position_fix_init(fix);
    fix->kind = DSD_POS_NMEA;
    fix->protocol = (uint8_t)state->lastsynctype;
    fix->slot = (uint8_t)slot;
    fix->source_id = src;
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

/* Admit a checked, in-range UDT NMEA report (the UDT's source, from the same checked header) against the reception
 * its UDT began on. */
static int
udt_nmea_admit(const dsd_opts* opts, dsd_state* state, int slot, dsd_position_fix* fix) {
    const dsd_position_origin* origin = (DSD_SYNC_IS_DMR(state->lastsynctype) && state->dmr_data_origin[slot].valid)
                                            ? &state->dmr_data_origin[slot]
                                            : NULL;
    return dsd_position_admit(opts, state, fix, 1, 1, origin) != DSD_POSITION_REFUSED;
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
nmea_iec_61162_1(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int type) {
    const int slot = state->currentslot & 1;

    //NOTE: The Only difference between Short (type == 1) and Long Format (type == 2)
    //is the utc_ss3 on short vs utc_ss6 and inclusion of COG value on long

    //NOTE: MFID Specific Formats are not handled here, unknown, could be added if worked out
    //they could share most of the same elements and use the large spare bits in block 2 for extra
    // uint8_t mfid = (uint8_t)convert_bits_into_output(&input[88], 8); //on type 3, last octet of first block carries MFID

    udt_nmea_fields f;
    udt_nmea_read(input, &f);
    const char* deg_glyph = dsd_degrees_glyph();
    const uint8_t crc_flags = state->event_crc_invalid[slot];
    const int in_range = udt_nmea_position_in_range(&f);

    // Q = 0 is "No fix" and the Table B.7 ranges bound every field: neither is a position (spec 2.5).
    dsd_position_fix fix;
    udt_nmea_fill_fix(state, &f, type, src, slot, &fix);
    const int admitted = crc_flags == 0U && f.q && in_range && udt_nmea_admit(opts, state, slot, &fix);
    udt_nmea_print_position(crc_flags, &f, in_range, admitted, &fix, deg_glyph);

    //Speed in Knots
    if (f.speed > 126) {
        DSD_FPRINTF(stderr, " SPD > 126 knots;");
    } else {
        DSD_FPRINTF(stderr, " SPD: %d knots; %f kph;", f.speed, (double)f.speed * 1.852);
    }
    udt_nmea_print_time(&f, type, deg_glyph);
    if (!admitted) {
        return;
    }
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "GPS: (%f%s, %f%s)",
                 (double)fix.lat_e7 / 1e7, deg_glyph, (double)fix.lon_e7 / 1e7, deg_glyph);
    (void)dsd_event_stage_position(state, (uint8_t)slot, &fix, state->dmr_embedded_gps[slot]);
    const int azimuth = fix.heading_ddeg == DSD_POSITION_HEADING_UNKNOWN ? 0 : (int)(fix.heading_ddeg / 10U);
    gps_write_lrrp_admitted(opts, &fix, gps_speed_kmh(fix.speed_dkmh), azimuth);
}

//restructured the Harris GPS to flow more like the DMR UDT NMEA Format when possible
void
nmea_harris(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int slot) {
    if (!opts || !state || !input) {
        return;
    }

    /*
     * L3Harris Talker GPS (P25 Phase 1/2)
     *
     * Aligns with SDRTrunk's L3HarrisGPS bitfield definitions.
     *
     * Note: In our codepaths, the formatted GPS data starts at bit offset 40:
     * - P25p1: we concatenate the two 56-bit LC blocks and place them at +40
     * - P25p2: vendor MAC structure places the GPS at offset 16+24 == 40
     */
    const uint32_t gps_off = 40;

    const char* deg_glyph = dsd_degrees_glyph();

    // Detect LCW vs MAC only for display context.
    uint16_t header = (uint16_t)convert_bits_into_output(&input[0], 16);

    // Latitude: degrees/minutes/fractional minutes (1/10000) with hemisphere flag
    uint32_t lat_frac = (uint32_t)convert_bits_into_output(&input[gps_off + 0], 16);
    uint8_t lat_hemi = input[gps_off + 16] & 1; //1 == negative
    uint32_t lat_min = (uint32_t)convert_bits_into_output(&input[gps_off + 17], 7);
    uint32_t lat_deg = (uint32_t)convert_bits_into_output(&input[gps_off + 24], 8);

    // Longitude: degrees/minutes/fractional minutes (1/10000) with hemisphere flag
    uint32_t lon_frac = (uint32_t)convert_bits_into_output(&input[gps_off + 32], 16);
    uint8_t lon_hemi = input[gps_off + 48] & 1; //1 == negative
    uint32_t lon_min = (uint32_t)convert_bits_into_output(&input[gps_off + 49], 7);
    uint32_t lon_deg = (uint32_t)convert_bits_into_output(&input[gps_off + 56], 8);

    double latitude = (double)lat_deg + (((double)lat_min + ((double)lat_frac / 10000.0)) / 60.0);
    if (lat_hemi) {
        latitude *= -1.0;
    }

    double longitude = (double)lon_deg + (((double)lon_min + ((double)lon_frac / 10000.0)) / 60.0);
    if (lon_hemi) {
        longitude *= -1.0;
    }

    // Timestamp: seconds since midnight UTC (16-bit + separate MSB flag)
    uint32_t seconds = (uint32_t)convert_bits_into_output(&input[gps_off + 64], 16);
    if (input[gps_off + 80]) {
        seconds += 65536U;
    }
    seconds %= 86400U; //match HH:mm:ss formatting behavior

    uint32_t thour = seconds / 3600U;
    uint32_t tmin = (seconds % 3600U) / 60U;
    uint32_t tsec = seconds % 60U;

    // Heading: 9-bit field (0-359 degrees per SDRTrunk; keep raw)
    uint16_t heading = (uint16_t)convert_bits_into_output(&input[gps_off + 95], 9);

    // Sanity check
    if (fabs(latitude) > 90.0 || fabs(longitude) > 180.0) {
        DSD_FPRINTF(stderr, "\n");
        nmea_harris_print_src_prefix(header, src, slot);
        DSD_FPRINTF(stderr, " Harris GPS: Invalid Position;");
        return;
    }

    uint8_t slot_idx = (slot >= 2) ? 1 : (uint8_t)slot;

    DSD_FPRINTF(stderr, "\n");
    nmea_harris_print_src_prefix(header, src, slot);
    DSD_FPRINTF(stderr, " Harris GPS: %.6lf%s, %.6lf%s;", latitude, deg_glyph, longitude, deg_glyph);
    DSD_FPRINTF(stderr, " HEADING: %03u%s;", (unsigned int)heading, deg_glyph);
    DSD_FPRINTF(stderr, " TIME: %02u:%02u:%02u UTC;", (unsigned int)thour, (unsigned int)tmin, (unsigned int)tsec);

    // save to ncurses string
    DSD_SNPRINTF(state->dmr_embedded_gps[slot_idx], sizeof state->dmr_embedded_gps[slot_idx],
                 "(%.6lf%s, %.6lf%s) %03u%s", latitude, deg_glyph, longitude, deg_glyph, (unsigned int)heading,
                 deg_glyph);

    // save to event history string
    (void)gps_enrich_active_call(state, slot_idx, src, state->dmr_embedded_gps[slot_idx], NULL, NULL);

    // save to LRRP report for mapping/logging
    gps_write_lrrp_compact(opts, state, slot_idx, src, latitude, longitude, 0, (int)heading);
}

//externalize embedded GPS - Confirmed working now on NE, NW, SE, and SW coordinates
/* The embedded GPS LC's attribution (spec 2.2): the slot's ACTIVE call, and whether a checked full LC of that call's
 * transmission (a voice LC header or a late-entry LC, not a RAS acceptance) named its source. */
static int
dmr_embedded_gps_call(const dsd_state* state, uint8_t slot, dsd_call_snapshot* call, int* source_verified) {
    *source_verified = 0;
    if (dsd_call_state_get(state, slot, call) <= 0 || call->phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    *source_verified = call->ota_source_id != 0U && call->ota_source_id <= UINT32_MAX
                       && state->dmr_lc_checked_transmission[slot] == dsd_call_snapshot_transmission(call)
                       && state->dmr_lc_checked_source[slot] == (uint32_t)call->ota_source_id;
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
    int source_verified = 0;
    a->has_call = dmr_embedded_gps_call(state, slot, &a->call, &source_verified);
    a->fix.protocol = (uint8_t)(a->has_call ? a->call.protocol : state->lastsynctype);
    a->fix.source_id = source_verified ? (uint32_t)a->call.ota_source_id : 0U;
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
        (void)dsd_event_enrich_position(state, slot, a->call.epoch, &a->fix, state->dmr_embedded_gps[slot]);
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

    if (pf) {
        DSD_FPRINTF(stderr, " Protected");
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    const uint8_t slot = (uint8_t)(state->currentslot & 1);
    // The caller ran the embedded LC checksum; a burst scope that failed (a mono-collapsed slot's) still withholds it.
    const uint8_t crc_flags = state->event_crc_invalid[slot];
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
    gps_print_position_error(pos_err, (unsigned int)a.fix.err_m);
    if (admitted) {
        dmr_embedded_gps_publish(opts, state, slot, &a, pos_err);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

//P25 Motorola APX GPS format (sign + magnitude encoding)
void
apx_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]) {

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " GPS:");
    uint8_t slot = state->currentslot;
    uint8_t pf = lc_bits[0];
    uint8_t res_a = lc_bits[1];
    uint8_t res_b = (uint8_t)convert_bits_into_output(&lc_bits[16], 7);
    uint8_t expired = lc_bits[23]; //this bit seems to indicate that the GPS coordinates are out of date or fresh

    const char* deg_glyph = dsd_degrees_glyph();

    uint32_t lon_sign = lc_bits[48];
    uint32_t lon = (uint32_t)convert_bits_into_output(&lc_bits[49], 23);
    uint32_t lat_sign = lc_bits[24];
    uint32_t lat = (uint32_t)convert_bits_into_output(&lc_bits[25], 23);

    double lat_unit = 90.0 / 0x7FFFFF;
    double lon_unit = 180.0 / 0x7FFFFF;

    char latstr[3];
    char lonstr[3];
    char valid[12];
    DSD_SNPRINTF(latstr, sizeof latstr, "%s", "N");
    DSD_SNPRINTF(lonstr, sizeof lonstr, "%s", "E");
    DSD_SNPRINTF(valid, sizeof valid, "%s", "Current Fix");

    if (pf) {
        DSD_FPRINTF(stderr, " Protected");
    } else {
        double latitude;
        double longitude;
        //Latitude: value encodes 0-90 degrees, sign indicates hemisphere
        latitude = ((double)lat * lat_unit);
        if (lat_sign) {
            latitude = -latitude;
            DSD_SNPRINTF(latstr, sizeof latstr, "%s", "S");
        }

        //Longitude: value encodes 0-180 degrees, sign indicates hemisphere via offset
        longitude = ((double)lon * lon_unit);
        if (lon_sign) {
            longitude -= 180.0;
            DSD_SNPRINTF(lonstr, sizeof lonstr, "%s", "W");
        }

        //sanity check
        if (fabs(latitude) <= 90.0 && fabs(longitude) <= 180.0) {
            DSD_FPRINTF(stderr, " Lat: %.5lf%s%s Lon: %.5lf%s%s (%.5lf, %.5lf) ", latitude, deg_glyph, latstr,
                        longitude, deg_glyph, lonstr, latitude, longitude);

            if (expired) {
                DSD_FPRINTF(stderr, "Last Fix; ");
                DSD_SNPRINTF(valid, sizeof valid, "%s", "Last Fix");
            } else {
                DSD_FPRINTF(stderr, "Current Fix; ");
            }

            if (res_a) {
                DSD_FPRINTF(stderr, "RES_A: %d; ", res_a);
            }

            if (res_b) {
                DSD_FPRINTF(stderr, "RES_B: %02X; ", res_b);
            }

            //save to array for ncurses (guard slot index)
            uint8_t slot_idx = (slot >= 2) ? 1 : slot;
            DSD_SNPRINTF(state->dmr_embedded_gps[slot_idx], sizeof state->dmr_embedded_gps[slot_idx],
                         "GPS: %lf%s%s %lf%s%s (%lf, %lf) %s", latitude, deg_glyph, latstr, longitude, deg_glyph,
                         lonstr, latitude, longitude, valid);

            dsd_call_snapshot call;
            uint32_t src = 0U;
            if (gps_enrich_active_call(state, slot_idx, 0U, state->dmr_embedded_gps[slot_idx], NULL, &call) > 0
                && call.ota_source_id <= UINT32_MAX) {
                src = (uint32_t)call.ota_source_id;
            }

            //save to LRRP report for mapping/logging
            gps_write_lrrp_compact(opts, state, slot_idx, src, latitude, longitude, 0, 0);
        }
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
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

uint8_t
nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot, int len_bytes) {
    if (opts == NULL || state == NULL || input == NULL || len_bytes <= 0) {
        return 0U;
    }

    uint8_t slot_idx = (slot >= 2U) ? 1U : slot;
    int have_events = (state->event_history_s != NULL);
    uint8_t start_value = (uint8_t)convert_bits_into_output(input, 8);
    uint8_t end_value = 0U;
    uint8_t checksum_calc = 0U;
    uint8_t checksum_ext = 0U;
    uint8_t valid = nmea_validate_checksum(input, len_bytes, &end_value, &checksum_calc, &checksum_ext);

    char local_out[256];
    DSD_MEMSET(local_out, 0, sizeof(local_out));
    if (valid) {
        nmea_copy_printable_sentence(input, len_bytes, local_out, sizeof(local_out));

        if (local_out[0] != '\0') {
            DSD_FPRINTF(stderr, "%s", local_out);
        }
    } else {
        nmea_print_invalid_reason(start_value, end_value, checksum_calc, checksum_ext);
    }

    if (have_events) {
        dsd_event_stage_text(state, slot_idx, local_out);

        if (valid) {
            uint32_t source = (uint32_t)state->dmr_lrrp_source[slot_idx];
            uint32_t target = (uint32_t)state->dmr_lrrp_target[slot_idx];
            char comp_string[128];
            DSD_SNPRINTF(comp_string, sizeof(comp_string), "NMEA SRC: %u; TGT: %u;", source, target);
            const dsd_call_observation observation =
                dsd_call_observation_data(state->lastsynctype, slot_idx, source, target);
            (void)dsd_event_emit_data_notice((dsd_opts*)opts, state, slot_idx, &observation, comp_string);
        }
    }

    state->dmr_lrrp_source[slot_idx] = 0U;
    state->dmr_lrrp_target[slot_idx] = 0U;
    return valid;
}

void
nxdn_gps_report(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src) {
    if (opts == NULL || state == NULL || input == NULL) {
        return;
    }
    int have_events = (state->event_history_s != NULL);

    const char* deg_glyph = dsd_degrees_glyph();

    int16_t elevation = (int16_t)convert_bits_into_output(input + 56, 16);
    uint16_t speed_raw = (uint16_t)convert_bits_into_output(input + 74, 14);
    uint16_t heading_raw = (uint16_t)convert_bits_into_output(input + 92, 12);
    double speed_kph = (double)speed_raw / 10.0;
    double heading = (double)heading_raw / 10.0;

    uint16_t year = (uint16_t)(convert_bits_into_output(input + 136, 7) + 2000U);
    uint8_t month = (uint8_t)convert_bits_into_output(input + 143, 4);
    uint8_t day = (uint8_t)(convert_bits_into_output(input + 147, 5) + 1U);
    uint8_t hour = (uint8_t)convert_bits_into_output(input + 247, 5);
    uint8_t minute = (uint8_t)convert_bits_into_output(input + 252, 6);

    uint16_t lon_degmin = (uint16_t)convert_bits_into_output(input + 152, 16);
    uint16_t lon_frac = (uint16_t)convert_bits_into_output(input + 16, 15);
    uint8_t lon_hem = (uint8_t)convert_bits_into_output(input + 183, 1);
    double lon_minutes = (double)(lon_degmin % 100U) + ((double)lon_frac / 10000.0);
    double lon_decimal = ((double)lon_degmin / 100.0) + (lon_minutes / 60.0);
    double longitude = (lon_hem == 0U) ? lon_decimal : -lon_decimal;

    uint16_t lat_degmin = (uint16_t)convert_bits_into_output(input + 184, 16);
    uint16_t lat_frac = (uint16_t)convert_bits_into_output(input + 200, 15);
    uint8_t lat_hem = (uint8_t)convert_bits_into_output(input + 215, 1);
    double lat_minutes = (double)(lat_degmin % 100U) + ((double)lat_frac / 10000.0);
    double lat_decimal = ((double)lat_degmin / 100.0) + (lat_minutes / 60.0);
    double latitude = (lat_hem == 0U) ? lat_decimal : -lat_decimal;

    if (fabs(latitude) > 90.0 || fabs(longitude) > 180.0) {
        DSD_FPRINTF(stderr, " GPS: Invalid NXDN position report;");
        state->dmr_lrrp_source[0] = 0U;
        state->dmr_lrrp_target[0] = 0U;
        return;
    }

    DSD_FPRINTF(stderr, "\n GPS: (%.6f%s, %.6f%s) ", latitude, deg_glyph, longitude, deg_glyph);
    DSD_FPRINTF(stderr, "Speed: %.1f k/h; ", speed_kph);
    DSD_FPRINTF(stderr, "COG: %.1f; ", heading);
    DSD_FPRINTF(stderr, "Elevation: %d; ", (int)elevation);
    DSD_FPRINTF(stderr, "Date: %04u/%02u/%02u; ", (unsigned)year, (unsigned)month, (unsigned)day);
    DSD_FPRINTF(stderr, "Time: %02u:%02u;", (unsigned)hour, (unsigned)minute);

    if (have_events) {
        char gps_string[256];
        DSD_SNPRINTF(gps_string, sizeof(gps_string), "(%.6f%s, %.6f%s)", latitude, deg_glyph, longitude, deg_glyph);
        (void)gps_enrich_active_call(state, 0U, src, gps_string, NULL, NULL);

        uint32_t source = (uint32_t)state->dmr_lrrp_source[0];
        uint32_t target = (uint32_t)state->dmr_lrrp_target[0];
        char comp_string[128];
        DSD_SNPRINTF(comp_string, sizeof(comp_string), "GPS SRC: %u; TGT: %u;", source, target);
        const dsd_call_observation observation = dsd_call_observation_data(state->lastsynctype, 0U, source, target);
        (void)dsd_event_emit_data_notice_with_gps((dsd_opts*)opts, state, 0U, &observation, comp_string, gps_string);
    }

    if (src != 0U) {
        gps_write_lrrp_slash_colon(opts, state, 0, src, latitude, longitude, (int)speed_kph, (int)heading);
    }

    state->dmr_lrrp_source[0] = 0U;
    state->dmr_lrrp_target[0] = 0U;
}
