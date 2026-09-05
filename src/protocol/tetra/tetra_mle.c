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
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>

/* Phase 81: bits_to_uint unified — see tetra_bits.h */
#define mle_bits_to_uint  tetra_bits_to_uint

/* -----------------------------------------------------------------------
 * CMCE D-SETUP, ETSI EN 300 392-2 table 14.15.
 * Mandatory part: type(5), call id(14), timeout(4), hook(1), duplex(1),
 * basic service(8), transmission grant(2), request permission(1), priority(4).
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_setup(const uint8_t *bits, int nbits,
                                int cc, dsd_state *state)
{
    if (nbits < 40) {
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
    uint8_t notification=0, cpti=0;
    uint32_t calling_ssi=0;

    /* Type-2 optionals occur in table order. */
    if (off < nbits) {
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) {
            if (off+6>nbits) goto truncated;
            notification=(uint8_t)mle_bits_to_uint(bits,off,6); off+=6;
        }
    }
    if (off < nbits) {
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) {
            if (off+24>nbits) goto truncated;
            off+=24;
        }
    }
    if (off < nbits) {
        uint8_t present=(uint8_t)mle_bits_to_uint(bits,off++,1);
        if (present) {
            if (off+2>nbits) goto truncated;
            cpti=(uint8_t)mle_bits_to_uint(bits,off,2); off+=2;
            if (cpti==1 || cpti==2) {
                if (off+24>nbits) goto truncated;
                calling_ssi=mle_bits_to_uint(bits,off,24); off+=24;
            }
            if (cpti==2) {
                if (off+24>nbits) goto truncated;
                off+=24;
            }
        }
    }

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
        state->tetra_enc_mode=encrypted;
        if (calling_ssi) state->tetra_calling_ssi=calling_ssi;
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
    if (nbits < 24) {
        fprintf(stderr,"[TETRA CMCE %s] CC=%d (too short: %d bits)\n",is_disconnect?"D-DISCONNECT":"D-RELEASE",cc,nbits);
        return;
    }
    uint16_t call_id=(uint16_t)mle_bits_to_uint(bits,5,14);
    uint8_t cause=(uint8_t)mle_bits_to_uint(bits,19,5);
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
        state->tetra_call_id=call_id;
        state->tetra_cmce_release_cause_type=0;
        state->tetra_cmce_release_cause=cause;
        tetra_sm_on_release(opts,state);
    }
}

