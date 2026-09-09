// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "auto_start_policy.h"

int
main() {
    using dsd_qt::autoStartAllowed;
    assert(autoStartAllowed(true, true, 0, false, true, true));
    assert(!autoStartAllowed(false, true, 0, false, true, true));
    assert(!autoStartAllowed(true, false, 0, false, true, true));
    for (int state = 1; state <= 4; ++state) {
        assert(!autoStartAllowed(true, true, state, false, true, true));
    }
    assert(!autoStartAllowed(true, true, 0, true, true, true));
    assert(!autoStartAllowed(true, true, 0, false, false, true));
    assert(!autoStartAllowed(true, true, 0, false, true, false));
    return 0;
}
