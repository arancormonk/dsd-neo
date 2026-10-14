// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_QT_KNOWN_FREQUENCY_INDEX_H
#define DSD_NEO_QT_KNOWN_FREQUENCY_INDEX_H
#include <QFileSystemWatcher>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

namespace dsd_qt {
class SavedSystemsModel;
class ScanListsModel;

class KnownFrequencyIndex : public QObject {
    Q_OBJECT
    Q_PROPERTY(qulonglong revision READ revision NOTIFY changed)
  public:
    explicit KnownFrequencyIndex(SavedSystemsModel* systems, ScanListsModel* lists, QObject* parent = nullptr);
    Q_INVOKABLE QVariantList lookup(double frequency, double width = 12500) const;
    /** Names saved for the channel tuned at @p frequency, filtered @p channelWidth wide (MetricsModel's
        channelBandwidthHz); 0, when the input publishes no width, takes a 12.5 kHz channel. */
    Q_INVOKABLE QStringList tunedNames(double frequency, double channelWidth) const;
    void rebuild();

    qulonglong
    revision() const {
        return m_revision;
    }

  Q_SIGNALS:
    void changed();

  private:
    struct Entry {
        qint64 hz = 0;
        double width = 0;
        QVariantMap value;
    };

    QVector<Entry> m_entries;
    qulonglong m_revision = 0;
    QFileSystemWatcher m_files;
    SavedSystemsModel* m_systems;
    ScanListsModel* m_lists;
    void add(const QVariantMap& s, const QString& label);
    void addCsv(const QString& path, const QVariantMap& s, const QString& label, double width, int depth = 0);
};
} // namespace dsd_qt
#endif
