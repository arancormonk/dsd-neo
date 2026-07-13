// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>

#include "engine_hooks_install.h"

void
dsd_engine_trunk_tuning_hooks_install(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = dsd_engine_trunk_tune_to_freq_request;
    hooks.tune_to_cc_request = dsd_engine_trunk_tune_to_cc_request;
    hooks.return_to_cc_request = dsd_engine_return_to_cc_request;
    dsd_trunk_tuning_hooks_set(hooks);
}
