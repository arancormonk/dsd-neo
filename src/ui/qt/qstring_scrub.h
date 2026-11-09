// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Overwrite a credential held in a QString before dropping it.
 */

#ifndef DSD_NEO_SRC_UI_QT_QSTRING_SCRUB_H_
#define DSD_NEO_SRC_UI_QT_QSTRING_SCRUB_H_

#include <QLatin1Char>
#include <QString>

namespace dsd_qt {

/**
 * @brief Overwrite a QString's characters, then drop it.
 *
 * QString::clear() only drops a reference; fill() writes over the characters
 * first. The runtime's own rr_secure_zero is static to the client, so this is
 * the Qt-side equivalent.
 *
 * Best effort by construction: fill() resizes, which detaches first, so a string
 * that is still implicitly shared - which a value handed over from QML or
 * copied from another holder is - leaves the original buffer alone and zeroes
 * the fresh copy instead. There is no way to reach the shared buffer that does
 * not also corrupt the other holder, so anything that must actually be erased
 * has to be a buffer the caller owns outright (a QByteArray or a native buffer
 * cleared with DSD_SECURE_ZERO).
 */
inline void
scrub_qstring(QString& text) {
    text.fill(QChar(u'\0'));
    text.clear();
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_QSTRING_SCRUB_H_ */
