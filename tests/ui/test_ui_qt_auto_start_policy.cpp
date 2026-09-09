// SPDX-License-Identifier: GPL-3.0-or-later
#include "auto_start_policy.h"

int
main() {
    using dsd_qt::autoStartAllowed;
    static_assert(autoStartAllowed(true, true, 0, false, true, true), "ready USB target starts");
    static_assert(!autoStartAllowed(false, true, 0, false, true, true), "disabled preference blocks start");
    static_assert(!autoStartAllowed(true, false, 0, false, true, true), "onboarding blocks start");
    for (int state = 1; state <= 4; ++state) {
        if (autoStartAllowed(true, true, state, false, true, true)) {
            return 1;
        }
    }
    static_assert(!autoStartAllowed(true, true, 0, true, true, true), "overlay blocks start");
    static_assert(!autoStartAllowed(true, true, 0, false, false, true), "missing target blocks start");
    static_assert(!autoStartAllowed(true, true, 0, false, true, false), "non-USB target blocks start");
    return 0;
}
