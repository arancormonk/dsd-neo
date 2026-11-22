// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The decoder's command line read as the decoder reads it: its long options first, then getopt's short ones.
 *
 * Header-only, so the saved systems (which do not link the session builder) judge a system's options by the same
 * reading the session builder starts it with.
 */

#ifndef DSD_NEO_SRC_UI_QT_CLI_OPTION_TOKENS_H_
#define DSD_NEO_SRC_UI_QT_CLI_OPTION_TOKENS_H_

#include <QByteArray>
#include <QChar>
#include <QLatin1String>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <Qt>
#include <QtGlobal>

#include <dsd-neo/runtime/cli.h>

namespace dsd_qt {

/* Whether the short option @p letter takes a value, by the parser's own option string. */
inline bool
short_takes_value(QChar letter) {
    return letter.unicode() < 0x80 && dsd_cli_short_option_takes_value(static_cast<char>(letter.unicode())) != 0;
}

/* Whether getopt knows the short option @p letter, by the parser's own option string (its ':'s are no option). */
inline bool
short_known(QChar letter) {
    return letter != QLatin1Char(':') && QLatin1String(dsd_cli_short_options()).contains(letter);
}

/* option_tokens()' first pass: the long options in @p split go to @p options, each with the value it takes stepped
 * over; @return what is left, from the first "--" on, for getopt. */
inline QStringList
long_option_tokens(const QStringList& split, QStringList* options) {
    QStringList rest;
    for (qsizetype i = 0; i < split.size(); ++i) {
        const QString& token = split.at(i);
        if (token == QLatin1String("--")) {
            rest.append(split.mid(i));
            break;
        }
        const dsd_cli_long_arity arity = dsd_cli_long_option_arity(token.toUtf8().constData());
        if (arity == DSD_CLI_LONG_UNKNOWN) {
            rest.append(token);
            continue;
        }
        options->append(token);
        const bool hasNext = i + 1 < split.size();
        const bool takesNext =
            arity == DSD_CLI_LONG_NEXT
            || (arity == DSD_CLI_LONG_NEXT_NON_OPTION && hasNext && !split.at(i + 1).startsWith(QLatin1Char('-')));
        if (takesNext && hasNext) {
            ++i;
        }
    }
    return rest;
}

/* option_tokens()' second pass: getopt's reading of @p rest, its options appended to @p options, and with @p values the
   values getopt hands option @p wanted, in order. */
inline void
short_option_tokens(const QStringList& rest, QStringList* options, QChar wanted = QChar(),
                    QStringList* values = nullptr) {
    for (qsizetype i = 0; i < rest.size(); ++i) {
        const QString& token = rest.at(i);
        if (token == QLatin1String("--")) {
            break;
        }
        if (token.size() < 2 || !token.startsWith(QLatin1Char('-'))) {
            continue;
        }
        if (token.startsWith(QLatin1String("--"))) {
            options->append(token);
            break;
        }
        for (qsizetype at = 1; at < token.size(); ++at) {
            const QChar letter = token.at(at);
            if (!short_known(letter)) {
                return;
            }
            options->append(QLatin1Char('-') + letter);
            if (short_takes_value(letter)) {
                const bool separate = at + 1 == token.size();
                if (values != nullptr && letter == wanted && (!separate || i + 1 < rest.size())) {
                    values->append(separate ? rest.at(i + 1) : token.mid(at + 1));
                }
                if (separate) {
                    ++i;
                }
                break;
            }
        }
    }
}

/**
 * @brief The options the parser reads in @p split, found as the decoder finds them, in its two passes.
 *
 * First its long options come out of the whole list wherever they stand, a short option's value position included,
 * each with the value its arity takes (dsd_cli_long_option_arity()), up to the first "--" that is no long option's
 * value, as dsd_parse_args()' pre-scan and dsd_cli_compact_args() take them; that "--" and everything after it stay, as
 * the compaction keeps them. Then getopt reads what is left, by its own rules: an element starting with '-' is a group
 * of short options, and the first of them that takes a value takes the rest of the element, or the next element
 * whatever it is ("--" included); a "--" that is no value ends the options; an operand is stepped over (glibc's getopt
 * permutes past it; one that stops there reads fewer options, never more); an unknown option ends the parse with an
 * error, and nothing after it is read. A long option the first pass does not know reaches getopt as option '-', which
 * is unknown: it is returned as typed, so its name can still be refused, and ends the parse.
 *
 * Long options are returned as typed, short ones as "-" and the letter.
 */
inline QStringList
option_tokens(const QStringList& split) {
    QStringList options;
    const QStringList rest = long_option_tokens(split, &options);
    short_option_tokens(rest, &options);
    return options;
}

/** @brief option_tokens() of @p tokens split on whitespace, as the session builder splits the extra options. */
inline QStringList
option_tokens(const QString& tokens) {
    return option_tokens(tokens.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts));
}

/** @brief The values the decoder hands short option @p wanted in @p split, in order (option_tokens()' reading). */
inline QStringList
short_option_values(const QStringList& split, QChar wanted) {
    QStringList options;
    QStringList values;
    short_option_tokens(long_option_tokens(split, &options), &options, wanted, &values);
    return values;
}

/** @brief Whether @p token is the long option @p name, alone or in its "=value" form. */
inline bool
long_option_is(const QString& token, QLatin1String name) {
    return token == name
           || (token.startsWith(name) && token.size() > name.size() && token.at(name.size()) == QLatin1Char('='));
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CLI_OPTION_TOKENS_H_ */
