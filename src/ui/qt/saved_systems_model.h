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
#include <QString>
#include <QVariantMap>

namespace dsd_qt {

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
        PpmRole,          // INT_MIN sentinel avoided; stored as string, empty = default
        BandwidthKhzRole, // -1 = default
        BiasTeeRole,
        ExtraArgsRole,
        FilePathRole,
        LastHeardRole // seconds since epoch; 0 = never
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

    /** @brief Row most recently heard, or 0 when the list is non-empty but unheard. */
    Q_INVOKABLE int mostRecentRow() const;

  Q_SIGNALS:
    void countChanged();

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
        bool biasTee = false;
        QString extraArgs;
        QString filePath;
        qint64 lastHeard = 0;
    };

    static Row rowFromMap(const QVariantMap& map, const Row& base);
    static QVariant identityRoleValue(const Row& row, int role);
    static QVariant tuningRoleValue(const Row& row, int role);
    QVariantMap mapFromRow(const Row& row) const;

    void load();
    void save() const;
    QString storePath() const;

    QList<Row> m_rows;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SAVED_SYSTEMS_MODEL_H_ */
