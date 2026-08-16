// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A channel-grant announcement heard in a voice channel's MAC signaling
 * describes a call on some other channel or slot. It must not overwrite the
 * decode slot's own per-slot service options (dmr_so / dmr_soR): those bits
 * feed p25p2_prepare_voice_crypto(), where a foreign grant's encrypted service
 * bit flips a clear call to ENCRYPTED_PENDING mid-transmission and, under
 * encryption lockout, closes its audio gate. Only the voice-channel-user MCOs,
 * which describe the decode slot's own call, may store service options.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Stubs to satisfy external references

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

static dsd_trunk_tune_result
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc_request = return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

static int
expect_eq(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
seed_call(dsd_state* state, uint8_t slot, uint64_t target) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .observed_m = 1.0,
    };
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0;
}

// Tuned TDMA carrier with a clear group call decoding on each slot. Slot 0's
// own service options are 0x00, slot 1's 0x20 (duplex, both clear).
static void
setup_dual_clear_calls(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->trunk_tune_enc_calls = 0;
    opts->trunk_tune_group_calls = 1;
    state->currentslot = 0;
    state->synctype = DSD_SYNC_P25P2_POS;
    state->lastsynctype = DSD_SYNC_P25P2_POS;
    state->dmr_so = 0x00U;
    state->dmr_soR = 0x20U;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state->p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    state->p25_p2_audio_allowed[0] = 1;
    state->p25_p2_audio_allowed[1] = 1;
    (void)seed_call(state, 0U, 1234);
    (void)seed_call(state, 1U, 2345);

    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    p25_sm_init_ctx(sm, opts, state);
    sm->state = P25_SM_TUNED;
    sm->vc_is_tdma = 1;
    sm->vc_freq_hz = 851500000;
    for (int slot = 0; slot < 2; slot++) {
        sm->slots[slot].grant_active = 1;
        sm->slots[slot].freq_hz = sm->vc_freq_hz;
        sm->slots[slot].target_id = slot == 0 ? 1234 : 2345;
        sm->slots[slot].ota_tg = sm->slots[slot].target_id;
        sm->slots[slot].is_group = 1;
        sm->slots[slot].voice_active = 1;
    }
}

typedef struct {
    const char* name;
    unsigned long long mac[24];
} foreign_announcement;

// Every announcement below names TG 0x7788 with encrypted service (0x44) on
// channel 0x100A -- a call that is not on either of this carrier's slots. The
// channel is deliberately absent from the iden/chan tables so the grant is
// undispatchable and the store is the only observable side effect.
static const foreign_announcement k_foreign_announcements[] = {
    {
        .name = "0x40 group voice channel grant",
        .mac = {0, 0x40, 0x44, 0x10, 0x0A, 0x77, 0x88, 0x0A, 0x0B, 0x0C},
    },
    {
        .name = "0xC0 group voice channel grant explicit",
        .mac = {0, 0xC0, 0x44, 0x10, 0x0A, 0x10, 0x0B, 0x77, 0x88, 0x0A, 0x0B, 0x0C},
    },
    {
        .name = "0xA3 mfid90 regroup channel grant implicit",
        .mac = {0, 0xA3, 0x90, 0x00, 0x44, 0x10, 0x0A, 0x77, 0x88, 0x0A, 0x0B, 0x0C},
    },
    {
        .name = "0xA4 mfid90 regroup channel grant explicit",
        .mac = {0, 0xA4, 0x90, 0x00, 0x44, 0x10, 0x0A, 0x10, 0x0B, 0x77, 0x88, 0x0A, 0x0B, 0x0C},
    },
    {
        .name = "0x83 mfid90 regroup voice channel update",
        .mac = {0, 0x83, 0x90, 0x44, 0x77, 0x88, 0x10, 0x0A},
    },
    {
        .name = "0x48 telephone interconnect voice channel grant",
        .mac = {0, 0x48, 0x00, 0x44, 0x10, 0x0A, 0x00, 0x64, 0x0A, 0x0B, 0x0C},
    },
};

static int
test_foreign_announcements_do_not_rewrite_slot_svc(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    const size_t count = sizeof(k_foreign_announcements) / sizeof(k_foreign_announcements[0]);
    for (size_t i = 0; i < count; i++) {
        const foreign_announcement* fa = &k_foreign_announcements[i];
        char tag[128];

        // FACCH while decoding slot 0: the announcement rides the clear call's
        // own signaling.
        setup_dual_clear_calls(&opts, &state);
        unsigned long long mac[24];
        DSD_MEMCPY(mac, fa->mac, sizeof mac);
        process_MAC_VPDU(&opts, &state, /*type FACCH*/ 0, P25_MAC_PDU_ACTIVE, mac);

        DSD_SNPRINTF(tag, sizeof tag, "%s: facch slot0 svc preserved", fa->name);
        rc |= expect_eq(tag, (long)state.dmr_so, 0x00);
        DSD_SNPRINTF(tag, sizeof tag, "%s: facch slot1 svc preserved", fa->name);
        rc |= expect_eq(tag, (long)state.dmr_soR, 0x20);
        dsd_state_ext_free_all(&state);

        // SACCH decoded in physical slot 1 maps to voice slot 0: the inverted
        // attribution must not contaminate slot 0 either.
        setup_dual_clear_calls(&opts, &state);
        state.currentslot = 1;
        DSD_MEMCPY(mac, fa->mac, sizeof mac);
        process_MAC_VPDU(&opts, &state, /*type SACCH*/ 1, P25_MAC_PDU_ACTIVE, mac);

        DSD_SNPRINTF(tag, sizeof tag, "%s: sacch slot0 svc preserved", fa->name);
        rc |= expect_eq(tag, (long)state.dmr_so, 0x00);
        DSD_SNPRINTF(tag, sizeof tag, "%s: sacch slot1 svc preserved", fa->name);
        rc |= expect_eq(tag, (long)state.dmr_soR, 0x20);
        dsd_state_ext_free_all(&state);
    }

    return rc;
}

// The voice-channel-user MCO describes the decode slot's own call and must
// keep storing its service options -- the guard above must not overcorrect.
static int
test_own_voice_channel_user_still_stores_svc(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    setup_dual_clear_calls(&opts, &state);
    unsigned long long mac[24] = {0};
    mac[1] = 0x01; // Group Voice Channel User, abbreviated
    mac[2] = 0x24; // Duplex, priority 4, clear
    mac[3] = 0x04;
    mac[4] = 0xD2; // TG 1234 (slot 0's own call)
    mac[5] = 0x00;
    mac[6] = 0x00;
    mac[7] = 0x2A;
    process_MAC_VPDU(&opts, &state, /*type FACCH*/ 0, P25_MAC_PDU_ACTIVE, mac);

    rc |= expect_eq("own voice user: slot0 svc stored", (long)state.dmr_so, 0x24);
    rc |= expect_eq("own voice user: slot1 svc preserved", (long)state.dmr_soR, 0x20);
    dsd_state_ext_free_all(&state);
    return rc;
}

int
main(void) {
    install_trunk_tuning_hooks();

    int rc = 0;
    rc |= test_foreign_announcements_do_not_rewrite_slot_svc();
    rc |= test_own_voice_channel_user_still_stores_svc();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25 P2 VPDU FOREIGN GRANT SVC: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
