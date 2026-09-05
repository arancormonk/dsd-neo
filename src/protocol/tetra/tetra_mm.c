// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MM (Mobility Management) PDU dispatcher.
 * ETSI EN 300 392-2, Chapter 16.
 *
 * Phase 11: MM decoding and SYSINFO-based DL carrier frequency computation.
 */

#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>

/* Phase 81: bits_to_uint unified — see tetra_bits.h */
#define mm_bits_to_uint  tetra_bits_to_uint

/* -----------------------------------------------------------------------
 * Carrier frequency table (ETSI EN 300 392-2 Annex A)
 *
 * Index = freq_band (4-bit field from SYSINFO PDU).
 * DL base frequency in Hz; 0 = unimplemented / unknown band.
 *
 * The formula for downlink carriers is:
 *   DL (Hz) = dl_base_hz[freq_band] + main_carrier × 25000
 *             + decoded frequency offset
 *
 * Only commonly deployed European TETRA bands are listed here; other bands
 * return 0 (caller uses the channel map or ignores the result).
 * ----------------------------------------------------------------------- */
static const long tetra_dl_base_hz[16] = {
    390000000L,  /* band  0: 380–385 / 390–395 MHz (European PMR, DL=390) */
    395000000L,  /* band  1: 385–390 / 395–400 MHz (DL=395)               */
    420000000L,  /* band  2: 410–415 / 420–425 MHz (DL=420)               */
    425000000L,  /* band  3: 415–420 / 425–430 MHz (DL=425)               */
    460000000L,  /* band  4: 450–455 / 460–465 MHz (DL=460)               */
    465000000L,  /* band  5: 455–460 / 465–470 MHz (DL=465)               */
          0L,   /* band  6: reserved / unimplemented                      */
          0L,   /* band  7: reserved / unimplemented                      */
    876025000L,  /* band  8: 870–876 / 876–882 MHz (UK 870/876, DL=876)   */
    921000000L,  /* band  9: 915–921 / 921–927 MHz (UK 915/921, DL=921)   */
          0L,   /* band 10: reserved                                      */
          0L,   /* band 11: reserved                                      */
          0L,   /* band 12: reserved                                      */
          0L,   /* band 13: reserved                                      */
          0L,   /* band 14: reserved                                      */
          0L,   /* band 15: reserved                                      */
};

long tetra_carrier_to_dl_hz(uint32_t main_carrier,
                              uint32_t freq_band,
                              uint32_t freq_offset)
{
    if (freq_band >= 16)
        return 0L;
    long base = tetra_dl_base_hz[freq_band];
    if (base == 0L)
        return 0L;
    static const long offset_hz[4] = {0L, 6250L, -6250L, 12500L};
    return base + (long)main_carrier * 25000L + offset_hz[freq_offset & 3u];
}

/* -----------------------------------------------------------------------
 * D-LOCATION-UPDATING-ACCEPT  (PDU type 5)
 *
 * Fixed layout after 5-bit PDU type:
 *   Bit     5 : Location Area present        (1 bit)
 *   Bits  6-19: Location Area (14 bits)      (if present)
 *   ... (optional IEs continue, but we stop here)
 *
 * We extract the LA and update state->tetra_mm_la + tetra_mm_la_valid.
 * ----------------------------------------------------------------------- */
static void parse_mm_location_updating_accept(const uint8_t *bits, int nbits,
                                               int cc, dsd_state *state)
{
    if (nbits < 6) {
        fprintf(stderr, "[TETRA MM D-LU-ACCEPT] CC=%d (too short: %d bits)\n",
                cc, nbits);
        return;
    }

    int off = 5; /* skip 5-bit PDU type */
    uint32_t la_present = mm_bits_to_uint(bits, off, 1); off += 1;

    uint32_t la = 0;
    if (la_present && off + 14 <= nbits) {
        la = mm_bits_to_uint(bits, off, 14); off += 14;
        fprintf(stderr, "[TETRA MM D-LU-ACCEPT] CC=%d  LA=%u\n", cc, la);
        if (state) {
            state->tetra_mm_la       = (uint16_t)la;
            state->tetra_mm_la_valid = 1;
        }
    } else {
        fprintf(stderr, "[TETRA MM D-LU-ACCEPT] CC=%d  (no LA IE)\n", cc);
    }
}

