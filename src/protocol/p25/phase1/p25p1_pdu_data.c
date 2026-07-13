// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * p25p1_pdu_data.c
 * P25p1 PDU Data Decoding
 *
 * LWVMOBILE
 * 2025-03 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/rc4.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/protocol/p25/p25_pdu.h>
#include <dsd-neo/protocol/pdu.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

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

enum { P25_PDU_MAX_DECODE_BYTES = 18 * 129 };

typedef struct {
    uint8_t fmt;
    uint8_t sap;
    uint8_t mfid;
    uint8_t io;
    uint32_t llid;
    uint8_t blks;
    uint8_t pad;
    uint8_t offset;
    int payload_len;
    int encrypted;
    const char* summary;
} p25_pdu_json_fields;

static void
p25_emit_pdu_json_if_enabled(const p25_pdu_json_fields* fields) {
    const dsdneoRuntimeConfig* rc = dsd_neo_get_config();
    if (!fields || !rc || !rc->pdu_json_enable) {
        return;
    }

    /* Skip trunking control SAPs to reduce noise unless explicitly desired later */
    if (fields->sap == 61 || fields->sap == 63) {
        return;
    }

    time_t ts = time(NULL);
    char sum[160];
    if (fields->summary && fields->summary[0] != '\0') {
        /* ensure no embedded quotes break JSON (very minimal escape) */
        int j = 0;
        for (int i = 0; fields->summary[i] != '\0' && j < (int)sizeof(sum) - 1; i++) {
            char ch = fields->summary[i];
            if (ch == '"') {
                continue; /* drop quotes */
            }
            sum[j++] = ch;
        }
        sum[j] = '\0';
    } else {
        sum[0] = '\0';
    }

    /* Start a new line and omit trailing newline so tests parse last segment. */
    fputc('\n', stderr);
    DSD_FPRINTF(stderr,
                "{\"ts\":%ld,\"proto\":\"p25\",\"fmt\":%u,\"sap\":%u,\"mfid\":%u,\"io\":%u,\"llid\":%u,"
                "\"blks\":%u,\"pad\":%u,\"offset\":%u,\"len\":%d,\"enc\":%d,\"summary\":\"%s\"}",
                (long)ts, fields->fmt, fields->sap, fields->mfid, fields->io, fields->llid, fields->blks, fields->pad,
                fields->offset, fields->payload_len, fields->encrypted ? 1 : 0, sum);
}

static void
p25_parse_sap32_regauth(dsd_opts* opts, dsd_state* state, const uint8_t* p, int plen, char* out_summary,
                        size_t out_sz) {
    UNUSED(opts);
    UNUSED(state);
    /* Minimal: first byte often indicates message subtype/opcode. Keep concise. */
    uint8_t subtype = (plen > 0) ? p[0] : 0xFF;
    DSD_SNPRINTF(out_summary, out_sz, "RegAuth subtype:%u bytes:%d", (unsigned)subtype, plen);
}

static void
p25_parse_sap34_syscfg(dsd_opts* opts, dsd_state* state, const uint8_t* p, int plen, char* out_summary, size_t out_sz) {
    UNUSED(opts);
    UNUSED(state);
    /* Minimal: emit subtype and a couple of key bytes to help field analysis. */
    uint8_t subtype = (plen > 0) ? p[0] : 0xFF;
    uint8_t b1 = (plen > 1) ? p[1] : 0;
    uint8_t b2 = (plen > 2) ? p[2] : 0;
    DSD_SNPRINTF(out_summary, out_sz, "SysCfg subtype:%u b1:%u b2:%u bytes:%d", (unsigned)subtype, (unsigned)b1,
                 (unsigned)b2, plen);
}

static uint8_t
p25_nibble_hi(uint8_t value) {
    return (uint8_t)((value >> 4U) & 0x0FU);
}

static uint8_t
p25_nibble_lo(uint8_t value) {
    return (uint8_t)(value & 0x0FU);
}

static const char*
p25_sndcp_type_label(uint8_t type, int outbound) {
    if (outbound) {
        switch (type) {
            case 0: return "Activate TDS Context Accept";
            case 1: return "Deactivate TDS Context Accept";
            case 2: return "Deactivate TDS Context Request";
            case 3: return "Activate TDS Context Reject";
            case 4: return "RF Unconfirmed Data";
            case 5: return "RF Confirmed Data";
            default: break;
        }
    } else {
        switch (type) {
            case 0: return "Activate TDS Context Request";
            case 1: return "Deactivate TDS Context Accept";
            case 2: return "Deactivate TDS Context Request";
            case 5: return "RF Confirmed Data";
            default: break;
        }
    }
    return outbound ? "Outbound Unknown" : "Inbound Unknown";
}

static const char*
p25_sndcp_nat_label(uint8_t nat) {
    switch (nat) {
        case 0: return "IPv4 Static";
        case 1: return "IPv4 Dynamic";
        case 15: return "No Address";
        default: return "Reserved";
    }
}

