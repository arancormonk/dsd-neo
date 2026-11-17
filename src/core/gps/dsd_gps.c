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
 * epoch the report was bound to, or else the slot's ACTIVE call. The writer's typed-source rule decides. */
static void
gps_report_write_history(dsd_state* state, uint8_t slot, const dsd_gps_report_ctx* ctx, const dsd_position_fix* fix,
                         const char* text) {
    if (ctx->closing != NULL && ctx->closing->valid) {
        (void)dsd_event_enrich_position_closing(state, ctx->closing, fix, text);
    } else if (ctx->call_epoch != 0U) {
        (void)dsd_event_enrich_position(state, slot, ctx->call_epoch, fix, text);
    } else {
        (void)gps_enrich_active_call(state, slot, 0U, text, fix, NULL);
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
    gps_report_write_history(state, slot, ctx, fix, state->dmr_embedded_gps[slot]);

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
    gps_report_write_history(state, slot, ctx, fix, state->dmr_embedded_gps[slot]);
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
