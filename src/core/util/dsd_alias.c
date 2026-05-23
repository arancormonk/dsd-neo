// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * dsd_alias.c
 * Talker Alias Handling for Various Protocols and Vendors
 *
 * LWVMOBILE
 * 2025-02 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/runtime/unicode.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
policy_try_append_alias(dsd_state* state, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry entry;
    dsd_tg_policy_lookup lookup;
    if (!state || id == 0 || !mode || !name) {
        return 0;
    }

    if (dsd_tg_policy_lookup_id(state, id, &lookup) == 0 && lookup.match == DSD_TG_POLICY_MATCH_EXACT) {
        return 0;
    }

    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &entry) != 0) {
        return 0;
    }

    return dsd_tg_policy_upsert_exact(state, &entry, DSD_TG_POLICY_UPSERT_ADD_IF_MISSING) == 0 ? 1 : 0;
}

//Motorola P25 OTA Alias Decoding ripped/demystified from Ilya Smirnov's SDRTrunk Voodoo Code
static uint8_t moto_alias_lut[256] = {
    0xD2, 0xF6, 0xD4, 0x2B, 0x63, 0x49, 0x94, 0x5E, 0xA7, 0x5C, 0x70, 0x69, 0xF7, 0x08, 0xB1, 0x7D, 0x38, 0xCF, 0xCC,
    0xD8, 0x51, 0x8F, 0xD5, 0x93, 0x6A, 0xF3, 0xEF, 0x7E, 0xFB, 0x64, 0xF4, 0x35, 0x27, 0x07, 0x31, 0x14, 0x87, 0x98,
    0x76, 0x34, 0xCA, 0x92, 0x33, 0x1B, 0x4F, 0x8C, 0x09, 0x40, 0x32, 0x36, 0x77, 0x12, 0xD3, 0xC3, 0x01, 0xAB, 0x72,
    0x81, 0x95, 0xC9, 0xC0, 0xE9, 0x65, 0x52, 0x24, 0x30, 0x1C, 0xDB, 0x88, 0xE8, 0x97, 0x9D, 0x58, 0x26, 0x04, 0x39,
    0xAC, 0x2A, 0x9E, 0xAA, 0x25, 0xD7, 0xCE, 0xEB, 0x96, 0xF5, 0x0E, 0x8D, 0xDC, 0xA9, 0x2F, 0xDD, 0x1F, 0xEA, 0x91,
    0xB7, 0xD6, 0x89, 0x8B, 0xD1, 0xB0, 0x99, 0x13, 0x7A, 0xE7, 0x9A, 0xB5, 0x86, 0xFF, 0x46, 0x85, 0xB2, 0x73, 0xDA,
    0xBF, 0xD0, 0x71, 0xCB, 0x4D, 0x80, 0x15, 0x67, 0x16, 0x1A, 0x20, 0x8E, 0x45, 0x3E, 0xF2, 0x2E, 0x66, 0x90, 0x74,
    0x8A, 0x6F, 0x78, 0xBB, 0x53, 0x03, 0x11, 0x68, 0xCD, 0x44, 0x17, 0x28, 0x5F, 0x1E, 0x84, 0x75, 0x79, 0x6E, 0x9B,
    0x2C, 0xBE, 0x62, 0x2D, 0xF1, 0x7C, 0xB8, 0x83, 0xD9, 0x4E, 0x6D, 0x02, 0x61, 0x3D, 0xA8, 0x06, 0xB9, 0xF8, 0x9C,
    0x37, 0x3A, 0x23, 0xC1, 0x50, 0xED, 0x9F, 0xAF, 0x3B, 0xBD, 0x82, 0xBA, 0xA0, 0xDF, 0xC2, 0x47, 0x22, 0xF0, 0xEE,
    0xA1, 0xFE, 0xA2, 0x10, 0x5B, 0x48, 0x57, 0xA3, 0x05, 0x60, 0x7B, 0x0D, 0xF9, 0x6C, 0xB3, 0x56, 0x4C, 0xBC, 0x29,
    0xA4, 0x0F, 0xEC, 0xB6, 0xA5, 0xA6, 0x3C, 0x7F, 0x6B, 0xB4, 0x21, 0xAD, 0xAE, 0xC4, 0xC8, 0xC5, 0x5D, 0xDE, 0xE0,
    0x1D, 0x19, 0x4B, 0xC6, 0x0C, 0x3F, 0x5A, 0xC7, 0xE1, 0x59, 0x55, 0x54, 0x4A, 0x43, 0x42, 0xE2, 0xE3, 0xFA, 0x00,
    0xE4, 0xE5, 0x18, 0x41, 0x0B, 0x0A, 0xE6, 0xFC, 0xFD};