static const char*
p25_sndcp_reject_reason(uint8_t reason) {
    static const char* labels[] = {
        "Any Reason",
        "MRC Not Provisioned For TDS",
        "MRC DSUT Not Supported",
        "Max TDS Contexts Exceeded",
        "SNDCP Version Not Supported",
        "TDS Not Supported By FNE",
        "TDS Not Supported By This System",
        "IPv4 Static Address Not Correct",
        "IPv4 Static Address Not Allowed",
        "IPv4 Static Address In Use",
        "IPv4 Not Supported",
        "IPv4 Dynamic Address Pool Empty",
        "IPv4 Dynamic Address Not Supported",
    };
    if (reason < (sizeof(labels) / sizeof(labels[0]))) {
        return labels[reason];
    }
    return "Unknown";
}

static int
p25_sndcp_ready_seconds(uint8_t value) {
    static const int seconds[16] = {0, 1, 2, 4, 6, 8, 10, 15, 20, 25, 30, 60, 120, 180, 300, 86400};
    return seconds[value & 0x0FU];
}

static int
p25_sndcp_standby_seconds(uint8_t value) {
    static const int seconds[16] = {0,    10,    30,    60,    600,   1200,   1800,   3600,
                                    7200, 14400, 28800, 43200, 86400, 172800, 259200, 1000000};
    return seconds[value & 0x0FU];
}

static int
p25_sndcp_mtu_bytes(uint8_t value) {
    switch (value) {
        case 1: return 296;
        case 2: return 510;
        case 3: return 1020;
        case 4: return 1500;
        default: return 0;
    }
}

static void
p25_sndcp_append_ipv4(char* out, size_t out_sz, const uint8_t* p, int off) {
    char ip[48];
    DSD_SNPRINTF(ip, sizeof(ip), " IP:%u.%u.%u.%u", (unsigned)p[off], (unsigned)p[off + 1], (unsigned)p[off + 2],
                 (unsigned)p[off + 3]);
    dsd_append(out, out_sz, ip);
}

static int
p25_sndcp_nat_has_ipv4(uint8_t nat) {
    return nat == 0U || nat == 1U;
}

static const char*
p25_sndcp_deactivate_label(uint8_t value) {
    switch (value) {
        case 0: return "All NSAPIs";
        case 1: return "This NSAPI";
        default: return "Reserved";
    }
}

static void
p25_sndcp_append_activate_request(const uint8_t* p, int plen, char* out_summary, size_t out_sz) {
    if (plen < 10) {
        return;
    }

    uint8_t nat = p25_nibble_lo(p[1]);
    char extra[128];
    DSD_SNPRINTF(extra, sizeof(extra), " Ver:%u NAT:%s DSUT:%u", (unsigned)p25_nibble_hi(p[1]),
                 p25_sndcp_nat_label(nat), (unsigned)p25_nibble_hi(p[6]));
    dsd_append(out_summary, out_sz, extra);
    if (p25_sndcp_nat_has_ipv4(nat)) {
        p25_sndcp_append_ipv4(out_summary, out_sz, p, 2);
    }
    DSD_SNPRINTF(extra, sizeof(extra), " IPComp:%u UDPComp:%u MDP:%u", (unsigned)(p[7] & 0xFFU),
                 (unsigned)p25_nibble_lo(p[6]), (unsigned)(p[9] & 0xFFU));
    dsd_append(out_summary, out_sz, extra);
}

static void
p25_sndcp_append_activate_accept(const uint8_t* p, int plen, char* out_summary, size_t out_sz) {
    if (plen < 11) {
        return;
    }

    uint8_t nat = p25_nibble_lo(p[2]);
    int mtu = p25_sndcp_mtu_bytes(p25_nibble_hi(p[9]));
    char extra[160];
    DSD_SNPRINTF(extra, sizeof(extra), " Priority:%u Ready:%ds Standby:%ds NAT:%s", (unsigned)p25_nibble_hi(p[1]),
                 p25_sndcp_ready_seconds(p25_nibble_lo(p[1])), p25_sndcp_standby_seconds(p25_nibble_hi(p[2])),
                 p25_sndcp_nat_label(nat));
    dsd_append(out_summary, out_sz, extra);
    if (p25_sndcp_nat_has_ipv4(nat)) {
        p25_sndcp_append_ipv4(out_summary, out_sz, p, 3);
    }
    DSD_SNPRINTF(extra, sizeof(extra), " MTU:%d IPComp:%u UDPComp:%u MDP:%u", mtu, (unsigned)(p[7] & 0xFFU),
                 (unsigned)p25_nibble_lo(p[9]), (unsigned)(p[10] & 0xFFU));
    dsd_append(out_summary, out_sz, extra);
}

static void
p25_sndcp_append_reject(const uint8_t* p, int plen, char* out_summary, size_t out_sz) {
    if (plen < 2) {
        return;
    }

    char extra[128];
    DSD_SNPRINTF(extra, sizeof(extra), " Reason:%s", p25_sndcp_reject_reason(p[1]));
    dsd_append(out_summary, out_sz, extra);
}

static void
p25_sndcp_append_deactivate(const uint8_t* p, int plen, char* out_summary, size_t out_sz) {
    if (plen < 2) {
        return;
    }

    char extra[80];
    DSD_SNPRINTF(extra, sizeof(extra), " Deactivate:%s", p25_sndcp_deactivate_label(p25_nibble_hi(p[1])));
    dsd_append(out_summary, out_sz, extra);
}

