// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// History CSV export: the file builder (columns, time zones, formula guard, the recordings cell) and the model's export
// rows (display values through data(), recording names in one provider call).

#include <QByteArray>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1String>
#include <QList>
#include <QLocale>
#include <QModelIndex>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTime>
#include <QTimeZone>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <clocale>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/analog_mode.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <stdio.h>
#include "../test_support/qt_test_paths.h"
#include "call_history_export.h"
#include "call_history_model.h"
#include "call_recording_provider.h"
#include "csv_text.h"
#include "json_store.h"

using dsd_qt::CallHistoryModel;
namespace history_export = dsd_qt::call_history_export;

namespace {

int g_failures = 0;

void
expect(int line, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: test_ui_qt_call_history_export.cpp:%d\n", line);
        g_failures++;
    }
}

const char kHeader[] =
    "started_local,utc_offset,started_utc,duration_s,type,name,talkgroup,radio_id,radio_name,system,channel,"
    "frequency_hz,access_code,tone,site,analog_mode,encrypted,emergency,message,recordings\r\n";

QByteArray
head() {
    return QByteArray("\xEF\xBB\xBF", 3) + kHeader;
}

qint64
utc_secs(int year, int month, int day, int hour, int minute, int second) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute, second), QTimeZone::UTC).toSecsSinceEpoch();
}

/* 2026-11-07 14:03:22 UTC. */
qint64
sample_when() {
    return utc_secs(2026, 11, 7, 14, 3, 22);
}

/* A digital voice call with every column filled. */
CallHistoryModel::ExportRow
voice_row() {
    CallHistoryModel::ExportRow row;
    row.when = sample_when();
    row.durationSecs = 42;
    row.kind = CallHistoryModel::KindVoice;
    row.name = QStringLiteral("County Fire");
    row.tg = 1001;
    row.src = 2002;
    row.sourceName = QStringLiteral("Engine 5");
    row.systemName = QStringLiteral("County P25");
    row.channel = QStringLiteral("North");
    row.freqHz = 851012500;
    row.accessCode = QStringLiteral("NAC 293");
    row.siteText = QStringLiteral("P25 · Site 5");
    row.enc = true;
    row.emergency = true;
    row.recordingNames = {QStringLiteral("County (1a2b3c4d)/sub/a.wav"), QStringLiteral("County (1a2b3c4d)/b.wav")};
    return row;
}

/* An analog reception in @p mode ("" when not known). */
CallHistoryModel::ExportRow
analog_row(const QString& mode) {
    CallHistoryModel::ExportRow row;
    row.when = sample_when();
    row.durationSecs = 7;
    row.name = mode.isEmpty() ? QStringLiteral("Analog") : mode;
    row.systemName = QStringLiteral("Scan");
    row.channel = QStringLiteral("Ch 3");
    row.freqHz = 462562500;
    row.receivedTone = QStringLiteral("CTCSS 100.0 Hz");
    row.analog = true;
    row.analogMode = mode;
    return row;
}

/* A text message notice whose text spans two lines and holds quotes. */
CallHistoryModel::ExportRow
notice_row() {
    CallHistoryModel::ExportRow row;
    row.when = sample_when();
    row.kind = CallHistoryModel::KindNotice;
    row.name = QStringLiteral("[CRC ERR] SMS 2002 > 1001");
    row.tg = 1001;
    row.src = 2002;
    row.detail = QStringLiteral("Line one\nLine \"two\"");
    return row;
}

/* The file of @p rows in UTC. */
QByteArray
utc_file(const QList<CallHistoryModel::ExportRow>& rows) {
    return history_export::file(rows, QTimeZone(QTimeZone::UTC));
}

/* The one record (header and BOM stripped) of @p row in @p zone. */
QByteArray
record_of(const CallHistoryModel::ExportRow& row, const QTimeZone& zone) {
    const QByteArray file = history_export::file({row}, zone);
    expect(__LINE__, file.startsWith(head()));
    return file.mid(head().size());
}

