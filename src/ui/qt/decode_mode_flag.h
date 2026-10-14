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

/**
 * @brief Whether @p flag is nothing but a decode preset the picker selects.
 *
 * The session builder splits a saved decode flag into argv like the extra
 * arguments, so a flag that is more than a preset carries decoder options of
 * its own. A preset is: empty (the engine's default), one `-f` selector that
 * decode_mode_for_flag() maps, or the P25 Simulcast chip's "-mq", the picker's
 * one preset that is not a `-f` selector. Anything else - a second token, an
 * unmapped selector - is more than a preset.
 */
inline bool
decode_flag_is_preset(const QString& flag) {
    const QStringList tokens = flag.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        return true;
    }
    if (tokens.size() != 1) {
        return false;
    }
    const QString& token = tokens.first();
    if (token == QLatin1String("-mq")) {
        return true;
    }
    return token.size() == 3 && token.startsWith(QLatin1String("-f")) && decode_mode_for_flag(token) >= 0;
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DECODE_MODE_FLAG_H_ */