static void
p25_parse_sap6_sndcp(const uint8_t* p, int plen, int outbound, char* out_summary, size_t out_sz) {
    if (!out_summary || out_sz == 0) {
        return;
    }
    if (!p || plen <= 0) {
        DSD_SNPRINTF(out_summary, out_sz, "SNDCP truncated");
        return;
    }

    uint8_t type = p25_nibble_hi(p[0]);
    uint8_t nsapi = p25_nibble_lo(p[0]);
    DSD_SNPRINTF(out_summary, out_sz, "SNDCP %s NSAPI:%u", p25_sndcp_type_label(type, outbound), (unsigned)nsapi);

    if (!outbound && type == 0) {
        p25_sndcp_append_activate_request(p, plen, out_summary, out_sz);
        return;
    }

    if (outbound && type == 0) {
        p25_sndcp_append_activate_accept(p, plen, out_summary, out_sz);
        return;
    }

    if (outbound && type == 3) {
        p25_sndcp_append_reject(p, plen, out_summary, out_sz);
        return;
    }

    if (type == 2) {
        p25_sndcp_append_deactivate(p, plen, out_summary, out_sz);
    }
}

void
p25_decode_rsp(uint8_t C, uint8_t T, uint8_t S, char* rsp_string, size_t rsp_string_size) {
    if (rsp_string == NULL || rsp_string_size == 0) {
        return;
    }

    if (C == 0) {
        DSD_SNPRINTF(rsp_string, rsp_string_size, " ACK (Success);");
    } else if (C == 2) {
        DSD_SNPRINTF(rsp_string, rsp_string_size, " SACK (Retry);");
    } else if (C == 1) {
        if (T == 0) {
            DSD_SNPRINTF(rsp_string, rsp_string_size, " NACK (Illegal Format);");
        } else if (T == 1) {
            DSD_SNPRINTF(rsp_string, rsp_string_size, " NACK (CRC32 Failure);");
        } else if (T == 2) {
            DSD_SNPRINTF(rsp_string, rsp_string_size, " NACK (Memory Full);");
        } else if (T == 3) {
            DSD_SNPRINTF(rsp_string, rsp_string_size, " NACK (FSN Sequence Error);");
        } else if (T == 4) {
            DSD_SNPRINTF(rsp_string, rsp_string_size, " NACK (Undeliverable);");
        } else if (T == 5) {
            DSD_SNPRINTF(rsp_string, rsp_string_size, " NACK (NS/VR Sequence Error);"); //depreciated
        } else if (T == 6) {
            DSD_SNPRINTF(rsp_string, rsp_string_size, " NACK (Invalid User on System);");
        }
    }

    //catch all for everything else
    else {
        DSD_SNPRINTF(rsp_string, rsp_string_size, " Unknown RSP;");
    }

    DSD_FPRINTF(stderr, " Response Packet:%s C: %X; T: %X; S: %X; ", rsp_string, C, T, S);
}

typedef struct {
    uint8_t sap;
    const char* label;
} P25SapLabel;

static const char*
p25_sap_label(uint8_t sap) {
    static const P25SapLabel labels[] = {
        {0, " User Data;"},
        {1, " Encrypted User Data;"},
        {2, " Circuit Data;"},
        {3, " Circuit Data Control;"},
        {4, " Packet Data;"},
        {5, " Address Resolution Protocol;"},
        {6, " SNDCP Packet Data Control;"},
        {15, " Packet Data Scan Preamble;"},
        {29, " Packet Data Encryption Support;"},
        {31, " Extended Address;"},
        {32, " Registration and Authorization;"},
        {33, " Channel Reassignment;"},
        {34, " System Configuration;"},
        {35, " Mobile Radio Loopback;"},
        {36, " Mobile Radio Statistics;"},
        {37, " Mobile Radio Out of Service;"},
        {38, " Mobile Radio Paging;"},
        {39, " Mobile Radio Configuration;"},
        {40, " Unencrypted Key Management;"},
        {41, " Encrypted Key Management;"},
        {48, " Location Service;"},
        {61, " Trunking Control;"},
        {63, " Encrypted Trunking Control;"},
    };

    for (size_t i = 0; i < (sizeof(labels) / sizeof(labels[0])); i++) {
        if (labels[i].sap == sap) {
            return labels[i].label;
        }
    }
    return " Unknown SAP;";
}

void
p25_decode_sap(uint8_t SAP, char* sap_string, size_t sap_string_size) {
    if (sap_string == NULL || sap_string_size == 0) {
        return;
    }

    DSD_SNPRINTF(sap_string, sap_string_size, "%s", p25_sap_label(SAP));

    DSD_FPRINTF(stderr, "SAP: 0x%02X;%s ", SAP, sap_string);
}

static void
lfsr_64_to_128(uint8_t* iv) {
    uint64_t lfsr = ((uint64_t)iv[0] << 56ULL) + ((uint64_t)iv[1] << 48ULL) + ((uint64_t)iv[2] << 40ULL)
                    + ((uint64_t)iv[3] << 32ULL) + ((uint64_t)iv[4] << 24ULL) + ((uint64_t)iv[5] << 16ULL)
                    + ((uint64_t)iv[6] << 8ULL) + ((uint64_t)iv[7] << 0ULL);

    uint8_t cnt = 0, x = 64;

    for (cnt = 0; cnt < 64; cnt++) {
        //63,61,45,37,27,14
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1
        uint64_t bit = ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1;
        lfsr = (lfsr << 1) | bit;

        // Continue packing iv
        iv[x / 8] = (iv[x / 8] << 1) + bit;

        x++;
    }
}

