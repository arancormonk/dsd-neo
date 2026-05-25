// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * dmr_block.c
 * DMR Data Header and Data Block Assembly/Handling
 *
 * LWVMOBILE
 * 2022-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/bp.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/rc4.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/pdu.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define DMR_PDU_DECRYPTION //disable to skip attempting to decrypt DMR PDUs

// Bounded string append helper (implemented later in file)
static inline void dsd_append(char* dst, size_t dstsz, const char* src);

typedef struct {
    uint8_t a;
    uint8_t bf;
    uint8_t dd_format;
    uint8_t dpf;
    uint8_t f;
    uint8_t fsn;
    uint8_t gi;
    uint8_t is_xpt;
    uint8_t p_mfid;
    uint8_t p_sap;
    uint8_t poc;
    uint8_t r_class;
    uint8_t r_status;
    uint8_t r_type;
    uint8_t s;
    uint8_t s_ab_fin;
    uint8_t s_dest_port;
    uint8_t s_source_port;
    uint8_t s_status_precoded;
    uint8_t sap;
    uint8_t sd_bp;
    uint8_t sd_f;
    uint8_t sd_sarq;
    uint8_t tg_hash;
    uint8_t udt_format;
    uint8_t udt_op;
    uint8_t udt_padnib;
    uint8_t udt_pf;
    uint8_t udt_sf;
    uint8_t udt_uab;
    uint8_t ns;
    uint32_t source;
    uint32_t target;
    char mfid_string[20];
    char sap_string[20];
    char sddd_string[20];
    char udtf_string[20];
} dmr_dheader_fields;

static void
dmr_clear_superframe_slot(dsd_state* state, uint8_t slot) {
    for (int i = 0; i < 24 * 127; i++) {
        state->dmr_pdu_sf[slot][i] = 0;
    }
}

static uint8_t
dmr_branding_is(const dsd_state* state, const char* value) {
    return (uint8_t)(strcmp(state->dmr_branding_sub, value) == 0);
}

static const char*
dmr_dpf_string(uint8_t dpf) {
    switch (dpf) {
        case 0: return "Unified Data Transport (UDT) ";
        case 1: return "Response Packet ";
        case 2: return "Unconfirmed Delivery ";
        case 3: return "Confirmed Delivery ";
        case 13: return "Short Data: Defined ";
        case 14: return "Short Data: Raw or S/P ";
        case 15: return "Extended";
        default: return NULL;
    }
}

static const char*
dmr_sap_string(uint8_t sap, uint8_t p_mfid) {
    switch (sap) {
        case 0: return "UDT Data";
        case 2: return "TCP Comp";
        case 3: return "UDP Comp";
        case 4: return "IP Based";
        case 5: return "ARP Prot";
        case 9: return "EXTD HDR";
        case 10: return "Short DT";
        case 1: return (p_mfid == 0x10) ? "Moto NET" : "Reserved";
        default: return "Reserved";
    }
}

static const char*
dmr_mfid_string(uint8_t p_mfid) {
    switch (p_mfid) {
        case 0x10: return "Moto";
        case 0x77: return "Vertex";
        case 0x58: return "Tait";
        case 0x68:
        case 0x08: return "Hytera";
        case 0x06: return "Trid/Mot";
        case 0x00: return "Standard";
        default: return "Other";
    }
}

static const char*
dmr_udt_format_string(uint8_t udt_format) {
    static const char* const map[] = {"Binary",  "MS/TG Adr", "4-bit BCD", "ISO7 Char", "ISO8 Char", "NMEA LOCN",
                                      "IP Addr", "UTF-16",    "Manu Spec", "Manu Spec", "Mixed UTF", "LIP LOCN"};
    if (udt_format < (sizeof(map) / sizeof(map[0]))) {
        return map[udt_format];
    }
    return "Reserved";
}

static const char*
dmr_dd_format_string(uint8_t dd_format) {
    static const char* const map[] = {"Binary",      "BCD   ",      "7-bit Char",  "IEC 8859-1",  "IEC 8859-2",
                                      "IEC 8859-3",  "IEC 8859-4",  "IEC 8859-5",  "IEC 8859-6",  "IEC 8859-7",
                                      "IEC 8859-8",  "IEC 8859-9",  "IEC 8859-10", "IEC 8859-11", "IEC 8859-13",
                                      "IEC 8859-14", "IEC 8859-15", "IEC 8859-16", "UTF-8   ",    "UTF-16  ",
                                      "UTF-16BE",    "UTF-16LE",    "UTF-32  ",    "UTF-32BE",    "UTF-32LE"};
    if (dd_format < (sizeof(map) / sizeof(map[0]))) {
        return map[dd_format];
    }
    return "Reserved";
}

static void
dmr_dheader_parse_fields(const dsd_state* state, const uint8_t dheader_bits[], dmr_dheader_fields* f) {
    uint8_t mpoc = dheader_bits[3];
    uint8_t s_ab_msb = (uint8_t)ConvertBitIntoBytes(&dheader_bits[2], 2);
    uint8_t s_ab_lsb = (uint8_t)ConvertBitIntoBytes(&dheader_bits[12], 4);
    f->gi = dheader_bits[0];
    f->a = dheader_bits[1];
    f->dpf = (uint8_t)ConvertBitIntoBytes(&dheader_bits[4], 4);
    f->sap = (uint8_t)ConvertBitIntoBytes(&dheader_bits[8], 4);
    f->poc = (uint8_t)ConvertBitIntoBytes(&dheader_bits[12], 4) + (mpoc << 4);
    f->target = (uint32_t)ConvertBitIntoBytes(&dheader_bits[16], 24);
    f->source = (uint32_t)ConvertBitIntoBytes(&dheader_bits[40], 24);
    f->is_xpt = dmr_branding_is(state, "XPT ");
    if (f->is_xpt == 1) {
        f->target = (uint32_t)ConvertBitIntoBytes(&dheader_bits[24], 16);
        f->source = (uint32_t)ConvertBitIntoBytes(&dheader_bits[48], 16);
        if (f->gi == 0) {
            uint8_t target_hash[16];
            for (int i = 0; i < 16; i++) {
                target_hash[i] = dheader_bits[24 + i];
            }
            f->tg_hash = crc8(target_hash, 16);
        }
    }
    if (dmr_branding_is(state, "Cap+ ")) {
        if (f->gi == 0) {
            f->target = (uint32_t)ConvertBitIntoBytes(&dheader_bits[24], 16);
        }
        f->source = (uint32_t)ConvertBitIntoBytes(&dheader_bits[48], 16);
    }
    f->f = dheader_bits[64];
    f->bf = (uint8_t)ConvertBitIntoBytes(&dheader_bits[65], 7);
    f->s = dheader_bits[72];
    f->ns = (uint8_t)ConvertBitIntoBytes(&dheader_bits[73], 3);
    f->fsn = (uint8_t)ConvertBitIntoBytes(&dheader_bits[76], 4);
    f->r_class = (uint8_t)ConvertBitIntoBytes(&dheader_bits[72], 2);
    f->r_type = (uint8_t)ConvertBitIntoBytes(&dheader_bits[74], 3);
    f->r_status = (uint8_t)ConvertBitIntoBytes(&dheader_bits[77], 3);
    f->s_ab_fin = (uint8_t)((s_ab_msb << 2) | s_ab_lsb);
    f->s_source_port = (uint8_t)ConvertBitIntoBytes(&dheader_bits[64], 3);
    f->s_dest_port = (uint8_t)ConvertBitIntoBytes(&dheader_bits[67], 3);
    f->s_status_precoded = (uint8_t)ConvertBitIntoBytes(&dheader_bits[70], 10);
    f->sd_sarq = dheader_bits[70];
    f->sd_f = dheader_bits[71];
    f->sd_bp = (uint8_t)ConvertBitIntoBytes(&dheader_bits[72], 8);
    f->dd_format = (uint8_t)ConvertBitIntoBytes(&dheader_bits[64], 6);
    f->udt_format = (uint8_t)ConvertBitIntoBytes(&dheader_bits[12], 4);
    f->udt_padnib = (uint8_t)ConvertBitIntoBytes(&dheader_bits[64], 5);
    f->udt_uab = (uint8_t)ConvertBitIntoBytes(&dheader_bits[70], 2);
    f->udt_sf = dheader_bits[72];
    f->udt_pf = dheader_bits[73];
    f->udt_op = (uint8_t)ConvertBitIntoBytes(&dheader_bits[74], 6);
    f->udt_uab += 1;
    f->p_sap = (uint8_t)ConvertBitIntoBytes(&dheader_bits[0], 4);
    f->p_mfid = (uint8_t)ConvertBitIntoBytes(&dheader_bits[8], 8);
}

static void
dmr_dheader_init_strings(dmr_dheader_fields* f) {
    DSD_SNPRINTF(f->sap_string, sizeof f->sap_string, "%s", "");
    DSD_SNPRINTF(f->mfid_string, sizeof f->mfid_string, "%s", "");
    DSD_SNPRINTF(f->udtf_string, sizeof f->udtf_string, "%s", "");
    DSD_SNPRINTF(f->sddd_string, sizeof f->sddd_string, "%s", "");
}

static void
dmr_dheader_set_strings(dmr_dheader_fields* f) {
    if (f->dpf == 15) {
        f->sap = f->p_sap;
    }
    DSD_SNPRINTF(f->sap_string, sizeof f->sap_string, "%s", dmr_sap_string(f->sap, f->p_mfid));
    if (f->dpf == 15) {
        DSD_SNPRINTF(f->mfid_string, sizeof f->mfid_string, "%s", dmr_mfid_string(f->p_mfid));
    }
    if (f->dpf == 0) {
        DSD_SNPRINTF(f->udtf_string, sizeof f->udtf_string, "%s", dmr_udt_format_string(f->udt_format));
    }
    if (f->dpf == 13) {
        DSD_SNPRINTF(f->sddd_string, sizeof f->sddd_string, "%s", dmr_dd_format_string(f->dd_format));
    }
}

static void
dmr_dheader_set_udt_reserved(dsd_state* state, uint8_t slot, const dmr_dheader_fields* f) {
    state->udt_uab_reserved[slot] = 0;
    if (f->dpf == 0 && f->udt_format == 0x05 && f->udt_uab == 3) {
        state->udt_uab_reserved[slot] = 1;
    }
}

