// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief SavedSystemCards: Listen's saved-system cards over the real SavedSystemsModel and AppPrefs.
 *
 * One card per site group (system and receiver), the display row Play starts, the group's last listened and key
 * state, every order and its sections, search (folded text, AND terms, frequencies in MHz and kHz), and the signals a
 * rebuild sends: moves rather than resets without a screen reader, a reset with one, held background reorders, and
 * one coalesced rebuild per turn.
 *
 * B1, B2 and B13 lived in HomeScreen.qml's bindings, which read the lead row (`card.lastHeard`, `card.freqMhz`) and
 * only `encKeyType`/`keyCsvPath`. Each of those cases first reads what that binding read from the same rows, to show
 * the old value, then asserts the card's.
 */

#include <QAbstractItemModel>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaObject>
#include <QModelIndex>
#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <cmath>
#include <cstdio>
#include <memory>
#include <utility>

#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "decoder_host.h"
#include "dsd-neo/core/safe_api.h"
#include "imported_files_model.h"
#include "json_store.h"
#include "listen_pins.h"
#include "realtime_clock.h"
#include "saved_system_cards.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "session_args.h"
#include "system_search_query.h"

using dsd_qt::AppPrefs;
using dsd_qt::ImportedFilesModel;
using dsd_qt::SavedSystemCards;
using dsd_qt::SavedSystemsModel;