static void
apx_phase1_pack_lcw(uint64_t upper_bits, uint8_t lower_bits, uint8_t* lcw) {
    DSD_MEMSET(lcw, 0, ((size_t)72) * sizeof(*lcw));
    for (uint64_t i = 0; i < 64; i++) {
        lcw[i] = (upper_bits >> (63 - i)) & 1;
    }
    for (uint64_t i = 0; i < 8; i++) {
        lcw[i + 64] = (lower_bits >> (7 - i)) & 1;
    }
}

static void
apx_phase1_send_lcw(dsd_opts* opts, dsd_state* state, uint64_t upper_bits, uint8_t lower_bits) {
    uint8_t lcw[72];
    apx_phase1_pack_lcw(upper_bits, lower_bits, lcw);
    p25_lcw(opts, state, lcw, 0);
}

void
apx_embedded_alias_test_phase1(dsd_opts* opts, dsd_state* state) {
    static const uint64_t phase1_upper[] = {0x15902D2806010005, 0x1790010BEE0740F0, 0x17900200DD2D2168,
                                            0x17900301B52FFBFB, 0x1790040E53FE86BE, 0x17900508FD5AB910,
                                            0x1790060376F9D800};
    static const uint8_t phase1_lower[] = {0xDE, 0x4E, 0x1A, 0xFE, 0xF7, 0xB2, 0x00};
    static const size_t phase1_count = sizeof(phase1_upper) / sizeof(phase1_upper[0]);

    for (size_t i = 0; i < phase1_count; i++) {
        apx_phase1_send_lcw(opts, state, phase1_upper[i], phase1_lower[i]);
    }

    //Harris Phase 1 GPS
    state->lastsrc = 1000;
    apx_phase1_send_lcw(opts, state, 0x2AA41D4C24262328, 0xAF);
    apx_phase1_send_lcw(opts, state, 0x2BA44E0DB2660108, 0x14);
    state->lastsrc = 0;
}

void
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {

    UNUSED(opts);
    uint8_t ta_len = (uint8_t)ConvertBitIntoBytes(&lc_bits[32], 8); //len in blocks of associated talker alias
    uint8_t sn = (uint8_t)ConvertBitIntoBytes(&lc_bits[56], 4);
    DSD_FPRINTF(stderr, " SN: %X;", sn);
    DSD_FPRINTF(stderr, " BN: 0/%d;", ta_len);

    //use dmr_pdu_sf for storage, store entire header (will be used to verify complete reception of full alias)
    DSD_MEMSET(state->dmr_pdu_sf[slot], 0, sizeof(state->dmr_pdu_sf[slot])); //reset storage for header and blocks
    DSD_MEMCPY(state->dmr_pdu_sf[slot], lc_bits, 72 * sizeof(uint8_t));
}

void
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {

    UNUSED(opts);
    uint8_t bn = (uint8_t)ConvertBitIntoBytes(&lc_bits[16], 8); //current block number
    uint8_t sn = (uint8_t)ConvertBitIntoBytes(&lc_bits[24], 4); //is a static value on all block sequences
    uint8_t ta_len =
        (uint8_t)ConvertBitIntoBytes(&state->dmr_pdu_sf[slot][32], 8); //len in blocks pulled from stored header
    uint16_t header = (uint16_t)ConvertBitIntoBytes(&state->dmr_pdu_sf[slot][0], 16); //header check, should be 0x1590

    if (ta_len == 0
        || header != 0x1590) //checkdown, make sure we have an up to date header for this with a good len value
    {
        DSD_FPRINTF(stderr, " Missing Header");
        DSD_FPRINTF(stderr, " BN: %d/??;", bn);
        DSD_FPRINTF(stderr, " SN: %X;", sn);
        DSD_FPRINTF(stderr, " Partial: ");
        for (uint8_t i = 7; i < 18; i++) {
            DSD_FPRINTF(stderr, "%0X", (uint8_t)ConvertBitIntoBytes(&lc_bits[0 + (i * 4)], 4));
        }

        //clear out now stale storage
        DSD_MEMSET(state->dmr_pdu_sf[slot], 0, sizeof(state->dmr_pdu_sf[slot]));
    }

    else //good len and header stored
    {

        //sanity check, bn cannot equal zero (this shouldn't happen, but bad decode could occur)
        if (bn == 0) {
            bn = 1;
        }

        DSD_FPRINTF(stderr, " SN: %X;", sn);
        DSD_FPRINTF(stderr, " BN: %d/%d;", bn, ta_len);

        //use dmr_pdu_sf for storage, store data relevant portion at ptr of (bn-1) * 44 + 72 offset for header
        DSD_MEMCPY(state->dmr_pdu_sf[slot] + (((bn - 1) * 44) + 72), lc_bits + 28, 44 * sizeof(uint8_t));

        if (ta_len == bn) //this is the last block, proceed to decoding
        {

            //Calculate variable len bits determined by non-zero octets
            int16_t num_bits = 56; //starting value is the static bit count on the fqsuid

            //evaluate the storage and determine how many octets/bits are present at this point (expanded to two octets each, CRC with a 00xx pattern failed this)
            for (int16_t i = 0; i < 184; i++) //(3072-128)/16
            {
                uint16_t bytes = (uint16_t)ConvertBitIntoBytes(&state->dmr_pdu_sf[slot][72 + 56 + (i * 16)], 16);
                if (bytes == 0) {
                    break;
                } else {
                    num_bits += 16;
                }
            }

            //pass to alias decoder
            apx_embedded_alias_decode(opts, state, slot, num_bits, state->dmr_pdu_sf[slot]);

            //clear out now stale storage
            DSD_MEMSET(state->dmr_pdu_sf[slot], 0, sizeof(state->dmr_pdu_sf[slot]));
        }
    }
}

