// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Listen's saved-system cards: one per site group (system and receiver) or ungrouped row, searched and sorted.
 *
 * SavedSystemsModel keeps its rows in the order they were added, and Home, Main and the wizard address them by that
 * row. This model is the view over it: one card per site_group_key() group, or per row in no group, standing on the
 * group's lowest row (its lead). A card carries what its face shows, already worked out for the whole group:
 *
 * - the display row: the one site Play starts when exactly one is not avoided, else the lead. The card's name,
 *   source, frequency, decode and key fields are that row's, so what the card says is what Play starts;
 * - lastHeard is the group's latest; keyConfigured is a direct key, a key file or a decryption profile on the display
 *   row (a boolean, never the key or profile);
 * - title is site_group_title() of the lead, as the card's Listen pin names it;
 * - placeLabel, section and the distance fields follow the members' places and positions.
 *
 * Search (query) is AND over system_search_terms(): a term matches the folded text of the members' names, site
 * names, place names and protocol label (system_search_text()), or any member's frequency (system_search_frequency()).
 * Orders are the stored AppPrefs::systemsSort(), each ending on the card key: name, place (sections by state, then
 * "Several states", then "No place"), nearest (by distance to the location fix; then "All sites avoided", then
 * "No coordinates"; name without location support or a fix, the pref unchanged), recent (Last listened, never-listened
 * last) and added (the lead's row).
 *
 * Every source change is coalesced into one queued rebuild, which diffs the shown cards by key into removes, moves,
 * inserts and one dataChanged per card whose roles changed, so surviving delegates stay. Android's accessibility
 * bridge learns of a move only through a hide and show, so while screenReaderActive a reorder is one model reset; a
 * reorder nobody asked for (an import, a lastHeard stamp) is held while holdReorders (Home visible on Systems), and
 * applied as a reset when that ends or at the next user-asked change. Inserts, removes and content changes apply at
 * once. Nothing here holds a source row across a rebuild: QML resolves uids at click time.
 */

#ifndef DSD_NEO_SRC_UI_QT_SAVED_SYSTEM_CARDS_H_
#define DSD_NEO_SRC_UI_QT_SAVED_SYSTEM_CARDS_H_

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QModelIndex>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <type_traits> // IWYU pragma: keep -- QPointer's conversion checks.

namespace dsd_qt {
class AppPrefs;
class ImportedFilesModel;
class SavedSystemsModel;

class SavedSystemCards : public QAbstractListModel {
    Q_OBJECT
    /* The search text; empty shows every card. */
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    /* The stored order (AppPrefs::systemsSort()), as the sort control names it. */
    Q_PROPERTY(QString sortId READ sortId NOTIFY sortIdChanged)
    /* The order applied: sortId, except "name" for "nearest" without location support or a location fix. */
    Q_PROPERTY(QString effectiveSort READ effectiveSort NOTIFY effectiveSortChanged)
    Q_PROPERTY(int shownCount READ shownCount NOTIFY shownCountChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY totalCountChanged)
    Q_PROPERTY(
        bool screenReaderActive READ screenReaderActive WRITE setScreenReaderActive NOTIFY screenReaderActiveChanged)
    /* True while Home is visible on its Systems tab: a reorder nobody asked for waits (screen reader only). */
    Q_PROPERTY(bool holdReorders READ holdReorders WRITE setHoldReorders NOTIFY holdReordersChanged)
    /* The protocol label search reads for each decodeFlag (Util.decodeLabel()'s), the "" entry for any other flag. */
    Q_PROPERTY(QVariantMap protocolLabels READ protocolLabels WRITE setProtocolLabels NOTIFY protocolLabelsChanged)

  public:
    enum Roles {
        KeyRole = Qt::UserRole + 1, // site_group_key(), or the row's uid for a row in no group
        LeadUidRole,
        PlayUidRole, // the row Play starts: the one site not avoided, the row itself in no group; else empty
        TitleRole,
        SiteCountRole, // the group's sites; 0 in no group
        NameRole,      // NameRole..KeyConfiguredRole: the display row's
        SourceTypeRole,
        HostRole,
        PortRole,
        FreqMhzRole,
        DecodeFlagRole,
        TrunkingRole,
        FilePathRole,
        KeyConfiguredRole,
        LastHeardRole, // the group's latest, seconds since the epoch; 0 = never
        RefreshHintRole,
        PlaceLabelRole,
        DistanceKmRole,    // the nearest member not avoided with a position; -1 = none or no fix
        DistanceStateRole, // "positioned" | "noCoordinates" | "allAvoided"; "" without a fix
        SectionRole,       // the section header text in place and nearest orders; "" otherwise
        SectionStartRole,  // the first shown card of its (non-empty) section
        RrLinkedRole       // a member is a RadioReference site or has a place: Update from RadioReference applies
    };

    SavedSystemCards(SavedSystemsModel* systems, AppPrefs* prefs, ImportedFilesModel* files, QObject* parent = nullptr);
    ~SavedSystemCards() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString
    query() const {
        return m_query;
    }

    void setQuery(const QString& text);
    QString sortId() const;

    QString
    effectiveSort() const {
        return m_effectiveSort;
    }

    int
    shownCount() const {
        return static_cast<int>(m_shown.size());
    }

    int
    totalCount() const {
        return m_totalCount;
    }

    bool
    screenReaderActive() const {
        return m_screenReaderActive;
    }

    void setScreenReaderActive(bool active);

    bool
    holdReorders() const {
        return m_holdReorders;
    }

    void setHoldReorders(bool hold);

    QVariantMap
    protocolLabels() const {
        return m_protocolLabels;
    }

    void setProtocolLabels(const QVariantMap& labels);

    /** @brief Whether this platform can take a location fix (DecoderHost::locationSupported()); Nearest needs it. */
    void setLocationSupported(bool supported);

    /**
     * @brief The next location change is the user's own (Nearest's Use or Update location): call right before asking
     *        for a fix. That change then counts as user-asked (a reorder resets under a screen reader); any other
     *        (a 24 h expiry, a fix another screen took) is a background change.
     */
    Q_INVOKABLE void markLocationRequested();

    /** @brief The request markLocationRequested() was for ended without a fix (it failed, was cancelled or was
     *         superseded): the next location change is a background change again. */
    Q_INVOKABLE void clearLocationRequest();

    /** @brief The shown row of the card keyed @p key, or -1. */
    Q_INVOKABLE int rowForKey(const QString& key) const;

    /** @brief How many rebuilds ran (one per coalesced batch of changes); for tests. */
    int
    rebuildCount() const {
        return m_rebuildCount;
    }

  Q_SIGNALS:
    void queryChanged();
    void sortIdChanged();
    void effectiveSortChanged();
    void shownCountChanged();
    void totalCountChanged();
    void screenReaderActiveChanged();
    void holdReordersChanged();
    void protocolLabelsChanged();

  public:
    /* The data a card is built from and holds; public for the comparisons and role reads in the source file. */

    /** One source row as the cards read it. */
    struct Member {
        QString uid;
        QString groupKey;
        QString name;
        QString siteName;
        QString sourceType;
        QString host;
        QString freqMhz;
        QString decodeFlag;
        QString filePath;
        QString chanCsvPath;
        QString groupCsvPath;
        QVariantMap place;
        qint64 lastHeard = 0;
        double siteLat = 0;
        double siteLon = 0;
        int row = -1;
        int port = 0;
        int rrSid = 0;
        int rrSiteId = 0;
        bool trunking = false;
        bool avoidSite = false;
        bool hasSitePos = false;
        bool keyConfigured = false;
    };

    /** One card: its role values, and what search and the orders read. */
    struct Card {
        QString key;
        QString leadUid;
        QString playUid;
        QString title;
        QString name;
        QString sourceType;
        QString host;
        QString freqMhz;
        QString decodeFlag;
        QString filePath;
        QString placeLabel;
        QString distanceState;
        QString section;
        QString haystack;       // system_search_text() of the members' texts, lines joined by '\n'
        QList<qint64> hz;       // the members' frequencies
        QString placeState;     // the one state's name, when the place names exactly one
        QString placeCounty;    // the one county's name, when the place names exactly one
        int placeKind = 2;      // 0 one state, 1 several states, 2 no place
        int placeCounties = 0;  // named counties
        double distanceKm = -1; // the nearest member not avoided with a position
        qint64 lastHeard = 0;
        int leadRow = -1;
        int siteCount = 0;
        int port = 0;
        int nameRank = 0; // the title's place in name order (its fold sorted once, not per comparison)
        int stateRank = 0;
        int countyRank = 0;
        bool trunking = false;
        bool keyConfigured = false;
        bool refreshHint = false;
        bool sectionStart = false;
        bool rrLinked = false;
    };

  private:
    /** A row's folded search text, kept by uid while what it is made of stays the same. */
    struct RowText {
        QString name;
        QString siteName;
        QString freqMhz;
        QVariantMap place;
        QString folded;
        qint64 hz = 0;
    };

    /** The location fix one rebuild measures from, read once (valid only with location support). */
    struct Fix {
        bool valid = false;
        qint64 at = 0;
        double lat = 0;
        double lon = 0;
    };

    void scheduleRebuild();
    void rebuild();
    Fix readFix() const;
    void collectCards(const Fix& fix);
    QList<Member> readMembers();
    Card buildCard(const QList<const Member*>& group, const QHash<QString, bool>& rrFiles, const Fix& fix) const;
    void placeCard(Card& card, const QList<const Member*>& group) const;
    static void locateCard(Card& card, const QList<const Member*>& group, const Fix& fix);
    /** @p member's search text and frequency, from the cache while what they are made of is unchanged; kept in @p next. */
    RowText memberText(const Member& member, QHash<QString, RowText>& next) const;
    QHash<QString, bool> radioReferenceFiles() const;
    void sortCards(const Fix& fix);
    QString applicableSort(const Fix& fix) const;
    QList<Card> filteredCards() const;
    void publish(QList<Card>&& next, bool userAsked);
    QList<Card> heldOrder(QList<Card>&& next) const;
    void applyDiff(QList<Card>&& next);
    void removeGone(const QHash<QString, int>& nextIndex);
    void moveSurvivors(const QHash<QString, int>& nextIndex);
    void insertNew(const QList<Card>& next);
    void updateContent(const QList<Card>& next);
    void resetTo(QList<Card>&& next);

    QPointer<SavedSystemsModel> m_systems;
    QPointer<AppPrefs> m_prefs;
    QPointer<ImportedFilesModel> m_files;
    QList<Card> m_all;   // every card, in source order
    QList<int> m_order;  // m_all's indices in the applied order
    QList<Card> m_shown; // what the view shows, in order
    QHash<QString, RowText> m_rowTexts;
    QHash<QString, int> m_nameRanks;  // each card title's place in name order
    QHash<QString, int> m_placeRanks; // each state and county name's
    QVariantMap m_protocolLabels;
    QString m_query;
    QString m_effectiveSort;
    Fix m_fix; // the fix the cards' distances were measured from
    int m_totalCount = 0;
    int m_rebuildCount = 0;
    bool m_locationSupported = false;
    bool m_screenReaderActive = false;
    bool m_holdReorders = false;
    bool m_rebuildPending = false;
    bool m_cardsDirty = true;
    bool m_orderDirty = true;
    bool m_userAsked = false;
    bool m_reorderHeld = false;
    bool m_locationRequested = false; // markLocationRequested() until the next location change
};

} // namespace dsd_qt

/* Plain values (implicitly shared Qt containers, numbers): a reorder moves them as bytes. */
Q_DECLARE_TYPEINFO(dsd_qt::SavedSystemCards::Card, Q_RELOCATABLE_TYPE);

#endif /* DSD_NEO_SRC_UI_QT_SAVED_SYSTEM_CARDS_H_ */
