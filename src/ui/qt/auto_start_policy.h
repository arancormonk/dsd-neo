// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_AUTO_START_POLICY_H
#define DSD_NEO_UI_QT_AUTO_START_POLICY_H

namespace dsd_qt {
// SessionState::Idle is 0 on every host. Failed deliberately does not qualify.
constexpr bool
autoStartAllowed(bool enabled, bool onboardingDone, int sessionState, bool overlayOpen, bool targetExists,
                 bool targetUsb) {
    return enabled && onboardingDone && sessionState == 0 && !overlayOpen && targetExists && targetUsb;
}
} // namespace dsd_qt
#endif
