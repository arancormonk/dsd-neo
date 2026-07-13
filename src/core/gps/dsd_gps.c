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
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/protocol/pdu.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/unicode.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static const uint32_t POSITION_ERROR_POW10[8] = {
    1U, 10U, 100U, 1000U, 10000U, 100000U, 1000000U, 10000000U,
};

static void
gps_write_lrrp_compact(const dsd_opts* opts, uint32_t src, double latitude, double longitude, int speed_kph,
                       int azimuth) {
    if (opts == NULL || opts->lrrp_file_output != 1) {
        return;
    }

    char datestr[9];
    char timestr[7];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_DATE_COMPACT, datestr, sizeof datestr);
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COMPACT, timestr, sizeof timestr);

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
gps_write_lrrp_slash_colon(const dsd_opts* opts, uint32_t src, double latitude, double longitude, int speed_kph,
                           int azimuth) {
    if (opts == NULL || opts->lrrp_file_output != 1) {
        return;
    }

    char datestr[11];
    char timestr[9];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_DATE_SLASH, datestr, sizeof datestr);
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);

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
    uint8_t add_hash;
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

static void DSD_ATTR_USED
lip_store_state_strings(dsd_state* state, int slot, const lip_state_strings* gps) {
    if (!state || !gps) {
        return;
    }

    if (gps->pos_err != 0x7U) {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                     "%03d; LIP: %.5lf%s%s %.5lf%s%s; Err: %dm; Spd: %d km/h; Dir: %d%s", gps->add_hash, gps->latitude,
                     gps->deg_glyph, gps->latstr, gps->longitude, gps->deg_glyph, gps->lonstr, gps->position_error,
                     gps->speed_kph, gps->direction_deg, gps->deg_glyph);
    } else {
        DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot],
                     "%03d; LIP: %.5lf%s%s %.5lf%s%s Unknown Pos Err; Spd: %d km/h; Dir %d%s", gps->add_hash,
                     gps->latitude, gps->deg_glyph, gps->latstr, gps->longitude, gps->deg_glyph, gps->lonstr,
                     gps->speed_kph, gps->direction_deg, gps->deg_glyph);
    }

    DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].gps_s,
                 sizeof state->event_history_s[slot].Event_History_Items[0].gps_s, "%s", state->dmr_embedded_gps[slot]);
}

