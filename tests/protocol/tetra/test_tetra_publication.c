// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * What the TETRA decoder publishes for the frontends (dsd_state::tetra_pub): taken from its private state at the end of
 * a burst, nothing before a block has decoded, the timeslot bitmap TN1 first, no key, and cleared with the state.
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <stdint.h>
#include <stdlib.h>
#include "tetra_state.h"

static dsd_state*
state_open(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    return state;
}

static void
state_close(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

/* A sync word alone is no evidence: the publication holds no mode until a block has passed its CRC. */
static void
test_nothing_before_a_decoded_block(void) {
    dsd_state* state = state_open();
    tetra_publish(NULL);
    tetra_publish(state); /* no TETRA state at all: the zero default */
    assert(state->tetra_pub.mode == DSD_TETRA_MODE_NONE && state->tetra_pub.net_known == 0U);
    tetra_state_t* const ts = tetra_st(state);
    assert(ts != NULL);
    tetra_publish(state); /* a TETRA state, but no block has passed its CRC yet */
    assert(state->tetra_pub.mode == DSD_TETRA_MODE_NONE);
    ts->decode_ok = 1U;
    tetra_publish(state);
    assert(state->tetra_pub.mode == DSD_TETRA_MODE_TMO_DOWNLINK);
    state_close(state);
}

/* A trunked downlink: network, encryption, the network's TEA, call following, channels, MM status and codec. */
static void
test_trunked_downlink(void) {
    dsd_state* state = state_open();
    tetra_state_t* const ts = tetra_st(state);
    assert(ts != NULL);
    ts->decode_ok = 3U;
    ts->net_known = 1U;
    ts->mcc = 262U;
    ts->mnc = 1001U;
    ts->colour = 0x19U;
    ts->frames_resource = 1U;
    ts->enc_mode = TETRA_ENC_MODE_NONE;
    ts->sm.state = (uint8_t)TETRA_SM_ON_CC;
    ts->frames_sysinfo = 2U;
    ts->sysinfo_main_carrier = 321U;
    ts->dl_carrier_hz = 392012500L;
    ts->mm_status_valid = 1U;
    ts->mm_status_code = 7U;
    ts->voice.codec_status = (uint8_t)TETRA_CODEC_STATUS_READY;
    ts->voice.frames = 18U;
    ts->voice.codec_errors = 2U;
    tetra_publish(state);
    const dsd_tetra_publication* const pub = &state->tetra_pub;
    assert(pub->mode == DSD_TETRA_MODE_TMO_DOWNLINK);
    assert(pub->net_known == 1U && pub->mcc == 262U && pub->mnc == 1001U && pub->colour == 0x19U);
    assert(pub->air_encryption == DSD_TETRA_AIR_ENC_CLEAR && pub->tea == 0U);
    assert(pub->trunk_state == DSD_TETRA_TRUNK_ON_CONTROL);
    assert(pub->cc_known == 1U && pub->cc_carrier == 321U && pub->cc_freq_hz == 392012500U);
    assert(pub->traffic_valid == 0U && pub->traffic_slots == 0U);
    assert(pub->mm_status_valid == 1U && pub->mm_status_code == 7U);
    assert(pub->codec_status == DSD_TETRA_CODEC_READY && pub->codec_frames == 18U && pub->codec_errors == 2U);

    /* A followed call on timeslots 1 and 3 (bit 3 is TN1), encrypted signalling, and the network's KSG from OTAR. */
    ts->sm.followed.carrier = 654U;
    ts->sm.followed.timeslots = 0x0AU;
    ts->sm.followed.freq_hz = 392125000L;
    ts->sm.followed.call_id = 0x55U;
    ts->enc_mode = TETRA_ENC_MODE_EVEN;
    ts->sm.state = (uint8_t)TETRA_SM_TUNED;
    assert(tetra_air_ksg_remember(state, 262U, 1001U, 1U) == 1);
    tetra_publish(state);
    assert(pub->traffic_valid == 1U && pub->traffic_carrier == 654U && pub->traffic_slots == 0x0AU);
    assert(pub->traffic_freq_hz == 392125000U && pub->trunk_state == DSD_TETRA_TRUNK_ON_TRAFFIC);
    assert(pub->air_encryption == DSD_TETRA_AIR_ENC_ENCRYPTED && pub->tea == 2U);
    ts->enc_mode = TETRA_ENC_MODE_ODD;
    tetra_publish(state);
    assert(pub->air_encryption == DSD_TETRA_AIR_ENC_ENCRYPTED);

    /* No timeslot is no traffic channel; an unresolved or impossible frequency publishes as 0. */
    ts->sm.followed.timeslots = 0U;
    ts->dl_carrier_hz = -5L;
    ts->enc_mode = TETRA_ENC_MODE_RESERVED;
    tetra_publish(state);
    assert(pub->traffic_valid == 0U && pub->traffic_carrier == 0U && pub->traffic_freq_hz == 0U);
    assert(pub->cc_freq_hz == 0U && pub->air_encryption == DSD_TETRA_AIR_ENC_UNKNOWN);
    state_close(state);
}

/* The codec statuses the decoder reports, each on the publication's, with the frame and error counts. */
static void
test_codec_status(void) {
    static const struct {
        tetra_codec_status decoder;
        uint8_t last_decode_failed;
        uint8_t published;
    } map[] = {
        {TETRA_CODEC_STATUS_UNLOADED, 0U, DSD_TETRA_CODEC_UNKNOWN},
        {TETRA_CODEC_STATUS_READY, 0U, DSD_TETRA_CODEC_READY},
        {TETRA_CODEC_STATUS_READY, 1U, DSD_TETRA_CODEC_DECODE_ERRORS},
        {TETRA_CODEC_STATUS_NONE, 0U, DSD_TETRA_CODEC_MISSING},
        {TETRA_CODEC_STATUS_FAILED, 0U, DSD_TETRA_CODEC_LOAD_FAILED},
    };

    dsd_state* state = state_open();
    tetra_state_t* const ts = tetra_st(state);
    assert(ts != NULL);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        ts->voice.codec_status = (uint8_t)map[i].decoder;
        ts->voice.last_decode_failed = map[i].last_decode_failed;
        tetra_publish(state);
        assert(state->tetra_pub.codec_status == map[i].published);
    }
    ts->voice.frames = 18U;
    ts->voice.codec_errors = 2U;
    tetra_publish(state);
    assert(state->tetra_pub.codec_frames == 18U && state->tetra_pub.codec_errors == 2U);
    state_close(state);
}

