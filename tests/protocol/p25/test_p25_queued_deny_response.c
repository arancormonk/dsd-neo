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
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

/* External entry point under test */
void process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, unsigned long long int MAC[24]);

/* SM wrapper functions under test */
void p25_sm_on_queued_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target);
void p25_sm_on_deny_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target);

/* Stubs for external hooks referenced by linked modules */
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

/* Alias decode helpers referenced by MAC VPDU handler */
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
    DSD_MEMSET(&api, 0, sizeof(api));
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
build_que_deny_mac_aii(unsigned long long MAC[24], int is_deny, int svc_type, int reason_code, int addl_info,
                       int target_addr, int has_addl_info) {
    DSD_MEMSET(MAC, 0, 24 * sizeof(unsigned long long));
    MAC[0] = 0x07;                                                                 // TSBK marker
    MAC[1] = (unsigned long long)(is_deny ? 0x67 : 0x61);                          // opcode
    MAC[2] = (unsigned long long)((svc_type & 0x3F) | (has_addl_info ? 0x80 : 0)); // AII, Service_Type
    MAC[3] = (unsigned long long)(reason_code & 0xFF);
    MAC[4] = (unsigned long long)((addl_info >> 16) & 0xFF);
    MAC[5] = (unsigned long long)((addl_info >> 8) & 0xFF);
    MAC[6] = (unsigned long long)(addl_info & 0xFF);
    MAC[7] = (unsigned long long)((target_addr >> 16) & 0xFF);
    MAC[8] = (unsigned long long)((target_addr >> 8) & 0xFF);
    MAC[9] = (unsigned long long)(target_addr & 0xFF);
}

static void
build_que_deny_mac(unsigned long long MAC[24], int is_deny, int svc_type, int reason_code, int addl_info,
                   int target_addr) {
    build_que_deny_mac_aii(MAC, is_deny, svc_type, reason_code, addl_info, target_addr, addl_info != 0);
}

/* ============================================================================
 * Test: QUE_RSP field extraction with known payload
 * ============================================================================ */