/* -----------------------------------------------------------------------
 * D-ATTACH-DETACH-GROUP-IDENTITY  (PDU type 14)
 *
 * Fixed layout after 5-bit PDU type:
 *   Bit     5 : Attach/Detach flag     (0=attach, 1=detach)
 *   Bit     6 : Class of group        (1 bit)
 *   Bits  7-8 : Address type          (2 bits;  00=GSI, 01=GTSI, 10=DGNA)
 *   (if GSI) Bits 9-32: Group SSI     (24 bits)
 * ----------------------------------------------------------------------- */
static void parse_mm_attach_detach_group(const uint8_t *bits, int nbits,
                                          int cc, dsd_state *state)
{
    if (nbits < 9) {
        fprintf(stderr, "[TETRA MM D-ATTACH-DETACH-GROUP] CC=%d (too short: %d bits)\n",
                cc, nbits);
        return;
    }

    int off = 5; /* skip PDU type */
    uint32_t detach        = mm_bits_to_uint(bits, off, 1); off += 1;
    uint32_t class_of_grp  = mm_bits_to_uint(bits, off, 1); off += 1;
    uint32_t addr_type     = mm_bits_to_uint(bits, off, 2); off += 2;

    uint32_t group_ssi = 0;
    if (addr_type == 0 /* GSI */ && off + 24 <= nbits) {
        group_ssi = mm_bits_to_uint(bits, off, 24);
    }

    fprintf(stderr, "[TETRA MM D-ATTACH-DETACH-GROUP] CC=%d  %s  class=%u"
                    "  addr_type=%u  group_SSI=%u\n",
            cc, detach ? "DETACH" : "ATTACH",
            class_of_grp, addr_type, group_ssi);

    if (state) {
        if (group_ssi != 0)
            state->tetra_mm_group_ssi = group_ssi;
        
        /* Phase 79: MM D-ATTACH-DETACH-GROUP dropped variables */
        state->tetra_mm_detach_flag  = (uint8_t)(detach & 1u);
        state->tetra_mm_class_of_grp = (uint8_t)(class_of_grp & 1u);
        state->tetra_mm_addr_type    = (uint8_t)(addr_type & 0x03u);
    }
}

/* -----------------------------------------------------------------------
 * D-DISABLE (PDU type 3)  — network disables the MS.
 * ----------------------------------------------------------------------- */
static void parse_mm_d_disable(const uint8_t *bits, int nbits,
                                int cc, dsd_state *state)
{
    (void)bits; (void)nbits;
    fprintf(stderr, "[TETRA MM D-DISABLE] CC=%d  (MS disabled)\n", cc);
    if (state)
        state->tetra_ms_enabled = 0;
}

/* -----------------------------------------------------------------------
 * D-ENABLE (PDU type 4)  — network re-enables the MS.
 * ----------------------------------------------------------------------- */
static void parse_mm_d_enable(const uint8_t *bits, int nbits,
                               int cc, dsd_state *state)
{
    (void)bits; (void)nbits;
    fprintf(stderr, "[TETRA MM D-ENABLE] CC=%d  (MS enabled)\n", cc);
    if (state)
        state->tetra_ms_enabled = 1;
}

/* -----------------------------------------------------------------------
 * D-SUBSCRIBER-CLASS-ASSIGN (PDU type 10)  — ETSI §16.10
 *
 * Fixed layout after 5-bit PDU type:
 *   Bits 5-20: SCG (Subscriber Class Group, 16 bits)
 * ----------------------------------------------------------------------- */
static void parse_mm_d_subscriber_class_assign(const uint8_t *bits, int nbits,
                                                int cc, dsd_state *state)
{
    if (nbits < 21) {
        fprintf(stderr, "[TETRA MM D-SUBSCR-CLASS-ASSIGN] CC=%d (too short: %d bits)\n",
                cc, nbits);
        return;
    }
    uint32_t scg = mm_bits_to_uint(bits, 5, 16);
    fprintf(stderr, "[TETRA MM D-SUBSCR-CLASS-ASSIGN] CC=%d  SCG=0x%04X\n",
            cc, scg);
    if (state)
        state->tetra_subscr_class = (uint16_t)(scg & 0xFFFFu);
}

/* -----------------------------------------------------------------------
 * D-MM-STATUS (PDU type 15)  — ETSI §16.15
 *
 * Fixed layout after 5-bit PDU type:
 *   Bits 5-12: status_value (8 bits)
 * ----------------------------------------------------------------------- */
