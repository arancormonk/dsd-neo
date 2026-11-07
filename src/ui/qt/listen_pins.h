// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The shortcuts pinned to Listen (issue #801), resolved against what is saved.
 *
 * The pins themselves are AppPrefs' `listen/pinned`: a kind and a UID each, in the order they were pinned. This object
 * names them from the saved systems and scan lists, says whether each is found, and pins, unpins and prunes them. A
 * pin whose target is missing stays stored (a store that failed to load reads as empty, and comes back); only a
 * removal the model has saved drops one.
 */

#ifndef DSD_NEO_SRC_UI_QT_LISTEN_PINS_H_
#define DSD_NEO_SRC_UI_QT_LISTEN_PINS_H_

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include "app_prefs.h"

namespace dsd_qt {
class SavedSystemsModel;
class ScanListsModel;

class ListenPins : public QObject {
    Q_OBJECT
    /* Every stored pin in order, each {key, kind, uid, label, accessibleName, found}: key is "<kind>:<uid>" (uid empty
     * for "search" and "explore"). A pin not found keeps the last label it had this run (else empty) and no
     * accessible name; the views name it as missing. */
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool full READ full NOTIFY fullChanged)
    /* Moves on every change of entries and on every change of either model's rows, even one that leaves entries as
     * they were: whether a card is pinned follows its group (isPinned()), which no entry shows. A binding that calls
     * isPinned() reads this first, so it is evaluated again. */
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

  public:
    static constexpr int kCap = AppPrefs::kPinnedCap;

    /** @brief The text pins show and say, each in one place. */
    enum class Text {
        FullRefusal,     // pin() when kCap pins are stored
        NotFoundRefusal, // pin() of a target that is not saved
        SiteLabel,       // a site: its system's name, then the site's
        SearchLabel,
        ExploreLabel,
        StartName,      // a system or site that starts as it is tapped
        ChooseSiteName, // a grouped system with no site, or several, to start
        StartScanListName,
        OpenSearchName,
        StartExploreName,
        SetUpExploreName // Explore before its radio is chosen
    };

    /** @brief The translated text @p id, with @p label (and @p detail, a site's name) in it where it takes them. */
    static QString text(Text id, const QString& label = QString(), const QString& detail = QString());

    ListenPins(AppPrefs* prefs, SavedSystemsModel* systems, ScanListsModel* lists, QObject* parent = nullptr);
    ~ListenPins() override;

    QVariantList entries() const;
    int count() const;
    bool full() const;

    int
    revision() const {
        return m_revision;
    }

    /**
     * @brief Whether what @p kind and @p uid name shows as pinned.
     *
     * "site" and "scan" match their own pin. "system" matches a system pin of this row or of any site in its group,
     * and for a row in no group a site pin of the row as well: a site that left its group is that card. "search" and
     * "explore" match their pin, whatever @p uid.
     */
    Q_INVOKABLE bool isPinned(const QString& kind, const QString& uid) const;
    /** @brief Pin it last: "" when done or already pinned (isPinned()), else why not (Text::FullRefusal,
     *         Text::NotFoundRefusal). Never changes what Listen last started. */
    Q_INVOKABLE QString pin(const QString& kind, const QString& uid);
    /** @brief Remove every pin isPinned() matched for @p kind and @p uid. */
    Q_INVOKABLE void unpin(const QString& kind, const QString& uid);
    /** @brief Remove the pin whose entry key is @p key. */
    Q_INVOKABLE void unpinKey(const QString& key);

  Q_SIGNALS:
    void entriesChanged();
    void countChanged();
    void fullChanged();
    void revisionChanged();

  private:
    void refresh(bool topology);
    QVariantMap resolve(const QVariantMap& stored);
    QVariantMap resolveSaved(const QString& kind, const QString& uid) const;
    QVariantMap resolveScan(const QString& uid) const;
    QVariantMap resolveExplore() const;
    bool exists(const QString& kind, const QString& uid) const;
    QList<int> matching(const QVariantList& pins, const QString& kind, const QString& uid) const;
    QStringList groupUids(const QString& uid, bool* standalone) const;
    void rememberGroups();
    void modelChanged();
    void followLeavers();
    QVariantList withoutSystems(int first, int last) const;
    QVariantList withoutLists(int first, int last) const;
    void holdPrune(const QVariantList& kept);
    void rowsRemoved();
    void store(const QVariantList& pins);

    AppPrefs* m_prefs;
    SavedSystemsModel* m_systems;
    ScanListsModel* m_lists;
    QVariantList m_entries;
    // The label each key last resolved to this run, for a pin whose target has gone missing since.
    QHash<QString, QString> m_lastLabels;
    // Each saved row's RadioReference group (0 for none) by UID, as the last refresh saw it: how a system pin whose row
    // has since left its group is found.
    QHash<QString, int> m_groupOf;
    // The pins a removal leaves, worked out before the rows go and stored once they have.
    QVariantList m_pendingPins;
    bool m_prunePending = false;
    int m_revision = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_LISTEN_PINS_H_ */