void
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {

    UNUSED(opts);
    uint8_t ta_len = (uint8_t)ConvertBitIntoBytes(&lc_bits[40], 8);
    uint8_t sn = (uint8_t)ConvertBitIntoBytes(&lc_bits[64], 4);
    uint8_t bn = (uint8_t)ConvertBitIntoBytes(&lc_bits[56], 8);
    DSD_FPRINTF(stderr, " SN: %X;",
                sn); //NOTE: vPDU header is also a partial block, and has a block num and SN value in it
    DSD_FPRINTF(stderr, " BN: %d/%d;", bn, ta_len);

    //bit array to rearrange input lc_bits from phase 2 input to match the phase 1 header and block handling
    uint8_t bits[136];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMCPY(bits, lc_bits, ((size_t)2) * 8 * sizeof(uint8_t));            //header 0x9190
    DSD_MEMCPY(bits + 16, lc_bits + 24, ((size_t)4) * 8 * sizeof(uint8_t));  //BN, SN, etc
    DSD_MEMCPY(bits + 56, lc_bits + 56, ((size_t)10) * 8 * sizeof(uint8_t)); //adding 8 bits of extra padding here

    int16_t alias_st = 136; //start of the encoded alias (the copied size of this header, basically)

    //debug, dump header arranged at this end
    // DSD_FPRINTF(stderr, " Header: ");
    // for (int16_t i = 0; i < alias_st/8; i++) //double check and adjust
    //   DSD_FPRINTF(stderr, "%02X", (uint8_t)ConvertBitIntoBytes(&bits[0+(i*8)], 8));

    //use dmr_pdu_sf for storage, store entire header (will be used to verify complete reception of full alias)
    DSD_MEMSET(state->dmr_pdu_sf[slot], 0, sizeof(state->dmr_pdu_sf[slot])); //reset storage for header and blocks
    DSD_MEMCPY(state->dmr_pdu_sf[slot], bits,
               (size_t)alias_st
                   * sizeof(uint8_t)); //this header block has 128 bits of relevant data (through the fqsuid)
}

void
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {

    UNUSED(opts);
    uint8_t bn = (uint8_t)ConvertBitIntoBytes(&lc_bits[24], 8); //current block number
    uint8_t sn = (uint8_t)ConvertBitIntoBytes(&lc_bits[32], 4); //is a static value on all block sequences
    uint8_t ta_len =
        (uint8_t)ConvertBitIntoBytes(&state->dmr_pdu_sf[slot][32], 8); //len in blocks pulled from stored header
    uint16_t header = (uint16_t)ConvertBitIntoBytes(&state->dmr_pdu_sf[slot][0], 16); //header check, should be 0x9190

    if (ta_len == 0 || header != 0x9190) {
        DSD_FPRINTF(stderr, " Missing Header");
        DSD_FPRINTF(stderr, " BN: %d/??;", bn);
        DSD_FPRINTF(stderr, " SN: %X;", sn);
        DSD_FPRINTF(stderr, " Partial: ");
        for (uint8_t i = 9; i < 32; i++) { //double check and adjust
            DSD_FPRINTF(stderr, "%0X", (uint8_t)ConvertBitIntoBytes(&lc_bits[0 + (i * 4)], 4));
        }

        //clear out now stale storage
        DSD_MEMSET(state->dmr_pdu_sf[slot], 0, sizeof(state->dmr_pdu_sf[slot]));
    }

    else //good len and header stored
    {
        int16_t rel_bits = 100; //number of relevant bits in each block
        int16_t rel_st = 36;    //start of relevant bits in this block
        int16_t alias_st = 136; //start of the encoded alias

        //sanity check, bn cannot equal zero (this shouldn't happen, but bad decode could occur)
        if (bn == 0) {
            bn = 1;
        }

        DSD_FPRINTF(stderr, " SN: %X;", sn);
        DSD_FPRINTF(stderr, " BN: %d/%d;", bn, ta_len);

        //use dmr_pdu_sf for storage, store data relevant portion at ptr calculated below
        DSD_MEMCPY(state->dmr_pdu_sf[slot] + (alias_st + ((bn - 1) * rel_bits)), lc_bits + rel_st,
                   rel_bits * sizeof(uint8_t)); //Fix this value when samples arrive

        //debug, dump accumulated data at this end
        // DSD_FPRINTF(stderr, " Accumulated: ");
        // for (int16_t i = 0; i < (alias_st+(rel_bits*bn))/8; i++) //double check and adjust
        //   DSD_FPRINTF(stderr, "%02X", (uint8_t)ConvertBitIntoBytes(&state->dmr_pdu_sf[slot][0+(i*8)], 8));

        if (ta_len == bn) //this is the last block, proceed to decoding
        {

            //Calculate variable len bits determined by non-zero octets
            int16_t num_bits = 56; //starting value is the static bit count on the fqsuid

            //evaluate the storage and determine how many octets/bits are present at this point (expanded to two octets each, CRC with a 00xx pattern failed this)
            for (int16_t i = 0; i < 184; i++) //(3072-128)/16
            {
                uint16_t bytes = (uint16_t)ConvertBitIntoBytes(&state->dmr_pdu_sf[slot][72 + 56 + (i * 16)], 16);
                if (bytes == 0) {
                    break;
                } else {
                    num_bits += 16;
                }
            }

            //pass to alias decoder
            apx_embedded_alias_decode(opts, state, slot, num_bits, state->dmr_pdu_sf[slot]);

            //clear out now stale storage
            DSD_MEMSET(state->dmr_pdu_sf[slot], 0, sizeof(state->dmr_pdu_sf[slot]));
        }
    }
}

