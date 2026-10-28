// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What call recording is doing in the session, for QML (issue #708): the context property `recordingStatus`.
 *
 * Scoped to the session: a start and an end clear the refusal, and the command readings stay "none" until this
 * session sends a switch, since the decoder's command state is process-wide and would show the last session's.
 *
 * The decoder's recording view (<dsd-neo/app_control/recording_view.h>), read on the UI tick: whether calls record,
 * whether a recording switch Settings sent is still applying or how it ended, what failed, and the journal. Beside it,
 * why CommandBridge last refused a switch without sending it (the session has no storage for call recordings, or it
 * records to one static WAV), so Settings can say so instead of showing "Applying...".
 */

#ifndef DSD_NEO_SRC_UI_QT_RECORDING_STATUS_H_
#define DSD_NEO_SRC_UI_QT_RECORDING_STATUS_H_

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <dsd-neo/app_control/recording_view.h>

namespace dsd_qt {

class DecoderHost;
class RecordingStatus;

/** @brief Tell @p status when @p host's session starts and ends (RecordingStatus::setSessionActive()), now and at every
 * change. */
void recording_status_follow_host(RecordingStatus* status, DecoderHost* host);

class RecordingStatus : public QObject {
    Q_OBJECT
    /* Every property below is meaningful once a decoder run published its view. */
    Q_PROPERTY(bool published READ published NOTIFY changed)
    Q_PROPERTY(bool perCall READ perCall NOTIFY changed)
    Q_PROPERTY(int slotFiles READ slotFiles NOTIFY changed)
    Q_PROPERTY(bool unknownOnly READ unknownOnly NOTIFY changed)
    Q_PROPERTY(bool staticWav READ staticWav NOTIFY changed)
    Q_PROPERTY(bool storageNamed READ storageNamed NOTIFY changed)
    Q_PROPERTY(QString journal READ journal NOTIFY changed)
    Q_PROPERTY(QString command READ command NOTIFY changed)
    Q_PROPERTY(QString commandState READ commandState NOTIFY changed)
    Q_PROPERTY(int failures READ failures NOTIFY changed)
    Q_PROPERTY(QVariantMap failureCounts READ failureCounts NOTIFY changed)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
    Q_PROPERTY(QString refusal READ refusal NOTIFY changed)
    Q_PROPERTY(int refusalCount READ refusalCount NOTIFY changed)

  public:
    explicit RecordingStatus(QObject* parent = nullptr);

    /** @brief Read the decoder's view (dsd_app_recording_view_get()); emits changed() when it moved. GUI thread. */
    void refresh();
    /** @brief Take @p view as the decoder's, as refresh() does. */
    void apply(const dsd_app_recording_view& view);

    /** @brief CommandBridge refused a recording switch for @p reason, sending nothing. */
    void noteRefusal(const QString& reason);
    /** @brief CommandBridge sent a recording switch in this session: the refusal clears and the command readings follow
     * the decoder's again (read now, so the switch shows applying at once). */
    void noteSent();

    /** @brief Whether a session is on (Starting, Running or Stopping). A start or an end clears the refusal and the
     * session's command readings. Off until the host says otherwise. */
    void setSessionActive(bool active);
    bool sessionActive() const;
    /** @brief Whether the session's run is up: its decoder takes commands and its recording view is its own (until
     * then the view is the last run's). Off until the host says otherwise; recording_status_follow_host() sets it. */
    void setRunUp(bool up);
    bool runUp() const;

    bool published() const;
    /** @brief Calls record, one file per call (-P or Record calls). */
    bool perCall() const;
    /** @brief Per-call slot files open now: bit 0 slot 1, bit 1 slot 2. */
    int slotFiles() const;
    bool unknownOnly() const;
    /** @brief The session records to one static WAV (-w). */
    bool staticWav() const;
    /** @brief The session was started with a folder of the app's to record into; false: no storage was available. */
    bool storageNamed() const;
    /** @brief "none", "on" or "failed" (a write failed: off for the rest of the run). */
    QString journal() const;
    /** @brief The last recording command this session sent: "start", "stop", "toggle", "static", "unknownOnly",
     *  "config", or empty before it sent one. */
    QString command() const;
    /** @brief Its state: "none" (none sent this session), "pending" (Applying...), "applied", "failed", "refused" (Signal
     *  search runs) or "dropped" (the session ended first). */
    QString commandState() const;
    /** @brief Recording failures this decoder run, all kinds. */
    int failures() const;
    /** @brief The same by kind: "open", "write", "close", "publish". */
    QVariantMap failureCounts() const;
    /** @brief The latest failure's kind, or empty. */
    QString lastError() const;
    /** @brief Why CommandBridge last refused a switch in this session without sending it: "storage-unavailable" (no
     *  folder to record into), "static-wav" (the session records to the user's own -w file), "no-session", "starting"
     *  (the session's run is not up yet), or empty.
     *  Such a change applies at the next start. */
    QString refusal() const;
    /** @brief How many switches CommandBridge has refused, the same reason again included: a caller that reads it
     *  before and after a switch knows whether that switch was the one refused (refusal() may be an earlier one's). */
    int refusalCount() const;

  Q_SIGNALS:
    void changed();

  private:
    dsd_app_recording_view m_view{};
    QString m_refusal;
    int m_refusalCount = 0;
    bool m_sessionActive = false;
    bool m_runUp = false;
    /* CommandBridge sent a switch since this session started: the decoder's command state is this session's. */
    bool m_sentThisSession = false;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RECORDING_STATUS_H_ */
