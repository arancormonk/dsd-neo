// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Android implementation of the shared UI's decoder lifecycle interface.
 *
 * Start/stop are relayed to the foreground service, which owns the engine thread;
 * the running state is read back in-process from the JNI lifecycle layer.
 */

#ifndef DSD_NEO_ANDROID_DECODER_HOST_ANDROID_H_
#define DSD_NEO_ANDROID_DECODER_HOST_ANDROID_H_

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>
#include <stdint.h>

#include "decoder_host.h"
#include "diagnostics_log.h"
#include "local_device_state.h"
#include "session_state_map.h"

class QObject;
class QJsonObject;

namespace dsd_android {

class DecoderHostAndroid : public dsd_qt::DecoderHost {
  public:
    explicit DecoderHostAndroid(QObject* parent = nullptr);
    ~DecoderHostAndroid() override;

    bool
    usesPlatformFontScaling() const override {
        return true;
    }

    int
    fontRevision() const override {
        return m_font_revision;
    }

    qreal fontPixelSize(qreal sp) const override;

    qreal
    keyboardTop() const override {
        return m_keyboard_top;
    }

    void setDarkAppearance(bool dark) override;
    void requestNotificationPermission() override;

    bool
    notificationPermissionNeeded() const override {
        return m_notification_permission_needed;
    }

    /** @brief The current attempt's own failure (#782) while it stands, else the last session's input failure. */
    int
    inputFailureKind() const override {
        return m_attempt_failed ? m_attempt_failure_kind : m_input_failure;
    }

    int
    inputFailureCode() const override {
        return m_attempt_failed ? 0 : m_input_error;
    }

    int
    terminalReason() const override {
        return m_terminal_reason;
    }

    QVariantMap
    receiverHealth() const override {
        return m_receiver_health;
    }

    QString
    audioRoute() const override {
        return m_audio_route;
    }

    // WP-D3: foreground location broker.
    bool
    locationSupported() const override {
        return true;
    }

    void requestCurrentLocation(qint64 requestId) override;
    void requestSiteLocation(qint64 requestId) override;
    void cancelLocationRequest(qint64 requestId) override;

    bool isRunning() const override;
    QString statusText() const override;
    SessionState sessionState() const override;
    QString failureText() const override;

    /** @brief Native Running can precede initialization; wait for its explicit success signal. */
    bool
    signalsSessionInitialized() const override {
        return true;
    }

    /** @brief True: an app has to obtain the USB descriptor from Java. */
    bool
    localDeviceBrokered() const override {
        return true;
    }

    bool localDeviceReady() const override;

    QString
    localDeviceSerial() const override {
        return m_usb.serial;
    }

    QString
    localDeviceSource() const override {
        return m_usb.source;
    }

    QString
    attachedDeviceSource() const override {
        return m_attached_source;
    }

    void requestLocalDeviceAccessForSource(const QString& source, const QString& serial = QString()) override;
    void noteStartAttempt(const QString& sourceType) override;
    QString localDeviceStatus() const override;
    int localDeviceFailureKind() const override;

    bool
    localDeviceRequestPending() const override {
        return m_usb.pending;
    }

    QString
    localDeviceRequestedSource() const override {
        return m_usb.requestedSource;
    }

    void hostDiagnostic(const QString& line) override;

    bool
    shareSupported() const override {
        return true;
    }

    void shareDiagnostics(const QString& text, const QString& title) override;

    /** @brief True: ScreenSupport applies the screen policy on Android. */
    bool
    screenPolicySupported() const override {
        return true;
    }

    bool start(const QStringList& argv) override;
    void stop() override;
    void dismissFailure() override;
    bool moveToBackground() override;
    void refresh() override;
    void requestLocalDeviceAccess() override;
    void setScreenPolicy(int mode, int delaySeconds) override;

    /** @brief Materialize a replay document into durable app-private storage; returns "" on failure. */
    QString importContentUri(const QString& reference, const QString& fileName) override;

    /**
     * @brief Materialize a SAF content URI into filesDir/imports; returns "" on failure.
     *
     * No default argument: on a virtual it would be bound from the static type of
     * the call, so a base-class default that ever changed would silently mean two
     * different things depending on which pointer type the caller held.
     */
    QString importDocument(const QString& reference, const QString& fileName, const QString& replacePath) override;

    /**
     * @brief Finish the Activity so a committed restore applies at the next launch (#580).
     *
     * Dispatched to the Android main thread and not waited for. DsdNeoActivity.onDestroy then stops the engine and
     * the service and ends the process, as it does for any other finish.
     */
    bool quitApp() override;