void
apx_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t num_bits, uint8_t* input) {

    UNUSED(opts);
    UNUSED(state);
    UNUSED(slot);

    //debug, dump completed data set
    // DSD_FPRINTF(stderr, "\n Full: ");
    // for (int16_t i = 0; i < (72+num_bits)/4; i++)
    //   DSD_FPRINTF(stderr, "%X", (uint8_t)ConvertBitIntoBytes(&input[0+(i*4)], 4));

    //extract CRC
    uint16_t crc_ext = (uint16_t)ConvertBitIntoBytes(&input[(72 + num_bits - 16)], 16);

    //compute CRC
    uint16_t crc_cmp = ComputeCrcCCITT16d(&input[72], num_bits - 16);

    //print comparison
    // DSD_FPRINTF(stderr, " CRC EXT: %04X CMP: %04X;", crc_ext, crc_cmp);
    if (crc_ext != crc_cmp) {
        DSD_FPRINTF(stderr, " Alias CRC Error;");
    }
    // else DSD_FPRINTF(stderr, " Alias Okay;");

    //start decoding the alias
    if (crc_ext == crc_cmp) {

        //extract fully qualified SUID
        uint32_t wacn = (uint32_t)ConvertBitIntoBytes(&input[72], 20);
        uint32_t sys = (uint32_t)ConvertBitIntoBytes(&input[92], 12);
        uint32_t rid = (uint32_t)ConvertBitIntoBytes(&input[104], 24);

        //print fully qualified SUID
        DSD_FPRINTF(stderr, "\n FQ-SUID: %05X.%03X.%06X (%d);", wacn, sys, rid, rid);

        //WIP: Working, needs more samples to verify various num_bits values
        uint16_t ptr = 128; //starting point of encoded alias
        uint8_t encoded[200];
        DSD_MEMSET(encoded, 0, sizeof(encoded));
        uint8_t decoded[200];
        DSD_MEMSET(decoded, 0, sizeof(decoded));
        uint16_t num_bytes = (num_bits / 8) - 9; //subtract 2 CRC and 7 FQSUID

        //sanity check
        if (num_bytes == 0) {
            num_bytes = 1;
        }

        for (uint16_t i = 0; i < num_bytes; i++) {
            encoded[i] = (uint8_t)ConvertBitIntoBytes(&input[ptr], 8);
            ptr += 8;
        }

        uint16_t accumulator = num_bytes;

        //Ilya's Voodoo Code
        for (uint16_t i = 0; i < num_bytes; i++) {
            // Multiplication step 1
            uint16_t accum_mult = accumulator * 293 + 0x72E9;

            // Lookup table step
            uint8_t lut = moto_alias_lut[encoded[i]];
            uint8_t mult1 = lut - (accum_mult >> 8);

            // Incrementing step
            uint8_t mult2 = 1;
            uint8_t shortstop = accum_mult | 0x1;
            uint8_t increment = shortstop << 1;

            //clang warning -- warning: result of comparison of constant -1 with expression of type 'uint8_t' (aka 'unsigned char') is always true [-Wtautological-constant-out-of-range-compare]
            // while(mult2 != -1 && shortstop != 1) //clang warning can't be -1 if uint8_t (set to 255 instead?)
            while (shortstop != 1) //this one tests out okay, so may use it instead
            {
                shortstop += increment;
                mult2 += 2;
            }

            // Multiplication step 2
            decoded[i] = mult1 * mult2;

            // Update the accumulator
            accumulator += encoded[i] + 1;
        }
        DSD_FPRINTF(stderr, " Alias: ");
        for (int i = 0; i < num_bytes / 2; i++) {
            uint16_t ch = (uint16_t)(((decoded[(i * 2) + 0]) << 8) | ((decoded[(i * 2) + 1]) << 0));
            if (dsd_unicode_supported()) {
                DSD_FPRINTF(stderr, "%lc", ch);
            } else {
                unsigned char lo = (unsigned char)(ch & 0xFF);
                if (lo >= 0x20 && lo < 0x7F) {
                    fputc((int)lo, stderr);
                } else {
                    fputc('?', stderr);
                }
            }
        }

        apx_embedded_alias_dump(opts, state, slot, num_bytes, input, decoded);
    }
}

