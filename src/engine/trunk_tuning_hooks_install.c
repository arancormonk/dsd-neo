// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/control.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>

void
dsd_engine_trunk_tuning_hooks_install(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq = trunk_tune_to_freq;
    hooks.tune_to_cc = trunk_tune_to_cc;
    hooks.return_to_cc = return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}
