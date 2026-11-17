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
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/state.h>
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

static void
gps_write_lrrp_compact(const dsd_opts* opts, const dsd_state* state, int slot, uint32_t src, double latitude,
                       double longitude, int speed_kph, int azimuth) {
    if (!gps_lrrp_row_allowed(opts, state, slot)) {
        return;
    }

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

static float
lip_velocity_kph(uint8_t hor_vel) {
    if (hor_vel <= 28U) {
        return (float)hor_vel;
    }

    // Precomputed LUT for v = 16 * (1.038)^(K-13), K in [0,127]
    static int lut_init = 0;
    static float v_lut[128];
    if (!lut_init) {
        const float factor = 1.038f;
        DSD_MEMSET(v_lut, 0, sizeof(v_lut));
        v_lut[13] = 16.0f; // (K-13)==0
        for (int k = 14; k < 128; k++) {
            v_lut[k] = v_lut[k - 1] * factor;
        }
        lut_init = 1;
    }
    uint8_t lut_idx = (hor_vel < 128U) ? hor_vel : 127U;
    return v_lut[lut_idx];
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

typedef struct {
    uint8_t time_elapsed;
    uint8_t lon_sign;
    uint32_t lon;
    uint8_t lat_sign;
    uint32_t lat;
    uint8_t pos_err;
    uint8_t hor_vel;
    uint8_t dir_tra;
    uint8_t reason;
    uint8_t reason_is_user_data;
} lip_fields;

static void
lip_fields_from_usbd(const uint8_t* input, lip_fields* f) {
    f->time_elapsed = (uint8_t)convert_bits_into_output(input + 4, 2);
    f->lon_sign = input[6];
    f->lon = (uint32_t)convert_bits_into_output(input + 7, 24);
    f->lat_sign = input[31];
    f->lat = (uint32_t)convert_bits_into_output(input + 32, 23);
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
    f->lon_sign = bits[4];
    f->lon = (uint32_t)convert_bits_into_output(bits + 5, 24);
    f->lat_sign = bits[29];
    f->lat = (uint32_t)convert_bits_into_output(bits + 30, 23);
    f->pos_err = (uint8_t)convert_bits_into_output(bits + 53, 3);
    f->hor_vel = (uint8_t)convert_bits_into_output(bits + 56, 7);
    f->dir_tra = (uint8_t)convert_bits_into_output(bits + 63, 4);
    f->reason_is_user_data = bits[67];
    f->reason = (uint8_t)convert_bits_into_output(bits + 68, 8);
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
lip_print_reason(const lip_fields* f, int hash_prefix) {
    if (hash_prefix >= 0) {
        // TS 102 361-4 Table 6.80 compresses LIP reason 32 to zero; other codes are reserved.
        DSD_FPRINTF(stderr, " Reason: %s (%u);", f->reason == 0 ? lip_reason_label(32) : "Reserved", f->reason);
    } else if (f->reason_is_user_data) {
        DSD_FPRINTF(stderr, " User data: 0x%02X;", f->reason);
    } else {
        DSD_FPRINTF(stderr, " Reason: %s (%u);", lip_reason_label(f->reason), f->reason);
    }
}

typedef struct {
    int hash_prefix;
    double latitude;
    double longitude;
    unsigned int position_error;
    uint8_t pos_err;
    int speed_kph;
    int direction_deg;
    const char* deg_glyph;
    const char* latstr;
    const char* lonstr;
} lip_state_strings;

static void
lip_store_state_strings(dsd_state* state, int slot, const lip_state_strings* gps) {
    if (!state || !gps) {
        return;
    }
    char prefix[16] = "";
    if (gps->hash_prefix >= 0) {
        DSD_SNPRINTF(prefix, sizeof prefix, "%03d; ", gps->hash_prefix);
    }
    if (gps->pos_err <= 5U) {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                     "%sLIP: %.5lf%s%s %.5lf%s%s; Err: %dm; Spd: %d km/h; Dir: %d%s", prefix, gps->latitude,
                     gps->deg_glyph, gps->latstr, gps->longitude, gps->deg_glyph, gps->lonstr, gps->position_error,
                     gps->speed_kph, gps->direction_deg, gps->deg_glyph);
    } else if (gps->pos_err == 6U) {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                     "%sLIP: %.5lf%s%s %.5lf%s%s; Err: >200km; Spd: %d km/h; Dir: %d%s", prefix, gps->latitude,
                     gps->deg_glyph, gps->latstr, gps->longitude, gps->deg_glyph, gps->lonstr, gps->speed_kph,
                     gps->direction_deg, gps->deg_glyph);
    } else {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                     "%sLIP: %.5lf%s%s %.5lf%s%s Unknown Pos Err; Spd: %d km/h; Dir %d%s", prefix, gps->latitude,
                     gps->deg_glyph, gps->latstr, gps->longitude, gps->deg_glyph, gps->lonstr, gps->speed_kph,
                     gps->direction_deg, gps->deg_glyph);
    }
}

