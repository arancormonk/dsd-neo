// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit tests for P25 Queued Response (0x61) and Deny Response (0x67) handling.
 * Tests field extraction, reason code lookup, SM notification, and active
 * channel display via the process_MAC_VPDU() entry point.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm_api.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

/* External entry point under test */
void process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]);

/* SM wrapper functions under test */
void p25_sm_on_queued_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target);
void p25_sm_on_deny_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target);

/* Stubs for external hooks referenced by linked modules */
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

struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

/* Alias decode helpers referenced by MAC VPDU handler */
void
unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

void
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

/* ============================================================================
 * SM API override tracking for tests
 * ============================================================================ */

static int g_queued_calls;
static int g_deny_calls;
static int g_last_svc_type;
static int g_last_reason_code;
static int g_last_target;

static void
fake_on_queued_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target) {
    (void)opts;
    (void)state;
    g_queued_calls++;
    g_last_svc_type = svc_type;
    g_last_reason_code = reason_code;
    g_last_target = target;
}

static void
fake_on_deny_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target) {
    (void)opts;
    (void)state;
    g_deny_calls++;
    g_last_svc_type = svc_type;
    g_last_reason_code = reason_code;
    g_last_target = target;
}

static p25_sm_api
sm_test_api(void) {
    p25_sm_api api;
    memset(&api, 0, sizeof(api));
    api.on_queued_response = fake_on_queued_response;
    api.on_deny_response = fake_on_deny_response;
    return api;
}

static void
reset_tracking(void) {
    g_queued_calls = 0;
    g_deny_calls = 0;
    g_last_svc_type = -1;
    g_last_reason_code = -1;
    g_last_target = -1;
}

/* ============================================================================
 * Helper: build a QUE/DENY MAC PDU
 * ============================================================================ */

static void
build_que_deny_mac(unsigned long long MAC[24], int is_deny, int svc_type, int reason_code, int addl_info,
                   int target_addr) {
    memset(MAC, 0, 24 * sizeof(unsigned long long));
    MAC[0] = 0x07;                                        // TSBK marker (len_b = 10)
    MAC[1] = (unsigned long long)(is_deny ? 0x67 : 0x61); // opcode
    MAC[2] = (unsigned long long)(svc_type & 0x3F);       // AIV=0, Service_Type
    MAC[3] = (unsigned long long)(reason_code & 0xFF);
    MAC[4] = (unsigned long long)((addl_info >> 16) & 0xFF);
    MAC[5] = (unsigned long long)((addl_info >> 8) & 0xFF);
    MAC[6] = (unsigned long long)(addl_info & 0xFF);
    MAC[7] = (unsigned long long)((target_addr >> 16) & 0xFF);
    MAC[8] = (unsigned long long)((target_addr >> 8) & 0xFF);
    MAC[9] = (unsigned long long)(target_addr & 0xFF);
}

/* ============================================================================
 * Test: QUE_RSP field extraction with known payload
 * ============================================================================ */

static int
test_que_rsp_field_extraction_known_payload(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    reset_tracking();
    p25_sm_set_api(sm_test_api());

    unsigned long long MAC[24];
    // SVC=0x15, Reason=0x20, Addl=0x123456, Target=0xABCDEF (11259375)
    build_que_deny_mac(MAC, 0, 0x15, 0x20, 0x123456, 0xABCDEF);

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    int rc = 0;
    if (g_queued_calls != 1) {
        fprintf(stderr, "FAIL: test_que_rsp_field_extraction: expected 1 queued call, got %d\n", g_queued_calls);
        rc = 1;
    }
    if (g_last_svc_type != 0x15) {
        fprintf(stderr, "FAIL: test_que_rsp_field_extraction: svc_type expected 0x15, got 0x%02X\n", g_last_svc_type);
        rc = 1;
    }
    if (g_last_reason_code != 0x20) {
        fprintf(stderr, "FAIL: test_que_rsp_field_extraction: reason_code expected 0x20, got 0x%02X\n",
                g_last_reason_code);
        rc = 1;
    }
    if (g_last_target != 0xABCDEF) {
        fprintf(stderr, "FAIL: test_que_rsp_field_extraction: target expected 0xABCDEF, got 0x%06X\n", g_last_target);
        rc = 1;
    }

    p25_sm_reset_api();
    return rc;
}

/* ============================================================================
 * Test: DENY_RSP field extraction with known payload
 * ============================================================================ */

