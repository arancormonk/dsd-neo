// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MLE PDU dispatcher and CMCE call-control decoder.
 * ETSI EN 300 392-2 §21.6 (D-MLE) and Chapter 14 (CMCE).
 *
 * Phase 9: decode the TM-SDU carried by MAC-RESOURCE to extract CMCE
 * call-setup / release events and update dsd_state accordingly.
 */

#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>

/* Phase 81: bits_to_uint unified — see tetra_bits.h */
#define mle_bits_to_uint  tetra_bits_to_uint

#define TETRA_SDS_CONCAT_CONTEXTS 4
#define TETRA_SDS_CONCAT_PARTS 255
#define TETRA_SDS_CONCAT_PART_BITS 2047
#define TETRA_SDS_CONCAT_PART_BYTES ((TETRA_SDS_CONCAT_PART_BITS + 7) / 8)
#define TETRA_SDS_CONCAT_TIMEOUT_SECONDS 300

typedef struct {
    uint8_t valid;
    uint8_t with_transport;
    uint8_t total;
    uint8_t received_count;
    uint8_t payload_pid_known;
    uint8_t payload_pid;
    uint16_t reference;
    uint32_t src_ssi;
    time_t updated;
    uint16_t part_bits[TETRA_SDS_CONCAT_PARTS];
    uint8_t received[(TETRA_SDS_CONCAT_PARTS + 7) / 8];
    uint8_t data[TETRA_SDS_CONCAT_PARTS][TETRA_SDS_CONCAT_PART_BYTES];
} tetra_sds_concat_ctx_t;

static tetra_sds_concat_ctx_t g_sds_concat[TETRA_SDS_CONCAT_CONTEXTS];

void
tetra_sds_concat_reset(void)
{
    memset(g_sds_concat, 0, sizeof(g_sds_concat));
}

static int
sds_concat_received(const tetra_sds_concat_ctx_t *ctx, unsigned index)
{
    return (ctx->received[index >> 3] >> (index & 7u)) & 1u;
}

static void
sds_concat_set_received(tetra_sds_concat_ctx_t *ctx, unsigned index)
{
    ctx->received[index >> 3] |= (uint8_t)(1u << (index & 7u));
}

/* Return 1 for complete, 0 for accepted/incomplete, 2 for an identical
 * duplicate, and -1 for malformed/conflicting input or allocation failure. */
static int
sds_concat_submit(uint32_t src_ssi, uint16_t reference, uint8_t with_transport,
                  uint8_t total, uint8_t sequence, int payload_pid,
                  const uint8_t *part, int part_bits, uint8_t **assembled,
                  int *assembled_bits, uint8_t *assembled_pid,
                  uint8_t *received_count)
{
    time_t now = time(NULL);
    tetra_sds_concat_ctx_t *ctx = NULL;
    tetra_sds_concat_ctx_t *free_ctx = NULL;
    tetra_sds_concat_ctx_t *oldest = &g_sds_concat[0];

    *assembled = NULL;
    *assembled_bits = 0;
    if (total < 2 || sequence == 0 || sequence > total || part_bits < 0
        || part_bits > TETRA_SDS_CONCAT_PART_BITS || (sequence == 1 && payload_pid < 0))
        return -1;

    for (unsigned i = 0; i < TETRA_SDS_CONCAT_CONTEXTS; i++) {
        tetra_sds_concat_ctx_t *candidate = &g_sds_concat[i];
        if (candidate->valid && now != (time_t)-1 && candidate->updated != (time_t)-1
            && now - candidate->updated > TETRA_SDS_CONCAT_TIMEOUT_SECONDS)
            memset(candidate, 0, sizeof(*candidate));
        if (!candidate->valid && !free_ctx) free_ctx = candidate;
        if (candidate->valid && candidate->updated < oldest->updated) oldest = candidate;
        if (candidate->valid && candidate->src_ssi == src_ssi
            && candidate->reference == reference
            && candidate->with_transport == with_transport) {
            ctx = candidate;
            break;
        }
    }
    if (ctx && ctx->total != total) {
        memset(ctx, 0, sizeof(*ctx));
        return -1;
    }
    if (!ctx) {
        ctx = free_ctx ? free_ctx : oldest;
        memset(ctx, 0, sizeof(*ctx));
        ctx->valid = 1;
        ctx->src_ssi = src_ssi;
        ctx->reference = reference;
        ctx->with_transport = with_transport;
        ctx->total = total;
    }
    ctx->updated = now;

    unsigned index = (unsigned)sequence - 1u;
    unsigned bytes = ((unsigned)part_bits + 7u) / 8u;
    uint8_t packed[TETRA_SDS_CONCAT_PART_BYTES] = {0};
    for (int i = 0; i < part_bits; i++)
        packed[(unsigned)i >> 3] |= (uint8_t)((part[i] & 1u) << (7u - ((unsigned)i & 7u)));

    if (sds_concat_received(ctx, index)) {
        if (ctx->part_bits[index] != (uint16_t)part_bits
            || memcmp(ctx->data[index], packed, bytes) != 0) {
            memset(ctx, 0, sizeof(*ctx));
            return -1;
        }
        *received_count = ctx->received_count;
        return 2;
    }
    memcpy(ctx->data[index], packed, bytes);
    ctx->part_bits[index] = (uint16_t)part_bits;
    sds_concat_set_received(ctx, index);
    ctx->received_count++;
    if (sequence == 1) {
        ctx->payload_pid = (uint8_t)payload_pid;
        ctx->payload_pid_known = 1;
    }
    *received_count = ctx->received_count;
    if (ctx->received_count != ctx->total || !ctx->payload_pid_known) return 0;

    unsigned total_bits = 0;
    for (unsigned i = 0; i < ctx->total; i++) {
        if (!sds_concat_received(ctx, i)) return 0;
        total_bits += ctx->part_bits[i];
    }
    uint8_t *result = total_bits ? (uint8_t *)malloc(total_bits) : (uint8_t *)malloc(1);
    if (!result) {
        memset(ctx, 0, sizeof(*ctx));
        return -1;
    }
    unsigned out = 0;
    for (unsigned i = 0; i < ctx->total; i++) {
        for (unsigned bit = 0; bit < ctx->part_bits[i]; bit++)
            result[out++] = (uint8_t)((ctx->data[i][bit >> 3] >> (7u - (bit & 7u))) & 1u);
    }
    *assembled = result;
    *assembled_bits = (int)total_bits;
    *assembled_pid = ctx->payload_pid;
    memset(ctx, 0, sizeof(*ctx));
    return 1;
}

static int cmce_matches_active_call(const dsd_state *state, uint32_t call_id)
{
    return !state || !state->tetra_call_active || state->tetra_call_id == call_id;
}

/* Annex E.1.1: after all type-2 P-bits, an M-bit terminates the PDU or
 * introduces a length-delimited type-3/4 element. Unknown elements are safe
 * to skip because their 11-bit length covers the complete following value. */
static int cmce_skip_type34(const uint8_t *bits, int nbits, int *off)
{
    if (!bits || !off || *off >= nbits)
        return 0;
    for (;;) {
        uint32_t more = mle_bits_to_uint(bits, *off, 1); (*off)++;
        if (!more)
            return 1;
        if (*off + 15 > nbits)
            return 0;
        *off += 4; /* type-3/4 element identifier */
        uint32_t length = mle_bits_to_uint(bits, *off, 11); *off += 11;
        if (length == 0 || length > (uint32_t)(nbits - *off))
            return 0;
        *off += (int)length;
        if (*off >= nbits)
            return 0;
    }
}

/* -----------------------------------------------------------------------
 * CMCE D-SETUP, ETSI EN 300 392-2 table 14.15.
 * Mandatory part: type(5), call id(14), timeout(4), hook(1), duplex(1),
 * basic service(8), transmission grant(2), request permission(1), priority(4).
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_setup(const uint8_t *bits, int nbits,
                                int cc, dsd_state *state)
{
    if (nbits < 41) {
        fprintf(stderr, "[TETRA CMCE D-SETUP] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    int off=5;
    uint16_t call_id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t timeout=(uint8_t)mle_bits_to_uint(bits,off,4); off+=4;
    uint8_t hook=(uint8_t)mle_bits_to_uint(bits,off,1); off++;
    uint8_t duplex=(uint8_t)mle_bits_to_uint(bits,off,1); off++;
    uint8_t basic=(uint8_t)mle_bits_to_uint(bits,off,8); off+=8;
    uint8_t grant=(uint8_t)mle_bits_to_uint(bits,off,2); off+=2;
    uint8_t request=(uint8_t)mle_bits_to_uint(bits,off,1); off++;
    uint8_t priority=(uint8_t)mle_bits_to_uint(bits,off,4); off+=4;
    uint8_t circuit=(uint8_t)(basic>>5);
    uint8_t encrypted=(uint8_t)((basic>>4)&1u);
    uint8_t communication=(uint8_t)((basic>>2)&3u);
    uint8_t service=(uint8_t)(basic&3u);
    uint8_t notification=0, notification_valid=0, cpti=0, cpti_valid=0;
    uint32_t temporary_address=0, calling_ssi=0, calling_extension=0;
    uint8_t temporary_address_valid=0, calling_ssi_valid=0, calling_extension_valid=0;

    /* Annex E.1.1 O-bit, followed by the table-ordered type-2 fields. */
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if (optional_tail) {
        if (off>=nbits) goto truncated;
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) {
            if (off+6>nbits) goto truncated;
            notification=(uint8_t)mle_bits_to_uint(bits,off,6); off+=6;
            notification_valid=1;
        }
    }
    if (optional_tail) {
        if (off>=nbits) goto truncated;
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) {
            if (off+24>nbits) goto truncated;
            temporary_address=mle_bits_to_uint(bits,off,24); off+=24;
            temporary_address_valid=1;
        }
    }
    if (optional_tail) {
        if (off>=nbits) goto truncated;
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) {
            if (off+2>nbits) goto truncated;
            cpti=(uint8_t)mle_bits_to_uint(bits,off,2); off+=2;
            cpti_valid=1;
            if (cpti==1 || cpti==2) {
                if (off+24>nbits) goto truncated;
                calling_ssi=mle_bits_to_uint(bits,off,24); off+=24;
                calling_ssi_valid=1;
            }
            if (cpti==2) {
                if (off+24>nbits) goto truncated;
                calling_extension=mle_bits_to_uint(bits,off,24); off+=24;
                calling_extension_valid=1;
            }
        }
    }

    if (optional_tail && !cmce_skip_type34(bits,nbits,&off))
        goto truncated;

    fprintf(stderr,"[TETRA CMCE D-SETUP] CC=%d call=%u timeout=%u circuit=%u enc=%u com=%u service=%u grant=%u request=%u priority=%u",
            cc,(unsigned)call_id,timeout,circuit,encrypted,communication,service,grant,request,priority);
    if (calling_ssi) fprintf(stderr," calling_SSI=%u",calling_ssi);
    fprintf(stderr,"\n");
    if (state) {
        state->tetra_call_active=1;
        state->tetra_call_id=call_id;
        state->tetra_call_timeout=timeout;
        state->tetra_call_type=circuit; /* compatibility: now the standardized circuit-mode type */
        state->tetra_call_slots=service;
        state->tetra_cmce_duplex=duplex;
        state->tetra_cmce_notif=notification;
        state->tetra_cmce_com_type=communication;
        state->tetra_cmce_called_type=cpti; /* compatibility field contains calling-party type */
        state->tetra_cmce_setup_hook=hook;
        state->tetra_cmce_setup_basic_service=basic;
        state->tetra_cmce_setup_tx_grant=grant;
        state->tetra_cmce_setup_tx_permission=request;
        state->tetra_cmce_setup_priority=priority;
        state->tetra_cmce_setup_notification_valid=notification_valid;
        state->tetra_cmce_setup_temporary_address=temporary_address;
        state->tetra_cmce_setup_temporary_address_valid=temporary_address_valid;
        state->tetra_cmce_setup_calling_type_valid=cpti_valid;
        state->tetra_cmce_setup_calling_extension=calling_extension;
        state->tetra_cmce_setup_calling_extension_valid=calling_extension_valid;
        state->tetra_enc_mode=encrypted;
        state->tetra_calling_ssi=calling_ssi_valid?calling_ssi:0;
        state->tetra_cmce_call_generation++;
    }
    return;

truncated:
    fprintf(stderr, "[TETRA CMCE D-SETUP] CC=%d (truncated optional IE: %d bits)\n",
            cc, nbits);
}

