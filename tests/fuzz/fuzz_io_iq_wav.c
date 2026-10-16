// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * WAV I/Q recordings (issue #701): the container parser, then the replay reader over whatever it accepts, read in
 * sizes the input picks (a read smaller than one cf32 frame included), with a seek and a rewind.
 */

#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_wav.h>

#include "fuzz_support.h"

static int
make_temp_wav_path(char* out_path, size_t out_path_size) {
    char base_path[DSD_TEST_PATH_MAX];
    if (dsd_fuzz_make_temp_path(base_path, sizeof(base_path), "dsdneo_fuzz_iq_wav") != 0) {
        return -1;
    }
    (void)remove(base_path);
    int n = DSD_SNPRINTF(out_path, out_path_size, "%s.wav", base_path);
    if (n < 0 || (size_t)n >= out_path_size) {
        return -1;
    }
    return 0;
}

static void
drain(dsd_iq_replay_source* src, size_t step) {
    static uint8_t buf[65536];
    for (int i = 0; i < 4096; i++) {
        size_t got = 0;
        if (dsd_iq_replay_read(src, buf, step, &got) != DSD_IQ_OK || got == 0U) {
            return;
        }
    }
}

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == NULL) {
        return 0;
    }
    size = dsd_fuzz_bounded_size(size);

    char path[DSD_TEST_PATH_MAX];
    if (make_temp_wav_path(path, sizeof(path)) != 0) {
        return 0;
    }
    if (dsd_fuzz_write_file(path, data, size) != 0) {
        (void)remove(path);
        return 0;
    }

    dsd_iq_wav_info info;
    char err[512];
    DSD_MEMSET(err, 0, sizeof(err));
    (void)dsd_iq_wav_read_info(path, &info, err, sizeof(err));

    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_source* src = NULL;
    if (dsd_iq_replay_open(path, &cfg, &src, err, sizeof(err)) == DSD_IQ_OK) {
        const size_t step = size > 0U ? (size_t)(data[size - 1U] % 23U) + 1U : 1U;
        drain(src, step);
        (void)dsd_iq_replay_seek(src, cfg.data_bytes / 2U + 3U);
        drain(src, 65536U);
        (void)dsd_iq_replay_rewind(src);
        drain(src, 4096U);
        dsd_iq_replay_close(src);
        dsd_iq_replay_config_clear(&cfg);
    }

    (void)remove(path);
    return 0;
}
