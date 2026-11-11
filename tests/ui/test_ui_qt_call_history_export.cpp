// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// History CSV export: the file builder (columns, time zones, formula guard, the recordings cell), the model's export
// rows (display values through data(), recording names in one provider call), and the exporter History's screens call
// (the rows a filter view shows, the token flow, the save through the writer, its teardown).

#include <QByteArray>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QList>
#include <QLocale>
#include <QModelIndex>
#include <QObject>
#include <QPair>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTime>
#include <QTimeZone>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <clocale>
#include <condition_variable>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/analog_mode.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <functional>
#include <memory>
#include <mutex>
#include <stdio.h>
#include "../test_support/qt_test_paths.h"
#include "call_history_export.h"
#include "call_history_exporter.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "call_recording_provider.h"
#include "csv_text.h"
#include "document_export_writer.h"
#include "document_save_transport.h"
#include "json_store.h"
#include "recording_store.h"

using dsd_qt::CallHistoryExporter;
using dsd_qt::CallHistoryFilterModel;
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

/* --- The exporter History's screens call. --- */

/* Runs the event loop for about @p ms. */
void
spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/* Runs the event loop until @p done holds or about 10 s pass; whether it held. */
bool
spin_until(const std::function<bool()>& done) {
    for (int i = 0; i < 1000 && !done(); ++i) {
        spin(10);
    }
    return done();
}

QByteArray
read_bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/* What @p dir holds, hidden entries too. */
QStringList
entries(const QString& dir) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

/* A save transport that writes nothing: each save records the bytes it was handed and waits at a gate, which an abort
 * opens too; each Download-folder row let go of is recorded. */
class GateTransport : public dsd_qt::DocumentSaveTransport {
  public:
    explicit GateTransport(bool open) : m_open(open) {}

    bool
    save(const QString& sourcePath, const QString& reference) override {
        const QByteArray bytes = read_bytes(sourcePath);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_saves.append(qMakePair(reference, bytes));
        m_cv.wait(lock, [this]() { return m_open || aborted(); });
        m_returned++;
        return !aborted();
    }

    void
    discardPendingDownload(const QString& reference) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_discards.append(reference);
    }

    void
    openGate() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_open = true;
        m_cv.notify_all();
    }

    QList<QPair<QString, QByteArray>>
    saves() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_saves;
    }

    QStringList
    discards() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_discards;
    }

    int
    returned() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_returned;
    }

  protected:
    void
    abortPlatform() override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_cv.notify_all();
    }

  private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_open;
    int m_returned = 0;
    QList<QPair<QString, QByteArray>> m_saves;
    QStringList m_discards;
};

/* What an exporter reported: each finished(token, ok) with busy() as it arrived, and busy() at each busyChanged(). */
struct ExporterLog {
    QList<QPair<int, bool>> finished;
    QList<bool> busyAtFinished;
    QList<bool> busyChanges;

    void
    follow(CallHistoryExporter* exporter) {
        QObject::connect(exporter, &CallHistoryExporter::finished, exporter, [this, exporter](int token, bool ok) {
            finished.append(qMakePair(token, ok));
            busyAtFinished.append(exporter->busy());
        });
        QObject::connect(exporter, &CallHistoryExporter::busyChanged, exporter,
                         [this, exporter]() { busyChanges.append(exporter->busy()); });
    }
};

/* An exporter of @p model saving through @p transport, staging under @p root, writing times in UTC; @p zoneReads (when
 * given) counts its reads of the zone. */
std::unique_ptr<CallHistoryExporter>
make_exporter(const std::shared_ptr<dsd_qt::DocumentSaveTransport>& transport, const QString& root,
              CallHistoryModel* model, int* zoneReads = nullptr) {
    return std::make_unique<CallHistoryExporter>(
        transport, root + QStringLiteral("/history-export"), model,
        [zoneReads]() {
            if (zoneReads != nullptr) {
                (*zoneReads)++;
            }
            return QTimeZone(QTimeZone::UTC);
        },
        nullptr);
}

