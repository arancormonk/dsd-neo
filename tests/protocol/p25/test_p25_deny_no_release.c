// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Deny and Queued Responses never release the followed carrier.
 *
 * A Deny or Queued Response answers a unit's service request: its Target Address is the unit that asked
 * (TIA-102.AABC-D §6.2.5 / §6.2.14; TIA-102.BBAD-A carries the same fields), and no field ties it to the assignment the
 * decoder follows. A deny for someone else's request heard while a voice channel is being acquired, or while it
 * carries a call, must leave that carrier alone; the acquisition watchdog ends an assignment that never carries voice.
 * A response whose MAC CRC failed (kept under -F) must not count either.
 *
 * Responses go through the MAC message dispatch (process_MAC_VPDU_bridged_p1() for a TSBK in MAC form,
 * process_MAC_VPDU() / process_MAC_VPDU_crc() for native Phase 2 PDUs) with the real trunking state machine linked.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

enum {
    DENY = 1,
    QUEUED = 0,
    TG = 100,         /* the talkgroup the followed carrier was granted for */
    TG_SOURCE = 4321, /* the unit that asked for it */
};

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} fixture;

static dsd_test_capture_stderr g_cap;

/* Handler text is not under test: each response is decoded with stderr captured and dropped. */
static void
quiet_begin(void) {
    (void)dsd_test_capture_stderr_begin(&g_cap, "p25_deny_no_release");
}

static void
quiet_end(void) {
    dsd_test_capture_stderr_end(&g_cap);
    if (g_cap.path[0] != '\0') {
        (void)remove(g_cap.path);
    }
}

static dsd_trunk_tune_result
test_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

/* A carrier just tuned for TG on a grant: the state machine follows it (TUNED), no voice has arrived yet. */
static int
fixture_open(fixture* f) {
    f->opts = (dsd_opts*)calloc(1, sizeof(*f->opts));
    f->state = (dsd_state*)calloc(1, sizeof(*f->state));
    if (f->opts == NULL || f->state == NULL) {
        free(f->opts);
        free(f->state);
        return -1;
    }
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc_request = test_return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);

    f->opts->trunk_enable = 1;
    f->opts->trunk_is_tuned = 1;
    f->opts->trunk_hangtime = 2.0f;
    f->state->p25_cc_freq = 851000000;
    f->state->trunk_cc_freq = 851000000;
    f->state->p25_vc_freq[0] = 852000000;
    f->state->p25_last_vc_tune_time_m = dsd_decode_now_mono_s();

    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->state = P25_SM_TUNED;
    ctx->initialized = 1;
    return 0;
}

static void
fixture_close(fixture* f) {
    dsd_state_ext_free_all(f->state);
    free(f->state);
    free(f->opts);
}

