// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief End to end: the system identity a trunked carrier sent does not follow the receiver to the next carrier.
 *
 * The real engine replays committed I/Q fixtures the way the CLI does. Carrier A is p25p1_c4fm_cc, the control channel
 * of a trunked P25 system (WACN BEE00, SYS 14C, NAC 140, RFSS 5 site 9, 851.375 MHz). Carrier B is p25p1_c4fm_vc, a
 * conventional P25 carrier of another system (NAC 293, a group call from radio 1 to group 1) that sends no network
 * status; its sidecar is copied with another centre (852.1 MHz) so the two carriers differ in frequency too. The
 * second run on the same decoder state starts another replay, the carrier boundary a stream on another source is
 * (dsd_engine_note_stream_source()).
 *
 * Carrier A must leave a record naming its system and its control channel. On carrier B the record, the shared view
 * every frontend formats (the terminal's spectrum legend prints its full form after "System: ", the Qt site line and
 * the Spectrum tokens are the same text) and the history row of B's call carry B's NAC alone: none of A's WACN, SYS,
 * site or control channel frequency. Both carriers are P25, so the protocol change that drops a record of another
 * family cannot hide a boundary that failed to reset it.
 */

#include <dsd-neo/app_control/site_identity_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#ifndef DSD_NEO_TEST_IQ_FIXTURE_DIR
#error "DSD_NEO_TEST_IQ_FIXTURE_DIR must name tests/fixtures/iq"
#endif

enum {
    A_WACN = 0xBEE00,
    A_SYSID = 0x14C,
    A_NAC = 0x140,
    B_NAC = 0x293,
};

static const uint32_t kCarrierAHz = 851375000U;
static const uint32_t kCarrierBHz = 852100000U;

static int g_failures = 0;

static void
check(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

static void
check_u(const char* label, unsigned long got, unsigned long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %lu (0x%lX), want %lu (0x%lX)\n", label, got, got, want, want);
        g_failures++;
    }
}

static void
check_text(const char* label, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", label, got, want);
        g_failures++;
    }
}

/* What a run left, read in the stop hook: after live processing, before the engine tears its own state down. */
typedef struct {
    int stops;
    dsd_received_identity record;
    char full[512];
    uint32_t view_cc_freq_hz;
} run_view;

/* The stop hook runs only after a start hook that succeeded. */
static int
observe_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    return 0;
}

static void
observe_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    run_view* view = (run_view*)context;
    view->stops++;
    view->record = state->rx_id;
    dsd_app_site_identity identity;
    dsd_app_site_identity_view(state, &identity);
    (void)dsd_app_site_identity_format(&identity, DSD_APP_SITE_IDENTITY_FULL, 1, view->full, sizeof view->full);
    view->view_cc_freq_hz = identity.cc_freq_hz;
}

/* Configure @p opts and @p state to replay @p meta under -f1, the way the CLI does, and run the engine. */
static int
run_replay(const char* meta, dsd_opts* opts, dsd_state* state, run_view* view) {
    char arg0[] = "dsd-neo";
    char arg_frontend[] = "--frontend";
    char arg_none[] = "none";
    char arg_mode[] = "-f1";
    char arg_replay[] = "--iq-replay";
    char arg_meta[DSD_TEST_PATH_MAX];
    char arg_out[] = "-o";
    char arg_null[] = "null";
    DSD_SNPRINTF(arg_meta, sizeof arg_meta, "%s", meta);
    char* argv[] = {arg0, arg_frontend, arg_none, arg_mode, arg_replay, arg_meta, arg_out, arg_null, NULL};
    int boot_rc = 1;
    if (dsd_runtime_bootstrap(8, argv, opts, state, NULL, &boot_rc) != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "FAIL: bootstrap did not continue for %s (rc %d)\n", meta, boot_rc);
        return -1;
    }
    const dsd_engine_lifecycle_hooks hooks = {.start = observe_start, .stop = observe_stop, .context = view};
    return dsd_engine_run_with_lifecycle(opts, state, &hooks);
}

