// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Discussion #748, NXDN: the receiver follows a site A call to voice channel F, and F is also site B's control channel.
 * A's call ends with its DISC missed, and B's control channel goes on syncing on F, dedicated (CAC) or composite
 * (RTCH_C, carrying a call of B's own). The hangtime return runs only from noCarrier(), which the synced-frames loop
 * never reaches while every frame syncs, so only the synced path can bring the receiver back to A's control channel.
 * This drives the real engine loop and the real NXDN frame decoder: a getFrameSync() that keeps finding NXDN sync on a
 * decode clock stepped 100 ms a frame, and a dibit reader that hands nxdn_frame() whole frames built as the air carries
 * them (A's CAC, then B's frames on F, then A's CAC again once back). The tunes reach a rigctl peer (__wrap_SetFreq()).
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/nxdn_channel_access.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static const uint64_t k_t0_ns = 1000000000000000000ULL;

enum { FRAMES = 80, STEP_MS = 100, HANGTIME_S = 2 };

/* Two sites of one Global system: system 2, sites 0x21 and 0x22, each with its own RAN. */
enum {
    SITE_A = (2 << 12) | 0x21,
    SITE_B = (2 << 12) | 0x22,
    RAN_A = 0x21,
    RAN_B = 0x22,
};

/* Site A's control channel, the voice channel F A's call is assigned (site B's control channel), and the channel B's
 * own call goes to. Channel notation, from the channel map. */
enum {
    CC_A_HZ = 451012500,
    F_CHANNEL = 12,
    F_HZ = 452512500,
    B_CALL_CHANNEL = 16,
    B_CALL_HZ = 453012500,
};

/* What the scenario under test puts on F once the call is followed. */
typedef enum {
    ON_F_SITE_B_CONTROL = 0,         /* site B's dedicated control channel: its CAC */
    ON_F_SITE_B_COMPOSITE = 1,       /* site B's composite control channel carrying B's call: RTCH_C */
    ON_F_SITE_B_COMPOSITE_A_RAN = 2, /* the same, its frames carrying A's RAN (a RAN reused across sites) */
    ON_F_NOTHING = 3,                /* the carrier is gone: the no-carrier return, as a control */
} on_f_kind;

static on_f_kind g_on_f = ON_F_SITE_B_CONTROL;

/* The frame the dibit reader hands nxdn_frame(), already scrambled as on the air. */
static uint8_t g_frame[182];
static size_t g_frame_pos = sizeof(g_frame);

static int g_sync_calls = 0;
static int g_followed_at = 0; /* the sync at which the receiver was first found on F */
static int g_returned_at = 0; /* the first sync after that at which it was off F again */
static int g_b_call_followed = 0;
static int g_tuned_at_last_sync = -1;
static long g_cc_at_last_sync = 0;
static long g_last_set_freq = 0;      /* where the rigctl peer was last sent */
static long g_set_freq_at_return = 0; /* ... as the receiver was first found off F */
/* What the follow to F left, which nothing heard on F may change: this site's identity, its RAN, the received
   identity's control channel and the call's voice clock. */
static unsigned int g_ran_at_follow = 0U;
static long g_site_code_at_follow = 0;
static long g_rx_cc_at_follow = 0;
static double g_voice_m_at_follow = 0.0;
static int g_changed_on_f = 0; /* the first sync on F that found one of them changed, 0 none */

static void
put_bits(uint8_t* bits, size_t at, uint32_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        bits[at + i] = (uint8_t)((value >> (width - 1U - i)) & 1U);
    }
}

/* The rate-1/2 K=5 code every NXDN channel uses (nxdn_conv_decode_soft()), over @p n input bits. */
static void
conv_encode(const uint8_t* in, size_t n, uint8_t* coded) {
    static const uint8_t PARITY[32] = {
        0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    };
    unsigned int reg = 0U;
    for (size_t i = 0; i < n; i++) {
        reg = ((reg << 1U) | in[i]) & 0x1FU;
        coded[2U * i] = PARITY[reg & 0x19U];
        coded[(2U * i) + 1U] = PARITY[reg & 0x17U];
    }
}

/*
 * An outbound CAC frame (LICH 0x01, TS 1-A §4.5.6): structure and RAN, the 147-bit message, CRC-16 to 171 bits, four
 * tail bits, coded, punctured to 300 (two of every 14) and permuted, then scrambled. The CRC is the one value that
 * leaves crc16cac() at 0 over the whole block, as the decoder checks it.
 */
static void
build_cac_frame(uint8_t ran, const uint8_t* elements, size_t bits) {
    uint8_t trellis[175];
    uint8_t coded[350];
    uint8_t punctured[300];
    uint8_t frame_bits[364];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    DSD_MEMSET(frame_bits, 0, sizeof(frame_bits));
    put_bits(trellis, 2U, ran, 6U);
    DSD_MEMCPY(&trellis[8], elements, bits);
    for (uint32_t crc = 0U; crc <= 0xFFFFU; crc++) {
        put_bits(trellis, 155U, crc, 16U);
        if (crc16cac(trellis, 171) == 0U) {
            break;
        }
    }
    conv_encode(trellis, 175U, coded);
    size_t n = 0;
    for (size_t k = 0; k < 25U; k++) {
        for (size_t j = 0; j < 14U; j++) {
            if (j != 3U && j != 11U) {
                punctured[n++] = coded[(k * 14U) + j];
            }
        }
    }
    const uint8_t lich_full = 0x02U; // LICH 0x01 with its parity bit (0)
    for (size_t i = 0; i < 8U; i++) {
        frame_bits[2U * i] = (uint8_t)((lich_full >> (7U - i)) & 1U);
        frame_bits[(2U * i) + 1U] = 1U;
    }
    for (size_t i = 0; i < 300U; i++) {
        frame_bits[16U + i] = punctured[PERM_12_25[i]];
    }
    for (size_t i = 0; i < 182U; i++) {
        g_frame[i] = (uint8_t)((frame_bits[2U * i] << 1U) | frame_bits[(2U * i) + 1U]);
    }
    nxdn_descramble_with_seed(g_frame, 182, 0U);
    g_frame_pos = 0U;
}

/* A SITE_INFO (0x18, TS 1-A §6.4.3.1) naming @p location, channel notation (144 element bits). */
static void
cac_site_info(uint8_t ran, uint32_t location) {
    uint8_t elements[144];
    DSD_MEMSET(elements, 0, sizeof(elements));
    put_bits(elements, 2U, 0x18U, 6U);
    put_bits(elements, 8U, location, 24U);
    build_cac_frame(ran, elements, sizeof(elements));
}

/* A group VCALL_ASSGN (0x04) or VCALL_ASSGN_DUP (0x05) to talkgroup @p target on @p channel. */
static void
cac_group_call(uint8_t ran, uint8_t message_type, uint32_t source, uint32_t target, uint32_t channel) {
    uint8_t elements[80];
    DSD_MEMSET(elements, 0, sizeof(elements));
    put_bits(elements, 2U, message_type, 6U);
    put_bits(elements, 16U, 1U, 3U); // group call
    put_bits(elements, 24U, source, 16U);
    put_bits(elements, 40U, target, 16U);
    put_bits(elements, 62U, channel, 10U);
    build_cac_frame(ran, elements, sizeof(elements));
}

/* A FACCH1 (TS 1-A §4.5.4): 80 element bits, CRC-12, four tail bits, coded, punctured to 144 and permuted. */
static void
facch1_encode(uint8_t out[144], const uint8_t elements[80]) {
    uint8_t trellis[96];
    uint8_t coded[192];
    uint8_t punctured[144];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    DSD_MEMCPY(trellis, elements, 80U);
    put_bits(trellis, 80U, crc12f(trellis, 80), 12U);
    conv_encode(trellis, 96U, coded);
    size_t n = 0;
    for (size_t k = 0; k < 48U; k++) {
        punctured[n++] = coded[(k * 4U) + 0U];
        punctured[n++] = coded[(k * 4U) + 2U];
        punctured[n++] = coded[(k * 4U) + 3U];
    }
    for (size_t i = 0; i < 144U; i++) {
        out[i] = punctured[PERM_16_9[i]];
    }
}

/* A SACCH segment (TS 1-A §4.5.2): structure and RAN, 18 element bits, CRC-6, four tail bits, coded, punctured to 60
 * and permuted. */
static void
sacch_encode(uint8_t out[60], uint8_t sf, uint8_t ran, const uint8_t elements[18]) {
    uint8_t trellis[36];
    uint8_t coded[72];
    uint8_t punctured[60];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    put_bits(trellis, 0U, sf, 2U);
    put_bits(trellis, 2U, ran, 6U);
    DSD_MEMCPY(&trellis[8], elements, 18U);
    put_bits(trellis, 26U, crc6(trellis, 26), 6U);
    conv_encode(trellis, 36U, coded);
    size_t n = 0;
    for (size_t k = 0; k < 6U; k++) {
        for (size_t j = 0; j < 12U; j++) {
            if (j != 5U && j != 11U) {
                punctured[n++] = coded[(k * 12U) + j];
            }
        }
    }
    for (size_t i = 0; i < 60U; i++) {
        out[i] = punctured[PERM_12_5[i]];
    }
}

/*
 * A composite control channel's outbound frame (RTCH_C, TS 1-A Table 5.2-2): the LICH @p lich with its parity, a SACCH
 * segment carrying @p ran, and two FACCH1s (the second an IDLE), scrambled. LICH 0x71 carries the FACCH1s; 0x77 carries
 * voice in their place.
 */
static void
build_composite_frame(uint8_t lich, uint8_t ran, const uint8_t* facch) {
    uint8_t bits[364];
    uint8_t idle[80];
    uint8_t sacch_elements[18];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(idle, 0, sizeof(idle));
    DSD_MEMSET(sacch_elements, 0, sizeof(sacch_elements));
    put_bits(idle, 2U, 0x10U, 6U); // IDLE

    const uint8_t full = (uint8_t)(lich << 1U);
    const uint8_t parity = (uint8_t)(((full >> 7U) + (full >> 6U) + (full >> 5U) + (full >> 4U)) & 1U);
    for (size_t i = 0; i < 8U; i++) {
        bits[2U * i] = (uint8_t)(((full | parity) >> (7U - i)) & 1U);
        bits[(2U * i) + 1U] = 1U;
    }
    sacch_encode(&bits[16], 3U, ran, sacch_elements); // a single-segment superframe position
    facch1_encode(&bits[76], facch != NULL ? facch : idle);
    facch1_encode(&bits[220], idle);
    for (size_t i = 0; i < 182U; i++) {
        g_frame[i] = (uint8_t)((bits[2U * i] << 1U) | bits[(2U * i) + 1U]);
    }
    nxdn_descramble_with_seed(g_frame, 182, 0U);
    g_frame_pos = 0U;
}

/* Site B's composite control channel carrying B's own call: its SRV_INFO (0x19) and the call's VCALL_ASSGN_DUP on
 * FACCH1 (TS 1-C §16.3.2.1), between the call's voice frames, under the SACCH RAN @p ran. */
static void
composite_frame(int n, uint8_t ran) {
    uint8_t elements[80];
    DSD_MEMSET(elements, 0, sizeof(elements));
    if (n % 3 == 0) {
        put_bits(elements, 2U, 0x19U, 6U);
        put_bits(elements, 8U, SITE_B, 24U);
        build_composite_frame(0x71U, ran, elements);
    } else if (n % 3 == 1) {
        put_bits(elements, 2U, 0x05U, 6U);
        put_bits(elements, 16U, 1U, 3U); // group call
        put_bits(elements, 24U, 2100U, 16U);
        put_bits(elements, 40U, 1100U, 16U);
        put_bits(elements, 62U, B_CALL_CHANNEL, 10U);
        build_composite_frame(0x71U, ran, elements);
    } else {
        build_composite_frame(0x77U, ran, NULL);
    }
}

/* The session as -T with a channel map leaves it on site A's control channel, before any frame. */
static void
enter_site_a_control_channel(dsd_opts* opts, dsd_state* state) {
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_hangtime = (float)HANGTIME_S;
    opts->use_rigctl = 1; /* a rigctl peer the tunes reach (__wrap_SetFreq()) */
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    state->p25_cc_freq = CC_A_HZ;
    state->trunk_cc_freq = CC_A_HZ;
    dsd_state_set_trunk_chan_freq(state, F_CHANNEL, F_HZ);
    dsd_state_set_trunk_chan_freq(state, B_CALL_CHANNEL, B_CALL_HZ);
}

/* The frame the receiver hears at sync @p n: A's control channel until A's call is followed to F, then what the
   scenario puts on F, then A's control channel again once the receiver is back. */
static int
script_frame(const dsd_opts* opts, int n) {
    const int on_f = opts->trunk_is_tuned == 1;
    if (!on_f && g_followed_at == 0) {
        if (n == 1) {
            cac_site_info(RAN_A, SITE_A);
        } else {
            cac_group_call(RAN_A, 0x04U, 2001U, 1001U, F_CHANNEL); // A's call, on F
        }
        return 1;
    }
    if (!on_f) {
        cac_site_info(RAN_A, SITE_A);
        return 1;
    }
    if (g_on_f == ON_F_NOTHING) {
        return 0;
    }
    if (g_on_f == ON_F_SITE_B_COMPOSITE || g_on_f == ON_F_SITE_B_COMPOSITE_A_RAN) {
        composite_frame(n, g_on_f == ON_F_SITE_B_COMPOSITE_A_RAN ? (uint8_t)RAN_A : (uint8_t)RAN_B);
        return 1;
    }
    /* B's control channel past A's missed DISC: its broadcasts, and a call of B's own that A's receiver must not take
       (its duplicate assignment past the hangtime of the call followed). */
    if (n % 2 == 0) {
        cac_site_info(RAN_B, SITE_B);
    } else {
        cac_group_call(RAN_B, 0x05U, 2100U, 1100U, B_CALL_CHANNEL);
    }
    return 1;
}

static void
note_receiver(const dsd_opts* opts, const dsd_state* state, int n) {
    const int on_f = opts->trunk_is_tuned == 1 && state->trunk_vc_freq[0] == F_HZ;
    if (on_f && g_followed_at == 0) {
        g_followed_at = n;
        g_ran_at_follow = state->nxdn_last_ran;
        g_site_code_at_follow = state->nxdn_location_site_code;
        g_rx_cc_at_follow = (long)state->rx_id.cc_freq_hz;
        g_voice_m_at_follow = state->last_vc_sync_time_m;
    } else if (on_f && g_changed_on_f == 0
               && (state->nxdn_last_ran != g_ran_at_follow || state->nxdn_location_site_code != g_site_code_at_follow
                   || (long)state->rx_id.cc_freq_hz != g_rx_cc_at_follow
                   || state->last_vc_sync_time_m > g_voice_m_at_follow || !dsd_nxdn_site_is(state, SITE_A))) {
        g_changed_on_f = n;
    }
    if (!on_f && g_followed_at != 0 && g_returned_at == 0) {
        g_returned_at = n;
        g_set_freq_at_return = g_last_set_freq;
    }
    if (g_last_set_freq == B_CALL_HZ) {
        g_b_call_followed = 1;
    }
    g_tuned_at_last_sync = opts->trunk_is_tuned;
    g_cc_at_last_sync = state->trunk_cc_freq;
}

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft);
int __wrap_getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft);
int __wrap_getFrameSync(dsd_opts* opts, dsd_state* state);
bool __wrap_SetFreq(dsd_socket_t sockfd, long int freq);

