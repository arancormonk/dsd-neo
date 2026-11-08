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
 * single JSON file in the app data directory — small, human-recoverable, and free
 * of schema migrations for a list that rarely exceeds a handful of rows.
 */

#ifndef DSD_NEO_SRC_UI_QT_SAVED_SYSTEMS_MODEL_H_
#define DSD_NEO_SRC_UI_QT_SAVED_SYSTEMS_MODEL_H_

#include <QAbstractListModel>
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
#include <type_traits> // IWYU pragma: keep -- QPointer's conversion checks.

namespace dsd_qt {
class ReplayStore;

class SavedSystemsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int mostRecentRow READ mostRecentRow NOTIFY mostRecentRowChanged)

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
        AirspyRole
    };

    /** Private-store secret access for session argument assembly only. Not invokable from QML.
     * QString copies cannot promise erasure. Never log or export the returned value. */
    QString keyValueForUid(const QString& uid) const;

    /**
     * @brief The replay copies' owner (#832): add() and update() adopt a copy a row gains before they save, update()
     * and remove() release one no row names any more after they saved. Without one, file paths are only stored.
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
     * @brief Replace one system's fields. Unknown keys are ignored. Persists first; false leaves the row as it was.
     *
     * A changed frequency, decode flag or CSV field takes an imported site out of its group (rrSid and rrSiteId
     * zeroed) and drops its avoid, which only the group's site chooser could lift.
     */
    Q_INVOKABLE bool update(int row, const QVariantMap& system);

    /** Persist confirmed Radio controls by stable identity, preserving unrelated wizard fields. */
    Q_INVOKABLE bool updateRadioSettings(const QString& uid, const QVariantMap& changes);

    /**
     * @brief Remove one system. Persists first: on a failed save it returns false, keeps the row and signals nothing.
     *
     * A saved removal signals rowsAboutToBeRemoved/rowsRemoved, then sitesChanged, countChanged and
     * mostRecentRowChanged, so whatever follows rows out of the model sees only removals that will hold.
     */
    Q_INVOKABLE bool remove(int row);

    /** @brief One system as a field map, for the wizard's edit path and argv building. */
    Q_INVOKABLE QVariantMap get(int row) const;
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

    /**
     * @brief Row most recently heard, or 0 when the list is non-empty but unheard.
     *
     * A NOTIFY property, not an invokable: the home screen's featured play button
     * binds to it, and an invokable in a binding would never re-evaluate.
     */
    int mostRecentRow() const;

    /** @brief Names of systems whose CSV fields reference @p path; for delete warnings. */
    Q_INVOKABLE QStringList systemsReferencingPath(const QString& path) const;

    /** @brief Every non-empty path the systems' CSV fields hold, each once: what clearCsvPath() can clear. */
    QStringList csvPaths() const;

    /**
     * @brief Blank every CSV field matching @p path, in every system. Persists first.
     *
     * Called when a library file is removed: a saved system pointing at a
     * deleted file would otherwise fail its next start with an opaque
     * "configure failed". A site that loses a file leaves its group and its
     * avoid. Returns false, with every row unchanged, when the save fails, and
     * true when nothing referenced @p path.
     */
    Q_INVOKABLE bool clearCsvPath(const QString& path);

  Q_SIGNALS:
    void sitesChanged();
    void countChanged();
    void mostRecentRowChanged();

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

    void load();
    bool save() const;
    bool saveRows(const QList<Row>& rows) const;
    /** Adopt @p path before a save names it; false when the replay store could not (nothing may be saved). */
    bool adoptReplay(const QString& path);
    /** After a saved change: release @p path when no row names it any more. */
    void releaseReplayIfUnused(const QString& path);

    QList<Row> m_rows;
    QPointer<ReplayStore> m_replays;
    bool m_storeLoaded = true;
    /* The one real clock, in ms since the Unix epoch, that touch() stamps lastHeard from and realtimeNowMs() ages it
       against. */
    qint64 (*m_realtime_now_ms)();
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SAVED_SYSTEMS_MODEL_H_ */
