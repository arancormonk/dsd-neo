// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Saved radio systems the home screen lists, persisted as JSON.
 *
 * A "system" is everything one tap needs to start listening: the input spec, the
 * decode selection, trunking, and any per-system tuner overrides. Persistence is a
 * single JSON file in the app data directory, small and human-recoverable. Lists
 * grow to hundreds of rows (a per-site RadioReference import adds one per site),
 * and load() brings older rows up to the current rules in place, without schema
 * versions.
 */

#ifndef DSD_NEO_SRC_UI_QT_SAVED_SYSTEMS_MODEL_H_
#define DSD_NEO_SRC_UI_QT_SAVED_SYSTEMS_MODEL_H_

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <functional>
#include <type_traits> // IWYU pragma: keep -- QPointer's conversion checks.

namespace dsd_qt {
class ReplayStore;

class SavedSystemsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        SourceTypeRole, // "usb" | "rtltcp" | "udp" | "tcp" | "file"
        HostRole,
        PortRole,
        FreqMhzRole, // e.g. "851.375"; empty for PCM/file sources
        DecodeFlagRole,
        TrunkingRole,
        GainDbRole,       // -1 = use the app-wide default
        HangtimeRole,     // seconds as a string; empty = app default
        PpmRole,          // INT_MIN sentinel avoided; stored as string, empty = default
        BandwidthKhzRole, // -1 = default
        BiasTeeRole,      // tri-state: -1 = follow the app-wide pref, 0 = off, 1 = on
        ExtraArgsRole,
        FilePathRole,
        LastHeardRole,          // seconds since epoch; 0 = never
        ChanCsvPathRole,        // imported channel map (-C); empty = none
        GroupCsvPathRole,       // imported group list (-G); empty = none
        KeyCsvPathRole,         // imported key file (-k/-K); empty = none
        KeyCsvHexRole,          // true = hex keys (-K), false = decimal (-k)
        P25BandplanCsvPathRole, // imported P25 band plan (--p25-bandplan); empty = none
        SrcCsvPathRole,         // imported radio ID aliases (--src-csv); empty = none
        UidRole,
        EncKeyTypeRole,
        EncForceKeyRole,
        RrSidRole,
        RrSiteIdRole,
        SiteNameRole,
        SiteLatRole,
        SiteLonRole,
        HasSitePosRole,
        AvoidSiteRole,
        AirspyRole,
        PlaceRole,        // RadioReference place, validPlace()'s form; empty = none
        KeyConfiguredRole // a direct key, a key file or a decryption profile: whether one is set, never which
    };

    /**
     * @brief @p raw as a stored place, or an empty map when it is none or breaks the schema anywhere.
     *
     * A place is `{states: [{stid, name?}], counties: [{ctid, stid?, name?}], candidateStates: [stid]}`: RadioReference
     * ids, whole numbers (not text) in 1..INT32_MAX; names text of at most 128 characters (an empty one is left out);
     * at most 64 states, 64 candidate states and 512 counties. Entries repeating an id merge into the first, which takes
     * only what it lacked. Unknown keys are ignored. A place with no entry at all is none.
     */
    static QVariantMap validPlace(const QVariant& raw);

    /** Fills names and county links a place lacks from what is already known (no network); may leave it as it was. */
    using PlaceNameFill = std::function<void(QVariantMap& place)>;
    /** @brief The fill add() and update() run on a place whose ids are new, before they save. Empty = none. */
    void setPlaceNameCache(PlaceNameFill fill);

    /** Private-store secret access for session argument assembly only. Not invokable from QML.
     * QString copies cannot promise erasure. Never log or export the returned value. */
    QString keyValueForUid(const QString& uid) const;

    /**
     * @brief The replay copies' owner (#832): add() and update() adopt a copy a row gains before they save, update()
     * and remove() release one no row names any more after they saved. Both save a cached file the migration moved
     * this session as its copy (ReplayStore::migratedPath()). Without one, file paths are only stored.
     */
    void setReplayStore(ReplayStore* replays);

    /** @brief Whether saved_systems.json read cleanly, or was absent, at load: what a deletion on its contents needs. */
    bool
    storeLoaded() const {
        return m_storeLoaded;
    }

    /** @brief Every non-empty filePath the systems hold (a replay's file), each once. */
    QStringList filePaths() const;

    /** @brief A row to point at a new replay file, while it still names the old one. */
    struct FilePathMove {
        QString uid;
        QString from;
        QString to;
    };

    /**
     * @brief Point each row @p moves names by uid at its new file, when its filePath is still the old one. Persists
     * first; the replay store is not told (the caller owns the copies).
     *
     * @param applied Receives every new path a row now names (empty when nothing moved or the save failed).
     * @return false when the save failed; nothing changed then.
     */
    bool moveFilePaths(const QList<FilePathMove>& moves, QStringList* applied);

    /**
     * @brief @p candidate in the form this model stores a UID (a brace-less UUID), or empty when it is no UUID.
     *
     * Loading adopts a stored UID through this and replaces one it rejects; a restored backup is held to it too.
     */
    static QString canonicalUid(const QString& candidate);

    explicit SavedSystemsModel(QObject* parent = nullptr);
    ~SavedSystemsModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return static_cast<int>(m_rows.size());
    }

    /** @brief Append a system from the wizard's field map. Persists immediately. */
    Q_INVOKABLE bool add(const QVariantMap& system);

    /** Append with a retained private key, resolved by stable source UID. No key
     * value crosses the QML boundary. Returns false without adding a row when
     * the source/key is absent, the type differs, or a replacement value is supplied.
     * QString copies retain the private store's existing erasure limitations. */
    Q_INVOKABLE bool addWithKeyFrom(const QString& sourceUid, const QVariantMap& system);

    /**
     * @brief Append every system in @p systems (a per-site import's rows), all in one save, or none of them.
     *
     * Every row is checked first as add() checks it, and, when @p keySourceUid names a row, takes that row's retained
     * key as addWithKeyFrom() would; an empty @p keySourceUid takes keys from the maps, as add() does. Each row gets
     * a fresh UID, its replay copy is adopted before the save (a copy adopted before a save that fails stays kept, as
     * add()'s does), and its place is filled from the place-name cache. One rowsInserted range follows the save.
     *
     * @return The first inserted row, or -1 when nothing was inserted (an empty list, a refused row or key, a replay
     *         copy that could not be adopted, or a failed save); no signal then.
     */
    Q_INVOKABLE int addAll(const QVariantList& systems, const QString& keySourceUid);

    /**
     * @brief Replace one system's fields. Unknown keys are ignored. Persists first; false leaves the row as it was.
     *
     * A changed frequency, decode flag or CSV field no longer describes the imported site: the row's stored
     * RadioReference provenance and geography are cleared (rr_provenance_cleared(): its group, its avoid, which only
     * the group's site chooser could lift, its site name, position and place). What @p system itself carries of
     * rrSid, rrSiteId, siteName, siteLat, siteLon, hasSitePos and place is then applied, validated: an edit-mode
     * re-import keeps its fresh identity. A changed siteName there renames a generated "base — oldSite" name.
     */
    Q_INVOKABLE bool update(int row, const QVariantMap& system);

    /** Persist confirmed Radio controls by stable identity, preserving unrelated wizard fields. */
    Q_INVOKABLE bool updateRadioSettings(const QString& uid, const QVariantMap& changes);

    /**
     * @brief Remove one system. Persists first: on a failed save it returns false, keeps the row and signals nothing.
     *
     * A saved removal signals rowsAboutToBeRemoved/rowsRemoved, then sitesChanged and countChanged, so whatever
     * follows rows out of the model sees only removals that will hold.
     */
    Q_INVOKABLE bool remove(int row);

    /** @brief The field names whose change in update() clears a row's RadioReference provenance. */
    Q_INVOKABLE QStringList tuningProvenanceFields() const;
    /** @brief One system as a field map, for the wizard's edit path and argv building. */
    Q_INVOKABLE QVariantMap get(int row) const;
    /** @brief Row @p row's site group key (site_group_key(): its RadioReference system on its receiver), read from
     *         its fields; empty for a row in no group or out of range. */
    QString groupKey(int row) const;
    /** @brief The rows of @p row's site group (groupKey()), in row order; empty for a row in no group. nearestRow()
     *         and siteCount() read the same group. */
    Q_INVOKABLE QVariantList siblingRows(int row) const;
    Q_INVOKABLE int nearestRow(int row, double lat, double lon) const;
    Q_INVOKABLE int siteCount(int row) const;
    Q_INVOKABLE double distanceKm(int row, double lat, double lon) const;
    /** @brief Mark a grouped site avoided or not; false when the row is unknown or the save failed. */
    Q_INVOKABLE bool setAvoidSite(int row, bool avoid);
    Q_INVOKABLE int rowForUid(const QString& uid) const;
    Q_INVOKABLE QVariantMap getByUid(const QString& uid) const;

    /**
     * @brief Stamp lastHeard = now; called when a session on this system starts.
     *
     * Persists first, and a failed save leaves lastHeard as it was. Recency is advisory, so nothing is reported.
     */
    Q_INVOKABLE void touch(int row);

    /**
     * @brief The real clock lastHeard is stamped from, in ms since the Unix epoch.
     *
     * For QML that ages lastHeard ("Last listened 2 minutes ago"): the age of the viewer's own
     * listening is real time, and reading it here keeps the stamp and the age on one clock.
     */
    Q_INVOKABLE qint64 realtimeNowMs() const;

    /** @brief Names of systems whose CSV fields reference @p path; for delete warnings. */
    Q_INVOKABLE QStringList systemsReferencingPath(const QString& path) const;

    /** @brief Every non-empty path the systems' CSV fields hold, each once: what clearCsvPath() can clear. */
    QStringList csvPaths() const;

    /**
     * @brief Blank every CSV field matching @p path, in every system. Persists first.
     *
     * Called when a library file is removed: a saved system pointing at a
     * deleted file would otherwise fail its next start with an opaque
     * "configure failed". A row that loses a file loses its RadioReference
     * provenance and geography too (rr_provenance_cleared()). One dataChanged
     * spans the changed rows. Returns false, with every row unchanged, when the
     * save fails, and true when nothing referenced @p path.
     */
    Q_INVOKABLE bool clearCsvPath(const QString& path);

    /**
     * @brief Clear the RadioReference provenance and geography of every system whose CSV fields name @p path, keeping
     * the path. Persists first.
     *
     * Called before the user's own bytes replace a library file in place: the file no longer describes the site the
     * rows came from. Returns false, with every row unchanged, when the save fails; true when nothing names @p path.
     */
    Q_INVOKABLE bool invalidatePathProvenance(const QString& path);

    /**
     * @brief RadioReference's names for state @p stid and its counties, applied to every row whose place names those
     * ids: the state's name, each listed county's name and its link to @p stid, and @p stid in the row's states once
     * one of its counties links to it. Metadata only, one save, one dataChanged spanning the changed rows.
     *
     * @return false when the save failed (every row as it was); true otherwise, nothing to change included.
     */
    bool applyPlaceNames(int stid, const QString& stateName, const QHash<int, QString>& countyNames);

    /** @brief applyPlaceNames()'s rule on one place (a cache fill's): true when @p place changed. */
    static bool namePlace(QVariantMap& place, int stid, const QString& stateName,
                          const QHash<int, QString>& countyNames);

    /** @brief Whether anything in @p place still lacks a name or, for a county, its state link; or it has candidate
     *         states and no state yet. */
    static bool placeNeedsNames(const QVariantMap& place);

    /** @brief One saved site's fresh RadioReference record, for refreshSites(). */
    struct SiteRefresh {
        QString uid;
        int rrSid = 0;
        int rrSiteId = 0;
        /** What the record says now: siteName, siteLat, siteLon, hasSitePos and place, each optional. */
        QVariantMap fields;
    };

    /**
     * @brief Update from RadioReference's fresh records, applied to every row a refresh names by uid that is still
     * the site @p rrSid / @p rrSiteId it describes (a row edited onto another site meanwhile is left alone).
     *
     * Metadata only, as update() takes them without a tuning change: the site name (a changed one renames a generated
     * "base — oldSite" name), the position, the place (filled from the place-name cache when its ids are new). One
     * save, then one dataChanged spanning the changed rows and sitesChanged.
     *
     * @return The rows that were still the site they describe (changed or not), or -1 when the save failed (every
     *         row as it was, no signal).
     */
    int refreshSites(const QList<SiteRefresh>& refreshes);

  Q_SIGNALS:
    void sitesChanged();
    void countChanged();

  private:
    struct Row {
        QVariantMap airspy;
        QVariantMap radioSettings;

        // UUIDs survive row deletion/reordering; a pending start must never touch
        // whichever unrelated row moved into its old index.
        QString uid;
        QString encKeyType;
        QString encKeyValue;
        int encForceKey = 0;
        int rrSid = 0;
        int rrSiteId = 0;
        QString siteName;
        double siteLat = 0;
        double siteLon = 0;
        bool hasSitePos = false;
        bool avoidSite = false;
        QVariantMap place; // validPlace()'s form
        // QString copies cannot promise secret erasure. Key values belong only to
        // private persistence and argv assembly, never labels or diagnostics.
        QString name;
        QString sourceType;
        QString host;
        int port = 0;
        QString freqMhz;
        QString decodeFlag;
        /* False, like every other layer reads an absent key: session_args'
         * `system.value("trunking").toBool()` appends -T only for a present
         * true, and the wizard starts a new system with it off. A default of
         * true here meant a map that never mentioned trunking - a hand-edited
         * or partially restored systems.json row, or a future caller that
         * simply forgot the key - was stored as call-following ON while every
         * reader of that same map said OFF. */
        bool trunking = false;
        int gainDb = -1;
        QString ppm;
        QString hangtime;
        int bandwidthKhz = -1;
        /* -1 follows the app-wide pref, 0 is explicitly off, 1 explicitly on. An
         * explicit off must survive a global on: it is the operator saying this
         * dongle or antenna must not be fed the tee's 4.5 V. */
        int biasTee = -1;
        QString extraArgs;
        QString filePath;
        QString decryptionProfileUid;
        qint64 lastHeard = 0;
        QString chanCsvPath;
        QString groupCsvPath;
        QString keyCsvPath;
        bool keyCsvHex = false;
        QString p25BandplanCsvPath;
        QString srcCsvPath;
    };

    static void fillRowKey(const QVariantMap& map, Row& row);
    static void fillRowDetails(const QVariantMap& map, Row& row);
    static Row rowFromMap(const QVariantMap& map, const Row& base);
    static QVariant identityRoleValue(const Row& row, int role);
    static QVariant detailRoleValue(const Row& row, int role);
    static QVariant tuningRoleValue(const Row& row, int role);
    QVariantMap mapFromRow(const Row& row) const;
    /** Load's normalization of a stored row; true when it changed something the store should save. */
    static bool normalizeLoaded(Row& row);
    /** @p system with row @p source's retained key (addWithKeyFrom()'s rule) in @p fields; false when it cannot be. */
    bool takeRetainedKey(int source, const QVariantMap& system, QVariantMap* fields) const;
    /** addAll()'s new row from @p system: checked, a fresh UID, its replay path and place filled; false = refused. */
    bool newRow(const QVariant& system, int keySource, Row* row) const;
    /** Adopt @p added's replay copies, save the rows with it appended, insert it as one range: its first row or -1. */
    int appendRows(const QList<Row>& added);
    /** Keep @p previous's names and links for the same ids in @p row's place, then run the place-name fill on it when
     *  its ids differ from @p previous's or it still lacks a name. */
    void fillPlaceNames(Row& row, const QVariantMap& previous) const;
    /** Clear the provenance of every row with a CSV field naming @p path, blanking those fields too if asked. */
    bool clearPathProvenance(const QString& path, bool clearFields);
    /** Install @p next, saved, and signal one dataChanged from @p first to @p last. */
    void commitChangedRange(QList<Row>&& next, int first, int last, const QList<int>& roles);

    void load();
    bool save() const;
    bool saveRows(const QList<Row>& rows) const;
    /** Adopt @p path before a save names it; false when the replay store could not (nothing may be saved). */
    bool adoptReplay(const QString& path);
    /** @p path, or the copy the replay migration moved it to this session (a draft opened before it finished). */
    QString migratedReplay(const QString& uid, const QString& path) const;
    /** After a saved change: release @p path when no row names it any more. */
    void releaseReplayIfUnused(const QString& path);

    QList<Row> m_rows;
    QPointer<ReplayStore> m_replays;
    PlaceNameFill m_placeNames;
    bool m_storeLoaded = true;
    /* The one real clock, in ms since the Unix epoch, that touch() stamps lastHeard from and realtimeNowMs() ages it
       against. */
    qint64 (*m_realtime_now_ms)();
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SAVED_SYSTEMS_MODEL_H_ */