/* -----------------------------------------------------------------------
 * CMCE D-CONNECT, ETSI EN 300 392-2 table 14.7.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_connect(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if (nbits<29) { fprintf(stderr,"[TETRA CMCE D-CONNECT] CC=%d (too short: %d bits)\n",cc,nbits); return; }
    int off=5; uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t timeout=(uint8_t)mle_bits_to_uint(bits,off,4); off+=4;
    uint8_t hook=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t duplex=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t grant=(uint8_t)mle_bits_to_uint(bits,off,2); off+=2;
    uint8_t permission=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t ownership=(uint8_t)mle_bits_to_uint(bits,off++,1);
    fprintf(stderr,"[TETRA CMCE D-CONNECT] CC=%d call=%u timeout=%u grant=%u request=%u\n",cc,(unsigned)id,timeout,grant,permission);
    if (state) { state->tetra_call_active=1; state->tetra_call_id=id; state->tetra_call_timeout=timeout;
        state->tetra_connect_valid=1; state->tetra_connect_call_type=0; state->tetra_connect_enc_mode=0;
        state->tetra_cmce_connect_hook=hook; state->tetra_cmce_connect_duplex=duplex;
        state->tetra_cmce_connect_tx_grant=grant; state->tetra_cmce_connect_tx_permission=permission;
        state->tetra_cmce_connect_ownership=ownership; }
}

/* -----------------------------------------------------------------------
 * CMCE D-TX-GRANTED (PDU type 10)  — ETSI EN 300 392-2 §14.7.1.15
 *
 * Mandatory bits (Table 14.18): PDU type (5), call identifier (14),
 * transmission grant (2), request permission (1), encryption control (1),
 * and reserved (1). Optional type-2 elements follow with presence flags.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_tx_granted(const uint8_t *bits, int nbits,
                                     int cc, dsd_opts *opts, dsd_state *state)
{
    if (nbits < 24) {
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

    uint32_t granted_ssi = 0;
    uint8_t  got_ssi     = 0;

    /* Notification indicator (optional type 2). */
    if (off < nbits) {
        uint32_t present = mle_bits_to_uint(bits, off, 1); off += 1;
        if (present) {
            if (off + 6 > nbits)
                goto truncated;
            off += 6;
        }
    }

    /* Transmitting party type identifier and its conditional SSI. */
    if (off < nbits) {
        uint32_t present = mle_bits_to_uint(bits, off, 1); off += 1;
        if (present) {
            if (off + 2 > nbits)
                goto truncated;
            uint32_t party_type = mle_bits_to_uint(bits, off, 2); off += 2;
            if (party_type == 1 || party_type == 2) {
                if (off + 24 > nbits)
                    goto truncated;
                granted_ssi = mle_bits_to_uint(bits, off, 24); off += 24;
                got_ssi = 1;
            }
            if (party_type == 2) {
                if (off + 24 > nbits)
                    goto truncated;
                off += 24; /* transmitting party extension (MCC + MNC) */
            }
        }
    }

    fprintf(stderr, "[TETRA CMCE D-TX-GRANTED] CC=%d  call=%u  grant=%u  request=%u  enc=%u",
            cc, call_id, tx_grant, tx_perm, enc_mode);
    if (got_ssi)
        fprintf(stderr, "  granted_SSI=%u", granted_ssi);
    if (reserved)
        fprintf(stderr, "  reserved=%u", reserved);
    fprintf(stderr, "\n");

    if (state) {
        /* Values 0 and 3 switch on the receive U-plane; 1 and 2 do not. */
        state->tetra_tx_granted_valid = (uint8_t)(tx_grant == 0 || tx_grant == 3);
        state->tetra_enc_mode = (uint8_t)(enc_mode & 0x01u);
        
        /* Phase 78: CMCE D-TX-GRANTED dropped variables */
        state->tetra_cmce_tx_granted_perm   = (uint8_t)(tx_perm & 1u);
        state->tetra_cmce_tx_granted_reserv = (uint8_t)(reserved & 1u);

        if (got_ssi) {
            state->tetra_tx_granted_ssi = granted_ssi;
        }
    }

    (void)opts; /* channel allocation and tuning are MAC-layer responsibilities */
    return;

truncated:
    fprintf(stderr, "[TETRA CMCE D-TX-GRANTED] CC=%d (truncated optional IE: %d bits)\n",
            cc, nbits);
    (void)opts;
}

/* -----------------------------------------------------------------------
 * CMCE D-TX-CEASED (PDU type 8) — ETSI EN 300 392-2 §14.7.1.13.
 * Mandatory fields are PDU type (5), call identifier (14), and transmission
 * request permission (1). Notification indicator is the first optional IE.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_tx_ceased(const uint8_t *bits, int nbits,
                                    int cc, dsd_opts *opts, dsd_state *state)
{
    if (nbits < 20) {
        fprintf(stderr, "[TETRA CMCE D-TX-CEASED] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    int off = 5;
    uint16_t call_id = (uint16_t)mle_bits_to_uint(bits, off, 14); off += 14;
    uint8_t tx_perm = (uint8_t)mle_bits_to_uint(bits, off, 1); off += 1;
    uint8_t notification = 0;
    uint8_t got_notification = 0;
    if (off < nbits) {
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
    }
    fprintf(stderr, "[TETRA CMCE D-TX-CEASED] CC=%d  call=%u  request=%u",
            cc, (unsigned)call_id, (unsigned)tx_perm);
    if (got_notification)
        fprintf(stderr, "  notification=%u", (unsigned)notification);
    fprintf(stderr, "  (floor released)\n");
    if (state) {
        state->tetra_tx_ceased_tx_perm     = tx_perm;
        state->tetra_tx_ceased_cipher_info = 0; /* retained ABI field; no such IE in Table 14.16 */
        state->tetra_tx_granted_valid      = 0;
    }
    /* Floor release does not release the assigned traffic channel. */
    (void)opts;
}