/* Whether @p map is a prepared export of @p count rows out of @p total. */
bool
prepared(const QVariantMap& map, int count, int total) {
    return map.value(QStringLiteral("token")).toInt() > 0 && map.value(QStringLiteral("count")).toInt() == count
           && map.value(QStringLiteral("total")).toInt() == total;
}

/* Whether @p map says nothing was prepared, with @p count (0: no rows shown; -1: not a History view of the model). */
bool
not_prepared(const QVariantMap& map, int count) {
    return map.value(QStringLiteral("token")).toInt() == 0 && map.value(QStringLiteral("count")).toInt() == count;
}

int
token_of(const QVariantMap& map) {
    return map.value(QStringLiteral("token")).toInt();
}

/* Six rows, newest first. A search for "fire" over the County system's clear calls shows rows 0 and 4 only: row 1 is
 * another system's, row 2 is encrypted, row 3 a message (its text holds "Fire"), and the search does not find row 5. */
QJsonArray
filter_store() {
    const qint64 base = sample_when();
    QJsonObject dispatch = stored_row(base, 100, 1);
    dispatch.insert(QLatin1String("name"), QStringLiteral("Fire Dispatch"));
    QJsonObject city = stored_row(base - 3600, 101, 2);
    city.insert(QLatin1String("name"), QStringLiteral("Fire North"));
    city.insert(QLatin1String("systemName"), QStringLiteral("City"));
    QJsonObject ops = stored_row(base - 7200, 102, 3);
    ops.insert(QLatin1String("name"), QStringLiteral("Fire Ops"));
    ops.insert(QLatin1String("enc"), true);
    QJsonObject sms = stored_row(base - 10800, 103, 4);
    sms.insert(QLatin1String("kind"), CallHistoryModel::KindNotice);
    sms.insert(QLatin1String("durationSecs"), -1);
    sms.insert(QLatin1String("name"), QStringLiteral("SMS 5103 > 103"));
    sms.insert(QLatin1String("detail"), QStringLiteral("Fire at the station"));
    QJsonObject tac = stored_row(base - 14400, 104, 5);
    tac.insert(QLatin1String("name"), QStringLiteral("Fire Tac"));
    QJsonObject police = stored_row(base - 18000, 105, 6);
    police.insert(QLatin1String("name"), QStringLiteral("Police"));
    return QJsonArray{dispatch, city, ops, sms, tac, police};
}

/* The exporter writes the rows a History view shows (search, system and kind applied), in the list's order, with the
 * log's total; the zone is read once per prepare; the save goes through the transport to a local file holding the
 * builder's bytes, and its outcome comes back as finished(token, ok). Unfiltered, the whole log. */
void
test_exporter_shown_rows(const QString& root) {
    save_store(filter_store());
    CallHistoryModel model;
    expect(__LINE__, model.count() == 6);
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    view.setFilterText(QStringLiteral("fire"));
    view.setFilterSystem(QStringLiteral("County"));
    view.setFilterKind(1);
    expect(__LINE__, view.rowCount() == 2);
    int zoneReads = 0;
    const auto exporter = make_exporter(std::make_shared<dsd_qt::DocumentSaveTransport>(), root, &model, &zoneReads);
    ExporterLog log;
    log.follow(exporter.get());

    const QVariantMap shown = exporter->prepare(&view);
    expect(__LINE__, prepared(shown, 2, 6));
    expect(__LINE__, zoneReads == 1);
    const QString path = root + QStringLiteral("/shown rows.csv");
    expect(__LINE__, exporter->save(token_of(shown), path));
    expect(__LINE__, spin_until([&log]() { return log.finished.size() == 1; }));
    expect(__LINE__, log.finished.value(0) == qMakePair(token_of(shown), true));
    const QByteArray file = read_bytes(path);
    expect(__LINE__, file == history_export::file(model.exportRows({0, 4}), QTimeZone(QTimeZone::UTC)));
    // Two records after the header, in the list's order.
    expect(__LINE__, file.count("\r\n") == 3);
    expect(__LINE__, file.indexOf("Fire Dispatch") > 0 && file.indexOf("Fire Dispatch") < file.indexOf("Fire Tac"));

    // With no search and no filter: the whole log.
    view.setFilterText(QString());
    view.setFilterSystem(QString());
    view.setFilterKind(0);
    const QVariantMap all = exporter->prepare(&view);
    expect(__LINE__, prepared(all, 6, 6) && token_of(all) != token_of(shown));
    expect(__LINE__, zoneReads == 2);
    const QString whole = root + QStringLiteral("/whole log.csv");
    expect(__LINE__, exporter->save(token_of(all), whole));
    expect(__LINE__, spin_until([&log]() { return log.finished.size() == 2; }));
    expect(__LINE__, log.finished.value(1) == qMakePair(token_of(all), true));
    expect(__LINE__,
           read_bytes(whole) == history_export::file(model.exportRows({0, 1, 2, 3, 4, 5}), QTimeZone(QTimeZone::UTC)));

    // A save the transport fails is still answered, with ok false.
    const QVariantMap again = exporter->prepare(&view);
    expect(__LINE__, exporter->save(token_of(again), QStringLiteral("relative.csv")));
    expect(__LINE__, spin_until([&log]() { return log.finished.size() == 3; }));
    expect(__LINE__, log.finished.value(2) == qMakePair(token_of(again), false));
}

