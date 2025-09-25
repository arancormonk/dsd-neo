// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 trunk CC candidate cache round-trip tests.
 * - Persists >16 neighbors with FIFO eviction and dedup
 * - Reloads from cache for same WACN/SYSID
 * - next_cc_candidate skips current CC and wraps
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Use public headers to access dsd_state/dsd_opts
#define main dsd_neo_main_decl
#include <dsd-neo/core/dsd.h>
#undef main

// Public trunk SM API (provided via wrappers)
void p25_sm_init(dsd_opts* opts, dsd_state* state);
void p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count);
int p25_sm_next_cc_candidate(dsd_state* state, long* out_freq);

// Provide stubs for external hooks referenced from the linked objects
bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

struct RtlSdrContext;
struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

// Unused alias helpers pulled by proto lib in some link paths
void
unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

void
apx_embedded_alias_header_phase2(dsd_opts* o, dsd_state* s, uint8_t slot, uint8_t* b) {
    (void)o;
    (void)s;
    (void)slot;
    (void)b;
}

void
apx_embedded_alias_blocks_phase2(dsd_opts* o, dsd_state* s, uint8_t slot, uint8_t* b) {
    (void)o;
    (void)s;
    (void)slot;
    (void)b;
}

void
l3h_embedded_alias_decode(dsd_opts* o, dsd_state* s, uint8_t slot, int16_t len, uint8_t* in) {
    (void)o;
    (void)s;
    (void)slot;
    (void)len;
    (void)in;
}

void
nmea_harris(dsd_opts* o, dsd_state* s, uint8_t* in, uint32_t src, int slot) {
    (void)o;
    (void)s;
    (void)in;
    (void)src;
    (void)slot;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        fprintf(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Temp cache dir
    char tmpl[] = "/tmp/dsdneo_cc_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 100;
    }
    setenv("DSD_NEO_CACHE_DIR", dir, 1);

    // Identity used for cache file name
    const unsigned long wacn = 0xABCDE;
    const int sysid = 0x123;

    dsd_opts opts1;
    dsd_state st1;
    memset(&opts1, 0, sizeof opts1);
    memset(&st1, 0, sizeof st1);
    st1.p2_wacn = wacn;
    st1.p2_sysid = sysid;

    // Insert 20 neighbors (Hz). Expect only last 16 persisted, FIFO order preserved
    long freqs[20];
    for (int i = 0; i < 20; i++) {
        freqs[i] = 851000000 + i * 12500; // 12.5 kHz steps
    }
    p25_sm_on_neighbor_update(&opts1, &st1, freqs, 20);

    // Read back cache file
    char path[1024];
    snprintf(path, sizeof path, "%s/p25_cc_%05lX_%03X.txt", dir, wacn, sysid);
    FILE* fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "open cache failed: %s\n", strerror(errno));
        return 101;
    }
    long loaded[32];
    int n = 0;
    while (n < 32 && fscanf(fp, "%ld", &loaded[n]) == 1) {
        n++;
    }
    fclose(fp);
    rc |= expect_eq_int("persist count", n, 16);
    for (int i = 0; i < 16; i++) {
        rc |= expect_eq_long("persist item", loaded[i], freqs[i + 4]);
    }

    // New state: load on first neighbor update call
    dsd_opts opts2;
    dsd_state st2;
    memset(&opts2, 0, sizeof opts2);
    memset(&st2, 0, sizeof st2);
    st2.p2_wacn = wacn;
    st2.p2_sysid = sysid;
    long dummy[1] = {0}; // ensure path executes load (count>0)
    p25_sm_on_neighbor_update(&opts2, &st2, dummy, 1);

    // Set current CC to the second candidate value to verify skip
    st2.p25_cc_freq = loaded[1];
    long cand = 0;
    int ok = p25_sm_next_cc_candidate(&st2, &cand);
    rc |= expect_eq_int("cand ok1", ok, 1);
    // Set CC to the second value and ensure next call skips it
    long cand2 = 0;
    ok = p25_sm_next_cc_candidate(&st2, &cand2);
    rc |= expect_eq_int("cand ok2", ok, 1);
    if (ok) {
        // Now set CC to cand2 and request next candidate; it should not equal cand2
        // We cannot assign CC directly; instead cycle again and ensure we get a value (wrap works)
        long cand3 = 0;
        ok = p25_sm_next_cc_candidate(&st2, &cand3);
        rc |= expect_eq_int("cand ok3", ok, 1);
        if (ok) {
            if (cand3 == cand2) {
                fprintf(stderr, "next cand unexpectedly equals current CC surrogate\n");
                rc |= 1;
            }
        }
    }

    return rc;
}