/* -----------------------------------------------------------------------
 * MLE D-NWRK-BROADCAST (MLE type 0)  —  ETSI EN 300 392-2 §21.6.1
 *
 * Carried in MAC-BROADCAST bcast_type 3 (TETRA_MAC_BC_NWRK_BCAST).
 * Mandatory fields after the 5-bit MLE PDU type:
 *   Bits  5-18  : Location Area       (14 bits)
 *   Bits 19-34  : Subscriber class    (16 bits)
 *   Bit     35  : Registration        ( 1 bit )
 *   (further mandatory / optional IEs not parsed here)
 * ----------------------------------------------------------------------- */
static void parse_mle_d_nwrk_broadcast(const uint8_t *bits, int nbits,
                                        int cc, dsd_state *state)
{
    if (nbits < 36) {
        fprintf(stderr, "[TETRA MLE D-NWRK-BROADCAST] CC=%d (too short: %d bits)\n",
                cc, nbits);
        return;
    }

    uint32_t la          = mle_bits_to_uint(bits, 5, 14);
    uint32_t subscr_cls  = mle_bits_to_uint(bits, 19, 16);
    uint32_t registration = mle_bits_to_uint(bits, 35, 1);

    fprintf(stderr, "[TETRA MLE D-NWRK-BROADCAST] CC=%d  LA=%u"
                    "  subscr_class=0x%04X  reg=%u\n",
            cc, la, subscr_cls, registration);

    if (state) {
        state->tetra_la               = (uint16_t)la;
        state->tetra_subscr_class     = (uint16_t)subscr_cls;
        state->tetra_nwrk_bcast_known = 1;
        /* Phase 78: dropped MLE D-NWRK-BROADCAST variable */
        state->tetra_mle_registration = (uint8_t)(registration & 1u);
    }
}

