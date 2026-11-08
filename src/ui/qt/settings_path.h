// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The file an organization/application QSettings opens, found without opening it (issue #828).
 */

#ifndef DSD_NEO_SRC_UI_QT_SETTINGS_PATH_H_
#define DSD_NEO_SRC_UI_QT_SETTINGS_PATH_H_

#include <QChar>
#include <QSettings>
#include <QString>
#include <QtGlobal>

namespace dsd_qt {

/**
 * @brief The file QSettings(@p format, UserScope, @p organization, @p application) opens, without opening it.
 *
 * Qt names it "<user path><organization>/<application><extension>", and the organization's own fallback file
 * "<user path><organization><extension>", which is the only file an organization-only QSettings opens. Opening the
 * settings file itself would read it unchecked (and wait forever on a FIFO), and Qt would keep that read for the next
 * QSettings of the file. Follows QSettings::setPath(). Empty without both names.
 */
inline QString
settings_file_path(QSettings::Format format, const QString& organization, const QString& application) {
    if (organization.isEmpty() || application.isEmpty()) {
        return QString();
    }
    const QString orgFile = QSettings(format, QSettings::UserScope, organization).fileName();
    const qsizetype at = orgFile.lastIndexOf(organization);
    if (at < 0) {
        return QString();
    }
    return orgFile.left(at) + organization + QLatin1Char('/') + application + orgFile.mid(at + organization.size());
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SETTINGS_PATH_H_ */