/* -----------------------------------------------------------------------
 * CMCE D-RELEASE / D-DISCONNECT, tables 14.12 and 14.9.
 * Mandatory part: type(5), call identifier(14), disconnect cause(5).
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_release(const uint8_t *bits, int nbits, int cc,
                                  int is_disconnect, dsd_opts *opts, dsd_state *state)
{
    if (nbits < 25) {
        fprintf(stderr,"[TETRA CMCE %s] CC=%d (too short: %d bits)\n",is_disconnect?"D-DISCONNECT":"D-RELEASE",cc,nbits);
        return;
    }
    uint16_t call_id=(uint16_t)mle_bits_to_uint(bits,5,14);
    uint8_t cause=(uint8_t)mle_bits_to_uint(bits,19,5);
    uint8_t notification=0,notification_valid=0;
    int off=24;
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if(optional_tail){
        if(off>=nbits)
            goto truncated;
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if(present){
            if(off+6>nbits)
                goto truncated;
            notification=(uint8_t)mle_bits_to_uint(bits,off,6);
            off+=6;
            notification_valid=1;
        }
        if(!cmce_skip_type34(bits,nbits,&off))
            goto truncated;
    }
    fprintf(stderr,"[TETRA CMCE %s] CC=%d call=%u cause=%u\n",is_disconnect?"D-DISCONNECT":"D-RELEASE",cc,(unsigned)call_id,cause);
    if (state) {
        if (state->tetra_call_active && state->tetra_call_id != call_id) {
            fprintf(stderr,
                    "[TETRA CMCE %s] CC=%d call=%u ignored; active call=%u\n",
                    is_disconnect ? "D-DISCONNECT" : "D-RELEASE", cc,
                    (unsigned)call_id, (unsigned)state->tetra_call_id);
            return;
        }
        state->tetra_call_active=0;
        state->tetra_tx_granted_valid=0;
        state->tetra_tx_granted_ssi=0;
        state->tetra_cmce_tx_granted_notification_valid=0;
        state->tetra_cmce_tx_granted_party_type_valid=0;
        state->tetra_cmce_tx_granted_party_extension_valid=0;
        state->tetra_tx_continue=0;
        state->tetra_tx_interrupted=0;
        state->tetra_tx_wait=0;
        state->tetra_tx_event_notification_valid=0;
        state->tetra_tx_event_party_type_valid=0;
        state->tetra_tx_event_party_ssi_valid=0;
        state->tetra_tx_event_party_extension_valid=0;
        state->tetra_call_id=call_id;
        state->tetra_cmce_release_cause_type=0;
        state->tetra_cmce_release_cause=cause;
        state->tetra_cmce_release_notification=notification;
        state->tetra_cmce_release_notification_valid=notification_valid;
        state->tetra_cmce_release_was_disconnect=(uint8_t)(is_disconnect?1:0);
        tetra_sm_on_release(opts,state);
    }
    return;

truncated:
    fprintf(stderr,"[TETRA CMCE %s] CC=%d (truncated optional IE: %d bits)\n",
            is_disconnect?"D-DISCONNECT":"D-RELEASE",cc,nbits);
}

/* -----------------------------------------------------------------------
 * CMCE D-CONNECT, ETSI EN 300 392-2 table 14.7.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_connect(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if (nbits<30) { fprintf(stderr,"[TETRA CMCE D-CONNECT] CC=%d (too short: %d bits)\n",cc,nbits); return; }
    int off=5; uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t timeout=(uint8_t)mle_bits_to_uint(bits,off,4); off+=4;
    uint8_t hook=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t duplex=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t grant=(uint8_t)mle_bits_to_uint(bits,off,2); off+=2;
    uint8_t permission=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t ownership=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t priority=0,basic=0,notification=0;
    uint32_t temporary_address=0;
    uint8_t priority_valid=0,basic_valid=0,temporary_address_valid=0,notification_valid=0;
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if(optional_tail){
#define CONNECT_TYPE2(width,value,valid) do { \
        uint8_t present; if(off>=nbits) goto truncated; \
        present=(uint8_t)mle_bits_to_uint(bits,off++,1); \
        if(present){if(off+(width)>nbits) goto truncated;(value)=mle_bits_to_uint(bits,off,(width));off+=(width);(valid)=1;} \
    } while(0)
        CONNECT_TYPE2(4,priority,priority_valid);
        CONNECT_TYPE2(8,basic,basic_valid);
        CONNECT_TYPE2(24,temporary_address,temporary_address_valid);
        CONNECT_TYPE2(6,notification,notification_valid);
#undef CONNECT_TYPE2
        if(!cmce_skip_type34(bits,nbits,&off)) goto truncated;
    }
    fprintf(stderr,"[TETRA CMCE D-CONNECT] CC=%d call=%u timeout=%u grant=%u request=%u\n",cc,(unsigned)id,timeout,grant,permission);
    if (state) { state->tetra_call_active=1; state->tetra_call_id=id; state->tetra_call_timeout=timeout;
        state->tetra_connect_valid=1;
        state->tetra_connect_call_type=basic_valid?(uint8_t)(basic>>5):0;
        state->tetra_connect_enc_mode=basic_valid?(uint8_t)((basic>>4)&1u):0;
        state->tetra_cmce_connect_hook=hook; state->tetra_cmce_connect_duplex=duplex;
        state->tetra_cmce_connect_tx_grant=grant; state->tetra_cmce_connect_tx_permission=permission;
        state->tetra_cmce_connect_ownership=ownership;state->tetra_cmce_connect_priority=priority;
        state->tetra_cmce_connect_priority_valid=priority_valid;state->tetra_cmce_connect_basic_service=basic;
        state->tetra_cmce_connect_basic_service_valid=basic_valid;
        state->tetra_cmce_connect_temporary_address=temporary_address;
        state->tetra_cmce_connect_temporary_address_valid=temporary_address_valid;
        state->tetra_cmce_connect_notification=notification;
        state->tetra_cmce_connect_notification_valid=notification_valid;
        if (basic_valid) {
            state->tetra_call_type=(uint8_t)(basic>>5);
            state->tetra_enc_mode=(uint8_t)((basic>>4)&1u);
            state->tetra_cmce_com_type=(uint8_t)((basic>>2)&3u);
            state->tetra_call_slots=(uint8_t)(basic&3u);
        }
        state->tetra_cmce_call_generation++; }
    return;
truncated:
    fprintf(stderr,"[TETRA CMCE D-CONNECT] CC=%d (truncated optional IE: %d bits)\n",cc,nbits);
}

/* -----------------------------------------------------------------------
 * CMCE D-TX-GRANTED (PDU type 11)  — ETSI EN 300 392-2 §14.7.1.15
 *
 * Mandatory bits (Table 14.18): PDU type (5), call identifier (14),
 * transmission grant (2), request permission (1), encryption control (1),
 * and reserved (1). Optional type-2 elements follow with presence flags.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_tx_granted(const uint8_t *bits, int nbits,
                                     int cc, dsd_opts *opts, dsd_state *state)
{
    if (nbits < 25) {
        fprintf(stderr, "[TETRA CMCE D-TX-GRANTED] CC=%d (too short: %d bits)\n",
                cc, nbits);
        return;
    }

    int off = 5;
    uint32_t call_id   = mle_bits_to_uint(bits, off, 14); off += 14;
    uint32_t tx_grant  = mle_bits_to_uint(bits, off, 2);  off += 2;
    uint32_t tx_perm   = mle_bits_to_uint(bits, off, 1);  off += 1;
    uint32_t enc_mode  = mle_bits_to_uint(bits, off, 1);  off += 1;
    uint32_t reserved  = mle_bits_to_uint(bits, off, 1);  off += 1;

    uint32_t notification = 0, granted_ssi = 0, party_extension = 0;
    uint8_t notification_valid = 0, party_type = 0, party_type_valid = 0;
    uint8_t got_ssi = 0, party_extension_valid = 0;
    uint8_t optional_tail = (uint8_t)mle_bits_to_uint(bits, off, 1); off += 1;

    /* Notification indicator (optional type 2). */
    if (optional_tail) {
        uint32_t present = mle_bits_to_uint(bits, off, 1); off += 1;
        if (present) {
            if (off + 6 > nbits)
                goto truncated;
            notification = mle_bits_to_uint(bits, off, 6); off += 6;
            notification_valid = 1;
        }
    }
    /* Transmitting party type identifier and its conditional SSI. */
    if (optional_tail) {
        if (off >= nbits)
            goto truncated;
        uint32_t present = mle_bits_to_uint(bits, off, 1); off += 1;
        if (present) {
            if (off + 2 > nbits)
                goto truncated;
            party_type = (uint8_t)mle_bits_to_uint(bits, off, 2); off += 2;
            party_type_valid = 1;
            if (party_type == 1 || party_type == 2) {
                if (off + 24 > nbits)
                    goto truncated;
                granted_ssi = mle_bits_to_uint(bits, off, 24); off += 24;
                got_ssi = 1;
            }
            if (party_type == 2) {
                if (off + 24 > nbits)
                    goto truncated;
                party_extension = mle_bits_to_uint(bits, off, 24); off += 24;
                party_extension_valid = 1;
            }
        }
        if (!cmce_skip_type34(bits, nbits, &off))
            goto truncated;
    }

    fprintf(stderr, "[TETRA CMCE D-TX-GRANTED] CC=%d  call=%u  grant=%u  request=%u  enc=%u",
            cc, call_id, tx_grant, tx_perm, enc_mode);
    if (got_ssi)
        fprintf(stderr, "  granted_SSI=%u", granted_ssi);
    if (reserved)
        fprintf(stderr, "  reserved=%u", reserved);
    fprintf(stderr, "\n");

    if (state) {
        if (!cmce_matches_active_call(state, call_id)) {
            fprintf(stderr, "[TETRA CMCE D-TX-GRANTED] CC=%d call=%u ignored; active call=%u\n",
                    cc, call_id, (unsigned)state->tetra_call_id);
            return;
        }
        /* Values 0 and 3 switch on the receive U-plane; 1 and 2 do not. */
        state->tetra_tx_granted_valid = (uint8_t)(tx_grant == 0 || tx_grant == 3);
        state->tetra_enc_mode = (uint8_t)(enc_mode & 0x01u);
        
        /* Phase 78: CMCE D-TX-GRANTED dropped variables */
        state->tetra_cmce_tx_granted_perm   = (uint8_t)(tx_perm & 1u);
        state->tetra_cmce_tx_granted_reserv = (uint8_t)(reserved & 1u);
        state->tetra_cmce_tx_granted_notification = (uint8_t)notification;
        state->tetra_cmce_tx_granted_notification_valid = notification_valid;
        state->tetra_cmce_tx_granted_party_type = party_type;
        state->tetra_cmce_tx_granted_party_type_valid = party_type_valid;
        state->tetra_tx_granted_ssi = got_ssi ? granted_ssi : 0;
        state->tetra_cmce_tx_granted_party_extension = party_extension;
        state->tetra_cmce_tx_granted_party_extension_valid = party_extension_valid;
    }

    (void)opts; /* channel allocation and tuning are MAC-layer responsibilities */
    return;

truncated:
    fprintf(stderr, "[TETRA CMCE D-TX-GRANTED] CC=%d (truncated optional IE: %d bits)\n",
            cc, nbits);
    (void)opts;
}

/* -----------------------------------------------------------------------
 * CMCE D-TX-CEASED (PDU type 9) — ETSI EN 300 392-2 §14.7.1.13.
 * Mandatory fields are PDU type (5), call identifier (14), and transmission
 * request permission (1). Notification indicator is the first optional IE.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_tx_ceased(const uint8_t *bits, int nbits,
                                    int cc, dsd_opts *opts, dsd_state *state)
{
    if (nbits < 21) {
        fprintf(stderr, "[TETRA CMCE D-TX-CEASED] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    int off = 5;
    uint16_t call_id = (uint16_t)mle_bits_to_uint(bits, off, 14); off += 14;
    uint8_t tx_perm = (uint8_t)mle_bits_to_uint(bits, off, 1); off += 1;
    uint8_t notification = 0;
    uint8_t got_notification = 0;
    uint8_t optional_tail = (uint8_t)mle_bits_to_uint(bits, off, 1); off += 1;
    if (optional_tail) {
        uint8_t present = (uint8_t)mle_bits_to_uint(bits, off, 1); off += 1;
        if (present) {
            if (off + 6 > nbits) {
                fprintf(stderr,
                        "[TETRA CMCE D-TX-CEASED] CC=%d (truncated optional IE: %d bits)\n",
                        cc, nbits);
                return;
            }
            notification = (uint8_t)mle_bits_to_uint(bits, off, 6);
            got_notification = 1;
        }
        if (!cmce_skip_type34(bits, nbits, &off)) {
            fprintf(stderr,
                    "[TETRA CMCE D-TX-CEASED] CC=%d (truncated optional IE: %d bits)\n",
                    cc, nbits);
            return;
        }
    }
    fprintf(stderr, "[TETRA CMCE D-TX-CEASED] CC=%d  call=%u  request=%u",
            cc, (unsigned)call_id, (unsigned)tx_perm);
    if (got_notification)
        fprintf(stderr, "  notification=%u", (unsigned)notification);
    fprintf(stderr, "  (floor released)\n");
    if (state) {
        if (!cmce_matches_active_call(state, call_id)) {
            fprintf(stderr, "[TETRA CMCE D-TX-CEASED] CC=%d call=%u ignored; active call=%u\n",
                    cc, (unsigned)call_id, (unsigned)state->tetra_call_id);
            return;
        }
        state->tetra_tx_ceased_tx_perm     = tx_perm;
        state->tetra_tx_ceased_cipher_info = 0; /* retained ABI field; no such IE in Table 14.16 */
        state->tetra_tx_granted_valid      = 0;
    }
    /* Floor release does not release the assigned traffic channel. */
    (void)opts;
}

/* Decode one table 18.64 CA neighbour.  The fixed fields are retained for
 * cell-selection/tracking consumers.  Optional fields are currently skipped,
 * but their complete standardized P-bit/value layout is still required. */
static int parse_mle_ca_neighbor(const uint8_t *bits, int nbits, int *off,
                                 uint8_t *cell_id, uint8_t *reselect_types,
                                 uint8_t *synchronized, uint8_t *load,
                                 uint16_t *main_carrier)
{
    static const uint8_t optional_widths[10] = {
        10, 10, 14, 14, 3, 4, 16, 12, 5, 6
    };
    if (!bits || !off || *off + 23 > nbits)
        return 0;

    *cell_id = (uint8_t)mle_bits_to_uint(bits, *off, 5); *off += 5;
    *reselect_types = (uint8_t)mle_bits_to_uint(bits, *off, 2); *off += 2;
    *synchronized = bits[(*off)++] & 1u;
    *load = (uint8_t)mle_bits_to_uint(bits, *off, 2); *off += 2;
    *main_carrier = (uint16_t)mle_bits_to_uint(bits, *off, 12); *off += 12;

    uint8_t optional = bits[(*off)++] & 1u;
    if (optional) {
        for (unsigned i = 0; i < sizeof(optional_widths); i++) {
            if (*off >= nbits)
                return 0;
            uint8_t present = bits[(*off)++] & 1u;
            if (present) {
                if (*off + optional_widths[i] > nbits)
                    return 0;
                *off += optional_widths[i];
            }
        }
    }
    return 1;
}

