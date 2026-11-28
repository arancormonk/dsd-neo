// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// Issue #909: History's playlist builder (CallHistoryPlaylist, context property `historyPlaylist`). A press takes the
// rows the History view it was given shows (the real CallHistoryFilterModel), oldest first from the start row, reads
// every row's recordings in one provider read, and hands the host the playable ones with their place in the call;
// every result code, and the host's base play.

#include <QByteArray>
#include <QByteArrayView>
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QModelIndex>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <dsd-neo/core/safe_api.h>
#include <limits>
#include <qtenvironmentvariables.h>
#include <stdio.h>
#include <time.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "call_history_playlist.h"
#include "call_recording_provider.h"
#include "decoder_host.h"
#include "json_store.h"

using dsd_qt::CallHistoryFilterModel;
using dsd_qt::CallHistoryModel;
using dsd_qt::CallHistoryPlaylist;
using dsd_qt::DecoderHost;

namespace {

int g_failures = 0;

void
expect(int line, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: test_ui_qt_call_history_playlist.cpp:%d\n", line);
        g_failures++;
    }
}

constexpr qint64 kBase = 1754800000;

/* A recording as the store's maps give it, keyed by its path: its identity derived from @p size. */
QVariantMap
recording(const QString& path, qint64 size, bool available = true, bool deleteQueued = false) {
    return {{QStringLiteral("id"), path},
            {QStringLiteral("path"), path},
            {QStringLiteral("bytes"), size / 2},
            {QStringLiteral("identitySize"), size},
            {QStringLiteral("mtimeSec"), kBase + size},
            {QStringLiteral("mtimeNanos"), static_cast<int>(size % 1000)},
            {QStringLiteral("durationMs"), 4000},
            {QStringLiteral("available"), available},
            {QStringLiteral("deleteQueued"), deleteQueued}};
}

/* The store as History asks it: each fragment's recordings, and how often each read was made. */
class FakeProvider : public dsd_qt::CallRecordingProvider {
  public:
    QHash<QString, QVariantList> byFragment;
    mutable int batchReads = 0;
    mutable int rowReads = 0;

    QVariantList
    recordingsForFragments(const QStringList& fragments) const override {
        rowReads++;
        return collect(fragments);
    }

    bool
    hasRecordingForFragments(const QStringList& fragments) const override {
        rowReads++;
        return !collect(fragments).isEmpty();
    }

    QList<QVariantList>
    recordingsForFragmentLists(const QList<QStringList>& fragmentsPerRow) const override {
        batchReads++;
        QList<QVariantList> rows;
        for (const QStringList& fragments : fragmentsPerRow) {
            rows.append(collect(fragments));
        }
        return rows;
    }

  private:
    QVariantList
    collect(const QStringList& fragments) const {
        QVariantList list;
        for (const QString& fragment : fragments) {
            list.append(byFragment.value(fragment));
        }
        return list;
    }
};

/* A host with a player that takes every play (or refuses with @c generation 0), keeping what it was handed. */
class FakeHost : public DecoderHost {
  public:
    bool supported = true;
    bool base = false; // answer with the base host's play
    int generation = 7;
    int plays = 0;
    QVariantList items;
    int mode = -1;
    QString origin;

    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return {};
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {}

    bool
    playbackSupported() const override {
        return supported;
    }

    int
    playRecordings(const QVariantList& played, int playMode, const QString& playOrigin) override {
        plays++;
        items = played;
        mode = playMode;
        origin = playOrigin;
        return base ? DecoderHost::playRecordings(played, playMode, playOrigin) : generation;
    }
};

QJsonObject
stored_row(qint64 when, qulonglong tg, const QString& name, const QStringList& fragments) {
    QJsonObject obj;
    obj.insert(QLatin1String("when"), when);
    obj.insert(QLatin1String("name"), name);
    obj.insert(QLatin1String("tg"), static_cast<qint64>(tg));
    obj.insert(QLatin1String("src"), static_cast<qint64>(tg + 5000));
    obj.insert(QLatin1String("durationSecs"), 4);
    obj.insert(QLatin1String("systemName"), QStringLiteral("County"));
    obj.insert(QLatin1String("kind"), CallHistoryModel::KindVoice);
    obj.insert(QLatin1String("slot"), 0);
    obj.insert(QLatin1String("seq"), static_cast<qint64>(tg));
    obj.insert(QLatin1String("session"), 1);
    obj.insert(QLatin1String("fr"), QJsonArray::fromStringList(fragments));
    return obj;
}