/* The BOM, the exact header, and every column of a voice, analog (NFM, AM, unknown) and notice row. */
void
test_columns() {
    expect(__LINE__, utc_file({}) == head());
    const QByteArray file = utc_file({voice_row(), analog_row(QStringLiteral("NFM")), analog_row(QStringLiteral("AM")),
                                      analog_row(QString()), notice_row()});
    const QByteArray expected =
        head()
        + QStringLiteral("2026-11-07 14:03:22,UTC+00:00,2026-11-07T14:03:22Z,42,call,County Fire,1001,2002,Engine 5,"
                         "County P25,North,851012500,NAC 293,,P25 · Site 5,,1,1,,"
                         "County (1a2b3c4d)/sub/a.wav | County (1a2b3c4d)/b.wav\r\n")
              .toUtf8()
        + "2026-11-07 14:03:22,UTC+00:00,2026-11-07T14:03:22Z,7,call,NFM,,,,Scan,Ch 3,462562500,,CTCSS 100.0 Hz,,NFM,"
          "0,0,,\r\n"
        + "2026-11-07 14:03:22,UTC+00:00,2026-11-07T14:03:22Z,7,call,AM,,,,Scan,Ch 3,462562500,,CTCSS 100.0 Hz,,AM,0,"
          "0,,\r\n"
        + "2026-11-07 14:03:22,UTC+00:00,2026-11-07T14:03:22Z,7,call,Analog,,,,Scan,Ch 3,462562500,,CTCSS 100.0 Hz,,"
          "analog,0,0,,\r\n"
        + "2026-11-07 14:03:22,UTC+00:00,2026-11-07T14:03:22Z,,message,[CRC ERR] SMS 2002 > 1001,1001,2002,,,,,,,,,0,"
          "0,\"Line one\nLine \"\"two\"\"\",\r\n";
    if (file != expected) {
        printf("got:\n%s\nwant:\n%s\n", file.constData(), expected.constData());
    }
    expect(__LINE__, file == expected);
}

/* Zero and empty values: no start (0, and a negative stamp) leaves all three time fields empty; an unknown duration,
 * a zero talkgroup, radio id and frequency are empty; flags are 0. */
void
test_empty_values() {
    const CallHistoryModel::ExportRow blank;
    expect(__LINE__, record_of(blank, QTimeZone(QTimeZone::UTC)) == ",,,,call,,,,,,,,,,,,0,0,,\r\n");
    CallHistoryModel::ExportRow negative;
    negative.when = -5;
    negative.durationSecs = 0;
    expect(__LINE__, record_of(negative, QTimeZone(QTimeZone::UTC)) == ",,,0,call,,,,,,,,,,,,0,0,,\r\n");
    const QTimeZone kolkata(QByteArrayLiteral("Asia/Kolkata"));
    if (kolkata.isValid()) {
        expect(__LINE__, record_of(blank, kolkata) == ",,,,call,,,,,,,,,,,,0,0,,\r\n");
    }
}

/* The formula guard on every text column (a recordings folder name starting `=` too), none on the time, numeric and
 * fixed columns. */
void
test_formula_guard() {
    CallHistoryModel::ExportRow row;
    row.when = sample_when();
    row.durationSecs = 3;
    row.name = QStringLiteral("=SUM(A1)");
    row.tg = 5;
    row.src = 6;
    row.sourceName = QStringLiteral("+Alias");
    row.systemName = QStringLiteral("-System");
    row.channel = QStringLiteral("@Channel");
    row.freqHz = 155340000;
    row.accessCode = QStringLiteral("=AC");
    row.receivedTone = QStringLiteral("+Tone");
    row.siteText = QStringLiteral("-Site");
    row.detail = QStringLiteral("=HYPERLINK(1)");
    row.recordingNames = {QStringLiteral("=Fire (1a2b3c4d)/a.wav")};
    expect(__LINE__,
           record_of(row, QTimeZone(QTimeZone::UTC))
               == "2026-11-07 14:03:22,UTC+00:00,2026-11-07T14:03:22Z,3,call,'=SUM(A1),5,6,'+Alias,'-System,'@Channel,"
                  "155340000,'=AC,'+Tone,'-Site,,0,0,'=HYPERLINK(1),'=Fire (1a2b3c4d)/a.wav\r\n");
    // A negative offset starts with its letters, so it is written as it is.
    const QTimeZone york(QByteArrayLiteral("America/New_York"));
    if (york.isValid()) {
        expect(__LINE__, record_of(row, york).startsWith("2026-11-07 09:03:22,UTC-05:00,2026-11-07T14:03:22Z,3,call,"));
    }
}