/* D-NWRK-BROADCAST, table 18.2. @bits starts at its 3-bit PDU type. */
static void parse_mle_d_nwrk_broadcast(const uint8_t *bits, int nbits,
                                        int cc, dsd_state *state)
{
    if (nbits < 22) { /* type + reselect parameters + load + O-bit */
        fprintf(stderr, "[TETRA MLE D-NWRK-BROADCAST] CC=%d (too short: %d bits)\n",
                cc, nbits);
        return;
    }

    uint32_t reselect = mle_bits_to_uint(bits, 3, 16);
    uint32_t load = mle_bits_to_uint(bits, 19, 2);
    int off = 21;
    uint64_t network_time = 0;
    uint8_t network_time_valid = 0;
    uint8_t neighbor_count = 0;
    uint8_t neighbor_count_valid = 0;
    uint8_t cell_id[7] = {0};
    uint8_t reselect_types[7] = {0};
    uint8_t synchronized[7] = {0};
    uint8_t neighbor_load[7] = {0};
    uint16_t main_carrier[7] = {0};

    uint8_t optional = bits[off++] & 1u;
    if (optional) {
        if (off >= nbits)
            goto truncated;
        uint8_t present = bits[off++] & 1u;
        if (present) {
            if (off + 48 > nbits)
                goto truncated;
            network_time = ((uint64_t)mle_bits_to_uint(bits, off, 16) << 32)
                         | mle_bits_to_uint(bits, off + 16, 32);
            off += 48;
            network_time_valid = 1;
        }
        if (off >= nbits)
            goto truncated;
        present = bits[off++] & 1u;
        if (present) {
            if (off + 3 > nbits)
                goto truncated;
            neighbor_count = (uint8_t)mle_bits_to_uint(bits, off, 3); off += 3;
            neighbor_count_valid = 1;
            for (uint8_t i = 0; i < neighbor_count; i++) {
                if (!parse_mle_ca_neighbor(bits, nbits, &off, &cell_id[i],
                                           &reselect_types[i], &synchronized[i],
                                           &neighbor_load[i], &main_carrier[i]))
                    goto truncated;
            }
        }
    }

    fprintf(stderr, "[TETRA MLE D-NWRK-BROADCAST] CC=%d reselect=0x%04X load=%u\n",
            cc, reselect, load);

    if (state) {
        state->tetra_nwrk_bcast_known = 1;
        state->tetra_mle_cell_reselect_params = (uint16_t)reselect;
        state->tetra_mle_cell_load = (uint8_t)load;
        state->tetra_mle_network_time = network_time;
        state->tetra_mle_network_time_valid = network_time_valid;
        state->tetra_mle_ca_neighbor_count = neighbor_count;
        state->tetra_mle_ca_neighbor_count_valid = neighbor_count_valid;
        memcpy(state->tetra_mle_ca_neighbor_cell_id, cell_id, sizeof(cell_id));
        memcpy(state->tetra_mle_ca_neighbor_reselect_types, reselect_types,
               sizeof(reselect_types));
        memcpy(state->tetra_mle_ca_neighbor_synchronized, synchronized,
               sizeof(synchronized));
        memcpy(state->tetra_mle_ca_neighbor_load, neighbor_load,
               sizeof(neighbor_load));
        memcpy(state->tetra_mle_ca_neighbor_main_carrier, main_carrier,
               sizeof(main_carrier));
    }
    return;

truncated:
    fprintf(stderr, "[TETRA MLE D-NWRK-BROADCAST] CC=%d (truncated optional data: %d bits)\n",
            cc, nbits);
}

/* Table 18.41: a channel class is its four-bit identifier, 18-bit channel
 * characteristics, and five-bit BS power relative to the main carrier. */
static int
parse_mle_channel_class(const uint8_t *bits, int nbits, int *off,
                        uint8_t *id, uint32_t *characteristics, uint8_t *bs_power)
{
    if (!bits || !off || *off + 27 > nbits)
        return 0;
    *id = (uint8_t)mle_bits_to_uint(bits, *off, 4); *off += 4;
    *characteristics = mle_bits_to_uint(bits, *off, 18); *off += 18;
    *bs_power = (uint8_t)mle_bits_to_uint(bits, *off, 5); *off += 5;
    return 1;
}

/* Table 18.96: the ten-bit carrier extension is conditional on its flag. */
static int
parse_mle_irregular_channel(const uint8_t *bits, int nbits, int *off,
                            uint8_t *channel_id, uint32_t *characteristics,
                            uint16_t *carrier, uint16_t *extension,
                            uint8_t *extension_valid)
{
    if (!bits || !off || *off + 36 > nbits)
        return 0;
    *channel_id = (uint8_t)mle_bits_to_uint(bits, *off, 5); *off += 5;
    *characteristics = mle_bits_to_uint(bits, *off, 18); *off += 18;
    *carrier = (uint16_t)mle_bits_to_uint(bits, *off, 12); *off += 12;
    *extension_valid = bits[(*off)++] & 1u;
    *extension = 0;
    if (*extension_valid) {
        if (*off + 10 > nbits)
            return 0;
        *extension = (uint16_t)mle_bits_to_uint(bits, *off, 10); *off += 10;
    }
    return 1;
}

/* D-NWRK-BROADCAST EXTENSION, table 18.4.  Decode into a temporary state so
 * a truncated list or optional header cannot partially replace the last valid
 * network snapshot. */
typedef struct {
    uint8_t  tetra_nwrk_bcast_ext_known;
    uint8_t  tetra_mle_ext_serving_classes_present;
    uint8_t  tetra_mle_ext_serving_class_count;
    uint8_t  tetra_mle_ext_serving_class_id[15];
    uint32_t tetra_mle_ext_serving_class_characteristics[15];
    uint8_t  tetra_mle_ext_serving_class_bs_power[15];
    uint8_t  tetra_mle_ext_neighbor_classes_present;
    uint8_t  tetra_mle_ext_neighbor_class_count;
    uint8_t  tetra_mle_ext_neighbor_class_cell_id[31];
    uint8_t  tetra_mle_ext_neighbor_class_id[31];
    uint32_t tetra_mle_ext_neighbor_class_characteristics[31];
    uint8_t  tetra_mle_ext_neighbor_class_bs_power[31];
    uint8_t  tetra_mle_ext_serving_irregular_present;
    uint8_t  tetra_mle_ext_serving_irregular_count;
    uint8_t  tetra_mle_ext_serving_irregular_channel_id[31];
    uint32_t tetra_mle_ext_serving_irregular_characteristics[31];
    uint16_t tetra_mle_ext_serving_irregular_carrier[31];
    uint16_t tetra_mle_ext_serving_irregular_carrier_extension[31];
    uint8_t  tetra_mle_ext_serving_irregular_extension_valid[31];
    uint8_t  tetra_mle_ext_neighbor_irregular_present;
    uint8_t  tetra_mle_ext_neighbor_irregular_count;
    uint8_t  tetra_mle_ext_neighbor_irregular_cell_id[63];
    uint8_t  tetra_mle_ext_neighbor_irregular_channel_id[63];
    uint32_t tetra_mle_ext_neighbor_irregular_characteristics[63];
    uint16_t tetra_mle_ext_neighbor_irregular_carrier[63];
    uint16_t tetra_mle_ext_neighbor_irregular_carrier_extension[63];
    uint8_t  tetra_mle_ext_neighbor_irregular_extension_valid[63];
} tetra_mle_nwrk_bcast_ext_snapshot;

static void
parse_mle_d_nwrk_broadcast_ext(const uint8_t *bits, int nbits, int cc,
                               dsd_state *state)
{
    tetra_mle_nwrk_bcast_ext_snapshot decoded;
    int off = 3;

    if (nbits < 4)
        goto truncated;
    memset(&decoded, 0, sizeof(decoded));
    decoded.tetra_nwrk_bcast_ext_known = 1;

    if (!(bits[off++] & 1u))
        goto commit;

    /* Number of channel classes for the serving cell + repeated table 18.41. */
    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 4 > nbits) goto truncated;
        decoded.tetra_mle_ext_serving_classes_present = 1;
        decoded.tetra_mle_ext_serving_class_count =
            (uint8_t)mle_bits_to_uint(bits, off, 4); off += 4;
        for (uint8_t i = 0; i < decoded.tetra_mle_ext_serving_class_count; i++)
            if (!parse_mle_channel_class(bits, nbits, &off,
                    &decoded.tetra_mle_ext_serving_class_id[i],
                    &decoded.tetra_mle_ext_serving_class_characteristics[i],
                    &decoded.tetra_mle_ext_serving_class_bs_power[i]))
                goto truncated;
    }

    /* CA neighbour cell identifier + channel class, table 18.62. */
    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 5 > nbits) goto truncated;
        decoded.tetra_mle_ext_neighbor_classes_present = 1;
        decoded.tetra_mle_ext_neighbor_class_count =
            (uint8_t)mle_bits_to_uint(bits, off, 5); off += 5;
        for (uint8_t i = 0; i < decoded.tetra_mle_ext_neighbor_class_count; i++) {
            if (off + 5 > nbits) goto truncated;
            decoded.tetra_mle_ext_neighbor_class_cell_id[i] =
                (uint8_t)mle_bits_to_uint(bits, off, 5); off += 5;
            if (!parse_mle_channel_class(bits, nbits, &off,
                    &decoded.tetra_mle_ext_neighbor_class_id[i],
                    &decoded.tetra_mle_ext_neighbor_class_characteristics[i],
                    &decoded.tetra_mle_ext_neighbor_class_bs_power[i]))
                goto truncated;
        }
    }

    /* Irregular channel details for the serving cell, table 18.96. */
    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 5 > nbits) goto truncated;
        decoded.tetra_mle_ext_serving_irregular_present = 1;
        decoded.tetra_mle_ext_serving_irregular_count =
            (uint8_t)mle_bits_to_uint(bits, off, 5); off += 5;
        for (uint8_t i = 0; i < decoded.tetra_mle_ext_serving_irregular_count; i++)
            if (!parse_mle_irregular_channel(bits, nbits, &off,
                    &decoded.tetra_mle_ext_serving_irregular_channel_id[i],
                    &decoded.tetra_mle_ext_serving_irregular_characteristics[i],
                    &decoded.tetra_mle_ext_serving_irregular_carrier[i],
                    &decoded.tetra_mle_ext_serving_irregular_carrier_extension[i],
                    &decoded.tetra_mle_ext_serving_irregular_extension_valid[i]))
                goto truncated;
    }

    /* CA neighbour cell identifier + irregular detail, table 18.66. */
    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 6 > nbits) goto truncated;
        decoded.tetra_mle_ext_neighbor_irregular_present = 1;
        decoded.tetra_mle_ext_neighbor_irregular_count =
            (uint8_t)mle_bits_to_uint(bits, off, 6); off += 6;
        for (uint8_t i = 0; i < decoded.tetra_mle_ext_neighbor_irregular_count; i++) {
            if (off + 5 > nbits) goto truncated;
            decoded.tetra_mle_ext_neighbor_irregular_cell_id[i] =
                (uint8_t)mle_bits_to_uint(bits, off, 5); off += 5;
            if (!parse_mle_irregular_channel(bits, nbits, &off,
                    &decoded.tetra_mle_ext_neighbor_irregular_channel_id[i],
                    &decoded.tetra_mle_ext_neighbor_irregular_characteristics[i],
                    &decoded.tetra_mle_ext_neighbor_irregular_carrier[i],
                    &decoded.tetra_mle_ext_neighbor_irregular_carrier_extension[i],
                    &decoded.tetra_mle_ext_neighbor_irregular_extension_valid[i]))
                goto truncated;
        }
    }

    /* The two reserved Type-2 elements shall not be used (table 18.4). */
    if (off + 2 > nbits || (bits[off] & 1u) || (bits[off + 1] & 1u))
        goto truncated;
    off += 2;

commit:
    fprintf(stderr,
            "[TETRA MLE D-NWRK-BROADCAST-EXT] CC=%d classes=%u/%u irregular=%u/%u\n",
            cc, decoded.tetra_mle_ext_serving_class_count,
            decoded.tetra_mle_ext_neighbor_class_count,
            decoded.tetra_mle_ext_serving_irregular_count,
            decoded.tetra_mle_ext_neighbor_irregular_count);
    if (state) {
#define COPY_EXT_FIELD(name) state->name = decoded.name
        COPY_EXT_FIELD(tetra_nwrk_bcast_ext_known);
        COPY_EXT_FIELD(tetra_mle_ext_serving_classes_present);
        COPY_EXT_FIELD(tetra_mle_ext_serving_class_count);
        COPY_EXT_FIELD(tetra_mle_ext_neighbor_classes_present);
        COPY_EXT_FIELD(tetra_mle_ext_neighbor_class_count);
        COPY_EXT_FIELD(tetra_mle_ext_serving_irregular_present);
        COPY_EXT_FIELD(tetra_mle_ext_serving_irregular_count);
        COPY_EXT_FIELD(tetra_mle_ext_neighbor_irregular_present);
        COPY_EXT_FIELD(tetra_mle_ext_neighbor_irregular_count);
#undef COPY_EXT_FIELD
#define COPY_EXT_ARRAY(name) memcpy(state->name, decoded.name, sizeof(state->name))
        COPY_EXT_ARRAY(tetra_mle_ext_serving_class_id);
        COPY_EXT_ARRAY(tetra_mle_ext_serving_class_characteristics);
        COPY_EXT_ARRAY(tetra_mle_ext_serving_class_bs_power);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_class_cell_id);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_class_id);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_class_characteristics);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_class_bs_power);
        COPY_EXT_ARRAY(tetra_mle_ext_serving_irregular_channel_id);
        COPY_EXT_ARRAY(tetra_mle_ext_serving_irregular_characteristics);
        COPY_EXT_ARRAY(tetra_mle_ext_serving_irregular_carrier);
        COPY_EXT_ARRAY(tetra_mle_ext_serving_irregular_carrier_extension);
        COPY_EXT_ARRAY(tetra_mle_ext_serving_irregular_extension_valid);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_irregular_cell_id);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_irregular_channel_id);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_irregular_characteristics);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_irregular_carrier);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_irregular_carrier_extension);
        COPY_EXT_ARRAY(tetra_mle_ext_neighbor_irregular_extension_valid);
#undef COPY_EXT_ARRAY
    }
    return;

truncated:
    fprintf(stderr,
            "[TETRA MLE D-NWRK-BROADCAST-EXT] CC=%d (invalid/truncated: %d bits)\n",
            cc, nbits);
}

/* Table 18.65 DA neighbour information.  The first eleven optional fields
 * are retained and the three reserved fields are required to be absent. */