void
save_store(const QJsonArray& rows) {
    QSettings().clear();
    expect(__LINE__, dsd_qt::json_store_save_array(QStringLiteral("call_history.json"), rows));
}

/*
 * History, newest first, and its recordings:
 *   0  Fire Ops   F8 playable, F8w withdrawn (another call's: never this row's)
 *   1  Fire Tac   F7 playable
 *   2  SMS        a notice: no recording
 *   3  City Fire  F5 playable, on the City system
 *   4  Fire North F4a away, F4b playable, F4c a delete waiting: plays part 2 of 3
 *   5  Fire East  F3 away: no recording that plays now
 *   6  Fire West  F2a and F2b playable, their starts after F4b's (a row's parts are never sorted with another's)
 *   7  Fire Main  not recorded
 */
bool
seed(FakeProvider& provider) {
    QJsonObject ops = stored_row(kBase + 700, 108, QStringLiteral("Fire Ops"), {"F8", "F8w"});
    QJsonObject kept;
    kept.insert(QLatin1String("fr"), QStringLiteral("F8"));
    kept.insert(QLatin1String("when"), kBase + 700);
    kept.insert(QLatin1String("durationSecs"), 4);
    kept.insert(QLatin1String("name"), QStringLiteral("Fire Ops"));
    QJsonObject withdrawn;
    withdrawn.insert(QLatin1String("fr"), QStringLiteral("F8w"));
    withdrawn.insert(QLatin1String("x"), true);
    ops.insert(QLatin1String("fp"), QJsonArray{kept, withdrawn});
    QJsonObject sms = stored_row(kBase + 500, 106, QStringLiteral("SMS 5106 > 106"), {"F6"});
    sms.insert(QLatin1String("kind"), CallHistoryModel::KindNotice);
    sms.insert(QLatin1String("durationSecs"), -1);
    QJsonObject city = stored_row(kBase + 400, 105, QStringLiteral("City Fire"), {"F5"});
    city.insert(QLatin1String("systemName"), QStringLiteral("City"));
    save_store(QJsonArray{ops, stored_row(kBase + 600, 107, QStringLiteral("Fire Tac"), {"F7"}), sms, city,
                          stored_row(kBase + 300, 104, QStringLiteral("Fire North"), {"F4a", "F4b", "F4c"}),
                          stored_row(kBase + 200, 103, QStringLiteral("Fire East"), {"F3"}),
                          stored_row(kBase + 100, 102, QStringLiteral("Fire West"), {"F2a", "F2b"}),
                          stored_row(kBase, 101, QStringLiteral("Fire Main"), {"F1"})});
    provider.byFragment = {
        {QStringLiteral("F8"), {recording(QStringLiteral("/r/f8.wav"), 1800)}},
        {QStringLiteral("F8w"), {recording(QStringLiteral("/r/f8w.wav"), 1801)}},
        {QStringLiteral("F7"), {recording(QStringLiteral("/r/f7.wav"), 1700)}},
        {QStringLiteral("F5"), {recording(QStringLiteral("/r/f5.wav"), 1500)}},
        {QStringLiteral("F4a"), {recording(QStringLiteral("/r/f4a.wav"), 1401, false)}},
        {QStringLiteral("F4b"), {recording(QStringLiteral("/r/f4b.wav"), 1402)}},
        {QStringLiteral("F4c"), {recording(QStringLiteral("/r/f4c.wav"), 1403, true, true)}},
        {QStringLiteral("F3"), {recording(QStringLiteral("/r/f3.wav"), 1300, false)}},
        {QStringLiteral("F2a"), {recording(QStringLiteral("/r/f2a.wav"), 1201)}},
        {QStringLiteral("F2b"), {recording(QStringLiteral("/r/f2b.wav"), 1202)}},
    };
    return true;
}

/* History's model over the seeded store and its recordings, one view of it, and a host. */
struct Fixture {
    FakeProvider provider;
    bool seeded = seed(provider); // the store saved before the model loads it
    CallHistoryModel model;
    CallHistoryFilterModel view;
    FakeHost host;

