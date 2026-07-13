// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 TSBK → vPDU bridge test (Group Voice Channel Grant).
 *
 * Builds a minimal TSBK-mapped vPDU (DUID=0x07, opcode=0x40) and feeds it to
 * process_MAC_VPDU. Verifies that the canonical state machine accepts the
 * expected channel, service bits, talkgroup, and source when trunking is
 * enabled and IDEN tables allow channel→frequency mapping.
 */

#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Alias decode helper stubs referenced by VPDU handler
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

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

// Rigctl/rtl stubs referenced by linked objects
bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct RtlSdrContext* g_rtl_ctx = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
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
expect_str_has(const char* tag, const char* haystack, const char* needle) {
    if (!haystack || !strstr(haystack, needle)) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle, haystack ? haystack : "(null)");
        return 1;
    }
    return 0;
}

// Test shim entry (implemented in tests/test_support/p25_test_shim.c)
void p25_test_invoke_mac_vpdu_with_state(const unsigned char* mac_bytes, int mac_len, int p25_trunk, long p25_cc_freq,
                                         int iden, int type, int tdma, long base, int spac);
void p25_test_process_mac_vpdu(int type, const unsigned char* mac_bytes, int mac_len);
const char* p25_extended_function_class0_operand_label(uint8_t operand);
int p25_extended_function_operand_is_ack(uint8_t operand);

static int
test_extended_function_command_abbreviated(void) {
    int rc = 0;

    rc |= expect_str_has("extfn label check", p25_extended_function_class0_operand_label(0x00), "Radio Check");
    rc |= expect_str_has("extfn label inhibit", p25_extended_function_class0_operand_label(0x7F), "Radio Inhibit");
    rc |= expect_str_has("extfn label detach", p25_extended_function_class0_operand_label(0x7D), "Radio Detach");
    rc |= expect_str_has("extfn label uninhibit", p25_extended_function_class0_operand_label(0x7E), "Radio Uninhibit");
    rc |= expect_str_has("extfn label reserved", p25_extended_function_class0_operand_label(0x01), "Reserved");
    rc |= expect_str_has("extfn ack masks label", p25_extended_function_class0_operand_label(0x80), "Radio Check");
    rc |= expect_eq("extfn ack bit", p25_extended_function_operand_is_ack(0xFF), 1);
    rc |= expect_eq("extfn no ack bit", p25_extended_function_operand_is_ack(0x7F), 0);

    unsigned char mac[24] = {0};
    mac[0] = 0x01;
    mac[1] = 0x64;
    mac[2] = 0x00;
    mac[3] = 0xFF;
    mac[4] = 0x12;
    mac[5] = 0x34;
    mac[6] = 0x56;
    mac[7] = 0xAB;
    mac[8] = 0xCD;
    mac[9] = 0xEF;

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_ext_fn_0x64") != 0) {
        DSD_FPRINTF(stderr, "capture stderr begin failed: %s\n", strerror(errno));
        return rc | 100;
    }
    p25_test_process_mac_vpdu(0, mac, 10);
    dsd_test_capture_stderr_end(&cap);

    FILE* f = fopen(cap.path, "r");
    if (!f) {
        DSD_FPRINTF(stderr, "open capture failed: %s\n", strerror(errno));
        return rc | 101;
    }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    rc |= expect_str_has("extfn output title", buf, "Extended Function Command - Abbreviated");
    rc |= expect_str_has("extfn output class operand", buf, "Class: 00 Operand: FF");
    rc |= expect_str_has("extfn output argument", buf, "Arg/Src: 123456");
    rc |= expect_str_has("extfn output target", buf, "Target: 11259375");
    rc |= expect_str_has("extfn output label", buf, "Radio Inhibit Ack");

    return rc;
}

int
main(void) {
    int rc = 0;

    // Build TSBK-mapped vPDU: DUID=0x07, opcode=0x40 (Group Voice)
    // svc=0x00 (clear), channel=0x100A (iden=1, ch=10), group=0x4567, source=0x00ABCDEF
    unsigned char mac[24] = {0};
    mac[0] = 0x07; // TSBK marker
    mac[1] = 0x40; // Group Voice Channel Grant (MAC-coded)
    mac[2] = 0x00; // svc bits
    mac[3] = 0x10; // channel hi
    mac[4] = 0x0A; // channel lo
    mac[5] = 0x45; // group hi
    mac[6] = 0x67; // group lo
    mac[7] = 0xAB; // source hi
    mac[8] = 0xCD;
    mac[9] = 0xEF; // source lo

    // Invoke decoder under a seeded state (trunking enabled, iden populated)
    p25_test_invoke_mac_vpdu_with_state((const unsigned char*)mac, 10, /*trunk*/ 1, /*cc*/ 851000000,
                                        /*iden*/ 1, /*type*/ 1, /*tdma*/ 0, /*base*/ 851000000 / 5, /*spac*/ 100);

    // Expect the state machine to retain the same channel/svc/group/src.
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    rc |= expect_eq("grant called", ctx->grant_count, 1);
    rc |= expect_eq("grant channel", ctx->vc_channel, 0x100A);
    rc |= expect_eq("grant svc", ctx->slots[0].svc_bits, 0x00);
    rc |= expect_eq("grant tg", ctx->vc_tg, 0x4567);
    rc |= expect_eq("grant src", ctx->vc_src, 0x00ABCDEF);

    // Case 2: Non-zero service options propagate to trunk SM
    unsigned char mac2[24] = {0};
    mac2[0] = 0x07; // TSBK marker
    mac2[1] = 0x40; // Group Voice Channel Grant
    mac2[2] = 0x87; // SVC: Emergency, priority=7 (no ENC gating)
    mac2[3] = 0x10;
    mac2[4] = 0x0A;
    mac2[5] = 0x12;
    mac2[6] = 0x34;
    mac2[7] = 0x00;
    mac2[8] = 0x00;
    mac2[9] = 0x01;
    p25_test_invoke_mac_vpdu_with_state((const unsigned char*)mac2, 10, /*trunk*/ 1, /*cc*/ 851000000,
                                        /*iden*/ 1, /*type*/ 1, /*tdma*/ 0, /*base*/ 851000000 / 5, /*spac*/ 100);
    ctx = p25_sm_get_ctx();
    rc |= expect_eq("grant2 called", ctx->grant_count, 1);
    rc |= expect_eq("grant2 svc", ctx->slots[0].svc_bits, 0x87);
    rc |= expect_eq("grant2 channel", ctx->vc_channel, 0x100A);

    rc |= test_extended_function_command_abbreviated();

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