void
apx_embedded_alias_dump(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint16_t num_bytes, const uint8_t* input,
                        const uint8_t* decoded) {

    char str[50];
    DSD_MEMSET(str, 0, sizeof(str));
    char fqs[50];
    DSD_MEMSET(fqs, 0, sizeof(fqs));

    //check num_bytes, if greter than 100, then set to 100
    if (num_bytes >= 98) {
        num_bytes = 98;
    }

    for (int i = 0; i < num_bytes / 2; i++) {
        if (decoded[(i * 2) + 1] == 0x2C) { //remove a comma if it exists, change it to a 0x2E dot
            str[i] = 0x2E;
        } else if (decoded[(i * 2) + 1] > 0x1F && decoded[(i * 2) + 1] < 0x7F) { //may not need or use this restriction
            str[i] = decoded[(i * 2) + 1];
        } else {
            str[i] = 0x20; //space
        }
    }

    //fully qualified SUID
    uint32_t wacn = (uint32_t)ConvertBitIntoBytes(&input[72], 20);
    uint32_t sys = (uint32_t)ConvertBitIntoBytes(&input[92], 12);
    uint32_t rid = (uint32_t)ConvertBitIntoBytes(&input[104], 24);

    DSD_SNPRINTF(fqs, sizeof fqs, " FQ-SUID: %05X:%03X.%06X (%d);", wacn, sys, rid, rid);
    if (rid != 0 && state->event_history_s[slot].Event_History_Items[0].source_id == rid) {
        DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].alias,
                     sizeof(state->event_history_s[slot].Event_History_Items[0].alias), "%s; %s", str, fqs);
    }

    if (policy_try_append_alias(state, rid, "D", str)) //not already in there, so save it there now
    {
        dsd_tg_policy_entry row;
        if (dsd_tg_policy_make_exact_entry(rid, "D", str, DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &row) == 0) {
            char metadata[128];
            DSD_SNPRINTF(metadata, sizeof(metadata), "FQS:%05X.%03X.%06X(%d),Moto", wacn, sys, rid, rid);
            (void)dsd_tg_policy_append_group_file_row(opts, &row, metadata);
        }
    }
}

//end Motorola P25 OTA Alias Decoding

void
l3h_embedded_alias_blocks_phase1(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {

    uint8_t op = (uint8_t)ConvertBitIntoBytes(&lc_bits[0], 8);
    uint8_t ptr = op - 0x32;
    uint8_t bytes[7];
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    for (uint8_t i = 0; i < 7; i++) {
        bytes[i] = (uint8_t)ConvertBitIntoBytes(&lc_bits[16 + (i * 8)], 8);
    }

    //use +4 offset to match the MAC vPDU since that was already worked out long ago
    DSD_MEMCPY(state->dmr_pdu_sf[slot] + 4 + ((size_t)ptr * 7), bytes, sizeof(bytes));

    //to be tested
    if (ptr == 4) { //is there always 4 blocks, or is it a variable amount?
        l3h_embedded_alias_decode(opts, state, slot, 4 + 28, state->dmr_pdu_sf[slot]);
    }
}

static void
l3h_alias_resolve_src_tg(const dsd_state* state, uint8_t slot, uint32_t* tsrc, uint32_t* ttg) {
    *tsrc = 0;
    *ttg = 0;
    if (slot == 0) {
        *tsrc = state->lastsrc;
        *ttg = state->lasttg;
    } else if (slot == 1) {
        *tsrc = state->lastsrcR;
        *ttg = state->lasttgR;
    }
}

static void
l3h_alias_print_char(uint8_t value) {
    if ((value > 0x19) && (value < 0x7F)) {
        DSD_FPRINTF(stderr, "%c", (char)value);
    } else {
        DSD_FPRINTF(stderr, " ");
    }
}

static uint8_t
l3h_alias_sanitize_char(uint8_t value) {
    if (value == 0x2C) {
        return 0x2E;
    }
    if ((value > 0x19) && (value < 0x7F)) {
        return value;
    }
    return value == 0 ? 0 : 0x20;
}

static void
l3h_alias_append_policy_row(const dsd_opts* opts, dsd_state* state, uint32_t tsrc, uint32_t ttg, const char* alias) {
    if (!policy_try_append_alias(state, tsrc, "D", alias)) {
        return;
    }

    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(tsrc, "D", alias, DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &row) == 0) {
        char metadata[160];
        DSD_SNPRINTF(metadata, sizeof(metadata), "TG:%d,SYS:%03llX,RFSS:%lld,SITE:%lld,Harris", ttg, state->p2_sysid,
                     state->p2_rfssid, state->p2_siteid);
        (void)dsd_tg_policy_append_group_file_row(opts, &row, metadata);
    }
}