static void DSD_ATTR_USED
lip_emit_position_metadata(const dsd_opts* opts, dsd_state* state, int slot, const lip_state_strings* gps,
                           double lat_sf, double lon_sf, uint8_t reason, uint8_t time_elapsed) {
    if (gps->pos_err == 0x7U) {
        DSD_FPRINTF(stderr, "\n  Position Error: Unknown or Invalid;");
    } else {
        DSD_FPRINTF(stderr, "\n  Position Error: Less than %dm;", gps->position_error);
    }

    if (reason != 0U) {
        DSD_FPRINTF(stderr, " Reserved: %d;", reason);
    } else {
        DSD_FPRINTF(stderr, " Request Response; ");
    }

    lip_print_time_elapsed(time_elapsed);
    lip_store_state_strings(state, slot, gps);
    gps_write_lrrp_compact(opts, gps->add_hash, lat_sf * gps->latitude, lon_sf * gps->longitude, gps->speed_kph,
                           gps->direction_deg);
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
    //NOTE: This is defined in ETSI TS 102 361-4 V1.12.1 (2023-07) p208

    //NOTE: This format is pretty much the same as DMR EMB GPS, but has a few extra elements,
    //so I got lazy and just lifted most of the code from there, also assuming same lat/lon calcs
    //since those have been tested to work in DMR EMB GPS and units are same in LIP

    DSD_FPRINTF(stderr, "Location Information Protocol; ");

    // uint8_t service_type = (uint8_t)convert_bits_into_output(&input[0], 4); //checked before arrival here
    uint8_t time_elapsed = (uint8_t)convert_bits_into_output(&input[6], 2);
    uint8_t lon_sign = input[8];
    uint32_t lon = (uint32_t)convert_bits_into_output(&input[9], 24); //8, 25
    uint8_t lat_sign = input[33];
    uint32_t lat = (uint32_t)convert_bits_into_output(&input[34], 23); //33, 24
    uint8_t pos_err = (uint8_t)convert_bits_into_output(&input[57], 2);
    uint8_t hor_vel = (uint8_t)convert_bits_into_output(&input[59], 7);
    uint8_t dir_tra = (uint8_t)convert_bits_into_output(&input[66], 4);
    uint8_t reason = (uint8_t)convert_bits_into_output(&input[70], 3);
    uint8_t add_hash = (uint8_t)convert_bits_into_output(&input[73], 8); //MS Source Address Hash

    //NOTE: May need to use double instead of float to avoid rounding errors
    double latitude = 0.0;
    double longitude = 0.0;
    /* Avoid pow(2, n): use exact constants for 2^24 and 2^25 */
    double lat_unit = 180.0 / 16777216.0; // 180 / (2^24)
    double lon_unit = 360.0 / 33554432.0; // 360 / (2^25)
    double lon_sf = 1.0f;                 //float value we can multiple longitude with
    double lat_sf = 1.0f;                 //float value we can multiple latitude with

    char latstr[3];
    char lonstr[3];
    DSD_SNPRINTF(latstr, sizeof latstr, "%s", "N");
    DSD_SNPRINTF(lonstr, sizeof lonstr, "%s", "E");

    const char* deg_glyph = dsd_degrees_glyph();

    //lat and lon calculations (two's complement conversion)
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

    //6.3.63 Position Error
    //6.3.17 Horizontal velocity
    /*
    Horizontal velocity shall be encoded for speeds 0 km/h to 28 km/h in 1 km/h steps and
    from 28 km/h onwards using equation: v = C × (1 + x)^(K-A) + B where:
  */
    float v = lip_velocity_kph(hor_vel);

    float dir = (((float)dir_tra + 11.25f) / 22.5f); //page 68, Table 6.45

    //truncated and rounded forms
    int vt = (int)v;
    int dt = (int)dir;

    //sanity check
    if (fabs(latitude) <= 90.0 && fabs(longitude) <= 180.0) {
        int slot = state->currentslot;
        DSD_FPRINTF(stderr, "Src(Hash); %03d;  Lat: %.5lf%s%s Lon: %.5lf%s%s (%.5lf, %.5lf); Spd: %d km/h; Dir: %d%s",
                    add_hash, latitude, deg_glyph, latstr, longitude, deg_glyph, lonstr, lat_sf * latitude,
                    lon_sf * longitude, vt, dt, deg_glyph);

        //6.3.63 Position Error (2 * 10^pos_err) via tiny LUT
        unsigned int position_error = (unsigned int)(2U * POSITION_ERROR_POW10[pos_err & 7U]);
        lip_state_strings gps = {add_hash, latitude, longitude, position_error, pos_err,
                                 vt,       dt,       deg_glyph, latstr,         lonstr};
        lip_emit_position_metadata(opts, state, slot, &gps, lat_sf, lon_sf, reason, time_elapsed);
    } else {
        DSD_FPRINTF(stderr, " Position Calculation Error;");
    }
}

