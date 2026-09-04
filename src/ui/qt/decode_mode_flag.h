// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The decode-chip flag to preset-enum mapping, shared with the QML tests.
 *
 * Header-only and free of every Qt UI type but QString, so the QML test can
 * compile the real thing rather than keep a second copy of it. A copy is what
 * this replaces: the test double used to answer with numbers that were not the
 * enum's, which made the case asserting "the DMR chip sends DMR" pass on a value
 * DSDCFG_MODE_DMR has never had.
 */

#ifndef DSD_NEO_SRC_UI_QT_DECODE_MODE_FLAG_H_
#define DSD_NEO_SRC_UI_QT_DECODE_MODE_FLAG_H_

#include <QChar>
#include <QLatin1String>
#include <QList>
#include <QString>
#include <QStringList>
#include <Qt>

#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>

namespace dsd_qt {

/**
 * @brief The decode preset a chip's CLI flag selects.
 *
 * The chip's flag is the CLI's own text, so the mapping is the CLI's own function
 * rather than a second table here that could drift from it. A chip may carry
 * several flags ("-f1 -mq"); the decode one is whichever is -f.
 *
 * @param flag The chip's flag text; empty means the engine's own default.
 * @return A `dsdneoUserDecodeMode` value, or -1 when the flag selects no preset.
 */
inline int
decode_mode_for_flag(const QString& flag) {
    const QStringList tokens = flag.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (!token.startsWith(QLatin1String("-f")) || token.size() != 3) {
            continue;
        }
        dsdneoUserDecodeMode mode = DSDCFG_MODE_UNSET;
        if (dsd_decode_mode_from_cli_preset(token.at(2).toLatin1(), &mode) == 0) {
            return static_cast<int>(mode);
        }
    }
    /* An empty flag is the engine's own default, which is the auto preset. */
    return flag.trimmed().isEmpty() ? static_cast<int>(DSDCFG_MODE_AUTO) : -1;
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DECODE_MODE_FLAG_H_ */