static int
parse_mle_da_neighbor(const uint8_t *bits, int nbits, int *off,
                      dsd_state *decoded, uint8_t index)
{
    static const uint8_t widths[11] = {10, 10, 14, 14, 8, 3, 4, 16, 7, 3, 6};
    if (!bits || !off || !decoded || index >= 7 || *off + 21 > nbits)
        return 0;

    decoded->tetra_mle_da_neighbor_cell_id[index] =
        (uint8_t)mle_bits_to_uint(bits, *off, 8); *off += 8;
    decoded->tetra_mle_da_neighbor_reselect_types[index] =
        (uint8_t)mle_bits_to_uint(bits, *off, 8); *off += 8;
    decoded->tetra_mle_da_neighbor_synchronized[index] = bits[(*off)++] & 1u;
    decoded->tetra_mle_da_neighbor_wide_area[index] = bits[(*off)++] & 1u;
    decoded->tetra_mle_da_neighbor_ci_path[index] =
        (uint8_t)mle_bits_to_uint(bits, *off, 2); *off += 2;
    decoded->tetra_mle_da_neighbor_load_known[index] = bits[(*off)++] & 1u;
    if (decoded->tetra_mle_da_neighbor_load_known[index]) {
        if (*off + 3 > nbits) return 0;
        decoded->tetra_mle_da_neighbor_load[index] =
            (uint8_t)mle_bits_to_uint(bits, *off, 3); *off += 3;
    }
    if (*off + 18 > nbits) return 0;
    decoded->tetra_mle_da_neighbor_main_carrier[index] =
        (uint16_t)mle_bits_to_uint(bits, *off, 12); *off += 12;
    decoded->tetra_mle_da_neighbor_modulation[index] =
        (uint8_t)mle_bits_to_uint(bits, *off, 3); *off += 3;
    decoded->tetra_mle_da_neighbor_bandwidth[index] =
        (uint8_t)mle_bits_to_uint(bits, *off, 3); *off += 3;

    if (*off >= nbits) return 0;
    if (bits[(*off)++] & 1u) {
        for (unsigned i = 0; i < 11; i++) {
            uint32_t value;
            if (*off >= nbits) return 0;
            if (!(bits[(*off)++] & 1u)) continue;
            if (*off + widths[i] > nbits) return 0;
            value = mle_bits_to_uint(bits, *off, widths[i]); *off += widths[i];
            decoded->tetra_mle_da_neighbor_optional_mask[index] |= (uint16_t)(1u << i);
            switch (i) {
            case 0: decoded->tetra_mle_da_neighbor_carrier_extension[index] = (uint16_t)value; break;
            case 1: decoded->tetra_mle_da_neighbor_mcc[index] = (uint16_t)value; break;
            case 2: decoded->tetra_mle_da_neighbor_mnc[index] = (uint16_t)value; break;
            case 3: decoded->tetra_mle_da_neighbor_la[index] = (uint16_t)value; break;
            case 4: decoded->tetra_mle_da_neighbor_local_cell_id[index] = (uint8_t)value; break;
            case 5: decoded->tetra_mle_da_neighbor_max_tx_power[index] = (uint8_t)value; break;
            case 6: decoded->tetra_mle_da_neighbor_min_rx_level[index] = (uint8_t)value; break;
            case 7: decoded->tetra_mle_da_neighbor_subscriber_class[index] = (uint16_t)value; break;
            case 8: decoded->tetra_mle_da_neighbor_bs_service_details[index] = (uint8_t)value; break;
            case 9: decoded->tetra_mle_da_neighbor_security[index] = (uint8_t)value; break;
            case 10: decoded->tetra_mle_da_neighbor_frame_offset[index] = (uint8_t)value; break;
            }
        }
        if (*off + 3 > nbits || bits[*off] || bits[*off + 1] || bits[*off + 2])
            return 0;
        *off += 3;
    }
    return 1;
}

/* Extended-PDU D-NWRK-BROADCAST-DA, table 18.3. */
static void
parse_mle_d_nwrk_broadcast_da(const uint8_t *bits, int nbits, int cc,
                              dsd_state *state)
{
    dsd_state *decoded = (dsd_state *)calloc(1, sizeof(*decoded));
    int off = 10; /* base type, extension, mandatory three reserved zero bits */
    if (!decoded) return;
    decoded->tetra_mle_da_broadcast_known = 1;
    if (nbits < 11 || mle_bits_to_uint(bits, 7, 3) != 0)
        goto truncated;
    if (!(bits[off++] & 1u))
        goto commit;

    /* Reserved 14-bit Type-2 field. */
    if (off >= nbits || bits[off++]) goto truncated;

#define DA_OUTER_VALUE(width, present_field, value_field) do { \
        if (off >= nbits) goto truncated; \
        if (bits[off++] & 1u) { \
            if (off + (width) > nbits) goto truncated; \
            decoded->present_field = 1; \
            decoded->value_field = mle_bits_to_uint(bits, off, (width)); \
            off += (width); \
        } \
    } while (0)
    DA_OUTER_VALUE(8, tetra_mle_da_cell_id_valid, tetra_mle_da_cell_id);
    DA_OUTER_VALUE(16, tetra_mle_da_reselect_valid, tetra_mle_da_reselect);
    DA_OUTER_VALUE(3, tetra_mle_da_load_valid, tetra_mle_da_load);
#undef DA_OUTER_VALUE

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 48 > nbits) goto truncated;
        decoded->tetra_mle_da_network_time =
            ((uint64_t)mle_bits_to_uint(bits, off, 16) << 32)
            | mle_bits_to_uint(bits, off + 16, 32);
        decoded->tetra_mle_da_network_time_valid = 1;
        off += 48;
    }

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        decoded->tetra_mle_da_local_ca_valid = 1;
        if (!parse_mle_ca_neighbor(bits, nbits, &off,
                &decoded->tetra_mle_da_local_ca_cell_id,
                &decoded->tetra_mle_da_local_ca_reselect_types,
                &decoded->tetra_mle_da_local_ca_synchronized,
                &decoded->tetra_mle_da_local_ca_load,
                &decoded->tetra_mle_da_local_ca_main_carrier))
            goto truncated;
    }

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 3 > nbits) goto truncated;
        decoded->tetra_mle_da_neighbor_list_present = 1;
        decoded->tetra_mle_da_neighbor_count =
            (uint8_t)mle_bits_to_uint(bits, off, 3); off += 3;
        for (uint8_t i = 0; i < decoded->tetra_mle_da_neighbor_count; i++)
            if (!parse_mle_da_neighbor(bits, nbits, &off, decoded, i))
                goto truncated;
    }

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 4 > nbits) goto truncated;
        decoded->tetra_mle_da_serving_classes_present = 1;
        decoded->tetra_mle_da_serving_class_count =
            (uint8_t)mle_bits_to_uint(bits, off, 4); off += 4;
        for (uint8_t i = 0; i < decoded->tetra_mle_da_serving_class_count; i++)
            if (!parse_mle_channel_class(bits, nbits, &off,
                    &decoded->tetra_mle_da_serving_class_id[i],
                    &decoded->tetra_mle_da_serving_class_characteristics[i],
                    &decoded->tetra_mle_da_serving_class_bs_power[i]))
                goto truncated;
    }

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 5 > nbits) goto truncated;
        decoded->tetra_mle_da_neighbor_classes_present = 1;
        decoded->tetra_mle_da_neighbor_class_count =
            (uint8_t)mle_bits_to_uint(bits, off, 5); off += 5;
        for (uint8_t i = 0; i < decoded->tetra_mle_da_neighbor_class_count; i++) {
            if (off + 8 > nbits) goto truncated;
            decoded->tetra_mle_da_neighbor_class_cell_id[i] =
                (uint8_t)mle_bits_to_uint(bits, off, 8); off += 8;
            if (!parse_mle_channel_class(bits, nbits, &off,
                    &decoded->tetra_mle_da_neighbor_class_id[i],
                    &decoded->tetra_mle_da_neighbor_class_characteristics[i],
                    &decoded->tetra_mle_da_neighbor_class_bs_power[i]))
                goto truncated;
        }
    }

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 5 > nbits) goto truncated;
        decoded->tetra_mle_da_serving_irregular_present = 1;
        decoded->tetra_mle_da_serving_irregular_count =
            (uint8_t)mle_bits_to_uint(bits, off, 5); off += 5;
        for (uint8_t i = 0; i < decoded->tetra_mle_da_serving_irregular_count; i++)
            if (!parse_mle_irregular_channel(bits, nbits, &off,
                    &decoded->tetra_mle_da_serving_irregular_channel_id[i],
                    &decoded->tetra_mle_da_serving_irregular_characteristics[i],
                    &decoded->tetra_mle_da_serving_irregular_carrier[i],
                    &decoded->tetra_mle_da_serving_irregular_extension[i],
                    &decoded->tetra_mle_da_serving_irregular_extension_valid[i]))
                goto truncated;
    }

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 6 > nbits) goto truncated;
        decoded->tetra_mle_da_neighbor_irregular_present = 1;
        decoded->tetra_mle_da_neighbor_irregular_count =
            (uint8_t)mle_bits_to_uint(bits, off, 6); off += 6;
        for (uint8_t i = 0; i < decoded->tetra_mle_da_neighbor_irregular_count; i++) {
            if (off + 8 > nbits) goto truncated;
            decoded->tetra_mle_da_neighbor_irregular_cell_id[i] =
                (uint8_t)mle_bits_to_uint(bits, off, 8); off += 8;
            if (!parse_mle_irregular_channel(bits, nbits, &off,
                    &decoded->tetra_mle_da_neighbor_irregular_channel_id[i],
                    &decoded->tetra_mle_da_neighbor_irregular_characteristics[i],
                    &decoded->tetra_mle_da_neighbor_irregular_carrier[i],
                    &decoded->tetra_mle_da_neighbor_irregular_extension[i],
                    &decoded->tetra_mle_da_neighbor_irregular_extension_valid[i]))
                goto truncated;
        }
    }

    /* Two final reserved Type-2 fields. */
    if (off + 2 > nbits || bits[off] || bits[off + 1]) goto truncated;
    off += 2;

commit:
    fprintf(stderr,
            "[TETRA MLE D-NWRK-BROADCAST-DA] CC=%d neighbours=%u classes=%u/%u irregular=%u/%u\n",
            cc, decoded->tetra_mle_da_neighbor_count,
            decoded->tetra_mle_da_serving_class_count,
            decoded->tetra_mle_da_neighbor_class_count,
            decoded->tetra_mle_da_serving_irregular_count,
            decoded->tetra_mle_da_neighbor_irregular_count);
    if (state) {
#define COPY_DA_SCALAR(name) state->name = decoded->name
        COPY_DA_SCALAR(tetra_mle_da_broadcast_known);
        COPY_DA_SCALAR(tetra_mle_da_cell_id_valid); COPY_DA_SCALAR(tetra_mle_da_cell_id);
        COPY_DA_SCALAR(tetra_mle_da_reselect_valid); COPY_DA_SCALAR(tetra_mle_da_reselect);
        COPY_DA_SCALAR(tetra_mle_da_load_valid); COPY_DA_SCALAR(tetra_mle_da_load);
        COPY_DA_SCALAR(tetra_mle_da_network_time_valid); COPY_DA_SCALAR(tetra_mle_da_network_time);
        COPY_DA_SCALAR(tetra_mle_da_local_ca_valid); COPY_DA_SCALAR(tetra_mle_da_local_ca_cell_id);
        COPY_DA_SCALAR(tetra_mle_da_local_ca_reselect_types);
        COPY_DA_SCALAR(tetra_mle_da_local_ca_synchronized); COPY_DA_SCALAR(tetra_mle_da_local_ca_load);
        COPY_DA_SCALAR(tetra_mle_da_local_ca_main_carrier);
        COPY_DA_SCALAR(tetra_mle_da_neighbor_list_present); COPY_DA_SCALAR(tetra_mle_da_neighbor_count);
        COPY_DA_SCALAR(tetra_mle_da_serving_classes_present); COPY_DA_SCALAR(tetra_mle_da_serving_class_count);
        COPY_DA_SCALAR(tetra_mle_da_neighbor_classes_present); COPY_DA_SCALAR(tetra_mle_da_neighbor_class_count);
        COPY_DA_SCALAR(tetra_mle_da_serving_irregular_present); COPY_DA_SCALAR(tetra_mle_da_serving_irregular_count);
        COPY_DA_SCALAR(tetra_mle_da_neighbor_irregular_present); COPY_DA_SCALAR(tetra_mle_da_neighbor_irregular_count);
#undef COPY_DA_SCALAR
#define COPY_DA_ARRAY(name) memcpy(state->name, decoded->name, sizeof(state->name))
        COPY_DA_ARRAY(tetra_mle_da_neighbor_cell_id); COPY_DA_ARRAY(tetra_mle_da_neighbor_reselect_types);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_synchronized); COPY_DA_ARRAY(tetra_mle_da_neighbor_wide_area);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_ci_path); COPY_DA_ARRAY(tetra_mle_da_neighbor_load_known);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_load); COPY_DA_ARRAY(tetra_mle_da_neighbor_main_carrier);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_modulation); COPY_DA_ARRAY(tetra_mle_da_neighbor_bandwidth);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_optional_mask); COPY_DA_ARRAY(tetra_mle_da_neighbor_carrier_extension);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_mcc); COPY_DA_ARRAY(tetra_mle_da_neighbor_mnc);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_la); COPY_DA_ARRAY(tetra_mle_da_neighbor_local_cell_id);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_max_tx_power); COPY_DA_ARRAY(tetra_mle_da_neighbor_min_rx_level);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_subscriber_class); COPY_DA_ARRAY(tetra_mle_da_neighbor_bs_service_details);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_security); COPY_DA_ARRAY(tetra_mle_da_neighbor_frame_offset);
        COPY_DA_ARRAY(tetra_mle_da_serving_class_id); COPY_DA_ARRAY(tetra_mle_da_serving_class_characteristics);
        COPY_DA_ARRAY(tetra_mle_da_serving_class_bs_power); COPY_DA_ARRAY(tetra_mle_da_neighbor_class_cell_id);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_class_id); COPY_DA_ARRAY(tetra_mle_da_neighbor_class_characteristics);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_class_bs_power); COPY_DA_ARRAY(tetra_mle_da_serving_irregular_channel_id);
        COPY_DA_ARRAY(tetra_mle_da_serving_irregular_characteristics); COPY_DA_ARRAY(tetra_mle_da_serving_irregular_carrier);
        COPY_DA_ARRAY(tetra_mle_da_serving_irregular_extension); COPY_DA_ARRAY(tetra_mle_da_serving_irregular_extension_valid);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_irregular_cell_id); COPY_DA_ARRAY(tetra_mle_da_neighbor_irregular_channel_id);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_irregular_characteristics); COPY_DA_ARRAY(tetra_mle_da_neighbor_irregular_carrier);
        COPY_DA_ARRAY(tetra_mle_da_neighbor_irregular_extension); COPY_DA_ARRAY(tetra_mle_da_neighbor_irregular_extension_valid);
#undef COPY_DA_ARRAY
    }
    free(decoded);
    return;

truncated:
    fprintf(stderr,
            "[TETRA MLE D-NWRK-BROADCAST-DA] CC=%d (invalid/truncated: %d bits)\n",
            cc, nbits);
    free(decoded);
}

typedef struct {
    uint8_t cell_id;
    uint8_t remove_cell;
    uint8_t class_count;
    uint8_t class_id[15];
    uint8_t channel_count;
    uint8_t channel_id[31];
} tetra_mle_cell_removal;