static void
dmr_dheader_print_banner(uint8_t slot, const dmr_dheader_fields* f) {
    char hdr[512];
    int off = 0;
    off += DSD_SNPRINTF(hdr + off, sizeof(hdr) - off, "%s \n Slot %d Data Header - ", KGRN, slot + 1);
    if (f->dpf != 15) {
        if (f->gi == 1) {
            off += DSD_SNPRINTF(hdr + off, sizeof(hdr) - off, "Group - ");
        } else if (f->gi == 0) {
            off += DSD_SNPRINTF(hdr + off, sizeof(hdr) - off, "Indiv - ");
        }
    }
    const char* dpf_str = dmr_dpf_string(f->dpf);
    if (dpf_str != NULL) {
        off += DSD_SNPRINTF(hdr + off, sizeof(hdr) - off, "%s", dpf_str);
    } else {
        off += DSD_SNPRINTF(hdr + off, sizeof(hdr) - off, "Reserved/Unknown DPF %X ", f->dpf);
    }
    if (f->a == 1 && f->dpf != 15) {
        off += DSD_SNPRINTF(hdr + off, sizeof(hdr) - off, "- Response Requested ");
    }
    if (f->dpf != 15) {
        off += DSD_SNPRINTF(hdr + off, sizeof(hdr) - off, "- Source: %d Target: %d ", f->source, f->target);
    }
    if (f->dpf != 15 && f->is_xpt == 1 && f->gi == 0) {
        (void)DSD_SNPRINTF(hdr + off, sizeof(hdr) - off, "Hash: %d ", f->tg_hash);
    }
    DSD_FPRINTF(stderr, "%s", hdr);
}

static void
dmr_dheader_handle_udt(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t slot, const dmr_dheader_fields* f) {
    if (state->udt_uab_reserved[slot]) {
        DSD_FPRINTF(stderr,
                    "\n  SAP %02d [%s] - FMT %d [%s] - PDn %d - BLOCKS %d (reserved/unknown) SF %d - PF %d OP %02X",
                    f->sap, f->sap_string, f->udt_format, f->udtf_string, f->udt_padnib, f->udt_uab, f->udt_sf,
                    f->udt_pf, f->udt_op);
    } else {
        DSD_FPRINTF(stderr, "\n  SAP %02d [%s] - FMT %d [%s] - PDn %d - BLOCKS %d SF %d - PF %d OP %02X", f->sap,
                    f->sap_string, f->udt_format, f->udtf_string, f->udt_padnib, f->udt_uab, f->udt_sf, f->udt_pf,
                    f->udt_op);
    }
    state->data_header_blocks[slot] = f->udt_uab;
    state->data_header_valid[slot] = 1;
    state->data_block_counter[slot] = 0;
    dmr_block_assembler(opts, state, dheader, 12, 0x0B, 3);
}

static void
dmr_dheader_handle_response(uint8_t slot, const dmr_dheader_fields* f) {
    char rsp_string[200];
    DSD_MEMSET(rsp_string, 0, sizeof(rsp_string));
    DSD_SPRINTF(rsp_string, "DATA RESP TGT: %d; SRC: %d; ", f->target, f->source);
    if (f->r_class == 0 && f->r_type == 1) {
        dsd_append(rsp_string, sizeof rsp_string, "ACK - Success");
    }
    if (f->r_class == 1) {
        dsd_append(rsp_string, sizeof rsp_string, "NACK - ");
        if (f->r_type == 0) {
            dsd_append(rsp_string, sizeof rsp_string, "Illegal Format");
        }
        if (f->r_type == 1) {
            dsd_append(rsp_string, sizeof rsp_string, "Illegal Format");
        }
        if (f->r_type == 2) {
            dsd_append(rsp_string, sizeof rsp_string, "Packet CRC ERR");
        }
        if (f->r_type == 3) {
            dsd_append(rsp_string, sizeof rsp_string, "Memory Full");
        }
        if (f->r_type == 4) {
            dsd_append(rsp_string, sizeof rsp_string, "FSN Out of Seq");
        }
        if (f->r_type == 5) {
            dsd_append(rsp_string, sizeof rsp_string, "Undeliverable");
        }
        if (f->r_type == 6) {
            dsd_append(rsp_string, sizeof rsp_string, "PKT Out of Seq");
        }
        if (f->r_type == 7) {
            dsd_append(rsp_string, sizeof rsp_string, "Invalid User");
        }
    }
    if (f->r_class == 2) {
        dsd_append(rsp_string, sizeof rsp_string, "SACK - Retry");
    }
    UNUSED(f->r_status);
    UNUSED(slot);
    DSD_FPRINTF(stderr, "\n %s", rsp_string);
}

static void
dmr_dheader_handle_unconfirmed_or_confirmed(dsd_state* state, uint8_t slot, const dmr_dheader_fields* f) {
    if (f->dpf == 2) {
        DSD_FPRINTF(stderr, "\n  SAP %02d [%s] - FMF %d - BLOCKS %02d - PAD %02d - FSN %d", f->sap, f->sap_string, f->f,
                    f->bf, f->poc, f->fsn);
    } else {
        DSD_FPRINTF(stderr, "\n  SAP %02d [%s] - FMF %d - BLOCKS %02d - PAD %02d - S %d - NS %d - FSN %d", f->sap,
                    f->sap_string, f->f, f->bf, f->poc, f->s, f->ns, f->fsn);
    }
    state->data_header_blocks[slot] = f->bf;
    if (f->dpf == 3) {
        state->data_conf_data[slot] = 1;
    }
}

static void
dmr_dheader_handle_short_data(dsd_state* state, uint8_t slot, const dmr_dheader_fields* f) {
    if (f->s_ab_fin) {
        state->data_header_blocks[slot] = f->s_ab_fin;
    }
    if (f->dpf == 13) {
        DSD_FPRINTF(stderr, "\n  SD:D [DD_HEAD] - SAP %02d [%s] - BLOCKS %02d - DD %02X - PADb %d - FMT %02X [%s]",
                    f->sap, f->sap_string, f->s_ab_fin, f->dd_format, f->sd_bp, f->dd_format, f->sddd_string);
    }
    if (f->dpf == 14) {
        if (f->s_ab_fin == 0) {
            DSD_FPRINTF(stderr, "\n  SD:S/P [SP_HEAD] - SAP %02d [%s] - SP %02d - DP %02d - S/P %02X", f->sap,
                        f->sap_string, f->s_source_port, f->s_dest_port, f->s_status_precoded);
        } else {
            DSD_FPRINTF(stderr,
                        "\n  SD:RAW [R_HEAD] - SAP %02d [%s] - BLOCKS %02d - SP %02d - DP %02d - SARQ %d - FMF %d - "
                        "PDb %d",
                        f->sap, f->sap_string, f->s_ab_fin, f->s_source_port, f->s_dest_port, f->sd_sarq, f->sd_f,
                        f->sd_bp);
        }
    }
    if (f->a == 1) {
        state->data_conf_data[slot] = 1;
        DSD_FPRINTF(stderr, " - Confirmed Data");
    }
}

static void
dmr_dheader_handle_moto_p_head(dsd_state* state, uint8_t slot, uint8_t dheader[], const uint8_t dheader_bits[]) {
    size_t start = 0;
    size_t len = 10U - start;
    DSD_MEMCPY(state->dmr_pdu_sf[slot], dheader + start, len * sizeof(uint8_t));
    state->data_block_counter[slot]++;
    state->data_byte_ctr[slot] = (uint16_t)len;
    state->data_p_head[slot] = 1;
    uint8_t p_opcode = (uint8_t)ConvertBitIntoBytes(&dheader_bits[16], 8);
    state->data_ks_start[slot] = (p_opcode == 0x02) ? 3 : 0;
}

static void
dmr_dheader_handle_vertex_enc(dsd_state* state, uint8_t slot, uint8_t p_mfid, const uint8_t dheader_bits[]) {
    uint8_t key_id = (uint8_t)ConvertBitIntoBytes(&dheader_bits[16], 8);
    uint32_t mi32 = (uint32_t)ConvertBitIntoBytes(&dheader_bits[48], 32);
    if (state->currentslot == 0) {
        state->dmr_so = 0x100;
        state->payload_keyid = key_id;
        state->payload_algid = 0x07;
        state->payload_mi = (unsigned long long int)mi32;
    } else {
        state->dmr_soR = 0x100;
        state->payload_keyidR = key_id;
        state->payload_algidR = 0x07;
        state->payload_miR = (unsigned long long int)mi32;
    }
    DSD_FPRINTF(stderr, "\n Vertex PDU ENC Header:");
    DSD_FPRINTF(stderr, " MFID: %02X;", p_mfid);
    DSD_FPRINTF(stderr, " Key ID: %02X;", key_id);
    DSD_FPRINTF(stderr, " ALG: 07; VTX STD;");
    if (mi32 != 0U) {
        DSD_FPRINTF(stderr, " MI(32): %08X", mi32);
    }
    state->data_ks_start[slot] = 0;
}

static void
dmr_dheader_print_alg_label(uint8_t alg) {
    if (alg == 0) {
        DSD_FPRINTF(stderr, " BP;");
    }
    if (alg == 1) {
        DSD_FPRINTF(stderr, " RC4;");
    }
    if (alg == 2) {
        DSD_FPRINTF(stderr, " DES56;");
    }
    if (alg == 4) {
        DSD_FPRINTF(stderr, " AES128;");
    }
    if (alg == 5) {
        DSD_FPRINTF(stderr, " AES256;");
    }
}

static void
dmr_dheader_handle_moto_enc(dsd_state* state, uint8_t slot, const uint8_t dheader_bits[]) {
    uint8_t enc = (uint8_t)ConvertBitIntoBytes(&dheader_bits[20], 4);
    uint8_t key_id = (uint8_t)ConvertBitIntoBytes(&dheader_bits[24], 8);
    uint8_t alg = (uint8_t)ConvertBitIntoBytes(&dheader_bits[17], 3);
    uint32_t mi32 = (uint32_t)ConvertBitIntoBytes(&dheader_bits[48], 32);
    if (enc == 1) {
        if (state->currentslot == 0) {
            state->dmr_so = 0x100;
        } else {
            state->dmr_soR = 0x100;
        }
    }
    DSD_FPRINTF(stderr, "\n PDU ENC Header:");
    DSD_FPRINTF(stderr, " MFID: %02X;", (uint8_t)ConvertBitIntoBytes(&dheader_bits[8], 8));
    DSD_FPRINTF(stderr, " ENC: %X;", enc);
    if (state->currentslot == 0) {
        state->payload_keyid = key_id;
        state->payload_algid = alg;
        state->payload_mi = (unsigned long long int)mi32;
    } else {
        state->payload_keyidR = key_id;
        state->payload_algidR = alg;
        state->payload_miR = (unsigned long long int)mi32;
    }
    DSD_FPRINTF(stderr, " Key ID: %02X;", key_id);
    DSD_FPRINTF(stderr, " ALG: %02X;", alg);
    dmr_dheader_print_alg_label(alg);
    if (mi32 != 0) {
        DSD_FPRINTF(stderr, " MI(32): %08X", mi32);
    }
    state->data_ks_start[slot] = 0;
}