/* Only a History filter view of the exporter's own model is exported: one over another model, one with no model, a
 * plain object and none are refused with {0, -1}, and the refusal drops the file a prepare before it held. */
void
test_exporter_refuses_other_views(const QString& root) {
    save_store(filter_store());
    CallHistoryModel model;
    CallHistoryModel other;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    CallHistoryFilterModel foreign;
    foreign.setSourceModel(&other);
    CallHistoryFilterModel unset;
    QObject plain;
    const auto transport = std::make_shared<GateTransport>(true);
    const auto exporter = make_exporter(transport, root, &model);

    const int token = token_of(exporter->prepare(&view));
    expect(__LINE__, token > 0);
    expect(__LINE__, not_prepared(exporter->prepare(&foreign), -1));
    expect(__LINE__, !exporter->save(token, QStringLiteral("after-refusal")));
    expect(__LINE__, transport->discards() == QStringList{QStringLiteral("after-refusal")});
    expect(__LINE__, not_prepared(exporter->prepare(&unset), -1));
    expect(__LINE__, not_prepared(exporter->prepare(&plain), -1));
    expect(__LINE__, not_prepared(exporter->prepare(nullptr), -1));
    expect(__LINE__, transport->saves().isEmpty() && !exporter->busy());
}

/* The token flow: the two History screens share one exporter, so a second prepare replaces the first's file (its save
 * is refused and its picked document's Download-folder row let go of); a used, discarded or stale token is refused the
 * same way; a discard of another token leaves the pending file; a prepare that finds no rows shown drops it. */