static void parse_mm_d_mm_status(const uint8_t *bits, int nbits,
                                  int cc, dsd_state *state)
{
    if (nbits < 13) {
        fprintf(stderr, "[TETRA MM D-MM-STATUS] CC=%d (too short: %d bits)\n",
                cc, nbits);
        return;
    }
    uint32_t status_val = mm_bits_to_uint(bits, 5, 8);
    fprintf(stderr, "[TETRA MM D-MM-STATUS] CC=%d  status=%u\n",
            cc, status_val);
    if (state)
        state->tetra_mm_status_code = (uint8_t)(status_val & 0xFFu);
}

/* -----------------------------------------------------------------------
 * D-LOCATION-UPDATING-COMMAND  (PDU type 6)  — network requests the MS to
 * perform a location update.  ETSI EN 300 392-2 §16.6
 *
 * Fixed layout after 5-bit PDU type:
 *   Bit     5 : Location Area present    (1 bit)
 *   Bits  6-19: Location Area            (14 bits, if present)
 * ----------------------------------------------------------------------- */
static void parse_mm_d_location_updating_command(const uint8_t *bits, int nbits,
                                                  int cc, dsd_state *state)
{
    int off = 5;
    uint32_t la = 0;
    if (nbits >= 6) {
        uint32_t la_present = mm_bits_to_uint(bits, off, 1); off += 1;
        if (la_present && off + 14 <= nbits) {
            la = mm_bits_to_uint(bits, off, 14);
            fprintf(stderr, "[TETRA MM D-LU-COMMAND] CC=%d  req_LA=%u\n", cc, la);
            if (state) {
                state->tetra_mm_lu_req_la       = (uint16_t)la;
                state->tetra_mm_lu_req_la_valid = 1;
            }
            return;
        }
    }
    fprintf(stderr, "[TETRA MM D-LU-COMMAND] CC=%d  (no LA IE)\n", cc);
}

/* -----------------------------------------------------------------------
 * D-LOCATION-UPDATING-REJECT  (PDU type 7)  — ETSI EN 300 392-2 §16.7
 *
 * Fixed layout after 5-bit PDU type:
 *   Bits 5-7 : Rejection cause  (3 bits)
 * ----------------------------------------------------------------------- */
static void parse_mm_d_lu_reject(const uint8_t *bits, int nbits,
                                  int cc, dsd_state *state)
{
    uint8_t cause = 0;
    if (nbits >= 8) {
        cause = (uint8_t)mm_bits_to_uint(bits, 5, 3);
        fprintf(stderr, "[TETRA MM D-LU-REJECT] CC=%d  cause=%u\n", cc, cause);
    } else {
        fprintf(stderr, "[TETRA MM D-LU-REJECT] CC=%d  (%d bits, too short)\n", cc, nbits);
    }
    if (state) {
        state->tetra_mm_lu_reject_cause = cause;
        state->tetra_mm_lu_reject_valid = 1;
    }
}

/* -----------------------------------------------------------------------
 * D-TEMPORARY-ADDRESS  (PDU type 9)  — ETSI EN 300 392-2 §16.9
 *
 * Fixed layout after 5-bit PDU type:
 *   Bits  5-28 : Temporary ITSI / SSI  (24-bit SSI portion)
 * ----------------------------------------------------------------------- */
static void parse_mm_d_temporary_address(const uint8_t *bits, int nbits,
                                          int cc, dsd_state *state)
{
    uint32_t temp_ssi = 0;
    if (nbits >= 29) {
        temp_ssi = mm_bits_to_uint(bits, 5, 24);
        fprintf(stderr, "[TETRA MM D-TEMPORARY-ADDRESS] CC=%d  temp_SSI=%u\n", cc, temp_ssi);
    } else {
        fprintf(stderr, "[TETRA MM D-TEMPORARY-ADDRESS] CC=%d  (%d bits, too short)\n", cc, nbits);
    }
    if (state) {
        state->tetra_mm_temp_ssi       = temp_ssi;
        state->tetra_mm_temp_ssi_valid = 1;
    }
}

/* -----------------------------------------------------------------------
 * D-PARAMETER-CHANGE  (PDU type 11)  — ETSI EN 300 392-2 §16.11
 *
 * Fixed layout after 5-bit PDU type:
 *   Bit     5 : LA present      (1 bit)
 *   Bits 6-19 : Location Area   (14 bits, if present)
 * ----------------------------------------------------------------------- */
