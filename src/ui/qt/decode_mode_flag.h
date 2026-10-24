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
#include <algorithm>

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
    /* An empty flag is the engine's own default, the Auto preset (-fa: every digital mode except TETRA, NXDN
       included). */
    return flag.trimmed().isEmpty() ? static_cast<int>(DSDCFG_MODE_AUTO) : -1;
}

/**
 * @brief Whether @p token is one the app itself writes into a saved decode flag.
 *
 * The allowlist is what the producers write, read from their code:
 * - the wizard's decode chips (qml/Util.js DECODE_MODES): -ft -mq -fs -fi -fn
 *   -fd -fy -fz -fT -fA -fM; the older saves it still labels (LEGACY_DECODE_LABELS)
 *   and the "-f1 -mq" chip saved_systems_model.cpp migrates on load: -f1;
 * - a scan list's frequency entries (scan_list_targets.cpp resolveSystem()):
 *   -ft -fs -fi -fn -fA -fM;
 * - signal search's detections (signal_search_model.cpp confirmedDecodeFlag()
 *   and systemForHit()): -ft -fs -fa -fd -fi -fn -fm -fz -fy -fp -fA -fM;
 * - the RadioReference generator (src/runtime/radioreference/rr_generate.c
 *   k_protocols, through dsd_rr_decode_flag() into radio_reference_model.cpp's
 *   plan): -ft -mq -^ -fs -fi -fn -Y and the EDACS selectors -fh -fH -fe -fE.
 * No producer writes any other -f selector (-fx, -f2, ...), so those count.
 */
inline bool
decode_flag_token_is_standard(const QString& token) {
    static const char* const kProducedTokens[] = {"-fa", "-fA", "-fM", "-fd", "-ft", "-f1", "-fs",
                                                  "-fi", "-fn", "-fy", "-fm", "-fz", "-fT", "-fh",
                                                  "-fH", "-fe", "-fE", "-fp", "-mq", "-^",  "-Y"};
    for (const char* produced : kProducedTokens) {
        if (token == QLatin1String(produced)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Whether every token of @p flag is one the app itself writes.
 *
 * The session builder splits a saved decode flag into argv like the extra
 * arguments, so a token no producer writes (decode_flag_token_is_standard())
 * is a decoder option of its own: an output file, a socket, an unknown switch.
 * An empty flag (the engine's default) is standard.
 */
inline bool
decode_flag_is_standard(const QString& flag) {
    const QStringList tokens = flag.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return std::all_of(tokens.cbegin(), tokens.cend(),
                       [](const QString& token) { return decode_flag_token_is_standard(token); });
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DECODE_MODE_FLAG_H_ */
