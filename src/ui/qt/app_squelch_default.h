// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The app-wide default squelch's value and grammar (issue #899).
 *
 * Header-only so the preferences, the session builder and the policy that follows decode switches share one reading
 * of it without linking each other.
 */

#ifndef DSD_NEO_SRC_UI_QT_APP_SQUELCH_DEFAULT_H_
#define DSD_NEO_SRC_UI_QT_APP_SQUELCH_DEFAULT_H_

#include <QLatin1String>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QString>
#include <dsd-neo/core/power.h>

namespace dsd_qt {

/** @brief The squelch analog sessions start with when nothing else names one (issue #899, D1). */
inline constexpr char kAppSquelchDefault[] = "noise+10";

/**
 * @brief Whether @p setting is a squelch the app-wide default can hold (issue #899): exactly "off", "auto+N" or
 * "noise+N", N a whole number of dB from DSD_SQUELCH_MARGIN_MIN_DB to DSD_SQUELCH_MARGIN_MAX_DB written without a
 * sign or leading zero. A fixed level is never one: levels stay a session's or a saved system's.
 */
inline bool
app_squelch_default_valid(const QString& setting) {
    if (setting == QLatin1String("off")) {
        return true;
    }
    static const QRegularExpression dynamic(QStringLiteral("^(auto|noise)\\+([1-9][0-9]?)$"));
    const QRegularExpressionMatch match = dynamic.match(setting);
    if (!match.hasMatch()) {
        return false;
    }
    const int margin = match.captured(2).toInt();
    return margin >= DSD_SQUELCH_MARGIN_MIN_DB && margin <= DSD_SQUELCH_MARGIN_MAX_DB;
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_APP_SQUELCH_DEFAULT_H_ */