static void parse_mm_d_parameter_change(const uint8_t *bits, int nbits,
                                         int cc, dsd_state *state)
{
    uint16_t la = 0;
    int off = 5;
    if (nbits >= 6) {
        uint32_t la_present = mm_bits_to_uint(bits, off, 1); off += 1;
        if (la_present && off + 14 <= nbits) {
            la = (uint16_t)mm_bits_to_uint(bits, off, 14);
            fprintf(stderr, "[TETRA MM D-PARAMETER-CHANGE] CC=%d  LA=%u\n", cc, la);
        } else {
            fprintf(stderr, "[TETRA MM D-PARAMETER-CHANGE] CC=%d  (no LA)\n", cc);
        }
    } else {
        fprintf(stderr, "[TETRA MM D-PARAMETER-CHANGE] CC=%d  (%d bits)\n", cc, nbits);
    }
    if (state) {
        state->tetra_mm_param_change_valid = 1;
        state->tetra_mm_param_change_la    = la;
    }
}

/* -----------------------------------------------------------------------
 * D-ITSI-DETACH-ACK (PDU type 12) — ETSI EN 300 392-2 §16.12
 *
 * The MS has been successfully detached from the network.
 * No mandatory fields beyond the 5-bit PDU type.
 * ----------------------------------------------------------------------- */
static void parse_mm_d_itsi_detach_ack(const uint8_t *bits, int nbits,
                                       int cc, dsd_state *state)
{
    (void)bits; (void)nbits;
    fprintf(stderr, "[TETRA MM D-ITSI-DETACH-ACK] CC=%d\n", cc);
    if (state)
        state->tetra_mm_detach_ack = 1;
}

/* -----------------------------------------------------------------------
 * D-LOCATION-UPDATING-DEMAND (PDU type 13)  — ETSI EN 300 392-2 §16.13
 *
 * Fixed layout after 5-bit PDU type:
 *   Bits 5-7 : Demand cause  (3 bits)
 * ----------------------------------------------------------------------- */
static void parse_mm_d_lu_demand(const uint8_t *bits, int nbits,
                                 int cc, dsd_state *state)
{
    uint8_t cause = 0;
    if (nbits >= 8) {
        cause = (uint8_t)mm_bits_to_uint(bits, 5, 3);
        fprintf(stderr, "[TETRA MM D-LU-DEMAND] CC=%d  cause=%u\n", cc, cause);
    } else {
        fprintf(stderr, "[TETRA MM D-LU-DEMAND] CC=%d  (%d bits, too short)\n", cc, nbits);
    }
    if (state) {
        state->tetra_mm_lu_demand_cause = cause;
        state->tetra_mm_lu_demand_valid = 1;
    }
}

/* -----------------------------------------------------------------------
 * D-AUTHENTICATION  (PDU type 1)  — ETSI EN 300 392-2 §16.10.1
 *
 * Fixed layout after 5-bit PDU type:
 *   Bits  5-7  : auth_type       (3 bits)
 *   Bits  8-39 : RAND            (32 bits, random challenge)
 *   Bits 40-71 : RS              (32 bits, response seed)
 *   further IEs may follow.
 * ----------------------------------------------------------------------- */
static void parse_mm_d_authentication(const uint8_t *bits, int nbits,
                                       int cc, dsd_state *state)
{
    uint32_t rand_val = 0;
    if (nbits >= 40) {
        uint32_t auth_type = mm_bits_to_uint(bits, 5, 3);
        rand_val = mm_bits_to_uint(bits, 8, 32);
        fprintf(stderr, "[TETRA MM D-AUTHENTICATION] CC=%d  auth_type=%u  RAND=0x%08X\n",
                cc, auth_type, rand_val);
    } else {
        fprintf(stderr, "[TETRA MM D-AUTHENTICATION] CC=%d  (%d bits, too short)\n",
                cc, nbits);
    }
    if (state) {
        state->tetra_mm_auth_rand  = rand_val;
        state->tetra_mm_auth_valid = 1;
    }
}

/* -----------------------------------------------------------------------
 * D-CHECK-TSI  (PDU type 2)  — ETSI EN 300 392-2 §16.10.2
 *
 * Fixed layout after 5-bit PDU type:
 *   Bits  5-28 : Individual TSI (24 bits)
 * ----------------------------------------------------------------------- */