    Fixture() {
        model.setRecordingProvider(&provider);
        view.setSourceModel(&model);
    }

    ~Fixture() { model.setRecordingProvider(nullptr); }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    /* The uid of the row named @p name. */
    double
    uid(const QString& name) const {
        for (int row = 0; row < model.rowCount(); row++) {
            const QModelIndex idx = model.index(row);
            if (model.data(idx, CallHistoryModel::NameRole).toString() == name) {
                return model.data(idx, CallHistoryModel::UidRole).toDouble();
            }
        }
        return 0.0;
    }
};

QString
result_of(const QVariantMap& outcome) {
    return outcome.value(QStringLiteral("result")).toString();
}

bool
is_outcome(const QVariantMap& outcome, const char* result, int generation, int calls) {
    return result_of(outcome) == QLatin1String(result)
           && outcome.value(QStringLiteral("generation")).toInt() == generation
           && outcome.value(QStringLiteral("calls")).toInt() == calls && outcome.size() == 3;
}

QStringList
paths_of(const QVariantList& items) {
    QStringList paths;
    for (const QVariant& item : items) {
        paths.append(item.toMap().value(QStringLiteral("path")).toString());
    }
    return paths;
}

QList<int>
ints_of(const QVariantList& items, const QString& key) {
    QList<int> values;
    for (const QVariant& item : items) {
        values.append(item.toMap().value(key).toInt());
    }
    return values;
}

/* Play all, nothing filtered: every recorded call, oldest first; a call's playable parts with their place among all
 * its recordings; the item contract in full; one store read; the host told PLAYLIST from History. */
void
test_play_all_order_and_items() {
    Fixture f;
    expect(__LINE__, f.model.count() == 8 && f.view.rowCount() == 8);
    CallHistoryPlaylist playlist(&f.view, &f.model, &f.host, nullptr);
    f.provider.rowReads = 0;
    const QVariantMap outcome = playlist.start(0);
    expect(__LINE__, is_outcome(outcome, "accepted", 7, 5));
    expect(__LINE__, f.provider.batchReads == 1 && f.provider.rowReads == 0);
    expect(__LINE__, f.host.plays == 1 && f.host.mode == DecoderHost::PlaybackPlaylist
                         && f.host.origin == QLatin1String("history"));
    expect(__LINE__,
           paths_of(f.host.items)
               == (QStringList{QStringLiteral("/r/f2a.wav"), QStringLiteral("/r/f2b.wav"), QStringLiteral("/r/f4b.wav"),
                               QStringLiteral("/r/f5.wav"), QStringLiteral("/r/f7.wav"), QStringLiteral("/r/f8.wav")}));
    expect(__LINE__, ints_of(f.host.items, QStringLiteral("call")) == (QList<int>{0, 0, 1, 2, 3, 4}));
    expect(__LINE__, ints_of(f.host.items, QStringLiteral("partOrdinal")) == (QList<int>{1, 2, 2, 1, 1, 1}));
    expect(__LINE__, ints_of(f.host.items, QStringLiteral("partCount")) == (QList<int>{2, 2, 3, 1, 1, 1}));

    // The middle part of three, with the row's name, time text and uid, and the file as the store indexed it.
    const QVariantMap item = f.host.items.value(2).toMap();
    const int north = f.model.rowForUid(static_cast<qint64>(f.uid(QStringLiteral("Fire North"))));
    const QModelIndex idx = f.model.index(north);
    const QStringList keys{QStringLiteral("call"),        QStringLiteral("key"),      QStringLiteral("line"),
                           QStringLiteral("mtimeNanos"),  QStringLiteral("mtimeSec"), QStringLiteral("partCount"),
                           QStringLiteral("partOrdinal"), QStringLiteral("path"),     QStringLiteral("size"),
                           QStringLiteral("title")};
    QStringList itemKeys = item.keys();
    itemKeys.sort();
    expect(__LINE__, itemKeys == keys);
    expect(__LINE__, item.value(QStringLiteral("size")).toLongLong() == 1402
                         && item.value(QStringLiteral("mtimeSec")).toLongLong() == kBase + 1402
                         && item.value(QStringLiteral("mtimeNanos")).toInt() == 402);
    expect(__LINE__, item.value(QStringLiteral("title")).toString() == QLatin1String("Fire North")
                         && item.value(QStringLiteral("title")) == f.model.data(idx, CallHistoryModel::NameRole));
    // The call's start in local time, date and all.
    const qint64 when = f.model.data(idx, CallHistoryModel::WhenRole).toLongLong();
    expect(__LINE__, when == kBase + 300
                         && item.value(QStringLiteral("line")).toString()
                                == QDateTime::fromSecsSinceEpoch(when).toString(QStringLiteral("yyyy-MM-dd hh:mm")));
    expect(__LINE__,
           item.value(QStringLiteral("key")).toLongLong() == f.model.data(idx, CallHistoryModel::UidRole).toLongLong());
}