/* Direct mode: the communication type names the mode, the call's security class the air encryption (end-to-end
 * encryption leaves the air interface clear), and no KSG. */
static void
test_direct_mode_and_uplink(void) {
    static const uint8_t dmo_modes[4] = {
        DSD_TETRA_MODE_DMO_DIRECT,
        DSD_TETRA_MODE_DMO_REPEATER,
        DSD_TETRA_MODE_DMO_GATEWAY,
        DSD_TETRA_MODE_DMO_REPEATER_GATEWAY,
    };
    static const uint8_t crypts[4] = {
        TETRA_DMO_CRYPT_UNKNOWN,
        TETRA_DMO_CRYPT_CLEAR,
        TETRA_DMO_CRYPT_AIR,
        TETRA_DMO_CRYPT_E2E,
    };
    static const uint8_t air_encryption[4] = {
        DSD_TETRA_AIR_ENC_UNKNOWN,
        DSD_TETRA_AIR_ENC_CLEAR,
        DSD_TETRA_AIR_ENC_ENCRYPTED,
        DSD_TETRA_AIR_ENC_CLEAR,
    };
    dsd_state* state = state_open();
    tetra_state_t* const ts = tetra_st(state);
    assert(ts != NULL);
    ts->decode_ok = 1U;
    ts->dmo = 1U;
    ts->net_known = 1U;
    assert(tetra_air_ksg_remember(state, 0U, 0U, 0U) == 1);
    for (uint8_t comm = 0; comm < 4U; comm++) {
        ts->dmo_comm_type = comm;
        ts->dmo_crypt = crypts[comm];
        tetra_publish(state);
        assert(state->tetra_pub.mode == dmo_modes[comm]);
        assert(state->tetra_pub.air_encryption == air_encryption[comm]);
        assert(state->tetra_pub.tea == 0U);
    }

    ts->dmo = 0U;
    ts->uplink = 1U;
    state->tetra_sync.cb = 0U;
    tetra_publish(state);
    assert(state->tetra_pub.mode == DSD_TETRA_MODE_TMO_UPLINK);
    state->tetra_sync.cb = 1U;
    tetra_publish(state);
    assert(state->tetra_pub.mode == DSD_TETRA_MODE_TMO_CONTROL_UPLINK);
    state_close(state);
}

/* The SCH/S of a real direct mode DSB (EN 300 396-3 table 21): a DMAC-SYNC, direct communication, DM-1. */
static const char* k_dmo_sch_s = "110100000000000001100000000000000000000000000000000000000000";

