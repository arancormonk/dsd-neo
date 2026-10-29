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

// Complete types are needed by inline Qt container and metatype instantiations.
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <dsd-neo/core/state_fwd.h>
#include <type_traits> // IWYU pragma: keep

#include "decoder_host.h"

namespace dsd_qt {

class CallHistoryModel;
class MetricsModel;
class P25NetworkModel; // WP-F2
class DiagnosticsLogModel;
class TalkgroupListModel;
class CommandBridge;
class RecordingStatus;

class UiController : public QObject {
    Q_OBJECT
    // WP-S2: QML supplies its complete overlay/pending-permission gate.
    Q_PROPERTY(bool autoStartBlocked MEMBER m_autoStartBlocked)
    // WP-D1: copied retained outcome, polled even without decoder redraw.
    Q_PROPERTY(QVariantMap talkgroupExportResult READ talkgroupExportResult NOTIFY talkgroupExportResultChanged)
    Q_PROPERTY(QVariantMap decryptionResult READ decryptionResult NOTIFY decryptionResultChanged)
    Q_PROPERTY(int pollIntervalMs READ pollIntervalMs WRITE setPollIntervalMs NOTIFY pollIntervalChanged)

  public:
    UiController(DecoderHost* host, MetricsModel* metrics, CallHistoryModel* history, TalkgroupListModel* talkgroups,
                 QObject* parent = nullptr);
    ~UiController() override;
    void setCommandBridge(CommandBridge* commands);

    QVariantMap
    decryptionResult() const {
        return m_decryptionResult;
    }

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

    // WP-F2: sheet-only network refresh uses the same held snapshot.
    void
    setP25Network(P25NetworkModel* model) {
        m_network = model;
    }

    // Issue #708: what call recording is doing, read every tick (the view stays readable when no redraw comes).
    void
    setRecordingStatus(RecordingStatus* status) {
        m_recordingStatus = status;
    }

    // WP-S2: called for a consumed host attachment, never on preference changes. @p attachedKind is the kind of
    // the receiver that attached ("usb" for an RTL-SDR, "airspy"); only a target on that kind starts (#647 D3a).
    void requestAutoStart(bool enabled, bool onboardingDone, const QString& kind, const QString& uid,
                          const QVariantMap& target, const QString& attachedKind);

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
    void autoStartRequested(const QString& kind, const QString& uid);
    void pollIntervalChanged();
    void talkgroupExportResultChanged();
    void decryptionResultChanged();

  private:
    bool sessionIsLive() const;
    void tick();
    /** @brief The redraw-driven part of tick(): metrics, network, history and talkgroups. */
    void refreshModels();
    void pollTalkgroupExport();
    void pollDecryptionResult();
    void invalidateForTarget(const dsd_state* snapshot);
    void onSessionStateChanged();
    void clearLiveModels();

    bool m_autoStartBlocked = true; // Closed until Main.qml installs its binding.
    DecoderHost* m_host = nullptr;
    MetricsModel* m_metrics = nullptr;
    CallHistoryModel* m_history = nullptr;
    TalkgroupListModel* m_talkgroups = nullptr;
    DiagnosticsLogModel* m_diagnostics = nullptr;
    P25NetworkModel* m_network = nullptr; // WP-F2
    RecordingStatus* m_recordingStatus = nullptr;
    QTimer m_timer;
    QVariantMap m_talkgroupExportResult;
    quint64 m_talkgroupExportSequence = 0;
    QPointer<CommandBridge> m_commands;
    QVariantMap m_decryptionResult;
    quint64 m_decryptionSequence = 0;
    unsigned int m_active_ordinal = 0;
    DecoderHost::SessionState m_session = DecoderHost::Idle;
    /* A session ended since the last tick: the history is saved after that tick ingests. */
    bool m_historySaveOwed = false;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_UI_CONTROLLER_H_ */
