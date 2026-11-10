// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_QT_SIGNAL_SEARCH_MODEL_H
#define DSD_NEO_QT_SIGNAL_SEARCH_MODEL_H
#include <QByteArray>
// QVariantList's template instantiations need QList in some consumers.
#include <QList> // IWYU pragma: keep
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <dsd-neo/runtime/signal_search.h>
#include <memory>
#include <stdint.h>

namespace dsd_qt {
class DecoderHost;
class DocumentExportWriter;
class DocumentSaveTransport;
class KnownFrequencyIndex;

class SignalSearchModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList ranges READ ranges NOTIFY rangesChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QVariantList avoids READ avoids NOTIFY avoidsChanged)
    Q_PROPERTY(QVariantList sessionAvoids READ sessionAvoids NOTIFY avoidsChanged)
    Q_PROPERTY(QVariantMap status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    /** The Signal search store's files that could not be read or saved, worded for the screen: one line each. */
    Q_PROPERTY(QString storeMessage READ storeMessage NOTIFY storeChanged)
    /** The Signal search store's files, {file, state, kept, writeFailed}: state "unreadable", "kept", "leftInPlace" (the
     * version 1 journal, never moved) or "" (read whole, or missing). Forwarded to StoreNotices for Home's card. */
    Q_PROPERTY(QVariantList storeFiles READ storeFiles NOTIFY storeChanged)
  public:
    explicit SignalSearchModel(DecoderHost* host, KnownFrequencyIndex* index, QObject* parent = nullptr);
    ~SignalSearchModel() override;

    QVariantList
    ranges() const {
        return m_ranges;
    }

    QVariantList
    history() const {
        return m_history;
    }

    QVariantList
    avoids() const {
        return m_avoids;
    }

    QVariantList
    sessionAvoids() const {
        return m_sessionAvoids;
    }

    QVariantMap
    status() const {
        return m_status;
    }

    /** This model's own refusal or failure. What the store could not read or save is storeMessage(), never here. */
    QString
    error() const {
        return m_error;
    }

    QString
    storeMessage() const {
        return m_storeMessage;
    }

    QVariantList
    storeFiles() const {
        return m_storeFiles;
    }

    /**
     * @brief The range the model loads from a stored row, in the shape it stores ranges.
     * @return false when loading would drop the row.
     */
    static bool storedRangeRow(const QVariantMap& stored, QVariantMap* range);
    Q_INVOKABLE QVariantMap defaultRange() const;
    Q_INVOKABLE QString saveRange(int row, const QVariantMap& value);
    Q_INVOKABLE void removeRange(int row);
    Q_INVOKABLE void moveRange(int row, int direction);
    Q_INVOKABLE QString arm();
    Q_INVOKABLE void cancelArm();
    Q_INVOKABLE bool control(int action, const QString& session, const QString& hit);
    /** Avoid a history row's frequency for the running session @p session (DSD_SEARCH_ACTION_AVOID_FREQUENCY). The
     * decoder's verdict, a full list included, arrives in the status message. */
    Q_INVOKABLE bool avoidFrequency(const QString& session, double frequency);
    /**
     * @brief Avoid a history row's frequency permanently.
     *
     * Only the store changes, here, with or without a search: a running search takes the permanent list at its next
     * tick and leaves the frequency if it is visiting it. A refusal (a full list, no frequency) is this model's error.
     */
    Q_INVOKABLE bool avoidPermanently(double frequency);
    /** Remove a permanent avoid from the store, here; a running search lifts it at its next tick. */
    Q_INVOKABLE void removeAvoid(double frequency);
    Q_INVOKABLE bool removeSessionAvoid(double frequency);
    Q_INVOKABLE QVariantMap systemForHit(const QVariantMap& hit, const QVariantMap& source) const;

    /**
     * @brief Take the rows to export now (#826) and keep the file built from them as the one pending export.
     *
     * @p kind is "detections" (every stored detection, newest first) or "avoids" (the permanent avoids, then the shown
     * session's). Any call first drops the export pending before it. The file is built here, from the store's
     * snapshots, so a list that changes afterwards does not change it.
     *
     * @return {token, count}: the pending export's token (above 0) and its row count; {0, 0} when there are no rows,
     *         {0, -1} for an unknown kind or avoid lists that kept changing while read. A 0 token keeps nothing.
     */
    Q_INVOKABLE QVariantMap prepareExport(const QString& kind);
    /**
     * @brief Save the pending export @p token, in the background, into the document a picker chose (@p reference).
     *
     * Either way the export is no longer pending. exportFinished() tells the outcome of an accepted save.
     *
     * @return false, with the reference's Download-folder row let go of, when @p token is not the pending export or a
     *         save is still running.
     */
    Q_INVOKABLE bool saveExport(int token, const QString& reference);
    /** Drop the pending export @p token (its picker was cancelled); another token is left alone. */
    Q_INVOKABLE void discardExport(int token);
    void refresh();
  Q_SIGNALS:
    void rangesChanged();
    void changed();
    void historyChanged();
    void avoidsChanged();
    void storeChanged();
    /** An accepted saveExport()'s outcome, on this model's thread. */
    void exportFinished(int token, bool ok);

  private:
    DecoderHost* m_host;
    KnownFrequencyIndex* m_index;
    QVariantList m_ranges, m_history, m_avoids, m_sessionAvoids;
    QVariantMap m_status;
    QString m_error, m_storeMessage;
    QVariantList m_storeFiles;
    QTimer m_timer;
    dsd_signal_search_config m_pending{};
    /* A start waiting for the decoder to take it, and whether the session it waits for has been seen up. */
    bool m_armed = false, m_armSessionSeen = false;
    uint64_t m_revision = UINT64_MAX, m_session = 0;
    uint64_t m_historyRevision = UINT64_MAX, m_avoidsRevision = UINT64_MAX;
    bool m_owned = false;
    void followSession();
    bool armSessionUp() const;
    void submitArm();
    void disarm();
    bool persistRanges(const QVariantList& list);
    bool refreshAvoids();
    bool refreshHistory();
    bool refreshStoreFiles();
    bool unavoid(uint32_t frequency);
    QVariantMap hitMap(const dsd_signal_search_hit& h) const;
    /** The saved frequencies a detection at @p frequency matches at @p width (matchWidth()): hitMap()'s `matches`. */
    QVariantList knownMatches(uint32_t frequency, double width) const;
    QByteArray detectionsExport(int* count) const;
    QByteArray avoidsExport(int* count) const;

    /* The export's save: the host's transport, kept for a refused save's Download row, and the writer that stages and
       saves an accepted one. Both live as long as the model, which lives as long as the QML engine. */
    std::shared_ptr<DocumentSaveTransport> m_saveTransport;
    std::unique_ptr<DocumentExportWriter> m_exportWriter;
    QByteArray m_exportBytes;
    int m_exportToken = 0, m_lastExportToken = 0;
};
} // namespace dsd_qt
#endif