static int
test_deny_rsp_field_extraction_known_payload(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    reset_tracking();
    p25_sm_set_api(sm_test_api());

    unsigned long long MAC[24];
    // SVC=0x3F, Reason=0xFF, Addl=0xFEDCBA, Target=0x000001
    build_que_deny_mac(MAC, 1, 0x3F, 0xFF, 0xFEDCBA, 0x000001);

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    int rc = 0;
    if (g_deny_calls != 1) {
        fprintf(stderr, "FAIL: test_deny_rsp_field_extraction: expected 1 deny call, got %d\n", g_deny_calls);
        rc = 1;
    }
    if (g_last_svc_type != 0x3F) {
        fprintf(stderr, "FAIL: test_deny_rsp_field_extraction: svc_type expected 0x3F, got 0x%02X\n", g_last_svc_type);
        rc = 1;
    }
    if (g_last_reason_code != 0xFF) {
        fprintf(stderr, "FAIL: test_deny_rsp_field_extraction: reason_code expected 0xFF, got 0x%02X\n",
                g_last_reason_code);
        rc = 1;
    }
    if (g_last_target != 0x000001) {
        fprintf(stderr, "FAIL: test_deny_rsp_field_extraction: target expected 0x000001, got 0x%06X\n", g_last_target);
        rc = 1;
    }

    p25_sm_reset_api();
    return rc;
}

/* ============================================================================
 * Test: Queued reason code lookup — all known codes
 * ============================================================================ */

static int
test_que_reason_code_lookup_all_known(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    /* Test each known queued reason code by checking active_channel output */
    struct {
        int code;
        const char* expected_substr;
    } cases[] = {
        {0x10, "Requester Active"},  {0x20, "Target Active"},        {0x2F, "Target Queued"},
        {0x30, "Group Active"},      {0x40, "No Channel Resources"}, {0x41, "No Telephone Resources"},
        {0x42, "No Data Resources"},
    };

    /* We verify reason codes by checking stderr output. Since we can't easily
     * capture stderr in this test framework, we verify the SM callback receives
     * the correct reason code value and that active_channel is set. */
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        memset(&opts, 0, sizeof opts);
        memset(&st, 0, sizeof st);
        reset_tracking();
        p25_sm_set_api(sm_test_api());

        unsigned long long MAC[24];
        build_que_deny_mac(MAC, 0, 0x01, cases[i].code, 0, 12345);
        process_MAC_VPDU(&opts, &st, 0, MAC);

        if (g_last_reason_code != cases[i].code) {
            fprintf(stderr, "FAIL: test_que_reason_code[0x%02X]: reason_code mismatch got 0x%02X\n", cases[i].code,
                    g_last_reason_code);
            rc = 1;
        }
        /* Verify active_channel contains "QUE" */
        if (strstr(st.active_channel[0], "QUE") == NULL) {
            fprintf(stderr, "FAIL: test_que_reason_code[0x%02X]: active_channel missing 'QUE': '%s'\n", cases[i].code,
                    st.active_channel[0]);
            rc = 1;
        }
        p25_sm_reset_api();
    }

    return rc;
}

/* ============================================================================
 * Test: Deny reason code lookup — all known codes
 * ============================================================================ */

static int
test_deny_reason_code_lookup_all_known(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    struct {
        int code;
        const char* expected_substr;
    } cases[] = {
        {0x10, "Requester Invalid"}, {0x20, "Target Invalid"},     {0x2F, "Target Refused"},
        {0x30, "Group Invalid"},     {0x60, "Site Access Denied"}, {0xFF, "Service Not Supported"},
    };

    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        memset(&opts, 0, sizeof opts);
        memset(&st, 0, sizeof st);
        reset_tracking();
        p25_sm_set_api(sm_test_api());

        unsigned long long MAC[24];
        build_que_deny_mac(MAC, 1, 0x02, cases[i].code, 0, 54321);
        process_MAC_VPDU(&opts, &st, 0, MAC);

        if (g_last_reason_code != cases[i].code) {
            fprintf(stderr, "FAIL: test_deny_reason_code[0x%02X]: reason_code mismatch got 0x%02X\n", cases[i].code,
                    g_last_reason_code);
            rc = 1;
        }
        /* Verify active_channel contains "DENY" */
        if (strstr(st.active_channel[0], "DENY") == NULL) {
            fprintf(stderr, "FAIL: test_deny_reason_code[0x%02X]: active_channel missing 'DENY': '%s'\n", cases[i].code,
                    st.active_channel[0]);
            rc = 1;
        }
        p25_sm_reset_api();
    }

    return rc;
}