static int
test_que_rsp_field_extraction_known_payload(void) {
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    reset_tracking();
    {
        p25_sm_api api = sm_test_api();
        p25_sm_set_api(&api);
    }

    unsigned long long MAC[24];
    // SVC=0x15, Reason=0x20, Addl=0x123456, Target=0xABCDEF (11259375)
    build_que_deny_mac(MAC, 0, 0x15, 0x20, 0x123456, 0xABCDEF);

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    int rc = 0;
    if (g_queued_calls != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_que_rsp_field_extraction: expected 1 queued call, got %d\n", g_queued_calls);
        rc = 1;
    }
    if (g_last_svc_type != 0x15) {
        DSD_FPRINTF(stderr, "FAIL: test_que_rsp_field_extraction: svc_type expected 0x15, got 0x%02X\n",
                    g_last_svc_type);
        rc = 1;
    }
    if (g_last_reason_code != 0x20) {
        DSD_FPRINTF(stderr, "FAIL: test_que_rsp_field_extraction: reason_code expected 0x20, got 0x%02X\n",
                    g_last_reason_code);
        rc = 1;
    }
    if (g_last_target != 0xABCDEF) {
        DSD_FPRINTF(stderr, "FAIL: test_que_rsp_field_extraction: target expected 0xABCDEF, got 0x%06X\n",
                    g_last_target);
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    reset_tracking();
    {
        p25_sm_api api = sm_test_api();
        p25_sm_set_api(&api);
    }

    unsigned long long MAC[24];
    // SVC=0x3F, Reason=0xFF, Addl=0xFEDCBA, Target=0x000001
    build_que_deny_mac(MAC, 1, 0x3F, 0xFF, 0xFEDCBA, 0x000001);

    process_MAC_VPDU(&opts, &st, 0 /*FACCH*/, MAC);

    int rc = 0;
    if (g_deny_calls != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_deny_rsp_field_extraction: expected 1 deny call, got %d\n", g_deny_calls);
        rc = 1;
    }
    if (g_last_svc_type != 0x3F) {
        DSD_FPRINTF(stderr, "FAIL: test_deny_rsp_field_extraction: svc_type expected 0x3F, got 0x%02X\n",
                    g_last_svc_type);
        rc = 1;
    }
    if (g_last_reason_code != 0xFF) {
        DSD_FPRINTF(stderr, "FAIL: test_deny_rsp_field_extraction: reason_code expected 0xFF, got 0x%02X\n",
                    g_last_reason_code);
        rc = 1;
    }
    if (g_last_target != 0x000001) {
        DSD_FPRINTF(stderr, "FAIL: test_deny_rsp_field_extraction: target expected 0x000001, got 0x%06X\n",
                    g_last_target);
        rc = 1;
    }

    p25_sm_reset_api();
    return rc;
}

/* ============================================================================
 * Test: Queued reason code lookup -- known codes and ranges
 * ============================================================================ */

static int
test_que_reason_code_lookup_all_known(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    struct {
        int code;
        const char* expected_substr;
    } cases[] = {
        {0x10, "Requesting Unit Busy Other Service"},
        {0x20, "Target Unit Busy Other Service"},
        {0x2F, "Target Unit Queued This Call"},
        {0x30, "Target Group Currently Active"},
        {0x40, "Channel Resources Unavailable"},
        {0x41, "Telephone Resources Unavailable"},
        {0x42, "Data Resources Unavailable"},
        {0x50, "Superseding Service Currently Active"},
        {0x00, "Reserved"},
        {0x7F, "Reserved"},
        {0x80, "User/System Defined"},
        {0xAB, "User/System Defined"},
    };

    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        reset_tracking();
        {
            p25_sm_api api = sm_test_api();
            p25_sm_set_api(&api);
        }

        unsigned long long MAC[24];
        build_que_deny_mac(MAC, 0, 0x01, cases[i].code, 0, 12345);
        process_MAC_VPDU(&opts, &st, 0, MAC);

        if (g_last_reason_code != cases[i].code) {
            DSD_FPRINTF(stderr, "FAIL: test_que_reason_code[0x%02X]: reason_code mismatch got 0x%02X\n", cases[i].code,
                        g_last_reason_code);
            rc = 1;
        }
        if (strstr(st.active_channel[0], "QUE") == NULL) {
            DSD_FPRINTF(stderr, "FAIL: test_que_reason_code[0x%02X]: active_channel missing 'QUE': '%s'\n",
                        cases[i].code, st.active_channel[0]);
            rc = 1;
        }
        if (strstr(st.active_channel[0], cases[i].expected_substr) == NULL) {
            DSD_FPRINTF(stderr, "FAIL: test_que_reason_code[0x%02X]: active_channel missing '%s': '%s'\n",
                        cases[i].code, cases[i].expected_substr, st.active_channel[0]);
            rc = 1;
        }
        p25_sm_reset_api();
    }

    return rc;
}

/* ============================================================================
 * Test: Deny reason code lookup -- known codes and ranges
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
        {0x10, "Requesting Unit Not Valid"},
        {0x11, "Requesting Unit Not Authorized"},
        {0x20, "Target Unit Not Valid"},
        {0x21, "Target Unit Not Authorized"},
        {0x2F, "Target Unit Refused Call"},
        {0x30, "Target Group Not Valid"},
        {0x31, "Target Group Not Authorized"},
        {0x40, "Invalid Dialing"},
        {0x41, "Telephone Number Not Authorized"},
        {0x42, "PSTN Not Valid"},
        {0x50, "Call Timeout"},
        {0x51, "Landline Terminated Call"},
        {0x52, "Subscriber Unit Terminated Call"},
        {0x5F, "Call Preempted"},
        {0x60, "Site Access Denial"},
        {0x67, "PTT Collide"},
        {0x77, "PTT Bonk"},
        {0xF0, "Call Options Not Valid For Service"},
        {0xF1, "Protection Service Option Not Valid"},
        {0xF2, "Duplex Service Option Not Valid"},
        {0xF3, "Circuit/Packet Mode Option Not Valid"},
        {0xFF, "System Does Not Support Service"},
        {0x00, "Reserved"},
        {0x5E, "Reserved"},
        {0x61, "User/System Defined"},
        {0x99, "User/System Defined"},
        {0xFE, "User/System Defined"},
    };

    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&st, 0, sizeof st);
        reset_tracking();
        {
            p25_sm_api api = sm_test_api();
            p25_sm_set_api(&api);
        }

        unsigned long long MAC[24];
        build_que_deny_mac(MAC, 1, 0x02, cases[i].code, 0, 54321);
        process_MAC_VPDU(&opts, &st, 0, MAC);

        if (g_last_reason_code != cases[i].code) {
            DSD_FPRINTF(stderr, "FAIL: test_deny_reason_code[0x%02X]: reason_code mismatch got 0x%02X\n", cases[i].code,
                        g_last_reason_code);
            rc = 1;
        }
        if (strstr(st.active_channel[0], "DENY") == NULL) {
            DSD_FPRINTF(stderr, "FAIL: test_deny_reason_code[0x%02X]: active_channel missing 'DENY': '%s'\n",
                        cases[i].code, st.active_channel[0]);
            rc = 1;
        }
        if (strstr(st.active_channel[0], cases[i].expected_substr) == NULL) {
            DSD_FPRINTF(stderr, "FAIL: test_deny_reason_code[0x%02X]: active_channel missing '%s': '%s'\n",
                        cases[i].code, cases[i].expected_substr, st.active_channel[0]);
            rc = 1;
        }
        p25_sm_reset_api();
    }

    return rc;
}

/* ============================================================================
 * Test: Queued user/system reason range is labeled
 * ============================================================================ */