static void
lip_report(const dsd_opts* opts, dsd_state* state, const lip_fields* f, uint32_t src, int hash_prefix,
           uint32_t expected_source) {
    DSD_FPRINTF(stderr, "Location Information Protocol; ");
    if (hash_prefix < 0) {
        DSD_FPRINTF(stderr, "Short Location Report; ");
    }
    uint32_t lat = f->lat;
    uint32_t lon = f->lon;
    double lat_unit = 180.0 / 16777216.0;
    double lon_unit = 360.0 / 33554432.0;
    double lon_sf = 1.0f;
    double lat_sf = 1.0f;
    const char* latstr = "N";
    const char* lonstr = "E";
    const char* deg_glyph = dsd_degrees_glyph();
    if (f->lat_sign) {
        lat = 0x800000 - lat;
        latstr = "S";
        lat_sf = -1.0f;
    }
    double latitude = (double)lat * lat_unit;
    if (f->lon_sign) {
        lon = 0x1000000 - lon;
        lonstr = "W";
        lon_sf = -1.0f;
    }
    double longitude = (double)lon * lon_unit;
    float v = lip_velocity_kph(f->hor_vel);
    float dir = (float)f->dir_tra * 22.5f; // TS 100 392-18-1 Table 6.45.
    int vt = (int)v;
    int dt = (int)dir;
    if (fabs(latitude) <= 90.0 && fabs(longitude) <= 180.0) {
        int slot = state->currentslot & 1;
        if (hash_prefix >= 0) {
            DSD_FPRINTF(stderr, "Src(Hash); %03d;  ", hash_prefix);
        } else {
            DSD_FPRINTF(stderr, "Src: %u; ", src);
        }
        DSD_FPRINTF(stderr, "Lat: %.5lf%s%s Lon: %.5lf%s%s (%.5lf, %.5lf); Spd: %d km/h; Dir: %d%s", latitude,
                    deg_glyph, latstr, longitude, deg_glyph, lonstr, lat_sf * latitude, lon_sf * longitude, vt, dt,
                    deg_glyph);
        unsigned int position_error = f->pos_err <= 5U ? (unsigned int)(2U * POSITION_ERROR_POW10[f->pos_err]) : 0U;
        lip_state_strings gps = {hash_prefix, latitude, longitude, position_error, f->pos_err,
                                 vt,          dt,       deg_glyph, latstr,         lonstr};
        gps_print_position_error(f->pos_err, position_error);
        DSD_FPRINTF(stderr, ";");
        lip_print_reason(f, hash_prefix);
        lip_print_time_elapsed(f->time_elapsed);
        lip_store_state_strings(state, slot, &gps);
        // Carrier reports need a known source before enriching an active call.
        // USBD hashes retain the existing wildcard-source behavior.
        if (hash_prefix >= 0 || expected_source != 0U) {
            (void)gps_enrich_active_call(state, (uint8_t)slot, expected_source, state->dmr_embedded_gps[slot], NULL,
                                         NULL);
        }
        gps_write_lrrp_compact(opts, state, slot, src, lat_sf * latitude, lon_sf * longitude, vt, dt);
    } else {
        DSD_FPRINTF(stderr, " Position Calculation Error;");
    }
}