static void
dmr_dheader_print_unknown_ext(const uint8_t dheader_bits[]) {
    DSD_FPRINTF(stderr, "\n Unknown Extended Header: ");
    for (uint8_t i = 2; i < 10; i++) {
        DSD_FPRINTF(stderr, "%02X", (uint8_t)ConvertBitIntoBytes(&dheader_bits[((size_t)i) * 8u], 8));
    }
}

static void
dmr_dheader_handle_proprietary(dsd_state* state, uint8_t slot, uint8_t dheader[], const uint8_t dheader_bits[],
                               const dmr_dheader_fields* f) {
    DSD_FPRINTF(stderr, " - SAP %02d [%s] - MFID %02X [%s]", f->p_sap, f->sap_string, f->p_mfid, f->mfid_string);
    if (f->p_mfid == 0x10 && f->p_sap == 1) {
        dmr_dheader_handle_moto_p_head(state, slot, dheader, dheader_bits);
    } else {
        if (state->data_header_blocks[slot] > 1) {
            state->data_header_blocks[slot]--;
        }
        state->data_byte_ctr[slot] = 0;
    }
    if (f->p_sap != 1 && f->p_mfid == 0x77) {
        dmr_dheader_handle_vertex_enc(state, slot, f->p_mfid, dheader_bits);
    } else if (f->p_sap != 1 && f->p_mfid == 0x10) {
        dmr_dheader_handle_moto_enc(state, slot, dheader_bits);
    } else if (f->p_sap == 1 && f->p_mfid == 0x10) {
        DSD_FPRINTF(stderr, "\n Motorola Network Interface Service Header (MNIS); ");
    } else {
        dmr_dheader_print_unknown_ext(dheader_bits);
    }
}

static void
dmr_dheader_reset_non_extended(dsd_state* state, uint8_t slot) {
    if (state->currentslot == 0) {
        state->payload_mi = 0;
        state->payload_algid = 0;
        state->payload_keyid = 0;
        state->dmr_so = 0;
    } else {
        state->payload_miR = 0;
        state->payload_algidR = 0;
        state->payload_keyidR = 0;
        state->dmr_soR = 0;
    }
    state->data_byte_ctr[slot] = 0;
}

static void
dmr_dheader_sanitize_blocks(dsd_state* state, uint8_t slot) {
    if (state->data_header_blocks[slot] > 127) {
        state->data_header_blocks[slot] = 127;
    }
    if (state->data_header_blocks[slot] < 1) {
        state->data_header_blocks[slot] = 1;
    }
}

static void
dmr_dheader_reset_irrecoverable(dsd_state* state, uint8_t slot) {
    state->data_header_valid[slot] = 0;
    DSD_SPRINTF(state->dmr_lrrp_gps[slot], "%s", "");
    state->data_p_head[slot] = 0;
    state->data_conf_data[slot] = 0;
    state->data_block_counter[slot] = 1;
    state->data_header_blocks[slot] = 1;
    state->data_header_format[slot] = 7;
}

static void
dmr_dheader_handle_by_format(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t dheader_bits[], uint8_t slot,
                             const dmr_dheader_fields* f) {
    switch (f->dpf) {
        case 0: dmr_dheader_handle_udt(opts, state, dheader, slot, f); break;
        case 1: dmr_dheader_handle_response(slot, f); break;
        case 2:
        case 3: dmr_dheader_handle_unconfirmed_or_confirmed(state, slot, f); break;
        case 13:
        case 14: dmr_dheader_handle_short_data(state, slot, f); break;
        case 15: dmr_dheader_handle_proprietary(state, slot, dheader, dheader_bits, f); break;
        default: break;
    }

    if (f->dpf != 15) {
        dmr_dheader_reset_non_extended(state, slot);
    }
}

//hopefully a more simplified (or logical) version...once you get past all the variables
void
dmr_dheader(dsd_opts* opts, dsd_state* state, uint8_t dheader[], uint8_t dheader_bits[], uint32_t CRCCorrect,
            uint32_t IrrecoverableErrors) {
    uint8_t slot = state->currentslot;
    dmr_dheader_fields f;
    DSD_MEMSET(&f, 0, sizeof(f));
    dmr_clear_superframe_slot(state, slot);
    state->data_block_counter[slot] = 1;
    if (IrrecoverableErrors != 0) {
        dmr_dheader_reset_irrecoverable(state, slot);
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }

    if (!(CRCCorrect == 1 || opts->aggressive_framesync == 0 || opts->dmr_crc_relaxed_default)) {
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }

    state->data_dbsn_have[slot] = 0;
    state->data_dbsn_expected[slot] = 0;
    dmr_dheader_parse_fields(state, dheader_bits, &f);
    if (f.dpf != 15) {
        state->dmr_lrrp_source[slot] = f.source;
        state->dmr_lrrp_target[slot] = f.target;
        state->data_block_poc[slot] = f.poc;
    }

    state->data_header_format[slot] = f.dpf;
    dmr_dheader_init_strings(&f);
    dmr_dheader_set_udt_reserved(state, slot, &f);
    dmr_dheader_print_banner(slot, &f);
    dmr_dheader_set_strings(&f);
    dmr_dheader_handle_by_format(opts, state, dheader, dheader_bits, slot, &f);
    dmr_dheader_sanitize_blocks(state, slot);
    if (f.dpf != 15) {
        state->data_header_valid[slot] = 1;
    }
    if (f.dpf != 1 && f.dpf != 15) {
        DSD_SPRINTF(state->dmr_lrrp_gps[slot], "Data Call - %s TGT: %d SRC: %d ", f.sap_string, f.target, f.source);
        if (f.a == 1) {
            dsd_append(state->dmr_lrrp_gps[slot], sizeof state->dmr_lrrp_gps[slot], "- RSP REQ ");
        }
    }
    state->data_header_sap[slot] = f.sap;
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void dmr_udt_decoder(dsd_opts* opts, dsd_state* state, const uint8_t* block_bytes, uint32_t CRCCorrect);

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    const uint8_t* block_bytes;
    uint8_t cs_bits[8 * 12 * 5];
    uint8_t add_ok;
    uint8_t add_res;
    uint8_t slot;
    uint8_t udt_format2;
    uint8_t udt_uab;
    uint32_t udt_source;
    uint32_t udt_target;
    int payload_bits;
    char udt_string[500];
} dmr_udt_ctx;

static void
dmr_unpack_bytes_to_bits(const uint8_t* src, int src_len, uint8_t* dst) {
    for (int i = 0, j = 0; i < src_len; i++, j += 8) {
        dst[j + 0] = (src[i] >> 7) & 0x01;
        dst[j + 1] = (src[i] >> 6) & 0x01;
        dst[j + 2] = (src[i] >> 5) & 0x01;
        dst[j + 3] = (src[i] >> 4) & 0x01;
        dst[j + 4] = (src[i] >> 3) & 0x01;
        dst[j + 5] = (src[i] >> 2) & 0x01;
        dst[j + 6] = (src[i] >> 1) & 0x01;
        dst[j + 7] = (src[i] >> 0) & 0x01;
    }
}

static int
dmr_udt_payload_bits(dsd_state* state, uint8_t slot, uint8_t udt_padnib) {
    int app_blocks = state->data_block_counter[slot];
    if (app_blocks < 0) {
        app_blocks = 0;
    }
    if (app_blocks > 4) {
        app_blocks = 4;
    }
    int payload_bits_total = (app_blocks * 96) - 16;
    if (payload_bits_total < 0) {
        payload_bits_total = 0;
    }
    int pad_bits = (int)udt_padnib * 4;
    if (pad_bits > payload_bits_total) {
        pad_bits = payload_bits_total;
    }
    int payload_bits = payload_bits_total - pad_bits;
    if (payload_bits < 0) {
        payload_bits = 0;
    }
    return payload_bits;
}

static void
dmr_udt_prepare_context(dmr_udt_ctx* ctx, dsd_opts* opts, dsd_state* state, const uint8_t* block_bytes) {
    uint8_t udt_ig;
    uint8_t udt_a;
    uint8_t udt_res;
    uint8_t udt_format1;
    uint8_t udt_sap;
    uint8_t udt_padnib;
    uint8_t udt_zero;
    uint8_t udt_sf;
    uint8_t udt_pf;
    uint8_t udt_op;
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->opts = opts;
    ctx->state = state;
    ctx->block_bytes = block_bytes;
    ctx->slot = state->currentslot;
    dmr_unpack_bytes_to_bits(block_bytes, 60, ctx->cs_bits);
    udt_ig = ctx->cs_bits[0];
    udt_a = ctx->cs_bits[1];
    udt_res = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[2], 2);
    udt_format1 = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[4], 4);
    udt_sap = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[8], 4);
    ctx->udt_format2 = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[12], 4);
    ctx->udt_target = (uint32_t)ConvertBitIntoBytes(&ctx->cs_bits[16], 24);
    ctx->udt_source = (uint32_t)ConvertBitIntoBytes(&ctx->cs_bits[40], 24);
    udt_padnib = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[64], 5);
    udt_zero = ctx->cs_bits[69];
    ctx->udt_uab = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[70], 2) + 1;
    udt_sf = ctx->cs_bits[72];
    udt_pf = ctx->cs_bits[73];
    udt_op = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[74], 6);
    UNUSED4(udt_ig, udt_a, udt_res, udt_format1);
    UNUSED4(udt_sap, udt_zero, udt_sf, udt_pf);
    UNUSED(udt_op);
    ctx->payload_bits = dmr_udt_payload_bits(state, ctx->slot, udt_padnib);
    ctx->add_res = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[96], 7);
    ctx->add_ok = ctx->cs_bits[103];
    DSD_MEMSET(ctx->udt_string, 0, sizeof(ctx->udt_string));
    DSD_SPRINTF(ctx->udt_string, "UDT SRC: %d; TGT: %d; ", ctx->udt_source, ctx->udt_target);
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n ");
    DSD_FPRINTF(stderr, "Slot %d - SRC: %d; TGT: %d; UDT ", ctx->slot + 1, ctx->udt_source, ctx->udt_target);
}