/* Each row's own offset in the zone given: two rows an hour apart across New York's 2026-11-01 DST end show the same
 * local time with UTC-04:00 then UTC-05:00; Kolkata's half hour; UTC. */
void
test_time_zones() {
    expect(__LINE__, record_of(voice_row(), QTimeZone(QTimeZone::UTC)).startsWith("2026-11-07 14:03:22,UTC+00:00,"));
    const QTimeZone york(QByteArrayLiteral("America/New_York"));
    if (!york.isValid()) {
        printf("SKIP time zones: America/New_York is not available on this host\n");
    } else {
        CallHistoryModel::ExportRow daylight;
        daylight.when = utc_secs(2026, 11, 1, 5, 30, 0);
        CallHistoryModel::ExportRow standard;
        standard.when = utc_secs(2026, 11, 1, 6, 30, 0);
        const QByteArray file = history_export::file({daylight, standard}, york);
        expect(__LINE__, file
                             == head() + "2026-11-01 01:30:00,UTC-04:00,2026-11-01T05:30:00Z,,call,,,,,,,,,,,,0,0,,\r\n"
                                    + "2026-11-01 01:30:00,UTC-05:00,2026-11-01T06:30:00Z,,call,,,,,,,,,,,,0,0,,\r\n");
    }
    const QTimeZone kolkata(QByteArrayLiteral("Asia/Kolkata"));
    if (!kolkata.isValid()) {
        printf("SKIP time zones: Asia/Kolkata is not available on this host\n");
    } else {
        expect(__LINE__,
               record_of(voice_row(), kolkata).startsWith("2026-11-07 19:33:22,UTC+05:30,2026-11-07T14:03:22Z,42,"));
    }
}

/* Times, offsets and numbers keep their digits and separators whatever the locale. */
void
test_locale_independent() {
    const QList<CallHistoryModel::ExportRow> rows{voice_row(), notice_row()};
    const QTimeZone kolkata(QByteArrayLiteral("Asia/Kolkata"));
    const QTimeZone zone = kolkata.isValid() ? kolkata : QTimeZone(QTimeZone::UTC);
    const QByteArray plain = history_export::file(rows, zone);
    const QLocale before;
    const QByteArray numeric = QByteArray(setlocale(LC_NUMERIC, nullptr));
    QLocale::setDefault(QLocale(QLocale::Arabic, QLocale::Egypt));
    expect(__LINE__, history_export::file(rows, zone) == plain);
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    const bool cLocale = setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr;
    printf("export locale check: QLocale Arabic and German%s\n",
           cLocale ? ", LC_NUMERIC de_DE.UTF-8" : " (no de_DE.UTF-8)");
    expect(__LINE__, history_export::file(rows, zone) == plain);
    QLocale::setDefault(before);
    setlocale(LC_NUMERIC, numeric.constData());
}

/* Text beyond ASCII is written as UTF-8; an access code is text, never a number. */
void
test_text_values() {
    CallHistoryModel::ExportRow row = voice_row();
    row.name = QStringLiteral("Ñandú 消防");
    row.sourceName = QStringLiteral("Пожар");
    row.recordingNames.clear();
    const QByteArray record = record_of(row, QTimeZone(QTimeZone::UTC));
    expect(__LINE__, record.contains(QStringLiteral(",Ñandú 消防,1001,2002,Пожар,").toUtf8()));
    expect(__LINE__, record.contains(",851012500,NAC 293,,"));
}