void
lip_protocol_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* input) {
    // ETSI TS 102 361-4 V1.12.1 Table 6.79: service 0..3, time 4..5, longitude 6..30,
    // latitude 31..54, position error 55..57, velocity 58..64, direction 65..68,
    // reason 69..71 and source hash 72..79. The 16-bit burst CRC follows at bit 80.
    lip_fields f;
    lip_fields_from_usbd(input, &f);
    uint8_t add_hash = (uint8_t)convert_bits_into_output(input + 72, 8);
    lip_report(opts, state, &f, add_hash, add_hash, 0U);
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
        if (bit_count < 76) {
            DSD_FPRINTF(stderr, "LIP: short location report truncated (%zu bits);", bit_count);
            return;
        }
        lip_fields f;
        lip_fields_from_short_report(bits, &f);
        lip_report(opts, state, &f, src, -1, src);
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

static void DSD_ATTR_USED
nmea_store_and_report(const dsd_opts* opts, dsd_state* state, int slot, uint32_t src, float latitude, float longitude,
                      float speed_kph, uint16_t cog, int type, const char* deg_glyph) {
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "GPS: (%f%s, %f%s)", latitude,
                 deg_glyph, longitude, deg_glyph);
    (void)gps_enrich_active_call(state, (uint8_t)slot, src, state->dmr_embedded_gps[slot], NULL, NULL);

    int speed_int = (int)speed_kph;
    int azimuth = (type == 2) ? (int)cog : 0;
    gps_write_lrrp_compact(opts, state, slot, src, latitude, longitude, speed_int, azimuth);
}