static void
p25_store_u64_be(uint64_t value, uint8_t* out) {
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)((value >> (56ULL - ((uint64_t)i * 8ULL))) & 0xFFU);
    }
}

static int
p25_bytes_any_nonzero(const uint8_t* bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] != 0U) {
            return 1;
        }
    }
    return 0;
}

static void
p25_load_aes_key(const dsd_state* state, uint16_t key_id, uint8_t aes_key[32]) {
    unsigned long long int parts[4] = {
        state->rkey_array[key_id + 0x000],
        state->rkey_array[key_id + 0x101],
        state->rkey_array[key_id + 0x201],
        state->rkey_array[key_id + 0x301],
    };

    if ((parts[0] == 0) && (parts[1] == 0) && (parts[2] == 0) && (parts[3] == 0)) {
        parts[0] = state->K1;
        parts[1] = state->K2;
        parts[2] = state->K3;
        parts[3] = state->K4;
    }

    for (int part = 0; part < 4; part++) {
        p25_store_u64_be((uint64_t)parts[part], aes_key + ((size_t)part * sizeof(uint64_t)));
    }
}

static uint8_t
p25_build_aes_pdu_keystream(const dsd_opts* opts, const dsd_state* state, uint8_t alg_id, uint16_t key_id,
                            unsigned long long int mi, int len, uint8_t* ks_bytes, int* ks_idx) {
    uint8_t aes_iv[16];
    uint8_t aes_key[32];
    DSD_MEMSET(aes_iv, 0, sizeof(aes_iv));
    DSD_MEMSET(aes_key, 0, sizeof(aes_key));

    *ks_idx = 16; // offset for OFB discard round
    p25_load_aes_key(state, key_id, aes_key);
    if (!p25_bytes_any_nonzero(aes_key, sizeof(aes_key))) {
        return 1;
    }

    p25_store_u64_be((uint64_t)mi, aes_iv);
    lfsr_64_to_128(aes_iv);

    int nblocks = (len / 16) + 1;
    aes_ofb_keystream_output(aes_iv, aes_key, ks_bytes, (alg_id == 0x84) ? 2 : 0, nblocks);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n AES-%s keystream ready", (alg_id == 0x84) ? "256" : "128");
    }
    return 0;
}

static uint8_t
p25_build_des_pdu_keystream(const dsd_opts* opts, const dsd_state* state, uint16_t key_id, unsigned long long int mi,
                            int len, uint8_t* ks_bytes, int* ks_idx) {
    unsigned long long int des_key = state->rkey_array[key_id];
    if (des_key == 0) {
        des_key = state->R;
    }

    *ks_idx = 8; // offset for OFB discard round
    if (des_key == 0) {
        return 1;
    }

    int nblocks = (len / 8) + 1;
    // codeql[cpp/weak-cryptographic-algorithm] DES is required for legacy P25 interoperability.
    des_multi_keystream_output(mi, des_key, ks_bytes, 1, nblocks);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n DES56 keystream ready");
    }
    return 0;
}

static uint8_t
p25_build_rc4_pdu_keystream(const dsd_opts* opts, const dsd_state* state, uint16_t key_id, unsigned long long int mi,
                            int len, uint8_t* ks_bytes, int* ks_idx) {
    unsigned long long int rc4_key = state->rkey_array[key_id];
    if (rc4_key == 0) {
        rc4_key = state->R;
    }

    uint8_t rc4_kiv[13];
    DSD_MEMSET(rc4_kiv, 0, sizeof(rc4_kiv));
    rc4_kiv[0] = (uint8_t)((rc4_key & 0xFF00000000ULL) >> 32);
    rc4_kiv[1] = (uint8_t)((rc4_key & 0xFF000000ULL) >> 24);
    rc4_kiv[2] = (uint8_t)((rc4_key & 0xFF0000ULL) >> 16);
    rc4_kiv[3] = (uint8_t)((rc4_key & 0xFF00ULL) >> 8);
    rc4_kiv[4] = (uint8_t)(rc4_key & 0xFFULL);
    p25_store_u64_be((uint64_t)mi, rc4_kiv + 5);

    *ks_idx = 0;
    if (rc4_key == 0) {
        return 1;
    }

    // codeql[cpp/weak-cryptographic-algorithm] RC4/ADP is required for legacy P25 interoperability.
    rc4_block_output(256, 13, len, rc4_kiv, ks_bytes);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n RC4 keystream ready");
    }
    return 0;
}

