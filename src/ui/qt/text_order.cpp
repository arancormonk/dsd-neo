// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "text_order.h"

#include <QChar>
#include <QList>

namespace dsd_qt {

namespace {

using Ucs4 = QList<uint>;

bool
is_digit(uint c) {
    return QChar::isDigit(c);
}

/* Length of the digit run that starts at `from`. */
qsizetype
digit_run_end(const Ucs4& s, qsizetype from) {
    qsizetype end = from;
    while (end < s.size() && is_digit(s[end])) {
        ++end;
    }
    return end;
}

/* Compares two digit runs by value: leading zeros stripped, then length, then digits. Runs of equal value compare 0. */
int
compare_digit_values(const Ucs4& a, qsizetype a_begin, qsizetype a_end, const Ucs4& b, qsizetype b_begin,
                     qsizetype b_end) {
    while (a_begin < a_end - 1 && QChar::digitValue(a[a_begin]) == 0) {
        ++a_begin;
    }
    while (b_begin < b_end - 1 && QChar::digitValue(b[b_begin]) == 0) {
        ++b_begin;
    }
    const qsizetype a_len = a_end - a_begin;
    const qsizetype b_len = b_end - b_begin;
    if (a_len != b_len) {
        return a_len < b_len ? -1 : 1;
    }
    for (qsizetype i = 0; i < a_len; ++i) {
        const int da = QChar::digitValue(a[a_begin + i]);
        const int db = QChar::digitValue(b[b_begin + i]);
        if (da != db) {
            return da < db ? -1 : 1;
        }
    }
    return 0;
}

/* Index of the first letter or number: leading punctuation and spaces do not take part in the order. */
qsizetype
skip_leading(const Ucs4& s) {
    qsizetype i = 0;
    while (i < s.size() && !QChar::isLetterOrNumber(s[i])) {
        ++i;
    }
    return i;
}

/* Folded natural compare. `length_tie` receives the first shorter-run-first difference between digit runs of equal
   value, which orders names that are otherwise alike. */
int
compare_folded(const QString& a, const QString& b, int& length_tie) {
    const Ucs4 ua = a.toUcs4();
    const Ucs4 ub = b.toUcs4();
    qsizetype i = skip_leading(ua);
    qsizetype j = skip_leading(ub);
    while (i < ua.size() && j < ub.size()) {
        if (is_digit(ua[i]) && is_digit(ub[j])) {
            const qsizetype ie = digit_run_end(ua, i);
            const qsizetype je = digit_run_end(ub, j);
            const int c = compare_digit_values(ua, i, ie, ub, j, je);
            if (c != 0) {
                return c;
            }
            if (length_tie == 0 && ie - i != je - j) {
                length_tie = (ie - i) < (je - j) ? -1 : 1;
            }
            i = ie;
            j = je;
            continue;
        }
        /* Any digit run sorts before any other character, whatever script the digits are in: comparing a digit's
           code point with a letter's is not transitive across scripts, and std::sort needs a strict weak order. */
        const bool a_digit = is_digit(ua[i]);
        if (a_digit != is_digit(ub[j])) {
            return a_digit ? -1 : 1;
        }
        if (ua[i] != ub[j]) {
            return ua[i] < ub[j] ? -1 : 1;
        }
        ++i;
        ++j;
    }
    const bool a_done = i >= ua.size();
    const bool b_done = j >= ub.size();
    if (a_done != b_done) {
        return a_done ? -1 : 1;
    }
    return 0;
}

} // namespace

QString
text_fold(const QString& s) {
    const QString decomposed = s.normalized(QString::NormalizationForm_KD);
    /* Code points, not UTF-16 units: a mark outside the BMP is a surrogate pair. */
    QList<char32_t> kept;
    for (const uint cp : decomposed.toUcs4()) {
        if (QChar::category(cp) != QChar::Mark_NonSpacing) {
            kept.append(static_cast<char32_t>(cp));
        }
    }
    const QString out = QString::fromUcs4(kept.constData(), kept.size());
    return out.toCaseFolded();
}

int
text_compare_names(const QString& a, const QString& b) {
    int length_tie = 0;
    const int folded = compare_folded(text_fold(a), text_fold(b), length_tie);
    if (folded != 0) {
        return folded;
    }
    if (length_tie != 0) {
        return length_tie;
    }
    const int raw = QString::compare(a, b);
    return raw < 0 ? -1 : (raw > 0 ? 1 : 0);
}

bool
text_fold_contains(const QString& foldedHaystack, const QString& foldedNeedle) {
    return foldedHaystack.contains(foldedNeedle);
}

TextOrder::TextOrder(QObject* parent) : QObject(parent) {}

QString
TextOrder::fold(const QString& s) const {
    return text_fold(s);
}

int
TextOrder::compare(const QString& a, const QString& b) const {
    return text_compare_names(a, b);
}

} // namespace dsd_qt