static void parse_mm_d_check_tsi(const uint8_t *bits, int nbits,
                                  int cc, dsd_state *state)
{
    uint32_t tsi = 0;
    if (nbits >= 29) {
        tsi = mm_bits_to_uint(bits, 5, 24);
        fprintf(stderr, "[TETRA MM D-CHECK-TSI] CC=%d  TSI=%u\n", cc, tsi);
    } else {
        fprintf(stderr, "[TETRA MM D-CHECK-TSI] CC=%d  (%d bits, too short)\n",
                cc, nbits);
    }
    if (state) {
        state->tetra_mm_check_tsi       = tsi;
        state->tetra_mm_check_tsi_valid = 1;
    }
}

/* -----------------------------------------------------------------------
 * MM D-STATUS  (PDU type 8)  — ETSI EN 300 392-2 §16.10.8
 *
 * Fixed layout after 5-bit PDU type:
 *   Bits 5-12 : Protocol status value (8 bits)
 * ----------------------------------------------------------------------- */
static void parse_mm_d_status(const uint8_t *bits, int nbits,
                               int cc, dsd_state *state)
{
    uint8_t status_val = 0;
    if (nbits >= 13) {
        status_val = (uint8_t)mm_bits_to_uint(bits, 5, 8);
        fprintf(stderr, "[TETRA MM D-STATUS] CC=%d  status=%u\n", cc, status_val);
    } else {
        fprintf(stderr, "[TETRA MM D-STATUS] CC=%d  (%d bits, too short)\n",
                cc, nbits);
    }
    if (state) {
        state->tetra_mm_d_status_val   = status_val;
        state->tetra_mm_d_status_valid = 1;
    }
}

/* -----------------------------------------------------------------------
 * MM D-OTAR  (PDU type 0)  — ETSI EN 300 392-7 Over-The-Air Re-keying.
 *
 * Fixed layout after 5-bit PDU type:
 *   Bits 5-9 : OTAR sub-PDU type (5 bits)
 *   Payload  : varies per OTAR sub-type (not decoded further here).
 * ----------------------------------------------------------------------- */
static void parse_mm_d_otar(const uint8_t *bits, int nbits,
                             int cc, dsd_state *state)
{
    uint8_t otar_type = 0;
    if (nbits >= 10) {
        otar_type = (uint8_t)mm_bits_to_uint(bits, 5, 5);
        fprintf(stderr, "[TETRA MM D-OTAR] CC=%d  otar_pdu_type=%u  (%d bits)\n",
                cc, otar_type, nbits);
    } else {
        fprintf(stderr, "[TETRA MM D-OTAR] CC=%d  (%d bits, too short)\n", cc, nbits);
    }
    if (state) {
        state->tetra_otar_pdu_type = otar_type;
        state->tetra_otar_valid    = 1;
    }
}

/* -----------------------------------------------------------------------
 * Public entry point
 * ----------------------------------------------------------------------- */
void tetra_mm_dispatch(const uint8_t *bits, int nbits,
                       int cc, const dsd_opts *opts, dsd_state *state)
{
    if (!bits || nbits < 5) {
        return;
    }

    uint32_t pdu_type = mm_bits_to_uint(bits, 0, 5);

    switch (pdu_type) {

    case TETRA_MM_D_OTAR:
        parse_mm_d_otar(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_AUTHENTICATION:
        parse_mm_d_authentication(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_CHECK_TSI:
        parse_mm_d_check_tsi(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_DISABLE:
        parse_mm_d_disable(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_ENABLE:
        parse_mm_d_enable(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_LOCATION_UPDATING_ACCEPT:
        parse_mm_location_updating_accept(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_LOCATION_UPDATING_COMMAND:
        parse_mm_d_location_updating_command(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_LOCATION_UPDATING_REJECT:
        parse_mm_d_lu_reject(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_STATUS:
        parse_mm_d_status(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_TEMPORARY_ADDRESS:
        parse_mm_d_temporary_address(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_SUBSCRIBER_CLASS_ASSIGN:
        parse_mm_d_subscriber_class_assign(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_PARAMETER_CHANGE:
        parse_mm_d_parameter_change(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_ITSI_DETACH_ACK:
        parse_mm_d_itsi_detach_ack(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_LOCATION_UPDATING_DEMAND:
        parse_mm_d_lu_demand(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_ATTACH_DETACH_GROUP:
        parse_mm_attach_detach_group(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_MM_STATUS:
        parse_mm_d_mm_status(bits, nbits, cc, state);
        break;

    default:
        if (opts && opts->errorbars) {
            fprintf(stderr, "[TETRA MM] CC=%d  pdu_type=%u  (%d bits)\n",
                    cc, pdu_type, nbits);
        }
        break;
    }
}