/* Tables 18.90/18.91.  The cell-id width distinguishes CA from DA records. */
static int
parse_mle_cell_removal(const uint8_t *bits, int nbits, int *off,
                       int cell_id_bits, tetra_mle_cell_removal *out)
{
    if (!bits || !off || !out || *off + cell_id_bits + 1 > nbits)
        return 0;
    memset(out, 0, sizeof(*out));
    out->cell_id = (uint8_t)mle_bits_to_uint(bits, *off, cell_id_bits);
    *off += cell_id_bits;
    out->remove_cell = bits[(*off)++] & 1u;
    if (out->remove_cell)
        return 1;

    if (*off + 4 > nbits)
        return 0;
    out->class_count = (uint8_t)mle_bits_to_uint(bits, *off, 4); *off += 4;
    for (uint8_t i = 0; i < out->class_count; i++) {
        if (*off + 4 > nbits)
            return 0;
        out->class_id[i] = (uint8_t)mle_bits_to_uint(bits, *off, 4); *off += 4;
    }
    if (*off + 5 > nbits)
        return 0;
    out->channel_count = (uint8_t)mle_bits_to_uint(bits, *off, 5); *off += 5;
    for (uint8_t i = 0; i < out->channel_count; i++) {
        if (*off + 5 > nbits)
            return 0;
        out->channel_id[i] = (uint8_t)mle_bits_to_uint(bits, *off, 5); *off += 5;
    }
    return 1;
}

/* D-NWRK-BROADCAST REMOVE, table 18.5. @bits starts at the base PDU type. */
static void
parse_mle_d_nwrk_broadcast_remove(const uint8_t *bits, int nbits, int cc,
                                  dsd_state *state)
{
    tetra_mle_cell_removal ca[31] = {{0}};
    tetra_mle_cell_removal da[255] = {{0}};
    uint8_t ca_present = 0, ca_count = 0;
    uint8_t da_present = 0, da_count = 0;
    uint8_t serving_present = 0, serving_class_count = 0;
    uint8_t serving_class_id[15] = {0};
    uint8_t serving_channel_count = 0, serving_channel_id[31] = {0};
    int off = 7; /* base type + type extension */

    if (nbits < 8)
        goto truncated;
    if (!(bits[off++] & 1u))
        goto commit;

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 5 > nbits) goto truncated;
        ca_present = 1;
        ca_count = (uint8_t)mle_bits_to_uint(bits, off, 5); off += 5;
        for (uint8_t i = 0; i < ca_count; i++)
            if (!parse_mle_cell_removal(bits, nbits, &off, 5, &ca[i]))
                goto truncated;
    }

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        if (off + 8 > nbits) goto truncated;
        da_present = 1;
        da_count = (uint8_t)mle_bits_to_uint(bits, off, 8); off += 8;
        for (unsigned i = 0; i < da_count; i++)
            if (!parse_mle_cell_removal(bits, nbits, &off, 8, &da[i]))
                goto truncated;
    }

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        serving_present = 1;
        if (off + 4 > nbits) goto truncated;
        serving_class_count = (uint8_t)mle_bits_to_uint(bits, off, 4); off += 4;
        for (uint8_t i = 0; i < serving_class_count; i++) {
            if (off + 4 > nbits) goto truncated;
            serving_class_id[i] = (uint8_t)mle_bits_to_uint(bits, off, 4); off += 4;
        }
        if (off + 5 > nbits) goto truncated;
        serving_channel_count = (uint8_t)mle_bits_to_uint(bits, off, 5); off += 5;
        for (uint8_t i = 0; i < serving_channel_count; i++) {
            if (off + 5 > nbits) goto truncated;
            serving_channel_id[i] = (uint8_t)mle_bits_to_uint(bits, off, 5); off += 5;
        }
    }

    /* Four remaining Type-2 entries are reserved and shall not be used. */
    if (off + 4 > nbits || bits[off] || bits[off + 1] || bits[off + 2] || bits[off + 3])
        goto truncated;
    off += 4;

commit:
    fprintf(stderr,
            "[TETRA MLE D-NWRK-BROADCAST-REMOVE] CC=%d CA=%u DA=%u serving=%u\n",
            cc, ca_count, da_count, serving_present);
    if (state) {
        state->tetra_mle_remove_known = 1;
        state->tetra_mle_remove_ca_present = ca_present;
        state->tetra_mle_remove_ca_count = ca_count;
        state->tetra_mle_remove_da_present = da_present;
        state->tetra_mle_remove_da_count = da_count;
        state->tetra_mle_remove_serving_present = serving_present;
        state->tetra_mle_remove_serving_class_count = serving_class_count;
        state->tetra_mle_remove_serving_channel_count = serving_channel_count;
        memset(state->tetra_mle_remove_ca_cell_id, 0, sizeof(state->tetra_mle_remove_ca_cell_id));
        memset(state->tetra_mle_remove_ca_remove_cell, 0, sizeof(state->tetra_mle_remove_ca_remove_cell));
        memset(state->tetra_mle_remove_ca_class_count, 0, sizeof(state->tetra_mle_remove_ca_class_count));
        memset(state->tetra_mle_remove_ca_class_id, 0, sizeof(state->tetra_mle_remove_ca_class_id));
        memset(state->tetra_mle_remove_ca_channel_count, 0, sizeof(state->tetra_mle_remove_ca_channel_count));
        memset(state->tetra_mle_remove_ca_channel_id, 0, sizeof(state->tetra_mle_remove_ca_channel_id));
        memset(state->tetra_mle_remove_da_cell_id, 0, sizeof(state->tetra_mle_remove_da_cell_id));
        memset(state->tetra_mle_remove_da_remove_cell, 0, sizeof(state->tetra_mle_remove_da_remove_cell));
        memset(state->tetra_mle_remove_da_class_count, 0, sizeof(state->tetra_mle_remove_da_class_count));
        memset(state->tetra_mle_remove_da_class_id, 0, sizeof(state->tetra_mle_remove_da_class_id));
        memset(state->tetra_mle_remove_da_channel_count, 0, sizeof(state->tetra_mle_remove_da_channel_count));
        memset(state->tetra_mle_remove_da_channel_id, 0, sizeof(state->tetra_mle_remove_da_channel_id));
        memset(state->tetra_mle_remove_serving_class_id, 0,
               sizeof(state->tetra_mle_remove_serving_class_id));
        memset(state->tetra_mle_remove_serving_channel_id, 0,
               sizeof(state->tetra_mle_remove_serving_channel_id));
        for (uint8_t i = 0; i < ca_count; i++) {
            state->tetra_mle_remove_ca_cell_id[i] = ca[i].cell_id;
            state->tetra_mle_remove_ca_remove_cell[i] = ca[i].remove_cell;
            state->tetra_mle_remove_ca_class_count[i] = ca[i].class_count;
            memcpy(state->tetra_mle_remove_ca_class_id[i], ca[i].class_id, sizeof(ca[i].class_id));
            state->tetra_mle_remove_ca_channel_count[i] = ca[i].channel_count;
            memcpy(state->tetra_mle_remove_ca_channel_id[i], ca[i].channel_id, sizeof(ca[i].channel_id));
        }
        for (unsigned i = 0; i < da_count; i++) {
            state->tetra_mle_remove_da_cell_id[i] = da[i].cell_id;
            state->tetra_mle_remove_da_remove_cell[i] = da[i].remove_cell;
            state->tetra_mle_remove_da_class_count[i] = da[i].class_count;
            memcpy(state->tetra_mle_remove_da_class_id[i], da[i].class_id, sizeof(da[i].class_id));
            state->tetra_mle_remove_da_channel_count[i] = da[i].channel_count;
            memcpy(state->tetra_mle_remove_da_channel_id[i], da[i].channel_id, sizeof(da[i].channel_id));
        }
        memcpy(state->tetra_mle_remove_serving_class_id, serving_class_id,
               sizeof(serving_class_id));
        memcpy(state->tetra_mle_remove_serving_channel_id, serving_channel_id,
               sizeof(serving_channel_id));
    }
    return;

truncated:
    fprintf(stderr,
            "[TETRA MLE D-NWRK-BROADCAST-REMOVE] CC=%d (invalid/truncated: %d bits)\n",
            cc, nbits);
}

/* -----------------------------------------------------------------------
 * CMCE D-ALERT and D-CALL-PROCEEDING, tables 14.4 and 14.5.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_alert(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if (nbits<26) { fprintf(stderr,"[TETRA CMCE D-ALERT] CC=%d (too short: %d bits)\n",cc,nbits); return; }
    int off=5; uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t timeout=(uint8_t)mle_bits_to_uint(bits,off,3); off+=3;
    uint8_t reserved=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t duplex=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t queued=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t basic=0, notification=0, basic_valid=0;
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if (optional_tail) {
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if (off+8>nbits) goto truncated; basic=(uint8_t)mle_bits_to_uint(bits,off,8); off+=8; basic_valid=1; }
        if (off>=nbits) goto truncated;
        present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if (off+6>nbits) goto truncated; notification=(uint8_t)mle_bits_to_uint(bits,off,6); off+=6; }
        if (!cmce_skip_type34(bits,nbits,&off)) goto truncated;
    }
    fprintf(stderr,"[TETRA CMCE D-ALERT] CC=%d call=%u setup_timeout=%u queued=%u\n",cc,(unsigned)id,timeout,queued);
    if(state){state->tetra_call_active=1; state->tetra_call_id=id; state->tetra_d_alert_call_id=id;
      state->tetra_d_alert_timeout=timeout; state->tetra_d_alert_reserved=reserved; state->tetra_d_alert_duplex=duplex;
      state->tetra_d_alert_queued=queued; state->tetra_d_alert_basic_service=basic;
      state->tetra_d_alert_notification=notification; state->tetra_d_alert_valid=1;
      if (basic_valid) {
          state->tetra_call_type=(uint8_t)(basic>>5);
          state->tetra_enc_mode=(uint8_t)((basic>>4)&1u);
          state->tetra_cmce_com_type=(uint8_t)((basic>>2)&3u);
          state->tetra_call_slots=(uint8_t)(basic&3u);
      }
      state->tetra_cmce_call_generation++;}
    return;
truncated:
    fprintf(stderr,"[TETRA CMCE D-ALERT] CC=%d (truncated optional IE: %d bits)\n",cc,nbits);
}
static void parse_cmce_d_call_proceeding(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<25){fprintf(stderr,"[TETRA CMCE D-CALL-PROCEEDING] CC=%d (too short: %d bits)\n",cc,nbits);return;}
    int off=5; uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t timeout=(uint8_t)mle_bits_to_uint(bits,off,3); off+=3;
    uint8_t hook=(uint8_t)mle_bits_to_uint(bits,off++,1); uint8_t duplex=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t basic=0, status=0, notification=0, basic_valid=0;
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if (optional_tail) {
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if (off+8>nbits) goto truncated; basic=(uint8_t)mle_bits_to_uint(bits,off,8); off+=8; basic_valid=1; }
        if (off>=nbits) goto truncated;
        present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if (off+3>nbits) goto truncated; status=(uint8_t)mle_bits_to_uint(bits,off,3); off+=3; }
        if (off>=nbits) goto truncated;
        present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if (off+6>nbits) goto truncated; notification=(uint8_t)mle_bits_to_uint(bits,off,6); off+=6; }
        if (!cmce_skip_type34(bits,nbits,&off)) goto truncated;
    }
    fprintf(stderr,"[TETRA CMCE D-CALL-PROCEEDING] CC=%d call=%u setup_timeout=%u\n",cc,(unsigned)id,timeout);
    if(state){state->tetra_call_active=1; state->tetra_call_id=id; state->tetra_d_call_proc_call_id=id;
      state->tetra_d_call_proc_timeout=timeout; state->tetra_d_call_proc_hook=hook;
      state->tetra_d_call_proc_duplex=duplex; state->tetra_d_call_proc_basic_service=basic;
      state->tetra_d_call_proc_status=status; state->tetra_d_call_proc_notification=notification;
      state->tetra_d_call_proc_valid=1;
      if (basic_valid) {
          state->tetra_call_type=(uint8_t)(basic>>5);
          state->tetra_enc_mode=(uint8_t)((basic>>4)&1u);
          state->tetra_cmce_com_type=(uint8_t)((basic>>2)&3u);
          state->tetra_call_slots=(uint8_t)(basic&3u);
      }
      state->tetra_cmce_call_generation++;}
    return;
truncated:
    fprintf(stderr,"[TETRA CMCE D-CALL-PROCEEDING] CC=%d (truncated optional IE: %d bits)\n",cc,nbits);
}

/* -----------------------------------------------------------------------
 * CMCE D-STATUS, ETSI EN 300 392-2 table 14.14.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_status(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    int off=5; uint32_t src_ssi=0; uint8_t cpti;
    if (nbits < 23) { fprintf(stderr,"[TETRA CMCE D-STATUS] CC=%d (too short: %d bits)\n",cc,nbits); return; }
    cpti=(uint8_t)mle_bits_to_uint(bits,off,2); off+=2;
    if (cpti==1 || cpti==2) { if (off+24>nbits) return; src_ssi=mle_bits_to_uint(bits,off,24); off+=24; }
    if (cpti==2) { if (off+24>nbits) return; off+=24; }
    if (off+17>nbits) return;
    uint16_t status=(uint16_t)mle_bits_to_uint(bits,off,16); off+=16;
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if (optional_tail && !cmce_skip_type34(bits,nbits,&off)) return;
    fprintf(stderr,"[TETRA CMCE D-STATUS] CC=%d src_SSI=%u status=0x%04X\n",cc,src_ssi,status);
    if (state) {
        state->tetra_sds_src=src_ssi; state->tetra_sds_status=status;
        state->tetra_sds_status_log[state->tetra_sds_status_log_head&3u]=status;
        state->tetra_sds_status_log_head=(uint8_t)((state->tetra_sds_status_log_head+1u)&3u);
        /* EN 300 392-2 V3.8.1 table 29.13: SDS-SHORT REPORT is
         * carried in pre-coded status, with the six-bit prefix 011111. */
        if ((status >> 10) == 0x1fu) {
            state->tetra_sds_short_report_result = (uint8_t)((status >> 8) & 3u);
            state->tetra_sds_short_report_msg_ref = (uint8_t)status;
            state->tetra_sds_short_report_valid = 1;
        }
    }
}