uint8_t
p25_decrypt_pdu(const dsd_opts* opts, const dsd_state* state, uint8_t* input, uint8_t alg_id, uint16_t key_id,
                unsigned long long int mi, int len) {

    uint8_t encrypted = 1;

    int ks_idx = 0;
    uint8_t ks_bytes[3096];
    DSD_MEMSET(ks_bytes, 0, sizeof(ks_bytes));

    if (alg_id == 0x84 || alg_id == 0x89) {
        encrypted = p25_build_aes_pdu_keystream(opts, state, alg_id, key_id, mi, len, ks_bytes, &ks_idx);
    } else if (alg_id == 0x81) {
        encrypted = p25_build_des_pdu_keystream(opts, state, key_id, mi, len, ks_bytes, &ks_idx);
    } else if (alg_id == 0xAA) {
        encrypted = p25_build_rc4_pdu_keystream(opts, state, key_id, mi, len, ks_bytes, &ks_idx);
    }

    //debug input offset

    //apply keystream
    for (int i = 0; i < len; i++) { //need to subtract pad bytes and crc bytes from keystream application
        input[i] ^= ks_bytes[i + ks_idx];
    }

    if (alg_id == 0x80) {
        encrypted = 0;
    }

    return encrypted;
}

//SAP 1
uint8_t
p25_decode_es_header(const dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t* sap, int* ptr, int len) {

    uint8_t encrypted = 0;

    uint8_t bits[13 * 8];
    DSD_MEMSET(bits, 0, sizeof(bits));
    unpack_byte_array_into_bit_array(input, bits, 13);

    DSD_FPRINTF(stderr, "%s", KYEL);
    unsigned long long int mi = (unsigned long long int)convert_bits_into_output(bits, 64);
    uint8_t mi_res = (uint8_t)convert_bits_into_output(bits + 64, 8);
    uint8_t alg_id = (uint8_t)convert_bits_into_output(bits + 72, 8);
    uint16_t key_id = (uint16_t)convert_bits_into_output(bits + 80, 16);
    DSD_FPRINTF(stderr, "\n ES Aux Encryption Header; ALG: %02X; KEY ID: %04X; MI: %016llX; ", alg_id, key_id, mi);
    if (mi_res != 0) {
        DSD_FPRINTF(stderr, " RES: %02X;", mi_res);
    }

    //The Auxiliary Header signals the actual SAP value of the encrypted message (this byte is not encrypted)
    uint8_t aux_res = (uint8_t)convert_bits_into_output(
        &bits[96],
        2); //these two bits should always be signalled as 1's, so 0b11, and if combined with the 2ndary SAP, 0xC0 if SAP == 0x00
    uint8_t aux_sap =
        (uint8_t)convert_bits_into_output(&bits[98], 6); //the SAP of the message that is encrypted immediately after
    char aux_sap_string[99];
    p25_decode_sap(aux_sap, aux_sap_string, sizeof aux_sap_string);
    DSD_FPRINTF(stderr, "%s", KNRM);
    UNUSED(aux_res);

    //Decrypt PDU
    if (alg_id != 0x80) {
        encrypted = p25_decrypt_pdu(opts, state, input + 13, alg_id, key_id, mi, len - 13);
    }

    *sap = aux_sap;
    *ptr += 13;

    //append enc at this point
    if (encrypted) {
        char ess_str[200];
        DSD_MEMSET(ess_str, 0, sizeof(ess_str));
        DSD_SNPRINTF(ess_str, sizeof(ess_str), "ALG: %02X; KID: %04X; SAP:%02X;%s", alg_id, key_id, aux_sap,
                     aux_sap_string);
        dsd_append(state->dmr_lrrp_gps[0], sizeof state->dmr_lrrp_gps[0], ess_str);
    }

    return encrypted;
}

//SAP 31 //Extended Addressing
void
p25_decode_extended_address(dsd_opts* opts, dsd_state* state, const uint8_t* input, uint8_t* sap, int* ptr) {

    UNUSED(opts);

    uint8_t bits[12 * 8];
    DSD_MEMSET(bits, 0, sizeof(bits));
    unpack_byte_array_into_bit_array(input, bits, 12);

    uint8_t ea_sap = (uint8_t)convert_bits_into_output(bits + 10, 6);
    uint8_t ea_mfid = (uint8_t)convert_bits_into_output(bits + 16, 6);
    uint32_t ea_llid = (uint32_t)convert_bits_into_output(bits + 24, 24);
    uint32_t ea_res = (uint32_t)convert_bits_into_output(bits + 48, 32);
    uint16_t ea_crc = (uint16_t)convert_bits_into_output(bits + 80, 16);

    DSD_FPRINTF(stderr, "\n Extended Addressing Header; MFID: %02X; SRC LLID: %d; RES: %08X; CRC: %04X; ", ea_mfid,
                ea_llid, ea_res, ea_crc);
    char ea_sap_string[99];
    p25_decode_sap(ea_sap, ea_sap_string, sizeof ea_sap_string);
    UNUSED(ea_sap_string);

    //Print to Data Call String for Ncurses Terminal
    state->lastsrc = ea_llid;
    char ea_str[200];
    DSD_MEMSET(ea_str, 0, sizeof(ea_str));
    DSD_SNPRINTF(ea_str, sizeof(ea_str), "EXT ADD SRC: %d; SAP:%02X;%s", ea_llid, ea_sap, ea_sap_string);
    dsd_append(state->dmr_lrrp_gps[0], sizeof state->dmr_lrrp_gps[0], ea_str);

    *sap = ea_sap;
    *ptr += 12;
}

