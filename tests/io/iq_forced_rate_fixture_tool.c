// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Makes the I/Q recordings the DECODE_IQ_*_AIRSPY_* replay cases decode (issue #806), in the build tree, from the
 * committed fixtures; nothing it writes is committed.
 *
 *   dsd-neo_test_iq_forced_rate_fixture_tool <tests/fixtures/iq> <out_dir>
 *
 * Each recording is a committed 48 or 72 kHz fixture as an Airspy at a forced rate would capture it
 * (forced_rate_input.h: upsampled with dsd_iq_resampler, receiver noise each side, cu8), with the sidecar a live
 * capture of that chain writes: the device's rate as the nominal one, forced, and the I/Q rate step to the DSP
 * bandwidth rate after the half-band passes. A replay keeps that chain and runs the step, as the live stream does. Each
 * goes to a directory of its own, <out_dir>/<name>/<name>.iq and .iq.json, one per test that reads it.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <errno.h>
#include <stdio.h>
#include "forced_rate_input.h"

typedef struct {
    const char* name;
    dsd_test_forced_rate_spec spec;
} forced_rate_fixture;

/* 50 ms of noise each side: far past every chain's tail bound (about 2 ms), and short next to the excerpt. */
#define PAD_S 0.05

static const forced_rate_fixture kFixtures[] = {
    {"p25p1_cqpsk_cc_airspy_312k", {"p25p1_cqpsk_cc", 0.0, 0.0, 312500U, 48000U, PAD_S}},
    {"p25p1_cqpsk_vc_airspy_375k_det", {"p25p1_cqpsk_vc", 0.0, 0.0, 375000U, 48000U, PAD_S}},
    {"p25p1_cqpsk_vc_airspy_375k_sink", {"p25p1_cqpsk_vc", 0.0, 0.0, 375000U, 48000U, PAD_S}},
    {"p25p1_cqpsk_simulcast_airspy_375k", {"p25p1_cqpsk_cc_simulcast", 0.0, 0.0, 375000U, 48000U, PAD_S}},
    {"p25p2_cc_airspy_375k", {"p25p2_cc", 0.0, 0.0, 375000U, 48000U, PAD_S}},
    {"dmr_voice_airspy_312k", {"dmr_voice_synth", 0.0, 2.0, 312500U, 48000U, PAD_S}},
    {"tetra_call_airspy_375k", {"tetra_call_real_air_72k", 0.0, 2.0, 375000U, 72000U, PAD_S}},
};

static int
write_bytes(const char* path, const void* data, size_t len) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    const int ok = fwrite(data, 1U, len, fp) == len;
    return (fclose(fp) == 0 && ok) ? 0 : -1;
}

/* The sidecar a live Airspy capture of @p in's chain writes, cu8 in place of its cf32. */
static int
write_sidecar(const char* path, const char* name, const forced_rate_fixture* fx, const dsd_test_forced_rate_input* in) {
    const unsigned base = 1U << in->passes;
    char text[4096];
    const int n = DSD_SNPRINTF(
        text, sizeof(text),
        "{\n"
        "  \"format\": \"dsd-neo-iq\",\n"
        "  \"version\": 1,\n"
        "  \"sample_format\": \"cu8\",\n"
        "  \"iq_order\": \"IQ\",\n"
        "  \"endianness\": \"none\",\n"
        "  \"capture_stage\": \"post_mute_pre_widen\",\n"
        "  \"sample_rate_hz\": %u,\n"
        "  \"center_frequency_hz\": %llu,\n"
        "  \"capture_center_frequency_hz\": %llu,\n"
        "  \"ppm\": 0,\n"
        "  \"tuner_gain_tenth_db\": 0,\n"
        "  \"tuner_gain_known\": false,\n"
        "  \"rtl_dsp_bw_khz\": %u,\n"
        "  \"base_decimation\": %u,\n"
        "  \"post_downsample\": 1,\n"
        "  \"demod_rate_hz\": %u,\n"
        "  \"nominal_rate_hz\": %u,\n"
        "  \"capture_rate_device_forced\": true,\n"
        "  \"dsp_rate_hz\": %u,\n"
        "  \"iq_step_l\": %d,\n"
        "  \"iq_step_m\": %d,\n"
        "  \"iq_step_approx_ppm\": 0.000000,\n"
        "  \"offset_tuning_enabled\": false,\n"
        "  \"fs4_shift_enabled\": false,\n"
        "  \"combine_rotate_enabled\": false,\n"
        "  \"muted_bytes_excluded\": true,\n"
        "  \"contains_retunes\": false,\n"
        "  \"capture_retune_count\": 0,\n"
        "  \"source_backend\": \"airspy\",\n"
        "  \"source_args\": \"fixture\",\n"
        "  \"capture_started_utc\": \"1970-01-01T00:00:00Z\",\n"
        "  \"data_file\": \"%s.iq\",\n"
        "  \"data_bytes\": %llu,\n"
        "  \"capture_drops\": 0,\n"
        "  \"capture_drop_blocks\": 0,\n"
        "  \"input_ring_drops\": 0,\n"
        "  \"notes\": \"made by dsd-neo_test_iq_forced_rate_fixture_tool: %s from %.3f s, upsampled to %u Hz\"\n"
        "}\n",
        (unsigned)in->device_rate_hz, (unsigned long long)in->center_frequency_hz,
        (unsigned long long)in->center_frequency_hz, (unsigned)(fx->spec.dsp_rate_hz / 1000U), base,
        (unsigned)(in->device_rate_hz / base), (unsigned)in->device_rate_hz, (unsigned)fx->spec.dsp_rate_hz, in->step_l,
        in->step_m, name, (unsigned long long)(in->pairs * 2U), fx->spec.fixture, fx->spec.start_s,
        (unsigned)in->device_rate_hz);
    if (n <= 0 || (size_t)n >= sizeof(text)) {
        return -1;
    }
    return write_bytes(path, text, (size_t)n);
}