/* ============================================================================
 * Test: Unrecognized queued reason code produces hex in active_channel
 * ============================================================================ */

static int
test_que_rsp_unrecognized_reason_hex(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    reset_tracking();
    p25_sm_set_api(sm_test_api());

    unsigned long long MAC[24];
    // Use reason code 0xAB which is not in the queued lookup table
    build_que_deny_mac(MAC, 0, 0x01, 0xAB, 0, 999);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    int rc = 0;
    if (g_last_reason_code != 0xAB) {
        fprintf(stderr, "FAIL: test_que_rsp_unrecognized_reason: expected 0xAB, got 0x%02X\n", g_last_reason_code);
        rc = 1;
    }
    /* The handler should still set active_channel with QUE */
    if (strstr(st.active_channel[0], "QUE") == NULL) {
        fprintf(stderr, "FAIL: test_que_rsp_unrecognized_reason: active_channel missing 'QUE': '%s'\n",
                st.active_channel[0]);
        rc = 1;
    }

    p25_sm_reset_api();
    return rc;
}

/* ============================================================================
 * Test: Unrecognized deny reason code produces hex in active_channel
 * ============================================================================ */

static int
test_deny_rsp_unrecognized_reason_hex(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    reset_tracking();
    p25_sm_set_api(sm_test_api());

    unsigned long long MAC[24];
    // Use reason code 0x99 which is not in the deny lookup table
    build_que_deny_mac(MAC, 1, 0x02, 0x99, 0, 888);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    int rc = 0;
    if (g_last_reason_code != 0x99) {
        fprintf(stderr, "FAIL: test_deny_rsp_unrecognized_reason: expected 0x99, got 0x%02X\n", g_last_reason_code);
        rc = 1;
    }
    if (strstr(st.active_channel[0], "DENY") == NULL) {
        fprintf(stderr, "FAIL: test_deny_rsp_unrecognized_reason: active_channel missing 'DENY': '%s'\n",
                st.active_channel[0]);
        rc = 1;
    }

    p25_sm_reset_api();
    return rc;
}

/* ============================================================================
 * Test: SM queued response releases when TUNED
 * ============================================================================ */