/* Play from here: the start call, then each newer one shown; the start's own playable parts first. A start row shown
 * with nothing to play now (away, a notice) is rowNone, one not shown is gone, and neither asks the host. */
void
test_play_from_here() {
    Fixture f;
    CallHistoryPlaylist playlist(&f.view, &f.model, &f.host, nullptr);
    expect(__LINE__, is_outcome(playlist.start(f.uid(QStringLiteral("Fire North"))), "accepted", 7, 4));
    expect(__LINE__, paths_of(f.host.items)
                         == (QStringList{QStringLiteral("/r/f4b.wav"), QStringLiteral("/r/f5.wav"),
                                         QStringLiteral("/r/f7.wav"), QStringLiteral("/r/f8.wav")}));
    expect(__LINE__, ints_of(f.host.items, QStringLiteral("call")) == (QList<int>{0, 1, 2, 3}));
    expect(__LINE__, is_outcome(playlist.start(f.uid(QStringLiteral("Fire Ops"))), "accepted", 7, 1));
    expect(__LINE__, paths_of(f.host.items) == QStringList{QStringLiteral("/r/f8.wav")});
    expect(__LINE__, f.host.plays == 2);

    const int reads = f.provider.batchReads;
    expect(__LINE__, is_outcome(playlist.start(f.uid(QStringLiteral("Fire East"))), "rowNone", 0, 0));
    expect(__LINE__, is_outcome(playlist.start(f.uid(QStringLiteral("SMS 5106 > 106"))), "rowNone", 0, 0));
    expect(__LINE__, is_outcome(playlist.start(f.uid(QStringLiteral("Fire Main"))), "rowNone", 0, 0));
    expect(__LINE__, f.provider.batchReads == reads + 3);
    expect(__LINE__, is_outcome(playlist.start(987654.0), "gone", 0, 0));
    expect(__LINE__, is_outcome(playlist.start(-3.0), "gone", 0, 0));
    expect(__LINE__, is_outcome(playlist.start(std::numeric_limits<double>::quiet_NaN()), "gone", 0, 0));
    expect(__LINE__, is_outcome(playlist.start(std::numeric_limits<double>::infinity()), "gone", 0, 0));
    expect(__LINE__, f.provider.batchReads == reads + 3 && f.host.plays == 2);
    // A start row whose only recording waits for its delete has nothing to play now either.
    f.provider.byFragment[QStringLiteral("F3")] = {recording(QStringLiteral("/r/f3.wav"), 1300, true, true)};
    expect(__LINE__, is_outcome(playlist.start(f.uid(QStringLiteral("Fire East"))), "rowNone", 0, 0));
    expect(__LINE__, f.host.plays == 2);

    // A row the search hides (as after the search flush) is gone; shown again, it plays.
    const double city = f.uid(QStringLiteral("City Fire"));
    f.view.setFilterSystem(QStringLiteral("County"));
    expect(__LINE__, is_outcome(playlist.start(city), "gone", 0, 0) && f.host.plays == 2);
    f.view.setFilterSystem(QString());
    expect(__LINE__, is_outcome(playlist.start(city), "accepted", 7, 3));
}

/* The search, System and Activity pills: only the rows shown play; a filter that shows only rows with nothing to play
 * is none; an empty view is none. */
