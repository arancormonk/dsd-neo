// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The name a position report's format goes by on screen (issue #800): the map's tracks and a History row's
 *        saved fix say it in the same words.
 */

#ifndef DSD_NEO_SRC_UI_QT_POSITION_KIND_TEXT_H_
#define DSD_NEO_SRC_UI_QT_POSITION_KIND_TEXT_H_

#include <QString>
#include <dsd-neo/core/position.h>

namespace dsd_qt {

/** @brief The report format @p kind (dsd_position_kind) in words, empty for one not known. */
inline QString
position_kind_text(int kind) {
    switch (kind) {
        case DSD_POS_DMR_LRRP: return QStringLiteral("LRRP");
        case DSD_POS_DMR_LOCN: return QStringLiteral("DMR LOCN");
        case DSD_POS_DMR_EMBEDDED: return QStringLiteral("DMR embedded GPS");
        case DSD_POS_LIP_USBD: return QStringLiteral("LIP (USBD)");
        case DSD_POS_LIP: return QStringLiteral("LIP");
        case DSD_POS_NMEA: return QStringLiteral("NMEA");
        case DSD_POS_P25_APX: return QStringLiteral("Motorola APX GPS");
        case DSD_POS_P25_HARRIS: return QStringLiteral("L3Harris GPS");
        case DSD_POS_NXDN_GPS: return QStringLiteral("NXDN GPS");
        case DSD_POS_DSTAR_DPRS: return QStringLiteral("D-STAR DPRS");
        case DSD_POS_M17_GNSS: return QStringLiteral("M17 GNSS");
        case DSD_POS_TETRA_LIP: return QStringLiteral("TETRA LIP");
        default: return QString();
    }
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_POSITION_KIND_TEXT_H_ */