void
test_exporter_tokens(const QString& root) {
    save_store(filter_store());
    CallHistoryModel model;
    CallHistoryFilterModel idle;
    idle.setSourceModel(&model);
    CallHistoryFilterModel session;
    session.setSourceModel(&model);
    session.setFilterText(QStringLiteral("police"));
    const auto transport = std::make_shared<GateTransport>(true);
    const auto exporter = make_exporter(transport, root, &model);
    ExporterLog log;
    log.follow(exporter.get());

    const QVariantMap first = exporter->prepare(&idle);
    const QVariantMap second = exporter->prepare(&session);
    expect(__LINE__, prepared(first, 6, 6) && prepared(second, 1, 6) && token_of(first) != token_of(second));
    expect(__LINE__, !exporter->save(token_of(first), QStringLiteral("first")));
    expect(__LINE__, transport->discards() == QStringList{QStringLiteral("first")});
    expect(__LINE__, exporter->save(token_of(second), QStringLiteral("second")));
    expect(__LINE__, spin_until([&log]() { return log.finished.size() == 1; }));
    expect(__LINE__, log.finished.value(0) == qMakePair(token_of(second), true));
    const auto saves = transport->saves();
    expect(__LINE__, saves.size() == 1 && saves.value(0).first == QStringLiteral("second"));
    expect(__LINE__, saves.value(0).second == history_export::file(model.exportRows({5}), QTimeZone(QTimeZone::UTC)));
    // Used once: a second save of the same token is refused.
    expect(__LINE__, !exporter->save(token_of(second), QStringLiteral("again")));
    expect(__LINE__, !exporter->save(0, QStringLiteral("zero")));

    // A discard of another token (or 0) leaves the pending file; its own drops it.
    const int kept = token_of(exporter->prepare(&idle));
    exporter->discard(0);
    exporter->discard(kept + 1);
    expect(__LINE__, exporter->save(kept, QStringLiteral("kept")));
    expect(__LINE__, spin_until([&log]() { return log.finished.size() == 2; }));
    const int dropped = token_of(exporter->prepare(&idle));
    exporter->discard(dropped);
    expect(__LINE__, !exporter->save(dropped, QStringLiteral("discarded")));

    // The list emptied between the press and the read: nothing to export, and the file before it is gone.
    const int emptied = token_of(exporter->prepare(&idle));
    idle.setFilterText(QStringLiteral("no such call"));
    expect(__LINE__, not_prepared(exporter->prepare(&idle), 0));
    expect(__LINE__, !exporter->save(emptied, QStringLiteral("emptied")));
    expect(__LINE__, transport->discards()
                         == (QStringList{QStringLiteral("first"), QStringLiteral("again"), QStringLiteral("zero"),
                                         QStringLiteral("discarded"), QStringLiteral("emptied")}));
    expect(__LINE__, transport->saves().size() == 2 && log.finished.size() == 2);
}

/* busy is true from an accepted save until its finished(), with busyChanged() at each edge (false again before
 * finished() arrives); a save while one runs is refused, its file dropped and its reference let go of. */
void
test_exporter_busy(const QString& root) {
    save_store(filter_store());
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    const auto transport = std::make_shared<GateTransport>(false);
    const auto exporter = make_exporter(transport, root, &model);
    ExporterLog log;
    log.follow(exporter.get());

    const int token = token_of(exporter->prepare(&view));
    expect(__LINE__, !exporter->busy() && log.busyChanges.isEmpty());
    expect(__LINE__, exporter->save(token, QStringLiteral("running")));
    expect(__LINE__, exporter->busy() && log.busyChanges == QList<bool>{true});
    expect(__LINE__, spin_until([&transport]() { return transport->saves().size() == 1; }));

    const int next = token_of(exporter->prepare(&view));
    expect(__LINE__, next > 0 && !exporter->save(next, QStringLiteral("while-running")));
    expect(__LINE__, transport->discards() == QStringList{QStringLiteral("while-running")});
    expect(__LINE__, exporter->busy() && log.busyChanges == QList<bool>{true});

    transport->openGate();
    expect(__LINE__, spin_until([&log]() { return log.finished.size() == 1; }));
    expect(__LINE__, log.finished.value(0) == qMakePair(token, true) && log.busyAtFinished == QList<bool>{false});
    expect(__LINE__, !exporter->busy() && log.busyChanges == (QList<bool>{true, false}));
    // The refused save used its file up.
    expect(__LINE__, !exporter->save(next, QStringLiteral("after")));
    expect(__LINE__, transport->saves().size() == 1);
}

/* Teardown with a save running: the exporter stops the writer (the transport aborted), returns within the writer's
 * bound, and its staging folder goes. */
void
test_exporter_teardown(const QString& root) {
    save_store(filter_store());
    CallHistoryModel model;
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    const auto transport = std::make_shared<GateTransport>(false);
    const QString exportRoot = root + QStringLiteral("/teardown");
    auto exporter = make_exporter(transport, exportRoot, &model);
    int finished = 0;
    QObject receiver;
    QObject::connect(exporter.get(), &CallHistoryExporter::finished, &receiver, [&finished]() { finished++; });
    expect(__LINE__, exporter->save(token_of(exporter->prepare(&view)), QStringLiteral("teardown")));
    expect(__LINE__, spin_until([&transport]() { return transport->saves().size() == 1; }));
    QElapsedTimer timer;
    timer.start();
    exporter.reset();
    expect(__LINE__, timer.elapsed() < dsd_qt::DocumentExportWriter::kStopWaitMs);
    expect(__LINE__, transport->aborted() && transport->returned() == 1);
    expect(__LINE__, entries(exportRoot + QStringLiteral("/history-export")).isEmpty());
    spin(50);
    expect(__LINE__, finished == 0);
}

