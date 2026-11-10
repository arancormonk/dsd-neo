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
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariantMap>
#include <QtGlobal>
#include <memory>
#include <stdint.h>

#include "decoder_host.h"
#include "diagnostics_log.h"
#include "local_device_state.h"
#include "presentation_scale.h"
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
        return m_scale.fontRevision();
    }

    qreal fontPixelSize(qreal sp) const override;

    qreal
    keyboardTop() const override {
        return m_scale.keyboardTop();
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

    /*
     * The app's own document picker (#829): DocumentPicker.kt asks Android on its main thread, and refresh() polls its
     * records and answers the live request on the Qt thread, never inside the request call. A request made without an
     * activity context, or one the platform could not take, is answered "failed", queued.
     */
    bool
    documentPickerSupported() const override {
        return true;
    }

    qint64 requestOpenDocument() override;
    qint64 requestSaveDocument(const QString& suggestedName, const QString& mimeType) override;
    void cancelDocumentRequest(qint64 requestId) override;
    /** @brief DownloadsSave's name for a Download-folder row it published, from memory only. */
    QString savedDocumentName(const QString& reference) const override;
    /** @brief DownloadsSave deletes the row on its own thread, if it is still pending and no write took it. */
    void discardPendingDownload(const QString& reference) override;

    bool isRunning() const override;
    QString statusText() const override;
    SessionState sessionState() const override;
    QString failureText() const override;

    /** @brief Known after the first poll of the decoder service (a session may already be running then). */
    bool
    sessionStateKnown() const override {
        return m_primed;
    }

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

    /**
     * @brief The import copies' transport (#832; decoder_host_android_imports.cpp), on the import worker thread.
     *
     * A content:// reference goes over JNI to DocumentCopies' import copies, on the held ContentResolver, as the
     * backup's copies do: the destination is cleared first (DocumentLocalFailed when it cannot be), and without a held
     * ContentResolver the copy is DocumentLocalFailed. The platform's codes pass through, DocumentCancelled included.
     * A copy reported without its file (a failed JNI call reads as 0) is DocumentLocalFailed; any other answer is
     * DocumentFailed. Any other reference takes the base transport's local copy. cancel() and forget() reach both
     * sides; progress() reads the side the latest copy took. abortDocumentTransfers() stops the platform's copies
     * (DocumentCopies.abort()) and aborts the transport.
     */
    std::shared_ptr<dsd_qt::DocumentImportTransport> importTransport() const override;

    /**
     * @brief Shared storage's "Call recordings" folder, then internal storage's (#708 D1; RecordingShare.roots()).
     *
     * Shared storage's root is named even while it is away, at the path Android gives the app there. The base host's
     * root when the platform names none.
     */
    QStringList recordingRoots() const override;

    /** @brief True: RecordingPlayback plays call recordings through MediaPlayer (#708 D6). */
    bool
    playbackSupported() const override {
        return true;
    }

    /*
     * The player's state reaches the host through refresh(): while a play is open, each poll reads RecordingPlayback's
     * status record and publishes it. Leaving the foreground pauses it there (D10); Qt is not running then. A play that
     * ends at the recording's end reads Stopped with its path kept and its position at its length, unlike a stop.
     */
    void pausePlayback() override;
    void resumePlayback() override;
    void seekPlayback(qint64 positionMs) override;
    void stopPlayback() override;

    /** @brief True: RecordingExport writes a ZIP into a picked document (#708 D9). */
    bool
    recordingExportSupported() const override {
        return true;
    }

    /** @brief Cancel the running export or save (RecordingExport.cancel()); it finishes as TransferCancelled. */
    void cancelRecordingTransfer() override;

    /**
     * @brief Cancel the platform's document calls in flight and stop their copies, then the base host's.
     *
     * A copy into a file still leaves nothing at its destination. A save stopped partway through leaves its document
     * partly written: DocumentCopies skips the delete after an abort, because deleteDocument() takes no cancellation
     * and could hold teardown. Such a document never opens as a backup, since the envelope's length and tag are
     * checked.
     */
    void abortDocumentTransfers() override;

    /* Android has the Autofill Framework, so the relay samples the sign-in form here. */
    bool
    signInAutofillSupported() const override {
        return true;
    }

    /*
     * The RadioReference sign-in form for password managers: each call goes to SignInAutofill.kt, which posts it to
     * the Android main thread and describes the form to the Autofill Framework there. JNI only, in
     * decoder_host_android.cpp. A call that fails is dropped, its Java exception cleared.
     */
    void signInAutofillBegin(int token) override;
    void signInAutofillField(int token, int field, const QRect& windowPixels, bool visible, bool focused,
                             const QString& structureValue) override;
    void signInAutofillRequest(int token, int field, const QRect& windowPixels) override;
    void signInAutofillCommit(int token, const QString& username, const QString& password) override;
    void signInAutofillEnd(int token) override;

    /**
     * @brief A password manager's fill from the Android main thread (DsdNative.nativeSignInFilled()).
     *
     * Takes both strings, leaving the caller's empty, and queues them to the newest live host, which hands them to
     * deliverSignInFill() on the GUI thread; that drops a fill for any other token. With no host, or a host gone
     * before the call runs, they are scrubbed and dropped. Any thread.
     */
    static void postSignInFill(int token, bool hasUsername, QString& username, bool hasPassword, QString& password);

  protected:
    /** @brief Hand @p path to RecordingPlayback; a play the platform refuses fails as "failed". */
    void beginPlayback(const QString& path) override;

    /**
     * @brief Export or save on the transfer worker thread, never the GUI thread (#708).
     *
     * The worker skips the files gone and counts the rest, then hands a content:// document to RecordingExport.
     * Another reference (a file URL) takes the base host's single-file copy there, and no ZIP; that copy cannot be
     * cancelled (cancelRecordingTransfer() reaches RecordingExport only), which is acceptable because the system
     * pickers hand out content:// documents. Progress is polled by refresh(); the end comes back to the GUI thread
     * queued.
     */
    bool beginRecordingTransfer(const QStringList& paths, const QStringList& names, const QString& reference,
                                bool zip) override;

    /**
     * @brief RecordingShare: only a .wav inside a recording root is offered, read-only, through the app's chooser, as a
     * copy the transfer worker makes in the app's cache: retention or a delete may take the original while the app it
     * went to still opens it. A share waits behind a running export.
     */
    bool shareRecordingFile(const QString& path, const QString& title) override;

  private:
    /**
     * @brief Makes its host one postSignInFill() can reach, from construction until it goes
     * (decoder_host_android_sign_in.cpp).
     */
    class SignInFillLink {
      public:
        explicit SignInFillLink(DecoderHostAndroid* host);
        ~SignInFillLink();
        SignInFillLink(const SignInFillLink&) = delete;
        SignInFillLink& operator=(const SignInFillLink&) = delete;

      private:
        DecoderHostAndroid* m_host;
    };

    /** @brief Whether @p reference is a content URI, which only the platform's ContentResolver opens. */
    static bool isContentUri(const QString& reference);
    /** @brief Publish the open play's status record (#708); a play that ended is polled no more. */
    void refreshPlayback();
    /** @brief Publish the running transfer's progress (#708). */
    void refreshRecordingTransfer();
    /** @brief Emit recordingShareFailed() for each share RecordingShare could not hand over (#708). */
    void refreshRecordingShares();
    /** @brief Emit recordingShareFailed(@p path), and have the store look again if the recording is gone. */
    void noteRecordingShareFailed(const QString& path);
    /** @brief A share's copy step ended (@p staged: the platform took it): its store hold @p hold goes. */
    void endRecordingShare(const QString& path, int hold, bool staged);
    /** @brief At start, on the transfer worker: remove the share copies past their day (RecordingShare.clean()). */
    void startRecordingShareClean();
    /** @brief How a transfer job reaches its host; defined in decoder_host_android_recordings.cpp. */
    struct TransferLink;
    static std::shared_ptr<TransferLink> newTransferLink(DecoderHostAndroid* host);
    /** @brief The one-thread pool transfers run on. */
    static std::unique_ptr<QThreadPool> newTransferWorker();
    /**
     * @brief The transfer worker's job: count, write, and queue the end back to the GUI thread.
     *
     * Static, and through @p link only: a job the destructor stopped waiting for never touches the host.
     */
    static void runRecordingTransfer(const std::shared_ptr<TransferLink>& link, qint64 id, const QStringList& paths,
                                     const QStringList& names, const QString& reference, bool zip);
    /**
     * @brief RecordingExport's transfer of @p paths into the platform document @p reference: its result, with the
     * paths it found gone added to @p missing. A call that gave no answer discards a Download-folder row (#829).
     */
    static int transferToDocument(qint64 id, const QStringList& paths, const QStringList& names,
                                  const QString& reference, bool zip, QStringList& missing);
    void noteRecordingTransferTotal(qint64 id, qint64 total);
    void endRecordingTransfer(qint64 id, int result, const QStringList& missing);
    /**
     * @brief At destruction: stop the running transfer (the teardown abort, then its cancel), wait for the worker a
     * bounded time, and stop the player.
     *
     * A write blocked on a provider's pipe ignores both; after the bound the worker is left to finish on its own
     * (logged), cut off from this host.
     */
    void closeRecordingWork();

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
    /** @brief DocumentCopies.copyFileToDocument() over JNI (the backup's save, #831); false when the call failed. */
    static bool platformCopyFileToDocument(const QString& sourcePath, const QString& reference);
    /*
     * DocumentCopies' import copies over JNI (#832), from the import worker. The transport that calls them is a nested
     * class so it can, without holding the host.
     */
    class ImportTransport;
    /**
     * @brief DocumentCopies.copyImportDocument(): a DocumentCopy value, 0 when the call failed, and DocumentLocalFailed
     * without a held ContentResolver.
     */
    static int platformCopyImportDocument(qint64 request, const QString& reference, const QString& destPath,
                                          qint64 maxBytes);
    static void platformCancelImport(qint64 request);
    /** @brief DocumentCopies.importProgress(): bytes copied and declared size; -1, -1 when none runs or it failed. */
    static void platformImportProgress(qint64 request, qint64& bytes, qint64& total);
    /** @brief DocumentCopies.importName(); empty when it has none or the call failed. */
    static QString platformImportName(qint64 request);
    static bool platformImportNoSpace(qint64 request);
    static void platformForgetImport(qint64 request);
    void requestServiceStop();
    /* Call recordings (#708) over JNI: RecordingShare, RecordingPlayback and RecordingExport. */
    /** @brief RecordingShare.roots(); empty when the call failed. */
    static QStringList platformRecordingRoots();
    /** @brief RecordingPlayback.play(): the play's generation, 0 when the call failed. */
    static int platformPlayRecording(const QString& path);
    static void platformPausePlayback();
    static void platformResumePlayback();
    static void platformSeekPlayback(qint64 positionMs);
    static void platformStopPlayback();
    /** @brief RecordingPlayback.status(): "generation, state, position, duration, error", tab-separated. */
    static QString platformPlaybackStatus();
    /**
     * @brief RecordingExport.transfer() on the calling (worker) thread: the result code, then the paths it found gone;
     * empty when the call failed.
     */
    static QStringList platformTransferRecordings(qint64 id, const QStringList& paths, const QStringList& names,
                                                  const QString& reference, bool zip);
    /** @brief RecordingExport.progress(): the source bytes written, -1 when transfer @p id is not running. */
    static qint64 platformRecordingTransferProgress(qint64 id);
    static void platformCancelRecordingTransfer(qint64 id);
    /** @brief RecordingShare.canShare(); false when the call failed. */
    static bool platformCanShareRecording(const QString& path);
    /**
     * @brief RecordingShare.stage() on the calling (worker) thread, then RecordingShare.offer() of its copy on the
     * Android main thread, not waited for. A share that fails in either is queued in RecordingShare
     * (platformTakeRecordingShareFailure()).
     * @return false when RecordingShare could not be asked, so nothing queued the failure.
     */
    static bool platformShareRecording(const QString& path, const QString& title);
    /** @brief RecordingShare.clean() on the calling (worker) thread: the share copies past their day go. */
    static void platformCleanRecordingShares();
    /** @brief RecordingShare.takeFailure(): the oldest share that failed after canShare(), or empty. */
    static QString platformTakeRecordingShareFailure();
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
    /**
     * @brief Make @p id the live document request (#829). One it replaces is cancelled on the platform and answered
     * documentPickCancelled(), queued; a record of its own that still comes is dropped.
     */
    void beginDocumentRequest(qint64 id);
    /** @brief Answer @p id documentPickUnavailable(@p reason), queued, unless it is no longer the live request then. */
    void refuseDocumentRequestLater(qint64 id, const QString& reason);
    /** @brief Deliver every DocumentPicker record waiting, oldest first (android/document_pick_record.h). */
    void refreshDocumentPicks();
    /*
     * DocumentPicker over JNI, from the Qt thread. open() and create() hand the activity over only when Qt's context
     * is one; each is false when it was not, or when the call failed, so nothing will answer the request.
     */
    static bool platformOpenDocument(qint64 id);
    static bool platformSaveDocument(qint64 id, const QString& suggestedName, const QString& mimeType);
    static void platformCancelDocument(qint64 id);
    /** @brief DocumentPicker.pollResult(): the oldest record not yet read, or empty. */
    static QString platformPollDocumentPick();
    /*
     * DownloadsSave over JNI (#829), from any thread; none waits on a provider. start() hands it the held
     * ContentResolver and releases the app's leftover persisted grants on its own thread.
     */
    static void platformStartDownloads();
    /** @brief DownloadsSave.publishedName(); empty when the call failed. */
    static QString platformSavedDocumentName(const QString& reference);
    static void platformDiscardPendingDownload(const QString& reference);
    void refreshPresentation();
    /** @brief Hands Qt the Activity's display density on the Android main thread; PresentationScale's push. */
    void startDisplayDensityPush();
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
    // Display size: the font table, the keyboard inset and the density follow (presentation_scale.h).
    PresentationScale m_scale;
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
    // #708: the open play's generation (0: nothing to poll), and the running transfer (0: none) with its total (-1
    // until the worker counted it).
    int m_playback_generation = 0;
    qint64 m_transfer_id = 0;
    qint64 m_transfer_total = -1;
    // The store holds of the shares queued on the transfer worker, released as each copy is made or fails, and at the
    // latest when this host goes.
    QSet<int> m_share_holds;
    // #829: the document request a DocumentPicker record may answer (0: none).
    qint64 m_document_request = 0;
    // The transfer jobs' way back to this host; cleared before it goes.
    std::shared_ptr<TransferLink> m_transfer_link = newTransferLink(this);
    // One transfer at a time, off the GUI thread. After the link it reaches the host through, so it goes first.
    std::unique_ptr<QThreadPool> m_transfer_worker = newTransferWorker();
    // Last, so it goes first: from then on no fill is queued to this host.
    SignInFillLink m_sign_in_fill_link{this};
};

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_DECODER_HOST_ANDROID_H_ */