/* The recordings cell: joined by " | ", backslash and pipe escaped inside a name, and split back exactly. */
void
test_recording_names() {
    expect(__LINE__, history_export::joinRecordingNames({}).isEmpty());
    expect(__LINE__, history_export::splitRecordingNames(QString()).isEmpty());
    expect(__LINE__,
           history_export::joinRecordingNames({QStringLiteral("A (1)/a.wav")}) == QStringLiteral("A (1)/a.wav"));
    expect(__LINE__,
           history_export::joinRecordingNames({QStringLiteral("A (1)/a.wav"), QStringLiteral("A (1)/sub/b.wav")})
               == QStringLiteral("A (1)/a.wav | A (1)/sub/b.wav"));
    expect(__LINE__, history_export::joinRecordingNames({QStringLiteral("County\\West (1a)/a.wav")})
                         == QStringLiteral("County\\\\West (1a)/a.wav"));
    expect(__LINE__,
           history_export::joinRecordingNames({QStringLiteral("x\\|y.wav")}) == QStringLiteral("x\\\\\\|y.wav"));
    expect(__LINE__, history_export::joinRecordingNames({QStringLiteral("a | b.wav"), QStringLiteral("c.wav")})
                         == QStringLiteral("a \\| b.wav | c.wav"));

    const QList<QStringList> lists{
        {QStringLiteral("County\\West (1a)/a.wav")},
        {QStringLiteral("x\\|y.wav"), QStringLiteral("F/z.wav")},
        {QStringLiteral("a | b.wav"), QStringLiteral("F/ | .wav"), QStringLiteral("F/|")},
        {QStringLiteral("F/say \"hi\".wav"), QStringLiteral("F/line\r\nbreak\n.wav")},
        {QStringLiteral("F/ends\\"), QStringLiteral("\\starts"), QStringLiteral("F/two  "), QStringLiteral("  lead")},
        {QStringLiteral("=Fire/a.wav"), QStringLiteral("|"), QStringLiteral(" | ")},
    };
    for (const QStringList& names : lists) {
        const QString cell = history_export::joinRecordingNames(names);
        if (history_export::splitRecordingNames(cell) != names) {
            printf("round trip failed: %s\n", qPrintable(cell));
        }
        expect(__LINE__, history_export::splitRecordingNames(cell) == names);
        // The file holds the cell as the CSV writer writes a text field.
        CallHistoryModel::ExportRow row;
        row.recordingNames = names;
        const QByteArray record = record_of(row, QTimeZone(QTimeZone::UTC));
        expect(__LINE__, record == ",,,,call,,,,,,,,,,,,0,0,," + dsd_qt::csv_text::field(cell, true).toUtf8() + "\r\n");
    }
}

/* --- The model's export rows. --- */

/* Counts every call History's rows make to it; names a row's recordings `R/<fragment>.wav`. */
class CountingProvider : public dsd_qt::CallRecordingProvider {
  public:
    QVariantList
    recordingsForFragments(const QStringList&) const override {
        perRowCalls++;
        return {};
    }

    bool
    hasRecordingForFragments(const QStringList&) const override {
        perRowCalls++;
        return false;
    }

    QList<QStringList>
    recordingEntryNames(const QList<QStringList>& fragmentsPerRow) const override {
        batchCalls++;
        lastBatch = fragmentsPerRow;
        if (shortAnswer) {
            return {};
        }
        QList<QStringList> names;
        for (const QStringList& fragments : fragmentsPerRow) {
            QStringList row;
            for (const QString& fragment : fragments) {
                row.append(QStringLiteral("R/%1.wav").arg(fragment));
            }
            names.append(row);
        }
        return names;
    }