bool
__wrap_SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    g_last_set_freq = freq;
    return true;
}

int
__wrap_getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    if (g_frame_pos >= sizeof(g_frame)) {
        return __real_getDibitSoft(opts, state, out_soft);
    }
    if (out_soft != NULL) {
        DSD_MEMSET(out_soft, 0, sizeof(*out_soft));
        out_soft->reliability = 255U;
    }
    return g_frame[g_frame_pos++];
}

int
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    g_sync_calls++;
    dsd_decode_clock_test_set_ns(k_t0_ns + (uint64_t)g_sync_calls * STEP_MS * 1000000ULL);
    if (g_sync_calls == 1) {
        enter_site_a_control_channel(opts, state);
    }
    note_receiver(opts, state, g_sync_calls);
    g_frame_pos = sizeof(g_frame);
    if (g_sync_calls <= FRAMES && script_frame(opts, g_sync_calls)) {
        /* As the NXDN matcher accepts: the frame's sync reject (nxdn_finalize_sync_reject()) reads the protocol from
           it. */
        state->lastsynctype = DSD_SYNC_NXDN_POS;
        return DSD_SYNC_NXDN_POS;
    }
    if (g_sync_calls <= FRAMES) {
        return DSD_SYNC_NONE;
    }
    dsd_exitflag_store(1);
    return DSD_SYNC_NONE;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static int
