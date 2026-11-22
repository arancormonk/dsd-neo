// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dPMR headers and end frames, and what dPMR publishes of a call.
 *
 * The frame sync accepts an FS1 or FS3 sync only behind a header or end frame that passed its CRC and FEC
 * (src/dsp/frame_sync_dpmr.c), and leaves what it decoded in dsd_state::dpmr_sync. This takes it for the sync being
 * dispatched and acts on it: a Communications_Start header (ETSI TS 102 658 V2.6.1 5.2.1) opens its call with the
 * identity and service its MI carries, and a checked end frame (5.3) is what ends a call as a terminator.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dpmr/dpmr.h>
#include <dsd-neo/protocol/dpmr/dpmr_data.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dpmr_confirm.h"
#include "dpmr_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static uint32_t
dpmr_field(const uint8_t* bits, int at, int count) {
    uint32_t value = 0U;
    for (int i = 0; i < count; i++) {
        value = (value << 1) | (bits[at + i] & 1U);
    }
    return value;
}

/* The checked frame of @p kind the sync being dispatched was accepted on, taken once. */
static int
dpmr_take_checked(dsd_state* state, uint8_t kind) {
    dsd_dpmr_frame_sync* sync = &state->dpmr_sync;
    if (sync->checked_kind != kind || sync->checked_clock != state->symbol_clock
        || sync->checked_epoch != state->symbol_clock_epoch) {
        return 0;
    }
    sync->checked_kind = DSD_DPMR_SYNC_FRAME_NONE;
    return 1;
}

int
dpmr_take_checked_header(dsd_state* state, dpmr_header* out) {
    if (state == NULL || out == NULL || !dpmr_take_checked(state, DSD_DPMR_SYNC_FRAME_HEADER)) {
        return 0;
    }
    const uint8_t* mi = state->dpmr_sync.checked_bits;
    /* Table 5.4: MT 4, ID0+1 24, ID2+3 24, M 3, V 2, F 2, EP 1, PM 1, MI_TYPE 3, MI_DET 8 (72 bits, 11.5). */
    out->message_type = dpmr_field(mi, 0, 4);
    out->called_id = dpmr_field(mi, 4, 24);
    out->calling_id = dpmr_field(mi, 28, 24);
    out->mode = dpmr_field(mi, 52, 3);
    out->version = dpmr_field(mi, 55, 2);
    out->format = dpmr_field(mi, 57, 2);
    out->emergency = dpmr_field(mi, 59, 1);
    out->mi_type = dpmr_field(mi, 61, 3);
    out->mi_detail = dpmr_field(mi, 64, 8);
    out->color_code = (int)GetdPmrColorCode(state->dpmr_sync.checked_cc);
    out->copies = state->dpmr_sync.checked_copies;
    return 1;
}

int
dpmr_take_checked_end(dsd_state* state, dpmr_end_frame* out) {
    if (state == NULL || out == NULL || !dpmr_take_checked(state, DSD_DPMR_SYNC_FRAME_END)) {
        return 0;
    }
    const uint8_t* end = state->dpmr_sync.checked_bits;
    /* Table 5.44: ET 2, ARQ 2, Tx_WAIT 4, STAT 5, RSVD 4 (17 bits, 11.6). */
    out->end_type = dpmr_field(end, 0, 2);
    out->ack_request = dpmr_field(end, 2, 2);
    out->tx_wait = dpmr_field(end, 4, 4);
    out->status = dpmr_field(end, 8, 5);
    out->copies = state->dpmr_sync.checked_copies;
    return 1;
}

dsd_call_kind
dpmr_call_kind_for_mode(uint32_t mode) {
    /* Table 5.55: 000 voice, 001 voice and slow data, 101 voice and attached data; 010, 011, 100 data. */
    if (mode == 0U || mode == 1U || mode == 5U) {
        return DSD_CALL_KIND_VOICE;
    }
    if (mode == 2U || mode == 3U || mode == 4U) {
        return DSD_CALL_KIND_DATA;
    }
    return DSD_CALL_KIND_UNKNOWN;
}

/* The epoch of the dPMR call open on the slot, 0 for none. */
static uint64_t
dpmr_open_call_epoch(const dsd_state* state) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
        && DSD_SYNC_IS_DPMR(call.protocol)) {
        return call.epoch;
    }
    return 0U;
}

/* Whether the record was made in another reception than this report's: another scan visit or carrier, another
   reception generation (a verified colour code change: another system on the frequency; a sync of another protocol; a
   retune), or another dPMR call than the one open now (the last one ended, and the frame that opened this one names one
   station only). A record made while no call was open is the next call's. */