typedef struct {
    uint8_t an;
    uint8_t io;
    uint8_t fmt;
    uint8_t sap;
    uint8_t mfid;
    uint32_t address;
    uint8_t blks;
    uint8_t fmf;
    uint8_t pad;
    uint8_t ns;
    uint8_t fsnf;
    uint8_t offset;
    uint8_t rsp_class;
    uint8_t rsp_type;
    uint8_t rsp_status;
} P25PduHeaderFields;

static int
p25_sap_is_trunking_control(uint8_t sap) {
    return sap == 61 || sap == 63;
}

static P25PduHeaderFields
p25_read_pdu_header_fields(const uint8_t* input) {
    P25PduHeaderFields h;
    h.an = (input[0] >> 6) & 0x1;
    h.io = (input[0] >> 5) & 0x1;
    h.fmt = input[0] & 0x1F;
    h.sap = input[1] & 0x3F;
    h.mfid = input[2];
    h.address = (input[3] << 16) | (input[4] << 8) | input[5];
    h.blks = input[6] & 0x7F;
    h.fmf = (input[6] >> 7) & 0x1;
    h.pad = input[7] & 0x1F;
    h.ns = (input[8] >> 4) & 0x7;
    h.fsnf = input[8] & 0xF;
    h.offset = input[9] & 0x3F;
    h.rsp_class = (input[1] >> 6) & 0x3;
    h.rsp_type = (input[1] >> 3) & 0x7;
    h.rsp_status = (input[1] >> 0) & 0x7;
    return h;
}

static void
p25_decode_pdu_header_strings(const P25PduHeaderFields* h, char* sap_string, size_t sap_string_size, char* rsp_string,
                              size_t rsp_string_size) {
    DSD_SNPRINTF(sap_string, sap_string_size, "%s", " ");
    DSD_SNPRINTF(rsp_string, rsp_string_size, "%s", " ");
    if (h->fmt != 3) {
        p25_decode_sap(h->sap, sap_string, sap_string_size);
    } else {
        p25_decode_rsp(h->rsp_class, h->rsp_type, h->rsp_status, rsp_string, rsp_string_size);
    }
}

static void
p25_log_pdu_header_fields(const P25PduHeaderFields* h) {
    if (p25_sap_is_trunking_control(h->sap)) {
        return;
    }

    DSD_FPRINTF(stderr, "\n F: %d; Blocks: %02X; Pad: %d; NS: %d; FSNF: %d; Offset: %d; MFID: %02X;", h->fmf, h->blks,
                h->pad, h->ns, h->fsnf, h->offset, h->mfid);
    if (h->io == 1) {
        DSD_FPRINTF(stderr, " DST LLID: %d;", h->address);
    } else {
        DSD_FPRINTF(stderr, " SRC LLID: %d;", h->address);
    }
}

static void
p25_update_pdu_header_state(dsd_opts* opts, dsd_state* state, const P25PduHeaderFields* h, const char* sap_string,
                            const char* rsp_string) {
    if (p25_sap_is_trunking_control(h->sap)) {
        return;
    }

    if (h->fmt != 3) {
        DSD_SNPRINTF(state->dmr_lrrp_gps[0], sizeof(state->dmr_lrrp_gps[0]), "Data Call:%s SAP:%02X; LLID: %d; ",
                     sap_string, h->sap, h->address);
    } else {
        DSD_SNPRINTF(state->dmr_lrrp_gps[0], sizeof(state->dmr_lrrp_gps[0]), "Data Call Response:%s LLID: %d; ",
                     rsp_string, h->address);
        state->lastsrc = 0xFFFFFF;
        watchdog_event_datacall(opts, state, state->lastsrc, state->lasttg, state->dmr_lrrp_gps[0], 0);
        state->lastsrc = 0;
        state->lasttg = 0;
        state->p25_policy_tg[0] = 0;
        watchdog_event_history(opts, state, 0);
        dsd_p25_optional_hook_watchdog_event_current(opts, state, 0);
    }

    if ((uint32_t)state->lasttg != h->address) {
        state->p25_policy_tg[0] = 0;
    }
    state->lasttg = h->address;
    state->lastsrc = 0xFFFFFF; // none given, unless extended, so put any here for now
}

//PDU Format Header Decode
void
p25_decode_pdu_header(dsd_opts* opts, dsd_state* state, const uint8_t* input) {
    P25PduHeaderFields h = p25_read_pdu_header_fields(input);

    DSD_FPRINTF(stderr, "%s", KGRN);
    DSD_FPRINTF(stderr, " P25 Data - AN: %d; IO: %d; FMT: %02X; ", h.an, h.io, h.fmt);
    char sap_string[40];
    char rsp_string[40];
    p25_decode_pdu_header_strings(&h, sap_string, sizeof sap_string, rsp_string, sizeof rsp_string);
    p25_log_pdu_header_fields(&h);
    p25_update_pdu_header_state(opts, state, &h, sap_string, rsp_string);
}

typedef struct {
    uint8_t sap;
    uint8_t fmt;
    uint8_t io;
    uint8_t mfid;
    uint32_t llid;
    uint8_t blks;
    uint8_t pad;
    uint8_t offset;
} P25PduDataFields;