/* Replace the one occurrence of @p from in @p text (capacity @p cap) with @p to. 0 on success. */
static int
replace_once(char* text, size_t cap, const char* from, const char* to) {
    char* at = strstr(text, from);
    if (at == NULL || strstr(at + 1, from) != NULL) {
        return -1;
    }
    const size_t from_len = strlen(from);
    const size_t to_len = strlen(to);
    const size_t tail_len = strlen(at + from_len);
    if ((size_t)(at - text) + to_len + tail_len + 1U > cap) {
        return -1;
    }
    DSD_MEMMOVE(at + to_len, at + from_len, tail_len + 1U);
    DSD_MEMCPY(at, to, to_len);
    return 0;
}

/* p25p1_c4fm_vc's sidecar with the capture's centre moved to kCarrierBHz and its data file named absolutely, written
   to @p dir/carrier_b.iq.json (@p out). */
static int
write_carrier_b_sidecar(const char* dir, char* out, size_t out_size) {
    char source[DSD_TEST_PATH_MAX];
    char data[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(source, sizeof source, DSD_NEO_TEST_IQ_FIXTURE_DIR, "p25p1_c4fm_vc.iq.json") != 0
        || dsd_test_path_join(data, sizeof data, DSD_NEO_TEST_IQ_FIXTURE_DIR, "p25p1_c4fm_vc.iq") != 0
        || dsd_test_path_join(out, out_size, dir, "carrier_b.iq.json") != 0) {
        return -1;
    }
    char text[8192];
    FILE* in = dsd_fopen_existing_regular_file(source, "rb");
    if (in == NULL) {
        return -1;
    }
    const size_t n = fread(text, 1U, sizeof text - 1U, in);
    (void)fclose(in);
    text[n] = '\0';
    /* A JSON string: escape the path's backslashes (Windows). */
    char data_json[2U * DSD_TEST_PATH_MAX + 32U];
    size_t used = 0U;
    used += (size_t)DSD_SNPRINTF(data_json, sizeof data_json, "\"data_file\": \"");
    for (const char* p = data; *p != '\0' && used + 3U < sizeof data_json; p++) {
        if (*p == '\\') {
            data_json[used++] = '\\';
        }
        data_json[used++] = *p;
    }
    DSD_SNPRINTF(data_json + used, sizeof data_json - used, "\"");
    char center[64];
    char capture_center[64];
    DSD_SNPRINTF(center, sizeof center, "\"center_frequency_hz\": %u", (unsigned)kCarrierBHz);
    DSD_SNPRINTF(capture_center, sizeof capture_center, "\"capture_center_frequency_hz\": %u", (unsigned)kCarrierBHz);
    if (replace_once(text, sizeof text, "\"data_file\": \"p25p1_c4fm_vc.iq\"", data_json) != 0
        || replace_once(text, sizeof text, "\"capture_center_frequency_hz\": 851375000", capture_center) != 0
        || replace_once(text, sizeof text, "\"center_frequency_hz\": 851375000", center) != 0) {
        return -1;
    }
    FILE* fp = dsd_fopen_private(out, "wb");
    if (fp == NULL) {
        return -1;
    }
    const size_t len = strlen(text);
    const int ok = fwrite(text, 1U, len, fp) == len;
    return (fclose(fp) == 0 && ok) ? 0 : -1;
}