static int
dpmr_scan_ids_stale(const dsd_state* state, const dsd_scan_observation_origin* origin, uint64_t rx_gen,
                    uint64_t call_epoch) {
    return state->dpmr_scan_ids_visit != origin->visit || state->dpmr_scan_ids_carrier_seq != origin->carrier_seq
           || state->dpmr_scan_ids_rx_gen != rx_gen
           || (state->dpmr_scan_ids_call_epoch != 0U && call_epoch != 0U
               && state->dpmr_scan_ids_call_epoch != call_epoch);
}

/* Merge the stations a report names into the transmission's checked record, and fill those it does not name from it
   (dpmr_report_scan_activity()). A record of another reception (dpmr_scan_ids_stale()) is forgotten first; a station
   named that differs from the one recorded begins another call, whose other station is not the recorded one. A
   report whose origin is gone (a frame a boundary cut) neither reads nor writes the record. Each report comes after
   its frame or header published what it checked (its colour code, its call), so the record is judged against them. */
static void
dpmr_scan_ids_merge(dsd_state* state, uint32_t* target, int* target_known, uint32_t* source, int* source_known) {
    dsd_scan_observation_origin origin;
    if (!dsd_state_scan_observation_origin(state, &origin)) {
        return;
    }
    const uint64_t rx_gen = dsd_state_reception_generation(state);
    const uint64_t call_epoch = dpmr_open_call_epoch(state);
    if (dpmr_scan_ids_stale(state, &origin, rx_gen, call_epoch)) {
        state->dpmr_scan_ids_known = 0U;
        state->dpmr_scan_ids_visit = origin.visit;
        state->dpmr_scan_ids_carrier_seq = origin.carrier_seq;
        state->dpmr_scan_ids_rx_gen = rx_gen;
        state->dpmr_scan_ids_call_epoch = 0U;
    }
    if (call_epoch != 0U) {
        state->dpmr_scan_ids_call_epoch = call_epoch;
    }
    const uint8_t known_target = DSD_SCAN_ACTIVITY_KNOWN_TARGET;
    const uint8_t known_source = DSD_SCAN_ACTIVITY_KNOWN_SOURCE;
    const uint32_t named_target = *target & 0xFFFFFFU;
    const uint32_t named_source = *source & 0xFFFFFFU;
    if ((*target_known && (state->dpmr_scan_ids_known & known_target) && state->dpmr_scan_target != named_target)
        || (*source_known && (state->dpmr_scan_ids_known & known_source) && state->dpmr_scan_source != named_source)) {
        state->dpmr_scan_ids_known = 0U; /* another call */
    }
    if (*target_known) {
        state->dpmr_scan_target = named_target;
        state->dpmr_scan_ids_known |= known_target;
    } else if (state->dpmr_scan_ids_known & known_target) {
        *target = state->dpmr_scan_target;
        *target_known = 1;
    }
    if (*source_known) {
        state->dpmr_scan_source = named_source;
        state->dpmr_scan_ids_known |= known_source;
    } else if (state->dpmr_scan_ids_known & known_source) {
        *source = state->dpmr_scan_source;
        *source_known = 1;
    }
}

void
dpmr_report_scan_activity(const dsd_opts* opts, dsd_state* state, uint32_t target, int target_known, uint32_t source,
                          int source_known, uint32_t mode, uint32_t version) {
    dpmr_scan_ids_merge(state, &target, &target_known, &source, &source_known);
    dsd_trunk_scan_activity_identity identity;
    DSD_MEMSET(&identity, 0, sizeof(identity));
    identity.known = DSD_SCAN_ACTIVITY_KNOWN_ENCRYPTED;
    identity.encrypted = (uint8_t)(version == 3U);
    if (target_known) {
        identity.target = target & 0xFFFFFFU;
        identity.known |= DSD_SCAN_ACTIVITY_KNOWN_TARGET;
    }
    if (source_known) {
        identity.source = source & 0xFFFFFFU;
        identity.known |= DSD_SCAN_ACTIVITY_KNOWN_SOURCE;
    }
    const dsd_call_kind kind = dpmr_call_kind_for_mode(mode);
    if (kind != DSD_CALL_KIND_UNKNOWN) {
        identity.data_call = (uint8_t)(kind == DSD_CALL_KIND_DATA);
        identity.known |= DSD_SCAN_ACTIVITY_KNOWN_DATA;
    }
    const int protocol = DSD_SYNC_IS_DPMR(state->synctype) ? state->synctype : DSD_SYNC_DPMR_FS2_POS;
    dsd_trunk_scan_hook_digital_conventional_activity(opts, state, protocol, -1, &identity);
}

