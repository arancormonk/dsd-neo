// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A Qt path in the bytes the native dsd_* path APIs read it as (issue #828).
 */

#ifndef DSD_NEO_SRC_UI_QT_PLATFORM_PATH_H_
#define DSD_NEO_SRC_UI_QT_PLATFORM_PATH_H_

#include <QByteArray>
#include <QFile>
#include <QString>

namespace dsd_qt {

/**
 * @brief @p path as the platform layer (<dsd-neo/platform/file_compat.h>) and the decoder read a path.
 *
 * On Windows that is UTF-8: the platform layer takes a UTF-8 path through the wide API, and the decoder's own paths
 * (recording folders, journals) are UTF-8. QFile::encodeName() would give the ANSI code page there, which names another
 * file, or none, for a name outside it. Elsewhere it is the local 8-bit encoding QFile itself uses (UTF-8 on Android and
 * in a UTF-8 locale).
 */
inline QByteArray
platform_path(const QString& path) {
#ifdef Q_OS_WIN
    return path.toUtf8();
#else
    return QFile::encodeName(path);
#endif
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_PLATFORM_PATH_H_ */
