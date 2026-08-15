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
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>

namespace dsd_qt {

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
        PpmRole,          // INT_MIN sentinel avoided; stored as string, empty = default
        BandwidthKhzRole, // -1 = default
        BiasTeeRole,      // tri-state: -1 = follow the app-wide pref, 0 = off, 1 = on
        ExtraArgsRole,
        FilePathRole,
        LastHeardRole,    // seconds since epoch; 0 = never
        ChanCsvPathRole,  // imported channel map (-C); empty = none
        GroupCsvPathRole, // imported group list (-G); empty = none
        KeyCsvPathRole,   // imported key file (-k/-K); empty = none
        KeyCsvHexRole     // true = hex keys (-K), false = decimal (-k)
    };

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
    Q_INVOKABLE void add(const QVariantMap& system);

    /** @brief Replace one system's fields. Unknown keys are ignored. */
    Q_INVOKABLE void update(int row, const QVariantMap& system);

    Q_INVOKABLE void remove(int row);

    /** @brief One system as a field map, for the wizard's edit path and argv building. */
    Q_INVOKABLE QVariantMap get(int row) const;

    /** @brief Stamp lastHeard = now; called when a session on this system starts. */
    Q_INVOKABLE void touch(int row);

    /**
     * @brief Row most recently heard, or 0 when the list is non-empty but unheard.
     *
     * A NOTIFY property, not an invokable: the home screen's featured play button
     * binds to it, and an invokable in a binding would never re-evaluate.
     */
    int mostRecentRow() const;

    /** @brief Names of systems whose CSV fields reference @p path; for delete warnings. */
    Q_INVOKABLE QStringList systemsReferencingPath(const QString& path) const;

    /**
     * @brief Blank every CSV field matching @p path, in every system. Persists.
     *
     * Called when a library file is removed: a saved system pointing at a
     * deleted file would otherwise fail its next start with an opaque
     * "configure failed".
     */
    Q_INVOKABLE void clearCsvPath(const QString& path);

  Q_SIGNALS:
    void countChanged();
    void mostRecentRowChanged();

  private:
    struct Row {
        QString name;
        QString sourceType;
        QString host;
        int port = 0;
        QString freqMhz;
        QString decodeFlag;
        bool trunking = true;
        int gainDb = -1;
        QString ppm;
        int bandwidthKhz = -1;
        /* -1 follows the app-wide pref, 0 is explicitly off, 1 explicitly on. An
         * explicit off must survive a global on: it is the operator saying this
         * dongle or antenna must not be fed the tee's 4.5 V. */
        int biasTee = -1;
        QString extraArgs;
        QString filePath;
        qint64 lastHeard = 0;
        QString chanCsvPath;
        QString groupCsvPath;
        QString keyCsvPath;
        bool keyCsvHex = false;
    };

    static Row rowFromMap(const QVariantMap& map, const Row& base);
    static QVariant identityRoleValue(const Row& row, int role);
    static QVariant tuningRoleValue(const Row& row, int role);
    QVariantMap mapFromRow(const Row& row) const;

    void load();
    void save() const;

    QList<Row> m_rows;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SAVED_SYSTEMS_MODEL_H_ */
