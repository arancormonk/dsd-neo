// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Exercise P_CLEAR with TG Hold override forcing immediate SM release.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <dsd-neo/core/dsd.h>

// Stubs and helpers (same pattern as other DMR tests)
uint64_t
ConvertBitIntoBytes(uint8_t* bits, uint32_t n) {
    uint64_t v = 0ULL;
    for (uint32_t i = 0; i < n; i++) {
        v = (v << 1) | (bits[i] & 1);
    }
    return v;
}

void
watchdog_event_history(dsd_opts* o, dsd_state* s, uint8_t sl) {
    (void)o;
    (void)s;
    (void)sl;
}

void
watchdog_event_current(dsd_opts* o, dsd_state* s, uint8_t sl) {
    (void)o;
    (void)s;
    (void)sl;
}

void
watchdog_event_datacall(dsd_opts* o, dsd_state* s, uint32_t a, uint32_t b, char* c, uint8_t d) {
    (void)o;
    (void)s;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
}

void
rotate_symbol_out_file(dsd_opts* o, dsd_state* s) {
    (void)o;
    (void)s;
}
struct RtlSdrContext;
struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* c, uint32_t f) {
    (void)c;
    (void)f;
    return 0;
}

bool
SetFreq(int fd, long int f) {
    (void)fd;
    (void)f;
    return false;
}

bool
SetModulation(int fd, int bw) {
    (void)fd;
    (void)bw;
    return false;
}

long int
GetCurrentFreq(int fd) {
    (void)fd;
    return 0;
}

void
dmr_reset_blocks(dsd_opts* o, dsd_state* s) {
    (void)o;
    (void)s;
}

uint8_t
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

void
dsd_drain_audio_output(dsd_opts* opts) {
    (void)opts;
}

void
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    if (!opts || !state || freq <= 0) {
        return;
    }
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->last_vc_sync_time = time(NULL);
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (opts) {
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    }
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);
extern void dmr_sm_on_group_grant(dsd_opts*, dsd_state*, long, int, int, int);

static void
init_env(dsd_opts* o, dsd_state* s) {
    memset(o, 0, sizeof(*o));
    memset(s, 0, sizeof(*s));
    o->p25_trunk = 1;
    o->trunk_enable = 1;
    s->p25_cc_freq = 851000000;
}

static void
build_pclear(uint8_t* bits, uint8_t* bytes) {
    memset(bits, 0, 256);
    memset(bytes, 0, 48);
    bytes[0] = (uint8_t)(46 & 0x3F);
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    dsd_opts opts;
    dsd_state state;
    init_env(&opts, &state);
    // 1) Tune to VC via SM grant call
    dmr_sm_on_group_grant(&opts, &state, /*freq_hz*/ 852000000, /*lpcn*/ 0, /*tg*/ 1234, /*src*/ 42);
    assert(opts.p25_is_tuned == 1);
    // Set TG Hold to match active TG; ensure slot 0 context
    state.lasttg = 1234;
    state.tg_hold = 1234;
    state.currentslot = 0;
    // 2) P_CLEAR should force release via SM (bypass hangtime/activity)
    uint8_t bits[256], bytes[48];
    build_pclear(bits, bytes);
    dmr_cspdu(&opts, &state, bits, bytes, 1, 0);
    assert(opts.p25_is_tuned == 0);
    printf("DMR_T3_FORCE_RELEASE: OK\n");
    return 0;
}
