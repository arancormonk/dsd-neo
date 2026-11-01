// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_UI_QT_STORED_INT_H_
#define DSD_NEO_SRC_UI_QT_STORED_INT_H_

#include <QVariant>
#include <QtGlobal>
#include <limits>

namespace dsd_qt {

/*
 * Whole numbers read from stored or restored data: preferences, saved systems,
 * imported files and decryption profiles. A backup restore keeps a large JSON
 * number as a 64-bit integer, and a settings file hands one back as text.
 * QVariant::toInt() and toUInt() take the low 32 bits of either, or of a
 * negative value, and still report success, so 4294969296 reads as 2000. These
 * read the value wide and narrow it only when it fits: a value past the type's
 * range is not a number of that type, and the caller treats it as it treats
 * text that is not a number.
 */

/** @brief Whether @p raw holds an int, stored in @p value when it does. */
inline bool
stored_int(const QVariant& raw, int* value) {
    bool ok = false;
    const qlonglong wide = raw.toLongLong(&ok);
    if (!ok || wide < std::numeric_limits<int>::min() || wide > std::numeric_limits<int>::max()) {
        return false;
    }
    *value = static_cast<int>(wide);
    return true;
}

/** @brief The int @p raw holds, else @p fallback; text that is not a number included. */
inline int
stored_int_or(const QVariant& raw, int fallback) {
    int value = fallback;
    return stored_int(raw, &value) ? value : fallback;
}

/** @brief Whether @p raw holds a 32-bit unsigned value, stored in @p value when it does; a negative one does not. */
inline bool
stored_uint32(const QVariant& raw, quint32* value) {
    bool ok = false;
    const qlonglong wide = raw.toLongLong(&ok);
    if (!ok || wide < 0 || wide > static_cast<qlonglong>(std::numeric_limits<quint32>::max())) {
        return false;
    }
    *value = static_cast<quint32>(wide);
    return true;
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_STORED_INT_H_ */