void
nmea_iec_61162_1(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint32_t src, int type) {
    int slot = state->currentslot;

    //NOTE: The Only difference between Short (type == 1) and Long Format (type == 2)
    //is the utc_ss3 on short vs utc_ss6 and inclusion of COG value on long

    //NOTE: MFID Specific Formats are not handled here, unknown, could be added if worked out
    //they could share most of the same elements and use the large spare bits in block 2 for extra
    // uint8_t mfid = (uint8_t)convert_bits_into_output(&input[88], 8); //on type 3, last octet of first block carries MFID

    // uint8_t nmea_c = input[0];  //encrypted -- checked before we get here
    uint8_t nmea_ns = input[1]; //north/south (lat sign)
    uint8_t nmea_ew = input[2]; //east/west (lon sign)
    uint8_t nmea_q = input[3];  //Quality Indicator (no fix or fix valid)
    uint8_t nmea_speed =
        (uint8_t)convert_bits_into_output(&input[4], 7); //speed in knots (127 = greater than 126 knots)

    //Latitude Bits
    uint8_t nmea_ndeg = (uint8_t)convert_bits_into_output(&input[11], 7);     //Latitude Degrees
    uint8_t nmea_nmin = (uint8_t)convert_bits_into_output(&input[18], 6);     //Latitude Minutes
    uint16_t nmea_nminf = (uint16_t)convert_bits_into_output(&input[24], 14); //Latitude Fractions of Minutes

    //Longitude Bits
    uint8_t nmea_edeg = (uint8_t)convert_bits_into_output(&input[38], 8);     //Longitude Degrees
    uint8_t nmea_emin = (uint8_t)convert_bits_into_output(&input[46], 6);     //Longitude Minutes
    uint16_t nmea_eminf = (uint16_t)convert_bits_into_output(&input[52], 14); //Longitude Fractions of Minutes

    //UTC Time and COG
    uint8_t nmea_utc_hh = (uint8_t)convert_bits_into_output(&input[66], 5);
    uint8_t nmea_utc_mm = (uint8_t)convert_bits_into_output(&input[71], 6);
    //seconds and the addition of COG is the difference between short and long formats
    uint8_t nmea_utc_ss3 = (uint8_t)convert_bits_into_output(&input[77], 3) * 10; //seconds in 10s
    uint8_t nmea_utc_ss6 = (uint8_t)convert_bits_into_output(&input[77], 6);      //seconds in 1s
    uint16_t nmea_cog = (uint16_t)convert_bits_into_output(&input[103], 9);       //course over ground in degrees

    //lat and lon conversion
    const char* deg_glyph = dsd_degrees_glyph();
    float latitude = 0.0f;
    float longitude = 0.0f;
    // float m_unit = 1.0f / 60.0f;     //unit to convert min into decimal value - (1/60)*60 minutes = 1 degree
    // float mm_unit = 1.0f / 10000.0f;  //unit to convert minf into decimal value - (0000 - 9999) 0.0001×9999 = .9999 minutes, so its sub 1 minute decimal

    //testing w/ Harris NMEA like values (ran tests over there with this code, this seems to work on those values)
    float m_unit = 1.0f / 60.0f; //unit to convert min into decimal value - (1/60)*60 minutes = 1 degree
    float mm_unit =
        1.0f
        / 600000.0f; //unit to convert minf into decimal value - (0000 - 9999) 0.0001×9999 = .9999 minutes, so its sub 1 minute decimal

    //speed conversion
    float fmps, fmph, fkph = 0.0f; //conversion of knots to mps, kph, and mph values
    fmps = (float)nmea_speed * 0.514444;
    UNUSED(fmps);
    fmph = (float)nmea_speed * 1.15078f;
    UNUSED(fmph);
    fkph = (float)nmea_speed * 1.852f;

    //calculate decimal representation of latidude and longitude (need some samples to test)
    latitude = ((float)nmea_ndeg + ((float)nmea_nmin * m_unit) + ((float)nmea_nminf * mm_unit));
    longitude = ((float)nmea_edeg + ((float)nmea_emin * m_unit) + ((float)nmea_eminf * mm_unit));

    if (!nmea_ns) {
        latitude *= -1.0f; //0 is South, 1 is North
    }
    if (!nmea_ew) {
        longitude *= -1.0f; //0 is West, 1 is East
    }

    DSD_FPRINTF(stderr, " GPS: %f%s, %f%s;", latitude, deg_glyph, longitude, deg_glyph);

    //Speed in Knots
    if (nmea_speed > 126) {
        DSD_FPRINTF(stderr, " SPD > 126 knots or %f kph;", fkph);
    } else {
        DSD_FPRINTF(stderr, " SPD: %d knots; %f kph;", nmea_speed, fkph);
    }

    //Print UTC Time and COG according to Format Type
    switch (type) {
        case 1:
            DSD_FPRINTF(stderr, " FIX: %d; %02d:%02d:%02d UTC; Short Format;", nmea_q, nmea_utc_hh, nmea_utc_mm,
                        nmea_utc_ss3);
            break;
        case 2:
            DSD_FPRINTF(stderr, " FIX: %d; %02d:%02d:%02d UTC; COG: %d%s; Long Format;", nmea_q, nmea_utc_hh,
                        nmea_utc_mm, nmea_utc_ss6, nmea_cog, deg_glyph);
            break;
        default: break;
    }

    nmea_store_and_report(opts, state, slot, src, latitude, longitude, fkph, nmea_cog, type, deg_glyph);
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
typedef struct {
    double latitude;
    double longitude;
    double lat_sf;
    double lon_sf;
    unsigned int position_error;
    uint8_t pos_err;
    const char* deg_glyph;
    const char* latstr;
    const char* lonstr;
} dmr_embedded_gps_fix;

static void
dmr_embedded_gps_store_clear_fix(const dsd_opts* opts, dsd_state* state, uint8_t slot,
                                 const dmr_embedded_gps_fix* fix) {
    if (fix->pos_err <= 0x5) {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                     "GPS: %.5lf%s%s %.5lf%s%s Err: %dm", fix->latitude, fix->deg_glyph, fix->latstr, fix->longitude,
                     fix->deg_glyph, fix->lonstr, fix->position_error);
    } else if (fix->pos_err == 0x6) {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                     "GPS: %.5lf%s%s %.5lf%s%s Err: >200km", fix->latitude, fix->deg_glyph, fix->latstr, fix->longitude,
                     fix->deg_glyph, fix->lonstr);
    } else {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                     "GPS: %.5lf%s%s %.5lf%s%s Unknown Pos Err", fix->latitude, fix->deg_glyph, fix->latstr,
                     fix->longitude, fix->deg_glyph, fix->lonstr);
    }

    dsd_call_snapshot call;
    uint32_t src = 0U;
    if (gps_enrich_active_call(state, slot, 0U, state->dmr_embedded_gps[slot], NULL, &call) > 0
        && call.ota_source_id <= UINT32_MAX) {
        src = (uint32_t)call.ota_source_id;
    }
    gps_write_lrrp_compact(opts, state, slot, src, fix->lat_sf * fix->latitude, fix->lon_sf * fix->longitude, 0, 0);
}

