// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/config.h>

#include "fuzz_support.h"

extern "C" int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == NULL) {
        return 0;
    }

    size = dsd_fuzz_bounded_size(size);

    char path[DSD_TEST_PATH_MAX];
    if (dsd_fuzz_make_temp_path(path, sizeof(path), "dsdneo_fuzz_config") != 0) {
        return 0;
    }
    if (dsd_fuzz_write_file(path, data, size) != 0) {
        (void)remove(path);
        return 0;
    }

    dsdneoUserConfig cfg;
    (void)dsd_user_config_load(path, &cfg);

    dsdcfg_diagnostics_t diags;
    (void)dsd_user_config_validate(path, &diags);
    dsdcfg_diags_free(&diags);

    (void)remove(path);
    return 0;
}