/* -----------------------------------------------------------------------
 * CMCE D-CONNECT ACKNOWLEDGE, table 14.8.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_connect_ack(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<27){fprintf(stderr,"[TETRA CMCE D-CONNECT-ACK] CC=%d (too short: %d bits)\n",cc,nbits);return;}
    int off=5; uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t timeout=(uint8_t)mle_bits_to_uint(bits,off,4); off+=4;
    uint8_t grant=(uint8_t)mle_bits_to_uint(bits,off,2); off+=2;
    uint8_t permission=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t notification=0,notification_valid=0;
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if(optional_tail){
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if(present){if(off+6>nbits) goto truncated;notification=(uint8_t)mle_bits_to_uint(bits,off,6);off+=6;notification_valid=1;}
        if(!cmce_skip_type34(bits,nbits,&off)) goto truncated;
    }
    fprintf(stderr,"[TETRA CMCE D-CONNECT-ACK] CC=%d call=%u timeout=%u grant=%u\n",cc,(unsigned)id,timeout,grant);
    if(state){state->tetra_call_active=1;state->tetra_call_id=id;state->tetra_call_timeout=timeout;
      state->tetra_d_connect_ack_call_id=id;state->tetra_d_connect_ack_tx_grant=grant;
      state->tetra_d_connect_ack_tx_permission=permission;state->tetra_d_connect_ack_notification=notification;
      state->tetra_d_connect_ack_notification_valid=notification_valid;state->tetra_d_connect_ack_valid=1;
      state->tetra_cmce_call_generation++;}
    return;
truncated:
    fprintf(stderr,"[TETRA CMCE D-CONNECT-ACK] CC=%d (truncated optional IE: %d bits)\n",cc,nbits);
}

/* -----------------------------------------------------------------------
 * CMCE floor-control messages from Tables 14.17, 14.19 and 14.20.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_tx_event(const uint8_t *bits, int nbits,
                                   int cc, uint32_t pdu_type,
                                   dsd_state *state)
{
    const char *name;
    switch (pdu_type) {
    case TETRA_CMCE_D_TX_CONTINUE:  name = "D-TX-CONTINUE";  break;
    case TETRA_CMCE_D_TX_INTERRUPT: name = "D-TX-INTERRUPT"; break;
    case TETRA_CMCE_D_TX_WAIT:      name = "D-TX-WAIT";      break;
    default:                         name = "D-TX-?";         break;
    }

    int mandatory = pdu_type == TETRA_CMCE_D_TX_CONTINUE ? 22
                  : pdu_type == TETRA_CMCE_D_TX_INTERRUPT ? 25 : 21;
    if (nbits < mandatory) {
        fprintf(stderr, "[TETRA CMCE %s] CC=%d (too short: %d bits)\n", name, cc, nbits);
        return;
    }

    int off = 5;
    uint32_t call_id = mle_bits_to_uint(bits, off, 14); off += 14;
    uint32_t continue_value = 0;
    uint32_t grant = 0;
    uint32_t encryption = 0;
    if (pdu_type == TETRA_CMCE_D_TX_CONTINUE) {
        continue_value = mle_bits_to_uint(bits, off, 1); off += 1;
    } else if (pdu_type == TETRA_CMCE_D_TX_INTERRUPT) {
        grant = mle_bits_to_uint(bits, off, 2); off += 2;
    }
    uint32_t request_perm = mle_bits_to_uint(bits, off, 1); off += 1;
    if (pdu_type == TETRA_CMCE_D_TX_INTERRUPT) {
        encryption = mle_bits_to_uint(bits, off, 1); off += 1;
        off += 1; /* reserved */
    }

    uint32_t notification = 0, party_ssi = 0, party_extension = 0;
    uint8_t notification_valid = 0, party_type = 0, party_type_valid = 0;
    uint8_t party_ssi_valid = 0, party_extension_valid = 0;
    uint8_t optional_tail = (uint8_t)mle_bits_to_uint(bits, off, 1); off += 1;
    if (optional_tail) {
        uint32_t present = mle_bits_to_uint(bits, off, 1); off += 1;
        if (present) {
            if (off + 6 > nbits) {
                goto truncated;
            }
            notification = mle_bits_to_uint(bits, off, 6);
            off += 6;
            notification_valid = 1;
        }
    }

    /* Table 14.19 adds transmitting-party identity after notification. */
    if (pdu_type == TETRA_CMCE_D_TX_INTERRUPT && optional_tail) {
        if (off >= nbits)
            goto truncated;
        uint32_t present = mle_bits_to_uint(bits, off, 1); off += 1;
        if (present) {
            if (off + 2 > nbits)
                goto truncated;
            party_type = (uint8_t)mle_bits_to_uint(bits, off, 2); off += 2;
            party_type_valid = 1;
            if (party_type == 1 || party_type == 2) {
                if (off + 24 > nbits)
                    goto truncated;
                party_ssi = mle_bits_to_uint(bits, off, 24); off += 24;
                party_ssi_valid = 1;
            }
            if (party_type == 2) {
                if (off + 24 > nbits)
                    goto truncated;
                party_extension = mle_bits_to_uint(bits, off, 24); off += 24;
                party_extension_valid = 1;
            }
        }
    }
    if (optional_tail && !cmce_skip_type34(bits, nbits, &off))
        goto truncated;

    fprintf(stderr, "[TETRA CMCE %s] CC=%d  call=%u  request=%u  notif=%u",
            name, cc, call_id, request_perm, notification);
    if (pdu_type == TETRA_CMCE_D_TX_CONTINUE)
        fprintf(stderr, "  continue=%u", continue_value);
    if (pdu_type == TETRA_CMCE_D_TX_INTERRUPT)
        fprintf(stderr, "  grant=%u  enc=%u", grant, encryption);
    fprintf(stderr, "\n");

    if (state) {
        if (!cmce_matches_active_call(state, call_id)) {
            fprintf(stderr, "[TETRA CMCE %s] CC=%d call=%u ignored; active call=%u\n",
                    name, cc, call_id, (unsigned)state->tetra_call_id);
            return;
        }
        state->tetra_tx_event_call_id      = (uint16_t)call_id;
        state->tetra_tx_event_notification = (uint8_t)notification;
        state->tetra_tx_event_request_perm = (uint8_t)request_perm;
        state->tetra_tx_event_continue     = (uint8_t)continue_value;
        state->tetra_tx_event_grant        = (uint8_t)grant;
        state->tetra_tx_event_encryption   = (uint8_t)encryption;
        state->tetra_tx_event_notification_valid = notification_valid;
        state->tetra_tx_event_party_type = party_type;
        state->tetra_tx_event_party_type_valid = party_type_valid;
        state->tetra_tx_event_party_ssi = party_ssi;
        state->tetra_tx_event_party_ssi_valid = party_ssi_valid;
        state->tetra_tx_event_party_extension = party_extension;
        state->tetra_tx_event_party_extension_valid = party_extension_valid;

        switch (pdu_type) {
        case TETRA_CMCE_D_TX_CONTINUE:  state->tetra_tx_continue    = 1; break;
        case TETRA_CMCE_D_TX_INTERRUPT: state->tetra_tx_interrupted = 1; break;
        case TETRA_CMCE_D_TX_WAIT:      state->tetra_tx_wait        = 1; break;
        default: break;
        }
    }
    return;

truncated:
    fprintf(stderr, "[TETRA CMCE %s] CC=%d (truncated optional IE: %d bits)\n",
            name, cc, nbits);
}

/* -----------------------------------------------------------------------
 * CMCE D-INFO, ETSI EN 300 392-2 table 14.11.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_info(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<22){fprintf(stderr,"[TETRA CMCE D-INFO] CC=%d (too short: %d bits)\n",cc,nbits);return;}
    int off=5;
    uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t reset=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t poll=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint16_t new_id=0, modify=0;
    uint32_t temporary_address=0;
    uint8_t timeout=0, setup_timeout=0, ownership=0, status=0, notification=0, poll_percentage=0, poll_number=0;
    uint8_t new_id_valid=0, timeout_valid=0, setup_timeout_valid=0, ownership_valid=0, modify_valid=0;
    uint8_t status_valid=0, temporary_address_valid=0, notification_valid=0;
    uint8_t poll_percentage_valid=0, poll_number_valid=0;

#define DINFO_TYPE2(width, value, valid) do { \
        uint8_t present; \
        if (off >= nbits) goto truncated; \
        present=(uint8_t)mle_bits_to_uint(bits,off++,1); \
        if (present) { \
            if (off+(width)>nbits) goto truncated; \
            (value)=mle_bits_to_uint(bits,off,(width)); off+=(width); (valid)=1; \
        } \
    } while (0)
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if (optional_tail) {
        DINFO_TYPE2(14,new_id,new_id_valid);
        DINFO_TYPE2(4,timeout,timeout_valid);
        DINFO_TYPE2(3,setup_timeout,setup_timeout_valid);
        DINFO_TYPE2(1,ownership,ownership_valid);
        DINFO_TYPE2(9,modify,modify_valid);
        DINFO_TYPE2(3,status,status_valid);
        DINFO_TYPE2(24,temporary_address,temporary_address_valid);
        DINFO_TYPE2(6,notification,notification_valid);
        DINFO_TYPE2(6,poll_percentage,poll_percentage_valid);
        DINFO_TYPE2(6,poll_number,poll_number_valid);
        if(!cmce_skip_type34(bits,nbits,&off)) goto truncated;
    }
#undef DINFO_TYPE2
    fprintf(stderr,"[TETRA CMCE D-INFO] CC=%d call=%u reset=%u poll=%u\n",cc,(unsigned)id,reset,poll);
    if(state){state->tetra_d_info_call_id=id;state->tetra_d_info_call_timeout=reset;
      state->tetra_d_info_notification=poll;state->tetra_d_info_new_call_id=new_id;
      state->tetra_d_info_new_call_id_valid=new_id_valid;state->tetra_d_info_timeout=timeout;
      state->tetra_d_info_timeout_valid=timeout_valid;state->tetra_d_info_setup_timeout=setup_timeout;
      state->tetra_d_info_setup_timeout_valid=setup_timeout_valid;state->tetra_d_info_ownership=ownership;
      state->tetra_d_info_ownership_valid=ownership_valid;state->tetra_d_info_modify=modify;
      state->tetra_d_info_modify_valid=modify_valid;state->tetra_d_info_status=status;
      state->tetra_d_info_status_valid=status_valid;state->tetra_d_info_temporary_address=temporary_address;
      state->tetra_d_info_temporary_address_valid=temporary_address_valid;
      state->tetra_d_info_notification_indicator=notification;
      state->tetra_d_info_notification_indicator_valid=notification_valid;
      state->tetra_d_info_poll_percentage=poll_percentage;
      state->tetra_d_info_poll_percentage_valid=poll_percentage_valid;
      state->tetra_d_info_poll_number=poll_number;state->tetra_d_info_poll_number_valid=poll_number_valid;
      if(new_id_valid) state->tetra_call_id=new_id;
      if(timeout_valid) state->tetra_call_timeout=timeout;
      state->tetra_d_info_valid=1;}
    return;
truncated:
    fprintf(stderr,"[TETRA CMCE D-INFO] CC=%d (truncated optional IE: %d bits)\n",cc,nbits);
}

/* -----------------------------------------------------------------------
 * CMCE D-CALL-RESTORE, ETSI EN 300 392-2 table 14.6.
 * ----------------------------------------------------------------------- */
static int parse_cmce_d_call_restore(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<24){fprintf(stderr,"[TETRA CMCE D-CALL-RESTORE] CC=%d (too short: %d bits)\n",cc,nbits);return 0;}
    int off=5;
    uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t grant=(uint8_t)mle_bits_to_uint(bits,off,2); off+=2;
    uint8_t permission=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t reset=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint16_t new_id=0, modify=0;
    uint8_t timeout=0, status=0, notification=0;
    uint8_t new_id_valid=0, timeout_valid=0, status_valid=0, modify_valid=0, notification_valid=0;

    /* Table 14.6 type-2 optionals occur in this exact order. If a tail is
     * present, all remaining presence flags must be available before publish. */
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if (optional_tail) {
        if(off>=nbits) goto truncated;
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if(off+14>nbits) goto truncated; new_id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14; new_id_valid=1; }
        if(off>=nbits) goto truncated; present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if(off+4>nbits) goto truncated; timeout=(uint8_t)mle_bits_to_uint(bits,off,4); off+=4; timeout_valid=1; }
        if(off>=nbits) goto truncated; present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if(off+3>nbits) goto truncated; status=(uint8_t)mle_bits_to_uint(bits,off,3); off+=3; status_valid=1; }
        if(off>=nbits) goto truncated; present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if(off+9>nbits) goto truncated; modify=(uint16_t)mle_bits_to_uint(bits,off,9); off+=9; modify_valid=1; }
        if(off>=nbits) goto truncated; present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) { if(off+6>nbits) goto truncated; notification=(uint8_t)mle_bits_to_uint(bits,off,6); off+=6; notification_valid=1; }
        if(!cmce_skip_type34(bits,nbits,&off)) goto truncated;
    }
    fprintf(stderr,"[TETRA CMCE D-CALL-RESTORE] CC=%d call=%u grant=%u request=%u reset=%u\n",cc,(unsigned)id,grant,permission,reset);
    if(state){state->tetra_call_active=1;state->tetra_call_id=new_id_valid?new_id:id;state->tetra_call_restore_id=id;
      state->tetra_call_restore_grant=grant;state->tetra_call_restore_permission=permission;
      state->tetra_call_restore_reset=reset;state->tetra_call_restore_new_id=new_id;
      state->tetra_call_restore_new_id_valid=new_id_valid;state->tetra_call_restore_timeout=timeout;
      state->tetra_call_restore_timeout_valid=timeout_valid;state->tetra_call_restore_status=status;
      state->tetra_call_restore_status_valid=status_valid;state->tetra_call_restore_modify=modify;
      state->tetra_call_restore_modify_valid=modify_valid;state->tetra_call_restore_notification=notification;
      state->tetra_call_restore_notification_valid=notification_valid;state->tetra_call_restore_valid=1;
      state->tetra_cmce_call_generation++;}
    return 1;
truncated:
    fprintf(stderr,"[TETRA CMCE D-CALL-RESTORE] CC=%d (truncated optional IE: %d bits)\n",cc,nbits);
    return 0;
}

/* CMCE FUNCTION NOT SUPPORTED, table 14.33. */
static void parse_cmce_function_not_supported(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<20){fprintf(stderr,"[TETRA CMCE FUNCTION-NOT-SUPPORTED] CC=%d (too short: %d bits)\n",cc,nbits);return;}
    int off=5; uint8_t rejected=(uint8_t)mle_bits_to_uint(bits,off,5);off+=5;
    uint8_t id_present=(uint8_t)mle_bits_to_uint(bits,off++,1);uint16_t id=0;
    if(id_present){if(off+14+8>nbits)return;id=(uint16_t)mle_bits_to_uint(bits,off,14);off+=14;}
    if(off+8>nbits)return; uint8_t pointer=(uint8_t)mle_bits_to_uint(bits,off,8);off+=8;
    uint8_t extract_len=0;
    if(pointer){if(off+8>nbits)return;extract_len=(uint8_t)mle_bits_to_uint(bits,off,8);off+=8;if(off+extract_len>nbits)return;off+=extract_len;}
    if(off>=nbits)return;
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if(optional_tail && !cmce_skip_type34(bits,nbits,&off))return;
    fprintf(stderr,"[TETRA CMCE FUNCTION-NOT-SUPPORTED] CC=%d rejected=%u call=%u pointer=%u extract=%u\n",cc,rejected,(unsigned)id,pointer,extract_len);
    if(state){state->tetra_cmce_fns_rejected_pdu=rejected;state->tetra_cmce_fns_call_id_present=id_present;
      state->tetra_cmce_fns_call_id=id;state->tetra_cmce_fns_pointer=pointer;
      state->tetra_cmce_fns_extract_bits=extract_len;state->tetra_cmce_fns_valid=1;}
}