void
dmr_embedded_gps(const dsd_opts* opts, dsd_state* state, const uint8_t lc_bits[]) {
    UNUSED(opts);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Embedded GPS:");
    uint8_t pf = lc_bits[0];
    uint8_t res_a = lc_bits[1];
    uint8_t res_b = (uint8_t)convert_bits_into_output(&lc_bits[16], 4);
    uint8_t pos_err = (uint8_t)convert_bits_into_output(&lc_bits[20], 3);
    UNUSED2(res_a, res_b);

    const char* deg_glyph = dsd_degrees_glyph();

    uint32_t lon_sign = lc_bits[23];
    uint32_t lon = (uint32_t)convert_bits_into_output(&lc_bits[24], 24);
    uint32_t lat_sign = lc_bits[48];
    uint32_t lat = (uint32_t)convert_bits_into_output(&lc_bits[49], 23);

    double lat_unit = 180.0 / 16777216.0; // 180 / (2^24)
    double lon_unit = 360.0 / 33554432.0; // 360 / (2^25)

    char latstr[3];
    char lonstr[3];
    DSD_SNPRINTF(latstr, sizeof latstr, "%s", "N");
    DSD_SNPRINTF(lonstr, sizeof lonstr, "%s", "E");

    //run calculations and print
    //7.2.16 and 7.2.17 (two's compliment)

    if (pf) {
        DSD_FPRINTF(stderr, " Protected");
    } else {
        double lon_sf = 1.0; //value multiplied against longitude for signed output
        double lat_sf = 1.0; //value multiplied against latitude for signed output
        double latitude;
        double longitude;
        //two's complement conversion (ETSI TS 102 361-2 7.2.16/7.2.17)
        if (lat_sign) {
            lat = 0x800000 - lat;
            DSD_SNPRINTF(latstr, sizeof latstr, "%s", "S");
            lat_sf = -1.0f;
        }
        latitude = ((double)lat * lat_unit);

        if (lon_sign) {
            lon = 0x1000000 - lon;
            DSD_SNPRINTF(lonstr, sizeof lonstr, "%s", "W");
            lon_sf = -1.0f;
        }
        longitude = ((double)lon * lon_unit);

        //sanity check
        if (fabs(latitude) <= 90.0 && fabs(longitude) <= 180.0) {
            DSD_FPRINTF(stderr, " Lat: %.5lf%s%s Lon: %.5lf%s%s (%.5lf, %.5lf)", latitude, deg_glyph, latstr, longitude,
                        deg_glyph, lonstr, lat_sf * latitude, lon_sf * longitude);

            //7.2.15 Position Error: 2 * 10^pos_err via LUT
            unsigned int position_error = 0;
            if (pos_err <= 0x5) {
                position_error = (unsigned int)(2U * POSITION_ERROR_POW10[pos_err]);
            }
            gps_print_position_error(pos_err, position_error);

            dmr_embedded_gps_fix fix = {latitude, longitude, lat_sf, lon_sf, position_error,
                                        pos_err,  deg_glyph, latstr, lonstr};
            uint8_t slot = state->currentslot;
            dmr_embedded_gps_store_clear_fix(opts, state, slot, &fix);
        }
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

/* ---- ASCII NMEA 0183 (P25 SAP 48, NXDN data call format 0x06) ---- */

enum {
    NMEA_PAYLOAD_MAX_BYTES = 1024, /* octets of a payload read for sentences */
    NMEA_TEXT_MAX = 256,           /* the checked sentences kept as text */
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
    nmea_parse_fix((const char*)bytes + s->start, s->len, &f);
    if (f.type == NMEA_TYPE_OTHER) {
        return;
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

/* Admit the packet's report: the position and the packet's source (the slot's dmr_lrrp_source) only from a packet
 * whose own check passed (both verdicts are the packet's). */
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
    const dsd_position_admit_result r = dsd_position_admit(opts, state, fix, packet_verified, packet_verified, origin);
    if (r != DSD_POSITION_REFUSED) {
        nmea_print_fix(fix);
    } else if (packet_verified == 1) {
        DSD_FPRINTF(stderr, "\n NMEA: position not admitted;");
    }
    return r;
}

/* History and -L: the checked sentences' text (none from a packet whose check failed), the data notice, and with an
 * admitted report its position (staged just before the notice) and its -L row. */
static void
nmea_publish(const dsd_opts* opts, dsd_state* state, uint8_t slot, const nmea_packet* pkt, int packet_verified,
             dsd_position_admit_result result, const dsd_position_fix* fix) {
    const int admitted = result != DSD_POSITION_REFUSED;
    if (state->event_history_s != NULL) {
        const char* text = (pkt->checked > 0 && packet_verified != 1) ? "NMEA withheld (CRC ERR)" : pkt->text;
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

uint8_t
nmea_sentence_checker(const dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t slot, int len_bytes,
                      int packet_verified, const dsd_position_origin* origin) {
    if (opts == NULL || state == NULL || input == NULL || len_bytes <= 0) {
        return 0U;
    }
    const uint8_t slot_idx = (slot >= 2U) ? 1U : slot;
    uint8_t bytes[NMEA_PAYLOAD_MAX_BYTES];
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    const size_t n = ((size_t)len_bytes < sizeof(bytes)) ? (size_t)len_bytes : sizeof(bytes);
    for (size_t i = 0U; i < n; i++) {
        bytes[i] = (uint8_t)convert_bits_into_output(input + (i * 8U), 8);
    }

    nmea_packet pkt;
    DSD_MEMSET(&pkt, 0, sizeof(pkt));
    nmea_packet_scan(bytes, n, &pkt);
    if (pkt.checked > 0 && packet_verified == 1) {
        DSD_FPRINTF(stderr, "%s", pkt.text);
    } else if (pkt.checked > 0) {
        DSD_FPRINTF(stderr, " NMEA Withheld (CRC ERR);");
    }

    dsd_position_fix fix;
    dsd_position_fix_init(&fix);
    const dsd_position_admit_result result =
        (pkt.checked > 0) ? nmea_admit_packet(opts, state, slot_idx, &pkt, packet_verified, origin, &fix)
                          : DSD_POSITION_REFUSED;
    nmea_publish(opts, state, slot_idx, &pkt, packet_verified, result, &fix);

    state->dmr_lrrp_source[slot_idx] = 0U;
    state->dmr_lrrp_target[slot_idx] = 0U;
    return (uint8_t)(pkt.checked > 0);
}

/* ---- NXDN GPS ---- */

/* The NXDN GPS report has no official source: its layout is SDRTrunk's (nxdn/layer3/data/GPS.java), whose offsets count
 * from the 0xFFFC marker, so they are 16 more than these: elevation 56 (16 bits, unit unknown), speed 74 (14, 0.1 km/h),
 * heading 92 (12, 0.1 degree; 0xFFF none), date 136 (7, year - 2000) / 143 (4, month) / 147 (5, day - 1), longitude
 * 152 (16, DDDMM) / 183 (hemisphere), latitude 184 (16, DDMM) / 200 (15, 1/10000 minute) / 215 (hemisphere), time 247
 * (5, hour) / 252 (6, minute) / 258 (6, second). The longitude's fraction is not read: its offset rests on that one
 * source (a decided limit), so the longitude keeps whole minutes. */
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

static void
nxdn_gps_publish(const dsd_opts* opts, dsd_state* state, const dsd_position_fix* fix) {
    if (state->event_history_s != NULL) {
        char gps_string[96];
        gps_format_position_text(fix, gps_string, sizeof(gps_string));
        (void)gps_enrich_active_call(state, 0U, fix->source_id, gps_string, fix, NULL);

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