/* -----------------------------------------------------------------------
 * CMCE D-ALERT and D-CALL-PROCEEDING, tables 14.4 and 14.5.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_alert(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if (nbits<25) { fprintf(stderr,"[TETRA CMCE D-ALERT] CC=%d (too short: %d bits)\n",cc,nbits); return; }
    int off=5; uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t timeout=(uint8_t)mle_bits_to_uint(bits,off,3); off+=3;
    uint8_t reserved=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t duplex=(uint8_t)mle_bits_to_uint(bits,off++,1);
    uint8_t queued=(uint8_t)mle_bits_to_uint(bits,off++,1);
    fprintf(stderr,"[TETRA CMCE D-ALERT] CC=%d call=%u setup_timeout=%u queued=%u\n",cc,(unsigned)id,timeout,queued);
    if(state){state->tetra_call_active=1; state->tetra_call_id=id; state->tetra_d_alert_call_id=id;
      state->tetra_d_alert_timeout=timeout; state->tetra_d_alert_reserved=reserved; state->tetra_d_alert_duplex=duplex;
      state->tetra_d_alert_queued=queued; state->tetra_d_alert_valid=1;}
}
static void parse_cmce_d_call_proceeding(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<24){fprintf(stderr,"[TETRA CMCE D-CALL-PROCEEDING] CC=%d (too short: %d bits)\n",cc,nbits);return;}
    int off=5; uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t timeout=(uint8_t)mle_bits_to_uint(bits,off,3); off+=3;
    uint8_t hook=(uint8_t)mle_bits_to_uint(bits,off++,1); uint8_t duplex=(uint8_t)mle_bits_to_uint(bits,off++,1);
    fprintf(stderr,"[TETRA CMCE D-CALL-PROCEEDING] CC=%d call=%u setup_timeout=%u\n",cc,(unsigned)id,timeout);
    if(state){state->tetra_call_active=1; state->tetra_call_id=id; state->tetra_d_call_proc_call_id=id;
      state->tetra_d_call_proc_timeout=timeout; state->tetra_d_call_proc_hook=hook;
      state->tetra_d_call_proc_duplex=duplex; state->tetra_d_call_proc_valid=1;}
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
    if (off+16>nbits) return;
    uint16_t status=(uint16_t)mle_bits_to_uint(bits,off,16);
    fprintf(stderr,"[TETRA CMCE D-STATUS] CC=%d src_SSI=%u status=0x%04X\n",cc,src_ssi,status);
    if (state) {
        state->tetra_sds_src=src_ssi; state->tetra_sds_status=status;
        state->tetra_sds_status_log[state->tetra_sds_status_log_head&3u]=status;
        state->tetra_sds_status_log_head=(uint8_t)((state->tetra_sds_status_log_head+1u)&3u);
    }
}

/* -----------------------------------------------------------------------
 * CMCE D-CONNECT ACKNOWLEDGE, table 14.8.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_connect_ack(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<26){fprintf(stderr,"[TETRA CMCE D-CONNECT-ACK] CC=%d (too short: %d bits)\n",cc,nbits);return;}
    int off=5; uint16_t id=(uint16_t)mle_bits_to_uint(bits,off,14); off+=14;
    uint8_t timeout=(uint8_t)mle_bits_to_uint(bits,off,4); off+=4;
    uint8_t grant=(uint8_t)mle_bits_to_uint(bits,off,2); off+=2;
    uint8_t permission=(uint8_t)mle_bits_to_uint(bits,off,1);
    fprintf(stderr,"[TETRA CMCE D-CONNECT-ACK] CC=%d call=%u timeout=%u grant=%u\n",cc,(unsigned)id,timeout,grant);
    if(state){state->tetra_call_active=1;state->tetra_call_id=id;state->tetra_call_timeout=timeout;
      state->tetra_d_connect_ack_call_id=id;state->tetra_d_connect_ack_tx_grant=grant;
      state->tetra_d_connect_ack_tx_permission=permission;state->tetra_d_connect_ack_valid=1;}
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

    int mandatory = pdu_type == TETRA_CMCE_D_TX_CONTINUE ? 21
                  : pdu_type == TETRA_CMCE_D_TX_INTERRUPT ? 24 : 20;
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

    uint32_t notification = 0;
    if (off < nbits) {
        uint32_t present = mle_bits_to_uint(bits, off, 1); off += 1;
        if (present) {
            if (off + 6 > nbits) {
                fprintf(stderr, "[TETRA CMCE %s] CC=%d (truncated optional IE: %d bits)\n",
                        name, cc, nbits);
                return;
            }
            notification = mle_bits_to_uint(bits, off, 6);
        }
    }

    fprintf(stderr, "[TETRA CMCE %s] CC=%d  call=%u  request=%u  notif=%u",
            name, cc, call_id, request_perm, notification);
    if (pdu_type == TETRA_CMCE_D_TX_CONTINUE)
        fprintf(stderr, "  continue=%u", continue_value);
    if (pdu_type == TETRA_CMCE_D_TX_INTERRUPT)
        fprintf(stderr, "  grant=%u  enc=%u", grant, encryption);
    fprintf(stderr, "\n");

    if (state) {
        state->tetra_tx_event_call_id      = (uint16_t)call_id;
        state->tetra_tx_event_notification = (uint8_t)notification;
        state->tetra_tx_event_request_perm = (uint8_t)request_perm;
        state->tetra_tx_event_continue     = (uint8_t)continue_value;
        state->tetra_tx_event_grant        = (uint8_t)grant;
        state->tetra_tx_event_encryption   = (uint8_t)encryption;

        switch (pdu_type) {
        case TETRA_CMCE_D_TX_CONTINUE:  state->tetra_tx_continue    = 1; break;
        case TETRA_CMCE_D_TX_INTERRUPT: state->tetra_tx_interrupted = 1; break;
        case TETRA_CMCE_D_TX_WAIT:      state->tetra_tx_wait        = 1; break;
        default: break;
        }
    }
}

/* -----------------------------------------------------------------------
 * CMCE D-INFO, ETSI EN 300 392-2 table 14.11.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_info(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<21){fprintf(stderr,"[TETRA CMCE D-INFO] CC=%d (too short: %d bits)\n",cc,nbits);return;}
    uint16_t id=(uint16_t)mle_bits_to_uint(bits,5,14);
    uint8_t reset=(uint8_t)mle_bits_to_uint(bits,19,1);
    uint8_t poll=(uint8_t)mle_bits_to_uint(bits,20,1);
    fprintf(stderr,"[TETRA CMCE D-INFO] CC=%d call=%u reset=%u poll=%u\n",cc,(unsigned)id,reset,poll);
    if(state){state->tetra_d_info_call_id=id;state->tetra_d_info_call_timeout=reset;
      state->tetra_d_info_notification=poll;state->tetra_d_info_valid=1;}
}

/* -----------------------------------------------------------------------
 * CMCE D-CALL-RESTORE, ETSI EN 300 392-2 table 14.6.
 * ----------------------------------------------------------------------- */
