// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "presentation_scale.h"

#include <QString>
#include <algorithm>
#include <cmath>
#include <utility>

namespace dsd_android {

namespace {

// Densities are multiples of 1/160 and ratios follow them, so a relative 1e-4 only absorbs float round trips.
constexpr qreal kScaleTolerance = 1e-4;
// Logical pixels; the keyboard edge is a whole physical pixel divided by the ratio.
constexpr qreal kKeyboardTolerance = 1e-3;

bool
usable(qreal value) {
    return std::isfinite(value) && value > 0;
}

} // namespace

bool
scale_differs(qreal a, qreal b) {
    const bool a_usable = usable(a);
    const bool b_usable = usable(b);
    if (!a_usable || !b_usable) {
        return a_usable != b_usable;
    }
    return std::fabs(a - b) > kScaleTolerance * std::max(a, b);
}

bool
FontKey::valid() const {
    return usable(font_scale) && usable(density_dpi);
}

bool
FontKey::sameAs(const FontKey& other) const {
    return !scale_differs(font_scale, other.font_scale) && !scale_differs(density_dpi, other.density_dpi);
}

PresentationScale::Changes
PresentationScale::tick(Platform& platform) {
    const qreal activity = platform.activityDensity();
    const qreal qt = platform.qtDensity();
    followDensity(platform, qt);
    const bool settled = considerPush(platform, activity, qt);
    // Read after the resync: announcing the new DPI is what turns Qt's scaling on when it started at 1.
    const qreal ratio = platform.effectiveRatio();
    Changes changes;
    // While a push can still bring Qt to Android's density, the table would pair Android's new pixels with Qt's old
    // ratio: keep the published one until they agree.
    changes.typography = settled && refreshFonts(platform, ratio);
    changes.keyboard = refreshKeyboard(platform, ratio);
    return changes;
}

void
PresentationScale::followDensity(Platform& platform, qreal density) {
    if (!usable(density) || (usable(m_synced_density) && !scale_differs(density, m_synced_density))) {
        return;
    }
    // The first sample too: Qt can have moved between creating the screen and this host's first tick.
    platform.resync();
    // Qt's listener writes the density on another thread. Had it moved during the announcements, some of them used
    // another value: nothing counts as synced, not even a return to the density synced before, and the next tick
    // announces again.
    m_synced_density = scale_differs(platform.qtDensity(), density) ? qreal(0) : density;
}

bool
PresentationScale::considerPush(Platform& platform, qreal activity, qreal qt) {
    if (!usable(activity) || !usable(qt)) {
        return true;
    }
    if (!scale_differs(activity, qt)) {
        // Qt reached it: should Qt move away from this density again, it gets a fresh budget.
        if (!scale_differs(activity, m_push_target)) {
            m_push_count = 0;
            m_budget_reported = false;
        }
        return true;
    }
    if (m_push_disabled) {
        return true; // nothing will close the gap but Qt itself
    }
    if (m_push_in_flight) {
        return false;
    }
    if (!scale_differs(activity, m_push_target) && m_push_count >= kPushBudget) {
        if (!m_budget_reported) {
            m_budget_reported = true;
            platform.diagnostic(QStringLiteral("display size: Qt kept density %1 after %2 updates to %3; the UI "
                                               "follows Qt's own density until the display size changes again")
                                    .arg(qt)
                                    .arg(kPushBudget)
                                    .arg(activity));
        }
        return true;
    }
    m_push_in_flight = true;
    platform.startDensityPush();
    return false;
}

void
PresentationScale::pushFinished(DensityPush outcome, qreal pushed_density, Platform& platform) {
    m_push_in_flight = false;
    if (outcome == DensityPush::Unavailable) {
        // A Qt upgrade that renamed or removed the entry point would fail the same way every time.
        m_push_disabled = true;
        platform.diagnostic(QStringLiteral("display size: Qt's density update is missing; the UI follows "
                                           "display-size changes only when Qt notices them"));
        return;
    }
    if (outcome != DensityPush::Pushed || !usable(pushed_density)) {
        return;
    }
    // Charged to the density written, which the push read when it ran: during quick changes it can differ from the
    // one the tick saw.
    if (scale_differs(pushed_density, m_push_target)) {
        m_push_target = pushed_density;
        m_push_count = 0;
        m_budget_reported = false;
    }
    ++m_push_count;
}

bool
PresentationScale::refreshFonts(Platform& platform, qreal ratio) {
    if (!usable(ratio)) {
        return false;
    }
    const FontKey key = platform.fontKey();
    if (!key.valid()) {
        return false;
    }
    if (!m_font_sizes.isEmpty() && key.sameAs(m_font_key) && !scale_differs(ratio, m_font_ratio)) {
        return false;
    }
    FontKey table_key;
    QVector<qreal> pixels;
    if (!platform.fontTable(&table_key, &pixels) || !table_key.valid() || pixels.size() != kFontTableMaxSp + 1) {
        return false; // the previous table stays; the next tick tries again
    }
    for (int sp = 0; sp <= kFontTableMaxSp; ++sp) {
        pixels[sp] /= ratio;
        // A failed read is 0: never publish it.
        if (!std::isfinite(pixels[sp]) || pixels[sp] < 0 || (sp >= 1 && !(pixels[sp] > 0))) {
            return false;
        }
    }
    // Keyed by what the table was read with: if the key moved since fontKey(), the next tick rebuilds.
    m_font_sizes = std::move(pixels);
    m_font_key = table_key;
    m_font_ratio = ratio;
    ++m_font_revision;
    return true;
}

bool
PresentationScale::refreshKeyboard(Platform& platform, qreal ratio) {
    int pixels = -1;
    if (!platform.keyboardTopPixels(&pixels)) {
        return false; // unreadable: keep the inset the UI has
    }
    const qreal top = pixels < 0 ? qreal(-1) : pixels / (usable(ratio) ? ratio : qreal(1));
    const bool changed = (top < 0) != (m_keyboard_top < 0) || std::fabs(top - m_keyboard_top) > kKeyboardTolerance;
    if (changed) {
        m_keyboard_top = top;
    }
    return changed;
}

bool
PresentationScale::fontPixelSize(qreal sp, qreal* pixels) const {
    if (m_font_sizes.size() != kFontTableMaxSp + 1 || !std::isfinite(sp) || sp < 0 || sp > kFontTableMaxSp) {
        return false;
    }
    const int low = static_cast<int>(std::floor(std::min(sp, qreal(kFontTableMaxSp - 1))));
    *pixels = m_font_sizes[low] + (m_font_sizes[low + 1] - m_font_sizes[low]) * (sp - low);
    return true;
}

} // namespace dsd_android