/* --- End to end with the real recording store. --- */

const quint64 kRing = 0x00C0FFEE12345678ULL;
const QLatin1String kOwnerUid("1a2b3c4d-0000-4000-8000-00000000000a");

QString
ring_fragment(qint64 row) {
    return dsd_qt::call_history_fragment_key(kRing, 0, static_cast<qulonglong>(row));
}

/* A recording published into @p folder: the file, and its `publish` journal line as the format contract spells it. */
void
publish_recording(const QString& folder, const QString& id, const QString& file, qint64 start, qint64 row) {
    QFile wav(folder + QLatin1Char('/') + file);
    expect(__LINE__, wav.open(QIODevice::WriteOnly | QIODevice::Truncate) && wav.write(QByteArray(1000, 'w')) == 1000);
    QJsonObject o;
    o.insert(QStringLiteral("v"), 1);
    o.insert(QStringLiteral("type"), QStringLiteral("publish"));
    o.insert(QStringLiteral("t"), start + 5);
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("file"), file);
    o.insert(QStringLiteral("folder"), QStringLiteral("/somewhere"));
    o.insert(QStringLiteral("start"), start);
    o.insert(QStringLiteral("duration_ms"), 4000);
    o.insert(QStringLiteral("rate"), 8000);
    o.insert(QStringLiteral("instance"), QStringLiteral("%1").arg(kRing, 16, 16, QLatin1Char('0')).toUpper());
    o.insert(QStringLiteral("slot"), 0);
    o.insert(QStringLiteral("row"), row);
    o.insert(QStringLiteral("pending"), false);
    o.insert(QStringLiteral("epoch"), 0);
    o.insert(QStringLiteral("rdio"), false);
    o.insert(QStringLiteral("failed"), false);
    o.insert(QStringLiteral("system"), QStringLiteral("County"));
    o.insert(QStringLiteral("target"), QStringLiteral("Fire"));
    o.insert(QStringLiteral("source"), QStringLiteral("1234"));
    QFile journal(folder + QStringLiteral("/recordings.jsonl"));
    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    expect(__LINE__, journal.open(QIODevice::WriteOnly | QIODevice::Append) && journal.write(line) == line.size());
}

/* The names a recordings zip gives the recordings @p maps lists, the selection's hold released at once. */
QStringList
zip_names_of(dsd_qt::RecordingStore& store, const QVariantList& maps) {
    QStringList ids;
    for (const QVariant& map : maps) {
        ids.append(map.toMap().value(QStringLiteral("id")).toString());
    }
    const QVariantMap selection = store.selectRecordings(ids);
    store.releaseSelection(selection.value(QStringLiteral("selection")).toInt());
    return selection.value(QStringLiteral("names")).toStringList();
}

/* A History export with the real recording store as the model's provider, saved through the base transport to a local
 * file: each row's recordings cell holds the names a recordings zip gives the same recordings (nested kept, start
 * order), and the owner's Export as zip names them the same. */