    mutable int batchCalls = 0;
    mutable int perRowCalls = 0;
    mutable QList<QStringList> lastBatch;
    bool shortAnswer = false;
};

QJsonObject
stored_row(qint64 when, qulonglong tg, qulonglong seq) {
    QJsonObject obj;
    obj.insert(QLatin1String("when"), when);
    obj.insert(QLatin1String("name"), QStringLiteral("TG %1").arg(tg));
    obj.insert(QLatin1String("tg"), static_cast<qint64>(tg));
    obj.insert(QLatin1String("src"), static_cast<qint64>(tg + 5000));
    obj.insert(QLatin1String("durationSecs"), 4);
    obj.insert(QLatin1String("systemName"), QStringLiteral("County"));
    obj.insert(QLatin1String("kind"), CallHistoryModel::KindVoice);
    obj.insert(QLatin1String("slot"), 0);
    obj.insert(QLatin1String("seq"), static_cast<qint64>(seq));
    obj.insert(QLatin1String("session"), 1);
    obj.insert(QLatin1String("fr"), QJsonArray{QStringLiteral("F%1").arg(seq)});
    return obj;
}

/* Saves @p rows (newest first) as History's store, for the next model to load. */
void
save_store(const QJsonArray& rows) {
    QSettings().clear();
    expect(__LINE__, dsd_qt::json_store_save_array(QStringLiteral("call_history.json"), rows));
}

/* Three rows: a trunk-scan channel's call named by its channel id, an analog NFM reception with a tone and two
 * fragments, and a text message notice. */
QJsonArray
mixed_store() {
    const qint64 base = sample_when();
    QJsonObject notice = stored_row(base + 7200, 300, 3);
    notice.insert(QLatin1String("kind"), CallHistoryModel::KindNotice);
    notice.insert(QLatin1String("durationSecs"), -1);
    notice.insert(QLatin1String("name"), QStringLiteral("SMS 5300 > 300"));
    notice.insert(QLatin1String("detail"), QStringLiteral("Meet at\nstation 2"));
    notice.insert(QLatin1String("em"), true);

    QJsonObject analog = stored_row(base + 3600, 0, 2);
    analog.insert(QLatin1String("src"), 0);
    analog.insert(QLatin1String("name"), QStringLiteral("NFM"));
    analog.insert(QLatin1String("analog"), true);
    analog.insert(QLatin1String("analogMode"), DSD_ANALOG_MODE_NFM);
    analog.insert(QLatin1String("receiveKind"), DSD_RECEIVE_CTCSS);
    analog.insert(QLatin1String("receiveValue"), 1000);
    analog.insert(QLatin1String("receiveKnown"), true);
    analog.insert(QLatin1String("freqHz"), 462562500);
    analog.insert(QLatin1String("fr"), QJsonArray{QStringLiteral("F2"), QStringLiteral("F2b")});

    QJsonObject channel = stored_row(base, 100, 1);
    channel.insert(QLatin1String("name"), QStringLiteral("uid-123"));
    channel.insert(QLatin1String("channel"), QStringLiteral("uid-123"));
    channel.insert(QLatin1String("srcName"), QStringLiteral("Engine 5"));
    channel.insert(QLatin1String("enc"), true);
    channel.insert(QLatin1String("freqHz"), 851012500);
    channel.insert(QLatin1String("acKind"), DSD_ACCESS_CODE_NAC);
    channel.insert(QLatin1String("ac"), 0x293);
    return QJsonArray{notice, analog, channel};
}

