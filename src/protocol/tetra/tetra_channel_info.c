// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA channel-info summary formatter.  Phase 32.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/tetra/tetra_channel_info.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>

#include <stdio.h>
#include <string.h>
#include "tetra_state.h"

void
tetra_channel_info_fmt(const dsd_state* state, char* buf, size_t len) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!buf || len == 0) {
        return;
    }

    if (!ts) {
        snprintf(buf, len, "?");
        return;
    }

    /* Build the string in pieces and append */
    char tmp[1024];
    int off = 0;

    /* Network identity */
    if (ts->net_known) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "MCC:%u MNC:%u CC:%u", (unsigned)ts->mcc,
                        (unsigned)ts->mnc, (unsigned)ts->colour);
    } else {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "MCC:? MNC:? CC:?");
    }

    /* DL carrier */
    if (ts->dl_carrier_hz > 0) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " DL:%.3fMHz", (double)ts->dl_carrier_hz / 1.0e6);
    }

    if (ts->mle_ca_neighbor_count_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " nbr:%u", (unsigned)ts->mle_ca_neighbor_count);
    }

    if (ts->mm_group_identity_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " groups:%u", (unsigned)ts->mm_group_entry_count);
    }

    /* Talkgroup and source */
    if (ts->call_active) {
        if (ts->gssi) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " TG:%u", (unsigned)ts->gssi);
        }
        if (ts->calling_ssi) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " SRC:%u", (unsigned)ts->calling_ssi);
        }
    }

    if (ts->tx_granted_valid && ts->tx_granted_ssi) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " PTT:%u", (unsigned)ts->tx_granted_ssi);
    } else if (ts->tx_event_party_ssi_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " TXSRC:%u", (unsigned)ts->tx_event_party_ssi);
    }

    /* Encryption mode */
    off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " enc:%s", tetra_enc_mode_name(ts->enc_mode));

    /* Phase 75: TDMA timestamp */
    if (ts->tdma_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " TN:%u FN:%u MN:%u", (unsigned)ts->tn, (unsigned)ts->fn,
                        (unsigned)ts->mn);
    }

    /* Phase 75: Voice channel frequency */
    if (ts->vc_freq_hz > 0) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " VC:%.3fMHz", (double)ts->vc_freq_hz / 1.0e6);
    }

    /* Phase 75: Last SDS text */
    if (ts->sds_text_len > 0 && ts->sds_text[0] != '\0') {
        if (ts->sds_src) {
            off +=
                snprintf(tmp + off, sizeof(tmp) - (size_t)off, " SDS(%u):\"%s\"", (unsigned)ts->sds_src, ts->sds_text);
        } else {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " SDS:\"%s\"", ts->sds_text);
        }
    }

    if (ts->sds_forward_valid) {
        switch (ts->sds_forward_type) {
            case 0:
                off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " fwd:SNA:%u", (unsigned)ts->sds_forward_sna);
                break;
            case 1:
                off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " fwd:SSI:%u", (unsigned)ts->sds_forward_ssi);
                break;
            case 2:
                off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " fwd:TSI:%u/%06X", (unsigned)ts->sds_forward_ssi,
                                (unsigned)ts->sds_forward_extension);
                break;
            case 3:
                off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " fwd:EXT:%s", ts->sds_forward_external);
                break;
            case 7: off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " fwd:none"); break;
            default: break;
        }
        if (ts->sds_storage_forward) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " vp:%u", (unsigned)ts->sds_validity_period);
        }
    }

    if (ts->sds_concat_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " cat:%03X:%u/%u%s", (unsigned)ts->sds_concat_ref,
                        (unsigned)ts->sds_concat_received, (unsigned)ts->sds_concat_total,
                        ts->sds_concat_complete    ? ":done"
                        : ts->sds_concat_duplicate ? ":dup"
                                                   : "");
    }

    /* Phase 84: Release cause (Phase 78 field) */
    if (ts->cmce_release_cause_type) {
        static const char* const cause_names[] = {"normal",     "abnormal",         "pre-emption",      "congestion",
                                                  "go-to-air",  "resource-unavail", "temp-fail",        "not-allowed",
                                                  "no-answer",  "not-reachable",    "area-not-allowed", "reserved11",
                                                  "reserved12", "reserved13",       "reserved14",       "unknown"};
        unsigned ci = ts->cmce_release_cause & 0x0Fu;
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " rel:%s", cause_names[ci]);
    }

    /* Phase 84: Access mode (Phase 77 field) */
    if (ts->access_common_flag) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " acc:common");
    }

    /* Phase 84: MM addr type (Phase 79 field) */
    if (ts->mm_addr_type) {
        static const char* const addr_names[] = {"SSI",       "event-label", "USSI",      "SMI",
                                                 "SSI+event", "SSI+usage",   "SMI+event", "reserved7"};
        unsigned ai = ts->mm_addr_type & 0x07u;
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " mm_addr:%s", addr_names[ai]);
    }

    /* Phase 86: SDS status */
    if (ts->sds_status) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " sts:%u", (unsigned)ts->sds_status);
    }

    /* Phase 86: SDS short data */
    if (ts->sds_short_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " sds_s:%u src:%u", (unsigned)ts->sds_short_data,
                        (unsigned)ts->sds_short_src);
    }

    /* Phase 86: SDS delivery report */
    if (ts->sds_report_valid) {
        static const char* const report_classes[] = {"ok",      "retry",    "fail",     "flow",
                                                     "control", "reserved", "reserved", "reserved"};
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " rpt:%s/%02X ref:%u",
                        report_classes[ts->sds_report_cause >> 5], (unsigned)ts->sds_report_cause,
                        (unsigned)ts->sds_report_msg_ref);
    }

    /* Phase 86: Facility */
    if (ts->facility_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " fac:%u", (unsigned)ts->facility_type);
    }

    /* Phase 86: SDS ACK */
    if (ts->sds_ack_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " sds_ack:ref%u", (unsigned)ts->sds_ack_msg_ref);
    }

    /* Phase 86: SDS short report */
    if (ts->sds_short_report_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " srpt:%u", (unsigned)ts->sds_short_report_result);
    }

    /* Phase 86: CCK info */
    if (ts->cck_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " CCK:%u", (unsigned)ts->cck_id);
    }

    /* Phase 86: Call timeout/slots */
    if (ts->call_active) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " tmo:%u slot:%u", (unsigned)ts->call_timeout,
                        (unsigned)ts->call_slots);
    }

    /* Phase 86: BSCH count */
    if (ts->bsch_count > 0) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " bsch:%u", (unsigned)ts->bsch_count);
    }

    /* Phase 86: Decode quality counters */
    if (ts->decode_ok > 0 || ts->decode_errors > 0) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " ok:%u err:%u", (unsigned)ts->decode_ok,
                        (unsigned)ts->decode_errors);
    }

    /* Phase 86: Frame counters */
    if (ts->frames_total > 0) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " fr:%u", (unsigned)ts->frames_total);
    }

    /* Phase 86: Floor-control events */
    if (ts->tx_continue || ts->tx_interrupted || ts->tx_wait || ts->tx_timed_out) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " tx:");
        if (ts->tx_continue) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "C");
        }
        if (ts->tx_interrupted) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "I");
        }
        if (ts->tx_wait) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "W");
        }
        if (ts->tx_timed_out) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "T");
        }
    }

    if (ts->mm_status_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " mm:%u/%s", (unsigned)ts->mm_status_code,
                        tetra_mm_status_name(ts->mm_status_code));
    }

    /* Phase 86: SNDCP info */
    if (ts->sndcp_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " sndcp:%u/%u", (unsigned)ts->sndcp_nsapi,
                        (unsigned)ts->sndcp_pdu_type);
    }

    /* D-INFO: keep the compact mandatory flags, then append only optionals
     * that were actually present on air. */
    if (ts->d_info_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " dinfo:id%u/t%u", (unsigned)ts->d_info_call_id,
                        (unsigned)ts->d_info_call_timeout);
        if (ts->d_info_new_call_id_valid) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "->%u", (unsigned)ts->d_info_new_call_id);
        }
        if (ts->d_info_timeout_valid) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "/to%u", (unsigned)ts->d_info_timeout);
        }
        if (ts->d_info_status_valid) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "/st%u", (unsigned)ts->d_info_status);
        }
        if (ts->d_info_notification_indicator_valid) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "/n%u", (unsigned)ts->d_info_notification_indicator);
        }
    }

    if (ts->call_restore_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " restore:id%u/g%u", (unsigned)ts->call_restore_id,
                        (unsigned)ts->call_restore_grant);
        if (ts->call_restore_new_id_valid) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "->%u", (unsigned)ts->call_restore_new_id);
        }
        if (ts->call_restore_status_valid) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "/st%u", (unsigned)ts->call_restore_status);
        }
    }

    snprintf(buf, len, "%s", tmp);
}