int
dpmr_header_opens_call(const dpmr_header* header) {
    return header != NULL && header->message_type == DPMR_MT_COMMUNICATIONS_START
           && dpmr_call_kind_for_mode(header->mode) != DSD_CALL_KIND_UNKNOWN;
}

void
dpmr_publish_color_code(dsd_state* state, int color_code) {
    /* This carrier's colour code, read with the call it belongs to (issue #575): published on the confirmed path. The
       engine puts it back to -1 between carriers. */
    if (color_code < 0) {
        return;
    }
    state->dpmr_color_code = color_code;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DPMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, (uint16_t)color_code);
}

void
dpmr_publish_crypto(dsd_state* state, unsigned int scrambled, uint8_t enc_observed) {
    dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_CLEAR,
        .audio_permitted = 1U,
        .enc_observed = enc_observed,
    };
    if (scrambled == DPMR_HALF_SCRAMBLED) {
        crypto.classification =
            dsd_key_scalar_present(state, 0) ? DSD_CALL_CRYPTO_DECRYPTABLE : DSD_CALL_CRYPTO_ENCRYPTED;
        crypto.audio_permitted = dsd_key_scalar_present(state, 0);
    }
    (void)dsd_call_state_update_crypto(state, 0U, DSD_SYNC_DPMR_FS2_POS, &crypto);
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0, &call) > 0) {
        int available = -1; /* clear: no key in question */
        if (crypto.classification == DSD_CALL_CRYPTO_DECRYPTABLE) {
            available = 1;
        } else if (crypto.classification == DSD_CALL_CRYPTO_ENCRYPTED) {
            available = 0;
        }
        (void)dsd_call_state_note_key_selection(state, 0, DSD_SYNC_DPMR_FS2_POS, call.epoch, DSD_CALL_KEY_DIRECT,
                                                call.kid, -1, available, 0);
    }
}

static const char*
dpmr_message_type_name(uint32_t mt) {
    switch (mt) {
        case DPMR_MT_COMMUNICATIONS_START: return "Comms Start";
        case DPMR_MT_CONNECTION_REQUEST: return "Connection Request";
        case DPMR_MT_DISCONNECT_REQUEST: return "Disconnect Request";
        case DPMR_MT_ACK: return "ACK";
        case DPMR_MT_MAINTENANCE_OR_SYSTEM_REQUEST: return "Maintenance/System Request";
        case DPMR_MT_SYSTEM_ACK: return "System ACK";
        case DPMR_MT_SYSTEM_DELIVERY: return "System Delivery";
        case DPMR_MT_STATUS_POLLING_RESPONSE: return "Status Polling Response";
        default: return "Header";
    }
}

/* The service options a header publishes (dsd_call_observation::service_options), as an FS2 frame's checked half does. */
static uint16_t
dpmr_header_service_options(const dpmr_header* header) {
    return (uint16_t)((header->version << 8U) | (header->mode << 4U) | header->format);
}

/* The call a Communications_Start header opens, published once: its called and calling IDs (Table 5.4), its service
   and its kind, the channel code read with it and the scrambling its Version signals. */