void
test_filters() {
    Fixture f;
    CallHistoryPlaylist playlist(&f.view, &f.model, &f.host, nullptr);
    f.view.setFilterSystem(QStringLiteral("County"));
    expect(__LINE__, is_outcome(playlist.start(0), "accepted", 7, 4));
    expect(__LINE__, !paths_of(f.host.items).contains(QStringLiteral("/r/f5.wav")));
    f.view.setFilterSystem(QString());
    f.view.setFilterText(QStringLiteral("fire tac"));
    expect(__LINE__, is_outcome(playlist.start(0), "accepted", 7, 1));
    expect(__LINE__, paths_of(f.host.items) == QStringList{QStringLiteral("/r/f7.wav")});
    f.view.setFilterText(QString());
    f.view.setFilterKind(3); // messages: the notice alone
    expect(__LINE__, f.view.rowCount() == 1);
    expect(__LINE__, is_outcome(playlist.start(0), "none", 0, 0));
    f.view.setFilterKind(0);
    f.view.setFilterText(QStringLiteral("east"));
    expect(__LINE__, is_outcome(playlist.start(0), "none", 0, 0));
    f.view.setFilterText(QStringLiteral("nothing matches this"));
    expect(__LINE__, f.view.rowCount() == 0 && is_outcome(playlist.start(0), "none", 0, 0));
    expect(__LINE__, f.host.plays == 2);
}

/* Fixed at the press: later filters and recordings do not change what the host was handed. */
void
test_fixed_at_the_press() {
    Fixture f;
    CallHistoryPlaylist playlist(&f.view, &f.model, &f.host, nullptr);
    expect(__LINE__, is_outcome(playlist.start(0), "accepted", 7, 5));
    const QVariantList handed = f.host.items;
    f.view.setFilterText(QStringLiteral("tac"));
    f.provider.byFragment.insert(QStringLiteral("F3"), {recording(QStringLiteral("/r/f3.wav"), 1300)});
    expect(__LINE__, f.host.items == handed && f.host.plays == 1);
}

/* Two playlists over two views of one log with different filters: each plays the rows its own view shows. */
void
test_bound_to_its_view() {
    Fixture f;
    CallHistoryFilterModel other;
    other.setSourceModel(&f.model);
    other.setFilterSystem(QStringLiteral("City"));
    f.view.setFilterSystem(QStringLiteral("County"));
    CallHistoryPlaylist county(&f.view, &f.model, &f.host, nullptr);
    CallHistoryPlaylist city(&other, &f.model, &f.host, nullptr);
    expect(__LINE__, is_outcome(city.start(0), "accepted", 7, 1));
    expect(__LINE__, paths_of(f.host.items) == QStringList{QStringLiteral("/r/f5.wav")});
    expect(__LINE__, is_outcome(county.start(0), "accepted", 7, 4));
    expect(__LINE__, !paths_of(f.host.items).contains(QStringLiteral("/r/f5.wav")));
    expect(__LINE__, is_outcome(county.start(f.uid(QStringLiteral("City Fire"))), "gone", 0, 0));
    expect(__LINE__, is_outcome(city.start(f.uid(QStringLiteral("Fire Tac"))), "gone", 0, 0));
}

/* unsupported: a host with no player (asked nothing, nothing read); refused: one with a player that does not take the
 * items (generation 0), the base host's play among them. A playlist
 * without a host, or over a view of another log, plays nothing. */
