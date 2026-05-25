// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/iq_replay.h>

#include "fuzz_support.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == NULL) {
        return 0;
    }

    size = dsd_fuzz_bounded_size(size);

    char path[DSD_TEST_PATH_MAX];
    if (dsd_fuzz_make_temp_json_path(path, sizeof(path), "dsdneo_fuzz_iq_metadata") != 0) {
        return 0;
    }
    if (dsd_fuzz_write_file(path, data, size) != 0) {
        (void)remove(path);
        return 0;
    }

    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char err[256];
    DSD_MEMSET(err, 0, sizeof(err));

    int rc = dsd_iq_replay_read_metadata(path, &cfg, err, sizeof(err));
    if (rc == 0) {
        dsd_iq_replay_config_clear(&cfg);
    }

    (void)remove(path);
    return 0;
}
