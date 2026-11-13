// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief text_order: the ICU-free fold and natural name order the saved-systems and RadioReference lists use.
 */

#include <QString>
#include <QStringList>
#include <cstdio>
#include <cstdlib>

#include "text_order.h"

namespace {

void
expect(bool condition, int line) {
    if (!condition) {
        (void)std::fprintf(stderr, "text order assertion failed at line %d\n", line);
        std::abort();
    }
}

#define check(condition) expect(!!(condition), __LINE__)

using dsd_qt::text_compare_names;
using dsd_qt::text_fold;
using dsd_qt::text_fold_contains;

QString
s(const char16_t* text) {
    return QString(QStringView(text));
}

void
test_fold() {
    check(text_fold(s(u"Jos\u00E9")) == QStringLiteral("jose"));
    check(text_fold(s(u"S\u00C3O PAULO")) == QStringLiteral("sao paulo"));
    check(text_fold(s(u"\uFB01re")) == QStringLiteral("fire"));
    check(text_fold(s(u"\uFF11\uFF12\uFF13")) == QStringLiteral("123"));
    check(text_fold(QString()).isEmpty());
    /* A non-spacing mark outside the BMP (U+1D165 is spacing; U+1E8D0 Mende Kikakui is non-spacing) is dropped. */
    check(text_fold(s(u"a\U0001E8D0b")) == QStringLiteral("ab"));
}

void
test_contains() {
    check(text_fold_contains(text_fold(s(u"S\u00E3o Paulo")), text_fold(QStringLiteral("sao"))));
    check(text_fold_contains(text_fold(s(u"\u00C9b\u00E8ne")), text_fold(QStringLiteral("eben"))));
    check(text_fold_contains(QStringLiteral("abc"), QString()));
    check(!text_fold_contains(QStringLiteral("abc"), QStringLiteral("abd")));
}

void
test_order() {
    check(text_compare_names(QStringLiteral("Site 2"), QStringLiteral("Site 10")) < 0);
    /* Case-only differences: folded tie, then the raw string, deterministic and nonzero. */
    check(text_compare_names(QStringLiteral("site 2"), QStringLiteral("Site 2")) != 0);
    check(text_compare_names(QStringLiteral("site 2"), QStringLiteral("Site 2"))
          == -text_compare_names(QStringLiteral("Site 2"), QStringLiteral("site 2")));
    check(text_compare_names(QStringLiteral("apple"), QStringLiteral("Banana")) < 0);
    /* Folded-equal, so the raw string decides: "E" (0x45) sorts before U+00C9. */
    check(text_compare_names(s(u"\u00C9clair"), QStringLiteral("Eclair")) > 0);
    check(text_compare_names(QStringLiteral("Eclair"), s(u"\u00C9clair")) < 0);
    check(text_compare_names(s(u"\u00C9clair"), QStringLiteral("Eclairs")) < 0);
    /* Equal value: shorter run first, and never equal. */
    check(text_compare_names(QStringLiteral("007"), QStringLiteral("7")) > 0);
    check(text_compare_names(QStringLiteral("7"), QStringLiteral("007")) < 0);
    check(text_compare_names(QStringLiteral("a007b"), QStringLiteral("a7c")) < 0);
    /* Leading space and punctuation are skipped; articles are kept. */
    check(text_compare_names(QStringLiteral("  Alpha"), QStringLiteral("Beta")) < 0);
    check(text_compare_names(QStringLiteral("- Zulu"), QStringLiteral("Alpha")) > 0);
    check(text_compare_names(QStringLiteral("The Woodlands"), QStringLiteral("Texas")) > 0);
    /* Documented limit: no decomposition for the sharp s. */
    check(text_compare_names(s(u"\u00DF"), QStringLiteral("ss")) != 0);
    check(text_compare_names(QStringLiteral("same"), QStringLiteral("same")) == 0);
    /* The pre-folded form answers as text_compare_names() does. */
    const QStringList names{s(u"\u00C9clair"),         QStringLiteral("Eclair"), QStringLiteral("Site 2"),
                            QStringLiteral("site 10"), QStringLiteral("007"),    QStringLiteral("7")};
    for (const QString& a : names) {
        for (const QString& b : names) {
            check(dsd_qt::text_compare_folded_names(text_fold(a), a, text_fold(b), b) == text_compare_names(a, b));
        }
    }
    check(text_compare_names(QString(), QStringLiteral("a")) < 0);
    check(text_compare_names(QStringLiteral("alpha 2"), QStringLiteral("alpha 10")) < 0);
}

void
test_antisymmetry() {
    const QStringList corpus = {
        QStringLiteral("Alpha"),
        QStringLiteral("alpha"),
        QStringLiteral("alpha 2"),
        QStringLiteral("alpha 10"),
        QStringLiteral("alpha 02"),
        QStringLiteral("Beta"),
        QStringLiteral("beta"),
        s(u"\u00C9bene"),
        s(u"Ebene"),
        s(u"ebene"),
        QStringLiteral("007"),
        QStringLiteral("7"),
        QStringLiteral("07"),
        QStringLiteral("Site 2"),
        QStringLiteral("Site 10"),
        QStringLiteral("site 2"),
        QStringLiteral("  Alpha"),
        QStringLiteral("-Alpha"),
        QStringLiteral("The Woods"),
        QStringLiteral("Texas"),
        s(u"S\u00E3o Paulo"),
        s(u"Sao Paulo"),
        s(u"\u00DF"),
        QStringLiteral("ss"),
        QStringLiteral("a1b2"),
        QStringLiteral("a01b2"),
        QStringLiteral("a1b02"),
        QStringLiteral("a1"),
        QStringLiteral("a"),
        QStringLiteral("1"),
        QStringLiteral("10"),
        QStringLiteral("2"),
        QString(),
        QStringLiteral(" "),
        s(u"\uFB01re"),
        QStringLiteral("fire"),
        QStringLiteral("Zed"),
        QStringLiteral("zed 1"),
        QStringLiteral("zed 01"),
        QStringLiteral("zed 1a"),
    };
    check(corpus.size() == 40);
    for (const QString& x : corpus) {
        for (const QString& y : corpus) {
            const int xy = text_compare_names(x, y);
            const int yx = text_compare_names(y, x);
            check((xy < 0) == (yx > 0));
            check((xy == 0) == (yx == 0));
            check((xy == 0) == (x == y));
            for (const QString& z : corpus) {
                if (xy <= 0 && text_compare_names(y, z) <= 0) {
                    check(text_compare_names(x, z) <= 0);
                }
            }
        }
    }
}

/* Digit runs in any script sort before every other character, so the order stays a strict weak order (Task 8 feeds
   it to std::sort) across Arabic-Indic digits, ASCII digits and letters of several scripts. */
void
test_transitive_across_scripts() {
    const QStringList corpus = {
        s(u"\u0662"),
        s(u"\u0663"),
        QStringLiteral("2"),
        QStringLiteral("3"),
        QStringLiteral("10"),
        s(u"\u0661\u0660"),
        s(u"\u0662a"),
        QStringLiteral("2a"),
        QStringLiteral("a"),
        QStringLiteral("z"),
        s(u"\u0430"),
        s(u"\u05D0"),
        s(u"\u0627"),
        s(u"\u4E2D"),
        s(u"\u0627 2"),
        s(u"\u0662 \u0627"),
        QStringLiteral("a2"),
        s(u"\u0430\u0662"),
        QStringLiteral("2 \u0430"),
        QString(),
    };
    check(text_compare_names(QStringLiteral("5"), QStringLiteral("a")) < 0);
    check(text_compare_names(s(u"\u0665"), QStringLiteral("a")) < 0);
    check(text_compare_names(s(u"\u0665"), s(u"\u0430")) < 0);
    check(text_compare_names(s(u"\u0665"), s(u"\u4E2D")) < 0);
    for (const QString& x : corpus) {
        for (const QString& y : corpus) {
            const int xy = text_compare_names(x, y);
            check((xy < 0) == (text_compare_names(y, x) > 0));
            for (const QString& z : corpus) {
                if (xy <= 0 && text_compare_names(y, z) <= 0) {
                    check(text_compare_names(x, z) <= 0);
                }
                if (xy < 0 && text_compare_names(y, z) <= 0) {
                    check(text_compare_names(x, z) < 0);
                }
            }
        }
    }
}

} // namespace

int
main() {
    test_fold();
    test_contains();
    test_order();
    test_antisymmetry();
    test_transitive_across_scripts();
    return 0;
}