static void
dmr_udt_append_text_event(dsd_state* state, uint8_t slot, char c) {
    char tmp[2];
    tmp[0] = c;
    tmp[1] = 0;
    dsd_append(state->event_history_s[slot].Event_History_Items[0].text_message,
               sizeof state->event_history_s[slot].Event_History_Items[0].text_message, tmp);
}

static void
dmr_udt_print_utf16_char(uint16_t utf16c) {
    if (dsd_unicode_supported()) {
        DSD_FPRINTF(stderr, "%lc", utf16c);
    } else {
        unsigned char lo = (unsigned char)(utf16c & 0xFF);
        if (lo >= 0x20 && lo < 0x7F) {
            fputc((int)lo, stderr);
        } else {
            fputc('?', stderr);
        }
    }
}

static void
dmr_udt_emit_utf16_text(dmr_udt_ctx* ctx, int bit_offset, int char_count) {
    for (int i = 0; i < char_count; i++) {
        uint16_t utf16c = (uint16_t)ConvertBitIntoBytes(&ctx->cs_bits[(i * 16) + bit_offset], 16);
        if (utf16c >= 0x20 && utf16c != 0x7F) {
            dmr_udt_print_utf16_char(utf16c);
            if (utf16c < 0x7F) {
                dmr_udt_append_text_event(ctx->state, ctx->slot, (char)(utf16c & 0xFF));
            }
        } else {
            DSD_FPRINTF(stderr, " ");
        }
    }
}

static void
dmr_udt_handle_binary(dmr_udt_ctx* ctx) {
    DSD_FPRINTF(stderr, "Binary Data;");
    dsd_append(ctx->udt_string, sizeof ctx->udt_string, "Binary Data; ");
    int bytes = ctx->payload_bits / 8;
    if (bytes <= 0) {
        return;
    }
    int offset = 96 / 8;
    if (offset + bytes > 60) {
        bytes = 60 - offset;
    }
    if (bytes > 0) {
        utf8_to_text(ctx->state, 0, (uint16_t)bytes, ctx->block_bytes + offset);
    }
}

static void
dmr_udt_handle_appended_addressing(dmr_udt_ctx* ctx) {
    DSD_FPRINTF(stderr, "Appended Addressing;\n ");
    dsd_append(ctx->udt_string, sizeof ctx->udt_string, "Appended Addressing; ");
    int addr_bits = ctx->payload_bits - 8;
    if (addr_bits < 0) {
        addr_bits = 0;
    }
    int end = addr_bits / 24;
    if (ctx->add_res) {
        DSD_FPRINTF(stderr, "RES: %d; ", ctx->add_res);
    }
    DSD_FPRINTF(stderr, "OK: %d; ", ctx->add_ok);
    DSD_FPRINTF(stderr, "ADDR:");
    for (int i = 0; i < end; i++) {
        DSD_FPRINTF(stderr, " %d;", (uint32_t)ConvertBitIntoBytes(&ctx->cs_bits[(i * 24) + 104], 24));
    }
}

static char
dmr_udt_bcd_char(int digit) {
    if (digit < 10) {
        return (char)(digit + 0x30);
    }
    if (digit == 10) {
        return (char)0x2A;
    }
    if (digit == 11) {
        return (char)0x23;
    }
    if (digit == 15) {
        return (char)0x20;
    }
    return (char)(digit + 0x38);
}

static void
dmr_udt_print_bcd_digit(int digit) {
    if (digit < 10) {
        DSD_FPRINTF(stderr, "%d", digit);
    } else if (digit == 10) {
        DSD_FPRINTF(stderr, "*");
    } else if (digit == 11) {
        DSD_FPRINTF(stderr, "#");
    } else if (digit == 15) {
        DSD_FPRINTF(stderr, " ");
    } else {
        DSD_FPRINTF(stderr, "R:%X", digit);
    }
}

static void
dmr_udt_handle_bcd(dmr_udt_ctx* ctx) {
    int end = ctx->payload_bits / 4;
    DSD_FPRINTF(stderr, "Dialer BCD: ");
    dsd_append(ctx->udt_string, sizeof ctx->udt_string, "Dialer Digits: ");
    for (int i = 0; i < end; i++) {
        int digit = (int)ConvertBitIntoBytes(&ctx->cs_bits[(i * 4) + 96], 4);
        dmr_udt_print_bcd_digit(digit);
        dmr_udt_append_text_event(ctx->state, ctx->slot, dmr_udt_bcd_char(digit));
    }
}

static void
dmr_udt_handle_iso7(dmr_udt_ctx* ctx) {
    int end = ctx->payload_bits / 7;
    DSD_FPRINTF(stderr, "ISO7 Text: ");
    dsd_append(ctx->udt_string, sizeof ctx->udt_string, "ISO7 Text; ");
    DSD_SPRINTF(ctx->state->event_history_s[ctx->slot].Event_History_Items[0].text_message, "%s", " ");
    for (int i = 0; i < end; i++) {
        uint8_t iso7c = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[(i * 7) + 96], 7);
        if (iso7c >= 0x20 && iso7c <= 0x7E) {
            DSD_FPRINTF(stderr, "%c", iso7c);
            dmr_udt_append_text_event(ctx->state, ctx->slot, (char)iso7c);
        } else {
            DSD_FPRINTF(stderr, " ");
        }
    }
}

static void
dmr_udt_handle_iso8(dmr_udt_ctx* ctx) {
    int end = ctx->payload_bits / 8;
    DSD_FPRINTF(stderr, "ISO8 Text: ");
    dsd_append(ctx->udt_string, sizeof ctx->udt_string, "ISO8 Text; ");
    DSD_SPRINTF(ctx->state->event_history_s[ctx->slot].Event_History_Items[0].text_message, "%s", " ");
    for (int i = 0; i < end; i++) {
        uint8_t iso8c = (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[(i * 8) + 96], 8);
        if (iso8c >= 0x20 && iso8c <= 0x7E) {
            DSD_FPRINTF(stderr, "%c", iso8c);
            dmr_udt_append_text_event(ctx->state, ctx->slot, (char)iso8c);
        } else {
            DSD_FPRINTF(stderr, " ");
        }
    }
}

static void
dmr_udt_handle_utf16(dmr_udt_ctx* ctx) {
    int end = ctx->payload_bits / 16;
    DSD_FPRINTF(stderr, "UTF16 Text: ");
    dsd_append(ctx->udt_string, sizeof ctx->udt_string, "UTF16 Text; ");
    DSD_SPRINTF(ctx->state->event_history_s[ctx->slot].Event_History_Items[0].text_message, "%s", " ");
    dmr_udt_emit_utf16_text(ctx, 96, end);
}

static void
dmr_udt_handle_ip(dmr_udt_ctx* ctx) {
    if (ctx->udt_uab == 1) {
        DSD_FPRINTF(stderr, "IP4: ");
        DSD_FPRINTF(stderr, "%d.", (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 0], 8));
        DSD_FPRINTF(stderr, "%d.", (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 8], 8));
        DSD_FPRINTF(stderr, "%d.", (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 16], 8));
        DSD_FPRINTF(stderr, "%d", (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 24], 8));
        dsd_append(ctx->udt_string, sizeof ctx->udt_string, "IP4; ");
    } else {
        DSD_FPRINTF(stderr, "IP6: ");
        DSD_FPRINTF(stderr, "%04X:", (uint16_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 0], 16));
        DSD_FPRINTF(stderr, "%04X:", (uint16_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 16], 16));
        DSD_FPRINTF(stderr, "%04X:", (uint16_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 32], 16));
        DSD_FPRINTF(stderr, "%04X:", (uint16_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 48], 16));
        DSD_FPRINTF(stderr, "%04X:", (uint16_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 64], 16));
        DSD_FPRINTF(stderr, "%04X:", (uint16_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 80], 16));
        DSD_FPRINTF(stderr, "%04X:", (uint16_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 96], 16));
        DSD_FPRINTF(stderr, "%04X", (uint16_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 112], 16));
        dsd_append(ctx->udt_string, sizeof ctx->udt_string, "IP6; ");
    }
}

static void
dmr_udt_handle_mixed_utf16(dmr_udt_ctx* ctx) {
    int text_bits = ctx->payload_bits - 32;
    if (text_bits < 0) {
        text_bits = 0;
    }
    int end = text_bits / 16;
    uint32_t address = (uint32_t)ConvertBitIntoBytes(&ctx->cs_bits[96 + 8], 24);
    DSD_FPRINTF(stderr, "Address: %d; ", address);
    DSD_FPRINTF(stderr, "UTF16 Text: ");
    dsd_append(ctx->udt_string, sizeof ctx->udt_string, "Mixed Add/Text; ");
    DSD_SPRINTF(ctx->state->event_history_s[ctx->slot].Event_History_Items[0].text_message, "Address: %d;", address);
    dmr_udt_emit_utf16_text(ctx, 96 + 32, end);
}

static void
dmr_udt_handle_nmea(dmr_udt_ctx* ctx) {
    DSD_FPRINTF(stderr, "NMEA");
    dsd_append(ctx->udt_string, sizeof ctx->udt_string, "NMEA; ");
    if (ctx->cs_bits[96] == 1) {
        DSD_FPRINTF(stderr, " Encrypted Format :(");
    } else if (ctx->udt_uab == 1) {
        nmea_iec_61162_1(ctx->opts, ctx->state, ctx->cs_bits + 96, ctx->udt_source, 1);
    } else if (ctx->udt_uab == 2) {
        nmea_iec_61162_1(ctx->opts, ctx->state, ctx->cs_bits + 96, ctx->udt_source, 2);
    } else if (ctx->udt_uab == 3) {
        DSD_FPRINTF(stderr, " Unspecified MFID Format: %02X;", (uint8_t)ConvertBitIntoBytes(&ctx->cs_bits[184], 8));
    } else {
        DSD_FPRINTF(stderr, " Reserved Format; ");
    }
}

static void
dmr_udt_handle_lip(dmr_udt_ctx* ctx) {
    dsd_append(ctx->udt_string, sizeof ctx->udt_string, "LIP; ");
    DSD_FPRINTF(stderr, "\n");
    lip_protocol_decoder(ctx->opts, ctx->state, ctx->cs_bits + 96);
}