static void parse_cmce_d_call_restore(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<23){fprintf(stderr,"[TETRA CMCE D-CALL-RESTORE] CC=%d (too short: %d bits)\n",cc,nbits);return;}
    uint16_t id=(uint16_t)mle_bits_to_uint(bits,5,14);
    uint8_t grant=(uint8_t)mle_bits_to_uint(bits,19,2);
    uint8_t permission=(uint8_t)mle_bits_to_uint(bits,21,1);
    uint8_t reset=(uint8_t)mle_bits_to_uint(bits,22,1);
    fprintf(stderr,"[TETRA CMCE D-CALL-RESTORE] CC=%d call=%u grant=%u request=%u reset=%u\n",cc,(unsigned)id,grant,permission,reset);
    if(state){state->tetra_call_active=1;state->tetra_call_id=id;state->tetra_call_restore_id=id;
      state->tetra_call_restore_grant=grant;state->tetra_call_restore_permission=permission;
      state->tetra_call_restore_reset=reset;state->tetra_call_restore_valid=1;}
}

/* CMCE FUNCTION NOT SUPPORTED, table 14.33. */
static void parse_cmce_function_not_supported(const uint8_t *bits,int nbits,int cc,dsd_state *state)
{
    if(nbits<19){fprintf(stderr,"[TETRA CMCE FUNCTION-NOT-SUPPORTED] CC=%d (too short: %d bits)\n",cc,nbits);return;}
    int off=5; uint8_t rejected=(uint8_t)mle_bits_to_uint(bits,off,5);off+=5;
    uint8_t id_present=(uint8_t)mle_bits_to_uint(bits,off++,1);uint16_t id=0;
    if(id_present){if(off+14+8>nbits)return;id=(uint16_t)mle_bits_to_uint(bits,off,14);off+=14;}
    if(off+8>nbits)return; uint8_t pointer=(uint8_t)mle_bits_to_uint(bits,off,8);off+=8;
    uint8_t extract_len=0;
    if(pointer){if(off+8>nbits)return;extract_len=(uint8_t)mle_bits_to_uint(bits,off,8);off+=8;if(off+extract_len>nbits)return;}
    fprintf(stderr,"[TETRA CMCE FUNCTION-NOT-SUPPORTED] CC=%d rejected=%u call=%u pointer=%u extract=%u\n",cc,rejected,(unsigned)id,pointer,extract_len);
    if(state){state->tetra_cmce_fns_rejected_pdu=rejected;state->tetra_cmce_fns_call_id_present=id_present;
      state->tetra_cmce_fns_call_id=id;state->tetra_cmce_fns_pointer=pointer;
      state->tetra_cmce_fns_extract_bits=extract_len;state->tetra_cmce_fns_valid=1;}
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

    fprintf(stderr, "[TETRA CMCE D-SDS-DATA] CC=%d src_SSI=%u CPTI=%u SDTI=%u data_bits=%d\n",
            cc, src_ssi, cpti, sdti, payload_bits);
    if (state) {
        /* Commit only after the complete declared payload is available. */
        state->tetra_sds_text_len = 0;
        state->tetra_sds_text[0] = '\0';
        state->tetra_sds_text_unicode = 0;
        state->tetra_sds_short_valid = 0;
        state->tetra_sds_src = src_ssi;
        state->tetra_cmce_sds_data_type = (uint8_t)sdti;
        state->tetra_sds_last_cc = (int8_t)cc;
        if (sdti == 0) {
            state->tetra_sds_short_data = (uint16_t)mle_bits_to_uint(bits, off, 16);
            state->tetra_sds_short_src = src_ssi;
            state->tetra_sds_short_valid = 1;
        }
    }
    return;

truncated:
    fprintf(stderr, "[TETRA CMCE D-SDS-DATA] CC=%d src_SSI=%u (truncated: %d bits)\n",
            cc, src_ssi, nbits);
}

