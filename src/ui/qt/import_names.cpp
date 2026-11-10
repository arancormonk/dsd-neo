// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "import_names.h"

#include <QByteArray>
#include <QChar>
#include <QFileInfo>
#include <QLatin1String>
#include <QtGlobal>

namespace dsd_qt {

namespace {

constexpr int kMaxNameBytes = 247; // 255 minus room for " (999)" and then some
constexpr int kMaxSuffixBytes = 32;

/** @brief Longest prefix of @p text whose UTF-8 form fits @p maxBytes, never splitting a code point. */
QString
cut_to_utf8_bytes(const QString& text, int maxBytes) {
    QString out;
    int used = 0;
    for (qsizetype i = 0; i < text.size();) {
        const bool pair = text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate();
        const qsizetype len = pair ? 2 : 1;
        const QString piece = text.mid(i, len);
        const int bytes = piece.toUtf8().size();
        if (used + bytes > maxBytes) {
            break;
        }
        used += bytes;
        out += piece;
        i += len;
    }
    return out;
}

QString
strip_trailing_dots_and_spaces(QString name) {
    while (!name.isEmpty() && (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))) {
        name.chop(1);
    }
    return name;
}

/** @brief @p name with every C0 control character (U+0000 to U+001F) replaced by '_'. */
QString
replace_control_characters(QString name) {
    for (QChar& c : name) {
        if (c.unicode() < 0x20) {
            c = QLatin1Char('_');
        }
    }
    return name;
}

} // namespace

QString
sanitized_import_name(const QString& providerName, const QString& hint) {
    // First: an embedded NUL would cut the name where a C-string call (the publishing rename) reads it, so the stored
    // file and the name recorded for it would differ. The hint is a name, taken as given: an escape in it is part of
    // the name (issue #830), and a caller holding only a reference decodes its last segment itself.
    QString name = replace_control_characters(providerName.trimmed().isEmpty() ? hint : providerName);
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const qsizetype slash = name.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) {
        name = name.mid(slash + 1);
    }
    name = strip_trailing_dots_and_spaces(name.trimmed());
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return QStringLiteral("import.bin");
    }

    if (name.toUtf8().size() > kMaxNameBytes) {
        QString suffix = QFileInfo(name).suffix();
        if (suffix.toUtf8().size() > kMaxSuffixBytes) {
            suffix.clear();
        }
        const QString dotSuffix = suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix;
        const QString base = name.left(name.size() - dotSuffix.size());
        name = strip_trailing_dots_and_spaces(cut_to_utf8_bytes(base, kMaxNameBytes - dotSuffix.toUtf8().size()));
        if (name.isEmpty()) {
            name = QStringLiteral("import");
        }
        name += dotSuffix;
    }
    return name;
}

QString
unique_import_destination(const QDir& dir, const QString& fileName, const QSet<QString>& reserved) {
    const auto taken = [&](const QString& candidate) {
        const QFileInfo info(dir.filePath(candidate));
        return info.exists() || info.isSymLink() || reserved.contains(dir.absoluteFilePath(candidate));
    };
    if (!taken(fileName)) {
        return dir.filePath(fileName);
    }
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    for (int i = 2; i <= 999; i++) {
        const QString candidate = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(i)
                                                   : QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(suffix);
        if (!taken(candidate)) {
            return dir.filePath(candidate);
        }
    }
    return QString();
}

} // namespace dsd_qt