void
l3h_embedded_alias_decode(const dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, const uint8_t* input) {

    //storage info for storing to groupName, if not available
    char str[40];
    DSD_MEMSET(str, 0, sizeof(str));
    char ttemp[40];
    DSD_MEMSET(ttemp, 0, sizeof(ttemp));
    uint32_t tsrc = 0;
    uint32_t ttg = 0;
    l3h_alias_resolve_src_tg(state, slot, &tsrc, &ttg);

    int8_t ptr = 0;
    if (tsrc != 0) {
        DSD_FPRINTF(stderr, " TG: %d; SRC: %d; Talker Alias: ", ttg, tsrc);
    } else {
        DSD_FPRINTF(stderr, " TG: UNK; SRC: UNK; Talker Alias: ");
    }
    for (int16_t i = 4; i <= len; i++) {
        l3h_alias_print_char(input[i]);
        ttemp[ptr] = (char)l3h_alias_sanitize_char(input[i]);
        ptr++;
    }

    //assign completed talker to a more useful string instead
    DSD_SNPRINTF(str, ptr + 1, "%s", ttemp);

    if (state->event_history_s[slot].Event_History_Items[0].source_id == tsrc && tsrc != 0) {
        DSD_SPRINTF(state->event_history_s[slot].Event_History_Items[0].alias, "%s", str);
    }

    //The Duke Energy system may relay two src values, may be a good idea to pick one and stick with it
    if (tsrc != 0) {
        l3h_alias_append_policy_row(opts, state, tsrc, ttg, str);
    }

    //reset storage
    {
        uint8_t slot_idx = (slot >= 2) ? 1 : slot;
        DSD_MEMSET(state->dmr_pdu_sf[slot_idx], 0, sizeof(state->dmr_pdu_sf[slot_idx]));
    }
}

void
tait_iso7_embedded_alias_decode(const dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len,
                                const uint8_t* input) {

    UNUSED(slot);
    uint8_t alias[24];
    DSD_MEMSET(alias, 0, sizeof(alias));
    for (int16_t i = 0; i < len; i++) {
        alias[i] = (uint8_t)ConvertBitIntoBytes(&input[16 + (i * 7)], 7);
        DSD_FPRINTF(stderr, "%c", alias[i]);
        if (alias[i] == 0x2C) { //change a comma to a dot
            alias[i] = 0x2E;
        } else if (alias[i] < 0x20) { //change any garble / control chars to a space
            alias[i] = 0x20;
        }
    }

    uint32_t rid = state->lastsrc;
    uint16_t nac = state->nac;

    if (state->event_history_s[slot].Event_History_Items[0].source_id == rid) {
        DSD_SPRINTF(state->event_history_s[slot].Event_History_Items[0].alias, "%s", alias);
    }

    if (rid != 0) {
        if (policy_try_append_alias(state, rid, "D", (const char*)alias)) //not already in there, so save it there now
        {
            dsd_tg_policy_entry row;
            if (dsd_tg_policy_make_exact_entry(rid, "D", (const char*)alias, DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &row)
                == 0) {
                char metadata[64];
                DSD_SNPRINTF(metadata, sizeof(metadata), "%03X,Tait", nac);
                (void)dsd_tg_policy_append_group_file_row(opts, &row, metadata);
            }
        }
    }
}

void
dmr_talker_alias_lc_header(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    uint8_t format = (uint8_t)ConvertBitIntoBytes(&lc_bits[16], 2);
    uint8_t block_len = (uint8_t)ConvertBitIntoBytes(&lc_bits[18], 5);
    uint8_t char_size = 0;

    if (format == 0) {
        char_size = 7;
    } else if (format == 1 || format == 2) {
        char_size = 8;
    } else {
        char_size = 16;
    }

    state->dmr_alias_format[slot] = format;
    state->dmr_alias_block_len[slot] = block_len; //data len, see below
    state->dmr_alias_char_size[slot] = char_size;

    //load into dmr_pdu_sf as bit wise values for this since iso7 has 49 bits in this header, otherwise, load with 48?
    if (char_size == 7) {
        DSD_MEMCPY(state->dmr_pdu_sf[slot], lc_bits + 23, 49 * sizeof(uint8_t));
    } else if (char_size == 8 || char_size == 16) {
        DSD_MEMCPY(state->dmr_pdu_sf[slot], lc_bits + 24, 48 * sizeof(uint8_t));
    }

    //TEST: The Block Len (Data Lan) value is, according to my interpretation, the number of encoded
    //character units in the alias, test to verify, but fall back to the ptr index if necessary
    //this is referenced in  5.4.3 ETSI TS 102 361-2 V2.5.1 (2023-05) 7.2.19 Talker Alias Data Length

    DSD_FPRINTF(stderr, " Slot %d - Talker Alias LC Header; Format %d; Char Len: %d; Char Size: %d;", slot, format,
                block_len, char_size);

    //Decode the header's alias portion
    DSD_FPRINTF(stderr, "\n");
    uint16_t max_chars = 0;
    if (char_size == 7) {
        max_chars = 49 / 7;
    } else if (char_size == 8) {
        max_chars = 48 / 8;
    } else if (char_size == 16) {
        max_chars = 48 / 16;
    }
    dmr_talker_alias_lc_decode(opts, state, slot, 0, char_size, max_chars);
}