static void
dmr_udt_decode_format(dmr_udt_ctx* ctx) {
    switch (ctx->udt_format2) {
        case 0x00: dmr_udt_handle_binary(ctx); break;
        case 0x01: dmr_udt_handle_appended_addressing(ctx); break;
        case 0x02: dmr_udt_handle_bcd(ctx); break;
        case 0x03: dmr_udt_handle_iso7(ctx); break;
        case 0x04: dmr_udt_handle_iso8(ctx); break;
        case 0x05: dmr_udt_handle_nmea(ctx); break;
        case 0x06: dmr_udt_handle_ip(ctx); break;
        case 0x07: dmr_udt_handle_utf16(ctx); break;
        case 0x08:
        case 0x09:
            DSD_FPRINTF(stderr, "MFID SPEC %02X: ", ctx->udt_format2);
            dsd_append(ctx->udt_string, sizeof ctx->udt_string, "MFID Specific; ");
            break;
        case 0x0A: dmr_udt_handle_mixed_utf16(ctx); break;
        case 0x0B: dmr_udt_handle_lip(ctx); break;
        default:
            DSD_FPRINTF(stderr, "Reserved %02X: ", ctx->udt_format2);
            dsd_append(ctx->udt_string, sizeof ctx->udt_string, "Reserved; ");
            break;
    }
}

static void
dmr_udt_finalize(dmr_udt_ctx* ctx) {
    DSD_FPRINTF(stderr, "%s", KNRM);
    if (ctx->slot == 0) {
        ctx->state->lastsrc = ctx->udt_source;
        ctx->state->lasttg = ctx->udt_target;
    } else {
        ctx->state->lastsrcR = ctx->udt_source;
        ctx->state->lasttgR = ctx->udt_target;
    }
    watchdog_event_datacall(ctx->opts, ctx->state, ctx->udt_source, ctx->udt_target, ctx->udt_string, ctx->slot);
    if (ctx->slot == 0) {
        ctx->state->lastsrc = 0;
        ctx->state->lasttg = 0;
    } else {
        ctx->state->lastsrcR = 0;
        ctx->state->lasttgR = 0;
    }
    watchdog_event_history(ctx->opts, ctx->state, ctx->slot);
    watchdog_event_current(ctx->opts, ctx->state, ctx->slot);
}

static void
dmr_udt_decoder(dsd_opts* opts, dsd_state* state, const uint8_t* block_bytes, uint32_t CRCCorrect) {
    dmr_udt_ctx ctx;
    UNUSED(CRCCorrect);
    dmr_udt_prepare_context(&ctx, opts, state, block_bytes);
    dmr_udt_decode_format(&ctx);
    dmr_udt_finalize(&ctx);
}

#ifdef DMR_PDU_DECRYPTION
typedef struct {
    int alg;
    int kid;
    int akl;
    int start;
    int end;
    unsigned long long mi;
    unsigned long long rkey;
    uint8_t kaes[32];
    uint8_t kiv[9];
} dmr_block_dec_ctx;

static void
dmr_block_dec_load_ctx(dsd_state* state, uint8_t slot, int blocks, uint8_t block_len, dmr_block_dec_ctx* ctx) {
    const uint8_t empt[32] = {0};

    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->start = (int)state->data_ks_start[slot];
    ctx->end = ((blocks + 1) * block_len) - 4 - (int)state->data_block_poc[slot] - ctx->start;
    if (ctx->end < 0) {
        ctx->end = 3096;
    }

    if (state->currentslot == 0) {
        ctx->alg = state->payload_algid;
        ctx->kid = state->payload_keyid;
        ctx->mi = (unsigned long long)state->payload_mi;
        ctx->rkey = state->rkey_array[state->payload_keyid];
    } else {
        ctx->alg = state->payload_algidR;
        ctx->kid = state->payload_keyidR;
        ctx->mi = (unsigned long long)state->payload_miR;
        ctx->rkey = state->rkey_array[state->payload_keyidR];
    }

    for (int i = 0; i < 8; i++) {
        ctx->kaes[i + 0] = ((state->rkey_array[ctx->kid + 0x000] >> (56 - (i * 8))) & 0xFF);
        ctx->kaes[i + 8] = ((state->rkey_array[ctx->kid + 0x101] >> (56 - (i * 8))) & 0xFF);
        ctx->kaes[i + 16] = ((state->rkey_array[ctx->kid + 0x201] >> (56 - (i * 8))) & 0xFF);
        ctx->kaes[i + 24] = ((state->rkey_array[ctx->kid + 0x301] >> (56 - (i * 8))) & 0xFF);
    }
    if (memcmp(ctx->kaes, empt, sizeof(ctx->kaes)) != 0) {
        ctx->akl = 1;
    }
    if (ctx->rkey == 0 && state->R != 0) {
        ctx->rkey = state->R;
    }

    ctx->kiv[0] = ((ctx->rkey & 0xFF00000000) >> 32);
    ctx->kiv[1] = ((ctx->rkey & 0xFF000000) >> 24);
    ctx->kiv[2] = ((ctx->rkey & 0xFF0000) >> 16);
    ctx->kiv[3] = ((ctx->rkey & 0xFF00) >> 8);
    ctx->kiv[4] = ((ctx->rkey & 0xFF) >> 0);
    ctx->kiv[5] = ((ctx->mi & 0xFF000000) >> 24);
    ctx->kiv[6] = ((ctx->mi & 0xFF0000) >> 16);
    ctx->kiv[7] = ((ctx->mi & 0xFF00) >> 8);
    ctx->kiv[8] = ((ctx->mi & 0xFF) >> 0);
}

static void
dmr_block_dec_print_info(const dmr_block_dec_ctx* ctx) {
    DSD_FPRINTF(stderr, "\\n PDU ALG: %02X; Key ID: %02X;", ctx->alg, ctx->kid);
    if (ctx->alg != 0) {
        DSD_FPRINTF(stderr, " MI(32): %08llX;", ctx->mi);
    }
    if (ctx->alg == 0) {
        DSD_FPRINTF(stderr, " Moto BP;");
    } else if (ctx->alg == 1) {
        DSD_FPRINTF(stderr, " RC4;");
    } else if (ctx->alg == 2) {
        DSD_FPRINTF(stderr, " DES;");
    } else if (ctx->alg == 4) {
        DSD_FPRINTF(stderr, " AES128;");
    } else if (ctx->alg == 5) {
        DSD_FPRINTF(stderr, " AES256;");
    }
    if (ctx->rkey && ctx->alg != 0) {
        DSD_FPRINTF(stderr, " Key: %010llX;", ctx->rkey);
    }
}

static void
dmr_block_dec_prepare_maes(dsd_state* state, uint8_t maes[16]) {
    LFSR128d(state);
    if (state->currentslot == 0) {
        DSD_MEMCPY(maes, state->aes_iv, 16);
    } else {
        DSD_MEMCPY(maes, state->aes_ivR, 16);
    }
}

static void
dmr_block_dec_generate_stream(dsd_state* state, uint8_t slot, const dmr_block_dec_ctx* ctx, uint8_t ob[129 * 24],
                              uint8_t* decrypted_pdu) {
    if (ctx->alg == 1 && ctx->rkey != 0) {
        rc4_block_output(256, 9, (int)state->data_byte_ctr[slot], ctx->kiv, ob);
        *decrypted_pdu = 1;
    } else if (ctx->alg == 2 && ctx->rkey != 0) {
        int nblocks = (state->data_byte_ctr[slot] / 8) + 1;
        des_multi_keystream_output(ctx->mi, ctx->rkey, ob, 1, nblocks);
        *decrypted_pdu = 1;
    } else if ((ctx->alg == 4 || ctx->alg == 5) && ctx->akl == 1) {
        uint8_t maes[16];
        int nblocks = (state->data_byte_ctr[slot] / 16) + 1;
        dmr_block_dec_prepare_maes(state, maes);
        aes_ofb_keystream_output(maes, ctx->kaes, ob, (ctx->alg == 5) ? 2 : 0, nblocks);
        *decrypted_pdu = 1;
    }
}

static void
dmr_block_dec_apply_stream(dsd_state* state, uint8_t slot, const dmr_block_dec_ctx* ctx, const uint8_t ob[129 * 24]) {
    for (int i = 0; i < ctx->end; i++) {
        state->dmr_pdu_sf[slot][i + ctx->start] ^= ob[i % 3096];
    }
}

static void
dmr_block_dec_apply_bp(dsd_state* state, uint8_t slot, const dmr_block_dec_ctx* ctx, uint8_t ob[129 * 24],
                       uint8_t* decrypted_pdu) {
    uint16_t bp_key = 0;

    if (ctx->alg != 0 || state->K == 0) {
        return;
    }

    bp_key = BPK[state->K];
    ob[0] = (bp_key >> 8) & 0xFF;
    ob[1] = (bp_key >> 0) & 0xFF;
    DSD_FPRINTF(stderr, " Key: %lld : %04X;", state->K, bp_key);
    if (bp_key == 0) {
        return;
    }
    for (int i = 0; i < ctx->end; i++) {
        state->dmr_pdu_sf[slot][i + ctx->start] ^= ob[i % 2];
    }
    *decrypted_pdu = 1;
}
#endif

static void
dmr_block_type1_decrypt_pdu(dsd_state* state, uint8_t slot, int blocks, uint8_t block_len, uint8_t* decrypted_pdu) {
#ifdef DMR_PDU_DECRYPTION
    dmr_block_dec_ctx ctx;
    uint8_t ob[129 * 24];
    uint8_t stream_ready = 0;

    DSD_MEMSET(ob, 0, sizeof(ob));
    dmr_block_dec_load_ctx(state, slot, blocks, block_len, &ctx);
    dmr_block_dec_print_info(&ctx);
    if (ctx.alg == 5) {
        DSD_FPRINTF(stderr, "\\n");
    }

    dmr_block_dec_generate_stream(state, slot, &ctx, ob, decrypted_pdu);
    stream_ready = ((ctx.alg == 1 && ctx.rkey != 0) || (ctx.alg == 2 && ctx.rkey != 0) || (ctx.alg == 4 && ctx.akl != 0)
                    || (ctx.alg == 5 && ctx.akl != 0))
                       ? 1
                       : 0;
    if (stream_ready) {
        dmr_block_dec_apply_stream(state, slot, &ctx, ob);
    } else {
        dmr_block_dec_apply_bp(state, slot, &ctx, ob, decrypted_pdu);
    }
#else
    UNUSED(state);
    UNUSED(slot);
    UNUSED(blocks);
    UNUSED(block_len);
    UNUSED(decrypted_pdu);
#endif
}

