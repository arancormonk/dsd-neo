// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

int
getAfsString(const dsd_state* state, char* buffer, int a, int f, int s) {
    (void)state;
    return DSD_SNPRINTF(buffer, 16, "%02d-%03d", a, (f * 8) + s);
}

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)state;
    if (BufferIn != NULL && BufferOut != NULL) {
        DSD_MEMCPY(BufferOut, BufferIn, 49);
    }
}

static void
write_bits_u64(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        const size_t shift = (nbits - 1U) - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
bytes_to_bits(uint8_t* bits, const uint8_t* bytes, size_t nbytes) {
    for (size_t i = 0; i < nbytes; i++) {
        write_bits_u64(bits, i * 8U, bytes[i], 8U);
    }
}

static uint8_t
hytera_checksum(const uint8_t bytes[8]) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < 8; i++) {
        checksum = (uint8_t)((checksum + bytes[i]) & 0xFFU);
    }
    checksum = (uint8_t)(~checksum & 0xFFU);
    checksum++;
    return checksum;
}

static int s_scan_activity_calls = 0;
static uint32_t s_scan_activity_target = 0;
static uint32_t s_scan_activity_source = 0;
static int s_scan_activity_is_private = 0;
static int s_scan_activity_encrypted = 0;
static int s_scan_activity_data_call = 0;

static void
capture_scan_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                                       int is_private, int encrypted, int data_call) {
    (void)opts;
    (void)state;
    s_scan_activity_calls++;
    s_scan_activity_target = target;
    s_scan_activity_source = source;
    s_scan_activity_is_private = is_private;
    s_scan_activity_encrypted = encrypted;
    s_scan_activity_data_call = data_call;
}

static void
clear_scan_hooks(void) {
    dsd_trunk_scan_hooks hooks = {0};
    dsd_trunk_scan_hooks_set(hooks);
}

static void
build_regular_flco(uint8_t* bits, uint8_t flco, uint8_t fid, uint8_t so, uint32_t target, uint32_t source) {
    DSD_MEMSET(bits, 0, 80);
    write_bits_u64(bits, 2U, flco, 6U);
    write_bits_u64(bits, 8U, fid, 8U);
    write_bits_u64(bits, 16U, so, 8U);
    write_bits_u64(bits, 24U, target & 0x00FFFFFFU, 24U);
    write_bits_u64(bits, 48U, source & 0x00FFFFFFU, 24U);
}

static int
capture_regular_flco(uint8_t type, char* out, size_t out_size) {
    if (out == NULL || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "dmr_flco_output") != 0) {
        return -1;
    }

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, type);

    int rc = dsd_test_capture_stderr_end(&cap);
    if (rc != 0) {
        (void)remove(cap.path);
        return -1;
    }

    FILE* fp = fopen(cap.path, "rb");
    if (fp == NULL) {
        (void)remove(cap.path);
        return -1;
    }
    size_t nread = fread(out, 1, out_size - 1U, fp);
    if (ferror(fp)) {
        fclose(fp);
        (void)remove(cap.path);
        return -1;
    }
    out[nread] = '\0';
    fclose(fp);
    (void)remove(cap.path);
    return 0;
}

static void
test_flco_output_uses_real_newlines(void) {
    char out[2048];
    assert(capture_regular_flco(1U, out, sizeof(out)) == 0);
    assert(strchr(out, '\n') != NULL);
    assert(strstr(out, "\\n") == NULL);
}

static void
test_kirisun_flco_sets_late_entry_mode(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x0AU, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(opts.dmr_le == 3);
    assert(irr == 0);

    opts.dmr_le = 3;
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x0AU, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(opts.dmr_le == 0);
    assert(irr == 0);
}

static void
test_hytera_enhanced_flco_uses_secondary_checksum(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    uint8_t bytes[9] = {0x02U, 0x68U, 0x34U, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0x00U};
    bytes[8] = hytera_checksum(bytes);

    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof(bits));
    bytes_to_bits(bits, bytes, sizeof(bytes));

    state.currentslot = 0;
    uint32_t irr = 0;
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    assert(irr == 0);
    assert(opts.dmr_le == 2);
    assert((state.dmr_so & 0x40U) != 0U);
    assert(state.payload_algid == 0x02);
    assert(state.payload_keyid == 0x34);
    assert(state.payload_mi == 0x0123456789ULL);
}

static void
test_flco_scan_hook_reports_encrypted_service_option(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_tune_enc_calls = 0;
    state.currentslot = 0;

    s_scan_activity_calls = 0;
    s_scan_activity_target = 0;
    s_scan_activity_source = 0;
    s_scan_activity_is_private = 0;
    s_scan_activity_encrypted = 0;
    s_scan_activity_data_call = 0;

    dsd_trunk_scan_hooks hooks = {0};
    hooks.dmr_conventional_activity = capture_scan_dmr_conventional_activity;
    dsd_trunk_scan_hooks_set(hooks);

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    clear_scan_hooks();

    assert(irr == 0);
    assert(s_scan_activity_calls == 1);
    assert(s_scan_activity_target == 1001U);
    assert(s_scan_activity_source == 2002U);
    assert(s_scan_activity_is_private == 0);
    assert(s_scan_activity_encrypted == 1);
    assert(s_scan_activity_data_call == 0);
}

static void
test_hytera_flco_scan_hook_uses_final_call_type(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.gi[0] = 0;

    s_scan_activity_calls = 0;
    s_scan_activity_is_private = 0;

    dsd_trunk_scan_hooks hooks = {0};
    hooks.dmr_conventional_activity = capture_scan_dmr_conventional_activity;
    dsd_trunk_scan_hooks_set(hooks);

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x03U, 0x68U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    clear_scan_hooks();

    assert(irr == 0);
    assert(state.gi[0] == 1);
    assert(s_scan_activity_calls == 1);
    assert(s_scan_activity_is_private == 1);
}

int
main(void) {
    test_flco_output_uses_real_newlines();
    test_kirisun_flco_sets_late_entry_mode();
    test_hytera_enhanced_flco_uses_secondary_checksum();
    test_flco_scan_hook_reports_encrypted_service_option();
    test_hytera_flco_scan_hook_uses_final_call_type();
    printf("DMR FLCO privacy modes: OK\n");
    return 0;
}
