// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The desktop CLI links the terminal UI and the Airspy adapter into one executable. The adapter's
 * gain calls must reach the SDK, not a terminal menu callback that carries the same name (#806).
 * This executable links the terminal UI and the adapter against the SDK double built as a shared
 * library, as dsd-neo links libairspy; a menu symbol named like an SDK call would win the lookup
 * and the double would never see the gains.
 */

#include <dsd-neo/core/airspy_config.h>
#include <stddef.h>
#include <stdint.h>
#include "airspy_sdk_double.h"
#include "airspy_source.h"

// Declared here, not through menu_actions.h: the header's compile-time clash with the SDK is the other test's job,
// and this one must build, and fail at run time, even where the clash is present.
extern "C" void act_toggle_invert(void* v);

using namespace airspy_double;

struct airspy_source;

static void
sink(void*, const float*, size_t, uint64_t) {}

int
main() {
    // Keep the terminal menu actions in the link, so a clashing name would be present in this executable.
    void (*volatile menu_action)(void*) = act_toggle_invert;
    CHECK(menu_action != nullptr);

    dsd_airspy_config cfg;
    dsd_airspy_config_defaults(&cfg);
    cfg.gain_mode = DSD_AIRSPY_MANUAL;
    cfg.lna_gain = 11;
    cfg.mixer_gain = 5;
    cfg.vga_gain = 9;
    cfg.lna_agc = 0;
    cfg.mixer_agc = 0;
    airspy_source* s = airspy_source_open(&cfg, sink, nullptr);
    CHECK(s != nullptr);
    CHECK(lna == 11 && mixer == 5 && vga == 9 && !lna_agc && !mixer_agc);
    cfg.gain_mode = DSD_AIRSPY_SENSITIVITY;
    cfg.sensitivity_gain = 13;
    CHECK(airspy_source_controls(s, &cfg) == 0 && sensitivity == 13);
    cfg.gain_mode = DSD_AIRSPY_LINEARITY;
    cfg.linearity_gain = 17;
    CHECK(airspy_source_controls(s, &cfg) == 0 && linearity == 17);
    airspy_source_close(s);
    return 0;
}