static int
expect_tuned(const char* tag) {
    if (p25_sm_get_ctx()->state != P25_SM_TUNED) {
        DSD_FPRINTF(stderr, "%s: the followed carrier was released\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_count(const char* tag, unsigned int got, unsigned int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: count %u want %u\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* Deny (TSBK 0x27, TIA-102.AABC-D §6.2.5) or Queued (0x21, §6.2.14) Response in MAC form: AIV + Service Type octet 2,
   Reason octet 3, Additional Information 4-6, Target Address 7-9. Native Phase 2 0x67 / 0x61 put the same octets after
   the MAC header. */
static void
build_response(unsigned long long int mac[24], int is_deny, int reason, uint32_t target, int bridged) {
    DSD_MEMSET(mac, 0, 24 * sizeof(mac[0]));
    mac[0] = bridged ? 0x07 : 0x00;
    mac[1] = (unsigned long long int)(is_deny ? 0x67 : 0x61);
    mac[2] = 0x80U; /* AIV set, service type 0: group voice */
    mac[3] = (unsigned long long int)reason;
    mac[5] = (TG >> 8) & 0xFFU; /* additional information: the group the request named */
    mac[6] = TG & 0xFFU;
    mac[7] = (target >> 16) & 0xFFU;
    mac[8] = (target >> 8) & 0xFFU;
    mac[9] = target & 0xFFU;
}

/* Motorola Phase 2 0xA7 / 0xA6 (MFID 0x90, length octet 3): the standard fields two octets later. */
static void
build_motorola_response(unsigned long long int mac[24], int is_deny, uint32_t target) {
    DSD_MEMSET(mac, 0, 24 * sizeof(mac[0]));
    mac[1] = (unsigned long long int)(is_deny ? 0xA7 : 0xA6);
    mac[2] = 0x90;
    mac[3] = 0x0B;
    mac[4] = 0x00;
    mac[5] = 0x60;
    mac[9] = (target >> 16) & 0xFFU;
    mac[10] = (target >> 8) & 0xFFU;
    mac[11] = target & 0xFFU;
}

static void
send_tsbk(fixture* f, int is_deny, uint32_t target) {
    unsigned long long int mac[24];
    build_response(mac, is_deny, 0x30, target, 1);
    f->state->synctype = DSD_SYNC_P25P1_POS;
    quiet_begin();
    process_MAC_VPDU_bridged_p1(f->opts, f->state, mac);
    quiet_end();
}

static void
send_p2(fixture* f, int is_deny, uint32_t target, int crc_ok) {
    unsigned long long int mac[24];
    build_response(mac, is_deny, 0x30, target, 0);
    f->state->synctype = DSD_SYNC_P25P2_POS;
    quiet_begin();
    process_MAC_VPDU_crc(f->opts, f->state, 0 /* FACCH */, P25_MAC_PDU_ACTIVE, mac, crc_ok);
    quiet_end();
}

static void
send_motorola_p2(fixture* f, int is_deny, uint32_t target) {
    unsigned long long int mac[24];
    build_motorola_response(mac, is_deny, target);
    f->state->synctype = DSD_SYNC_P25P2_POS;
    quiet_begin();
    process_MAC_VPDU(f->opts, f->state, 0 /* FACCH */, P25_MAC_PDU_ACTIVE, mac);
    quiet_end();
}

/* Every response form, deny and queued, for @p target. */
static void
send_all_forms(fixture* f, uint32_t target) {
    for (int is_deny = 0; is_deny <= 1; is_deny++) {
        send_tsbk(f, is_deny, target);
        send_p2(f, is_deny, target, 1);
        send_motorola_p2(f, is_deny, target);
    }
}

/* A carrier carrying a call keeps it. */
static int
test_deny_never_releases_occupied(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    f.state->p25_p2_audio_allowed[0] = 1;
    send_all_forms(&f, 5555);
    rc |= expect_tuned("occupied");
    rc |= expect_count("occupied denies", f.state->p25_sm_deny_count, 3);
    rc |= expect_count("occupied queued", f.state->p25_sm_queued_count, 3);
    fixture_close(&f);
    return rc;
}

/* A carrier being acquired (granted, no voice yet) is the watchdog's to end, not a response's. */
static int
test_deny_never_releases_acquiring(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    send_tsbk(&f, DENY, TG_SOURCE);
    rc |= expect_tuned("acquiring tsbk deny");
    send_p2(&f, QUEUED, TG_SOURCE, 1);
    rc |= expect_tuned("acquiring p2 queued");
    send_motorola_p2(&f, DENY, TG_SOURCE);
    rc |= expect_tuned("acquiring motorola deny");
    fixture_close(&f);
    return rc;
}

/* A response for another unit's request says nothing about the followed call. */
static int
test_deny_never_releases_unrelated(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    send_all_forms(&f, 7777777);
    rc |= expect_tuned("unrelated");
    fixture_close(&f);
    return rc;
}

/* A deny's Target is a radio: one whose number equals the followed talkgroup is still someone else's request. */
static int
test_deny_never_releases_rid_equals_tg(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    send_all_forms(&f, TG);
    rc |= expect_tuned("rid equals tg");
    fixture_close(&f);
    return rc;
}

/* A response whose MAC CRC failed (decoded under -F) is neither acted on nor counted. */
static int
test_deny_never_releases_relaxed_crc(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    f.opts->aggressive_framesync = 0;
    send_p2(&f, DENY, 5555, 0);
    send_p2(&f, QUEUED, 5555, 0);
    rc |= expect_tuned("relaxed crc");
    rc |= expect_count("relaxed crc denies", f.state->p25_sm_deny_count, 0);
    rc |= expect_count("relaxed crc queued", f.state->p25_sm_queued_count, 0);
    send_p2(&f, DENY, 5555, 1);
    rc |= expect_count("checked deny counted", f.state->p25_sm_deny_count, 1);
    rc |= expect_tuned("checked deny");
    fixture_close(&f);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_deny_never_releases_occupied();
    rc |= test_deny_never_releases_acquiring();
    rc |= test_deny_never_releases_unrelated();
    rc |= test_deny_never_releases_rid_equals_tg();
    rc |= test_deny_never_releases_relaxed_crc();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "P25_DENY_NO_RELEASE: all tests passed\n");
    }
    return rc;
}
