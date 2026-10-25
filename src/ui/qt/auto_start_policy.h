// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_AUTO_START_POLICY_H
#define DSD_NEO_UI_QT_AUTO_START_POLICY_H

namespace dsd_qt {
// SessionState::Idle is 0 on every host. Failed deliberately does not qualify.
// targetKindMatches: the target runs on a USB receiver (RTL-SDR or Airspy) of the kind that just attached (#647 D3a).
constexpr bool
autoStartAllowed(bool enabled, bool onboardingDone, int sessionState, bool overlayOpen, bool targetExists,
                 bool targetKindMatches) {
    return enabled && onboardingDone && sessionState == 0 && !overlayOpen && targetExists && targetKindMatches;
}
} // namespace dsd_qt
#endif