/* What the publication says of a trunked network -- its MCC, MNC and colour code, its D-MM STATUS, the SYSINFO control
 * channel and a traffic channel of its calls -- describes a trunked mode carrier: a direct mode carrier has none of
 * them, so after a retune from a TMO cell to a DMO carrier (the TETRA state kept, as the network identity is the
 * session's) the publication shows none, and back on the trunked carrier the network shows again. */
static void
test_direct_mode_publishes_no_trunked_network(void) {
    dsd_state* state = state_open();
    tetra_state_t* const ts = tetra_st(state);
    assert(ts != NULL);
    ts->decode_ok = 1U;
    ts->net_known = 1U;
    ts->mcc = 262U;
    ts->mnc = 1001U;
    ts->colour = 0x19U;
    ts->frames_sysinfo = 1U;
    ts->sysinfo_main_carrier = 321U;
    ts->dl_carrier_hz = 392012500L;
    ts->mm_status_valid = 1U;
    ts->mm_status_code = 7U;
    ts->calls.call[0].phase = TETRA_CALL_SETUP;
    ts->calls.call[0].call_id = 0x0AU;
    ts->calls.call[0].alloc.carrier = 330U;
    ts->calls.call[0].alloc.timeslots = 0x04U;
    ts->calls.call[0].alloc.freq_hz = 392237500L;
    tetra_publish(state);
    const dsd_tetra_publication* const pub = &state->tetra_pub;
    assert(pub->mode == DSD_TETRA_MODE_TMO_DOWNLINK && pub->net_known == 1U && pub->mcc == 262U);
    assert(pub->mm_status_valid == 1U && pub->cc_known == 1U && pub->traffic_valid == 1U);

    uint8_t bits[64] = {0};
    for (size_t i = 0; k_dmo_sch_s[i] != '\0'; i++) {
        bits[i] = (uint8_t)(k_dmo_sch_s[i] == '1');
    }
    tetra_dmo_sync_s_t s;
    assert(tetra_dmo_parse_sync_s(bits, &s) == 1 && s.sync_pdu_type == TETRA_DMO_SYNC_DMAC);
    tetra_forget_carrier(state);
    tetra_dmo_apply_sync(state, &s, NULL);
    assert(ts->net_known == 1U && ts->mcc == 262U && ts->mm_status_valid == 1U);
    assert(pub->mode == DSD_TETRA_MODE_DMO_DIRECT);
    assert(pub->net_known == 0U && pub->mcc == 0U && pub->mnc == 0U && pub->colour == 0U);
    assert(pub->mm_status_valid == 0U && pub->mm_status_code == 0U);
    assert(pub->cc_known == 0U && pub->cc_carrier == 0U && pub->cc_freq_hz == 0U);
    assert(pub->traffic_valid == 0U && pub->traffic_carrier == 0U && pub->traffic_slots == 0U);

    tetra_dmo_leave(NULL, state);
    tetra_publish(state);
    assert(pub->mode == DSD_TETRA_MODE_TMO_DOWNLINK && pub->net_known == 1U && pub->mcc == 262U);
    assert(pub->mnc == 1001U && pub->colour == 0x19U && pub->mm_status_valid == 1U && pub->cc_known == 1U);
    state_close(state);
}

/* Dropping the TETRA state drops what it published, and bumps the generation for the frontends. */
static void
test_reset_clears_the_publication(void) {
    dsd_state* state = state_open();
    tetra_state_t* const ts = tetra_st(state);
    assert(ts != NULL);
    ts->decode_ok = 1U;
    ts->net_known = 1U;
    ts->mcc = 1U;
    tetra_publish(state);
    const uint32_t generation = state->tetra_pub.generation;
    assert(state->tetra_pub.mode != DSD_TETRA_MODE_NONE);
    tetra_state_reset(state);
    assert(state->tetra_pub.mode == DSD_TETRA_MODE_NONE && state->tetra_pub.net_known == 0U);
    assert(state->tetra_pub.mcc == 0U && state->tetra_pub.generation == generation + 1U);
    tetra_publish(state); /* the next burst starts from the fresh state */
    assert(state->tetra_pub.mode == DSD_TETRA_MODE_NONE && state->tetra_pub.generation == generation + 1U);
    state_close(state);
}

int
main(void) {
    test_nothing_before_a_decoded_block();
    test_trunked_downlink();
    test_codec_status();
    test_direct_mode_and_uplink();
    test_direct_mode_publishes_no_trunked_network();
    test_reset_clears_the_publication();
    return 0;
}
