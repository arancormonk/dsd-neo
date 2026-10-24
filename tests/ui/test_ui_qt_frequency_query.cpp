// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the History search read as a frequency (frequency_query.h). The grammar
 * takes a decimal point or "MHz" and nothing looser, so a bare number stays an ID
 * search. A query finds a row when it begins the row's frequency in either spelling
 * the app prints, and the contract is checked against the real Util.js: whatever
 * Util.mhzText and Util.fmtMhz print for a frequency, and every shorter dotted prefix
 * of it, finds that frequency, as does every dotted prefix of the six-decimal text the
 * Site sheet prints. */

#include <QCoreApplication>
#include <QFile>
#include <QIODevice>
#include <QJSEngine>
#include <QJSValue>
#include <QLatin1String>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <limits>
#include <stdio.h>

#include "dsd-neo/core/safe_api.h"
#include "frequency_query.h"

using dsd_qt::frequency_query_matches;
using dsd_qt::frequency_query_parse;
using dsd_qt::FrequencyQuery;

namespace {

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

bool
finds(const char* query, qint64 hz) {
    return frequency_query_matches(frequency_query_parse(QLatin1String(query)), hz);
}

void
test_grammar(void) {
    struct Case {
        const char* text;
        qint64 mhz;
        const char* fraction;
    };

    const Case valid[] = {
        {"851.0125", 851, "0125"},
        {"851.", 851, ""},
        {"851.0125 MHz", 851, "0125"},
        {"851.0125MHz", 851, "0125"},
        {"851.0125 mhz", 851, "0125"},
        {"851 MHz", 851, ""},
        {"851MHZ", 851, ""},
        {" 851.0125\t", 851, "0125"},
        {"851.0125000", 851, "012500"},
        {"0.5", 0, "5"},
        {"99999.999999", 99999, "999999"},
    };
    for (const Case& c : valid) {
        const FrequencyQuery q = frequency_query_parse(QLatin1String(c.text));
        if (!q.valid || q.mhz != c.mhz || q.fraction != QLatin1String(c.fraction)) {
            DSD_FPRINTF(stderr, "FAIL: \"%s\" should parse as %lld.%s\n", c.text, static_cast<long long>(c.mhz),
                        c.fraction);
            ++g_failures;
        }
    }

    const char* invalid[] = {
        "851",    ".0125",    "851.0125.1",      "851.0125x", "851.0000001", "123456.0",     "",
        "   ",    "MHz",      "-851.0",          "+851.0",    "851,0125",    "851.0125 kHz", "851 . 0125",
        "8 51.0", "TG 851.0", "851.0125 MHz MHz"};
    for (const char* text : invalid) {
        if (frequency_query_parse(QLatin1String(text)).valid) {
            DSD_FPRINTF(stderr, "FAIL: \"%s\" should not parse as a frequency\n", text);
            ++g_failures;
        }
    }
    expect("a default query is invalid", !FrequencyQuery().valid);
}

void
test_matches(void) {
    expect("exact", finds("851.0125", 851012500));
    expect("the row's stripped trailing zero", finds("851.01250", 851012500));
    expect("with its unit", finds("851.0125 MHz", 851012500));
    expect("whole MHz with the point", finds("851.", 851012500));
    expect("whole MHz with the unit", finds("851 MHz", 851012500));
    expect("not the next MHz", !finds("850.", 851012500) && !finds("852.", 851012500));
    expect("not a neighbouring channel", !finds("851.0125", 851025000));
    expect("not a prefix in the middle", !finds("51.0125", 851012500));

    // Six decimals name the frequency to the Hz; the neighbour that rounds alike is not it.
    expect("six decimals, exact", finds("851.012500", 851012500));
    expect("six decimals, not the 1 Hz neighbour", !finds("851.012500", 851012501));
    expect("six decimals of an off-raster frequency", finds("851.012496", 851012496));
    expect("five decimals of an off-raster frequency, rounded", finds("851.0125", 851012496));
    expect("five decimals of an off-raster frequency, truncated", finds("851.01249", 851012496));

    // 5 Hz ties: toFixed rounds them either way, so both spellings find the row.
    for (qint64 hz : {155000005LL, 851012505LL, 851015625LL}) {
        const QString exact = QString::number(hz / 1000000) + QLatin1Char('.')
                              + QString::number(hz % 1000000).rightJustified(6, QLatin1Char('0'));
        const QString down = exact.left(exact.size() - 1);
        const qint64 tens = hz / 10 + 1;
        const QString up = QString::number(tens / 100000) + QLatin1Char('.')
                           + QString::number(tens % 100000).rightJustified(5, QLatin1Char('0'));
        expect("a tie rounded down", frequency_query_matches(frequency_query_parse(down), hz));
        expect("a tie rounded up", frequency_query_matches(frequency_query_parse(up), hz));
        expect("a tie to the Hz", frequency_query_matches(frequency_query_parse(exact), hz));
    }

    // A carry into the MHz digits: 851.999996 prints as 852.0000.
    expect("carry, rounded text", finds("852.0000", 851999996) && finds("852.", 851999996));
    expect("carry, with the unit", finds("852 MHz", 851999996));
    expect("carry, six decimals", finds("851.999996", 851999996) && finds("851.99999", 851999996));
    expect("a bare number is not a frequency query", !finds("852", 851999996) && !finds("851", 851012500));

    expect("unknown frequency", !finds("851.", 0) && !finds("0.", 0) && !finds("851.", -1));
    expect("no overflow at the top of qint64", !finds("851.", std::numeric_limits<qint64>::max())
                                                   && !finds("851.", std::numeric_limits<qint64>::max() - 4)
                                                   && !finds("99999.", std::numeric_limits<qint64>::max()));
    expect("an invalid query finds nothing", !frequency_query_matches(FrequencyQuery(), 851012500));
}

/** @brief Every prefix of @p text that holds the decimal point (so that it is a query). */
QList<QString>
dotted_prefixes(const QString& text) {
    QList<QString> prefixes;
    const qsizetype point = text.indexOf(QLatin1Char('.'));
    for (qsizetype n = point + 1; point >= 0 && n <= text.size(); ++n) {
        prefixes.append(text.left(n));
    }
    return prefixes;
}

/** @brief A deterministic spread of frequencies: rasters across the bands, 5 Hz ties, 1 Hz neighbours, carries. */
QList<qint64>
sweep(void) {
    QList<qint64> hz;
    for (qint64 base :
         {30000000LL, 136000000LL, 155000000LL, 450000000LL, 769000000LL, 851000000LL, 935000000LL, 1296000000LL}) {
        for (qint64 step : {3125LL, 6250LL, 12500LL, 25000LL}) {
            for (qint64 k = 0; k < 40; ++k) {
                hz.append(base + k * step);
            }
        }
        for (qint64 off :
             {5LL, 15LL, 25LL, 95LL, 995LL, 9995LL, 1LL, 4LL, 6LL, 999994LL, 999995LL, 999996LL, 999999LL}) {
            hz.append(base + off);
            hz.append(base + 12345670 + off);
        }
    }
    hz.append(1);
    hz.append(5);
    hz.append(999995);
    return hz;
}

void
test_util_js_contract(void) {
    QFile file(QStringLiteral(DSD_QML_UI_DIR "/Util.js"));
    expect("Util.js opens", file.open(QIODevice::ReadOnly));
    QString source = QString::fromUtf8(file.readAll());
    source.remove(QStringLiteral(".pragma library"));
    QJSEngine engine;
    engine.installExtensions(QJSEngine::TranslationExtension);
    expect("Util.js evaluates", !engine.evaluate(source, file.fileName()).isError());
    QJSValue mhzText = engine.globalObject().property(QStringLiteral("mhzText"));
    QJSValue fmtMhz = engine.globalObject().property(QStringLiteral("fmtMhz"));
    expect("Util.mhzText and Util.fmtMhz are callable", mhzText.isCallable() && fmtMhz.isCallable());

    for (qint64 hz : sweep()) {
        const double value = static_cast<double>(hz);
        const QString shown = mhzText.call({QJSValue(value)}).toString();
        const QString shownWithUnit = fmtMhz.call({QJSValue(value)}).toString();
        const QString siteSheet = engine.evaluate(QStringLiteral("(%1 / 1000000).toFixed(6)").arg(hz)).toString();
        QList<QString> typed = dotted_prefixes(shown) + dotted_prefixes(siteSheet);
        typed.append(shownWithUnit);
        for (const QString& query : typed) {
            if (!frequency_query_matches(frequency_query_parse(query), hz)) {
                DSD_FPRINTF(stderr, "FAIL: \"%s\" (Util prints \"%s\") does not find %lld Hz\n",
                            query.toUtf8().constData(), shown.toUtf8().constData(), static_cast<long long>(hz));
                ++g_failures;
            }
        }
    }
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_grammar();
    test_matches();
    test_util_js_contract();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