run_scenario(on_f_kind kind) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }
    g_on_f = kind;
    g_sync_calls = 0;
    g_followed_at = 0;
    g_returned_at = 0;
    g_b_call_followed = 0;
    g_tuned_at_last_sync = -1;
    g_cc_at_last_sync = 0;
    g_last_set_freq = 0;
    g_set_freq_at_return = 0;
    g_changed_on_f = 0;
    dsd_exitflag_store(0);
    dsd_decode_clock_use_test(k_t0_ns);
    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;

    static const char* const k_names[] = {
        "site B's control channel on F",
        "site B's composite control channel on F",
        "site B's composite control channel on F with A's RAN",
        "carrier gone from F",
    };
    const char* const name = k_names[kind];
    int rc = 0;
    if (dsd_engine_run_with_lifecycle(opts, state, NULL) != 0) {
        DSD_FPRINTF(stderr, "%s: engine run failed\n", name);
        rc = 1;
    }
    /* The followed call's last activity is its tune. The hangtime is counted in whole seconds of the decode clock
       (no_carrier_is_cc_return_due()), so the return is due between the hangtime and a second more after it, and the
       sync after the frame that finds it due is the first off F. Not before: the call's own hangtime holds. */
    const int syncs_a_second = 1000 / STEP_MS;
    const int due_from = g_followed_at + (HANGTIME_S * syncs_a_second);
    const int due_by = g_followed_at + ((HANGTIME_S + 1) * syncs_a_second) + 2;
    if (g_followed_at == 0 || g_returned_at < due_from || g_returned_at > due_by || g_tuned_at_last_sync != 0
        || g_cc_at_last_sync != CC_A_HZ || g_set_freq_at_return != CC_A_HZ || g_b_call_followed) {
        DSD_FPRINTF(stderr,
                    "FAIL %s: followed to F at sync %d, back at sync %d (due from %d to %d, hangtime %d s, %d ms a "
                    "sync) with the peer sent to %ld, trunk_is_tuned=%d and control channel %ld at the last sync, B's "
                    "call followed=%d\n",
                    name, g_followed_at, g_returned_at, due_from, due_by, HANGTIME_S, STEP_MS, g_set_freq_at_return,
                    g_tuned_at_last_sync, g_cc_at_last_sync, g_b_call_followed);
        rc = 1;
    }
    if (kind != ON_F_NOTHING && !dsd_nxdn_site_is(state, SITE_A)) {
        DSD_FPRINTF(stderr, "FAIL %s: this site is no longer site A\n", name);
        rc = 1;
    }
    if (g_changed_on_f != 0) {
        DSD_FPRINTF(stderr,
                    "FAIL %s: at sync %d on F the site, its RAN, the received control channel or the call's voice "
                    "clock changed (at the follow: RAN %u, site code %ld, received CC %ld)\n",
                    name, g_changed_on_f, g_ran_at_follow, g_site_code_at_follow, g_rx_cc_at_follow);
        rc = 1;
    }

    freeState(state);
    free(state);
    free(opts);
    dsd_decode_clock_use_system();
    return rc;
}

int
main(void) {
    int rc = run_scenario(ON_F_SITE_B_CONTROL);
    rc |= run_scenario(ON_F_SITE_B_COMPOSITE);
    rc |= run_scenario(ON_F_SITE_B_COMPOSITE_A_RAN);
    rc |= run_scenario(ON_F_NOTHING);
    if (rc == 0) {
        printf("ENGINE_NXDN_FOREIGN_CONTROL_RETURN: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
