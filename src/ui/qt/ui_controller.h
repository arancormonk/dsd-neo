// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The UI's single poll driver and the object graph QML binds to.
 *
 * Binding threading contract: every snapshot-backed read in the process happens on
 * this timer, on the Qt main thread. The app-control consume accessors hand back a
 * shared buffer after dropping their lock, so a second concurrent reader sees rows
 * mid-overwrite. One polling thread, no exceptions.
 */

#ifndef DSD_NEO_SRC_UI_QT_UI_CONTROLLER_H_
#define DSD_NEO_SRC_UI_QT_UI_CONTROLLER_H_

#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include "decoder_host.h"

namespace dsd_qt {

class CallHistoryModel;
class MetricsModel;
class DiagnosticsLogModel;
class TalkgroupListModel;

class UiController : public QObject {
    Q_OBJECT
    // WP-D1: copied retained outcome, polled even without decoder redraw.
    Q_PROPERTY(QVariantMap talkgroupExportResult READ talkgroupExportResult NOTIFY talkgroupExportResultChanged)
    Q_PROPERTY(int pollIntervalMs READ pollIntervalMs WRITE setPollIntervalMs NOTIFY pollIntervalChanged)

  public:
    UiController(DecoderHost* host, MetricsModel* metrics, CallHistoryModel* history, TalkgroupListModel* talkgroups,
                 QObject* parent = nullptr);
    ~UiController() override;

    QVariantMap
    talkgroupExportResult() const {
        return m_talkgroupExportResult;
    }

    int pollIntervalMs() const;
    void setPollIntervalMs(int interval_ms);

    // WP-F5: process diagnostics refresh even when decoder redraw is idle.
    void
    setDiagnosticsLog(DiagnosticsLogModel* model) {
        m_diagnostics = model;
    }

    void start();
    void stop();

    /**
     * @brief Ingest the latest snapshot into the call history right now.
     *
     * For the one moment ordering matters: startSystem() is about to change the
     * session label, and any backlog the previous session committed since the
     * last tick must be attributed to that session, not the incoming one. Runs
     * on the Qt main thread like every other snapshot read, and leaves the
     * redraw flag alone so the regular tick still fires.
     */
    Q_INVOKABLE void flushHistory();

  Q_SIGNALS:
    void pollIntervalChanged();
    void talkgroupExportResultChanged();

  private:
    void tick();
    void onSessionStateChanged();
    void clearLiveModels();

    DecoderHost* m_host = nullptr;
    MetricsModel* m_metrics = nullptr;
    CallHistoryModel* m_history = nullptr;
    TalkgroupListModel* m_talkgroups = nullptr;
    DiagnosticsLogModel* m_diagnostics = nullptr;
    QTimer m_timer;
    QVariantMap m_talkgroupExportResult;
    quint64 m_talkgroupExportSequence = 0;
    unsigned int m_active_ordinal = 0;
    DecoderHost::SessionState m_session = DecoderHost::Idle;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_UI_CONTROLLER_H_ */
