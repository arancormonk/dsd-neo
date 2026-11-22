// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Discussion #900 F3: what a saved system's start options say about its DMR Tier III channels: whether the
 * session decodes DMR, and whether the user's own options name a channels file.
 *
 * Header-only and read the way the decoder reads its command line (cli_option_tokens.h), so the start
 * (session_args.cpp) and an edit kept on the system (saved_systems_model.cpp, which does not link the session
 * builder) judge a system alike.
 */

#ifndef DSD_NEO_SRC_UI_QT_DMR_CHANNELS_ARGS_H_
#define DSD_NEO_SRC_UI_QT_DMR_CHANNELS_ARGS_H_

#include <QChar>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <algorithm>

#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>

#include "cli_option_tokens.h"
#include "radio_settings.h"

namespace dsd_qt {

/**
 * @brief The options of a start of @p system that can set its decode set, in the order session_args_build() puts them
 * on the command line: the decode flag, the system's extra options, the app-wide ones (@p appExtra), then the
 * confirmed live radio settings (radio_settings_append()), the latest of all. What the start puts between them
 * (trunking, policy, hangtime and recording options) is whole options of the app's own, with no -f among them.
 */
inline QStringList
dmr_channels_decode_options(const QVariantMap& system, const QString& appExtra) {
    const QRegularExpression space(QStringLiteral("\\s+"));
    QStringList args = system.value(QStringLiteral("decodeFlag")).toString().split(space, Qt::SkipEmptyParts);
    args << system.value(QStringLiteral("extraArgs")).toString().split(space, Qt::SkipEmptyParts);
    args << appExtra.split(space, Qt::SkipEmptyParts);
    radio_settings_append(args, system.value(QStringLiteral("radioSettings")).toMap());
    return args;
}

/**
 * @brief Whether a start of @p system decodes DMR, with @p appExtra the app-wide extra options: the last -f the
 * decoder reads (getopt's "-fs" and "-f s" alike) among dmr_channels_decode_options(), as each -f sets the decode
 * set over the one before, selects a preset that frames DMR; or none does, which leaves the engine's default, Auto.
 *
 * The presets that frame DMR are the ones src/runtime/decode_mode.c sets frame_dmr for: Auto (-fa), DMR (-fs), DMR
 * mono and TDMA (-ft, P25 with DMR). The CLI takes -fr (the single-slot mono decoder) as the DMR preset, and reads only
 * the value's first character. A selector the CLI takes no preset from (EDACS, ProVoice, the M17 encoders, an unknown
 * one) frames no DMR.
 */
inline bool
dmr_channels_session_decodes_dmr(const QVariantMap& system, const QString& appExtra) {
    const QStringList values = short_option_values(dmr_channels_decode_options(system, appExtra), QLatin1Char('f'));
    if (values.isEmpty()) {
        return true;
    }
    const QString& last = values.last();
    if (last.isEmpty() || last.at(0).unicode() >= 0x80) {
        return false;
    }
    const char selector = static_cast<char>(last.at(0).unicode());
    dsdneoUserDecodeMode mode = DSDCFG_MODE_UNSET;
    if (dsd_decode_mode_from_cli_preset(selector == 'r' ? 's' : selector, &mode) != 0) {
        return false;
    }
    return mode == DSDCFG_MODE_AUTO || mode == DSDCFG_MODE_DMR || mode == DSDCFG_MODE_DMR_MONO
           || mode == DSDCFG_MODE_TDMA;
}

/**
 * @brief Whether the user's own options, @p systemExtra (the system's extra options) then @p appExtra (the app-wide
 * ones), name a --dmr-chan-overrides file the decoder reads. The decoder keeps only the last such option, and the
 * user's come after the one the start writes from the saved system's channels, so theirs is the one used.
 */
inline bool
dmr_channels_extra_names_overrides(const QString& systemExtra, const QString& appExtra) {
    const QStringList options = option_tokens(systemExtra + QLatin1Char(' ') + appExtra);
    return std::any_of(options.cbegin(), options.cend(), [](const QString& token) {
        return long_option_is(token, QLatin1String("--dmr-chan-overrides"));
    });
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DMR_CHANNELS_ARGS_H_ */