//assemble the blocks as they come in, shuffle them into the unified dmr_pdu_sf
typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    const uint8_t* block_bytes;
    uint8_t block_len;
    uint8_t type;
    uint8_t slot;
    uint8_t blockcounter;
    int blocks;
    uint8_t is_udt;
    uint8_t lb;
    uint8_t pf;
    uint32_t crc_correct;
    uint32_t crc_computed;
    uint32_t crc_extracted;
    uint32_t irrecoverable_errors;
    uint8_t dmr_pdu_sf_bits[8 * 24 * 129];
    uint8_t mbc_crc_good[2];
} dmr_block_assembler_ctx;

static void
dmr_block_assembler_init_ctx(dmr_block_assembler_ctx* ctx, dsd_opts* opts, dsd_state* state, const uint8_t* block_bytes,
                             uint8_t block_len, uint8_t type) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->opts = opts;
    ctx->state = state;
    ctx->block_bytes = block_bytes;
    ctx->block_len = block_len;
    ctx->type = type;
    ctx->slot = (uint8_t)(state->currentslot & 1);
    ctx->blockcounter = state->data_block_counter[ctx->slot];
    ctx->blocks = 1;

    if (type == 1) {
        ctx->blocks = state->data_header_blocks[ctx->slot] - 1;
    } else if (type == 2) {
        ctx->blocks = state->data_block_counter[ctx->slot];
    } else if (type == 3) {
        ctx->blocks = state->data_header_blocks[ctx->slot];
        ctx->is_udt = 1;
        ctx->type = 2;
    }

    if (ctx->blocks < 1) {
        ctx->blocks = 1;
    }
    if (ctx->blocks > 127) {
        ctx->blocks = 127;
    }
    if (ctx->block_len == 0) {
        ctx->block_len = 18;
    }
    if (ctx->block_len > 24) {
        ctx->block_len = 24;
    }
}

static int
dmr_block_type1_offset(const dmr_block_assembler_ctx* ctx) {
    return (ctx->state->data_p_head[ctx->slot] == 1) ? 12 : 0;
}

static uint8_t
dmr_block_type1_complete(const dmr_block_assembler_ctx* ctx) {
    return (uint8_t)(ctx->state->data_block_counter[ctx->slot] == ctx->state->data_header_blocks[ctx->slot]
                     && ctx->state->data_header_valid[ctx->slot] == 1);
}

static void
dmr_block_type1_append_bytes(dmr_block_assembler_ctx* ctx, uint16_t* ctr_out) {
    uint16_t ctr = ctx->state->data_byte_ctr[ctx->slot];
    for (int i = 0; i < ctx->block_len; i++) {
        ctx->state->dmr_pdu_sf[ctx->slot][ctr++] = ctx->block_bytes[i];
    }
    ctx->state->data_byte_ctr[ctx->slot] += ctx->block_len;
    *ctr_out = ctr;
}

static uint32_t
dmr_block_type1_extract_crc32(const dsd_state* state, uint8_t slot_idx, uint16_t ctr) {
    if (ctr < 4) {
        return 0u;
    }
    return (state->dmr_pdu_sf[slot_idx][ctr - 4] << 24) | (state->dmr_pdu_sf[slot_idx][ctr - 3] << 16)
           | (state->dmr_pdu_sf[slot_idx][ctr - 2] << 8) | (state->dmr_pdu_sf[slot_idx][ctr - 1] << 0);
}

static void
dmr_block_type1_pack_crc_bits(const dsd_state* state, uint8_t slot, uint8_t block_len, uint16_t ctr, int offset,
                              uint8_t bits[]) {
    for (int i = 0, j = 0; i < ctr; i += 2, j += 16) {
        if ((i + 1) < ctr) {
            bits[j + 0] = (state->dmr_pdu_sf[slot][i + 1] >> 7) & 0x01;
            bits[j + 1] = (state->dmr_pdu_sf[slot][i + 1] >> 6) & 0x01;
            bits[j + 2] = (state->dmr_pdu_sf[slot][i + 1] >> 5) & 0x01;
            bits[j + 3] = (state->dmr_pdu_sf[slot][i + 1] >> 4) & 0x01;
            bits[j + 4] = (state->dmr_pdu_sf[slot][i + 1] >> 3) & 0x01;
            bits[j + 5] = (state->dmr_pdu_sf[slot][i + 1] >> 2) & 0x01;
            bits[j + 6] = (state->dmr_pdu_sf[slot][i + 1] >> 1) & 0x01;
            bits[j + 7] = (state->dmr_pdu_sf[slot][i + 1] >> 0) & 0x01;
        }

        bits[j + 8] = (state->dmr_pdu_sf[slot][i] >> 7) & 0x01;
        bits[j + 9] = (state->dmr_pdu_sf[slot][i] >> 6) & 0x01;
        bits[j + 10] = (state->dmr_pdu_sf[slot][i] >> 5) & 0x01;
        bits[j + 11] = (state->dmr_pdu_sf[slot][i] >> 4) & 0x01;
        bits[j + 12] = (state->dmr_pdu_sf[slot][i] >> 3) & 0x01;
        bits[j + 13] = (state->dmr_pdu_sf[slot][i] >> 2) & 0x01;
        bits[j + 14] = (state->dmr_pdu_sf[slot][i] >> 1) & 0x01;
        bits[j + 15] = (state->dmr_pdu_sf[slot][i] >> 0) & 0x01;

        if (i == (block_len - 1 + offset) && state->data_conf_data[slot] == 1) {
            i += 2;
        }
    }
}

static void
dmr_block_type1_update_crc(dmr_block_assembler_ctx* ctx, uint16_t ctr, int offset) {
    uint8_t slot_idx = (ctx->slot >= 2) ? 1 : ctx->slot;

    dmr_unpack_bytes_to_bits(ctx->state->dmr_pdu_sf[slot_idx], ctr, ctx->dmr_pdu_sf_bits);
    ctx->crc_extracted = dmr_block_type1_extract_crc32(ctx->state, slot_idx, ctr);
    dmr_block_type1_pack_crc_bits(ctx->state, ctx->slot, ctx->block_len, ctr, offset, ctx->dmr_pdu_sf_bits);
    ctx->crc_computed = (uint32_t)ComputeCrc32Bit(ctx->dmr_pdu_sf_bits, (ctr * 8) - 32);
    if (ctx->crc_computed == ctx->crc_extracted
        || (ctx->state->data_header_format[ctx->slot] == 0xF && ctx->state->data_header_sap[ctx->slot] == 1)) {
        ctx->crc_correct = 1;
    }
}

static uint8_t
dmr_block_type1_encryption_required(const dmr_block_assembler_ctx* ctx) {
    return (uint8_t)((ctx->slot == 0 && ctx->state->dmr_so == 0x100)
                     || (ctx->slot == 1 && ctx->state->dmr_soR == 0x100));
}

static void
dmr_block_type1_handle_encrypted_notice(dmr_block_assembler_ctx* ctx) {
    uint8_t alg = (ctx->slot == 0) ? ctx->state->payload_algid : ctx->state->payload_algidR;
    uint8_t kid = (ctx->slot == 0) ? ctx->state->payload_keyid : ctx->state->payload_keyidR;
    char enc_str[200];

    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, "\n Slot %d - Encrypted PDU;", ctx->slot + 1);
    DSD_FPRINTF(stderr, "%s", KNRM);
    if (alg == 0x07) {
        DSD_FPRINTF(stderr, " Vertex Std data decrypt not implemented;");
    }

    DSD_MEMSET(enc_str, 0, sizeof(enc_str));
    DSD_SPRINTF(enc_str, "DATA TGT: %lld; SRC: %lld; ENC PDU; ALG: %02X; KID: %02X;",
                ctx->state->dmr_lrrp_source[ctx->slot], ctx->state->dmr_lrrp_target[ctx->slot], alg, kid);
    DSD_SPRINTF(ctx->state->dmr_lrrp_gps[ctx->slot], "%s", enc_str);
    watchdog_event_datacall(ctx->opts, ctx->state, ctx->state->dmr_lrrp_source[ctx->slot],
                            ctx->state->dmr_lrrp_target[ctx->slot], enc_str, ctx->slot);
}

static uint8_t
dmr_block_type1_lrrp_crc_ok(const dsd_state* state, uint8_t slot) {
    uint8_t pdu_crc_ok = 1;
    if (!state->data_conf_data[slot]) {
        return pdu_crc_ok;
    }

    int start = state->data_p_head[slot] ? 2 : 1;
    int end = state->data_header_blocks[slot];
    if (end > 126) {
        end = 126;
    }
    if (end < start) {
        return 0;
    }

    for (int bi = start; bi <= end; bi++) {
        if (state->data_block_crc_valid[slot][bi] != 1) {
            pdu_crc_ok = 0;
            break;
        }
    }
    return pdu_crc_ok;
}

static void
dmr_block_type1_print_mnis_type(uint8_t mnis_type) {
    if (mnis_type == 0x01) {
        DSD_FPRINTF(stderr, "MNIS LOCN; ");
    } else if (mnis_type == 0x11) {
        DSD_FPRINTF(stderr, "MNIS LRRP; ");
    } else if (mnis_type == 0x33) {
        DSD_FPRINTF(stderr, "MNIS ARS;  ");
    } else if (mnis_type == 0x88) {
        DSD_FPRINTF(stderr, "MNIS XCMP; ");
    } else {
        DSD_FPRINTF(stderr, "Unknown MNIS Type: %02X; ", mnis_type);
    }
}

static void
dmr_block_type1_handle_mnis_payload(dmr_block_assembler_ctx* ctx, uint16_t len, uint8_t mnis_type, int offset,
                                    uint32_t msrc, uint32_t mdst) {
    if (mnis_type == 0x11) {
        uint8_t pdu_crc_ok = dmr_block_type1_lrrp_crc_ok(ctx->state, ctx->slot);
        dmr_lrrp(ctx->opts, ctx->state, len, msrc, mdst, ctx->state->dmr_pdu_sf[ctx->slot] + 7, pdu_crc_ok);
    } else if (mnis_type == 0x33) {
        utf8_to_text(ctx->state, 0, 15, ctx->state->dmr_pdu_sf[ctx->slot] + 7);
    } else if (mnis_type == 0x01) {
        utf8_to_text(ctx->state, 0, len - offset, ctx->state->dmr_pdu_sf[ctx->slot] + 7);
        dmr_locn(ctx->opts, ctx->state, len, ctx->state->dmr_pdu_sf[ctx->slot] + 7);
        DSD_SPRINTF(ctx->state->event_history_s[ctx->slot].Event_History_Items[0].gps_s, "%s",
                    ctx->state->dmr_lrrp_gps[ctx->slot]);
    }

    if (mnis_type != 0x11 && mnis_type != 0x01) {
        char mnis_str[200];
        DSD_MEMSET(mnis_str, 200, sizeof(mnis_str));
        DSD_SPRINTF(mnis_str, "MNIS TGT: %lld; SRC: %lld;", ctx->state->dmr_lrrp_source[ctx->slot],
                    ctx->state->dmr_lrrp_target[ctx->slot]);
        watchdog_event_datacall(ctx->opts, ctx->state, ctx->state->dmr_lrrp_source[ctx->slot],
                                ctx->state->dmr_lrrp_target[ctx->slot], mnis_str, ctx->slot);
    } else if (mnis_type == 0x11 || mnis_type == 0x01) {
        watchdog_event_datacall(ctx->opts, ctx->state, ctx->state->dmr_lrrp_source[ctx->slot],
                                ctx->state->dmr_lrrp_target[ctx->slot], ctx->state->dmr_lrrp_gps[ctx->slot], ctx->slot);
    }
}