static void
dpmr_publish_header_call(dsd_opts* opts, dsd_state* state, const dpmr_header* header, int continues) {
    char called[8];
    char calling[8];
    dpmr_convert_air_interface_id(header->called_id, called);
    dpmr_convert_air_interface_id(header->calling_id, calling);
    if (!continues) {
        /* A new call's code is its own header's: none when its channel code did not decode, never the code an earlier
           call left. A repeat keeps the run's, or supplies the one it lacked. */
        state->dpmr_color_code = -1;
    }
    dpmr_publish_color_code(state, header->color_code);
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_IS_DPMR(state->synctype) ? state->synctype : DSD_SYNC_DPMR_FS1_POS,
        .slot = 0U,
        .kind = dpmr_call_kind_for_mode(header->mode),
        .service_options = dpmr_header_service_options(header),
        .emergency = (uint8_t)(header->emergency != 0U),
        .has_service_metadata = 1U,
    };
    dsd_strncpy_s(observation.target_text, sizeof(observation.target_text), called,
                  sizeof(observation.target_text) - 1U);
    dsd_strncpy_s(observation.source_text, sizeof(observation.source_text), calling,
                  sizeof(observation.source_text) - 1U);
    /* The checked MI's raw 24-bit IDs as the call's numbers (issue #890), beside the dial strings shown: the policies
       that read the call (the voice-only scan's, the audio gate's) judge it by them, as the activity reports are. */
    observation.ota_target_id = header->called_id & 0xFFFFFFU;
    observation.ota_source_id = header->calling_id & 0xFFFFFFU;
    /* A header opens its transmission (dsd_call_boundary: a voice header is per-transmission evidence); a repeat of the
       header that opened it goes on with it. */
    (void)dsd_call_state_observe(state, &observation, continues ? DSD_CALL_BOUNDARY_CONTINUE : DSD_CALL_BOUNDARY_BEGIN);
    /* The transmission's scrambling, as a CRC-verified CCH half's Version would set it (issue #655). */
    const int scrambled = header->version == 3U;
    state->dpmr_scrambled = scrambled ? 2U : 1U;
    /* And its checked service, which a later CCH half whose CRC-7 failed plays by (dpmr_resolve_half_modes()). */
    state->dpmr_mode = (uint8_t)(header->mode + 1U);
    dpmr_publish_crypto(state, scrambled ? DPMR_HALF_SCRAMBLED : DPMR_HALF_CLEAR,
                        scrambled ? DSD_CALL_ENC_OBSERVED_ENCRYPTED : DSD_CALL_ENC_OBSERVED_CLEAR);
    dsd_event_sync_slot(opts, state, 0U);
    DSD_FPRINTF(stderr, "%s TG=%s Src=%s%s", KGRN, called, calling, KNRM);
    if (header->color_code >= 0) {
        DSD_FPRINTF(stderr, "%s Channel Code=%02d%s", KGRN, header->color_code, KNRM);
    }
    if (scrambled) {
        DSD_FPRINTF(stderr, "%s Scrambler%s", KRED, KNRM);
    }
}

/* Whether the header being dispatched is the next message frame after the last call header, on the same stream. */
static int
dpmr_header_follows_call_header(const dsd_state* state) {
    const dsd_dpmr_sync_mark* last = &state->dpmr_sync.call_header;
    if (last->kind != DSD_DPMR_SYNC_FRAME_HEADER || last->epoch != state->symbol_clock_epoch
        || last->carrier_seq != state->carrier_seq || last->input != state->symbol_clock_input
        || state->symbol_clock < last->clock) {
        return 0;
    }
    /* Back to back: k message frames on, within the symbol a frame may slip, k up to the frames the last header said
       follow it (Table 5.71: a powersave header's WU), so a header lost in the run does not split the call; one, the
       next frame, after a normal header. */
    const uint64_t since = state->symbol_clock - last->clock;
    const uint64_t frames = (since + 1U) / DSD_DPMR_MESSAGE_FRAME_SYMBOLS;
    const uint64_t most = state->dpmr_sync.call_header_wu != 0U ? state->dpmr_sync.call_header_wu : 1U;
    const uint64_t at = frames * DSD_DPMR_MESSAGE_FRAME_SYMBOLS;
    return frames >= 1U && frames <= most && since + 1U >= at && since <= at + 1U;
}

/* Whether the open dPMR call is the one @p header names: its called and calling IDs, service and colour code. The
   channel code travels outside the MI's CRC and may be unread in one header of the run and read in the next: a code
   unknown on either side is missing, and only two known codes that differ are two calls. */
static int
dpmr_header_names_open_call(const dsd_state* state, const dpmr_header* header) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || !DSD_SYNC_IS_DPMR(call.protocol) || call.has_service_metadata == 0U
        || call.service_options != dpmr_header_service_options(header)) {
        return 0;
    }
    char called[8];
    char calling[8];
    dpmr_convert_air_interface_id(header->called_id, called);
    dpmr_convert_air_interface_id(header->calling_id, calling);
    return strcmp(call.target_text, called) == 0 && strcmp(call.source_text, calling) == 0
           && (header->color_code < 0 || state->dpmr_sync.call_header_color_code < 0
               || header->color_code == state->dpmr_sync.call_header_color_code);
}

int
dpmr_header_continues_call(const dsd_state* state, const dpmr_header* header) {
    return state != NULL && dpmr_header_opens_call(header) && dpmr_header_follows_call_header(state)
           && dpmr_header_names_open_call(state, header);
}