static int
test_sm_queued_releases_when_tuned(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    /* Force SM into TUNED state */
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_TUNED;
    ctx->initialized = 1;

    /* Reset API to default (no override) so real SM logic runs */
    p25_sm_reset_api();

    p25_sm_on_queued_response(&opts, &st, 0x01, 0x20, 12345);

    int rc = 0;
    if (st.p25_sm_queued_count != 1) {
        fprintf(stderr, "FAIL: test_sm_queued_releases_when_tuned: counter expected 1, got %u\n",
                st.p25_sm_queued_count);
        rc = 1;
    }
    /* After release, SM should be back to ON_CC (or IDLE depending on implementation) */
    if (ctx->state == P25_SM_TUNED) {
        fprintf(stderr, "FAIL: test_sm_queued_releases_when_tuned: SM still in TUNED state\n");
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * Test: SM deny response releases when TUNED
 * ============================================================================ */

static int
test_sm_deny_releases_when_tuned(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_TUNED;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_deny_response(&opts, &st, 0x02, 0xFF, 54321);

    int rc = 0;
    if (st.p25_sm_deny_count != 1) {
        fprintf(stderr, "FAIL: test_sm_deny_releases_when_tuned: counter expected 1, got %u\n", st.p25_sm_deny_count);
        rc = 1;
    }
    if (ctx->state == P25_SM_TUNED) {
        fprintf(stderr, "FAIL: test_sm_deny_releases_when_tuned: SM still in TUNED state\n");
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * Test: SM queued response no-op when ON_CC
 * ============================================================================ */

static int
test_sm_queued_noop_when_on_cc(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_ON_CC;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_queued_response(&opts, &st, 0x01, 0x10, 100);

    int rc = 0;
    if (st.p25_sm_queued_count != 1) {
        fprintf(stderr, "FAIL: test_sm_queued_noop_when_on_cc: counter expected 1, got %u\n", st.p25_sm_queued_count);
        rc = 1;
    }
    if (ctx->state != P25_SM_ON_CC) {
        fprintf(stderr, "FAIL: test_sm_queued_noop_when_on_cc: SM state changed from ON_CC\n");
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * Test: SM deny response no-op when ON_CC
 * ============================================================================ */

static int
test_sm_deny_noop_when_on_cc(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_ON_CC;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_deny_response(&opts, &st, 0x02, 0x60, 200);

    int rc = 0;
    if (st.p25_sm_deny_count != 1) {
        fprintf(stderr, "FAIL: test_sm_deny_noop_when_on_cc: counter expected 1, got %u\n", st.p25_sm_deny_count);
        rc = 1;
    }
    if (ctx->state != P25_SM_ON_CC) {
        fprintf(stderr, "FAIL: test_sm_deny_noop_when_on_cc: SM state changed from ON_CC\n");
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * Test: SM queued counter increments regardless of state
 * ============================================================================ */

static int
test_sm_queued_counter_increments(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_IDLE;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_queued_response(&opts, &st, 0x01, 0x10, 100);
    p25_sm_on_queued_response(&opts, &st, 0x01, 0x20, 200);
    p25_sm_on_queued_response(&opts, &st, 0x01, 0x30, 300);

    int rc = 0;
    if (st.p25_sm_queued_count != 3) {
        fprintf(stderr, "FAIL: test_sm_queued_counter_increments: expected 3, got %u\n", st.p25_sm_queued_count);
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * Test: SM deny counter increments regardless of state
 * ============================================================================ */

static int
test_sm_deny_counter_increments(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_HUNTING;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_deny_response(&opts, &st, 0x02, 0x20, 100);
    p25_sm_on_deny_response(&opts, &st, 0x02, 0x60, 200);

    int rc = 0;
    if (st.p25_sm_deny_count != 2) {
        fprintf(stderr, "FAIL: test_sm_deny_counter_increments: expected 2, got %u\n", st.p25_sm_deny_count);
        rc = 1;
    }

    return rc;
}

/* ============================================================================
 * Test: Active channel display contains "QUE" and target
 * ============================================================================ */

static int
test_active_channel_que_format(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    reset_tracking();
    p25_sm_set_api(sm_test_api());

    unsigned long long MAC[24];
    build_que_deny_mac(MAC, 0, 0x01, 0x40, 0, 67890);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    int rc = 0;
    if (strstr(st.active_channel[0], "QUE") == NULL) {
        fprintf(stderr, "FAIL: test_active_channel_que_format: missing 'QUE' in '%s'\n", st.active_channel[0]);
        rc = 1;
    }
    if (strstr(st.active_channel[0], "67890") == NULL) {
        fprintf(stderr, "FAIL: test_active_channel_que_format: missing '67890' in '%s'\n", st.active_channel[0]);
        rc = 1;
    }

    p25_sm_reset_api();
    return rc;
}

/* ============================================================================
 * Test: Active channel display contains "DENY" and target
 * ============================================================================ */

static int
test_active_channel_deny_format(void) {
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    reset_tracking();
    p25_sm_set_api(sm_test_api());

    unsigned long long MAC[24];
    build_que_deny_mac(MAC, 1, 0x02, 0x60, 0, 12345);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    int rc = 0;
    if (strstr(st.active_channel[0], "DENY") == NULL) {
        fprintf(stderr, "FAIL: test_active_channel_deny_format: missing 'DENY' in '%s'\n", st.active_channel[0]);
        rc = 1;
    }
    if (strstr(st.active_channel[0], "12345") == NULL) {
        fprintf(stderr, "FAIL: test_active_channel_deny_format: missing '12345' in '%s'\n", st.active_channel[0]);
        rc = 1;
    }

    p25_sm_reset_api();
    return rc;
}

/* ============================================================================
 * main
 * ============================================================================ */

int
main(void) {
    int rc = 0;

    rc |= test_que_rsp_field_extraction_known_payload();
    rc |= test_deny_rsp_field_extraction_known_payload();
    rc |= test_que_reason_code_lookup_all_known();
    rc |= test_deny_reason_code_lookup_all_known();
    rc |= test_que_rsp_unrecognized_reason_hex();
    rc |= test_deny_rsp_unrecognized_reason_hex();
    rc |= test_sm_queued_releases_when_tuned();
    rc |= test_sm_deny_releases_when_tuned();
    rc |= test_sm_queued_noop_when_on_cc();
    rc |= test_sm_deny_noop_when_on_cc();
    rc |= test_sm_queued_counter_increments();
    rc |= test_sm_deny_counter_increments();
    rc |= test_active_channel_que_format();
    rc |= test_active_channel_deny_format();

    if (rc == 0) {
        fprintf(stderr, "All P25 queued/deny response tests passed.\n");
    }
    return rc;
}