static void
dmr_block_type1_handle_mnis(dmr_block_assembler_ctx* ctx, int offset) {
    uint16_t byte_count = ctx->state->data_byte_ctr[ctx->slot];
    uint8_t poc = ctx->state->data_block_poc[ctx->slot];
    uint16_t len = byte_count - poc - 4 - 7;
    uint32_t msrc = ctx->state->dmr_lrrp_source[ctx->slot];
    uint32_t mdst = ctx->state->dmr_lrrp_target[ctx->slot];
    uint8_t mnis_type = ctx->state->dmr_pdu_sf[ctx->slot][4];
    uint16_t mnis_unk;

    if (len > 150) {
        len = 150;
    }
    DSD_FPRINTF(stderr, "\n SRC(MNIS): %08d; ", msrc);
    DSD_FPRINTF(stderr, "\n DST(MNIS): %08d; ", mdst);
    dmr_block_type1_print_mnis_type(mnis_type);
    mnis_unk = (ctx->state->dmr_pdu_sf[ctx->slot][5] << 8) | ctx->state->dmr_pdu_sf[ctx->slot][6];
    DSD_FPRINTF(stderr, " ???: %04X", mnis_unk);
    DSD_SPRINTF(ctx->state->dmr_lrrp_gps[ctx->slot], "MNIS SRC: %d; DST: %d; ", msrc, mdst);
    dmr_block_type1_handle_mnis_payload(ctx, len, mnis_type, offset, msrc, mdst);
}

static void
dmr_block_type1_handle_unknown_pdu(dmr_block_assembler_ctx* ctx) {
    char unk_str[200];
    int safe_slot = (ctx->slot == 0 || ctx->slot == 1) ? ctx->slot : 0;
    unsigned long long source = 0;
    unsigned long long target = 0;
    if (ctx->slot == 0 || ctx->slot == 1) {
        source = ctx->state->dmr_lrrp_source[safe_slot];
        target = ctx->state->dmr_lrrp_target[safe_slot];
    }
    DSD_MEMSET(unk_str, 200, sizeof(unk_str));
    DSD_SPRINTF(unk_str, "DATA TGT: %lld; SRC: %lld; Unknown PDU Format;", source, target);
    watchdog_event_datacall(ctx->opts, ctx->state, source, target, unk_str, safe_slot);
}

static void
dmr_block_type1_handle_sap(dmr_block_assembler_ctx* ctx, int offset) {
    if (ctx->slot > 1) {
        dmr_block_type1_handle_unknown_pdu(ctx);
        return;
    }

    uint8_t sap = ctx->state->data_header_sap[ctx->slot];
    uint16_t len = ((ctx->blocks + 1) * ctx->block_len) - 4;

    if (sap == 4) {
        decode_ip_pdu(ctx->opts, ctx->state, len, ctx->state->dmr_pdu_sf[ctx->slot]);
    } else if (sap == 10) {
        dmr_sd_pdu(ctx->opts, ctx->state, len, ctx->state->dmr_pdu_sf[ctx->slot]);
    } else if (sap == 2 || sap == 3) {
        dmr_udp_comp_pdu(ctx->opts, ctx->state, len, ctx->state->dmr_pdu_sf[ctx->slot]);
    } else if (sap == 1 && ctx->state->dmr_pdu_sf[ctx->slot][1] == 0x10) {
        dmr_block_type1_handle_mnis(ctx, offset);
    } else {
        dmr_block_type1_handle_unknown_pdu(ctx);
    }
}

static void
dmr_block_type1_process_payload(dmr_block_assembler_ctx* ctx, int offset) {
    uint8_t enc_check = dmr_block_type1_encryption_required(ctx);
    uint8_t decrypted_pdu = enc_check ? 0 : 1;

    if (enc_check) {
        dmr_block_type1_decrypt_pdu(ctx->state, ctx->slot, ctx->blocks, ctx->block_len, &decrypted_pdu);
    }
    if (enc_check == 1 && decrypted_pdu == 0) {
        dmr_block_type1_handle_encrypted_notice(ctx);
    } else if (ctx->opts->aggressive_framesync == 0 || ctx->opts->dmr_crc_relaxed_default) {
        dmr_block_type1_handle_sap(ctx, offset);
    }
}

static void
dmr_block_type1_log_crc_and_payload(dmr_block_assembler_ctx* ctx) {
    if (!ctx->crc_correct) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "\n Slot %d - Multi Block PDU Message CRC32 ERR", ctx->slot + 1);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (ctx->opts->payload == 1) {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, "\n Slot %d - Multi Block PDU Message\n  ", ctx->slot + 1);
        for (int i = 0; i < ((ctx->blocks + 1) * ctx->block_len); i++) {
            if ((i != 0) && (i % 12 == 0)) {
                DSD_FPRINTF(stderr, "\n  ");
            }
            DSD_FPRINTF(stderr, "%02X", ctx->state->dmr_pdu_sf[ctx->slot][i]);
        }
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
}

static void
dmr_block_type1_clear_header_state(dmr_block_assembler_ctx* ctx) {
    ctx->state->data_header_format[ctx->slot] = 7;
    ctx->state->data_header_sap[ctx->slot] = 0;
    ctx->state->data_header_valid[ctx->slot] = 0;
    ctx->state->data_conf_data[ctx->slot] = 0;
    ctx->state->data_block_poc[ctx->slot] = 0;
    ctx->state->data_byte_ctr[ctx->slot] = 0;
    ctx->state->data_ks_start[ctx->slot] = 0;
}

static void
dmr_block_assembler_handle_type1(dmr_block_assembler_ctx* ctx) {
    uint16_t ctr = 0;
    int offset = dmr_block_type1_offset(ctx);

    dmr_block_type1_append_bytes(ctx, &ctr);
    if (!dmr_block_type1_complete(ctx)) {
        return;
    }
    dmr_block_type1_update_crc(ctx, ctr, offset);
    dmr_block_type1_process_payload(ctx, offset);
    dmr_block_type1_log_crc_and_payload(ctx);
    dmr_block_type1_clear_header_state(ctx);
}

static uint32_t
dmr_block_extract_crc16(const uint8_t bits[], int total_bits) {
    uint32_t extracted = 0;
    for (int i = 0; i < 16; i++) {
        extracted = (extracted << 1) | (uint32_t)(bits[(total_bits - 16) + i] & 1);
    }
    return extracted;
}

static void
dmr_block_type2_set_lb_pf(dmr_block_assembler_ctx* ctx) {
    ctx->lb = ctx->block_bytes[0] >> 7;
    ctx->pf = (ctx->block_bytes[0] >> 6) & 1;
    if (!ctx->is_udt) {
        return;
    }

    ctx->pf = 0;
    if (ctx->state->udt_uab_reserved[ctx->slot]) {
        uint8_t mbc_block_bits[12 * 8 * 6];
        int msg_bytes = (1 + ctx->blockcounter) * ctx->block_len;
        int mbits = (int)(ctx->blockcounter * 96);
        ctx->lb = 0;

        DSD_MEMSET(ctx->dmr_pdu_sf_bits, 0, sizeof(ctx->dmr_pdu_sf_bits));
        dmr_unpack_bytes_to_bits(ctx->state->dmr_pdu_sf[ctx->slot], msg_bytes, ctx->dmr_pdu_sf_bits);
        ctx->crc_extracted = dmr_block_extract_crc16(ctx->dmr_pdu_sf_bits, 96 * (1 + ctx->blockcounter));
        DSD_MEMSET(mbc_block_bits, 0, sizeof(mbc_block_bits));
        for (int i = 0; i < mbits; i++) {
            mbc_block_bits[i] = ctx->dmr_pdu_sf_bits[i + 96];
        }
        ctx->crc_computed = ComputeCrcCCITT16d(mbc_block_bits, (uint16_t)(mbits - 16));
        if (ctx->crc_computed == ctx->crc_extracted) {
            ctx->lb = 1;
            ctx->blocks = ctx->blockcounter;
        }
    } else {
        ctx->lb = 0;
        if (ctx->blocks == ctx->blockcounter) {
            ctx->lb = 1;
        }
    }
}

static uint8_t
dmr_block_type2_length_ok(dmr_block_assembler_ctx* ctx) {
    if (ctx->is_udt || (ctx->blocks >= 1 && ctx->blocks <= 4)) {
        return 1;
    }

    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, "\n Slot %d - MBC aggregate length out of bounds: %d", ctx->slot + 1, ctx->blocks);
    DSD_FPRINTF(stderr, "%s", KNRM);
    ctx->state->data_block_crc_valid[ctx->slot][0] = 0;
    return 0;
}

static void
dmr_block_type2_unpack_bits(dmr_block_assembler_ctx* ctx) {
    int total_bytes = (1 + ctx->blocks) * ctx->block_len;
    if (total_bytes > 12 * 5) {
        total_bytes = 12 * 5;
    }

    DSD_MEMSET(ctx->dmr_pdu_sf_bits, 0, sizeof(ctx->dmr_pdu_sf_bits));
    dmr_unpack_bytes_to_bits(ctx->state->dmr_pdu_sf[ctx->slot], total_bytes, ctx->dmr_pdu_sf_bits);
    if (ctx->is_udt) {
        ctx->pf = ctx->dmr_pdu_sf_bits[73];
    }
}