/* The call history rows of carrier B's call: P25, from radio 1 to group 1. */
static void
check_history(const dsd_state* state) {
    int rows = 0;
    for (int slot = 0; slot < 2; slot++) {
        const Event_History_I* history = &state->event_history_s[slot];
        for (int i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
            const Event_History* row = &history->Event_History_Items[i];
            if (!DSD_SYNC_IS_P25((int)row->systype) || row->source_id != 1U || row->target_id != 1U) {
                continue;
            }
            rows++;
            check_u("B's history row: no WACN", row->sys_id1, 0U);
            check_u("B's history row: no SYS", row->sys_id2, 0U);
            check_u("B's history row: no RFSS", row->sys_id4, 0U);
            check_u("B's history row: no SITE", row->sys_id5, 0U);
            check_u("B's history row: its NAC",
                    row->access_code_kind == (uint8_t)DSD_ACCESS_CODE_NAC ? row->access_code : 0xFFFFU, B_NAC);
            check_text("B's history row: system identity", row->sysid_string, "P25_293");
            check_u("B's history row: B's frequency", (unsigned long)row->freq_hz, kCarrierBHz);
        }
    }
    check("B's call reached the history", rows > 0);
}

int
main(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_replay_identity")) {
        DSD_FPRINTF(stderr, "FAIL: could not create a temp directory\n");
        return 1;
    }
    char meta_a[DSD_TEST_PATH_MAX];
    char meta_b[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(meta_a, sizeof meta_a, DSD_NEO_TEST_IQ_FIXTURE_DIR, "p25p1_c4fm_cc.iq.json") != 0
        || write_carrier_b_sidecar(dir, meta_b, sizeof meta_b) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not prepare the fixtures\n");
        return 1;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        DSD_FPRINTF(stderr, "FAIL: out of memory\n");
        free(state);
        free(opts);
        return 1;
    }
    initOpts(opts);
    initState(state);

    /* Carrier A: the trunked system's control channel. */
    run_view a = {0};
    check("carrier A ran cleanly", run_replay(meta_a, opts, state, &a) == 0);
    check_u("carrier A: the stop hook ran once", (unsigned long)a.stops, 1U);
    const uint32_t a_parts =
        DSD_RXID_ACCESS | DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE | DSD_RXID_CC_FREQ;
    check_u("carrier A: family", a.record.family, DSD_RXID_FAMILY_P25);
    check_u("carrier A: parts", a.record.valid & a_parts, a_parts);
    check_u("carrier A: WACN", a.record.p25_wacn, A_WACN);
    check_u("carrier A: SYS", a.record.p25_sysid, A_SYSID);
    check_u("carrier A: NAC", a.record.access, A_NAC);
    check_u("carrier A: control channel", a.record.cc_freq_hz, kCarrierAHz);
    check_u("carrier A: the view's control channel", a.view_cc_freq_hz, kCarrierAHz);
    check_text(
        "carrier A: the shared text", a.full,
        "P25 \xC2\xB7 NAC 140 \xC2\xB7 SYS 14C \xC2\xB7 RFSS 5 \xC2\xB7 SITE 9 \xC2\xB7 WACN BEE00 \xC2\xB7 LRA 00");

    /* Carrier B, on the same decoder state: another replay is another carrier. */
    run_view b = {0};
    check("carrier B ran cleanly", run_replay(meta_b, opts, state, &b) == 0);
    check_u("carrier B: the stop hook ran once", (unsigned long)b.stops, 1U);
    check_u("carrier B: family", b.record.family, DSD_RXID_FAMILY_P25);
    check_u("carrier B: only its NAC", b.record.valid, DSD_RXID_ACCESS);
    check_u("carrier B: NAC", b.record.access, B_NAC);
    check_u("carrier B: no control channel in the view", b.view_cc_freq_hz, 0U);
    check_text("carrier B: the shared text", b.full, "P25 \xC2\xB7 NAC 293");
    check("carrier B: no WACN in the text", strstr(b.full, "BEE00") == NULL && strstr(b.full, "14C") == NULL);
    check_history(state);

    freeState(state);
    free(state);
    free(opts);
    const char* const files[] = {"carrier_b.iq.json", NULL};
    if (dsd_test_remove_temp_dir(dir, files) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not remove %s\n", dir);
        g_failures++;
    }
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d replay identity check(s) failed\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "ENGINE_REPLAY_IDENTITY: OK\n");
    return 0;
}