    /**
     * @brief Whether the service is anything but IDLE or the engine atomic is set (engine_busy()).
     *
     * Reads both directly and never refreshes: ui_load asks before the first poll, and a refresh can deliver a USB
     * attachment that starts a session.
     */
    bool engineBusy() const override;

    /*
     * Document copies (#580). These run on BackupModel's worker thread, never the Qt main thread: the JNI calls
     * attach that thread, use the application's ContentResolver, and never wait on the Qt or Android main thread,
     * which BackupModel's destructor may be blocking in waitForDone(). A content:// reference goes to the platform's
     * ContentResolver; a file:// URL or plain path takes the base host's copy.
     *
     * For a content:// reference, copyDocumentToFile() replaces @p destPath: whatever is there is removed before the
     * copy starts (DocumentLocalFailed when it cannot be), so a failure leaves nothing at @p destPath and
     * DocumentCopied always names this copy's file. The platform's DocumentLocalFailed (no held ContentResolver, or
     * its temp file or the rename failed) passes through, as does DocumentTooLarge. A copy reported without its file
     * (a failed JNI call reads as 0) is DocumentLocalFailed too; any other answer is DocumentFailed.
     */
    int copyDocumentToFile(const QString& reference, const QString& destPath, qint64 maxBytes) const override;
    bool copyFileToDocument(const QString& sourcePath, const QString& reference) const override;

    /**
     * @brief Cancel the platform's document calls in flight and stop their copies, then the base host's.
     *
     * A copy into a file still leaves nothing at its destination. A save stopped partway through leaves its document
     * partly written: DocumentCopies skips the delete after an abort, because deleteDocument() takes no cancellation
     * and could hold teardown. Such a document never opens as a backup, since the envelope's length and tag are
     * checked.
     */
    void abortDocumentTransfers() override;

  private:
    /** @brief Records a start attempt in the persistent diagnostics, with the platform's context. */
    void noteAttempt(const QString& sourceType, dsd_qt::DiagnosticsLog::AttemptStage stage);
    // JNI supplies one coherent record; desktop tests exercise the same publication path.
    void applyLifecycleStatus(const QJsonObject& status, bool running);
    /**
     * @brief Whether @p status is a record of another start attempt than m_attempt (gate r5 L1): one this host
     * retired or superseded, or the previous session's while the start waits. Such a record drives no phase, run
     * result, receiver health or device error. False while the host follows no attempt, and for a record that names
     * none.
     */
    bool recordOfOtherAttempt(const QJsonObject& status) const;
    /**
     * @brief The start grace's expiry, if the record just folded ran it out: log it, retire the attempt, and either
     * follow it (the service took it after all) or latch it undelivered. True when it expired.
     */
    bool noteStartExpiry(const QByteArray& service_state, uint64_t now_ms);
    /** @brief Publish @p phase with @p reason, or the current attempt's own failure while one stands. */
    void publishPhase(SessionPhase phase, const QString& reason);
    bool acknowledgeFailure(const QJsonObject& status);

    QJsonObject readLifecycleStatus() const;
    static bool readEngineRunning();
    /** @brief Real monotonic milliseconds (dsd_realtime_mono_ms()) for the stop request's grace. */
    static uint64_t readRealtimeMs();
    /** @brief DecoderService.stateName(); empty when the call failed. */
    static QByteArray readServiceState();
    /** @brief Whether the application's ContentResolver is held, without which no platform transfer can run. */
    static bool platformHoldsResolver();
    /**
     * @brief DocumentCopies.copyDocumentToFile() over JNI: a DocumentCopy value, 0 when the call failed, and
     * DocumentLocalFailed without a held ContentResolver.
     */
    static int platformCopyDocumentToFile(const QString& reference, const QString& destPath, qint64 maxBytes);
    /** @brief DocumentCopies.copyFileToDocument() over JNI; false when the call failed. */
    static bool platformCopyFileToDocument(const QString& sourcePath, const QString& reference);
    void requestServiceStop();
    /**
     * @brief DecoderService.startDecoder() with @p argv for start attempt @p attempt; returns why it could not be
     * handed over, or empty.
     */
    static QString platformStartDecoder(const QStringList& argv, uint64_t attempt);
    /**
     * @brief DecoderService.retireAttempts(): no attempt up to @p through may run any more (#782). Synchronous, from
     * the Qt thread. True when the service had already taken attempt @p through past the point a retirement cancels
     * (it runs, or it ended on its own and its record says how); false when it never will run, and when the call
     * failed.
     */
    static bool platformRetireAttempts(uint64_t through);
    /** @brief UsbSourceManager.requestAccess() / requestAccessForSource(); the answer arrives through refresh(). */
    static void platformRequestAccess();
    static void platformRequestAccessForSource(const QString& source, const QString& serial);
    /** @brief DecoderService.acknowledgeReceiverEnding(): endings up to @p session stay acknowledged past this host. */
    static void platformAcknowledgeReceiverEnding(uint64_t session);
    QString serviceFailureText() const;
    /**
     * @brief Refuse start attempt @p attempt because the service is @p service_state, not IDLE: logged, and published
     * as kAttemptPreviousStopping unless a run is on screen (Starting or Running), which stays as it is. Always
     * returns false.
     */
    bool refuseStart(uint64_t attempt, const QByteArray& service_state);
    /** @brief Record a start that never reached the service. Always returns false. */
    bool failStart(const QString& reason);
    /**
     * @brief Latch the current attempt's own failure (#782): published as Failed with @p text and as
     * inputFailureKind() @p kind (an AttemptFailureKind, or 0 for none the UI words), over whatever the service's
     * records say, until the next start or a dismissal.
     */
    void setAttemptFailure(int kind, const QString& text);
    /** @brief Drop the previous attempt's failure for a start that goes ahead or a refusal of its own. */
    void clearAttemptFailure();
    /** @brief Retire the current attempt and drop its failure, then publish the phase the records give. */
    void dismissAttemptFailure();

