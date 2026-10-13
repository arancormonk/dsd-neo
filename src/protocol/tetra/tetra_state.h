// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA decoder state, held in the TETRA state extension slot (DSD_STATE_EXT_PROTO_TETRA).
 *
 * Decoder-thread private. Frontends never read it; they get TETRA data through published views.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_STATE_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_STATE_H_

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tetra_state {
    /* uplink: 1 with --tetra-uplink. Normal bursts are then NUBs
     * (EN 300 392-2 table 9.5): the same training sequence as a downlink NDB,
     * but block 1 sits directly against it (no AACH). Scrambling stays the
     * TMO colour code, from a BSCH or from --tetra-net mcc,mnc,colour.
     * tetra_cb: this sync is a control uplink burst (extended training). */
    uint8_t uplink;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA network identity — populated when a BSCH (SB1 block) is decoded.
     *
     * net_known:  1 once MCC/MNC/colour have been parsed from BSCH.
     * mcc:        10-bit Mobile Country Code (valid bits 9:0).
     * mnc:        14-bit Mobile Network Code (valid bits 13:0).
     * colour:     6-bit full colour code from BSCH (CB field carries the
     *                   lower 2 bits of this value as the 2-bit CC).
     * lfsr_seed:  Cached scrambling seed = tetra_compute_scramb_seed(mcc,mnc,colour).
     *                   When net_known==0 this defaults to 3 (all-zero seed).
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t net_known;
    uint16_t mcc;
    uint16_t mnc;
    uint8_t colour;
    uint32_t lfsr_seed;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA Direct Mode Operation (DMO, EN 300 396-2/-3).
     *
     * dmo:           1 once a DSB (SCH/S with a DMO system code) has been
     *                      decoded; selects DNB block layout (no AACH) and the
     *                      DM colour code scrambler. Cleared by a TMO BSCH.
     * dmo_dcc_valid: 1 when dmo_dcc_seed was derived from a
     *                      DM-SETUP / DM-CONNECT ACK / DM-OCCUPIED DMAC-SYNC.
     * dmo_dcc_seed:  LFSR seed for SCH/F, STCH and TCH.
     * dmo_since_sync: DNBs processed since the last DSB (staleness).
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t dmo;
    uint8_t dmo_dcc_valid;
    uint8_t dmo_slot;
    uint8_t dmo_frame;
    uint8_t dmo_comm_type;
    uint8_t dmo_ab_usage;
    uint8_t dmo_enc_state;
    uint8_t dmo_msg_type;
    uint16_t dmo_since_sync;
    uint32_t dmo_dcc_seed;
    uint32_t dmo_mni;
    uint32_t dmo_src;
    uint32_t dmo_dst;
    uint32_t dmo_sync_count;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA network parameters — populated by MAC-BROADCAST/SYSINFO (MLE).
     *
     * sysinfo_known:    1 once a valid SYSINFO PDU has been decoded.
     * nwrk_bcast_known: 1 once a D-NWRK-BROADCAST (MLE type 0) PDU
     *                          has been received via MAC-BROADCAST bcast_type 3.
     * la:               14-bit Location Area number (last received from
     *                          either SYSINFO/MLE or D-NWRK-BROADCAST).
     * bs_service_det:   12-bit BS service details bitmask.
     * subscr_class:     16-bit subscriber class mask.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t sysinfo_known;
    uint8_t nwrk_bcast_known;
    uint16_t la;
    uint16_t bs_service_det;
    uint16_t subscr_class;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA call tracking — updated by MAC-RESOURCE on each NDB block.
     *
     * ssi_valid:  1 when active_ssi holds a valid SSI.
     * active_ssi: 24-bit Short Subscriber Identity of the current call.
     * enc_mode:   2-bit encryption mode (TETRA_ENC_MODE_* constants).
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t ssi_valid;
    uint32_t active_ssi;
    uint8_t enc_mode;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA MLE / CMCE call-event state — updated by tetra_mle_dispatch().
     *
     * call_active:  1 while a call is in progress (set on D-SETUP or
     *                     D-CONNECT, cleared on D-RELEASE / D-DISCONNECT).
     * call_type:    3-bit circuit-mode type from D-SETUP basic service.
     * calling_ssi:  24-bit SSI of the calling party extracted from the
     *                     CMCE D-SETUP "Calling party" optional IE.
     * gssi:         24-bit Group SSI (talkgroup ID) from the MAC-RESOURCE
     *                     address. Set for group/multipoint calls and cleared
     *                     for point-to-point calls.
     * call_id:      14-bit call identifier from CMCE D-SETUP.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t call_active;
    uint8_t call_type;
    uint32_t calling_ssi;
    uint32_t gssi;
    uint16_t call_id;
    uint32_t cmce_call_generation; /* increments after complete call-establishment state */
    uint8_t vocoder_status;        /* tetra_vocoder_status_e */
    uint32_t vocoder_frames;
    uint32_t vocoder_errors;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA floor-control state — updated by CMCE D-TX-GRANTED / D-TX-CEASED.
     *
     * tx_granted_valid: 1 while a transmit grant is in force.
     * tx_granted_ssi:   24-bit SSI of the currently-speaking party
     *                          (from the D-TX-GRANTED optional IE).
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t tx_granted_valid;
    uint32_t tx_granted_ssi;

    uint8_t ms_enabled;
    uint8_t mm_status_code;
    uint8_t mm_status_valid;
    /* EN 300 392-7 tables A.31/A.32.  A complete command replaces this
     * snapshot; truncated identity or authentication data publishes nothing. */
    uint8_t mm_enable_disable_valid;
    uint8_t mm_enable_disable_is_enable;
    uint8_t mm_enable_disable_intent;
    uint8_t mm_disable_permanent;
    uint8_t mm_enable_disable_equipment;
    uint64_t mm_enable_disable_tei;
    uint8_t mm_enable_disable_subscription;
    uint16_t mm_enable_disable_mcc;
    uint16_t mm_enable_disable_mnc;
    uint32_t mm_enable_disable_ssi;
    uint8_t mm_enable_disable_auth_valid;
    uint8_t mm_enable_disable_auth_challenge[20];
    /* EN 300 392-7 table A.26 D-CK CHANGE DEMAND.  Arrays retain every
     * repeated SCK/GCK descriptor (the four-bit count limits each to 15). */
    uint8_t mm_ck_change_valid;
    uint8_t mm_ck_change_ack;
    uint8_t mm_ck_change_security_class;
    uint8_t mm_ck_change_key_type;
    uint8_t mm_ck_change_sck_use;
    uint8_t mm_ck_change_sck_count;
    uint8_t mm_ck_change_sck_grouping;
    uint8_t mm_ck_change_sck_subset;
    uint16_t mm_ck_change_sck_vn;
    uint8_t mm_ck_change_sck_number[15];
    uint16_t mm_ck_change_sck_version[15];
    uint16_t mm_ck_change_cck_id;
    uint8_t mm_ck_change_gck_count;
    uint16_t mm_ck_change_gck_number[15];
    uint16_t mm_ck_change_gck_version[15];
    uint16_t mm_ck_change_gck_vn;
    uint8_t mm_ck_change_time_type;
    uint8_t mm_ck_change_slot;
    uint8_t mm_ck_change_frame;
    uint8_t mm_ck_change_multiframe;
    uint16_t mm_ck_change_hyperframe;
    uint64_t mm_ck_change_network_time;
    uint8_t mm_lu_accept_type;
    uint8_t mm_lu_accept_valid;
    uint32_t mm_lu_accept_ssi;
    uint8_t mm_lu_accept_ssi_valid;
    uint16_t mm_lu_accept_mcc;
    uint16_t mm_lu_accept_mnc;
    uint8_t mm_lu_accept_address_ext_valid;
    uint16_t mm_lu_accept_subscriber_class;
    uint8_t mm_lu_accept_subscriber_class_valid;
    uint16_t mm_lu_accept_energy_saving;
    uint8_t mm_lu_accept_energy_saving_valid;
    uint8_t mm_lu_accept_scch;
    uint8_t mm_lu_accept_scch_valid;
    uint8_t mm_lu_command_group_report;
    uint8_t mm_lu_command_cipher_control;
    uint16_t mm_lu_command_cipher_params;
    uint8_t mm_lu_command_valid;
    uint16_t mm_lu_command_mcc;
    uint16_t mm_lu_command_mnc;
    uint8_t mm_lu_command_address_ext_valid;
    uint32_t mm_lu_proceeding_ssi;
    uint32_t mm_lu_proceeding_mni;
    uint8_t mm_lu_proceeding_valid;
    uint8_t mm_group_identity_valid;
    uint8_t mm_group_identity_report;
    uint8_t mm_group_identity_ack_request;
    uint8_t mm_group_identity_attach_detach_mode;
    uint8_t mm_group_entry_count;
    uint8_t mm_group_entry_action[63];
    uint8_t mm_group_entry_attachment_lifetime[63];
    uint8_t mm_group_entry_class_of_usage[63];
    uint8_t mm_group_entry_detachment_reason[63];
    uint8_t mm_group_entry_address_type[63];
    uint32_t mm_group_entry_gssi[63];
    uint32_t mm_group_entry_extension[63];
    uint32_t mm_group_entry_vgssi[63];
    uint8_t mm_group_ack_result;
    uint8_t mm_group_ack_valid;
    uint8_t mm_fns_pdu_type;
    uint8_t mm_fns_valid;
    long dl_carrier_hz;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA frequency band params cached from SYSINFO so that a MAC Channel
     * Allocation IE can resolve an assigned carrier to a DL frequency.
     *
     * freq_band:   4-bit band index from last SYSINFO.
     * freq_offset: 2-bit freq_offset from last SYSINFO.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t freq_band;
    uint8_t freq_offset;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA assigned-channel state. The authoritative allocation is the
     * Channel Allocation IE in a downlink MAC-RESOURCE/MAC-END PDU.
     *
     * vc_assignment_type: compatibility copy of MAC allocation type.
     * vc_carrier:         12-bit carrier number.
     * vc_slot:            compatibility copy of the timeslot bitmap.
     * vc_freq_hz:         Resolved VC downlink frequency in Hz (0=unknown).
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t vc_assignment_type;
    uint16_t vc_carrier;
    uint8_t vc_slot;
    long vc_freq_hz;
    uint8_t vc_assignment_valid;
    uint8_t vc_timeslot_bitmap;
    uint8_t vc_uplink_downlink;
    uint8_t vc_grant_pending; /* allocation waits for a fragmented CMCE TM-SDU */
    uint32_t vc_grant_call_generation;
    uint8_t trunk_state; /* tetra_sm_state_e, mirrored for UI/state snapshots */

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA SDS (Short Data Service) state — updated by CMCE D-STATUS.
     *
     * sds_status: 16-bit pre-coded status value from CMCE D-STATUS.
     *                   0 means no status has been received yet.
     * sds_src:    24-bit SSI of the sending party (from the optional
     *                   Calling Party IE in D-STATUS), or 0 if not present.
     * sds_text:   NUL-terminated UTF-8 SDS message text (Phase 19).
     * sds_text_len: byte length of sds_text (excluding NUL).
     * sds_status_log: ring buffer of last 4 pre-coded status values.
     * sds_status_log_head: next-write position in the ring buffer.
     * tetra_sds_forward_*: validated SDS-TL storage/forward address. Types are
     *                     0=SNA, 1=SSI, 2=TSI, 3=external, 7=no address.
     * ───────────────────────────────────────────────────────────────────────── */
    uint16_t sds_status;
    uint32_t sds_src;
    char sds_text[256];
    uint16_t sds_text_len;
    uint16_t sds_status_log[4];
    uint8_t sds_status_log_head;
    uint8_t sds_storage_forward;
    uint8_t sds_validity_period;
    uint8_t sds_forward_type;
    uint8_t sds_forward_valid;
    uint8_t sds_forward_sna;
    uint32_t sds_forward_ssi;
    uint32_t sds_forward_extension;
    char sds_forward_external[25];
    /* Last accepted concatenated SDS fragment/reassembly status. */
    uint16_t sds_concat_ref;
    uint8_t sds_concat_total;
    uint8_t sds_concat_received;
    uint8_t sds_concat_sequence;
    uint8_t sds_concat_valid;
    uint8_t sds_concat_duplicate;
    uint8_t sds_concat_complete;
    uint32_t sds_concat_generation;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA SYSINFO extended fields (Phase 20-22):
     *
     * cck_valid:         1 when cck_id contains a valid CCK-ID.
     * cck_id:            16-bit CCK-ID from SYSINFO (or 0 if hyperframe).
     * duplex_spacing:    3-bit duplex spacing index from SYSINFO.
     * num_csch:          2-bit number of SCH channels from SYSINFO.
     * ms_txpwr_max:      3-bit MS TX power max from SYSINFO.
     * rxlev_access_min:  4-bit RXLEV access min from SYSINFO.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t cck_valid;
    uint16_t cck_id;
    uint8_t duplex_spacing;
    uint8_t num_csch;
    uint8_t ms_txpwr_max;
    uint8_t rxlev_access_min;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA ACCESS-DEFINE cached params (Phase 23):
     *
     * access_imm:       4-bit immediate value from last ACCESS-DEFINE.
     * access_wait_time: 4-bit waiting time from last ACCESS-DEFINE.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t access_imm;
    uint8_t access_wait_time;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA BSCH statistics (Phase 24):
     *
     * bsch_count:          number of BSCH frames parsed so far.
     * bsch_colour_changed: 1 if colour code changed since last reset.
     * ───────────────────────────────────────────────────────────────────────── */
    uint32_t bsch_count;
    uint8_t bsch_colour_changed;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA MAC frame counters (Phase 25):
     *
     * frames_total:    total MAC PDUs processed.
     * frames_sysinfo:  SYSINFO broadcast PDUs processed.
     * frames_resource: MAC-RESOURCE PDUs processed.
     * frames_frag:     MAC-FRAG/END PDUs processed.
     * ───────────────────────────────────────────────────────────────────────── */
    uint32_t frames_total;
    uint32_t frames_sysinfo;
    uint32_t frames_resource;
    uint32_t frames_frag;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA MAC-FRAG/END reassembly tracking (Phase 28 + Phase 69):
     *
     * frag_active:  1 while a MAC-FRAG sequence is in progress.
     * frag_seq:     running fragment sequence counter.
     * frag_buf:     one-byte-per-bit reassembly buffer (1024 bits max).
     * frag_nbits:   number of bits currently accumulated in the buffer.
     * frag_cc:      CC that initiated the current fragment sequence.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t frag_active;
    uint8_t frag_seq;
    uint8_t frag_buf[1024]; /* one byte per bit */
    uint16_t frag_nbits;
    int8_t frag_cc;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA CMCE D-SETUP supplementary fields (Phase 29):
     *
     * call_timeout: 4-bit call timeout from D-SETUP.
     * call_slots:   2-bit speech-service or slots-per-frame subfield.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t call_timeout;
    uint8_t call_slots;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA decode quality counters (Phase 35):
     *
     * decode_errors: cumulative count of frames that failed FEC.
     * decode_ok:     cumulative count of frames successfully decoded.
     * ───────────────────────────────────────────────────────────────────────── */
    uint32_t decode_errors;
    uint32_t decode_ok;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA TDMA timestamps extracted from BSCH (Phase 43):
     *
     * tdma_valid:  1 once the first SB1 has been decoded successfully.
     * tn:          Timeslot Number 1-4 (BSCH bits[10..11] + 1).
     * fn:          Frame Number 0-17  (BSCH bits[12..16]).
     * mn:          Multiframe Number 0-59 (BSCH bits[17..22]).
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t tdma_valid;
    uint8_t tn;
    uint8_t fn;
    uint8_t mn;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA MM D-LOCATION-UPDATING-REJECT fields (Phase 44):
     *
     * mm_lu_reject_cause:  5-bit rejection cause code.
     * mm_lu_reject_valid:  1 when cause has been populated.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t mm_lu_reject_cause;
    uint8_t mm_lu_reject_valid;
    uint8_t mm_lu_reject_type;
    uint8_t mm_lu_reject_cipher_control;
    uint16_t mm_lu_reject_cipher_params;
    uint16_t mm_lu_reject_mcc;
    uint16_t mm_lu_reject_mnc;
    uint8_t mm_lu_reject_address_ext_valid;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA CMCE D-SDS-DATA tracking fields (Phase 46):
     *
     * sds_msg_ref:  8-bit message reference from the last decoded SDS-TL header.
     * sds_last_cc:  Control channel number of the last SDS PDU received
     *                     (-1 = not yet received).
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t sds_msg_ref;
    int8_t sds_last_cc;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA CMCE D-CONNECT parsed fields (Phase 49):
     *
     * connect_enc_mode:  encryption flag from the optional Basic Service IE.
     * connect_call_type: circuit-mode service from the optional Basic Service IE.
     * Both remain zero when that optional IE is absent.
     * connect_valid:     1 once a D-CONNECT has been fully parsed.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t connect_enc_mode;
    uint8_t connect_call_type;
    uint8_t connect_valid;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA CMCE D-SDS-DATA SDTI=0 fields (Phase 50):
     *
     * sds_short_data:   16-bit pre-defined short data status.
     * sds_short_src:    24-bit calling party SSI.
     * sds_short_valid:  1 when populated.
     * ───────────────────────────────────────────────────────────────────────── */
    uint16_t sds_short_data;
    uint32_t sds_short_src;
    uint8_t sds_short_valid;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA CMCE floor-control event flags (Phase 54):
     *
     * Each flag is set to 1 when the corresponding PDU is received.
     * tx_continue:   D-TX-CONTINUE  (type 10) — speaker may continue.
     * tx_interrupted: D-TX-INTERRUPT (type 13) — speaker interrupted.
     * tx_wait:       D-TX-WAIT      (type 12) — MS must wait.
     * tx_timed_out: legacy ABI field; no downlink PDU has this type.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t tx_continue;
    uint8_t tx_interrupted;
    uint8_t tx_wait;
    uint8_t tx_timed_out;
    uint16_t tx_event_call_id;
    uint8_t tx_event_notification;
    uint8_t tx_event_request_perm;
    uint8_t tx_event_continue;
    uint8_t tx_event_grant;
    uint8_t tx_event_encryption;
    uint8_t tx_event_notification_valid;
    uint8_t tx_event_party_type;
    uint8_t tx_event_party_type_valid;
    uint32_t tx_event_party_ssi;
    uint8_t tx_event_party_ssi_valid;
    uint32_t tx_event_party_extension;
    uint8_t tx_event_party_extension_valid;

    /* TETRA MLE cell-reselection and restore state. */
    uint8_t restore_ack;
    uint8_t mle_new_cell_channel_command;
    uint8_t mle_new_cell_valid;
    uint8_t mle_prepare_fail_cause;
    uint8_t mle_prepare_fail_valid;
    uint8_t mle_channel_response_type;
    uint8_t mle_channel_response_reason;
    uint8_t mle_channel_response_retry_delay;
    uint8_t mle_channel_response_valid;
    uint16_t mle_cell_reselect_params;
    uint8_t mle_cell_load;
    uint64_t mle_network_time;
    uint8_t mle_network_time_valid;
    uint8_t mle_ca_neighbor_count;
    uint8_t mle_ca_neighbor_count_valid;
    uint8_t mle_ca_neighbor_cell_id[7];
    uint8_t mle_ca_neighbor_reselect_types[7];
    uint8_t mle_ca_neighbor_synchronized[7];
    uint8_t mle_ca_neighbor_load[7];
    uint16_t mle_ca_neighbor_main_carrier[7];
    uint8_t mle_restore_fail_cause;
    uint8_t mle_restore_fail_valid;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA CMCE D-INFO parsed fields (Phase 57):
     *
     * d_info_call_id:      14-bit call identifier.
     * d_info_call_timeout: reset-call-timeout flag.
     * d_info_notification: legacy name for the poll-request flag.
     * d_info_valid:         1 once a D-INFO has been decoded.
     * ───────────────────────────────────────────────────────────────────────── */
    uint16_t d_info_call_id;
    uint8_t d_info_call_timeout;
    uint8_t d_info_notification;
    uint16_t d_info_new_call_id;
    uint8_t d_info_new_call_id_valid;
    uint8_t d_info_timeout;
    uint8_t d_info_timeout_valid;
    uint8_t d_info_setup_timeout;
    uint8_t d_info_setup_timeout_valid;
    uint8_t d_info_ownership;
    uint8_t d_info_ownership_valid;
    uint16_t d_info_modify;
    uint8_t d_info_modify_valid;
    uint8_t d_info_status;
    uint8_t d_info_status_valid;
    uint32_t d_info_temporary_address;
    uint8_t d_info_temporary_address_valid;
    uint8_t d_info_notification_indicator;
    uint8_t d_info_notification_indicator_valid;
    uint8_t d_info_poll_percentage;
    uint8_t d_info_poll_percentage_valid;
    uint8_t d_info_poll_number;
    uint8_t d_info_poll_number_valid;
    uint8_t d_info_valid;

    /* ───────────────────────────────────────────────────────────────────────
     * TETRA SDS-DATA Unicode flag (Phase 58):
     *
     * sds_text_unicode: 1 when the last decoded SDS text used Unicode
     *                          encoding (bpc=10 or bpc=16).
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t sds_text_unicode;

    /* TETRA MLE D-NWRK-BROADCAST EXTENSION, EN 300 392-2 table 18.4.
     * Counts are the decoded list sizes; the matching present flag distinguishes
     * an explicitly advertised empty list from an omitted list. */
    uint8_t nwrk_bcast_ext_known;
    uint8_t mle_ext_serving_classes_present;
    uint8_t mle_ext_serving_class_count;
    uint8_t mle_ext_serving_class_id[15];
    uint32_t mle_ext_serving_class_characteristics[15];
    uint8_t mle_ext_serving_class_bs_power[15];
    uint8_t mle_ext_neighbor_classes_present;
    uint8_t mle_ext_neighbor_class_count;
    uint8_t mle_ext_neighbor_class_cell_id[31];
    uint8_t mle_ext_neighbor_class_id[31];
    uint32_t mle_ext_neighbor_class_characteristics[31];
    uint8_t mle_ext_neighbor_class_bs_power[31];
    uint8_t mle_ext_serving_irregular_present;
    uint8_t mle_ext_serving_irregular_count;
    uint8_t mle_ext_serving_irregular_channel_id[31];
    uint32_t mle_ext_serving_irregular_characteristics[31];
    uint16_t mle_ext_serving_irregular_carrier[31];
    uint16_t mle_ext_serving_irregular_carrier_extension[31];
    uint8_t mle_ext_serving_irregular_extension_valid[31];
    uint8_t mle_ext_neighbor_irregular_present;
    uint8_t mle_ext_neighbor_irregular_count;
    uint8_t mle_ext_neighbor_irregular_cell_id[63];
    uint8_t mle_ext_neighbor_irregular_channel_id[63];
    uint32_t mle_ext_neighbor_irregular_characteristics[63];
    uint16_t mle_ext_neighbor_irregular_carrier[63];
    uint16_t mle_ext_neighbor_irregular_carrier_extension[63];
    uint8_t mle_ext_neighbor_irregular_extension_valid[63];

    /* TETRA MLE D-NWRK-BROADCAST REMOVE, table 18.5.  Each row retains
     * the full bounded list of class/channel identifiers from its removal
     * data element. remove_cell=1 means those lists are absent. */
    uint8_t mle_remove_known;
    uint8_t mle_remove_ca_present;
    uint8_t mle_remove_ca_count;
    uint8_t mle_remove_ca_cell_id[31];
    uint8_t mle_remove_ca_remove_cell[31];
    uint8_t mle_remove_ca_class_count[31];
    uint8_t mle_remove_ca_class_id[31][15];
    uint8_t mle_remove_ca_channel_count[31];
    uint8_t mle_remove_ca_channel_id[31][31];
    uint8_t mle_remove_da_present;
    uint8_t mle_remove_da_count;
    uint8_t mle_remove_da_cell_id[255];
    uint8_t mle_remove_da_remove_cell[255];
    uint8_t mle_remove_da_class_count[255];
    uint8_t mle_remove_da_class_id[255][15];
    uint8_t mle_remove_da_channel_count[255];
    uint8_t mle_remove_da_channel_id[255][31];
    uint8_t mle_remove_serving_present;
    uint8_t mle_remove_serving_class_count;
    uint8_t mle_remove_serving_class_id[15];
    uint8_t mle_remove_serving_channel_count;
    uint8_t mle_remove_serving_channel_id[31];

    /* Extended-PDU D-NWRK-BROADCAST-DA, table 18.3. */
    uint8_t mle_da_broadcast_known;
    uint8_t mle_da_cell_id_valid;
    uint8_t mle_da_cell_id;
    uint8_t mle_da_reselect_valid;
    uint16_t mle_da_reselect;
    uint8_t mle_da_load_valid;
    uint8_t mle_da_load;
    uint8_t mle_da_network_time_valid;
    uint64_t mle_da_network_time;
    uint8_t mle_da_local_ca_valid;
    uint8_t mle_da_local_ca_cell_id;
    uint8_t mle_da_local_ca_reselect_types;
    uint8_t mle_da_local_ca_synchronized;
    uint8_t mle_da_local_ca_load;
    uint16_t mle_da_local_ca_main_carrier;
    uint8_t mle_da_neighbor_list_present;
    uint8_t mle_da_neighbor_count;
    uint8_t mle_da_neighbor_cell_id[7];
    uint8_t mle_da_neighbor_reselect_types[7];
    uint8_t mle_da_neighbor_synchronized[7];
    uint8_t mle_da_neighbor_wide_area[7];
    uint8_t mle_da_neighbor_ci_path[7];
    uint8_t mle_da_neighbor_load_known[7];
    uint8_t mle_da_neighbor_load[7];
    uint16_t mle_da_neighbor_main_carrier[7];
    uint8_t mle_da_neighbor_modulation[7];
    uint8_t mle_da_neighbor_bandwidth[7];
    uint16_t mle_da_neighbor_optional_mask[7];
    uint16_t mle_da_neighbor_carrier_extension[7];
    uint16_t mle_da_neighbor_mcc[7];
    uint16_t mle_da_neighbor_mnc[7];
    uint16_t mle_da_neighbor_la[7];
    uint8_t mle_da_neighbor_local_cell_id[7];
    uint8_t mle_da_neighbor_max_tx_power[7];
    uint8_t mle_da_neighbor_min_rx_level[7];
    uint16_t mle_da_neighbor_subscriber_class[7];
    uint8_t mle_da_neighbor_bs_service_details[7];
    uint8_t mle_da_neighbor_security[7];
    uint8_t mle_da_neighbor_frame_offset[7];
    uint8_t mle_da_serving_classes_present;
    uint8_t mle_da_serving_class_count;
    uint8_t mle_da_serving_class_id[15];
    uint32_t mle_da_serving_class_characteristics[15];
    uint8_t mle_da_serving_class_bs_power[15];
    uint8_t mle_da_neighbor_classes_present;
    uint8_t mle_da_neighbor_class_count;
    uint8_t mle_da_neighbor_class_cell_id[31];
    uint8_t mle_da_neighbor_class_id[31];
    uint32_t mle_da_neighbor_class_characteristics[31];
    uint8_t mle_da_neighbor_class_bs_power[31];
    uint8_t mle_da_serving_irregular_present;
    uint8_t mle_da_serving_irregular_count;
    uint8_t mle_da_serving_irregular_channel_id[31];
    uint32_t mle_da_serving_irregular_characteristics[31];
    uint16_t mle_da_serving_irregular_carrier[31];
    uint16_t mle_da_serving_irregular_extension[31];
    uint8_t mle_da_serving_irregular_extension_valid[31];
    uint8_t mle_da_neighbor_irregular_present;
    uint8_t mle_da_neighbor_irregular_count;
    uint8_t mle_da_neighbor_irregular_cell_id[63];
    uint8_t mle_da_neighbor_irregular_channel_id[63];
    uint32_t mle_da_neighbor_irregular_characteristics[63];
    uint16_t mle_da_neighbor_irregular_carrier[63];
    uint16_t mle_da_neighbor_irregular_extension[63];
    uint8_t mle_da_neighbor_irregular_extension_valid[63];

    /* EN 300 392-7 tables A.1-A.4 D-AUTHENTICATION snapshot. */
    uint8_t mm_auth_valid;
    uint8_t mm_auth_subtype;
    uint8_t mm_auth_random_challenge[10];
    uint8_t mm_auth_random_seed[10];
    uint32_t mm_auth_response;
    uint8_t mm_auth_mutual;
    uint8_t mm_auth_result;
    uint8_t mm_auth_reject_reason;

    /* EN 300 392-7 D-OTAR downlink key-management snapshot. */
    uint8_t mm_otar_valid;
    uint8_t mm_otar_subtype;
    uint8_t mm_otar_cck_provision;
    uint16_t mm_otar_cck_id;
    uint8_t mm_otar_cck_key_type;
    uint8_t mm_otar_cck_sealed[15];
    uint8_t mm_otar_cck_la_type;
    uint8_t mm_otar_cck_la_count;
    uint16_t mm_otar_cck_la[15];
    uint16_t mm_otar_cck_la_mask;
    uint16_t mm_otar_cck_la_selector;
    uint16_t mm_otar_cck_la_low;
    uint16_t mm_otar_cck_la_high;
    uint8_t mm_otar_cck_future;
    uint8_t mm_otar_cck_future_sealed[15];
    uint8_t mm_otar_ack;
    uint8_t mm_otar_explicit_response;
    uint16_t mm_otar_max_response_timer;
    uint8_t mm_otar_session_key;
    uint8_t mm_otar_random_seed[10];
    uint16_t mm_otar_gsko_version;
    uint8_t mm_otar_key_count;
    uint8_t mm_otar_sck_number[7];
    uint16_t mm_otar_sck_version[7];
    uint8_t mm_otar_sck_use[7];
    uint8_t mm_otar_sck_sealed[7][15];
    uint16_t mm_otar_gck_number[7];
    uint16_t mm_otar_gck_version[7];
    uint8_t mm_otar_gck_sealed[7][15];
    uint8_t mm_otar_group_association;
    uint8_t mm_otar_gck_reject_group_association[7];
    uint32_t mm_otar_gck_reject_gssi[7];
    uint8_t mm_otar_key_association_type;
    uint8_t mm_otar_sck_select;
    uint8_t mm_otar_sck_subset_grouping;
    uint32_t mm_otar_gck_select;
    uint8_t mm_otar_group_count;
    uint8_t mm_otar_group_is_range;
    uint8_t mm_otar_group_value_count;
    uint32_t mm_otar_group_gssi[30];
    uint8_t mm_otar_dck_forwarding_result;
    uint8_t mm_otar_key_status_type;
    uint8_t mm_otar_key_status_sck_number;
    uint8_t mm_otar_key_status_sck_grouping;
    uint8_t mm_otar_key_status_sck_subset;
    uint16_t mm_otar_key_status_gck_number;
    uint8_t mm_otar_key_delete_type;
    uint8_t mm_otar_key_delete_sck_count;
    uint8_t mm_otar_key_delete_sck_number[31];
    uint8_t mm_otar_key_delete_sck_grouping;
    uint8_t mm_otar_key_delete_sck_subset;
    uint8_t mm_otar_key_delete_gck_count;
    uint16_t mm_otar_key_delete_gck_number[15];
    uint8_t mm_otar_dm_sck_ack;
    uint8_t mm_otar_dm_sck_count;
    uint8_t mm_otar_dm_sck_grouping;
    uint8_t mm_otar_dm_sck_subset;
    uint16_t mm_otar_dm_sck_vn;
    uint8_t mm_otar_dm_sck_number[15];
    uint16_t mm_otar_dm_sck_version[15];
    uint8_t mm_otar_dm_sck_time_type;
    uint8_t mm_otar_dm_sck_slot;
    uint8_t mm_otar_dm_sck_frame;
    uint8_t mm_otar_dm_sck_multiframe;
    uint16_t mm_otar_dm_sck_hyperframe;
    uint64_t mm_otar_dm_sck_network_time;
    uint8_t mm_otar_reject_reason[7];
    uint8_t mm_otar_ksg;
    uint8_t mm_otar_retry_interval;
    /* KSG 0..3 remembered from D-OTAR SCK/GCK Provide, keyed by MCC/MNC.
     * hyperframe is the SYSINFO value from a burst whose CCK flag is clear,
     * kept apart from cck_id so a later CCK identifier does not erase it.
     * The operator air keys live in the key set (dsd_state::tetra_air_keys). */
    uint8_t air_ksg_valid[4];
    uint16_t air_ksg_mcc[4];
    uint16_t air_ksg_mnc[4];
    uint8_t air_ksg[4];
    uint8_t hyperframe_valid;
    uint16_t hyperframe;
    uint8_t mm_otar_address_valid;
    uint16_t mm_otar_mcc;
    uint16_t mm_otar_mnc;
    uint8_t mm_otar_gsko_sealed[15];
    uint32_t mm_otar_gssi;

    /* ───────────────────────────────────────────────────────────────────────
     * Phase 64: CMCE D-ALERT call_id.
     * Phase 65: CMCE D-CALL-PROCEEDING call_id.
     * Phase 66: CMCE D-CONNECT-ACK call_id.
     * ───────────────────────────────────────────────────────────────────────── */
    uint16_t d_alert_call_id;
    uint8_t d_alert_valid;
    uint16_t d_call_proc_call_id;
    uint8_t d_call_proc_valid;
    uint16_t d_connect_ack_call_id;
    uint8_t d_connect_ack_valid;
    uint8_t d_alert_timeout;
    uint8_t d_alert_reserved;
    uint8_t d_alert_duplex;
    uint8_t d_alert_queued;
    uint8_t d_alert_basic_service;
    uint8_t d_alert_notification;
    uint8_t d_call_proc_timeout;
    uint8_t d_call_proc_hook;
    uint8_t d_call_proc_duplex;
    uint8_t d_call_proc_basic_service;
    uint8_t d_call_proc_status;
    uint8_t d_call_proc_notification;
    uint8_t d_connect_ack_tx_grant;
    uint8_t d_connect_ack_tx_permission;
    uint8_t d_connect_ack_notification;
    uint8_t d_connect_ack_notification_valid;
    uint16_t call_restore_id;
    uint8_t call_restore_grant;
    uint8_t call_restore_permission;
    uint8_t call_restore_reset;
    uint16_t call_restore_new_id;
    uint8_t call_restore_new_id_valid;
    uint8_t call_restore_timeout;
    uint8_t call_restore_timeout_valid;
    uint8_t call_restore_status;
    uint8_t call_restore_status_valid;
    uint16_t call_restore_modify;
    uint8_t call_restore_modify_valid;
    uint8_t call_restore_notification;
    uint8_t call_restore_notification_valid;
    uint8_t call_restore_valid;
    uint8_t cmce_fns_rejected_pdu;
    uint8_t cmce_fns_call_id_present;
    uint16_t cmce_fns_call_id;
    uint8_t cmce_fns_pointer;
    uint8_t cmce_fns_extract_bits;
    uint8_t cmce_fns_valid;

    /* ───────────────────────────────────────────────────────────────────────
     * CMCE D-TX-CEASED parsed state.
     * tx_ceased_tx_perm: Transmission request permission bit.
     * tx_ceased_cipher_info: legacy ABI field; always zero because
     *                              Table 14.16 contains no cipher-info IE.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t tx_ceased_tx_perm;
    uint8_t tx_ceased_cipher_info;

    /* ───────────────────────────────────────────────────────────────────────
     * SDS-TL SDS-REPORT carried inside D-SDS-DATA, SDTI=3.
     * sds_report_valid:       1 after an SDS-REPORT is received.
     * sds_report_delivery_ok: 1=delivered, 0=failure.
     * sds_report_cause:       Full 8-bit delivery status.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t sds_report_valid;
    uint8_t sds_report_delivery_ok;
    uint8_t sds_report_cause;
    uint8_t sds_report_msg_ref;

    /* ───────────────────────────────────────────────────────────────────────
     * Phase 74: MAC ACCESS-DEFINE extended fields.
     * access_num_ra:      4-bit number of random access channels.
     * access_frame_len_f: 1-bit frame length factor.
     * access_ts_ptr:      4-bit timeslot pointer.
     * access_min_pdu_pri: 3-bit minimum PDU priority.
     * ───────────────────────────────────────────────────────────────────────── */
    uint8_t access_num_ra;
    uint8_t access_frame_len_f;
    uint8_t access_ts_ptr;
    uint8_t access_min_pdu_pri;

    /* --- Phase 77: TETRA MAC dropped variables --- */
    uint8_t mac_fill_bits;
    uint8_t mac_grant_pos;
    uint8_t mac_rand_acc;
    uint8_t mac_len_ind;
    uint8_t mac_addr_type;
    uint16_t mac_event_label; // 10 bits
    uint8_t mac_usage_marker; // 6 bits

    uint16_t sysinfo_main_carrier;
    uint8_t sysinfo_rev_op;
    uint8_t sysinfo_acc_param;
    uint8_t sysinfo_radio_dl_tmo;
    uint8_t sysinfo_opt_field_type;
    uint32_t sysinfo_opt_field_data; // 20 bits

    uint8_t access_common_flag;

    /* --- Phase 78: TETRA CMCE/MLE dropped variables --- */
    uint8_t cmce_duplex;
    uint8_t cmce_notif;
    uint8_t cmce_com_type;
    uint8_t cmce_called_type;
    uint8_t cmce_setup_hook;
    uint8_t cmce_setup_basic_service;
    uint8_t cmce_setup_tx_grant;
    uint8_t cmce_setup_tx_permission;
    uint8_t cmce_setup_priority;
    uint8_t cmce_setup_notification_valid;
    uint32_t cmce_setup_temporary_address;
    uint8_t cmce_setup_temporary_address_valid;
    uint8_t cmce_setup_calling_type_valid;
    uint32_t cmce_setup_calling_extension;
    uint8_t cmce_setup_calling_extension_valid;
    uint8_t cmce_connect_hook;
    uint8_t cmce_connect_duplex;
    uint8_t cmce_connect_tx_grant;
    uint8_t cmce_connect_tx_permission;
    uint8_t cmce_connect_ownership;
    uint8_t cmce_connect_priority;
    uint8_t cmce_connect_priority_valid;
    uint8_t cmce_connect_basic_service;
    uint8_t cmce_connect_basic_service_valid;
    uint32_t cmce_connect_temporary_address;
    uint8_t cmce_connect_temporary_address_valid;
    uint8_t cmce_connect_notification;
    uint8_t cmce_connect_notification_valid;

    uint8_t cmce_release_cause_type;
    uint8_t cmce_release_cause;
    uint8_t cmce_release_notification;
    uint8_t cmce_release_notification_valid;
    uint8_t cmce_release_was_disconnect;

    uint8_t cmce_tx_granted_perm;
    uint8_t cmce_tx_granted_reserv;
    uint8_t cmce_tx_granted_notification;
    uint8_t cmce_tx_granted_notification_valid;
    uint8_t cmce_tx_granted_party_type;
    uint8_t cmce_tx_granted_party_type_valid;
    uint32_t cmce_tx_granted_party_extension;
    uint8_t cmce_tx_granted_party_extension_valid;

    uint8_t cmce_sds_data_type;

    /* --- Phase 79: TETRA MM dropped variables --- */
    uint8_t mm_detach_flag;
    uint8_t mm_class_of_grp;
    uint8_t mm_addr_type;

    /* --- Phase 82-83: additional CMCE PDU fields --- */
    uint8_t facility_valid;
    uint8_t facility_type;

    uint8_t sds_ack_valid;
    uint8_t sds_ack_msg_ref;
    uint8_t sds_ack_delivery_status;

    uint8_t sds_short_report_valid;
    uint8_t sds_short_report_result;
    uint8_t sds_short_report_msg_ref;

    /* --- Phase 86: SNDCP (PD=8) parsed fields --- */
    uint8_t sndcp_valid;
    uint8_t sndcp_nsapi;
    uint8_t sndcp_pdu_type;
    uint16_t sndcp_nbits;
} tetra_state_t;

/** The TETRA state of @p state, created on first use; NULL only when @p state is NULL or memory runs out. */
tetra_state_t* tetra_st(dsd_state* state);

/** The TETRA state of @p state without creating it: a zeroed default when it has none; NULL only for a NULL @p state. */
const tetra_state_t* tetra_st_peek(const dsd_state* state);

/** Drop the TETRA state of @p state; the next tetra_st() starts from zero. */
void tetra_state_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_STATE_H_ */