static int
make_fixture(const char* fixtures, const char* out_dir, const forced_rate_fixture* fx) {
    char err[512] = {0};
    dsd_test_forced_rate_input in;
    if (dsd_test_forced_rate_input_make(fixtures, &fx->spec, &in, err, sizeof(err)) != 0) {
        DSD_FPRINTF(stderr, "iq_forced_rate_fixture_tool: %s: %s\n", fx->name, err);
        return -1;
    }
    char dir[4096];
    char data_path[4096];
    char meta_path[4096];
    DSD_SNPRINTF(dir, sizeof(dir), "%s/%s", out_dir, fx->name);
    DSD_SNPRINTF(data_path, sizeof(data_path), "%s/%s.iq", dir, fx->name);
    DSD_SNPRINTF(meta_path, sizeof(meta_path), "%s/%s.iq.json", dir, fx->name);
    int rc = (dsd_mkdir(dir, 0755) == 0 || errno == EEXIST) ? 0 : -1;
    if (rc == 0) {
        rc = write_bytes(data_path, in.cu8, in.pairs * 2U);
    }
    if (rc == 0) {
        rc = write_sidecar(meta_path, fx->name, fx, &in);
    }
    if (rc != 0) {
        DSD_FPRINTF(stderr, "iq_forced_rate_fixture_tool: %s: cannot write %s\n", fx->name, dir);
    }
    /* The sidecar must read back as the chain it records: the reader's own checks, the rate chain's included. */
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    if (rc == 0 && dsd_iq_replay_read_metadata(meta_path, &cfg, err, sizeof(err)) != DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "iq_forced_rate_fixture_tool: %s: the sidecar does not read back: %s\n", fx->name, err);
        rc = -1;
    }
    dsd_iq_replay_config_clear(&cfg);
    if (rc == 0 && in.clipped != 0U) {
        DSD_FPRINTF(stderr, "iq_forced_rate_fixture_tool: %s: %zu samples clipped\n", fx->name, in.clipped);
        rc = -1;
    }
    dsd_test_forced_rate_input_free(&in);
    return rc;
}

int
main(int argc, char** argv) {
    if (argc != 3) {
        DSD_FPRINTF(stderr, "usage: %s <tests/fixtures/iq> <out_dir>\n",
                    argc > 0 ? argv[0] : "iq_forced_rate_fixture_tool");
        return 2;
    }
    const char* fixtures = argv[1];
    const char* out_dir = argv[2];
    if (dsd_mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        DSD_FPRINTF(stderr, "iq_forced_rate_fixture_tool: cannot make %s\n", out_dir);
        return 1;
    }
    for (size_t i = 0; i < sizeof(kFixtures) / sizeof(kFixtures[0]); i++) {
        if (make_fixture(fixtures, out_dir, &kFixtures[i]) != 0) {
            return 1;
        }
    }
    printf("iq_forced_rate_fixture_tool: wrote the forced-rate fixtures into %s\n", out_dir);
    return 0;
}