static P25PduDataFields
p25_read_pdu_data_fields(const uint8_t* input) {
    P25PduDataFields pdu;
    pdu.sap = input[1] & 0x3F;
    pdu.fmt = input[0] & 0x1F;
    pdu.io = (input[0] >> 5) & 0x1;
    pdu.mfid = input[2];
    pdu.llid = (input[3] << 16) | (input[4] << 8) | input[5];
    pdu.blks = input[6] & 0x7F;
    pdu.pad = input[7] & 0x1F;
    pdu.offset = input[9] & 0x3F;
    return pdu;
}

static int
p25_pdu_payload_len(int len, uint8_t pad) {
    if (len > (12 + 4 + pad)) {
        return len - (12 + 4 + pad);
    }
    return len;
}

static int
p25_pdu_payload_span(int len, int ptr) {
    int consumed_after_header = ptr - 12;
    if (consumed_after_header < 0) {
        consumed_after_header = 0;
    }
    if (len <= consumed_after_header) {
        return 0;
    }
    return len - consumed_after_header;
}

static void
p25_emit_pdu_json_for_fields(const P25PduDataFields* pdu, int len, int encrypted, const char* summary) {
    p25_pdu_json_fields fields = {pdu->fmt, pdu->sap,    pdu->mfid, pdu->io,   pdu->llid, pdu->blks,
                                  pdu->pad, pdu->offset, len,       encrypted, summary};
    p25_emit_pdu_json_if_enabled(&fields);
}

static void
p25_handle_sap32_regauth_data(dsd_opts* opts, dsd_state* state, const P25PduDataFields* pdu, const uint8_t* payload,
                              int len, int ptr, int encrypted) {
    char summary[128] = {0};
    p25_parse_sap32_regauth(opts, state, payload, p25_pdu_payload_span(len, ptr), summary, sizeof(summary));
    if (summary[0] != '\0') {
        DSD_SNPRINTF(state->dmr_lrrp_gps[0], sizeof(state->dmr_lrrp_gps[0]), "RegAuth: %s", summary);
    }
    p25_emit_pdu_json_for_fields(pdu, len, encrypted, summary);
}

static void
p25_handle_sap34_syscfg_data(dsd_opts* opts, dsd_state* state, const P25PduDataFields* pdu, const uint8_t* payload,
                             int len, int ptr, int encrypted) {
    char summary[128] = {0};
    p25_parse_sap34_syscfg(opts, state, payload, p25_pdu_payload_span(len, ptr), summary, sizeof(summary));
    if (summary[0] != '\0') {
        DSD_SNPRINTF(state->dmr_lrrp_gps[0], sizeof(state->dmr_lrrp_gps[0]), "SysCfg: %s", summary);
    }
    p25_emit_pdu_json_for_fields(pdu, len, encrypted, summary);
}

static void
p25_handle_sap6_sndcp_data(dsd_opts* opts, dsd_state* state, const P25PduDataFields* pdu, const uint8_t* payload,
                           int len, int ptr, int encrypted) {
    UNUSED(opts);
    char summary[256] = {0};
    int span = p25_pdu_payload_span(len, ptr);
    p25_parse_sap6_sndcp(payload, span, pdu->io == 1, summary, sizeof(summary));
    if (summary[0] != '\0') {
        DSD_FPRINTF(stderr, " %s;", summary);
        DSD_SNPRINTF(state->dmr_lrrp_gps[0], sizeof(state->dmr_lrrp_gps[0]), "%s", summary);
    }
    p25_emit_pdu_json_for_fields(pdu, len, encrypted, summary);
}

static int
p25_handle_sap4_packet_data(dsd_opts* opts, dsd_state* state, const P25PduDataFields* pdu, uint8_t* input, int len,
                            int ptr, int encrypted) {
    uint8_t emitted = 0;
    if (pdu->offset == 2 && len >= 2) {
        const uint8_t* header = input + 12;
        uint8_t type = p25_nibble_hi(header[0]);
        uint8_t nsapi = p25_nibble_lo(header[0]);
        uint8_t ip_comp = p25_nibble_hi(header[1]);
        uint8_t udp_comp = p25_nibble_lo(header[1]);
        char summary[160];
        DSD_SNPRINTF(summary, sizeof(summary), "SNDCP Packet Header %s NSAPI:%u IPComp:%u UDPComp:%u",
                     p25_sndcp_type_label(type, pdu->io == 1), (unsigned)nsapi, (unsigned)ip_comp, (unsigned)udp_comp);
        DSD_FPRINTF(stderr, " %s;", summary);
        DSD_SNPRINTF(state->dmr_lrrp_gps[0], sizeof(state->dmr_lrrp_gps[0]), "%s", summary);
        p25_emit_pdu_json_for_fields(pdu, len, encrypted, summary);
        emitted = 1;
    }
    decode_ip_pdu(opts, state, (uint16_t)(len + 1), input + ptr);
    return emitted;
}

static void
p25_store_lrrp_text_for_history(dsd_state* state) {
    if (state == NULL || state->event_history_s == NULL) {
        return;
    }
    if (state->event_history_s[0].Event_History_Items[0].text_message[0] == '\0') {
        return;
    }

    const char* src = (const char*)state->event_history_s[0].Event_History_Items[0].text_message;
    size_t cap = sizeof(state->dmr_lrrp_gps[0]);
    size_t maxcpy = cap - 7 - 1; /* prefix "LRRP: " + N + NUL */
    DSD_SNPRINTF(state->dmr_lrrp_gps[0], cap, "LRRP: %.*s", (int)maxcpy, src);
    DSD_SNPRINTF(state->event_history_s[0].Event_History_Items[0].gps_s,
                 sizeof(state->event_history_s[0].Event_History_Items[0].gps_s), "%s", state->dmr_lrrp_gps[0]);
}

