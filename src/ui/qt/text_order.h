// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Name ordering and accent-blind matching that does not depend on ICU.
 *
 * Qt for Android is built without ICU, so QCollator and QML localeCompare fall back to raw code-point order there:
 * "alpha 10" sorts before "alpha 2" and "Zed" before "apple". These helpers give the same answer on every platform.
 *
 * fold() is NFKD, then non-spacing marks dropped, then case folded, so "SÃO PAULO" folds to "sao paulo" and a
 * ligature or full-width digit folds to its plain letters. compareNames() orders folded text with digit runs by value
 * (leading zeros stripped, then length, then digits), everything else by code point, leading punctuation and spaces
 * skipped and articles kept; names that fold alike are ordered by shorter digit run, then by the raw string, so the
 * order is total and deterministic.
 *
 * Documented limits: letters that have no canonical decomposition (ß, Ø, Æ, Ł) are not expanded, so "ß" does not
 * match "ss".
 */

#ifndef DSD_NEO_SRC_UI_QT_TEXT_ORDER_H_
#define DSD_NEO_SRC_UI_QT_TEXT_ORDER_H_

#include <QObject>
#include <QString>

namespace dsd_qt {

/** @brief NFKD, non-spacing marks dropped, case folded. */
QString text_fold(const QString& s);

/** @brief Natural, accent- and case-blind order of two names; <0, 0 (only for identical strings) or >0. */
int text_compare_names(const QString& a, const QString& b);

/** @brief Whether an already folded haystack contains an already folded needle (an empty needle matches). */
bool text_fold_contains(const QString& foldedHaystack, const QString& foldedNeedle);

/** @brief The `textOrder` context object: the helpers above for QML. */
class TextOrder : public QObject {
    Q_OBJECT

  public:
    explicit TextOrder(QObject* parent = nullptr);

    Q_INVOKABLE QString fold(const QString& s) const;
    Q_INVOKABLE int compare(const QString& a, const QString& b) const;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_TEXT_ORDER_H_