/* A powersave header's WU, or -1 when @p header is no powersave header. Only the traffic channel headers whose PARMS
 * end in MI_TYPE and MI_DET (Tables 5.4 to 5.28: message types 0000 to 1010) can be one (5.5.19.1, Tables 5.69, 5.71:
 * MI_TYPE 111, WU the low four bits of MI_DET); a Broadcast (1011) gives MI_TYPE its own meanings, and the beacon and UDT
 * messages (1100, 1110, 1111; an Appended_Data header carries payload there, 5.6.2) another layout. */
static int
dpmr_header_powersave_wu(const dpmr_header* header) {
    if (header->message_type > 0xAU || header->mi_type != 7U) {
        return -1;
    }
    return (int)(header->mi_detail & 0xFU);
}

static void
dpmr_note_call_header(dsd_state* state, const dpmr_header* header, int continues) {
    dsd_dpmr_sync_mark* mark = &state->dpmr_sync.call_header;
    mark->clock = state->symbol_clock;
    mark->epoch = state->symbol_clock_epoch;
    mark->input = state->symbol_clock_input;
    mark->carrier_seq = state->carrier_seq;
    mark->sps_hunt_idx = state->sps_hunt_idx;
    mark->kind = DSD_DPMR_SYNC_FRAME_HEADER;
    mark->inverted = (uint8_t)(state->synctype == DSD_SYNC_DPMR_FS1_NEG);
    /* How many header frames this one says follow it: a powersave header's WU (Table 5.71), none for a normal one. */
    const int wu = dpmr_header_powersave_wu(header);
    state->dpmr_sync.call_header_wu = wu > 0 ? (uint8_t)wu : 0U;
    /* The run's colour code: a new call's header's own (or none); a repeat keeps it, or supplies the one it lacked. */
    if (!continues || state->dpmr_sync.call_header_color_code < 0) {
        state->dpmr_sync.call_header_color_code = header->color_code;
    }
}

void
dpmr_process_header(dsd_opts* opts, dsd_state* state, const dpmr_header* header, int continues) {
    if (state == NULL || header == NULL) {
        return;
    }
    DSD_FPRINTF(stderr, "%s%s%s", KYEL, dpmr_message_type_name(header->message_type), KNRM);
    const int wu = dpmr_header_powersave_wu(header);
    if (wu >= 0) {
        /* A powersave wake-up header: WU more follow before the normal one (Table 5.71). */
        DSD_FPRINTF(stderr, " Powersave WU=%d", wu);
    }
    if (dpmr_header_opens_call(header)) {
        /* A checked MI stands behind an exact 24-symbol FS1 (one noise window in 2^24), and a noise copy passes its
           ten Hamming words and CRC-8 about once in 2000: a header that opens a call confirms its transmission at once,
           as both CCH halves do. */
        dpmr_confirm_begin_frame(state);
        dpmr_confirm_note_evidence(state, DPMR_EVIDENCE_STRONG);
        dpmr_publish_header_call(opts, state, header, continues);
        dpmr_note_call_header(state, header, continues);
        /* A Communications_Start header whose CRC-8 passed holds a scan target (issue #890): both stations known. */
        dpmr_report_scan_activity(opts, state, header->called_id, 1, header->calling_id, 1, header->mode,
                                  header->version);
        /* The search and a scan quiet check hear it after the call is published (dpmr_confirm_end_frame()). */
        dpmr_confirm_end_frame(opts, state);
    } else if (header->message_type == DPMR_MT_COMMUNICATIONS_START && header->mode == 7U) {
        /* Communications Mode 111 is reserved (Table 5.55), as message type 1101 is (Table 5.3, which the frame sync
           refuses): a Communications_Start naming it describes nothing a transmitter sends, so it is no activity. */
        DSD_FPRINTF(stderr, " (reserved mode)");
    } else {
        /* Checked content of a message that opens no call (an ACK, a request, a poll): the channel is in use, so a
           search and a scan quiet check hear it, but it confirms no transmission. The confirmation is what lets the
           frames after it publish and play (#407), and this message opens no call whose frames may. */
        dsd_signal_search_note_evidence(state, "dPMR", 0, 0, NULL);
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
    }
    DSD_FPRINTF(stderr, "\n");
}

void
dpmr_process_end(const dsd_opts* opts, dsd_state* state, const dpmr_end_frame* end) {
    if (state == NULL || end == NULL) {
        return;
    }
    DSD_FPRINTF(stderr, "%sEnd%s", KYEL, KNRM);
    if (end->end_type == 1U) {
        DSD_FPRINTF(stderr, " Status=%u", (unsigned)end->status);
    }
    DSD_FPRINTF(stderr, "\n");
    dsd_signal_search_note_evidence(state, "dPMR", 0, 0, NULL);
    dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
}