static void
p25_handle_sap48_location_data(const dsd_opts* opts, dsd_state* state, const P25PduDataFields* pdu,
                               const uint8_t* payload, int len, int ptr, int encrypted) {
    int span = p25_pdu_payload_span(len, ptr);
    if (span <= 0) {
        p25_emit_pdu_json_for_fields(pdu, len, encrypted, "");
        return;
    }
    if (span > P25_PDU_MAX_DECODE_BYTES) {
        span = P25_PDU_MAX_DECODE_BYTES;
    }

    uint8_t nmea_valid = 0;
    if (payload[0] == (uint8_t)'$' || payload[0] == (uint8_t)'!') {
        uint8_t payload_bits[P25_PDU_MAX_DECODE_BYTES * 8];
        DSD_MEMSET(payload_bits, 0, sizeof(payload_bits));
        unpack_byte_array_into_bit_array(payload, payload_bits, span);

        uint8_t slot = (state->currentslot >= 2) ? 1U : (uint8_t)state->currentslot;
        state->dmr_lrrp_source[slot] = (uint32_t)state->lastsrc;
        state->dmr_lrrp_target[slot] = (uint32_t)state->lasttg;
        nmea_valid = nmea_sentence_checker(opts, state, payload_bits, slot, span);
    }

    if (!nmea_valid) {
        uint8_t write_history = (state != NULL && state->event_history_s != NULL) ? 1U : 0U;
        utf8_to_text(state, write_history, (uint16_t)span, payload);
    }
    p25_store_lrrp_text_for_history(state);
    const char* summary = "";
    if (state != NULL && state->event_history_s != NULL) {
        summary = state->event_history_s[0].Event_History_Items[0].text_message;
    }
    p25_emit_pdu_json_for_fields(pdu, len, encrypted, summary);
}

static int
p25_decode_clear_pdu_payload(dsd_opts* opts, dsd_state* state, const P25PduDataFields* pdu, uint8_t* input, int len,
                             int ptr, int encrypted) {
    uint8_t* payload = input + ptr;
    switch (pdu->sap) {
        case 0: decode_ip_pdu(opts, state, (uint16_t)(len + 1), payload); return 0;
        case 4: return p25_handle_sap4_packet_data(opts, state, pdu, input, len, ptr, encrypted);
        case 6: p25_handle_sap6_sndcp_data(opts, state, pdu, payload, len, ptr, encrypted); return 1;
        case 32: p25_handle_sap32_regauth_data(opts, state, pdu, payload, len, ptr, encrypted); return 1;
        case 34: p25_handle_sap34_syscfg_data(opts, state, pdu, payload, len, ptr, encrypted); return 1;
        case 48: p25_handle_sap48_location_data(opts, state, pdu, payload, len, ptr, encrypted); return 1;
        default: break;
    }
    return 0;
}

static uint8_t
p25_decode_pdu_optional_headers(dsd_opts* opts, dsd_state* state, uint8_t* input, P25PduDataFields* pdu, int* ptr,
                                int len) {
    uint8_t encrypted = 0;
    if (pdu->sap == 31) {
        p25_decode_extended_address(opts, state, input + *ptr, &pdu->sap, ptr);
    }
    if (pdu->sap == 1) {
        encrypted = p25_decode_es_header(opts, state, input + *ptr, &pdu->sap, ptr, len);
    }
    return encrypted;
}

//user or other data delivered via PDU format
void
p25_decode_pdu_data(dsd_opts* opts, dsd_state* state, uint8_t* input, int len) {
    P25PduDataFields pdu = p25_read_pdu_data_fields(input);
    uint8_t encrypted = 0;
    int json_emitted = 0;
    int ptr = 12; //initial ptr index value past the first header

    len = p25_pdu_payload_len(len, pdu.pad);
    DSD_FPRINTF(stderr, " PDU Len: %d;", len);

    encrypted = p25_decode_pdu_optional_headers(opts, state, input, &pdu, &ptr, len);
    if (!encrypted) {
        if (pdu.offset) {
            ptr = 12 + pdu.offset;
        }
        json_emitted = p25_decode_clear_pdu_payload(opts, state, &pdu, input, len, ptr, encrypted);
    } else {
        DSD_FPRINTF(stderr, " Encrypted PDU;");
    }

    if (!json_emitted && pdu.sap != 32 && pdu.sap != 34 && pdu.sap != 48) {
        p25_emit_pdu_json_for_fields(&pdu, len, encrypted, "");
    }

    watchdog_event_datacall(opts, state, state->lastsrc, state->lasttg, state->dmr_lrrp_gps[0], 0);
    state->lastsrc = 0;
    state->lasttg = 0;
    state->p25_policy_tg[0] = 0;
    watchdog_event_history(opts, state, 0);
    dsd_p25_optional_hook_watchdog_event_current(opts, state, 0);
}
