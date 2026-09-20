// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA channel-info summary formatter.  Phase 32.
 */

#include <dsd-neo/protocol/tetra/tetra_channel_info.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/core/state.h>

#include <stdio.h>
#include <string.h>

void
tetra_channel_info_fmt(const dsd_state *state, char *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    if (!state) {
        snprintf(buf, len, "?");
        return;
    }

    /* Build the string in pieces and append */
    char tmp[1024];
    int  off = 0;

    /* Network identity */
    if (state->tetra_net_known)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        "MCC:%u MNC:%u CC:%u",
                        (unsigned)state->tetra_mcc,
                        (unsigned)state->tetra_mnc,
                        (unsigned)state->tetra_colour);
    else
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "MCC:? MNC:? CC:?");

    /* DL carrier */
    if (state->tetra_dl_carrier_hz > 0)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " DL:%.3fMHz",
                        (double)state->tetra_dl_carrier_hz / 1.0e6);

    if (state->tetra_mle_ca_neighbor_count_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " nbr:%u", (unsigned)state->tetra_mle_ca_neighbor_count);

    if (state->tetra_mm_group_identity_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " groups:%u", (unsigned)state->tetra_mm_group_entry_count);

    /* Talkgroup and source */
    if (state->tetra_call_active) {
        if (state->tetra_gssi)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            " TG:%u", (unsigned)state->tetra_gssi);
        if (state->tetra_calling_ssi)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            " SRC:%u", (unsigned)state->tetra_calling_ssi);
    }

    if (state->tetra_tx_granted_valid && state->tetra_tx_granted_ssi)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " PTT:%u", (unsigned)state->tetra_tx_granted_ssi);
    else if (state->tetra_tx_event_party_ssi_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " TXSRC:%u", (unsigned)state->tetra_tx_event_party_ssi);

    /* Encryption mode */
    off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                    " enc:%s", tetra_enc_mode_name(state->tetra_enc_mode));

    /* Phase 75: TDMA timestamp */
    if (state->tetra_tdma_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " TN:%u FN:%u MN:%u",
                        (unsigned)state->tetra_tn,
                        (unsigned)state->tetra_fn,
                        (unsigned)state->tetra_mn);

    /* Phase 75: Voice channel frequency */
    if (state->tetra_vc_freq_hz > 0)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " VC:%.3fMHz",
                        (double)state->tetra_vc_freq_hz / 1.0e6);

    /* Phase 75: Last SDS text */
    if (state->tetra_sds_text_len > 0 && state->tetra_sds_text[0] != '\0') {
        if (state->tetra_sds_src)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            " SDS(%u):\"%s\"",
                            (unsigned)state->tetra_sds_src,
                            state->tetra_sds_text);
        else
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            " SDS:\"%s\"", state->tetra_sds_text);
    }

    if (state->tetra_sds_forward_valid) {
        switch (state->tetra_sds_forward_type) {
        case 0:
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            " fwd:SNA:%u", (unsigned)state->tetra_sds_forward_sna);
            break;
        case 1:
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            " fwd:SSI:%u", (unsigned)state->tetra_sds_forward_ssi);
            break;
        case 2:
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            " fwd:TSI:%u/%06X",
                            (unsigned)state->tetra_sds_forward_ssi,
                            (unsigned)state->tetra_sds_forward_extension);
            break;
        case 3:
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            " fwd:EXT:%s", state->tetra_sds_forward_external);
            break;
        case 7:
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " fwd:none");
            break;
        default:
            break;
        }
        if (state->tetra_sds_storage_forward)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            " vp:%u", (unsigned)state->tetra_sds_validity_period);
    }

    if (state->tetra_sds_concat_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " cat:%03X:%u/%u%s",
                        (unsigned)state->tetra_sds_concat_ref,
                        (unsigned)state->tetra_sds_concat_received,
                        (unsigned)state->tetra_sds_concat_total,
                        state->tetra_sds_concat_complete ? ":done" :
                        state->tetra_sds_concat_duplicate ? ":dup" : "");

    /* Phase 84: Release cause (Phase 78 field) */
    if (state->tetra_cmce_release_cause_type) {
        static const char *const cause_names[] = {
            "normal", "abnormal", "pre-emption", "congestion",
            "go-to-air", "resource-unavail", "temp-fail", "not-allowed",
            "no-answer", "not-reachable", "area-not-allowed", "reserved11",
            "reserved12", "reserved13", "reserved14", "unknown"
        };
        unsigned ci = state->tetra_cmce_release_cause & 0x0Fu;
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " rel:%s", cause_names[ci]);
    }

    /* Phase 84: Access mode (Phase 77 field) */
    if (state->tetra_access_common_flag)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " acc:common");

    /* Phase 84: MM addr type (Phase 79 field) */
    if (state->tetra_mm_addr_type) {
        static const char *const addr_names[] = {
            "SSI", "event-label", "USSI", "SMI",
            "SSI+event", "SSI+usage", "SMI+event", "reserved7"
        };
        unsigned ai = state->tetra_mm_addr_type & 0x07u;
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " mm_addr:%s", addr_names[ai]);
    }

    /* Phase 86: SDS status */
    if (state->tetra_sds_status)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " sts:%u", (unsigned)state->tetra_sds_status);

    /* Phase 86: SDS short data */
    if (state->tetra_sds_short_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " sds_s:%u src:%u",
                        (unsigned)state->tetra_sds_short_data,
                        (unsigned)state->tetra_sds_short_src);

    /* Phase 86: SDS delivery report */
    if (state->tetra_sds_report_valid) {
        static const char *const report_classes[] = {
            "ok", "retry", "fail", "flow", "control", "reserved", "reserved", "reserved"
        };
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " rpt:%s/%02X ref:%u",
                        report_classes[state->tetra_sds_report_cause >> 5],
                        (unsigned)state->tetra_sds_report_cause,
                        (unsigned)state->tetra_sds_report_msg_ref);
    }

    /* Phase 86: Facility */
    if (state->tetra_facility_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " fac:%u", (unsigned)state->tetra_facility_type);

    /* Phase 86: SDS ACK */
    if (state->tetra_sds_ack_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " sds_ack:ref%u", (unsigned)state->tetra_sds_ack_msg_ref);

    /* Phase 86: SDS short report */
    if (state->tetra_sds_short_report_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " srpt:%u", (unsigned)state->tetra_sds_short_report_result);

    /* Phase 86: CCK info */
    if (state->tetra_cck_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " CCK:%u", (unsigned)state->tetra_cck_id);

    /* Phase 86: Call timeout/slots */
    if (state->tetra_call_active) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " tmo:%u slot:%u",
                        (unsigned)state->tetra_call_timeout,
                        (unsigned)state->tetra_call_slots);
    }

    /* Phase 86: BSCH count */
    if (state->tetra_bsch_count > 0)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " bsch:%u", (unsigned)state->tetra_bsch_count);

    /* Phase 86: Decode quality counters */
    if (state->tetra_decode_ok > 0 || state->tetra_decode_errors > 0)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " ok:%u err:%u",
                        (unsigned)state->tetra_decode_ok,
                        (unsigned)state->tetra_decode_errors);

    /* Phase 86: Frame counters */
    if (state->tetra_frames_total > 0)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " fr:%u", (unsigned)state->tetra_frames_total);

    /* Phase 86: Floor-control events */
    if (state->tetra_tx_continue || state->tetra_tx_interrupted ||
        state->tetra_tx_wait || state->tetra_tx_timed_out) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " tx:");
        if (state->tetra_tx_continue)    off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "C");
        if (state->tetra_tx_interrupted) off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "I");
        if (state->tetra_tx_wait)        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "W");
        if (state->tetra_tx_timed_out)   off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "T");
    }

    if (state->tetra_mm_status_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " mm:%u/%s",
                        (unsigned)state->tetra_mm_status_code,
                        tetra_mm_status_name(state->tetra_mm_status_code));

    /* Phase 86: SNDCP info */
    if (state->tetra_sndcp_valid)
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " sndcp:%u/%u",
                        (unsigned)state->tetra_sndcp_nsapi,
                        (unsigned)state->tetra_sndcp_pdu_type);

    /* D-INFO: keep the compact mandatory flags, then append only optionals
     * that were actually present on air. */
    if (state->tetra_d_info_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                        " dinfo:id%u/t%u",
                        (unsigned)state->tetra_d_info_call_id,
                        (unsigned)state->tetra_d_info_call_timeout);
        if (state->tetra_d_info_new_call_id_valid)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "->%u",
                            (unsigned)state->tetra_d_info_new_call_id);
        if (state->tetra_d_info_timeout_valid)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "/to%u",
                            (unsigned)state->tetra_d_info_timeout);
        if (state->tetra_d_info_status_valid)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "/st%u",
                            (unsigned)state->tetra_d_info_status);
        if (state->tetra_d_info_notification_indicator_valid)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "/n%u",
                            (unsigned)state->tetra_d_info_notification_indicator);
    }

    if (state->tetra_call_restore_valid) {
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, " restore:id%u/g%u",
                        (unsigned)state->tetra_call_restore_id,
                        (unsigned)state->tetra_call_restore_grant);
        if (state->tetra_call_restore_new_id_valid)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "->%u",
                            (unsigned)state->tetra_call_restore_new_id);
        if (state->tetra_call_restore_status_valid)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "/st%u",
                            (unsigned)state->tetra_call_restore_status);
    }

    snprintf(buf, len, "%s", tmp);
}