static int
test_que_rsp_user_reason_range(void) {
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    reset_tracking();
    {
        p25_sm_api api = sm_test_api();
        p25_sm_set_api(&api);
    }

    unsigned long long MAC[24];
    build_que_deny_mac(MAC, 0, 0x01, 0xAB, 0, 999);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    int rc = 0;
    if (g_last_reason_code != 0xAB) {
        DSD_FPRINTF(stderr, "FAIL: test_que_rsp_user_reason_range: expected 0xAB, got 0x%02X\n", g_last_reason_code);
        rc = 1;
    }
    if (strstr(st.active_channel[0], "QUE") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_que_rsp_user_reason_range: active_channel missing 'QUE': '%s'\n",
                    st.active_channel[0]);
        rc = 1;
    }
    if (strstr(st.active_channel[0], "User/System Defined") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_que_rsp_user_reason_range: active_channel missing user/system label: '%s'\n",
                    st.active_channel[0]);
        rc = 1;
    }

    p25_sm_reset_api();
    return rc;
}

/* ============================================================================
 * Test: Deny user/system reason range is labeled
 * ============================================================================ */

static int
test_deny_rsp_user_reason_range(void) {
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    reset_tracking();
    {
        p25_sm_api api = sm_test_api();
        p25_sm_set_api(&api);
    }

    unsigned long long MAC[24];
    build_que_deny_mac(MAC, 1, 0x02, 0x99, 0, 888);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    int rc = 0;
    if (g_last_reason_code != 0x99) {
        DSD_FPRINTF(stderr, "FAIL: test_deny_rsp_user_reason_range: expected 0x99, got 0x%02X\n", g_last_reason_code);
        rc = 1;
    }
    if (strstr(st.active_channel[0], "DENY") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_deny_rsp_user_reason_range: active_channel missing 'DENY': '%s'\n",
                    st.active_channel[0]);
        rc = 1;
    }
    if (strstr(st.active_channel[0], "User/System Defined") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_deny_rsp_user_reason_range: active_channel missing user/system label: '%s'\n",
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    /* Force SM into TUNED state */
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_TUNED;
    ctx->initialized = 1;

    /* Reset API to default (no override) so real SM logic runs */
    p25_sm_reset_api();

    p25_sm_on_queued_response(&opts, &st, 0x01, 0x20, 12345);

    int rc = 0;
    if (st.p25_sm_queued_count != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_queued_releases_when_tuned: counter expected 1, got %u\n",
                    st.p25_sm_queued_count);
        rc = 1;
    }
    /* After release, SM should be back to ON_CC (or IDLE depending on implementation) */
    if (ctx->state == P25_SM_TUNED) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_queued_releases_when_tuned: SM still in TUNED state\n");
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_TUNED;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_deny_response(&opts, &st, 0x02, 0xFF, 54321);

    int rc = 0;
    if (st.p25_sm_deny_count != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_deny_releases_when_tuned: counter expected 1, got %u\n",
                    st.p25_sm_deny_count);
        rc = 1;
    }
    if (ctx->state == P25_SM_TUNED) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_deny_releases_when_tuned: SM still in TUNED state\n");
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_ON_CC;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_queued_response(&opts, &st, 0x01, 0x10, 100);

    int rc = 0;
    if (st.p25_sm_queued_count != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_queued_noop_when_on_cc: counter expected 1, got %u\n",
                    st.p25_sm_queued_count);
        rc = 1;
    }
    if (ctx->state != P25_SM_ON_CC) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_queued_noop_when_on_cc: SM state changed from ON_CC\n");
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_ON_CC;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_deny_response(&opts, &st, 0x02, 0x60, 200);

    int rc = 0;
    if (st.p25_sm_deny_count != 1) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_deny_noop_when_on_cc: counter expected 1, got %u\n", st.p25_sm_deny_count);
        rc = 1;
    }
    if (ctx->state != P25_SM_ON_CC) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_deny_noop_when_on_cc: SM state changed from ON_CC\n");
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_IDLE;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_queued_response(&opts, &st, 0x01, 0x10, 100);
    p25_sm_on_queued_response(&opts, &st, 0x01, 0x20, 200);
    p25_sm_on_queued_response(&opts, &st, 0x01, 0x30, 300);

    int rc = 0;
    if (st.p25_sm_queued_count != 3) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_queued_counter_increments: expected 3, got %u\n", st.p25_sm_queued_count);
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_HUNTING;
    ctx->initialized = 1;

    p25_sm_reset_api();

    p25_sm_on_deny_response(&opts, &st, 0x02, 0x20, 100);
    p25_sm_on_deny_response(&opts, &st, 0x02, 0x60, 200);

    int rc = 0;
    if (st.p25_sm_deny_count != 2) {
        DSD_FPRINTF(stderr, "FAIL: test_sm_deny_counter_increments: expected 2, got %u\n", st.p25_sm_deny_count);
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    reset_tracking();
    {
        p25_sm_api api = sm_test_api();
        p25_sm_set_api(&api);
    }

    unsigned long long MAC[24];
    build_que_deny_mac(MAC, 0, 0x01, 0x40, 0, 67890);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    int rc = 0;
    if (strstr(st.active_channel[0], "QUE") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_active_channel_que_format: missing 'QUE' in '%s'\n", st.active_channel[0]);
        rc = 1;
    }
    if (strstr(st.active_channel[0], "67890") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_active_channel_que_format: missing '67890' in '%s'\n", st.active_channel[0]);
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
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    reset_tracking();
    {
        p25_sm_api api = sm_test_api();
        p25_sm_set_api(&api);
    }

    unsigned long long MAC[24];
    build_que_deny_mac(MAC, 1, 0x02, 0x60, 0, 12345);
    process_MAC_VPDU(&opts, &st, 0, MAC);

    int rc = 0;
    if (strstr(st.active_channel[0], "DENY") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_active_channel_deny_format: missing 'DENY' in '%s'\n", st.active_channel[0]);
        rc = 1;
    }
    if (strstr(st.active_channel[0], "12345") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_active_channel_deny_format: missing '12345' in '%s'\n", st.active_channel[0]);
        rc = 1;
    }

    p25_sm_reset_api();
    return rc;
}

/* ============================================================================
 * Test: Additional information is displayed only when AII is set
 * ============================================================================ */

static int
test_additional_info_indicator_controls_display(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    reset_tracking();
    {
        p25_sm_api api = sm_test_api();
        p25_sm_set_api(&api);
    }

    unsigned long long MAC[24];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    build_que_deny_mac_aii(MAC, 0, 0x01, 0x40, 0x123456, 777, 0);
    process_MAC_VPDU(&opts, &st, 0, MAC);
    if (strstr(st.active_channel[0], "Info:") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_additional_info_indicator: displayed info without AII: '%s'\n",
                    st.active_channel[0]);
        rc = 1;
    }

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    build_que_deny_mac_aii(MAC, 0, 0x01, 0x40, 0x123456, 777, 1);
    process_MAC_VPDU(&opts, &st, 0, MAC);
    if (strstr(st.active_channel[0], "Info: 123456") == NULL) {
        DSD_FPRINTF(stderr, "FAIL: test_additional_info_indicator: missing AII info: '%s'\n", st.active_channel[0]);
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
    rc |= test_que_rsp_user_reason_range();
    rc |= test_deny_rsp_user_reason_range();
    rc |= test_sm_queued_releases_when_tuned();
    rc |= test_sm_deny_releases_when_tuned();
    rc |= test_sm_queued_noop_when_on_cc();
    rc |= test_sm_deny_noop_when_on_cc();
    rc |= test_sm_queued_counter_increments();
    rc |= test_sm_deny_counter_increments();
    rc |= test_active_channel_que_format();
    rc |= test_active_channel_deny_format();
    rc |= test_additional_info_indicator_controls_display();

    if (rc == 0) {
        DSD_FPRINTF(stderr, "All P25 queued/deny response tests passed.\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
