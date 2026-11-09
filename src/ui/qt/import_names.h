// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The one rule both platforms use to name a stored import and to find a free name for it.
 */

#ifndef DSD_NEO_SRC_UI_QT_IMPORT_NAMES_H_
#define DSD_NEO_SRC_UI_QT_IMPORT_NAMES_H_

#include <QDir>
#include <QSet>
#include <QString>

namespace dsd_qt {

/**
 * @brief The file name an import is stored under.
 *
 * Takes @p providerName (the provider's display name or the source file name) when it is not
 * blank, else the percent-decoded @p hint (a picker URL's last segment). Replaces every C0
 * control character (U+0000 included) with '_' first, then keeps the last `/`-component, trims it, maps empty, "." and ".." to "import.bin", strips trailing dots and
 * spaces (Windows), and cuts the base at a code-point boundary so the UTF-8 name is at most 247
 * bytes: 8 bytes stay free for a later " (999)". The extension is kept.
 */
QString sanitized_import_name(const QString& providerName, const QString& hint);

/**
 * @brief chan.csv, chan (2).csv … chan (999).csv: the first path in @p dir that is free.
 *
 * A name is taken when any entry of any type exists there (a dangling symlink counts) or its
 * absolute path is in @p reserved. Empty when none is free.
 */
QString unique_import_destination(const QDir& dir, const QString& fileName, const QSet<QString>& reserved);

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_IMPORT_NAMES_H_