void
test_unsupported_and_unwired() {
    Fixture f;
    CallHistoryPlaylist playlist(&f.view, &f.model, &f.host, nullptr);
    f.host.supported = false;
    expect(__LINE__, is_outcome(playlist.start(0), "unsupported", 0, 0));
    expect(__LINE__, is_outcome(playlist.start(f.uid(QStringLiteral("Fire Tac"))), "unsupported", 0, 0));
    expect(__LINE__, f.host.plays == 0 && f.provider.batchReads == 0);
    f.host.supported = true;
    f.host.generation = 0;
    expect(__LINE__, is_outcome(playlist.start(0), "refused", 0, 0) && f.host.plays == 1);
    expect(__LINE__,
           is_outcome(playlist.start(f.uid(QStringLiteral("Fire Tac"))), "refused", 0, 0) && f.host.plays == 2);
    // A host with a player whose play is the base host's: refused, its reading Failed "unsupported".
    f.host.base = true;
    expect(__LINE__, is_outcome(playlist.start(0), "refused", 0, 0) && f.host.plays == 3);
    expect(__LINE__, f.host.playbackState() == DecoderHost::PlaybackFailed
                         && f.host.playbackError() == QLatin1String("unsupported")
                         && f.host.playbackPath() == QLatin1String("/r/f2a.wav"));

    CallHistoryPlaylist hostless(&f.view, &f.model, nullptr, nullptr);
    expect(__LINE__, is_outcome(hostless.start(0), "unsupported", 0, 0));
    CallHistoryModel other;
    CallHistoryFilterModel foreign;
    foreign.setSourceModel(&other);
    f.host.base = false;
    f.host.generation = 9;
    CallHistoryPlaylist unwired(&foreign, &f.model, &f.host, nullptr);
    expect(__LINE__, is_outcome(unwired.start(0), "none", 0, 0));
    expect(__LINE__, is_outcome(unwired.start(f.uid(QStringLiteral("Fire Tac"))), "gone", 0, 0));
    CallHistoryPlaylist viewless(nullptr, &f.model, &f.host, nullptr);
    expect(__LINE__, is_outcome(viewless.start(0), "none", 0, 0) && f.host.plays == 3);
}

/* The local time zone set from a POSIX TZ string, as the C library and Qt's local time read it. */
void
apply_tz() {
#ifdef _WIN32
    _tzset();
#else
    tzset();
#endif
}

/* An item's line is the call's start in local time as the detail sheet's share title says it, its date included: with
 * TZ=UTC-09 (nine hours ahead of UTC), a call at 20:00 UTC on the 21st reads "2026-11-22 05:00". A row with no start
 * has no line. A host whose runtime does not take the string skips the dated case. */
void
test_item_line() {
    const bool had = qEnvironmentVariableIsSet("TZ");
    const QByteArray previous = qgetenv("TZ");
    qputenv("TZ", "UTC-09");
    apply_tz();
    const QDateTime start(QDate(2026, 11, 21), QTime(20, 0), QTimeZone::UTC);
    FakeProvider provider;
    save_store(QJsonArray{stored_row(start.toSecsSinceEpoch(), 201, QStringLiteral("Fire Ops"), {"Y1"}),
                          stored_row(0, 202, QStringLiteral("Fire Undated"), {"Y0"})});
    provider.byFragment = {{QStringLiteral("Y1"), {recording(QStringLiteral("/r/y1.wav"), 2100)}},
                           {QStringLiteral("Y0"), {recording(QStringLiteral("/r/y0.wav"), 2000)}}};
    {
        CallHistoryModel model;
        model.setRecordingProvider(&provider);
        CallHistoryFilterModel view;
        view.setSourceModel(&model);
        FakeHost host;
        CallHistoryPlaylist playlist(&view, &model, &host, nullptr);
        expect(__LINE__, result_of(playlist.start(0)) == QLatin1String("accepted"));
        QHash<QString, QString> lines;
        for (const QVariant& item : host.items) {
            const QVariantMap map = item.toMap();
            lines.insert(map.value(QStringLiteral("path")).toString(), map.value(QStringLiteral("line")).toString());
        }
        expect(__LINE__, model.count() == 2 && host.items.size() == 2);
        if (start.toTimeZone(QTimeZone(QTimeZone::LocalTime)).offsetFromUtc() != 9 * 3600) {
            DSD_FPRINTF(stdout, "SKIP item line: this host does not take the POSIX TZ string UTC-09\n");
        } else {
            expect(__LINE__, lines.value(QStringLiteral("/r/y1.wav")) == QLatin1String("2026-11-22 05:00"));
        }
        expect(__LINE__,
               lines.contains(QStringLiteral("/r/y0.wav")) && lines.value(QStringLiteral("/r/y0.wav")).isEmpty());
        model.setRecordingProvider(nullptr);
    }
    if (had) {
        qputenv("TZ", previous);
    } else {
        qunsetenv("TZ");
    }
    apply_tz();
}

