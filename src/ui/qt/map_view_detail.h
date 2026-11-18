// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Small rules MapViewItem's two source files share (map_view_item.cpp, map_view_paint.cpp). Private to them.
 */

#ifndef DSD_NEO_SRC_UI_QT_MAP_VIEW_DETAIL_H_
#define DSD_NEO_SRC_UI_QT_MAP_VIEW_DETAIL_H_

#include <cmath>

#include "map_view_item.h"

namespace dsd_qt {
namespace map_view_detail {

/** @brief Whether two values differ by more than rounding: a property set to what it holds changes nothing. */
inline bool
differs(double a, double b) {
    return std::fabs(a - b) > 1.0e-12;
}

/** @brief Draw order rank: plain tracks first, then emergencies, the selected track on top. */
inline int
draw_rank(const MapViewItem::TrackDraw& t) {
    if (t.selected) {
        return 2;
    }
    return t.emergency ? 1 : 0;
}

/**
 * @brief Label priority rank (higher is placed first): emergencies, then the selected track, then the rest.
 *
 * An emergency's label carries the word, so it is never the one dropped for another label (spec 4.6: never hue
 * alone).
 */
inline int
label_rank(const MapViewItem::TrackDraw& t) {
    if (t.emergency) {
        return 2;
    }
    return t.selected ? 1 : 0;
}

} // namespace map_view_detail
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_MAP_VIEW_DETAIL_H_ */