    void setStatus(const QString& text);
    void refreshLocation();
    void refreshPresentation();
    void refreshNotificationPermission();
    void refreshLocalDevice();
    /** @brief Publish a phase; @p reason overrides the failure text when non-empty. */
    void setSessionPhase(SessionPhase phase, const QString& reason = QString());
    /** @brief Recompute receiverHealth() from the last record; true when it changed (the caller emits). */
    bool updateReceiverHealth();
    /** @brief A new start acknowledges the endings of sessions up to @p session; emits runResultChanged(). */
    void clearReceiverHealth(uint64_t session);
    /** @brief A new access request acknowledges the ending on screen once its session ended, never one to come. */
    void acknowledgeReceiverEnding();
    /** @brief Whether the native claim error belongs to the receiver that is ready now (B11). */
    bool deviceErrorApplies() const;

    bool m_primed = false;
    // #782: the start attempt this host follows (0: none), and its own failure while it stands. Records of older
    // sessions never replace it; the next start or a dismissal does.
    uint64_t m_attempt = 0;
    bool m_attempt_failed = false;
    int m_attempt_failure_kind = 0;
    QString m_attempt_failure_text;
    uint64_t m_adopted_idle_session = 0;
    uint64_t m_initialized_session = 0;
    int m_device_error = 0;
    // WP-D5: Retry acknowledges only the retained error of this session.
    uint64_t m_device_error_session = 0;
    uint64_t m_retried_device_session = 0;
    // #647 B11: the receiver the claim error came from; it is shown only while that receiver is the ready one.
    // #782: identified by the connection the session took (attachment and connection generation, from the session's
    // record) when the record names one, else by source and serial as before.
    uint64_t m_device_error_owner = 0;
    QString m_device_error_source;
    QString m_device_error_serial;
    QString m_device_error_device;
    qint64 m_device_error_epoch = 0;
    qint64 m_device_error_connection = 0;
    LocalDeviceState m_usb;
    // UsbSourceManager.attachedSource(), polled with the USB record.
    QString m_attached_source;
    bool m_running = false;
    QString m_status = QStringLiteral("Idle");
    SessionPhaseTracker m_phase;
    SessionPhase m_published_phase = kSessionIdle;
    QString m_failure;
    QString m_font_configuration;
    QVector<qreal> m_font_sizes;
    int m_font_revision = 0;
    qreal m_keyboard_top = -1;
    int m_input_failure = 0;
    int m_input_error = 0;
    int m_terminal_reason = 0;
    // #647: the last record's ending (RunEndCause) and receiver (ReceiverSource); endings of sessions up to
    // m_health_cleared_session were acknowledged by a start or an access request, here or by a host before this one
    // (the service keeps the mark, as endingAcknowledged, across Activity recreation).
    uint64_t m_health_session = 0;
    int m_end_cause = 0;
    int m_receiver_source = 0;
    uint64_t m_health_cleared_session = 0;
    QVariantMap m_receiver_health = DecoderHost::healthyReceiver();
    QString m_audio_route = QStringLiteral("System default");
    bool m_notification_permission_needed = false;
};

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_DECODER_HOST_ANDROID_H_ */
