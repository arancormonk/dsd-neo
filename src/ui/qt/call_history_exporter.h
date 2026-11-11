// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief History's Export… (context property `historyExport`): the rows a History view shows, as a CSV file.
 *
 * A press prepare()s the whole file at once from the rows the screen's filter view shows, in its order, and holds it
 * under a token (PendingExport) while QML asks the user where to save it. The picker's answer is save() or discard().
 * Both History screens share one exporter, so a second prepare replaces the first's file. GUI thread only.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_EXPORTER_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_EXPORTER_H_

#include <QObject>
#include <QString>
#include <QTimeZone>
#include <QVariantMap>
#include <functional>
#include <memory>

namespace dsd_qt {

class CallHistoryModel;
class DecoderHost;
class DocumentSaveTransport;
class PendingExport;

class CallHistoryExporter : public QObject {
    Q_OBJECT
    /** An accepted save has not finished yet: a new one would be refused. */
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

  public:
    /**
     * @brief The app's exporter of @p model: saves through @p host's save transport (a base transport without a
     *        host), stages under `<CacheLocation>/history-export`, and writes times in the device's current time zone.
     */
    CallHistoryExporter(const DecoderHost* host, CallHistoryModel* model, QObject* parent);
    /**
     * @brief An exporter of @p model that saves through @p transport, stages under @p cacheRoot
     *        (DocumentExportWriter), and writes times in the zone @p zone gives, asked once per prepare().
     */
    CallHistoryExporter(std::shared_ptr<DocumentSaveTransport> transport, const QString& cacheRoot,
                        CallHistoryModel* model, std::function<QTimeZone()> zone, QObject* parent);
    /** A save still running is aborted and waited for, within DocumentExportWriter::kStopWaitMs. */
    ~CallHistoryExporter() override;

    CallHistoryExporter(const CallHistoryExporter&) = delete;
    CallHistoryExporter& operator=(const CallHistoryExporter&) = delete;

    /**
     * @brief Build the file of the rows @p view shows now, in its order, and hold it as the one pending export.
     *
     * Any file held before is dropped first, also when this then holds none.
     *
     * @return `{token, count, total}`: a token above 0, the rows written and every row History holds;
     *         `{token 0, count 0}` when the view shows no row; `{token 0, count -1}` when @p view is not a
     *         CallHistoryFilterModel over this exporter's model.
     */
    Q_INVOKABLE QVariantMap prepare(QObject* view);
    /**
     * @brief Save the pending file @p token into the document @p reference names, in the background; finished()
     *        tells how it went.
     *
     * Refused (false) for a token that is 0 or not the pending one, and while a save runs; a refused save lets go of
     * the reference's Download-folder row, and the pending file is used up either way.
     */
    Q_INVOKABLE bool save(int token, const QString& reference);
    /** @brief Drop the pending file @p token (a cancelled picker); another token is left alone. */
    Q_INVOKABLE void discard(int token);

    bool busy() const;

  Q_SIGNALS:
    void busyChanged();
    /** @brief The accepted save of @p token ended, written whole (@p ok) or not; busy() is already false. */
    void finished(int token, bool ok);

  private:
    CallHistoryModel* m_model;
    std::function<QTimeZone()> m_zone;
    std::unique_ptr<PendingExport> m_export;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_CALL_HISTORY_EXPORTER_H_