static int
decode_sds_text_payload(const uint8_t *bits, int payload_bits, int text_offset,
                        int timestamp_allowed, char decoded_text[256],
                        unsigned *decoded_length, uint8_t *decoded_unicode)
{
    if (payload_bits < text_offset + 8) return 0;
    unsigned timestamp = mle_bits_to_uint(bits, text_offset, 1);
    unsigned coding = mle_bits_to_uint(bits, text_offset + 1, 7);
    text_offset += 8;
    if (timestamp_allowed && timestamp) text_offset += 24;
    if (payload_bits < text_offset) return 0;

    if (coding == 0) {
        static const uint16_t alphabet[128] = {
            64,163,36,165,232,233,249,236,242,199,10,216,248,13,197,229,
            916,95,934,915,923,937,928,936,931,920,926,32,198,230,223,201,
            32,33,34,35,164,37,38,39,40,41,42,43,44,45,46,47,
            48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
            161,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,
            80,81,82,83,84,85,86,87,88,89,90,196,214,209,220,167,
            191,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,
            112,113,114,115,116,117,118,119,120,121,122,228,246,241,252,224
        };
        int available = payload_bits - text_offset;
        if (available % 8) return 0;
        int escaped = 0, full = 0;
        for (int pos = 0; pos + 7 <= available; pos += 7) {
            unsigned value = 0;
            for (int b = 0; b < 7; b++) {
                int packed = pos + b;
                int index = text_offset + (packed / 8) * 8 + 7 - packed % 8;
                value |= (unsigned)(bits[index] & 1u) << b;
            }
            if (!escaped && value == 27) { escaped = 1; continue; }
            unsigned ch = alphabet[value];
            if (escaped) {
                switch (value) {
                case 10: ch = 12; break;
                case 20: ch = '^'; break;
                case 40: ch = '{'; break;
                case 41: ch = '}'; break;
                case 47: ch = 92; break;
                case 60: ch = '['; break;
                case 61: ch = '~'; break;
                case 62: ch = ']'; break;
                case 64: ch = '|'; break;
                case 101: ch = 0x20ac; break;
                default: break;
                }
                escaped = 0;
            }
            unsigned count = ch < 128 ? 1 : ch < 2048 ? 2 : 3;
            if (*decoded_length + count >= 256) full = 1;
            if (full) continue;
            if (count == 1) decoded_text[(*decoded_length)++] = (char)ch;
            else {
                unsigned shift = (count - 1) * 6;
                decoded_text[(*decoded_length)++] = (char)((count == 2 ? 0xc0 : 0xe0) | (ch >> shift));
                while (shift) {
                    shift -= 6;
                    decoded_text[(*decoded_length)++] = (char)(0x80 | ((ch >> shift) & 63));
                }
            }
        }
        if (escaped) return 0;
    } else if (coding == 26) {
        if ((payload_bits - text_offset) % 16) return 0;
        *decoded_unicode = 1;
        int full = 0;
        for (int i = text_offset; i < payload_bits; i += 16) {
            uint32_t ch = mle_bits_to_uint(bits, i, 16);
            if (ch >= 0xd800 && ch <= 0xdbff) {
                if (i + 32 > payload_bits) return 0;
                uint32_t low = mle_bits_to_uint(bits, i + 16, 16);
                if (low < 0xdc00 || low > 0xdfff) return 0;
                ch = 0x10000 + ((ch - 0xd800) << 10) + low - 0xdc00;
                i += 16;
            } else if (ch >= 0xdc00 && ch <= 0xdfff) return 0;
            unsigned count = ch < 128 ? 1 : ch < 2048 ? 2 : ch < 65536 ? 3 : 4;
            if (*decoded_length + count >= 256) full = 1;
            if (full) continue;
            if (count == 1) decoded_text[(*decoded_length)++] = (char)(ch ? ch : ' ');
            else {
                unsigned shift = (count - 1) * 6;
                decoded_text[(*decoded_length)++] = (char)((count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0) | (ch >> shift));
                while (shift) {
                    shift -= 6;
                    decoded_text[(*decoded_length)++] = (char)(0x80 | ((ch >> shift) & 63));
                }
            }
        }
    } else if (coding == 1) {
        if ((payload_bits - text_offset) % 8) return 0;
        for (int i = text_offset; i < payload_bits; i += 8) {
            unsigned ch = mle_bits_to_uint(bits, i, 8);
            unsigned count = ch < 128 ? 1 : 2;
            if (*decoded_length + count >= 256) break;
            if (ch >= 128) decoded_text[(*decoded_length)++] = (char)(0xc0u | (ch >> 6));
            decoded_text[(*decoded_length)++] = (char)(ch >= 128 ? (0x80u | (ch & 63u)) : (ch ? ch : ' '));
        }
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * CMCE D-SDS-DATA (PDU type 15), ETSI EN 300 392-2 table 14.13.
 *
 * SDS-ACK, SDS-REPORT and SDS-TRANSFER are SDS-TL PDUs carried inside the
 * SDTI=3 user data.  They are not separate CMCE PDU types.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_sds_data(const uint8_t *bits, int nbits,
                                   int cc, dsd_state *state)
{
    int off = 5;
    uint32_t src_ssi = 0;
    uint32_t cpti;
    uint32_t sdti;
    int payload_bits;
    uint8_t *assembled = NULL;

    if (nbits < off + 2) {
        fprintf(stderr, "[TETRA CMCE D-SDS-DATA] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }

    cpti = mle_bits_to_uint(bits, off, 2); off += 2;
    if (cpti == 1 || cpti == 2) {
        if (off + 24 > nbits) goto truncated;
        src_ssi = mle_bits_to_uint(bits, off, 24); off += 24;
    }
    if (cpti == 2) {
        if (off + 24 > nbits) goto truncated;
        off += 24;
    }
    if (off + 2 > nbits) goto truncated;

    sdti = mle_bits_to_uint(bits, off, 2); off += 2;
    if (sdti < 3) {
        static const int fixed_lengths[3] = { 16, 32, 64 };
        payload_bits = fixed_lengths[sdti];
    } else {
        if (off + 11 > nbits) goto truncated;
        payload_bits = (int)mle_bits_to_uint(bits, off, 11); off += 11;
    }
    if (off + payload_bits > nbits) goto truncated;
    int tail_off=off+payload_bits;
    if(tail_off>=nbits) goto truncated;
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,tail_off++,1);
    if(optional_tail && !cmce_skip_type34(bits,nbits,&tail_off)) goto truncated;

    /* SDS-ACK: table 29.11. Only transport PIDs (128..254) use
     * this header; PID 255 is an extension, not a transport PID. */
    int sds_ack = 0, sds_report = 0, sds_transfer = 0;
    int text_offset = -1;
    char decoded_text[256] = {0};
    unsigned decoded_length = 0;
    uint8_t decoded_unicode = 0;
    uint8_t ack_status = 0, ack_ref = 0;
    uint8_t storage_forward = 0, validity_period = 0;
    uint8_t forward_type = 0, forward_valid = 0, forward_sna = 0;
    uint32_t forward_ssi = 0, forward_extension = 0;
    char forward_external[25] = {0};
    const uint8_t *text_bits = bits + off;
    int text_payload_bits = payload_bits;
    int text_timestamp_allowed = 0;
    int transport_header = 0;
    int concat_message = 0;
    if (sdti == 3 && payload_bits >= 8) {
        uint32_t pid = mle_bits_to_uint(bits, off, 8);
        if ((pid == 2 || pid == 9 || (pid >= 128 && pid < 255)) && payload_bits < 12)
            goto truncated;
        if (payload_bits < 12) goto publish_sds;
        uint32_t type = mle_bits_to_uint(bits, off + 8, 4);
        if (pid == 2 || pid == 9) text_offset = 8;
        if (pid >= 128 && pid < 255 && type <= 2) {
            int header = type == 0 ? 24 : 32;
            transport_header = header;
            if (payload_bits < header) goto truncated;
            if (type != 0) ack_status = (uint8_t)mle_bits_to_uint(bits, off + 16, 8);
            ack_ref = (uint8_t)mle_bits_to_uint(bits, off + header - 8, 8);
            /* Tables 29.12/29.14: REPORT/TRANSFER storage/forward fields.
             * Validate the whole conditional address before publishing. */
            if (type != 2 && mle_bits_to_uint(bits, off + 15, 1)) {
                if (payload_bits < header + 8) goto truncated;
                storage_forward = 1;
                validity_period = (uint8_t)mle_bits_to_uint(bits, off + header, 5);
                unsigned address = mle_bits_to_uint(bits, off + header + 5, 3);
                int required = header + 8;
                switch (address) {
                case 0:
                    if (payload_bits < required + 8) goto truncated;
                    forward_sna = (uint8_t)mle_bits_to_uint(bits, off + required, 8);
                    required += 8;
                    break;
                case 1:
                    if (payload_bits < required + 24) goto truncated;
                    forward_ssi = mle_bits_to_uint(bits, off + required, 24);
                    required += 24;
                    break;
                case 2:
                    if (payload_bits < required + 48) goto truncated;
                    forward_ssi = mle_bits_to_uint(bits, off + required, 24);
                    forward_extension = mle_bits_to_uint(bits, off + required + 24, 24);
                    required += 48;
                    break;
                case 3: {
                    if (payload_bits < header + 16) goto truncated;
                    unsigned digits = mle_bits_to_uint(bits, off + header + 8, 8);
                    if (digits == 0 || digits > 24) goto truncated;
                    required = header + 16 + (int)((digits + 1u) & ~1u) * 4;
                    if (payload_bits < required) goto truncated;
                    static const char digit_chars[] = "0123456789*#+";
                    for (unsigned digit = 0; digit < digits; digit++) {
                        unsigned value = mle_bits_to_uint(bits, off + header + 16 + (int)digit * 4, 4);
                        if (value > 12) goto truncated;
                        forward_external[digit] = digit_chars[value];
                    }
                    if ((digits & 1u) != 0u
                        && mle_bits_to_uint(bits, off + header + 16 + (int)digits * 4, 4) != 0)
                        goto truncated;
                    break;
                }
                case 7: break;
                default: goto truncated; /* Reserved address layout. */
                }
                if (payload_bits < required) goto truncated;
                forward_type = (uint8_t)address;
                forward_valid = 1;
                header = required;
                transport_header = header;
            }
            sds_ack = type == 2;
            sds_report = type == 1;
            sds_transfer = type == 0;
            if (sds_transfer && (pid == 130 || pid == 137)) {
                text_offset = header;
                text_timestamp_allowed = 1;
            }
        }

        if (pid == 12 || pid == 140) {
            concat_message = 1;
            int concat_off = pid == 12 ? 8 : transport_header;
            if (pid == 140 && !sds_transfer) goto truncated;
            if (concat_off <= 0 || payload_bits < concat_off + 24) goto truncated;
            if (mle_bits_to_uint(bits, off + concat_off, 3) != 0) goto truncated;
            concat_off += 3;
            unsigned extended = mle_bits_to_uint(bits, off + concat_off, 1); concat_off++;
            unsigned ref_extension = 0;
            if (extended) {
                if (payload_bits < concat_off + 8 + 20) goto truncated;
                ref_extension = mle_bits_to_uint(bits, off + concat_off, 8); concat_off += 8;
            }
            uint16_t reference = (uint16_t)((ref_extension << 4)
                | mle_bits_to_uint(bits, off + concat_off, 4)); concat_off += 4;
            uint8_t total = (uint8_t)mle_bits_to_uint(bits, off + concat_off, 8); concat_off += 8;
            uint8_t sequence = (uint8_t)mle_bits_to_uint(bits, off + concat_off, 8); concat_off += 8;
            int logical_pid = -1;
            if (sequence == 1) {
                if (payload_bits < concat_off + 8) goto truncated;
                logical_pid = (int)mle_bits_to_uint(bits, off + concat_off, 8); concat_off += 8;
                if (logical_pid == 12 || logical_pid == 140) goto truncated;
            }
            if (payload_bits < concat_off) goto truncated;
            int assembled_bits = 0;
            uint8_t assembled_pid = 0, received = 0;
            int result = sds_concat_submit(src_ssi, reference, (uint8_t)(pid == 140),
                                           total, sequence, logical_pid,
                                           bits + off + concat_off,
                                           payload_bits - concat_off, &assembled,
                                           &assembled_bits, &assembled_pid, &received);
            if (result < 0) goto truncated;
            if (state) {
                state->tetra_sds_concat_ref = reference;
                state->tetra_sds_concat_total = total;
                state->tetra_sds_concat_received = received;
                state->tetra_sds_concat_sequence = sequence;
                state->tetra_sds_concat_valid = 1;
                state->tetra_sds_concat_duplicate = (uint8_t)(result == 2);
                state->tetra_sds_concat_complete = (uint8_t)(result == 1);
                state->tetra_sds_src = src_ssi;
                state->tetra_sds_last_cc = (int8_t)cc;
                if (result == 1) state->tetra_sds_concat_generation++;
            }
            fprintf(stderr,
                    "[TETRA SDS CONCAT] CC=%d src_SSI=%u ref=%u part=%u/%u received=%u%s\n",
                    cc, src_ssi, reference, sequence, total, received,
                    result == 1 ? " complete" : result == 2 ? " duplicate" : "");
            if (result != 1) return;
            text_bits = assembled;
            text_payload_bits = assembled_bits;
            text_offset = (assembled_pid == 2 || assembled_pid == 9
                           || assembled_pid == 130 || assembled_pid == 137) ? 0 : -1;
            text_timestamp_allowed = assembled_pid == 130 || assembled_pid == 137;
        }
    }

    if (text_offset >= 0) {
        if (!decode_sds_text_payload(text_bits, text_payload_bits, text_offset,
                                     text_timestamp_allowed, decoded_text,
                                     &decoded_length, &decoded_unicode))
            goto truncated;
    }

publish_sds:
    fprintf(stderr, "[TETRA CMCE D-SDS-DATA] CC=%d src_SSI=%u CPTI=%u SDTI=%u data_bits=%d\n",
            cc, src_ssi, cpti, sdti, payload_bits);
    if (state) {
        /* Commit only after the complete declared payload is available. */
        state->tetra_sds_text_len = 0;
        state->tetra_sds_text[0] = '\0';
        state->tetra_sds_text_unicode = decoded_unicode;
        memcpy(state->tetra_sds_text, decoded_text, sizeof(decoded_text));
        state->tetra_sds_text_len = (uint16_t)decoded_length;
        state->tetra_sds_short_valid = 0;
        state->tetra_sds_src = src_ssi;
        state->tetra_cmce_sds_data_type = (uint8_t)sdti;
        state->tetra_sds_last_cc = (int8_t)cc;
        if (!concat_message) {
            state->tetra_sds_concat_valid = 0;
            state->tetra_sds_concat_duplicate = 0;
            state->tetra_sds_concat_complete = 0;
        }
        state->tetra_sds_storage_forward = storage_forward;
        state->tetra_sds_validity_period = validity_period;
        state->tetra_sds_forward_type = forward_type;
        state->tetra_sds_forward_valid = forward_valid;
        state->tetra_sds_forward_sna = forward_sna;
        state->tetra_sds_forward_ssi = forward_ssi;
        state->tetra_sds_forward_extension = forward_extension;
        memcpy(state->tetra_sds_forward_external, forward_external,
               sizeof(state->tetra_sds_forward_external));
        if (sds_transfer) state->tetra_sds_msg_ref = ack_ref;
        if (sds_ack) {
            state->tetra_sds_ack_delivery_status = ack_status;
            state->tetra_sds_ack_msg_ref = ack_ref;
            state->tetra_sds_msg_ref = ack_ref;
            state->tetra_sds_ack_valid = 1;
        }
        if (sds_report) {
            state->tetra_sds_report_cause = ack_status;
            state->tetra_sds_report_delivery_ok = ack_status < 32;
            state->tetra_sds_report_msg_ref = ack_ref;
            state->tetra_sds_msg_ref = ack_ref;
            state->tetra_sds_report_valid = 1;
        }
        if (sdti == 0) {
            state->tetra_sds_short_data = (uint16_t)mle_bits_to_uint(bits, off, 16);
            state->tetra_sds_short_src = src_ssi;
            state->tetra_sds_short_valid = 1;
        }
    }
    free(assembled);
    return;

truncated:
    free(assembled);
    fprintf(stderr, "[TETRA CMCE D-SDS-DATA] CC=%d src_SSI=%u (truncated: %d bits)\n",
            cc, src_ssi, nbits);
}

/* CMCE D-FACILITY, ETSI EN 300 392-2 table 14.10 and annex E.1.2.
 * After the PDU type, a four-bit count introduces independently encoded
 * SS-PDUs. Each has an 11-bit length followed by that many content bits; the
 * collection ends with the outer D-FACILITY O-bit and, when set, its
 * length-delimited type-3/4 chain. */
static void parse_cmce_d_facility(const uint8_t *bits, int nbits,
                                   int cc, dsd_state *state)
{
    int off = 5;
    uint32_t count;
    uint32_t first_ss_type = 0;

    if (nbits < off + 4) goto truncated;
    count = mle_bits_to_uint(bits, off, 4); off += 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t length;
        if (off + 11 > nbits) goto truncated;
        length = mle_bits_to_uint(bits, off, 11); off += 11;
        if (length > (uint32_t)(nbits - off)) goto truncated;
        if (i == 0 && length >= 6)
            first_ss_type = mle_bits_to_uint(bits, off, 6);
        off += (int)length;
    }
    if (off + 1 > nbits) goto truncated;
    uint8_t optional_tail=(uint8_t)mle_bits_to_uint(bits,off++,1);
    if(optional_tail && !cmce_skip_type34(bits,nbits,&off))goto truncated;

    fprintf(stderr, "[TETRA CMCE D-FACILITY] CC=%d ss_pdus=%u first_ss_type=%u\n",
            cc, count, first_ss_type);
    if (state) {
        state->tetra_facility_type = (uint8_t)first_ss_type;
        state->tetra_facility_valid = 1;
    }
    return;

truncated:
    fprintf(stderr, "[TETRA CMCE D-FACILITY] CC=%d (truncated: %d bits)\n", cc, nbits);
}

/* -----------------------------------------------------------------------
 * CMCE PDU dispatcher  (bits = CMCE PDU starting at bit 0)
 * ----------------------------------------------------------------------- */
static void tetra_cmce_parse(const uint8_t *bits, int nbits,
                              int cc, const dsd_opts *opts, dsd_state *state)
{
    if (nbits < 5) {
        fprintf(stderr, "[TETRA CMCE] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }

    uint32_t pdu_type = mle_bits_to_uint(bits, 0, 5);

    switch (pdu_type) {

    case TETRA_CMCE_D_ALERT:
        parse_cmce_d_alert(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_D_CALL_PROCEEDING:
        parse_cmce_d_call_proceeding(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_D_SETUP:
        parse_cmce_d_setup(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_D_STATUS:
        parse_cmce_d_status(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_D_RELEASE:
        parse_cmce_d_release(bits, nbits, cc, 0, (dsd_opts *)(uintptr_t)opts, state);
        break;

    case TETRA_CMCE_D_DISCONNECT:
        parse_cmce_d_release(bits, nbits, cc, 1, (dsd_opts *)(uintptr_t)opts, state);
        break;

    case TETRA_CMCE_D_CONNECT:
        parse_cmce_d_connect(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_D_CONNECT_ACK:
        parse_cmce_d_connect_ack(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_D_TX_GRANTED:
        parse_cmce_d_tx_granted(bits, nbits, cc, (dsd_opts *)(uintptr_t)opts, state);
        break;

    case TETRA_CMCE_D_TX_CEASED:
        parse_cmce_d_tx_ceased(bits, nbits, cc, (dsd_opts *)(uintptr_t)opts, state);
        break;

    case TETRA_CMCE_D_TX_CONTINUE:  /* fall-through */
    case TETRA_CMCE_D_TX_INTERRUPT: /* fall-through */
    case TETRA_CMCE_D_TX_WAIT:
        parse_cmce_d_tx_event(bits, nbits, cc, pdu_type, state);
        break;

    case TETRA_CMCE_D_INFO:
        parse_cmce_d_info(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_D_CALL_RESTORE:
        parse_cmce_d_call_restore(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_FUNCTION_NOT_SUPPORTED:
        parse_cmce_function_not_supported(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_D_FACILITY:
        parse_cmce_d_facility(bits, nbits, cc, state);
        break;

    case TETRA_CMCE_D_SDS_DATA:
        parse_cmce_d_sds_data(bits, nbits, cc, state);
        break;

    default:
        if (opts && opts->errorbars) {
            fprintf(stderr, "[TETRA CMCE] CC=%d  pdu_type=%u  (%d bits)\n",
                    cc, pdu_type, nbits);
        }
        break;
    }
}

/* MLE cell-reselection response PDUs, tables 18.6-18.9 and Annex E.19-E.24.
 * Each PDU has the mandatory O-bit required by Annex E encoding.  These four
 * messages define no Type 2 fields, so an asserted O-bit is malformed. */
static void
parse_mle_d_new_cell(const uint8_t *bits, int nbits, int cc,
                     const dsd_opts *opts, dsd_state *state)
{
    if (nbits < 6 || (bits[5] & 1u)) {
        fprintf(stderr, "[TETRA MLE D-NEW-CELL] CC=%d (invalid/truncated: %d bits)\n",
                cc, nbits);
        return;
    }
    uint8_t command = (uint8_t)mle_bits_to_uint(bits, 3, 2);
    if (state) {
        state->tetra_mle_new_cell_channel_command = command;
        state->tetra_mle_new_cell_valid = 1;
    }
    if (nbits > 6)
        tetra_mm_dispatch(bits + 6, nbits - 6, cc, opts, state);
}

static void
parse_mle_d_prepare_fail(const uint8_t *bits, int nbits, int cc,
                         const dsd_opts *opts, dsd_state *state)
{
    if (nbits < 6 || (bits[5] & 1u)) {
        fprintf(stderr, "[TETRA MLE D-PREPARE-FAIL] CC=%d (invalid/truncated: %d bits)\n",
                cc, nbits);
        return;
    }
    uint8_t cause = (uint8_t)mle_bits_to_uint(bits, 3, 2);
    if (state) {
        state->tetra_mle_prepare_fail_cause = cause;
        state->tetra_mle_prepare_fail_valid = 1;
    }
    if (nbits > 6)
        tetra_mm_dispatch(bits + 6, nbits - 6, cc, opts, state);
}

static void
parse_mle_d_restore_ack(const uint8_t *bits, int nbits, int cc,
                        dsd_state *state)
{
    /* O-bit followed by the mandatory CMCE D-CALL-RESTORE SDU. */
    if (nbits < 28 || (bits[3] & 1u)
        || mle_bits_to_uint(bits, 4, 5) != TETRA_CMCE_D_CALL_RESTORE) {
        fprintf(stderr, "[TETRA MLE D-RESTORE-ACK] CC=%d (invalid/truncated: %d bits)\n",
                cc, nbits);
        return;
    }
    if (parse_cmce_d_call_restore(bits + 4, nbits - 4, cc, state) && state)
        state->tetra_restore_ack = 1;
}

static void
parse_mle_d_restore_fail(const uint8_t *bits, int nbits, int cc,
                         dsd_state *state)
{
    if (nbits < 6 || (bits[5] & 1u)) {
        fprintf(stderr, "[TETRA MLE D-RESTORE-FAIL] CC=%d (invalid/truncated: %d bits)\n",
                cc, nbits);
        return;
    }
    if (state) {
        state->tetra_mle_restore_fail_cause =
            (uint8_t)mle_bits_to_uint(bits, 3, 2);
        state->tetra_mle_restore_fail_valid = 1;
    }
}

/* D-CHANNEL RESPONSE, table 18.10.  Its two Type 2 fields are reserved and
 * explicitly forbidden by the current specification, so a conforming PDU has
 * O=0 after the three mandatory values. */
static void
parse_mle_d_channel_response(const uint8_t *bits, int nbits, int cc,
                             dsd_state *state)
{
    if (nbits < 12 || (bits[11] & 1u)) {
        fprintf(stderr, "[TETRA MLE D-CHANNEL-RESPONSE] CC=%d (invalid/truncated: %d bits)\n",
                cc, nbits);
        return;
    }
    if (state) {
        state->tetra_mle_channel_response_type = bits[3] & 1u;
        state->tetra_mle_channel_response_reason =
            (uint8_t)mle_bits_to_uint(bits, 4, 3);
        state->tetra_mle_channel_response_retry_delay =
            (uint8_t)mle_bits_to_uint(bits, 7, 4);
        state->tetra_mle_channel_response_valid = 1;
    }
}

/* -----------------------------------------------------------------------
 * Public entry point: tetra_mle_dispatch()
 * ----------------------------------------------------------------------- */
void tetra_mle_dispatch(const uint8_t *bits, int nbits,
                        int cc, const dsd_opts *opts, dsd_state *state)
{
    if (!bits || nbits < 3) {
        return;
    }

    /* EN 300 392-2 table 18.87: every TM-SDU begins with this 3-bit
     * discriminator. MLE protocol PDUs then add the table 18.85 3-bit type;
     * all other protocol bodies start immediately at bit 3. */
    uint32_t protocol = mle_bits_to_uint(bits, 0, 3);
    if (protocol == TETRA_MLE_PD_CMCE) {
        tetra_cmce_parse(bits + 3, nbits - 3, cc, opts, state);
        return;
    }
    if (protocol == TETRA_MLE_PD_MM) {
        tetra_mm_dispatch(bits + 3, nbits - 3, cc, opts, state);
        return;
    }
    if (protocol == TETRA_MLE_PD_SNDCP) {
        const uint8_t *sn = bits + 3;
        int sn_nbits = nbits - 3;
        if (sn_nbits >= 8) {
            uint32_t nsapi = mle_bits_to_uint(sn, 0, 4);
            uint32_t sn_type = mle_bits_to_uint(sn, 4, 4);
            if (state) {
                state->tetra_sndcp_nsapi = (uint8_t)nsapi;
                state->tetra_sndcp_pdu_type = (uint8_t)sn_type;
                state->tetra_sndcp_nbits = (uint16_t)sn_nbits;
                state->tetra_sndcp_valid = 1;
            }
        }
        return;
    }
    if (protocol == TETRA_MLE_PD_MLE) {
        if (nbits < 6) return;
        const uint8_t *pdu = bits + 3;
        int pdu_nbits = nbits - 3;
        uint32_t type = mle_bits_to_uint(pdu, 0, 3);
        if (type == TETRA_MLE_D_NEW_CELL) {
            parse_mle_d_new_cell(pdu, pdu_nbits, cc, opts, state);
        } else if (type == TETRA_MLE_D_PREPARE_FAIL) {
            parse_mle_d_prepare_fail(pdu, pdu_nbits, cc, opts, state);
        } else if (type == TETRA_MLE_D_NWRK_BROADCAST) {
            parse_mle_d_nwrk_broadcast(pdu, pdu_nbits, cc, state);
        } else if (type == TETRA_MLE_D_NWRK_BCAST_EXT) {
            parse_mle_d_nwrk_broadcast_ext(pdu, pdu_nbits, cc, state);
        } else if (type == TETRA_MLE_D_RESTORE_ACK) {
            parse_mle_d_restore_ack(pdu, pdu_nbits, cc, state);
        } else if (type == TETRA_MLE_D_RESTORE_FAIL) {
            parse_mle_d_restore_fail(pdu, pdu_nbits, cc, state);
        } else if (type == TETRA_MLE_D_CHANNEL_RESPONSE) {
            parse_mle_d_channel_response(pdu, pdu_nbits, cc, state);
        } else if (type == TETRA_MLE_EXTENDED_PDU && pdu_nbits >= 7) {
            uint32_t extension = mle_bits_to_uint(pdu, 3, 4);
            if (extension == TETRA_MLE_D_NWRK_BROADCAST_DA)
                parse_mle_d_nwrk_broadcast_da(pdu, pdu_nbits, cc, state);
            else if (extension == TETRA_MLE_D_NWRK_BCAST_REMOVE)
                parse_mle_d_nwrk_broadcast_remove(pdu, pdu_nbits, cc, state);
        }
        return;
    }

    if (opts && opts->errorbars) {
        fprintf(stderr, "[TETRA MLE] CC=%d protocol_discriminator=%u (%d bits)\n",
                cc, protocol, nbits);
    }
    return;
}