/* CMCE D-FACILITY, ETSI EN 300 392-2 table 14.10 and annex E.1.2.
 * After the PDU type, a four-bit count introduces independently encoded
 * SS-PDUs. Each has an 11-bit length followed by that many content bits; the
 * collection ends with the type-2 optional-elements presence bit. */
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

/* -----------------------------------------------------------------------
 * Public entry point: tetra_mle_dispatch()
 * ----------------------------------------------------------------------- */
void tetra_mle_dispatch(const uint8_t *bits, int nbits,
                        int cc, const dsd_opts *opts, dsd_state *state)
{
    if (!bits || nbits < 5) {
        /* Need at least 5-bit MLE type */
        return;
    }

    uint32_t mle_type = mle_bits_to_uint(bits, 0, 5);

    if (mle_type == TETRA_MLE_C_PLANE_DATA) {
        /* ----------------------------------------------------------------
         * C-PLANE-DATA: carries CMCE or MM PDU
         * ---------------------------------------------------------------- */
        if (nbits < 9) return; /* need 4-bit PD field as well */
        uint32_t pd = mle_bits_to_uint(bits, 5, 4);

        if (pd == TETRA_MLE_PD_CMCE) {
            /* CMCE PDU starts at bit 9 */
            tetra_cmce_parse(bits + 9, nbits - 9, cc, opts, state);
        } else if (pd == TETRA_MLE_PD_MM) {
            tetra_mm_dispatch(bits + 9, nbits - 9, cc, opts, state);
        } else if (pd == TETRA_MLE_PD_SNDCP) {
            /* ────────────────────────────────────────────────────────────
             * SNDCP — Sub-Network Dependent Convergence Protocol (PD=8)
             * ETSI EN 300 392-3 §11 / EN 300 392-2 Table 21.2
             *
             * Minimal parser: extract NSAPI (4 bits) and SNDCP PDU type
             * (4 bits) from the first octet, log, and save to state.
             * ──────────────────────────────────────────────────────────── */
            const uint8_t *sn = bits + 9;
            int sn_nbits = nbits - 9;
            if (sn_nbits >= 8) {
                uint32_t nsapi    = mle_bits_to_uint(sn, 0, 4);
                uint32_t sn_type  = mle_bits_to_uint(sn, 4, 4);
                fprintf(stderr,
                        "[TETRA SNDCP] CC=%d  NSAPI=%u  pdu_type=%u  (%d bits)\n",
                        cc, nsapi, sn_type, sn_nbits);
                if (state) {
                    state->tetra_sndcp_nsapi    = (uint8_t)nsapi;
                    state->tetra_sndcp_pdu_type = (uint8_t)sn_type;
                    state->tetra_sndcp_nbits    = (uint16_t)sn_nbits;
                    state->tetra_sndcp_valid    = 1;
                }
            } else {
                fprintf(stderr,
                        "[TETRA SNDCP] CC=%d  (%d bits, too short)\n",
                        cc, sn_nbits);
            }
        } else {
            if (opts && opts->errorbars) {
                fprintf(stderr, "[TETRA MLE C-PLANE] CC=%d  PD=%u  (%d bits)\n",
                        cc, pd, nbits - 9);
            }
        }
    } else if (mle_type == TETRA_MLE_D_NWRK_BROADCAST) {
        /* Carried in MAC-BROADCAST bcast_type 3 (TETRA_MAC_BC_NWRK_BCAST). */
        parse_mle_d_nwrk_broadcast(bits, nbits, cc, state);
    } else if (mle_type == TETRA_MLE_D_NWRK_BCAST_EXT) {
        /* ----------------------------------------------------------------
         * D-NWRK-BROADCAST-EXTENSION  (MLE type 1)
         * ETSI EN 300 392-2 §21.6.2
         *
         * Bits  0-4 : MLE type = 1
         * Bits  5-18: Location Area (14 bits)
         * further optional IEs follow
         * ---------------------------------------------------------------- */
        if (nbits >= 19) {
            uint32_t la = mle_bits_to_uint(bits, 5, 14);
            fprintf(stderr, "[TETRA MLE D-NWRK-BROADCAST-EXT] CC=%d  LA=%u  (%d bits)\n",
                    cc, la, nbits);
            if (state) {
                state->tetra_nwrk_bcast_ext_la    = (uint16_t)la;
                state->tetra_nwrk_bcast_ext_known = 1;
            }
        } else {
            fprintf(stderr, "[TETRA MLE D-NWRK-BROADCAST-EXT] CC=%d  (%d bits, too short)\n",
                    cc, nbits);
        }
    } else if (mle_type == TETRA_MLE_D_RESTORE_ACK) {
        if (nbits >= 19) {
            uint16_t la = (uint16_t)mle_bits_to_uint(bits, 5, 14);
            fprintf(stderr, "[TETRA MLE D-RESTORE-ACK] CC=%d  LA=%u  (%d bits)\n",
                    cc, la, nbits);
            if (state) {
                state->tetra_restore_ack          = 1;
                state->tetra_restore_ack_la       = la;
                state->tetra_restore_ack_la_valid = 1;
            }
        } else {
            fprintf(stderr, "[TETRA MLE D-RESTORE-ACK] CC=%d  (%d bits)\n",
                    cc, nbits);
        }
    } else if (mle_type == TETRA_MLE_D_RESTORE_RESPONSE) {
        uint8_t result = 0;
        if (nbits >= 6) {
            result = (uint8_t)mle_bits_to_uint(bits, 5, 1);
            fprintf(stderr, "[TETRA MLE D-RESTORE-RESPONSE] CC=%d  result=%u  (%d bits)\n",
                    cc, result, nbits);
        } else {
            fprintf(stderr, "[TETRA MLE D-RESTORE-RESPONSE] CC=%d  (%d bits)\n",
                    cc, nbits);
        }
        if (state && nbits >= 6) {
            state->tetra_restore_response              = 1;
            state->tetra_restore_response_result       = result;
            state->tetra_restore_response_result_valid = 1;
        }
    } else {
        /* Unknown / future MLE types. */
        if (opts && opts->errorbars) {
            fprintf(stderr, "[TETRA MLE] CC=%d  mle_type=%u  (%d bits)\n",
                    cc, mle_type, nbits);
        }
    }
}
