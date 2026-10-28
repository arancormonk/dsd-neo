// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What call recording is doing in the session, for QML (issue #708): the context property `recordingStatus`.
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

  public:
    explicit RecordingStatus(QObject* parent = nullptr);

    /** @brief Read the decoder's view (dsd_app_recording_view_get()); emits changed() when it moved. GUI thread. */
    void refresh();
    /** @brief Take @p view as the decoder's, as refresh() does. */
    void apply(const dsd_app_recording_view& view);

    /** @brief CommandBridge refused a recording switch for @p reason, or (empty) sent the last one. */
    void noteRefusal(const QString& reason);

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
    /** @brief The last recording command: "start", "stop", "toggle", "static", "unknownOnly", "config", or empty. */
    QString command() const;
    /** @brief Its state: "none", "pending" (Applying...), "applied", "failed", "refused" (Signal search runs) or
     *  "dropped" (the session ended first). */
    QString commandState() const;
    /** @brief Recording failures this decoder run, all kinds. */
    int failures() const;
    /** @brief The same by kind: "open", "write", "close", "publish". */
    QVariantMap failureCounts() const;
    /** @brief The latest failure's kind, or empty. */
    QString lastError() const;
    /** @brief Why CommandBridge last refused a switch without sending it: "storage-unavailable", "static-wav", or
     *  empty. Such a change applies at the next start. */
    QString refusal() const;

  Q_SIGNALS:
    void changed();

  private:
    dsd_app_recording_view m_view{};
    QString m_refusal;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RECORDING_STATUS_H_ */