void
dmr_talker_alias_lc_blocks(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t block_num, const uint8_t* lc_bits) {
    UNUSED(opts); //delete if we don't use this, but may want it if we dump alias to a file later on
    uint8_t char_size = state->dmr_alias_char_size[slot];
    uint16_t ptr = 0;

    //Note: The Joann sample only carries block 5, and no header on block 4
    //another radio on that ham setup also has a broken alias similar, but
    //the other talkers have the 04 and the sample has a good decode otherwise
    //debug broken aliasing on ham radio

    // if (char_size == 0) //Enable this is you need to check alias even with missing header (set 7, 8, or 16)
    //   char_size = 8;

    /*
    22:54:51 Sync: +DMR   slot1  [SLOT2] | Color Code=01 | VC6 
    Slot 1 - Talker Alias Block Num: 1; Valid Block; Talker Alias:       Joann  
    DMR PDU Payload [05][00][4A][6F][61][6E][6E][00][FF]
  */

    //set the pointer to the current index of the dmr_pdu_sf
    if (char_size == 7) {
        ptr = 49 + (block_num * 56);
    } else if (char_size == 8 || char_size == 16) {
        ptr = 48 + (block_num * 56);
    }

    if (char_size == 0) { //unset, no header received
        DSD_FPRINTF(stderr, " Slot %d - Talker Alias Block Num: %d; Invalid Header;", slot, block_num + 1);
    }

    else if (block_num > 3) { //invalid block (data error)
        DSD_FPRINTF(stderr, " Slot %d - Talker Alias Block Num: %d; Invalid Block;", slot, block_num + 1);
    }

    else //valid header present, continue
    {
        uint16_t max_chars = 0;
        if (char_size == 7) {
            DSD_MEMCPY(state->dmr_pdu_sf[slot] + ptr, lc_bits + 16, 56 * sizeof(uint8_t));
            ptr += 56;
            max_chars = ptr / 7;
        } else if (char_size == 8) {
            DSD_MEMCPY(state->dmr_pdu_sf[slot] + ptr, lc_bits + 16, 56 * sizeof(uint8_t));
            ptr += 56;
            max_chars = ptr / 8;
        } else if (char_size == 16) {
            DSD_MEMCPY(state->dmr_pdu_sf[slot] + ptr, lc_bits + 16, 56 * sizeof(uint8_t));
            ptr += 56;
            max_chars = ptr / 16;
        }

        dmr_talker_alias_lc_decode(opts, state, slot, block_num + 1, char_size, max_chars);
    }
}

static uint16_t
dmr_talker_alias_effective_len(const uint8_t* bits, uint8_t char_size, uint16_t max_chars) {
    uint16_t last = 0;
    for (uint16_t i = 0; i < max_chars; i++) {
        if (char_size == 7) {
            uint8_t character = (uint8_t)ConvertBitIntoBytes((uint8_t*)&bits[((size_t)i * 7)], 7);
            if (character >= 0x21 && character <= 0x7E) {
                last = i + 1;
            }
        } else if (char_size == 8) {
            uint8_t character = (uint8_t)ConvertBitIntoBytes((uint8_t*)&bits[((size_t)i * 8)], 8);
            if (character >= 0x21 && character != 0x7F && character != 0xFF) {
                last = i + 1;
            }
        } else if (char_size == 16) {
            uint16_t character = (uint16_t)ConvertBitIntoBytes((uint8_t*)&bits[((size_t)i * 16)], 16);
            if (character >= 0x21 && character != 0x7F && character != 0xFFFF) {
                last = i + 1;
            }
        }
    }
    return last;
}

static void
dmr_talker_alias_append_text(char* alias_string, size_t alias_size, const char* text) {
    size_t rem = alias_size - strlen(alias_string) - 1;
    if (rem > 0) {
        DSD_STRNCAT(alias_string, text, rem);
    }
}

static void
dmr_talker_alias_append_char(char* alias_string, size_t alias_size, char character) {
    char ch[2];
    ch[0] = character;
    ch[1] = 0;
    dmr_talker_alias_append_text(alias_string, alias_size, ch);
}