/* Every exported value is what data() shows for the row, through its display role. */
void
expect_matches_data(const CallHistoryModel& model, int source, const CallHistoryModel::ExportRow& row) {
    const QModelIndex idx = model.index(source);
    const auto value = [&model, &idx](int role) { return model.data(idx, role); };
    expect(__LINE__, row.when == value(CallHistoryModel::WhenRole).toLongLong());
    expect(__LINE__, row.durationSecs == value(CallHistoryModel::DurationSecsRole).toInt());
    expect(__LINE__, row.kind == value(CallHistoryModel::KindRole).toInt());
    expect(__LINE__, row.name == value(CallHistoryModel::NameRole).toString());
    expect(__LINE__, row.tg == value(CallHistoryModel::TgRole).toULongLong());
    expect(__LINE__, row.src == value(CallHistoryModel::SrcRole).toULongLong());
    expect(__LINE__, row.sourceName == value(CallHistoryModel::SourceNameRole).toString());
    expect(__LINE__, row.systemName == value(CallHistoryModel::SystemNameRole).toString());
    expect(__LINE__, row.channel == value(CallHistoryModel::ChannelRole).toString());
    expect(__LINE__, row.freqHz == value(CallHistoryModel::FreqHzRole).toLongLong());
    expect(__LINE__, row.accessCode == value(CallHistoryModel::AccessCodeRole).toString());
    expect(__LINE__, row.receivedTone == value(CallHistoryModel::ReceivedToneRole).toString());
    expect(__LINE__, row.siteText == value(CallHistoryModel::SiteTextRole).toString());
    expect(__LINE__, row.analog == value(CallHistoryModel::AnalogRole).toBool());
    expect(__LINE__, row.analogMode == value(CallHistoryModel::AnalogModeRole).toString());
    expect(__LINE__, row.enc == value(CallHistoryModel::EncRole).toBool());
    expect(__LINE__, row.emergency == value(CallHistoryModel::EmergencyRole).toBool());
    expect(__LINE__, row.detail == value(CallHistoryModel::DetailRole).toString());
}

/* The export rows read what the screen shows (a channel id by the name setChannelNames() gives it), in the order
 * asked, rows out of range skipped, and every row's recording names from one provider call. */
void
test_export_rows_match_display() {
    save_store(mixed_store());
    CallHistoryModel model;
    expect(__LINE__, model.count() == 3);
    // Without a provider, no names, and nothing to call.
    const QList<CallHistoryModel::ExportRow> bare = model.exportRows({0, 1, 2});
    expect(__LINE__, bare.size() == 3);
    for (const auto& row : bare) {
        expect(__LINE__, row.recordingNames.isEmpty());
    }

    CountingProvider provider;
    model.setRecordingProvider(&provider);
    model.setChannelNames({{QStringLiteral("uid-123"), QStringLiteral("County Fire")}});
    provider.perRowCalls = 0;
    const QList<CallHistoryModel::ExportRow> rows = model.exportRows({0, 1, 2});
    expect(__LINE__, provider.batchCalls == 1 && provider.perRowCalls == 0);
    expect(__LINE__, rows.size() == 3);
    if (rows.size() != 3) {
        return;
    }
    for (int i = 0; i < rows.size(); i++) {
        expect_matches_data(model, i, rows.at(i));
    }
    // The fixture's values reached the rows as the screen shows them.
    expect(__LINE__, rows.at(0).kind == CallHistoryModel::KindNotice
                         && rows.at(0).detail == QStringLiteral("Meet at\nstation 2"));
    expect(__LINE__, rows.at(0).emergency && rows.at(0).durationSecs == -1);
    expect(__LINE__, rows.at(1).analog && rows.at(1).analogMode == QStringLiteral("NFM"));
    expect(__LINE__, rows.at(1).receivedTone == QStringLiteral("CTCSS 100.0 Hz") && rows.at(1).freqHz == 462562500);
    expect(__LINE__,
           rows.at(2).name == QStringLiteral("County Fire") && rows.at(2).channel == QStringLiteral("County Fire"));
    expect(__LINE__, rows.at(2).accessCode == QStringLiteral("NAC 293") && rows.at(2).enc);
    expect(__LINE__, rows.at(2).sourceName == QStringLiteral("Engine 5") && rows.at(2).tg == 100U);
    expect(__LINE__, rows.at(0).recordingNames == QStringList{QStringLiteral("R/F3.wav")});
    expect(__LINE__,
           rows.at(1).recordingNames == (QStringList{QStringLiteral("R/F2.wav"), QStringLiteral("R/F2b.wav")}));
    expect(__LINE__, rows.at(2).recordingNames == QStringList{QStringLiteral("R/F1.wav")});

    // A rename shows in the next export.
    model.setChannelNames({{QStringLiteral("uid-123"), QStringLiteral("Station 9")}});
    const QList<CallHistoryModel::ExportRow> renamed = model.exportRows({2});
    if (renamed.size() != 1) {
        expect(__LINE__, false);
        return;
    }
    expect(__LINE__, renamed.at(0).name == QStringLiteral("Station 9"));
    expect(__LINE__, renamed.at(0).channel == QStringLiteral("Station 9"));
    expect_matches_data(model, 2, renamed.at(0));

    // The order asked; rows out of range are skipped; still one call, with only the rows kept.
    provider.batchCalls = 0;
    const QList<CallHistoryModel::ExportRow> picked = model.exportRows({2, -1, 0, 3, 99});
    expect(__LINE__, provider.batchCalls == 1);
    expect(__LINE__, provider.lastBatch == (QList<QStringList>{{QStringLiteral("F1")}, {QStringLiteral("F3")}}));
    expect(__LINE__, picked.size() == 2);
    if (picked.size() != 2) {
        return;
    }
    expect_matches_data(model, 2, picked.at(0));
    expect_matches_data(model, 0, picked.at(1));
    expect(__LINE__, picked.at(0).recordingNames == QStringList{QStringLiteral("R/F1.wav")});

    // A provider that answers for fewer rows than asked names none, rather than another row's.
    provider.shortAnswer = true;
    const QList<CallHistoryModel::ExportRow> shortRows = model.exportRows({0, 1, 2});
    expect(__LINE__, shortRows.size() == 3);
    for (const auto& row : shortRows) {
        expect(__LINE__, row.recordingNames.isEmpty());
    }
    model.setRecordingProvider(nullptr);
}