namespace {

int g_failures = 0;

void
expect(const char* what, bool ok, int line) {
    if (!ok) {
        ++g_failures;
        DSD_FPRINTF(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define check(what, condition) expect((what), !!(condition), __LINE__)

/* The host the library model needs; nothing here runs a decoder. */
class IdleHost : public dsd_qt::DecoderHost {
  public:
    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {}
};

void
settle() {
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

QVariantMap
plain(const QString& name, const QString& freq) {
    return {{QStringLiteral("name"), name},
            {QStringLiteral("sourceType"), QStringLiteral("usb")},
            {QStringLiteral("freqMhz"), freq},
            {QStringLiteral("decodeFlag"), QStringLiteral("-ft")},
            {QStringLiteral("trunking"), true}};
}

/* A RadioReference site of system @p sid on the USB dongle, optionally with a position. */
QVariantMap
site(const QString& name, int sid, int siteId, const QString& freq, const QString& siteName = QString()) {
    QVariantMap row = plain(name, freq);
    row.insert(QStringLiteral("rrSid"), sid);
    row.insert(QStringLiteral("rrSiteId"), siteId);
    row.insert(QStringLiteral("siteName"), siteName);
    return row;
}

QVariantMap
positioned(QVariantMap row, double lat, double lon) {
    row.insert(QStringLiteral("siteLat"), lat);
    row.insert(QStringLiteral("siteLon"), lon);
    row.insert(QStringLiteral("hasSitePos"), true);
    return row;
}

QVariantMap
on_receiver(QVariantMap row, const QString& host) {
    row.insert(QStringLiteral("sourceType"), QStringLiteral("rtltcp"));
    row.insert(QStringLiteral("host"), host);
    row.insert(QStringLiteral("port"), 1234);
    return row;
}

QVariantMap
state(int stid, const QString& name) {
    return {{QStringLiteral("stid"), stid}, {QStringLiteral("name"), name}};
}

QVariantMap
county(int ctid, int stid, const QString& name) {
    return {{QStringLiteral("ctid"), ctid}, {QStringLiteral("stid"), stid}, {QStringLiteral("name"), name}};
}

QVariantMap
placed(QVariantMap row, const QVariantList& states, const QVariantList& counties) {
    row.insert(QStringLiteral("place"), QVariantMap{{QStringLiteral("states"), states},
                                                    {QStringLiteral("counties"), counties},
                                                    {QStringLiteral("candidateStates"), QVariantList()}});
    return row;
}

/* Fresh stores, the default order and no location fix; the cards over them. */
struct Fixture {
    AppPrefs prefs;
    std::unique_ptr<SavedSystemsModel> systems;
    std::unique_ptr<SavedSystemCards> cards;

    explicit Fixture(ImportedFilesModel* files = nullptr) {
        (void)dsd_qt::json_store_save_array(QStringLiteral("saved_systems.json"), QJsonArray());
        prefs.setSystemsSort(QStringLiteral("added"));
        prefs.setLocationFix(0, 0, 0);
        systems = std::make_unique<SavedSystemsModel>();
        cards = std::make_unique<SavedSystemCards>(systems.get(), &prefs, files);
    }

    void
    add(const QVariantList& rows) {
        if (systems->addAll(rows, QString()) < 0) {
            DSD_FPRINTF(stderr, "FAIL: could not add rows\n");
            ++g_failures;
        }
        settle();
    }

    QVariant
    at(int row, int role) const {
        return cards->data(cards->index(row), role);
    }

    QStringList
    column(int role) const {
        QStringList values;
        for (int row = 0; row < cards->rowCount(); ++row) {
            values.append(at(row, role).toString());
        }
        return values;
    }

    int
    rowOfUid(int sourceRow) const {
        return cards->rowForKey(keyOf(sourceRow));
    }

    QString
    keyOf(int sourceRow) const {
        const QString group = systems->groupKey(sourceRow);
        return group.isEmpty() ? systems->get(sourceRow).value(QStringLiteral("uid")).toString() : group;
    }

    QString
    uidOf(int sourceRow) const {
        return systems->get(sourceRow).value(QStringLiteral("uid")).toString();
    }
};

/* The signals a model sent, and the key list a view that followed them holds. */
struct Recorder {
    SavedSystemCards& model;
    QList<QMetaObject::Connection> connections;
    QStringList mirror;
    QList<QList<int>> changedRoles;
    QList<int> changedRows;
    int moves = 0;
    int inserts = 0;
    int removes = 0;
    int resets = 0;
    bool singleMoves = true; // the mirror follows one-row moves, which is all the model sends

    QStringList
    keys() const {
        QStringList list;
        for (int row = 0; row < model.rowCount(); ++row) {
            list.append(model.data(model.index(row), SavedSystemCards::KeyRole).toString());
        }
        return list;
    }

    explicit Recorder(SavedSystemCards& cards) : model(cards), mirror(keys()) {
        connections.append(
            QObject::connect(&model, &QAbstractItemModel::rowsMoved, &model,
                             [this](const QModelIndex&, int start, int end, const QModelIndex&, int row) {
                                 ++moves;
                                 singleMoves = singleMoves && start == end;
                                 mirror.move(start, row > start ? row - 1 : row);
                             }));
        connections.append(QObject::connect(
            &model, &QAbstractItemModel::rowsInserted, &model, [this](const QModelIndex&, int first, int last) {
                ++inserts;
                for (int i = first; i <= last; ++i) {
                    mirror.insert(i, model.data(model.index(i), SavedSystemCards::KeyRole).toString());
                }
            }));
        connections.append(QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                                            [this](const QModelIndex&, int first, int last) {
                                                ++removes;
                                                mirror.remove(first, last - first + 1);
                                            }));
        connections.append(QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [this]() {
            ++resets;
            mirror = keys();
        }));
        connections.append(
            QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                             [this](const QModelIndex& from, const QModelIndex& to, const QList<int>& roles) {
                                 for (int row = from.row(); row <= to.row(); ++row) {
                                     changedRows.append(row);
                                     changedRoles.append(roles);
                                 }
                             }));
    }

    ~Recorder() {
        for (const QMetaObject::Connection& connection : connections) {
            QObject::disconnect(connection);
        }
    }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool
    followed() const {
        return singleMoves && mirror == keys();
    }

    void
    clear() {
        moves = inserts = removes = resets = 0;
        changedRoles.clear();
        changedRows.clear();
    }
};

void
test_one_card_per_group() {
    Fixture f;
    f.add(
        {site(QStringLiteral("Metro — North"), 100, 1, QStringLiteral("851.0125"), QStringLiteral("North")),
         site(QStringLiteral("Metro — South"), 100, 2, QStringLiteral("852.0125"), QStringLiteral("South")),
         on_receiver(site(QStringLiteral("Metro — North"), 100, 1, QStringLiteral("851.0125"), QStringLiteral("North")),
                     QStringLiteral("10.0.0.2")),
         on_receiver(site(QStringLiteral("Metro — East"), 100, 3, QStringLiteral("853.0125"), QStringLiteral("East")),
                     QStringLiteral("10.0.0.2")),
         plain(QStringLiteral("Marine 16"), QStringLiteral("156.8"))});
    check("two receivers make two cards, plus the ungrouped row", f.cards->rowCount() == 3);
    check("totalCount counts cards", f.cards->totalCount() == 3 && f.cards->shownCount() == 3);
    check("each group card counts its sites",
          f.column(SavedSystemCards::SiteCountRole)
              == QStringList({QStringLiteral("2"), QStringLiteral("2"), QStringLiteral("0")}));
    check("a group's key is its site group key", f.at(0, SavedSystemCards::KeyRole) == f.systems->groupKey(0)
                                                     && f.at(1, SavedSystemCards::KeyRole) == f.systems->groupKey(2));
    check("an ungrouped row's key is its uid", f.at(2, SavedSystemCards::KeyRole) == f.uidOf(4));
    check("the lead is the group's lowest row", f.at(1, SavedSystemCards::LeadUidRole) == f.uidOf(2));
    check("rowForKey finds a shown card", f.cards->rowForKey(f.systems->groupKey(2)) == 1);
    check("rowForKey answers -1 for no card", f.cards->rowForKey(QStringLiteral("nothing")) == -1);
    check("an ungrouped row plays itself", f.at(2, SavedSystemCards::PlayUidRole) == f.uidOf(4));
    check("a group with two playable sites plays none directly",
          f.at(0, SavedSystemCards::PlayUidRole).toString().isEmpty());
}

/* B1: HomeScreen's card read `lastHeard` from its delegate's row, the lead, so listening on any other site of the
   group never moved "Last listened". */
void
test_b1_group_last_heard() {
    Fixture f;
    f.add({site(QStringLiteral("Metro — North"), 100, 1, QStringLiteral("851.0125"), QStringLiteral("North")),
           site(QStringLiteral("Metro — South"), 100, 2, QStringLiteral("852.0125"), QStringLiteral("South"))});
    f.systems->touch(1);
    settle();
    const qint64 heard = f.systems->get(1).value(QStringLiteral("lastHeard")).toLongLong();
    const qint64 homeShowed = f.systems->data(f.systems->index(0), SavedSystemsModel::LastHeardRole).toLongLong();
    DSD_FPRINTF(stderr, "B1: HomeScreen's card read the lead's lastHeard=%lld; the site listened has %lld\n",
                static_cast<long long>(homeShowed), static_cast<long long>(heard));
    check("the old binding read the lead's never-listened stamp", homeShowed == 0 && heard > 0);
    check("B1: the card's lastHeard is the group's latest",
          f.at(0, SavedSystemCards::LastHeardRole).toLongLong() == heard);
}

/* B2: with the lead avoided, Play started the other site while the card still printed the lead's frequency. */
void
test_b2_display_row() {
    Fixture f;
    f.add({site(QStringLiteral("Metro — North"), 100, 1, QStringLiteral("851.0125"), QStringLiteral("North")),
           site(QStringLiteral("Metro — South"), 100, 2, QStringLiteral("852.5000"), QStringLiteral("South"))});
    check("avoid the lead", f.systems->setAvoidSite(0, true));
    settle();
    const QString homeShowed = f.systems->get(0).value(QStringLiteral("freqMhz")).toString();
    DSD_FPRINTF(stderr, "B2: HomeScreen's card printed the lead's freqMhz=%s; Play starts the site at 852.5000\n",
                qPrintable(homeShowed));
    check("the old binding read the avoided lead's frequency", homeShowed == QStringLiteral("851.0125"));
    check("B2: the card's frequency is the site Play starts",
          f.at(0, SavedSystemCards::FreqMhzRole).toString() == QStringLiteral("852.5000"));
    check("B2: Play starts that site", f.at(0, SavedSystemCards::PlayUidRole) == f.uidOf(1));
    check("B2: the card's name is that site's", f.at(0, SavedSystemCards::NameRole) == QStringLiteral("Metro — South"));
    check("the lead is still the lead", f.at(0, SavedSystemCards::LeadUidRole) == f.uidOf(0));
    check("the title stays the group's", f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Metro"));
    check("avoid both: no single site to play", f.systems->setAvoidSite(1, true));
    settle();
    check("with no playable site the card shows the lead",
          f.at(0, SavedSystemCards::PlayUidRole).toString().isEmpty()
              && f.at(0, SavedSystemCards::FreqMhzRole) == QStringLiteral("851.0125"));
}

/* B13: picking a decryption profile clears encKeyType and keyCsvPath, which were all HomeScreen checked. */
void
test_b13_profile_key_configured() {
    Fixture f;
    QVariantMap profile = plain(QStringLiteral("Profile"), QStringLiteral("851.0"));
    profile.insert(QStringLiteral("decryptionProfileUid"), QStringLiteral("8e0f1f0a-5d7e-4d2a-9b3c-6f1e2d3c4b5a"));
    QVariantMap keyFile = plain(QStringLiteral("Key file"), QStringLiteral("852.0"));
    keyFile.insert(QStringLiteral("keyCsvPath"), QStringLiteral("/tmp/keys.csv"));
    f.add({profile, keyFile, plain(QStringLiteral("None"), QStringLiteral("853.0"))});
    const QVariantMap stored = f.systems->get(0);
    const bool homeShowed = !stored.value(QStringLiteral("encKeyType")).toString().isEmpty()
                            || !stored.value(QStringLiteral("keyCsvPath")).toString().isEmpty();
    DSD_FPRINTF(stderr, "B13: HomeScreen's key check for a profile-only row read %s\n", homeShowed ? "true" : "false");
    check("the old binding missed the profile", !homeShowed);
    check("B13: a profile-only system has a key configured", f.at(0, SavedSystemCards::KeyConfiguredRole).toBool());
    check("a key file is a key configured", f.at(1, SavedSystemCards::KeyConfiguredRole).toBool());
    check("no key, no profile: none configured", !f.at(2, SavedSystemCards::KeyConfiguredRole).toBool());
}

void
test_titles_and_pins() {
    Fixture f;
    dsd_qt::ScanListsModel lists;
    f.prefs.setPinned({});
    dsd_qt::ListenPins pins(&f.prefs, f.systems.get(), &lists);
    f.add({site(QStringLiteral("Metro — North"), 100, 1, QStringLiteral("851.0125"), QStringLiteral("North")),
           site(QStringLiteral("Metro — South"), 100, 2, QStringLiteral("852.0125"), QStringLiteral("South")),
           site(QStringLiteral("Custom name"), 200, 1, QStringLiteral("855.0"), QStringLiteral("Hill")),
           plain(QStringLiteral("Marine 16"), QStringLiteral("156.8"))});
    check("a generated name loses its site", f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Metro"));
    check("a custom name stays", f.at(1, SavedSystemCards::TitleRole) == QStringLiteral("Custom name"));
    check("an ungrouped row is its name", f.at(2, SavedSystemCards::TitleRole) == QStringLiteral("Marine 16"));
    check("pin the group", pins.pin(QStringLiteral("system"), f.uidOf(0)).isEmpty());
    const QString pinLabel = pins.entries().value(0).toMap().value(QStringLiteral("label")).toString();
    check("the card and its pin name the group alike", pinLabel == f.at(0, SavedSystemCards::TitleRole).toString());
    f.prefs.setPinned({});
}

void
test_orders() {
    Fixture f;
    f.add({plain(QStringLiteral("Site 10"), QStringLiteral("851.0")),
           site(QStringLiteral("Alpha — North"), 100, 1, QStringLiteral("852.0"), QStringLiteral("North")),
           plain(QStringLiteral("Same"), QStringLiteral("853.0")),
           plain(QStringLiteral("Site 2"), QStringLiteral("854.0")),
           plain(QStringLiteral("Same"), QStringLiteral("855.0")),
           site(QStringLiteral("Alpha — South"), 100, 2, QStringLiteral("856.0"), QStringLiteral("South"))});
    const QString sameA = f.uidOf(2);
    const QString sameB = f.uidOf(4);
    const QStringList sames = sameA < sameB ? QStringList({sameA, sameB}) : QStringList({sameB, sameA});
    check("order added is the lowest row",
          f.column(SavedSystemCards::TitleRole)
              == QStringList({QStringLiteral("Site 10"), QStringLiteral("Alpha"), QStringLiteral("Same"),
                              QStringLiteral("Site 2"), QStringLiteral("Same")}));
    check("effectiveSort follows", f.cards->effectiveSort() == QStringLiteral("added"));

    f.prefs.setSystemsSort(QStringLiteral("name"));
    settle();
    check("name order is natural, ties by key",
          f.column(SavedSystemCards::TitleRole)
                  == QStringList({QStringLiteral("Alpha"), QStringLiteral("Same"), QStringLiteral("Same"),
                                  QStringLiteral("Site 2"), QStringLiteral("Site 10")})
              && f.at(1, SavedSystemCards::KeyRole) == sames.at(0)
              && f.at(2, SavedSystemCards::KeyRole) == sames.at(1));
    check("sortId mirrors the pref", f.cards->sortId() == QStringLiteral("name"));
    check("no sections outside place and nearest", f.column(SavedSystemCards::SectionRole).join(QString()).isEmpty());

    check("listen to Site 2", f.systems->update(3, {{QStringLiteral("lastHeard"), 2000}}));
    check("listen to Alpha's south site", f.systems->update(5, {{QStringLiteral("lastHeard"), 3000}}));
    f.prefs.setSystemsSort(QStringLiteral("recent"));
    settle();
    check("last listened first, never listened after by name",
          f.column(SavedSystemCards::TitleRole)
              == QStringList({QStringLiteral("Alpha"), QStringLiteral("Site 2"), QStringLiteral("Same"),
                              QStringLiteral("Same"), QStringLiteral("Site 10")}));
}

void
test_place_sections() {
    Fixture f;
    const QVariantMap iowa = state(19, QStringLiteral("Iowa"));
    const QVariantMap illinois = state(17, QStringLiteral("Illinois"));
    f.add({placed(plain(QStringLiteral("Johnson County"), QStringLiteral("851.0")), {iowa},
                  {county(1, 19, QStringLiteral("Johnson"))}),
           placed(plain(QStringLiteral("Statewide"), QStringLiteral("852.0")), {iowa, illinois}, {}),
           placed(plain(QStringLiteral("Two counties"), QStringLiteral("853.0")), {iowa},
                  {county(1, 19, QStringLiteral("Johnson")), county(2, 19, QStringLiteral("Linn"))}),
           plain(QStringLiteral("Nowhere"), QStringLiteral("854.0")),
           placed(plain(QStringLiteral("Black Hawk County"), QStringLiteral("855.0")), {iowa},
                  {county(3, 19, QStringLiteral("Black Hawk"))}),
           placed(plain(QStringLiteral("Iowa only"), QStringLiteral("856.0")), {iowa}, {}),
           placed(plain(QStringLiteral("Cook County"), QStringLiteral("857.0")), {illinois},
                  {county(4, 17, QStringLiteral("Cook"))}),
           placed(plain(QStringLiteral("Unnamed"), QStringLiteral("858.0")), {QVariantMap{{QStringLiteral("stid"), 5}}},
                  {})});
    f.prefs.setSystemsSort(QStringLiteral("place"));
    settle();
    check("state sections by name, then several states, then no place; one county by county, then several, then none",
          f.column(SavedSystemCards::TitleRole)
              == QStringList({QStringLiteral("Cook County"), QStringLiteral("Black Hawk County"),
                              QStringLiteral("Johnson County"), QStringLiteral("Two counties"),
                              QStringLiteral("Iowa only"), QStringLiteral("Statewide"), QStringLiteral("Nowhere"),
                              QStringLiteral("Unnamed")}));
    check("section names",
          f.column(SavedSystemCards::SectionRole)
              == QStringList({QStringLiteral("Illinois"), QStringLiteral("Iowa"), QStringLiteral("Iowa"),
                              QStringLiteral("Iowa"), QStringLiteral("Iowa"), QStringLiteral("Several states"),
                              QStringLiteral("No place"), QStringLiteral("No place")}));
    check("a section starts on its first card",
          f.column(SavedSystemCards::SectionStartRole)
              == QStringList({QStringLiteral("true"), QStringLiteral("true"), QStringLiteral("false"),
                              QStringLiteral("false"), QStringLiteral("false"), QStringLiteral("true"),
                              QStringLiteral("true"), QStringLiteral("false")}));
    check("place labels",
          f.column(SavedSystemCards::PlaceLabelRole)
              == QStringList({QStringLiteral("Cook, Illinois"), QStringLiteral("Black Hawk, Iowa"),
                              QStringLiteral("Johnson, Iowa"), QStringLiteral("Iowa · 2 counties"),
                              QStringLiteral("Iowa"), QStringLiteral("Illinois, Iowa"), QString(), QString()}));
    check("a place links the card to RadioReference",
          f.at(0, SavedSystemCards::RrLinkedRole).toBool() && !f.at(6, SavedSystemCards::RrLinkedRole).toBool());
}

void
set_fix(AppPrefs& prefs, double lat, double lon) {
    prefs.setLocationFix(lat, lon, dsd_qt::realtimeMSecsSinceEpoch(), 10);
}

void
test_nearest() {
    Fixture f;
    f.add(
        {plain(QStringLiteral("No position"), QStringLiteral("851.0")),
         positioned(site(QStringLiteral("Far — A"), 300, 1, QStringLiteral("852.0"), QStringLiteral("A")), 44.0, -93.6),
         positioned(site(QStringLiteral("Near — Avoided"), 100, 1, QStringLiteral("853.0"), QStringLiteral("Avoided")),
                    41.6, -93.6),
         positioned(site(QStringLiteral("Near — Open"), 100, 2, QStringLiteral("854.0"), QStringLiteral("Open")), 42.0,
                    -93.6),
         positioned(site(QStringLiteral("Shut — A"), 200, 1, QStringLiteral("855.0"), QStringLiteral("A")), 41.6,
                    -93.6)});
    check("avoid Near's closer site", f.systems->setAvoidSite(2, true));
    check("avoid Shut's only site", f.systems->setAvoidSite(4, true));
    f.prefs.setSystemsSort(QStringLiteral("nearest"));
    settle();
    check("desktop: nearest applies as name",
          f.cards->effectiveSort() == QStringLiteral("name") && f.cards->sortId() == QStringLiteral("nearest"));
    check("desktop: the pref is kept", f.prefs.systemsSort() == QStringLiteral("nearest"));
    f.cards->setLocationSupported(true);
    settle();
    check("no fix: still name", f.cards->effectiveSort() == QStringLiteral("name")
                                    && f.column(SavedSystemCards::DistanceStateRole).join(QString()).isEmpty());
    set_fix(f.prefs, 41.6, -93.6);
    settle();
    check("a fix: nearest applies", f.cards->effectiveSort() == QStringLiteral("nearest"));
    check("by the nearest site not avoided, then all avoided, then no coordinates",
          f.column(SavedSystemCards::TitleRole)
              == QStringList({QStringLiteral("Near"), QStringLiteral("Far"), QStringLiteral("Shut"),
                              QStringLiteral("No position")}));
    check("distance states", f.column(SavedSystemCards::DistanceStateRole)
                                 == QStringList({QStringLiteral("positioned"), QStringLiteral("positioned"),
                                                 QStringLiteral("allAvoided"), QStringLiteral("noCoordinates")}));
    check("sections", f.column(SavedSystemCards::SectionRole)
                          == QStringList({QString(), QString(), QStringLiteral("All sites avoided"),
                                          QStringLiteral("No coordinates")}));
    const double nearKm = f.at(0, SavedSystemCards::DistanceKmRole).toDouble();
    check("Near is measured from its open site, about 44.5 km", std::fabs(nearKm - 44.48) < 0.5);
    check("no distance without a ranked site", f.at(3, SavedSystemCards::DistanceKmRole).toDouble() < 0);
    f.prefs.setLocationFix(0, 0, 0);
    settle();
    check("the fix gone: back to name, the pref kept",
          f.cards->effectiveSort() == QStringLiteral("name") && f.prefs.systemsSort() == QStringLiteral("nearest"));
}

/* Review focus 4: an edited site no longer ranks by its old tower or county. */
void
test_edited_row_unplaced() {
    Fixture f;
    const QVariantMap iowa = state(19, QStringLiteral("Iowa"));
    f.add({placed(positioned(site(QStringLiteral("Metro — North"), 100, 1, QStringLiteral("851.0125"),
                                  QStringLiteral("North")),
                             41.6, -93.6),
                  {iowa}, {county(1, 19, QStringLiteral("Johnson"))}),
           placed(positioned(site(QStringLiteral("Metro — South"), 100, 2, QStringLiteral("852.0125"),
                                  QStringLiteral("South")),
                             41.7, -93.6),
                  {iowa}, {county(1, 19, QStringLiteral("Johnson"))})});
    f.cards->setLocationSupported(true);
    set_fix(f.prefs, 41.6, -93.6);
    f.prefs.setSystemsSort(QStringLiteral("nearest"));
    check("retune the south site", f.systems->update(1, {{QStringLiteral("freqMhz"), QStringLiteral("860.0")}}));
    settle();
    const int edited = f.rowOfUid(1);
    check("the edited row is a card of its own", f.cards->rowCount() == 2 && edited >= 0);
    check("nearest: no coordinates",
          f.at(edited, SavedSystemCards::DistanceStateRole) == QStringLiteral("noCoordinates")
              && f.at(edited, SavedSystemCards::SectionRole) == QStringLiteral("No coordinates"));
    f.prefs.setSystemsSort(QStringLiteral("place"));
    settle();
    const int placedRow = f.rowOfUid(1);
    check("place: no place", f.at(placedRow, SavedSystemCards::SectionRole) == QStringLiteral("No place")
                                 && f.at(placedRow, SavedSystemCards::PlaceLabelRole).toString().isEmpty());
}

void
test_search() {
    Fixture f;
    f.add({placed(site(QStringLiteral("Metro County — North Tower"), 100, 1, QStringLiteral("851.0125"),
                       QStringLiteral("North Tower")),
                  {state(19, QStringLiteral("Iowa"))}, {county(1, 19, QStringLiteral("Johnson"))}),
           plain(QStringLiteral("Valley"), QStringLiteral("460.0000")),
           plain(QStringLiteral("Site 851"), QStringLiteral("155.0")),
           plain(QStringLiteral("São Paulo"), QStringLiteral("162.55"))});
    QVariantMap tetra = plain(QStringLiteral("Harbour"), QStringLiteral("390.0"));
    tetra.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fT"));
    f.add({tetra});
    f.cards->setProtocolLabels({{QStringLiteral("-ft"), QStringLiteral("P25")},
                                {QStringLiteral("-fT"), QStringLiteral("TETRA")},
                                {QString(), QStringLiteral("Auto")}});
    const auto shown = [&f](const QString& query) {
        f.cards->setQuery(query);
        settle();
        return f.column(SavedSystemCards::TitleRole);
    };
    check("AND terms", shown(QStringLiteral("metro north")) == QStringList({QStringLiteral("Metro County")}));
    check("AND terms exclude", shown(QStringLiteral("metro valley")).isEmpty());
    check("accents fold", shown(QStringLiteral("SAO")) == QStringList({QStringLiteral("São Paulo")}));
    check("a word and a frequency with its unit",
          shown(QStringLiteral("County 851 MHz")) == QStringList({QStringLiteral("Metro County")}));
    check("a bare number: the frequency and the name",
          shown(QStringLiteral("851")) == QStringList({QStringLiteral("Metro County"), QStringLiteral("Site 851")}));
    check("a decimal frequency", shown(QStringLiteral("851.0125")) == QStringList({QStringLiteral("Metro County")}));
    check("a frequency prefix", shown(QStringLiteral("851.012")) == QStringList({QStringLiteral("Metro County")}));
    check("kHz", shown(QStringLiteral("460000 kHz")) == QStringList({QStringLiteral("Valley")}));
    check("a name with digits", shown(QStringLiteral("Site 851")) == QStringList({QStringLiteral("Site 851")}));
    check("county and state names",
          shown(QStringLiteral("johnson")) == QStringList({QStringLiteral("Metro County")})
              && shown(QStringLiteral("iowa")) == QStringList({QStringLiteral("Metro County")}));
    check("a site name", shown(QStringLiteral("tower")) == QStringList({QStringLiteral("Metro County")}));
    check("the protocol label", shown(QStringLiteral("tetra")) == QStringList({QStringLiteral("Harbour")}));
    check("punctuation reads as a blank",
          shown(QStringLiteral("county—north")) == QStringList({QStringLiteral("Metro County")}));
    shown(QStringLiteral("p25"));
    check("counts while filtered", f.cards->shownCount() == 4 && f.cards->totalCount() == 5);
    check("cleared, everything", shown(QString()).size() == 5 && f.cards->shownCount() == 5);
}

void
test_search_adapter() {
    using dsd_qt::system_search_matches;
    using dsd_qt::system_search_terms;
    check("unit tokens join their number", system_search_terms(QStringLiteral("County 851 MHz  460 khz x"))
                                               == QStringList({QStringLiteral("County"), QStringLiteral("851 MHz"),
                                                               QStringLiteral("460 khz"), QStringLiteral("x")}));
    const QStringList freqs{QStringLiteral("851.0125"), QStringLiteral("460.0000")};
    check("bare 3+ digits is MHz", system_search_matches(QStringLiteral("851"), freqs));
    check("two digits is not", !system_search_matches(QStringLiteral("85"), freqs));
    check("decimal", system_search_matches(QStringLiteral("851.0125"), freqs));
    check("kHz", system_search_matches(QStringLiteral("460000 kHz"), freqs)
                     && system_search_matches(QStringLiteral("851012.5kHz"), freqs)
                     && !system_search_matches(QStringLiteral("851013 kHz"), freqs));
    check("text is no frequency", !system_search_matches(QStringLiteral("Site"), freqs));
    check("Hz from text", dsd_qt::system_search_hz(QStringLiteral("851.0125")) == 851012500
                              && dsd_qt::system_search_hz(QStringLiteral("851")) == 851000000
                              && dsd_qt::system_search_hz(QString()) == 0);
}

/* A saved frequency the tuner reads (session_args_freq_valid) is searchable as the tuner reads it: a sign, an exponent
 * or digits past the Hz all tune 851.0125 MHz, so "851" and "851.0125" find them. */
void
test_search_stored_frequency_spellings() {
    Fixture f;
    f.add({plain(QStringLiteral("Signed"), QStringLiteral("+851.0125")),
           plain(QStringLiteral("Exponent"), QStringLiteral("8.510125e2")),
           plain(QStringLiteral("Past the Hz"), QStringLiteral("851.0125004")),
           plain(QStringLiteral("Other"), QStringLiteral("460.0"))});
    const QStringList all{QStringLiteral("Exponent"), QStringLiteral("Past the Hz"), QStringLiteral("Signed")};
    const auto shown = [&f](const QString& query) {
        f.cards->setQuery(query);
        settle();
        QStringList titles = f.column(SavedSystemCards::TitleRole);
        titles.sort();
        return titles;
    };
    check("stored spellings: whole MHz finds them", shown(QStringLiteral("851")) == all);
    check("stored spellings: the decimal finds them", shown(QStringLiteral("851.0125")) == all);
    for (const QString& freq :
         {QStringLiteral("+851.0125"), QStringLiteral("8.510125e2"), QStringLiteral("851.0125004")}) {
        if (dsd_qt::system_search_hz(freq) != 851012500) {
            DSD_FPRINTF(stderr, "stored spellings: %s reads as %lld Hz\n", qPrintable(freq),
                        static_cast<long long>(dsd_qt::system_search_hz(freq)));
        }
        check("stored spellings: the tuner's Hz", dsd_qt::system_search_hz(freq) == 851012500);
    }
    for (const QString& freq : {QStringLiteral(" 851 "), QStringLiteral("851.0125 MHz"), QStringLiteral("0"),
                                QStringLiteral("-851"), QStringLiteral("nan"), QStringLiteral("5000"), QString()}) {
        check("stored spellings: the tuner's reading, refusals included",
              dsd_qt::system_search_hz(freq) == static_cast<qint64>(dsd_qt::session_args_freq_hz(freq)));
    }
}

void
test_sort_signals() {
    Fixture f;
    QVariantList rows;
    for (int i = 0; i < 12; ++i) {
        rows.append(plain(QStringLiteral("System %1").arg(20 - i), QStringLiteral("851.%1").arg(i)));
    }
    f.add(rows);
    Recorder recorder(*f.cards);
    f.prefs.setSystemsSort(QStringLiteral("name"));
    settle();
    check("a sort change without a screen reader moves cards",
          recorder.moves > 0 && recorder.resets == 0 && recorder.inserts == 0 && recorder.removes == 0);
    check("a view following the moves ends in order", recorder.followed());
    check("the order is the name order", f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("System 9"));

    recorder.clear();
    f.cards->setScreenReaderActive(true);
    f.prefs.setSystemsSort(QStringLiteral("added"));
    settle();
    check("with a screen reader a sort change is one reset", recorder.resets == 1 && recorder.moves == 0);
    check("reset to the order added",
          recorder.followed() && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("System 20"));

    recorder.clear();
    f.cards->setQuery(QStringLiteral("System 1"));
    settle();
    check("a filter with a screen reader hides cards, no reset",
          recorder.resets == 0 && recorder.removes > 0 && recorder.followed());
    f.cards->setQuery(QString());
    settle();
    check("clearing it shows them again", recorder.inserts > 0 && recorder.resets == 0 && recorder.followed());
}

void
test_metadata_edit_and_hold() {
    Fixture f;
    f.add({plain(QStringLiteral("A"), QStringLiteral("851.0")), plain(QStringLiteral("B"), QStringLiteral("852.0")),
           plain(QStringLiteral("C"), QStringLiteral("853.0"))});
    Recorder recorder(*f.cards);
    check("rename B", f.systems->update(1, {{QStringLiteral("name"), QStringLiteral("Bee")}}));
    settle();
    check("a metadata-only edit is one dataChanged on its card",
          recorder.changedRows == QList<int>({1}) && recorder.moves == 0 && recorder.resets == 0
              && recorder.changedRoles.value(0).contains(SavedSystemCards::NameRole)
              && recorder.changedRoles.value(0).contains(SavedSystemCards::TitleRole));

    f.prefs.setSystemsSort(QStringLiteral("recent"));
    settle();
    f.cards->setScreenReaderActive(true);
    f.cards->setHoldReorders(true);
    recorder.clear();
    check("listen to C", f.systems->update(2, {{QStringLiteral("lastHeard"), 5000}}));
    settle();
    check("a background reorder is held on screen",
          recorder.resets == 0 && recorder.moves == 0 && f.at(2, SavedSystemCards::TitleRole) == QStringLiteral("C"));
    check("its content still changes", recorder.changedRows.contains(2));
    f.add({plain(QStringLiteral("D"), QStringLiteral("854.0"))});
    check("an insert still shows at once", f.cards->rowCount() == 4 && recorder.inserts == 1 && recorder.followed());
    f.cards->setHoldReorders(false);
    settle();
    check("released as one reset", recorder.resets == 1 && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("C"));

    f.cards->setHoldReorders(true);
    check("listen to A", f.systems->update(0, {{QStringLiteral("lastHeard"), 6000}}));
    settle();
    recorder.clear();
    f.cards->setQuery(QStringLiteral(" "));
    settle();
    check("the next user-asked change applies the held order as a reset",
          recorder.resets == 1 && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("A"));
}

/* R24: a held order (screen reader, background reorder) never starts a section twice. Xray moves from Iowa to Missouri
   and is held where it was; Alpha, new in Kansas, goes first (it has no card before it in the new order), so Kansas
   shows above and below Missouri: only its first card starts the section. */
void
test_held_order_starts_each_section_once() {
    Fixture f;
    const QVariantMap kansas = state(20, QStringLiteral("Kansas"));
    f.add({placed(plain(QStringLiteral("Xray"), QStringLiteral("851.0")), {state(19, QStringLiteral("Iowa"))}, {}),
           placed(plain(QStringLiteral("Yankee"), QStringLiteral("852.0")), {kansas}, {})});
    f.prefs.setSystemsSort(QStringLiteral("place"));
    settle();
    f.cards->setScreenReaderActive(true);
    f.cards->setHoldReorders(true);
    Recorder recorder(*f.cards);
    check("Xray moves to Missouri",
          f.systems->update(
              0, {{QStringLiteral("place"),
                   QVariantMap{{QStringLiteral("states"), QVariantList{state(29, QStringLiteral("Missouri"))}},
                               {QStringLiteral("counties"), QVariantList()}}}}));
    settle();
    check("the reorder is held", recorder.resets == 0 && recorder.moves == 0
                                     && f.column(SavedSystemCards::TitleRole)
                                            == QStringList({QStringLiteral("Xray"), QStringLiteral("Yankee")}));
    f.add({placed(plain(QStringLiteral("Alpha"), QStringLiteral("853.0")), {kansas}, {})});
    check("the new card shows at once, the order still held",
          recorder.resets == 0 && recorder.followed()
              && f.column(SavedSystemCards::TitleRole)
                     == QStringList({QStringLiteral("Alpha"), QStringLiteral("Xray"), QStringLiteral("Yankee")}));
    check("sections as held",
          f.column(SavedSystemCards::SectionRole)
              == QStringList({QStringLiteral("Kansas"), QStringLiteral("Missouri"), QStringLiteral("Kansas")}));
    check("Kansas starts once",
          f.column(SavedSystemCards::SectionStartRole)
              == QStringList({QStringLiteral("true"), QStringLiteral("true"), QStringLiteral("false")}));
    f.cards->setHoldReorders(false);
    settle();
    check("released: Kansas together, each section started once",
          f.column(SavedSystemCards::SectionRole)
                  == QStringList({QStringLiteral("Kansas"), QStringLiteral("Kansas"), QStringLiteral("Missouri")})
              && f.column(SavedSystemCards::SectionStartRole)
                     == QStringList({QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("true")}));
}

/* Only a state's own section is tiered by its county count; "Several states" and "No place" are in name order. Equal
   titles in one place are ordered by key. */
void
test_place_other_sections_by_name() {
    Fixture f;
    const QVariantMap iowa = state(19, QStringLiteral("Iowa"));
    const QVariantMap illinois = state(17, QStringLiteral("Illinois"));
    const QVariantMap johnson = county(1, 19, QStringLiteral("Johnson"));
    f.add({placed(plain(QStringLiteral("Zulu"), QStringLiteral("851.0")), {iowa, illinois}, {johnson}),
           placed(plain(QStringLiteral("Alpha"), QStringLiteral("852.0")), {iowa, illinois},
                  {johnson, county(2, 19, QStringLiteral("Linn")), county(3, 17, QStringLiteral("Cook"))}),
           placed(plain(QStringLiteral("Mike"), QStringLiteral("853.0")), {iowa, illinois}, {}),
           placed(plain(QStringLiteral("Yankee"), QStringLiteral("854.0")), {QVariantMap{{QStringLiteral("stid"), 5}}},
                  {county(9, 5, QStringLiteral("Named county"))}),
           plain(QStringLiteral("Bravo"), QStringLiteral("855.0")),
           placed(plain(QStringLiteral("Twin"), QStringLiteral("856.0")), {iowa}, {johnson}),
           placed(plain(QStringLiteral("Twin"), QStringLiteral("857.0")), {iowa}, {johnson})});
    f.prefs.setSystemsSort(QStringLiteral("place"));
    settle();
    check("several states and no place in name order, not by county count",
          f.column(SavedSystemCards::TitleRole)
              == QStringList({QStringLiteral("Twin"), QStringLiteral("Twin"), QStringLiteral("Alpha"),
                              QStringLiteral("Mike"), QStringLiteral("Zulu"), QStringLiteral("Bravo"),
                              QStringLiteral("Yankee")}));
    const QString twinA = f.uidOf(5);
    const QString twinB = f.uidOf(6);
    check("place: equal titles in one place by key", f.at(0, SavedSystemCards::KeyRole) == qMin(twinA, twinB)
                                                         && f.at(1, SavedSystemCards::KeyRole) == qMax(twinA, twinB));
}

/* Equal distances and titles, and equal stamps and titles, are ordered by key. */
void
test_tie_breaks_by_key() {
    Fixture f;
    f.add({positioned(site(QStringLiteral("Twin — A"), 501, 1, QStringLiteral("851.0"), QStringLiteral("A")), 42.0,
                      -93.6),
           positioned(site(QStringLiteral("Twin — A"), 500, 1, QStringLiteral("852.0"), QStringLiteral("A")), 42.0,
                      -93.6)});
    f.cards->setLocationSupported(true);
    set_fix(f.prefs, 41.6, -93.6);
    f.prefs.setSystemsSort(QStringLiteral("nearest"));
    settle();
    const QString first = qMin(f.systems->groupKey(0), f.systems->groupKey(1));
    check("nearest: equal distance and title by key",
          f.cards->effectiveSort() == QStringLiteral("nearest") && f.at(0, SavedSystemCards::KeyRole) == first);

    Fixture g;
    g.add({plain(QStringLiteral("Same"), QStringLiteral("851.0")),
           plain(QStringLiteral("Same"), QStringLiteral("852.0"))});
    check("stamp both", g.systems->update(0, {{QStringLiteral("lastHeard"), 4000}})
                            && g.systems->update(1, {{QStringLiteral("lastHeard"), 4000}}));
    g.prefs.setSystemsSort(QStringLiteral("recent"));
    settle();
    check("last listened: equal stamp and title by key",
          g.at(0, SavedSystemCards::KeyRole) == qMin(g.uidOf(0), g.uidOf(1)));
}

/* Rows A..C named so the name order is the reverse of their distance order from 41.6, -93.6. */
void
add_distance_rows(Fixture& f) {
    f.add({positioned(site(QStringLiteral("Charlie — S"), 100, 1, QStringLiteral("851.0"), QStringLiteral("S")), 41.7,
                      -93.6),
           positioned(site(QStringLiteral("Bravo — S"), 200, 1, QStringLiteral("852.0"), QStringLiteral("S")), 42.6,
                      -93.6),
           positioned(site(QStringLiteral("Alpha — S"), 300, 1, QStringLiteral("853.0"), QStringLiteral("S")), 43.6,
                      -93.6)});
}

/* R15: only the fix the user asked for is a user-asked change. */
void
test_location_request_mark() {
    Fixture f;
    add_distance_rows(f);
    f.cards->setLocationSupported(true);
    f.prefs.setSystemsSort(QStringLiteral("nearest"));
    settle();
    check("no fix yet: name order", f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Alpha"));
    f.cards->setScreenReaderActive(true);
    f.cards->setHoldReorders(true);
    Recorder recorder(*f.cards);
    const qint64 now = dsd_qt::realtimeMSecsSinceEpoch();
    f.prefs.setLocationFix(41.6, -93.6, now - 60000, 10);
    settle();
    check("a fix nobody asked for here is held on screen",
          recorder.resets == 0 && recorder.moves == 0
              && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Alpha"));
    f.cards->markLocationRequested();
    f.prefs.setLocationFix(41.6, -93.6, now, 10); // newer than the fix at the mark
    settle();
    check("the fix the user asked for applies as a reset",
          recorder.resets == 1 && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Charlie"));
    recorder.clear();
    f.prefs.setLocationFix(0, 0, 0);
    settle();
    check("the mark is spent: an expiry is a background change, held",
          recorder.resets == 0 && recorder.moves == 0
              && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Charlie"));
    check("and it goes back to name order", f.cards->effectiveSort() == QStringLiteral("name"));
}

/* The mark is spent by the location change it was for, even when another user-asked change is pending in the same
   turn; a later expiry is then a background change. A new fix at the same moment but another place re-measures. */
void
test_location_mark_spent_with_other_changes() {
    Fixture f;
    add_distance_rows(f);
    f.cards->setLocationSupported(true);
    f.prefs.setSystemsSort(QStringLiteral("nearest"));
    settle();
    f.cards->setScreenReaderActive(true);
    f.cards->setHoldReorders(true);
    Recorder recorder(*f.cards);
    f.cards->markLocationRequested();
    f.cards->setQuery(QStringLiteral(" ")); // a user-asked change pending in the same turn
    const qint64 at = dsd_qt::realtimeMSecsSinceEpoch();
    f.prefs.setLocationFix(41.6, -93.6, at, 10);
    settle();
    check("the asked-for fix resets to distance order",
          recorder.resets == 1 && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Charlie"));
    recorder.clear();
    f.prefs.setLocationFix(0, 0, 0);
    settle();
    check("the mark was spent: a later expiry is held", recorder.resets == 0 && recorder.moves == 0);
    f.cards->setScreenReaderActive(false);
    f.prefs.setLocationFix(41.6, -93.6, at, 10);
    settle();
    const double before = f.at(0, SavedSystemCards::DistanceKmRole).toDouble();
    f.prefs.setLocationFix(43.6, -93.6, at, 10);
    settle();
    const int alpha = f.cards->rowForKey(f.systems->groupKey(2));
    check("a fix at the same moment in another place re-measures",
          alpha >= 0 && f.at(alpha, SavedSystemCards::DistanceKmRole).toDouble() < 1.0 && before > 1.0);
}

/* A fix can lapse with no locationChanged: AppPrefs drops it lazily, at the first read 24 wall-clock hours on, and its
   expiry timer runs on the monotonic clock, which stops while a phone sleeps. The next rebuild, for whatever reason,
   notices it: the distances measured from it go and the order falls back to name. Here the stored stamp is moved back
   behind AppPrefs, and a search (which changes no card) is the rebuild. */
void
test_lapsed_fix_is_dropped_at_the_next_rebuild() {
    Fixture f;
    add_distance_rows(f);
    f.cards->setLocationSupported(true);
    f.prefs.setSystemsSort(QStringLiteral("nearest"));
    set_fix(f.prefs, 41.6, -93.6);
    settle();
    check("measured from the fix", f.cards->effectiveSort() == QStringLiteral("nearest")
                                       && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Charlie")
                                       && f.at(0, SavedSystemCards::DistanceKmRole).toDouble() >= 0);
    QSettings(QSettings::IniFormat, QSettings::UserScope, AppPrefs::settingsOrganization(),
              AppPrefs::settingsApplication())
        .setValue(QStringLiteral("location/lastFixAt"), dsd_qt::realtimeMSecsSinceEpoch() - 25LL * 60 * 60 * 1000);
    f.cards->setQuery(QStringLiteral("S"));
    settle();
    check("the lapsed fix falls back to name order",
          f.cards->effectiveSort() == QStringLiteral("name")
              && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Alpha"));
    bool measured = false;
    for (int row = 0; row < f.cards->rowCount(); ++row) {
        measured = measured || f.at(row, SavedSystemCards::DistanceKmRole).toDouble() >= 0
                   || !f.at(row, SavedSystemCards::DistanceStateRole).toString().isEmpty();
    }
    check("no card keeps a distance from it", !measured);
}

/* R17: a location request that ends without a fix (failed, cancelled, superseded) gives its mark back, so a later
   fix nobody asked for here is a background change, held on screen. */
void
test_location_request_cleared() {
    Fixture f;
    add_distance_rows(f);
    f.cards->setLocationSupported(true);
    f.prefs.setSystemsSort(QStringLiteral("nearest"));
    settle();
    f.cards->setScreenReaderActive(true);
    f.cards->setHoldReorders(true);
    Recorder recorder(*f.cards);
    f.cards->markLocationRequested();
    f.cards->clearLocationRequest(); // the request failed
    set_fix(f.prefs, 41.6, -93.6);
    settle();
    check("a fix after a cleared request is held",
          recorder.resets == 0 && recorder.moves == 0
              && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Alpha"));
}

/* An old fix that lapses while the user's request is pending is no answer to it: the expiry (here AppPrefs' lazy one,
   at a read, and then the explicit clear) stays a background change, held, and the mark waits for the fix the request
   brings, which applies as a reset. */
void
test_location_mark_survives_an_expiry() {
    Fixture f;
    add_distance_rows(f);
    f.cards->setLocationSupported(true);
    f.prefs.setSystemsSort(QStringLiteral("nearest"));
    const qint64 now = dsd_qt::realtimeMSecsSinceEpoch();
    f.prefs.setLocationFix(41.6, -93.6, now - 60000, 10);
    settle();
    check("mark survives an expiry: measured from the old fix",
          f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Charlie"));
    f.cards->setScreenReaderActive(true);
    f.cards->setHoldReorders(true);
    Recorder recorder(*f.cards);
    f.cards->markLocationRequested();
    QSettings(QSettings::IniFormat, QSettings::UserScope, AppPrefs::settingsOrganization(),
              AppPrefs::settingsApplication())
        .setValue(QStringLiteral("location/lastFixAt"), now - 25LL * 60 * 60 * 1000);
    check("mark survives an expiry: the read drops the fix", f.prefs.lastFixAt() == 0);
    settle(); // the queued locationChanged of the lazy expiry
    check("mark survives an expiry: the lazy expiry is held",
          recorder.resets == 0 && recorder.moves == 0
              && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Charlie"));
    f.prefs.setLocationFix(0, 0, 0);
    settle();
    check("mark survives an expiry: and so is a cleared fix", recorder.resets == 0 && recorder.moves == 0);
    recorder.clear();
    f.prefs.setLocationFix(43.6, -93.6, now, 10); // the answer to the request
    settle();
    check("mark survives an expiry: the answer applies as a reset",
          recorder.resets == 1 && f.cards->effectiveSort() == QStringLiteral("nearest")
              && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Alpha")
              && f.at(0, SavedSystemCards::DistanceKmRole).toDouble() >= 0
              && f.at(0, SavedSystemCards::DistanceKmRole).toDouble() < 1.0);
    recorder.clear();
    f.prefs.setLocationFix(41.6, -93.6, now + 1000, 10);
    settle();
    check("mark survives an expiry: spent by the answer, a later fix is held",
          recorder.resets == 0 && recorder.moves == 0
              && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Alpha"));
}

/* A held reorder applies as moves once the screen reader is off; a background reorder with one on and no hold is a
   reset. */
void
test_reader_off_and_unheld_background() {
    Fixture f;
    add_distance_rows(f);
    f.prefs.setSystemsSort(QStringLiteral("recent"));
    settle();
    f.cards->setScreenReaderActive(true);
    f.cards->setHoldReorders(true);
    Recorder recorder(*f.cards);
    check("listen to Charlie", f.systems->update(0, {{QStringLiteral("lastHeard"), 7000}}));
    settle();
    check("held", recorder.resets == 0 && recorder.moves == 0
                      && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Alpha"));
    f.cards->setScreenReaderActive(false);
    settle();
    check("screen reader off: the held reorder applies as moves",
          recorder.resets == 0 && recorder.moves > 0 && recorder.followed()
              && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Charlie"));
    recorder.clear();
    f.cards->setScreenReaderActive(true);
    f.cards->setHoldReorders(false);
    check("listen to Bravo", f.systems->update(1, {{QStringLiteral("lastHeard"), 8000}}));
    settle();
    check("a background reorder with a screen reader and no hold is a reset",
          recorder.resets == 1 && recorder.moves == 0
              && f.at(0, SavedSystemCards::TitleRole) == QStringLiteral("Bravo"));
}

void
test_coalescing() {
    Fixture f;
    QVariantList rows;
    for (int i = 0; i < 100; ++i) {
        rows.append(plain(QStringLiteral("System %1").arg(i), QStringLiteral("851.%1").arg(i)));
    }
    f.add(rows);
    const int before = f.cards->rebuildCount();
    for (int i = 0; i < 100; ++i) {
        (void)f.systems->update(i, {{QStringLiteral("lastHeard"), 1000 + i}});
    }
    check("nothing rebuilt inside the turn", f.cards->rebuildCount() == before);
    settle();
    check("100 changes in one turn, one rebuild", f.cards->rebuildCount() == before + 1);
    check("and every card has its stamp", f.at(99, SavedSystemCards::LastHeardRole).toLongLong() == 1099);
}

/* Random renames, filters and sorts: a view that follows only the signals always ends where the model is. */
void
test_signals_follow_random_changes() {
    Fixture f;
    QVariantList rows;
    for (int i = 0; i < 40; ++i) {
        rows.append(plain(QStringLiteral("S%1").arg((i * 17) % 40), QStringLiteral("851.%1").arg(i)));
    }
    f.add(rows);
    f.prefs.setSystemsSort(QStringLiteral("name"));
    settle();
    Recorder recorder(*f.cards);
    quint32 seed = 12345;
    const auto next = [&seed](int bound) {
        seed = seed * 1103515245U + 12345U;
        return static_cast<int>((seed >> 16) % static_cast<quint32>(bound));
    };
    bool followed = true;
    for (int round = 0; round < 40; ++round) {
        for (int edit = 0; edit < 3; ++edit) {
            (void)f.systems->update(next(40), {{QStringLiteral("name"), QStringLiteral("S%1").arg(next(60))}});
        }
        if (round % 5 == 0) {
            f.cards->setQuery(round % 10 == 0 ? QStringLiteral("1") : QString());
        }
        settle();
        followed = followed && recorder.followed();
    }
    check("a view following the signals stays in step", followed && recorder.resets == 0);
}

void
test_refresh_hint() {
    IdleHost host;
    const QString library = ImportedFilesModel::libraryRoot();
    (void)QDir().mkpath(library);
    const QString rrPath = library + QStringLiteral("/rr-chan.csv");
    const QString pickedPath = library + QStringLiteral("/picked-chan.csv");
    for (const QString& path : {rrPath, pickedPath}) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write("1,851.0125\n") < 0) {
            check("write a library file", false);
        }
    }
    QJsonObject rr{{QStringLiteral("name"), QStringLiteral("rr-chan.csv")},
                   {QStringLiteral("path"), rrPath},
                   {QStringLiteral("type"), QStringLiteral("chan")},
                   {QStringLiteral("origin"), QStringLiteral("radioreference")},
                   {QStringLiteral("rrSid"), 100}};
    QJsonObject picked{{QStringLiteral("name"), QStringLiteral("picked-chan.csv")},
                       {QStringLiteral("path"), pickedPath},
                       {QStringLiteral("type"), QStringLiteral("chan")}};
    check("seed the library",
          dsd_qt::json_store_save_array(QStringLiteral("imported_files.json"), QJsonArray{rr, picked}));
    ImportedFilesModel files(&host);
    check("the library loaded", files.rowForPath(rrPath) >= 0);
    Fixture f(&files);
    QVariantMap fromRr = plain(QStringLiteral("From RR"), QStringLiteral("851.0"));
    fromRr.insert(QStringLiteral("chanCsvPath"), rrPath);
    QVariantMap fromPicked = plain(QStringLiteral("Picked"), QStringLiteral("852.0"));
    fromPicked.insert(QStringLiteral("chanCsvPath"), pickedPath);
    QVariantMap conventional = fromRr;
    conventional.insert(QStringLiteral("trunking"), false);
    QVariantMap grouped = site(QStringLiteral("Grouped — A"), 100, 1, QStringLiteral("853.0"), QStringLiteral("A"));
    grouped.insert(QStringLiteral("chanCsvPath"), rrPath);
    f.add({fromRr, fromPicked, conventional, grouped});
    check("a trunked ungrouped row on a RadioReference file asks for a refresh",
          f.column(SavedSystemCards::RefreshHintRole)
              == QStringList(
                  {QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("false"), QStringLiteral("false")}));
    QFile::remove(rrPath);
    QFile::remove(pickedPath);
    (void)dsd_qt::json_store_save_array(QStringLiteral("imported_files.json"), QJsonArray());
}

/* 500 rows in 300 cards: a reverse sort and a search keystroke, measured to a settled model. */
void
test_timing() {
    Fixture f;
    QVariantList rows;
    for (int group = 0; group < 100; ++group) {
        for (int s = 1; s <= 3; ++s) {
            rows.append(site(QStringLiteral("Group %1 — Site %2").arg(999 - group).arg(s), 1000 + group, s,
                             QStringLiteral("85%1.%2").arg(s).arg(group), QStringLiteral("Site %1").arg(s)));
        }
    }
    for (int i = 0; i < 200; ++i) {
        rows.append(plain(QStringLiteral("System %1").arg(899 - i), QStringLiteral("460.%1").arg(i)));
    }
    f.add(rows);
    check("500 rows, 300 cards", f.systems->count() == 500 && f.cards->rowCount() == 300);
    Recorder recorder(*f.cards);
    QElapsedTimer timer;
    timer.start();
    f.prefs.setSystemsSort(QStringLiteral("name"));
    settle();
    const qint64 sortMs = timer.elapsed();
    check("the reverse sort followed", recorder.followed() && recorder.resets == 0);
    timer.restart();
    f.cards->setQuery(QStringLiteral("Group 95"));
    settle();
    const qint64 searchMs = timer.elapsed();
    check("the keystroke filtered: Group 950-959 and 995", f.cards->shownCount() == 11 && recorder.followed());
    f.systems->touch(0); // its save is the saved-systems model's, not measured here
    timer.restart();
    settle();
    const qint64 touchMs = timer.elapsed();
    DSD_FPRINTF(stderr,
                "timing (500 rows, 300 cards): reverse sort %lld ms (%d moves), search keystroke %lld ms, "
                "rebuild after one touch %lld ms\n",
                static_cast<long long>(sortMs), recorder.moves, static_cast<long long>(searchMs),
                static_cast<long long>(touchMs));
    if (qEnvironmentVariableIntValue("DSD_TEST_TIMING") == 1) {
        check("reverse sort under 50 ms", sortMs < 50);
        check("search keystroke under 50 ms", searchMs < 50);
        check("a rebuild after one change under 50 ms", touchMs < 50);
    }
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-saved-system-cards-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        DSD_FPRINTF(stderr, "FAIL: could not create settings dir\n");
        return 1;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    test_one_card_per_group();
    test_b1_group_last_heard();
    test_b2_display_row();
    test_b13_profile_key_configured();
    test_titles_and_pins();
    test_orders();
    test_place_sections();
    test_nearest();
    test_edited_row_unplaced();
    test_search();
    test_search_adapter();
    test_search_stored_frequency_spellings();
    test_sort_signals();
    test_metadata_edit_and_hold();
    test_held_order_starts_each_section_once();
    test_place_other_sections_by_name();
    test_tie_breaks_by_key();
    test_location_request_mark();
    test_location_mark_spent_with_other_changes();
    test_location_request_cleared();
    test_location_mark_survives_an_expiry();
    test_lapsed_fix_is_dropped_at_the_next_rebuild();
    test_reader_off_and_unheld_background();
    test_coalescing();
    test_signals_follow_random_changes();
    test_refresh_hint();
    test_timing();

    if (g_failures > 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "saved system cards: all checks passed\n");
    return 0;
}
