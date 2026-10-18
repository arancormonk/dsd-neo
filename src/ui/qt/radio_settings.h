// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_UI_QT_RADIO_SETTINGS_H_
#define DSD_NEO_SRC_UI_QT_RADIO_SETTINGS_H_

#include <QByteArray>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/squelch.h>
#include "decode_mode_flag.h"

namespace dsd_qt {

inline bool
radio_squelch_valid(const QString& text) {
    // Also a single safe token in a generated scan CSV's options cell.
    static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9.+-]+$"));
    const QByteArray bytes = text.toUtf8();
    dsd_squelch_setting parsed{};
    return token.match(text).hasMatch() && dsd_squelch_setting_parse(bytes.constData(), &parsed, nullptr, 0) == 0;
}

/** Only typed Radio controls belong here; never interpret this map as arbitrary CLI options. */
inline bool
radio_settings_valid(const QVariant& value) {
    if (!value.isValid()) {
        return true;
    }
    if (value.typeId() != QMetaType::QVariantMap) {
        return false;
    }
    const auto settings = value.toMap();
    for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
        const QString text = it.value().toString();
        if (it.key() == QLatin1String("decodeFlag")) {
            if (!text.isEmpty()
                && (text.size() != 3 || !text.startsWith(QLatin1String("-f")) || decode_mode_for_flag(text) < 0)) {
                return false;
            }
        } else if (it.key() == QLatin1String("modulation")) {
            if (text != QLatin1String("0") && text != QLatin1String("1") && text != QLatin1String("2")) {
                return false;
            }
        } else if (it.key() == QLatin1String("squelch")) {
            if (!radio_squelch_valid(text)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

/** Append after advanced options: a confirmed live choice is the latest explicit setting. */
inline void
radio_settings_append(QStringList& args, const QVariantMap& settings) {
    if (settings.contains(QStringLiteral("decodeFlag"))) {
        // Live preset changes clear an earlier manual modulation lock. Do the
        // same before applying the preset, especially Auto's modulation hunt.
        const QString flag = settings.value(QStringLiteral("decodeFlag")).toString();
        args << QStringLiteral("-ma") << (flag.isEmpty() ? QStringLiteral("-fa") : flag);
    }
    if (settings.contains(QStringLiteral("modulation"))) {
        const QStringList flags{QStringLiteral("-mc"), QStringLiteral("-mq"), QStringLiteral("-mg")};
        const int modulation = settings.value(QStringLiteral("modulation")).toInt();
        if (modulation >= 0 && modulation < flags.size()) {
            args << flags.at(modulation);
        }
    }
    if (settings.contains(QStringLiteral("squelch"))) {
        args << QStringLiteral("--squelch") << settings.value(QStringLiteral("squelch")).toString();
    }
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RADIO_SETTINGS_H_ */