static void DSD_ATTR_USED
nmea_store_and_report(const dsd_opts* opts, dsd_state* state, int slot, uint32_t src, float latitude, float longitude,
                      float speed_kph, uint16_t cog, int type, const char* deg_glyph) {
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof state->dmr_embedded_gps[slot], "GPS: (%f%s, %f%s)", latitude,
                 deg_glyph, longitude, deg_glyph);
    DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].gps_s,
                 sizeof state->event_history_s[slot].Event_History_Items[0].gps_s, "(%f%s, %f%s)", latitude, deg_glyph,
                 longitude, deg_glyph);

    int speed_int = (int)speed_kph;
    int azimuth = (type == 2) ? (int)cog : 0;
    gps_write_lrrp_compact(opts, src, latitude, longitude, speed_int, azimuth);
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
    if (state->event_history_s[slot_idx].Event_History_Items[0].source_id == src && src != 0) {
        DSD_SNPRINTF(state->event_history_s[slot_idx].Event_History_Items[0].gps_s,
                     sizeof state->event_history_s[slot_idx].Event_History_Items[0].gps_s, "%s",
                     state->dmr_embedded_gps[slot_idx]);
    }

    // save to LRRP report for mapping/logging
    gps_write_lrrp_compact(opts, src, latitude, longitude, 0, (int)heading);
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

    uint32_t src = (slot == 0U) ? (uint32_t)state->lasttg : ((slot == 1U) ? (uint32_t)state->lasttgR : 0U);
    if (state->event_history_s[slot].Event_History_Items[0].source_id == src) {
        DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].gps_s,
                     sizeof state->event_history_s[slot].Event_History_Items[0].gps_s, "%s",
                     state->dmr_embedded_gps[slot]);
    }
    gps_write_lrrp_compact(opts, src, fix->lat_sf * fix->latitude, fix->lon_sf * fix->longitude, 0, 0);
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
            if (pos_err == 0x7) {
                DSD_FPRINTF(stderr, "\n  Position Error: Unknown or Invalid");
            } else if (pos_err == 0x6) {
                DSD_FPRINTF(stderr, "\n  Position Error: More than 200km");
            } else {
                DSD_FPRINTF(stderr, "\n  Position Error: Less than %dm", position_error);
            }

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

            uint32_t src = 0U;
            if (slot == 0U) {
                src = state->lastsrc;
            } else if (slot == 1U) {
                src = state->lastsrcR;
            }

            //save to array for ncurses (guard slot index)
            uint8_t slot_idx = (slot >= 2) ? 1 : slot;
            DSD_SNPRINTF(state->dmr_embedded_gps[slot_idx], sizeof state->dmr_embedded_gps[slot_idx],
                         "GPS: %lf%s%s %lf%s%s (%lf, %lf) %s", latitude, deg_glyph, latstr, longitude, deg_glyph,
                         lonstr, latitude, longitude, valid);

            //save to event history string
            if (state->event_history_s[slot_idx].Event_History_Items[0].source_id == src) {
                DSD_SNPRINTF(state->event_history_s[slot_idx].Event_History_Items[0].gps_s,
                             sizeof state->event_history_s[slot_idx].Event_History_Items[0].gps_s, "%s",
                             state->dmr_embedded_gps[slot_idx]);
            }

            //save to LRRP report for mapping/logging
            gps_write_lrrp_compact(opts, src, latitude, longitude, 0, 0);
        }
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

void
decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len) {
    //TODO: This
    UNUSED(opts);
    UNUSED(state);
    UNUSED(input);
    UNUSED(len);

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
    char* out = local_out;
    size_t out_cap = sizeof(local_out);
    if (have_events) {
        DSD_MEMSET(state->event_history_s[slot_idx].Event_History_Items[0].text_message, 0,
                   sizeof(state->event_history_s[slot_idx].Event_History_Items[0].text_message));
        out = state->event_history_s[slot_idx].Event_History_Items[0].text_message;
        out_cap = sizeof(state->event_history_s[slot_idx].Event_History_Items[0].text_message);
    }

    if (valid) {
        nmea_copy_printable_sentence(input, len_bytes, out, out_cap);

        if (out[0] != '\0') {
            DSD_FPRINTF(stderr, "%s", out);
        }

        if (have_events) {
            uint32_t source = (uint32_t)state->dmr_lrrp_source[slot_idx];
            uint32_t target = (uint32_t)state->dmr_lrrp_target[slot_idx];
            char comp_string[128];
            DSD_SNPRINTF(comp_string, sizeof(comp_string), "NMEA SRC: %u; TGT: %u;", source, target);
            watchdog_event_datacall((dsd_opts*)opts, state, source, target, comp_string, slot_idx);
        }
    } else {
        nmea_print_invalid_reason(start_value, end_value, checksum_calc, checksum_ext);
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
        DSD_SNPRINTF(state->event_history_s[0].Event_History_Items[0].gps_s,
                     sizeof(state->event_history_s[0].Event_History_Items[0].gps_s), "(%.6f%s, %.6f%s)", latitude,
                     deg_glyph, longitude, deg_glyph);

        uint32_t source = (uint32_t)state->dmr_lrrp_source[0];
        uint32_t target = (uint32_t)state->dmr_lrrp_target[0];
        char comp_string[128];
        DSD_SNPRINTF(comp_string, sizeof(comp_string), "GPS SRC: %u; TGT: %u;", source, target);
        watchdog_event_datacall((dsd_opts*)opts, state, source, target, comp_string, 0);
    }

    if (src != 0U) {
        gps_write_lrrp_slash_colon(opts, src, latitude, longitude, (int)speed_kph, (int)heading);
    }

    state->dmr_lrrp_source[0] = 0U;
    state->dmr_lrrp_target[0] = 0U;
}