/* The base host has no player: a play publishes the first item's path Failed "unsupported" and returns 0. */
void
test_base_host_play() {
    FakeHost host;
    host.base = true;
    // Each item with its identity, as a playlist lists it (an item without one is refused before the player).
    const auto item = [](const QString& path) {
        return QVariantMap{{QStringLiteral("path"), path},
                           {QStringLiteral("size"), qint64{1000}},
                           {QStringLiteral("mtimeSec"), kBase},
                           {QStringLiteral("mtimeNanos"), 0}};
    };
    const QVariantList items{item(QStringLiteral("/r/a.wav")), item(QStringLiteral("/r/b.wav"))};
    expect(__LINE__, host.playRecordings(items, DecoderHost::PlaybackPlaylist, QStringLiteral("history")) == 0);
    expect(__LINE__, host.playbackState() == DecoderHost::PlaybackFailed
                         && host.playbackError() == QLatin1String("unsupported")
                         && host.playbackPath() == QLatin1String("/r/a.wav"));
    expect(__LINE__,
           DecoderHost::PlaybackSingle == 0 && DecoderHost::PlaybackParts == 1 && DecoderHost::PlaybackPlaylist == 2);
    // No word on the keyboard of its own: the UI falls back to Qt's input method.
    expect(__LINE__, !host.property("reportsKeyboard").toBool() && host.keyboardTop() < 0);
}

/* A full log: 1,000 rows of three recordings each (the first away), read once for the whole press, every row its own
 * parts in order, oldest first. */
void
test_full_log_one_read() {
    constexpr int kRows = 1000;
    QJsonArray stored;
    FakeProvider provider;
    for (int i = 0; i < kRows; i++) {
        // Newest first, an hour apart, each its own talkgroup: nothing folds.
        const QString key = QStringLiteral("G%1").arg(i);
        const QStringList fragments{key + QStringLiteral("a"), key + QStringLiteral("b")};
        stored.append(stored_row(kBase - qint64{i} * 3600, static_cast<qulonglong>(i) + 1,
                                 QStringLiteral("TG %1").arg(i), fragments));
        provider.byFragment.insert(fragments.at(0), {recording(key + QStringLiteral("-1"), 100 + i, false)});
        provider.byFragment.insert(fragments.at(1), {recording(key + QStringLiteral("-2"), 2000 + i),
                                                     recording(key + QStringLiteral("-3"), 4000 + i)});
    }
    save_store(stored);
    CallHistoryModel model;
    expect(__LINE__, model.count() == kRows);
    model.setRecordingProvider(&provider);
    CallHistoryFilterModel view;
    view.setSourceModel(&model);
    FakeHost host;
    CallHistoryPlaylist playlist(&view, &model, &host, nullptr);
    provider.rowReads = 0;
    expect(__LINE__, is_outcome(playlist.start(0), "accepted", 7, kRows));
    expect(__LINE__, provider.batchReads == 1 && provider.rowReads == 0);
    expect(__LINE__, host.items.size() == qsizetype{2} * kRows);
    bool ordered = host.items.size() == qsizetype{2} * kRows;
    for (int call = 0; ordered && call < kRows; call++) {
        const QString key = QStringLiteral("G%1").arg(kRows - 1 - call);
        const QVariantMap second = host.items.at(qsizetype{2} * call).toMap();
        const QVariantMap third = host.items.at(2 * call + 1).toMap();
        ordered = second.value(QStringLiteral("path")).toString() == key + QStringLiteral("-2")
                  && third.value(QStringLiteral("path")).toString() == key + QStringLiteral("-3")
                  && second.value(QStringLiteral("call")).toInt() == call
                  && third.value(QStringLiteral("call")).toInt() == call
                  && second.value(QStringLiteral("partOrdinal")).toInt() == 2
                  && third.value(QStringLiteral("partOrdinal")).toInt() == 3
                  && third.value(QStringLiteral("partCount")).toInt() == 3;
    }
    expect(__LINE__, ordered);
    model.setRecordingProvider(nullptr);
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-call-history-playlist-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    QTemporaryDir settingsDir;
    expect(__LINE__, settingsDir.isValid());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());

    test_play_all_order_and_items();
    test_play_from_here();
    test_filters();
    test_fixed_at_the_press();
    test_bound_to_its_view();
    test_unsupported_and_unwired();
    test_base_host_play();
    test_item_line();
    test_full_log_one_read();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "UI_QT_CALL_HISTORY_PLAYLIST: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("UI_QT_CALL_HISTORY_PLAYLIST: all passed\n");
    return 0;
}
