// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/iq_replay.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%" PRIu64 " want=%" PRIu64 "\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_double(const char* label, double got, double want) {
    double delta = got - want;
    if (delta < 0.0) {
        delta = -delta;
    }
    if (delta > 0.000001) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%f want=%f\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_cstr(const char* label, const char* got, const char* want) {
    const char* actual = got ? got : "(null)";
    if (strcmp(actual, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%s want=%s\n", label, actual, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    uint64_t effective = 0;
    int mismatch = 0;

    rc |= expect_u64("cu8 alignment", dsd_iq_sample_format_alignment_bytes(DSD_IQ_FORMAT_CU8), 2);
    rc |= expect_u64("cf32 alignment", dsd_iq_sample_format_alignment_bytes(DSD_IQ_FORMAT_CF32), 8);
    rc |= expect_u64("cs16 alignment", dsd_iq_sample_format_alignment_bytes(DSD_IQ_FORMAT_CS16), 4);
    rc |= expect_u64("unknown alignment", dsd_iq_sample_format_alignment_bytes(DSD_IQ_FORMAT_UNKNOWN), 0);
    rc |= expect_cstr("cu8 name", dsd_iq_sample_format_name(DSD_IQ_FORMAT_CU8), "cu8");
    rc |= expect_cstr("cf32 name", dsd_iq_sample_format_name(DSD_IQ_FORMAT_CF32), "cf32");
    rc |= expect_cstr("cs16 name", dsd_iq_sample_format_name(DSD_IQ_FORMAT_CS16), "cs16");
    rc |= expect_cstr("unknown name", dsd_iq_sample_format_name(DSD_IQ_FORMAT_UNKNOWN), "unknown");

    rc |= expect_double("duration zero sample rate", dsd_iq_replay_estimate_duration_seconds(16, DSD_IQ_FORMAT_CU8, 0),
                        0.0);
    rc |= expect_double("duration unknown format",
                        dsd_iq_replay_estimate_duration_seconds(16, DSD_IQ_FORMAT_UNKNOWN, 48000), 0.0);
    rc |= expect_double("duration cu8", dsd_iq_replay_estimate_duration_seconds(96000, DSD_IQ_FORMAT_CU8, 48000), 1.0);
    rc |=
        expect_double("duration cf32", dsd_iq_replay_estimate_duration_seconds(384000, DSD_IQ_FORMAT_CF32, 48000), 1.0);
    rc |=
        expect_double("duration cs16", dsd_iq_replay_estimate_duration_seconds(192000, DSD_IQ_FORMAT_CS16, 48000), 1.0);

    rc |= expect_int("data_bytes=0 actual=10 cu8",
                     dsd_iq_replay_compute_effective_bytes(0, 10, DSD_IQ_FORMAT_CU8, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_u64("effective(0,10,cu8)", effective, 10);
    rc |= expect_int("mismatch(0,10,cu8)", mismatch, 0);

    rc |=
        expect_int("data_bytes=10 actual=10 cu8",
                   dsd_iq_replay_compute_effective_bytes(10, 10, DSD_IQ_FORMAT_CU8, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_u64("effective(10,10,cu8)", effective, 10);
    rc |= expect_int("mismatch(10,10,cu8)", mismatch, 0);

    rc |= expect_int("data_bytes=10 actual=8 cu8",
                     dsd_iq_replay_compute_effective_bytes(10, 8, DSD_IQ_FORMAT_CU8, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_u64("effective(10,8,cu8)", effective, 8);
    rc |= expect_int("mismatch(10,8,cu8)", mismatch, 1);

    rc |=
        expect_int("data_bytes=10 actual=12 cu8",
                   dsd_iq_replay_compute_effective_bytes(10, 12, DSD_IQ_FORMAT_CU8, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_u64("effective(10,12,cu8)", effective, 10);
    rc |= expect_int("mismatch(10,12,cu8)", mismatch, 1);

    rc |=
        expect_int("data_bytes=11 actual=11 cu8",
                   dsd_iq_replay_compute_effective_bytes(11, 11, DSD_IQ_FORMAT_CU8, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_u64("effective(11,11,cu8)", effective, 10);
    rc |= expect_int("mismatch(11,11,cu8)", mismatch, 0);

    rc |=
        expect_int("data_bytes=15 actual=15 cf32",
                   dsd_iq_replay_compute_effective_bytes(15, 15, DSD_IQ_FORMAT_CF32, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_u64("effective(15,15,cf32)", effective, 8);
    rc |= expect_int("mismatch(15,15,cf32)", mismatch, 0);

    rc |= expect_int("data_bytes=0 actual=1 cu8",
                     dsd_iq_replay_compute_effective_bytes(0, 1, DSD_IQ_FORMAT_CU8, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_u64("effective(0,1,cu8)", effective, 0);
    rc |= expect_int("mismatch(0,1,cu8)", mismatch, 0);
    rc |= expect_int("validator rejects zero", dsd_iq_replay_validate_effective_bytes_for_replay(effective, 0),
                     DSD_IQ_ERR_ALIGNMENT);

    dsd_iq_sample_format invalid_format = DSD_IQ_FORMAT_CU8;
    DSD_MEMSET(&invalid_format, 0, sizeof(invalid_format));
    rc |=
        expect_int("invalid format", dsd_iq_replay_compute_effective_bytes(0, 1, invalid_format, &effective, &mismatch),
                   DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_int("null out_effective",
                     dsd_iq_replay_compute_effective_bytes(0, 1, DSD_IQ_FORMAT_CU8, NULL, &mismatch),
                     DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_int("null mismatch accepted",
                     dsd_iq_replay_compute_effective_bytes(10, 10, DSD_IQ_FORMAT_CU8, &effective, NULL), DSD_IQ_OK);
    rc |= expect_u64("effective with null mismatch", effective, 10);

    rc |= expect_int("validator rejects zero loop=1", dsd_iq_replay_validate_effective_bytes_for_replay(0, 1),
                     DSD_IQ_ERR_ALIGNMENT);
    rc |= expect_int("validator accepts non-zero", dsd_iq_replay_validate_effective_bytes_for_replay(2, 0), DSD_IQ_OK);

    return rc ? 1 : 0;
}