static void
dmr_talker_alias_decode_iso7(const uint8_t* bits, uint16_t end, char* alias_string, size_t alias_size) {
    for (uint16_t i = 0; i < end; i++) {
        uint8_t character = (uint8_t)ConvertBitIntoBytes((uint8_t*)&bits[((size_t)i * 7)], 7);
        if (character >= 0x20 && character <= 0x7E) {
            DSD_FPRINTF(stderr, "%c", character);
            dmr_talker_alias_append_char(alias_string, alias_size, (char)character);
        } else {
            DSD_FPRINTF(stderr, " ");
            dmr_talker_alias_append_text(alias_string, alias_size, " ");
        }
    }
}

static void
dmr_talker_alias_decode_iso8(const uint8_t* bits, uint16_t end, char* alias_string, size_t alias_size) {
    for (uint16_t i = 0; i < end; i++) {
        uint8_t character = (uint8_t)ConvertBitIntoBytes((uint8_t*)&bits[((size_t)i * 8)], 8);
        if (character >= 0x20 && character != 0x7F && character != 0xFF) {
            DSD_FPRINTF(stderr, "%c", character);
            dmr_talker_alias_append_char(alias_string, alias_size, (char)character);
        } else {
            DSD_FPRINTF(stderr, " ");
            dmr_talker_alias_append_text(alias_string, alias_size, " ");
        }
    }
}

static void
dmr_talker_alias_print_utf16_char(uint16_t character) {
    if (character >= 0x20 && character != 0x7F && character != 0xFFFF) {
        if (dsd_unicode_supported()) {
            DSD_FPRINTF(stderr, "%lc", character);
        } else {
            unsigned char lo = (unsigned char)(character & 0xFF);
            if (lo >= 0x20 && lo < 0x7F) {
                fputc((int)lo, stderr);
            } else {
                fputc('?', stderr);
            }
        }
    } else {
        DSD_FPRINTF(stderr, " ");
    }
}

static void
dmr_talker_alias_collect_utf16_char(uint16_t character, char* alias_string, size_t alias_size) {
    if (character == 0) {
        dmr_talker_alias_append_text(alias_string, alias_size, " ");
    } else if (character >= 0x20 && character <= 0xFE) {
        dmr_talker_alias_append_char(alias_string, alias_size, (char)(character & 0xFF));
    } else {
        dmr_talker_alias_append_text(alias_string, alias_size, "*");
    }
}

static void
dmr_talker_alias_decode_utf16(const uint8_t* bits, uint16_t end, char* alias_string, size_t alias_size) {
    setlocale(
        LC_ALL,
        ""); //needed when encoded alias contains Chinese (or probably any non-roman charset that isn't default on users terminal)
    for (uint16_t i = 0; i < end; i++) {
        uint16_t character = (uint16_t)ConvertBitIntoBytes((uint8_t*)&bits[((size_t)i * 16)], 16);
        dmr_talker_alias_print_utf16_char(character);
        dmr_talker_alias_collect_utf16_char(character, alias_string, alias_size);
    }
}

static uint32_t
dmr_talker_alias_source_for_slot(const dsd_state* state, uint8_t slot) {
    if (slot == 0) {
        return state->lastsrc;
    }
    return state->lastsrcR;
}

//Decode partial or completed alias
void
dmr_talker_alias_lc_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t block_num, uint8_t char_size,
                           uint16_t max_chars) {
    UNUSED(opts);
    DSD_FPRINTF(stderr, " Slot %d - Talker Alias Block Num: %d; Valid Block;", slot, block_num + 1);
    DSD_FPRINTF(stderr, " Talker Alias: ");

    char alias_string[500];
    DSD_MEMSET(alias_string, 0, sizeof(alias_string));
    DSD_SPRINTF(alias_string, "%s", "");

    const uint16_t end = dmr_talker_alias_effective_len(state->dmr_pdu_sf[slot], char_size, max_chars);

    if (char_size == 7) {
        dmr_talker_alias_decode_iso7(state->dmr_pdu_sf[slot], end, alias_string, sizeof(alias_string));
    } else if (char_size == 8) {
        dmr_talker_alias_decode_iso8(state->dmr_pdu_sf[slot], end, alias_string, sizeof(alias_string));
    } else if (char_size == 16) {
        dmr_talker_alias_decode_utf16(state->dmr_pdu_sf[slot], end, alias_string, sizeof(alias_string));
    }

    //assign to string for event history and ncurses display
    uint32_t source = dmr_talker_alias_source_for_slot(state, slot);

    if (state->event_history_s[slot].Event_History_Items[0].source_id == source) {
        DSD_SPRINTF(state->event_history_s[slot].Event_History_Items[0].alias, "%s; ", alias_string);
    }
    DSD_SPRINTF(state->generic_talker_alias[slot], "Talker Alias: %s; ", alias_string);
    state->generic_talker_alias_src[slot] = source;
}