void
test_exporter_real_recordings(const QString& root) {
    const QString base = root + QStringLiteral("/recording roots");
    expect(__LINE__, QDir().mkpath(base + QStringLiteral("/ext")) && QDir().mkpath(base + QStringLiteral("/int")));
    dsd_qt::RecordingStore store;
    store.setRoots({base + QStringLiteral("/ext/Call recordings"), base + QStringLiteral("/int/Call recordings")});
    store.hostSessionReport(true, false, false);
    store.start();
    const QString folder = store.beginSession(QStringLiteral("saved"), kOwnerUid, QStringLiteral("County"));
    expect(__LINE__, !folder.isEmpty() && QDir().mkpath(folder + QStringLiteral("/sub")));
    const qint64 start = sample_when();
    publish_recording(folder, QStringLiteral("0000000000000097-1"), QStringLiteral("b.wav"), start + 100, 1);
    publish_recording(folder, QStringLiteral("0000000000000097-2"), QStringLiteral("sub/a.wav"), start, 1);
    publish_recording(folder, QStringLiteral("0000000000000097-3"), QStringLiteral("c.wav"), start + 200, 2);
    store.endSession();
    const QStringList both{ring_fragment(1), ring_fragment(2)};
    expect(__LINE__, spin_until([&store, &both]() {
               return store.recordingCount() == 3 && store.recordingsForFragments(both).size() == 3;
           }));

    // A row that holds every recording, one that holds the last, and one whose fragment has none.
    QJsonObject merged = stored_row(start + 300, 200, 1);
    merged.insert(QLatin1String("fr"), QJsonArray{ring_fragment(1), ring_fragment(2)});
    QJsonObject single = stored_row(start - 3600, 201, 2);
    single.insert(QLatin1String("fr"), QJsonArray{ring_fragment(2)});
    QJsonObject none = stored_row(start - 7200, 202, 3);
    none.insert(QLatin1String("fr"), QJsonArray{ring_fragment(9)});
    save_store(QJsonArray{merged, single, none});
    CallHistoryModel model;
    model.setRecordingProvider(&store);
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    const auto exporter = make_exporter(std::make_shared<dsd_qt::DocumentSaveTransport>(), root, &model);
    ExporterLog log;
    log.follow(exporter.get());
    const QVariantMap map = exporter->prepare(&view);
    expect(__LINE__, prepared(map, 3, 3));
    const QString path = root + QStringLiteral("/with recordings.csv");
    expect(__LINE__, exporter->save(token_of(map), path));
    expect(__LINE__, spin_until([&log]() { return log.finished.size() == 1; }));
    expect(__LINE__, log.finished.value(0) == qMakePair(token_of(map), true));

    const QList<QByteArray> records = read_bytes(path).mid(head().size()).split('\n');
    expect(__LINE__, records.size() == 4 && records.value(3).isEmpty());
    const QList<QStringList> fragments{{ring_fragment(1), ring_fragment(2)}, {ring_fragment(2)}, {ring_fragment(9)}};
    QList<QStringList> zipNames;
    for (int i = 0; i < fragments.size(); i++) {
        zipNames.append(zip_names_of(store, store.recordingsForFragments(fragments.value(i))));
        const QByteArray cell =
            dsd_qt::csv_text::field(history_export::joinRecordingNames(zipNames.value(i)), true).toUtf8();
        expect(__LINE__, records.value(i).endsWith("," + cell + "\r"));
    }
    const QString owner = QStringLiteral("County (1a2b3c4d)/");
    expect(__LINE__, zipNames.value(0)
                         == (QStringList{owner + QStringLiteral("sub/a.wav"), owner + QStringLiteral("b.wav"),
                                         owner + QStringLiteral("c.wav")}));
    expect(__LINE__, zipNames.value(1) == QStringList{owner + QStringLiteral("c.wav")} && zipNames.value(2).isEmpty());
    expect(__LINE__, records.value(0).endsWith(",County (1a2b3c4d)/sub/a.wav | County (1a2b3c4d)/b.wav | "
                                               "County (1a2b3c4d)/c.wav\r"));
    const QVariantMap every = store.selectForExport(kOwnerUid, QStringLiteral("all"));
    store.releaseSelection(every.value(QStringLiteral("selection")).toInt());
    expect(__LINE__, every.value(QStringLiteral("names")).toStringList() == zipNames.value(0));
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
    QTemporaryDir exportDir;
    expect(__LINE__, exportDir.isValid());
    test_exporter_shown_rows(exportDir.path());
    test_exporter_refuses_other_views(exportDir.path());
    test_exporter_tokens(exportDir.path());
    test_exporter_busy(exportDir.path());
    test_exporter_teardown(exportDir.path());
    test_exporter_real_recordings(exportDir.path());
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "UI_QT_CALL_HISTORY_EXPORT: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("UI_QT_CALL_HISTORY_EXPORT: all passed\n");
    return 0;
}
