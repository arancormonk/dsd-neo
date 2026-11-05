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

/**
 * @brief Every frequency the app has saved, found by the channel a receiver is tuned to.
 *
 * Each match is a map: `name` (as the scan header names it), `frequencyHz` (the saved row's own frequency, not the one
 * tuned), `systemUid` (the saved system's uid, a scan-list entry's uid, or the list's uid for a target CSV row), and
 * what it is (issue #644):
 * - `source`: "system" (a saved system's own frequency), "scanEntry" (a scan list's entry, a saved system's entry at
 *   that system's frequency included), "scanTarget" (a row of a list's imported target CSV) or "channelMap" (a row of a
 *   saved system's or a trunked target's channel map);
 * - `listUid`: the scan list a scanEntry, a scanTarget or a target's channelMap row belongs to, else empty;
 * - `targetId`: the id the list's session names the row by (the entry's uid, the CSV row's `id`), else empty;
 * - `selectable`: whether the match can be started with its own settings: a saved system, or a scan list's row that
 *   becomes a target when the list starts (an enabled entry of a list that is no draft, a CSV row with an id). A
 *   channel map's row only names the channel;
 * - `protocolLabel`: the protocol of a saved system, an entry or a target as the scan list editor names it ("NXDN48"),
 *   which tells two matches of one name apart; empty for a channel map's row and a saved system with no decode flag.
 */
class KnownFrequencyIndex : public QObject {
    Q_OBJECT
    Q_PROPERTY(qulonglong revision READ revision NOTIFY changed)
  public:
    explicit KnownFrequencyIndex(SavedSystemsModel* systems, ScanListsModel* lists, QObject* parent = nullptr);
    Q_INVOKABLE QVariantList lookup(double frequency, double width = 12500) const;
    /** The matches for the channel tuned at @p frequency, filtered @p channelWidth wide (MetricsModel's
        channelBandwidthHz); 0, when the input publishes no width, takes a 12.5 kHz channel. */
    Q_INVOKABLE QVariantList tunedMatches(double frequency, double channelWidth) const;
    /** The names of tunedMatches(), each once. */
    Q_INVOKABLE QStringList tunedNames(double frequency, double channelWidth) const;
    /**
     * The targets a running scan-list session scans, read from the target CSVs it started with (issue #644): @p lists
     * holds the session's lists in list order, each {uid, name, path} (path: ScanListStarter::sessionCsvPath()). One
     * map per target row with an id: `listNumber` (1-based), `listUid`, `targetId`, `frequencyHz`, `width` (the
     * channel width its mode names, as the index takes it), `name` ("<list name> / <label>") and `protocolLabel`. A
     * list whose file cannot be read gives none. Reads the files on each call: for a session's start or its lists
     * changing, not per frame.
     */
    Q_INVOKABLE QVariantList sessionTargets(const QVariantList& lists) const;
    /** The names of @p matches, each once, in their order: two rows of one name on one channel are named once. */
    static QStringList matchNames(const QVariantList& matches);
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
    /* @p s's own frequency, as @p identity says what it is, then its channel map. */
    void add(const QVariantMap& s, const QString& label, const QVariantMap& identity);
    /* Scan list @p listUid's @p entry, named under @p listName: a frequency entry, or a saved system's entry at that
       system's frequency; selectable when the list is @p startable and the entry enabled. */
    void addListEntry(const QVariantMap& entry, const QString& listName, const QString& listUid, bool startable);
    /* The rows of @p path: a list's target CSV when @p identity's source is "scanTarget", else a channel map. A
       target's own channel map is read one level deep. */
    void addCsv(const QString& path, const QVariantMap& s, const QString& label, double width,
                const QVariantMap& identity, int depth = 0);
};
} // namespace dsd_qt
#endif
