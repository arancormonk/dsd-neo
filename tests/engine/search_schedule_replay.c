// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/**
 * @file
 * @brief How long a Signal search probe takes to identify a carrier (issue #804).
 *
 * Production Signal search refuses an I/Q replay, so this runs the real engine on a replay with the search's probe
 * forced instead: the frame sync's signal_search_probe hook answers 1 from the first acquisition reset (as a search
 * visit's tune runs one) until the first admitted protocol evidence, which a recording evidence hook takes in place of
 * the search coordinator and which clears the hunt's kept order as the coordinator does. Every other frame-sync hook
 * is the engine's own. The decoder starts on 4800/4 (C4FM unless a -m lock says otherwise), as every Auto visit does.
 *
 * Usage: search_schedule_replay FIXTURE.iq.json OFFSET_MS DEADLINE_MS [--expect PROTOCOL] [--expect-profile N]
 *        [--hold-ms MS] [--ordinary] [-- DECODER ARGS...]
 *
 * The landing comes OFFSET_MS into the capture: the run replays a scratch copy of the cu8 fixture that starts there
 * and goes on cyclically (back to the capture's start at its end) for as long as the run needs. The decoder arguments
 * default to -fa. The run stops DEADLINE_MS after the landing when nothing identified the carrier, and HOLD_MS after it
 * otherwise (DEADLINE_MS by default), so a longer hold shows what the visit decodes once it listens. --ordinary
 * forces the probe's answer to 0 instead: the hunt's ordinary order and dwell, for comparison. Prints
 * "identified=<protocol> at_ms=<decode-clock ms after the landing> profile=<symbol profile index>" (or
 * "identified=none") and exits 0 when the carrier was identified within the deadline, as PROTOCOL and on profile N when
 * they are given.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

/* Scratch fixtures run on past the deadline by this much, so the run stops on its own clock, not the capture's end. */
#define SCHEDULE_TAIL_MS  500U
#define SCHEDULE_MAX_ARGS 32

typedef struct {
    const char* fixture;
    uint32_t offset_ms;
    uint32_t deadline_ms;
    uint32_t hold_ms;
    const char* expect;
    int expect_profile;
    int ordinary;
    const char* decoder_args[SCHEDULE_MAX_ARGS];
    int decoder_argc;
} schedule_args;

static struct {
    const dsd_opts* opts;
    dsd_state* state;
    dsd_frame_sync_hooks engine;
    uint64_t landed_ms;
    uint32_t deadline_ms;
    uint32_t hold_ms;
    int probing;
    int ordinary;
    int identified;
    uint64_t at_ms;
    int profile;
    char protocol[16];
} g_run;

/* The forced search probe: an Auto visit probing until its first admitted evidence. */
static int
forced_search_probe(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return g_run.probing;
}

/* The engine's yield check, with the run's end added: the deadline when nothing identified the carrier, the hold
   otherwise. It runs every 64 symbols on the decoder thread, whatever the protocol reads. */
static int
schedule_should_yield(const dsd_opts* opts, dsd_state* state) {
    const int engine =
        g_run.engine.signal_search_should_yield ? g_run.engine.signal_search_should_yield(opts, state) : 0;
    const uint64_t elapsed = dsd_decode_now_mono_ms() - g_run.landed_ms;
    if (elapsed >= (g_run.identified ? g_run.hold_ms : g_run.deadline_ms)) {
        dsd_exitflag_store(1);
    }
    return engine;
}

/* The coordinator's evidence handling, as far as the schedule goes: the first evidence the search would admit ends the
   probe and the hunt's search order (dsd_engine_signal_search_evidence_at()). NXDN is named by the profile that
   confirmed it, as the search names it. */
static void
record_evidence(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target, uint32_t source,
                const char* access) {
    (void)target;
    (void)source;
    (void)access;
    if (!g_run.probing || !state || !protocol || state->input_interrupted) {
        return;
    }
    const int admitted = sync >= 0 ? dsd_receive_allows(g_run.opts, state, sync, slot)
                                   : dsd_receive_current_allows(g_run.opts, state, state->currentslot);
    if (!admitted) {
        return;
    }
    g_run.probing = 0;
    g_run.state->sps_hunt_search_order = 0U;
    g_run.identified = 1;
    g_run.at_ms = dsd_decode_now_mono_ms() - g_run.landed_ms;
    g_run.profile = state->sps_hunt_idx;
    const char* named = protocol;
    if (strcmp(protocol, "NXDN") == 0) {
        named = state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4 ? "NXDN48" : "NXDN96";
    }
    DSD_SNPRINTF(g_run.protocol, sizeof(g_run.protocol), "%s", named);
}

/* The landing: after the engine installed its hooks and set the session up, before the first symbol. */
static int
schedule_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    if (state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4 || (!opts->mod_cli_lock && state->rf_mod != 0)) {
        DSD_FPRINTF(stderr, "SEARCH SCHEDULE: the session starts on profile %d, modulation %d, not 4800/4 C4FM\n",
                    state->sps_hunt_idx, state->rf_mod);
        return -1;
    }
    g_run.opts = opts;
    g_run.state = state;
    dsd_frame_sync_hooks_get(&g_run.engine);
    dsd_frame_sync_hooks hooks = g_run.engine;
    hooks.signal_search_probe = g_run.ordinary ? NULL : forced_search_probe;
    hooks.signal_search_should_yield = schedule_should_yield;
    dsd_frame_sync_hooks_set(&hooks);
    dsd_signal_search_evidence_set(record_evidence);
    g_run.probing = 1;
    g_run.landed_ms = dsd_decode_now_mono_ms();
    /* The visit's tune resets the acquisition, which keeps the probe's answer for the hunt. */
    dsd_frame_sync_reset_acquisition(opts, state, 1);
    return state->sps_hunt_search_order == (g_run.ordinary ? 0U : 1U) ? 0 : -1;
}

static int
parse_ms(const char* text, uint32_t* out) {
    return dsd_parse_uint32_strict(text, 10, 600000U, out) == 0 ? 0 : -1;
}

/* One option at argv[*i] (and its value, which moves *i on). 0 when it was taken. */
static int
parse_option(int argc, char** argv, int* i, schedule_args* args) {
    const char* option = argv[*i];
    if (strcmp(option, "--ordinary") == 0) {
        args->ordinary = 1;
        return 0;
    }
    if (*i + 1 >= argc) {
        return -1;
    }
    const char* value = argv[++*i];
    if (strcmp(option, "--expect") == 0) {
        args->expect = value;
        return 0;
    }
    if (strcmp(option, "--expect-profile") == 0) {
        return dsd_parse_int_strict(value, 10, 0, DSD_FRAME_SYNC_SPS_PROFILE_COUNT - 1, &args->expect_profile);
    }
    if (strcmp(option, "--hold-ms") == 0) {
        return parse_ms(value, &args->hold_ms);
    }
    return -1;
}

static int
parse_args(int argc, char** argv, schedule_args* args) {
    DSD_MEMSET(args, 0, sizeof(*args));
    args->expect_profile = -1;
    if (argc < 4 || parse_ms(argv[2], &args->offset_ms) != 0 || parse_ms(argv[3], &args->deadline_ms) != 0
        || args->deadline_ms == 0U) {
        return -1;
    }
    args->fixture = argv[1];
    int i = 4;
    for (; i < argc && strcmp(argv[i], "--") != 0; i++) {
        if (parse_option(argc, argv, &i, args) != 0) {
            return -1;
        }
    }
    for (i++; i < argc && args->decoder_argc < SCHEDULE_MAX_ARGS; i++) {
        args->decoder_args[args->decoder_argc++] = argv[i];
    }
    if (args->decoder_argc == 0) {
        args->decoder_args[args->decoder_argc++] = "-fa";
    }
    if (args->hold_ms < args->deadline_ms) {
        args->hold_ms = args->deadline_ms;
    }
    return 0;
}

/* Copy the fixture's sidecar to @p json_path, naming @p data_leaf of @p data_bytes as its data. */
static int
write_sidecar(const char* source, const char* json_path, const char* data_leaf, uint64_t data_bytes) {
    FILE* in = fopen(source, "rb");
    FILE* out = in ? dsd_fopen_private(json_path, "wb") : NULL;
    int rc = in && out ? 0 : -1;
    char line[4096];
    while (rc == 0 && fgets(line, sizeof(line), in) != NULL) {
        const char* comma = strchr(line, ',') != NULL ? "," : "";
        if (strstr(line, "\"events\"") != NULL) {
            rc = -1; // a capture with retunes is not one carrier
        } else if (strstr(line, "\"data_file\"") != NULL) {
            DSD_FPRINTF(out, "  \"data_file\": \"%s\"%s\n", data_leaf, comma);
        } else if (strstr(line, "\"data_bytes\"") != NULL) {
            DSD_FPRINTF(out, "  \"data_bytes\": %llu%s\n", (unsigned long long)data_bytes, comma);
        } else {
            (void)fputs(line, out);
        }
    }
    if (out != NULL && fclose(out) != 0) {
        rc = -1;
    }
    if (in != NULL) {
        (void)fclose(in);
    }
    return rc;
}

/* Write the fixture's samples from @p offset_bytes on, cyclically, @p out_bytes of them. */
static int
write_cyclic_data(const char* source, uint64_t source_bytes, uint64_t offset_bytes, uint64_t out_bytes,
                  const char* data_path) {
    uint8_t* data = (uint8_t*)malloc((size_t)source_bytes);
    FILE* in = fopen(source, "rb");
    int rc = data && in && fread(data, 1, (size_t)source_bytes, in) == (size_t)source_bytes ? 0 : -1;
    if (in != NULL) {
        (void)fclose(in);
    }
    FILE* out = rc == 0 ? dsd_fopen_private(data_path, "wb") : NULL;
    rc = out ? 0 : -1;
    for (uint64_t written = 0; rc == 0 && written < out_bytes;) {
        const uint64_t at = (offset_bytes + written) % source_bytes;
        uint64_t chunk = source_bytes - at;
        if (chunk > out_bytes - written) {
            chunk = out_bytes - written;
        }
        rc = fwrite(data + at, 1, (size_t)chunk, out) == (size_t)chunk ? 0 : -1;
        written += chunk;
    }
    if (out != NULL && fclose(out) != 0) {
        rc = -1;
    }
    free(data);
    return rc;
}

/* The scratch fixture the landing replays: @p dir/landing.iq(.json). */
static int
write_landing_fixture(const schedule_args* args, const char* dir, char* json_path, size_t json_path_size,
                      char* data_path, size_t data_path_size) {
    dsd_iq_replay_config cfg;
    char error[256];
    if (dsd_iq_replay_read_metadata(args->fixture, &cfg, error, sizeof(error)) != 0) {
        DSD_FPRINTF(stderr, "SEARCH SCHEDULE: %s: %s\n", args->fixture, error);
        return -1;
    }
    const uint64_t frame_bytes = 2U; // cu8 I/Q
    const uint64_t rate = cfg.sample_rate_hz;
    if (cfg.format != DSD_IQ_FORMAT_CU8 || rate == 0U || cfg.data_bytes < frame_bytes) {
        DSD_FPRINTF(stderr, "SEARCH SCHEDULE: %s is not a cu8 capture\n", args->fixture);
        return -1;
    }
    const uint64_t source_bytes = cfg.data_bytes - cfg.data_bytes % frame_bytes;
    const uint64_t offset_bytes = ((uint64_t)args->offset_ms * rate / 1000U) * frame_bytes;
    uint64_t out_bytes = ((uint64_t)(args->hold_ms + SCHEDULE_TAIL_MS) * rate / 1000U) * frame_bytes;
    if (out_bytes < source_bytes) {
        out_bytes = source_bytes;
    }
    if (dsd_test_path_join(json_path, json_path_size, dir, "landing.iq.json") != 0
        || dsd_test_path_join(data_path, data_path_size, dir, "landing.iq") != 0
        || write_cyclic_data(cfg.data_path, source_bytes, offset_bytes, out_bytes, data_path) != 0
        || write_sidecar(args->fixture, json_path, "landing.iq", out_bytes) != 0) {
        DSD_FPRINTF(stderr, "SEARCH SCHEDULE: cannot write the landing fixture in %s\n", dir);
        return -1;
    }
    return 0;
}

static int
run_engine(const schedule_args* args, const char* json_path) {
    char* argv[SCHEDULE_MAX_ARGS + 8];
    int argc = 0;
    argv[argc++] = "dsd-neo";
    argv[argc++] = "--frontend";
    argv[argc++] = "none";
    for (int i = 0; i < args->decoder_argc; i++) {
        argv[argc++] = (char*)args->decoder_args[i];
    }
    argv[argc++] = "--iq-replay";
    argv[argc++] = (char*)json_path;
    argv[argc++] = "-o";
    argv[argc++] = "null";
    argv[argc] = NULL;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    int rc = 1;
    if (opts != NULL && state != NULL) {
        initOpts(opts);
        initState(state);
        g_run.deadline_ms = args->deadline_ms;
        g_run.hold_ms = args->hold_ms;
        g_run.ordinary = args->ordinary;
        const dsd_engine_lifecycle_hooks lifecycle = {schedule_start, NULL, NULL};
        dsd_exitflag_store(0);
        if (dsd_runtime_bootstrap(argc, argv, opts, state, NULL, &rc) == DSD_BOOTSTRAP_CONTINUE) {
            rc = dsd_engine_run_with_lifecycle(opts, state, &lifecycle);
        }
        freeState(state);
    }
    dsd_signal_search_evidence_set(NULL);
    free(state);
    free(opts);
    return rc;
}

int
main(int argc, char** argv) {
    schedule_args args;
    if (parse_args(argc, argv, &args) != 0) {
        DSD_FPRINTF(stderr,
                    "usage: %s FIXTURE.iq.json OFFSET_MS DEADLINE_MS [--expect PROTOCOL] [--expect-profile N] "
                    "[--hold-ms MS] [--ordinary] [-- DECODER ARGS...]\n",
                    argv[0]);
        return 2;
    }
    char dir[1024];
    char json_path[1200];
    char data_path[1200];
    if (dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_search_schedule") == NULL) {
        DSD_FPRINTF(stderr, "SEARCH SCHEDULE: cannot make a scratch directory\n");
        return 1;
    }
    int rc = write_landing_fixture(&args, dir, json_path, sizeof(json_path), data_path, sizeof(data_path));
    if (rc == 0) {
        rc = run_engine(&args, json_path);
    }
    (void)remove(json_path);
    (void)remove(data_path);
    (void)dsd_test_rmdir(dir);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "SEARCH SCHEDULE FAIL: the run returned %d\n", rc);
        return 1;
    }
    if (!g_run.identified || g_run.at_ms > args.deadline_ms) {
        DSD_FPRINTF(stdout, "identified=none\n");
        return 1;
    }
    DSD_FPRINTF(stdout, "identified=%s at_ms=%llu profile=%d\n", g_run.protocol, (unsigned long long)g_run.at_ms,
                g_run.profile);
    const int protocol_ok = args.expect == NULL || strcmp(args.expect, g_run.protocol) == 0;
    return protocol_ok && (args.expect_profile < 0 || args.expect_profile == g_run.profile) ? 0 : 1;
}