/* A full log of 1,000 rows: one provider call for all of them, each row its own names. */
void
test_export_rows_full_log() {
    constexpr int kRows = 1000;
    QJsonArray stored;
    const qint64 base = sample_when();
    for (int i = 0; i < kRows; i++) {
        // Newest first, an hour apart, each its own talkgroup: nothing folds.
        stored.append(
            stored_row(base - qint64{i} * 3600, static_cast<qulonglong>(i + 1), static_cast<qulonglong>(i + 1)));
    }
    save_store(stored);
    CallHistoryModel model;
    expect(__LINE__, model.count() == kRows);
    CountingProvider provider;
    model.setRecordingProvider(&provider);
    provider.perRowCalls = 0;
    QList<int> all;
    for (int i = 0; i < kRows; i++) {
        all.append(i);
    }
    const QList<CallHistoryModel::ExportRow> rows = model.exportRows(all);
    expect(__LINE__, provider.batchCalls == 1 && provider.perRowCalls == 0);
    expect(__LINE__, rows.size() == kRows && provider.lastBatch.size() == kRows);
    if (rows.size() != kRows) {
        return;
    }
    for (int i = 0; i < kRows; i++) {
        expect(__LINE__, rows.at(i).tg == static_cast<qulonglong>(i + 1));
        expect(__LINE__, rows.at(i).recordingNames == QStringList{QStringLiteral("R/F%1.wav").arg(i + 1)});
    }
    model.setRecordingProvider(nullptr);
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-call-history-export-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    QTemporaryDir settingsDir;
    expect(__LINE__, settingsDir.isValid());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());

    test_columns();
    test_empty_values();
    test_formula_guard();
    test_time_zones();
    test_locale_independent();
    test_text_values();
    test_recording_names();
    test_export_rows_match_display();
    test_export_rows_full_log();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "UI_QT_CALL_HISTORY_EXPORT: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("UI_QT_CALL_HISTORY_EXPORT: all passed\n");
    return 0;
}