static void
dmr_block_type2_update_crc(dmr_block_assembler_ctx* ctx) {
    uint8_t mbc_block_bits[12 * 8 * 6];
    int limit = 12 * 8 * 3;

    ctx->mbc_crc_good[0] = ctx->state->data_block_crc_valid[ctx->slot][0];
    ctx->crc_extracted = dmr_block_extract_crc16(ctx->dmr_pdu_sf_bits, 96 * (1 + ctx->blocks));
    DSD_MEMSET(mbc_block_bits, 0, sizeof(mbc_block_bits));
    for (int i = 0; i < limit; i++) {
        mbc_block_bits[i] = ctx->dmr_pdu_sf_bits[i + 96];
    }
    if (ctx->is_udt) {
        DSD_MEMSET(mbc_block_bits, 0, sizeof(mbc_block_bits));
        limit = 12 * 8 * ctx->blocks;
        for (int i = 0; i < limit; i++) {
            mbc_block_bits[i] = ctx->dmr_pdu_sf_bits[i + 96];
        }
    }

    ctx->crc_computed = ComputeCrcCCITT16d(mbc_block_bits, ((ctx->blocks + 0) * 96) - 16);
    if (ctx->crc_computed == ctx->crc_extracted) {
        ctx->mbc_crc_good[1] = 1;
    }

    ctx->crc_correct = 0;
    ctx->irrecoverable_errors = 1;
    if (ctx->mbc_crc_good[0] == 1 && ctx->mbc_crc_good[1] == 1) {
        ctx->crc_correct = 1;
        ctx->irrecoverable_errors = 0;
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "\n Slot %d - Multi Block Control Message CRC16 ERR", ctx->slot + 1);
        DSD_FPRINTF(stderr, " %X - %X", ctx->crc_extracted, ctx->crc_computed);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static void
dmr_block_type2_dispatch(dmr_block_assembler_ctx* ctx) {
    if (!ctx->is_udt && !ctx->pf) {
        dmr_cspdu(ctx->opts, ctx->state, ctx->dmr_pdu_sf_bits, ctx->state->dmr_pdu_sf[ctx->slot], ctx->crc_correct,
                  ctx->irrecoverable_errors);
    }
    if (ctx->is_udt && !ctx->pf) {
        dmr_udt_decoder(ctx->opts, ctx->state, ctx->state->dmr_pdu_sf[ctx->slot], ctx->crc_correct);
    }
}

static void
dmr_block_type2_log_payload(dmr_block_assembler_ctx* ctx) {
    if (ctx->opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "%s", KGRN);
    DSD_FPRINTF(stderr, "\n Slot %d - Multi Block Control Message\n  ", ctx->slot + 1);
    for (int i = 0; i < ((ctx->blocks + 1) * ctx->block_len); i++) {
        DSD_FPRINTF(stderr, "%02X", ctx->state->dmr_pdu_sf[ctx->slot][i]);
        if (i == 11 || i == 23 || i == 35 || i == 47 || i == 59 || i == 71 || i == 83 || i == 95) {
            DSD_FPRINTF(stderr, "\n  ");
        }
    }
    DSD_FPRINTF(stderr, "%s", KRED);
    if (ctx->mbc_crc_good[0] == 0) {
        DSD_FPRINTF(stderr, "MBC/UDT Header CRC ERR ");
    }
    if (ctx->mbc_crc_good[1] == 0) {
        DSD_FPRINTF(stderr, "MBC/UDT Blocks CRC ERR ");
    }
    if (ctx->pf) {
        DSD_FPRINTF(stderr, "MBC/UDT Header/Blocks Protected ");
    }
    DSD_FPRINTF(stderr, "%s ", KNRM);
}

static uint8_t
dmr_block_assembler_handle_type2(dmr_block_assembler_ctx* ctx) {
    if (ctx->state->data_block_counter[ctx->slot] > 4) {
        ctx->state->data_block_counter[ctx->slot] = 4;
    }
    for (int i = 0; i < ctx->block_len; i++) {
        ctx->state->dmr_pdu_sf[ctx->slot][i + (ctx->blockcounter * ctx->block_len)] = ctx->block_bytes[i];
    }

    dmr_block_type2_set_lb_pf(ctx);
    if (ctx->lb != 1 || ctx->state->data_header_valid[ctx->slot] != 1) {
        return 1;
    }
    if (!dmr_block_type2_length_ok(ctx)) {
        return 0;
    }

    dmr_block_type2_unpack_bits(ctx);
    dmr_block_type2_update_crc(ctx);
    dmr_block_type2_dispatch(ctx);
    dmr_block_type2_log_payload(ctx);
    return 1;
}

static void
dmr_block_assembler_reset_type1(dmr_block_assembler_ctx* ctx) {
    dmr_clear_superframe_slot(ctx->state, ctx->slot);
    ctx->state->data_block_crc_valid[ctx->slot][0] = 0;
    ctx->state->data_block_counter[ctx->slot] = 1;
    ctx->state->data_header_format[ctx->slot] = 7;
    ctx->state->data_header_sap[ctx->slot] = 0;
    ctx->state->data_header_valid[ctx->slot] = 0;
    ctx->state->data_conf_data[ctx->slot] = 0;
    ctx->state->data_p_head[ctx->slot] = 0;
    ctx->state->data_block_poc[ctx->slot] = 0;
    ctx->state->data_byte_ctr[ctx->slot] = 0;
    ctx->state->data_ks_start[ctx->slot] = 0;
    ctx->state->udt_uab_reserved[ctx->slot] = 0;
}

static void
dmr_block_assembler_reset_type2(dmr_block_assembler_ctx* ctx) {
    dmr_clear_superframe_slot(ctx->state, ctx->slot);
    ctx->state->data_block_crc_valid[ctx->slot][0] = 0;
    ctx->state->data_block_counter[ctx->slot] = 1;
    ctx->state->data_header_format[ctx->slot] = 7;
    ctx->state->data_header_sap[ctx->slot] = 0;
    ctx->state->data_header_valid[ctx->slot] = 0;
    ctx->state->data_conf_data[ctx->slot] = 0;
    ctx->state->data_p_head[ctx->slot] = 0;
    ctx->state->udt_uab_reserved[ctx->slot] = 0;
}

static void
dmr_block_assembler_finalize(dmr_block_assembler_ctx* ctx) {
    if (ctx->type == 1 && ctx->state->data_block_counter[ctx->slot] == ctx->state->data_header_blocks[ctx->slot]) {
        dmr_block_assembler_reset_type1(ctx);
    } else if (ctx->type == 2 && ctx->lb == 1) {
        dmr_block_assembler_reset_type2(ctx);
    } else {
        ctx->state->data_block_counter[ctx->slot]++;
    }
}

static void
dmr_block_assembler_body(dsd_opts* opts, dsd_state* state, uint8_t block_bytes[], uint8_t block_len, uint8_t databurst,
                         uint8_t type) {
    dmr_block_assembler_ctx ctx;

    UNUSED(databurst);
    dmr_block_assembler_init_ctx(&ctx, opts, state, block_bytes, block_len, type);
    if (ctx.type == 1) {
        dmr_block_assembler_handle_type1(&ctx);
    } else if (ctx.type == 2) {
        if (!dmr_block_assembler_handle_type2(&ctx)) {
            return;
        }
    }
    dmr_block_assembler_finalize(&ctx);
}

void
dmr_block_assembler(dsd_opts* opts, dsd_state* state, uint8_t block_bytes[], uint8_t block_len, uint8_t databurst,
                    uint8_t type) {
    dmr_block_assembler_body(opts, state, block_bytes, block_len, databurst, type);
}

//failsafe to clear old data header, block info, cach, in case of tact/emb/slottype failures
//or tuning away and we can no longer verify accurate data block reporting
void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    UNUSED(opts);
    DSD_MEMSET(state->gi, -1, sizeof(state->gi));
    DSD_MEMSET(state->data_p_head, 0, sizeof(state->data_p_head));
    DSD_MEMSET(state->data_conf_data, 0, sizeof(state->data_conf_data));
    DSD_MEMSET(state->dmr_pdu_sf, 0, sizeof(state->dmr_pdu_sf));
    DSD_MEMSET(state->data_block_counter, 1, sizeof(state->data_block_counter));
    DSD_MEMSET(state->data_block_poc, 0, sizeof(state->data_block_poc));
    DSD_MEMSET(state->data_byte_ctr, 0, sizeof(state->data_byte_ctr));
    DSD_MEMSET(state->udt_uab_reserved, 0, sizeof(state->udt_uab_reserved));
    DSD_MEMSET(state->data_ks_start, 0, sizeof(state->data_ks_start));
    DSD_MEMSET(state->data_header_blocks, 1, sizeof(state->data_header_blocks));
    DSD_MEMSET(state->data_block_crc_valid, 0, sizeof(state->data_block_crc_valid));
    DSD_MEMSET(state->dmr_lrrp_source, 0, sizeof(state->dmr_lrrp_source));
    DSD_MEMSET(state->dmr_lrrp_target, 0, sizeof(state->dmr_lrrp_target));
    DSD_MEMSET(state->dmr_cach_fragment, 1, sizeof(state->dmr_cach_fragment));
    DSD_MEMSET(state->cap_plus_csbk_bits, 0, sizeof(state->cap_plus_csbk_bits));
    DSD_MEMSET(state->cap_plus_block_num, 0, sizeof(state->cap_plus_block_num));
    DSD_MEMSET(state->data_header_valid, 0, sizeof(state->data_header_valid));
    DSD_MEMSET(state->data_header_format, 7, sizeof(state->data_header_format));
    DSD_MEMSET(state->data_header_sap, 0, sizeof(state->data_header_sap));
    DSD_MEMSET(state->data_dbsn_expected, 0, sizeof(state->data_dbsn_expected));
    DSD_MEMSET(state->data_dbsn_have, 0, sizeof(state->data_dbsn_have));
    //reset some strings -- resetting call string here causes random blink on ncurses terminal (cap+)
    // DSD_SPRINTF(state->call_string[0], "%s", "                     "); //21 spaces
    // DSD_SPRINTF(state->call_string[1], "%s", "                     "); //21 spaces
    DSD_SPRINTF(state->dmr_lrrp_gps[0], "%s", "");
    DSD_SPRINTF(state->dmr_lrrp_gps[1], "%s", "");
}

// Safe append helper for bounded concatenation
static inline void
dsd_append(char* dst, size_t dstsz, const char* src) {
    if (!dst || !src || dstsz == 0) {
        return;
    }
    size_t len = strlen(dst);
    if (len >= dstsz) {
        return;
    }
    DSD_SNPRINTF(dst + len, dstsz - len, "%s", src);
}
