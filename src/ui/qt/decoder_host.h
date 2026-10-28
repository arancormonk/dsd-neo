// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Platform-free decoder lifecycle interface consumed by the Qt Quick UI.
 *
 * The shared UI never owns the engine. It drives this interface, and each platform
 * host implements it: on Android by relaying to the foreground service that owns the
 * engine thread, on desktop by owning that thread directly.
 *
 * Implementations must be safe to call from the Qt main thread only.
 */

#ifndef DSD_NEO_SRC_UI_QT_DECODER_HOST_H_
#define DSD_NEO_SRC_UI_QT_DECODER_HOST_H_

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic>
#include <functional>
#include <utility>

namespace dsd_qt {

class DecoderHost : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString localDeviceSerial READ localDeviceSerial NOTIFY localDeviceChanged)
    Q_PROPERTY(QString localDeviceSource READ localDeviceSource NOTIFY localDeviceChanged)
    Q_PROPERTY(QString attachedDeviceSource READ attachedDeviceSource NOTIFY localDeviceChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(bool signalsSessionInitialized READ signalsSessionInitialized CONSTANT)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(SessionState sessionState READ sessionState NOTIFY sessionStateChanged)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionStateChanged)
    Q_PROPERTY(bool transitioning READ transitioning NOTIFY sessionStateChanged)
    Q_PROPERTY(QString failureText READ failureText NOTIFY sessionStateChanged)
    Q_PROPERTY(bool localDeviceBrokered READ localDeviceBrokered CONSTANT)
    Q_PROPERTY(bool localDeviceReady READ localDeviceReady NOTIFY localDeviceChanged)
    Q_PROPERTY(QString localDeviceStatus READ localDeviceStatus NOTIFY localDeviceChanged)
    Q_PROPERTY(bool screenPolicySupported READ screenPolicySupported CONSTANT)
    Q_PROPERTY(bool locationSupported READ locationSupported CONSTANT)
    Q_PROPERTY(bool shareSupported READ shareSupported CONSTANT)
    Q_PROPERTY(int localDeviceFailureKind READ localDeviceFailureKind NOTIFY localDeviceChanged)
    Q_PROPERTY(bool localDeviceRequestPending READ localDeviceRequestPending NOTIFY localDeviceChanged)
    Q_PROPERTY(QString localDeviceRequestedSource READ localDeviceRequestedSource NOTIFY localDeviceChanged)
    Q_PROPERTY(bool usesPlatformFontScaling READ usesPlatformFontScaling CONSTANT)
    Q_PROPERTY(qreal keyboardTop READ keyboardTop NOTIFY keyboardChanged)
    Q_PROPERTY(int fontRevision READ fontRevision NOTIFY typographyChanged)
    Q_PROPERTY(int inputFailureKind READ inputFailureKind NOTIFY runResultChanged)
    Q_PROPERTY(int inputFailureCode READ inputFailureCode NOTIFY runResultChanged)
    Q_PROPERTY(int terminalReason READ terminalReason NOTIFY runResultChanged)
    Q_PROPERTY(QVariantMap receiverHealth READ receiverHealth NOTIFY runResultChanged)
    Q_PROPERTY(QString audioRoute READ audioRoute NOTIFY audioRouteChanged)
    Q_PROPERTY(bool notificationPermissionNeeded READ notificationPermissionNeeded NOTIFY notificationPermissionChanged)
    /* Call recordings (issue #708): playback, export and share. */
    Q_PROPERTY(bool playbackSupported READ playbackSupported CONSTANT)
    Q_PROPERTY(QString playbackPath READ playbackPath NOTIFY playbackChanged)
    Q_PROPERTY(int playbackState READ playbackState NOTIFY playbackChanged)
    Q_PROPERTY(qint64 playbackPositionMs READ playbackPositionMs NOTIFY playbackPositionChanged)
    Q_PROPERTY(qint64 playbackDurationMs READ playbackDurationMs NOTIFY playbackChanged)
    Q_PROPERTY(QString playbackError READ playbackError NOTIFY playbackChanged)
    Q_PROPERTY(bool recordingExportSupported READ recordingExportSupported CONSTANT)
    Q_PROPERTY(bool recordingTransferActive READ recordingTransferActive NOTIFY recordingTransferChanged)
    Q_PROPERTY(qint64 recordingTransferBytes READ recordingTransferBytes NOTIFY recordingTransferChanged)
    Q_PROPERTY(qint64 recordingTransferTotalBytes READ recordingTransferTotalBytes NOTIFY recordingTransferChanged)

  public:
    /**
     * @brief What the UI is showing, as opposed to what the engine is doing.
     *
     * @c running alone cannot drive the screen: it is false both before a start has
     * landed and after one has failed, and those are different pictures. The values
     * are pinned because the Android host static_asserts them against its own
     * Qt-free copy (android/session_state_map.h).
     */
    enum SessionState { Idle = 0, Starting = 1, Running = 2, Stopping = 3, Failed = 4 };
    Q_ENUM(SessionState)

    enum LocalDeviceFailureKind {
        NoDeviceFailure = 0,
        DeviceBusy = 1,
        DeviceOpenFailed = 2,
        DeviceDetached = 3,
        DevicePermission = 4
    };
    Q_ENUM(LocalDeviceFailureKind)

    /**
     * @brief copyDocumentToFile() results. DocumentFailed: the document could not be read (or the copy was aborted);
     * DocumentLocalFailed: it could, but this device could not write the copy (its destination could not be cleared,
     * opened, written or committed).
     */
    enum DocumentCopy { DocumentCopied = 0, DocumentTooLarge = 1, DocumentFailed = 2, DocumentLocalFailed = 3 };

    /**
     * @brief What the call recording player is doing (issue #708). Stopped: nothing open. Preparing: opening
     * playbackPath. Failed: it could not play; playbackError says why.
     */
    enum PlaybackState {
        PlaybackStopped = 0,
        PlaybackPreparing = 1,
        PlaybackPlaying = 2,
        PlaybackPaused = 3,
        PlaybackFailed = 4
    };
    Q_ENUM(PlaybackState)

    /**
     * @brief How a recording export or save ended (recordingTransferFinished()). Missing: every file it named was
     * gone. Busy: another transfer was running. Unsupported: this host cannot write it (the base host has no zip).
     */
    enum RecordingTransferResult {
        TransferDone = 0,
        TransferFailed = 1,
        TransferCancelled = 2,
        TransferMissing = 3,
        TransferBusy = 4,
        TransferUnsupported = 5
    };
    Q_ENUM(RecordingTransferResult)

    explicit DecoderHost(QObject* parent = nullptr);
    ~DecoderHost() override;

    /*
     * Backup primitives (#580). Unlike the rest of this interface they are thread-agnostic: they touch no QObject
     * state, so a worker thread may call them. A transfer is not cancellable once it starts.
     */

    /**
     * @brief Close the app so a staged restore applies on the next launch.
     * @return true when the platform is closing the app; the base host cannot and answers false.
     */
    virtual bool
    quitApp() {
        return false;
    }

    /**
     * @brief Whether a decoder engine outside this UI's view is starting or running.
     *
     * Asked right before a restore commits and before the startup apply changes anything. The base host owns no
     * out-of-process engine and answers false.
     */
    virtual bool
    engineBusy() const {
        return false;
    }

    /**
     * @brief Copy a picked document to @p destPath (written private, through QSaveFile).
     *
     * The base host takes a file:// URL or a plain absolute path. A source larger than @p maxBytes gives
     * DocumentTooLarge, and a destination that cannot be written DocumentLocalFailed; any failure leaves no file at
     * @p destPath. The destination's directory must exist.
     *
     * @return A DocumentCopy value.
     */
    virtual int copyDocumentToFile(const QString& reference, const QString& destPath, qint64 maxBytes) const;

    /**
     * @brief Copy @p sourcePath into the document @p reference chose.
     *
     * The base host takes a file:// URL or a plain absolute path and writes it private, through QSaveFile, so a
     * failure leaves no partial document.
     */
    virtual bool copyFileToDocument(const QString& sourcePath, const QString& reference) const;

    /**
     * @brief Stop every document copy in progress and refuse new ones, for the rest of the process.
     *
     * Only teardown calls it (BackupModel's destructor), so that a copy a slow document provider holds cannot keep
     * the UI thread waiting while the app closes. Sticky: no document copy succeeds afterwards. A copy it stops fails
     * (DocumentFailed, or false). The base host's copies then leave no destination file. A host's save into a document
     * provider may leave the document partly written (Android skips the delete after an abort, because the delete is
     * one more provider call that could hold teardown); such a document never opens as a backup, since the envelope's
     * length and tag are checked. The base host raises a flag its copies check before every chunk; a host that runs
     * transfers of its own stops them too and calls this base.
     */
    virtual void abortDocumentTransfers();

    virtual bool
    usesPlatformFontScaling() const {
        return false;
    }

    virtual qreal
    keyboardTop() const {
        return -1;
    }

    virtual int
    fontRevision() const {
        return 0;
    }

    Q_INVOKABLE virtual qreal
    fontPixelSize(qreal sp) const {
        return sp;
    }

    Q_INVOKABLE virtual void
    setDarkAppearance(bool dark) {
        (void)dark;
    }

    Q_INVOKABLE virtual void
    requestNotificationPermission() {}

    virtual bool
    notificationPermissionNeeded() const {
        return false;
    }

    Q_INVOKABLE QString licenseNotices() const;
    Q_INVOKABLE bool copyText(const QString& text) const;

    virtual int
    inputFailureKind() const {
        return 0;
    }

    virtual int
    inputFailureCode() const {
        return 0;
    }

    virtual int
    terminalReason() const {
        return 0;
    }

    /**
     * @brief Why the current or last session's receiver ended, scoped to that session (#647).
     *
     * {state, source, session}: @c state is "ok", "lost" (the receiver was unplugged) or "failed" (it stopped
     * delivering samples); @c source names the session's receiver ("usb" for an RTL-SDR, "airspy") and @c session
     * the service run, both empty (and 0) while the state is "ok". A host that reports it clears it to "ok" on a new
     * start and on a new access request. Hosts with no brokered receiver always answer healthyReceiver().
     */
    virtual QVariantMap
    receiverHealth() const {
        return healthyReceiver();
    }

    /** @brief The receiverHealth() of a session whose receiver did not end. */
    static QVariantMap
    healthyReceiver() {
        return {{QStringLiteral("state"), QStringLiteral("ok")},
                {QStringLiteral("source"), QString()},
                {QStringLiteral("session"), qlonglong(0)}};
    }

    /**
     * @brief The folders call recordings live in, preferred first (issue #708): new recording folders go to the first
     * one available.
     *
     * Every root the host knows, a root whose storage is not mounted right now included, so the recording store can
     * tell storage that is away from recordings that were deleted; the last is the fallback in the app's own storage.
     * The base host keeps them in one root, "Call recordings" in the app data folder.
     */
    virtual QStringList recordingRoots() const;

    /*
     * Call recording playback (issue #708, D6/D7/D10). One recording plays at a time, beside the live audio. The
     * platform's player runs it and publishes what it does through publishPlayback(); a host without one answers
     * playbackSupported() false and every play fails with "unsupported".
     *
     * playbackError is empty, or why the play failed, for the UI to word: "missing" (the file is gone; the recording
     * store is told), "unavailable" (the platform would not give the app audio now, e.g. during a phone call),
     * "unsupported", or "failed" (it could not be read).
     */
    virtual bool
    playbackSupported() const {
        return false;
    }

    QString
    playbackPath() const {
        return m_playbackPath;
    }

    int
    playbackState() const {
        return m_playbackState;
    }

    qint64
    playbackPositionMs() const {
        return m_playbackPositionMs;
    }

    qint64
    playbackDurationMs() const {
        return m_playbackDurationMs;
    }

    QString
    playbackError() const {
        return m_playbackError;
    }

    /**
     * @brief Play the call recording at @p path from its start, replacing the one playing.
     *
     * Not virtual: it routes through the virtuals a platform overrides. A file that is gone, or a host with no player,
     * first stops what plays (stopPlayback()) and then fails; a file that is there is published Preparing and handed
     * to beginPlayback(), which replaces what plays.
     */
    Q_INVOKABLE void playRecording(const QString& path);

    Q_INVOKABLE virtual void
    pausePlayback() {}

    Q_INVOKABLE virtual void
    resumePlayback() {}

    Q_INVOKABLE virtual void
    seekPlayback(qint64 positionMs) {
        (void)positionMs;
    }

    /** @brief Close the recording playing. A host with a player stops it and calls this base, which publishes it. */
    Q_INVOKABLE virtual void stopPlayback();

    /*
     * Call recording export, save and share (issue #708, D9). The recording store resolves what to send and holds
     * those files (RecordingStore::selectForExport() / selectRecordings()); the host writes them into the document the
     * user picked (@p reference, a file:// URL or a platform document URI) and then emits recordingTransferFinished()
     * with that selection, exactly once for every call, a refused one included, so the store can let the files go.
     * One transfer runs at a time.
     */
    virtual bool
    recordingExportSupported() const {
        return false;
    }

    bool
    recordingTransferActive() const {
        return m_transferSelection >= 0;
    }

    qint64
    recordingTransferBytes() const {
        return m_transferBytes;
    }

    qint64
    recordingTransferTotalBytes() const {
        return m_transferTotalBytes;
    }

    /**
     * @brief Write the recordings of @p selection into one zip, @p reference.
     * @return Whether the export began; recordingTransferFinished() follows either way.
     */
    Q_INVOKABLE bool exportRecordings(const QVariantMap& selection, const QString& reference);
    /**
     * @brief Copy the one recording of @p selection, as it is, into @p reference.
     * @return Whether the save began; recordingTransferFinished() follows either way.
     */
    Q_INVOKABLE bool saveRecording(const QVariantMap& selection, const QString& reference);

    /** @brief Stop the running export or save; it finishes as TransferCancelled. Hosts that run one override it. */
    Q_INVOKABLE virtual void
    cancelRecordingTransfer() {}

    /**
     * @brief Hand the call recording at @p path to another app (shareSupported()), titled @p title.
     *
     * Only a finished recording the store manages (setRecordingIndex()) is shared; a missing one is reported.
     * @return Whether the platform took it.
     */
    Q_INVOKABLE bool shareRecording(const QString& path, const QString& title);

    /** @brief Which paths are call recordings the store manages (shareRecording() shares nothing else). */
    void setRecordingIndex(std::function<bool(const QString&)> indexed);

    virtual QString
    audioRoute() const {
        return QStringLiteral("System default");
    }

    Q_INVOKABLE QVariantMap documentInfo(const QString& path) const;

    /** @brief Whether the engine is running; initialization may still be pending. */
    virtual bool isRunning() const = 0;

    /**
     * @brief Whether sessionInitialized() reports successful engine initialization.
     *
     * Running can precede tuner/file initialization on these hosts, so recency
     * bookkeeping must wait for the signal. Running can also precede the decoder's
     * command admission there, so a command submitted on that edge can be refused;
     * the signal follows admission. Other hosts use Running as their readiness
     * edge. This capability is constant for the lifetime of a host.
     */
    virtual bool
    signalsSessionInitialized() const {
        return false;
    }

    /** @brief Short human-readable host state (also used for platform notifications). */
    virtual QString statusText() const = 0;

    /**
     * @brief Lifecycle phase the UI branches on.
     *
     * The default collapses to the two states a host with no transition reporting can
     * distinguish; hosts that own a real state machine override it.
     */
    virtual SessionState
    sessionState() const {
        return isRunning() ? Running : Idle;
    }

    /**
     * @brief Whether a session is on screen — starting, decoding or winding down.
     *
     * This is the monitoring-view predicate: the status and event panes exist exactly
     * while it is true, so a start that is still coming up already has somewhere to
     * report progress.
     */
    bool
    sessionActive() const {
        const SessionState state = sessionState();
        return state == Starting || state == Running || state == Stopping;
    }

    /**
     * @brief Whether sessionState() reflects the decoder yet (issue #708).
     *
     * A host that learns its session state by polling (Android asks the decoder service) does not know it until its
     * first poll, so a session already running when the UI loads first reads as Idle. sessionStateKnownChanged()
     * fires when it turns true. Hosts that always know answer true.
     */
    virtual bool
    sessionStateKnown() const {
        return true;
    }

    /** @brief Whether the session is mid-transition, so the primary action must wait. */
    bool
    transitioning() const {
        const SessionState state = sessionState();
        return state == Starting || state == Stopping;
    }

    /**
     * @brief Why the last start failed, or empty when it did not.
     *
     * A start that dies inside the platform layer otherwise leaves nothing on screen
     * but a return to idle, which reads as "nothing happened".
     */
    virtual QString
    failureText() const {
        return QString();
    }

    /**
     * @brief Whether this platform has to broker access to a directly attached SDR.
     *
     * An Android app cannot open a USB device itself: the host obtains a descriptor,
     * with a permission prompt in between, and hands it to the engine. Hosts that let
     * the engine open the device directly answer false, and the UI then offers the
     * local-device input with no extra gesture.
     */
    virtual bool
    localDeviceBrokered() const {
        return false;
    }

    /** @brief Whether a directly attached SDR can be used right now. */
    virtual bool
    localDeviceReady() const {
        return true;
    }

    /** @brief Short human-readable state of the directly attached SDR. */
    virtual QString
    localDeviceStatus() const {
        return QString();
    }

    /**
     * @brief Whether setScreenPolicy() does anything on this platform.
     *
     * The Settings screen hides the Screen rows when it does not: a setting that
     * persists but changes nothing is worse than no setting.
     */
    virtual bool
    screenPolicySupported() const {
        return false;
    }

    virtual bool
    locationSupported() const {
        return false;
    }

    virtual bool
    shareSupported() const {
        return false;
    }

    virtual int
    localDeviceFailureKind() const {
        return NoDeviceFailure;
    }

    /** @brief Whether an access request for a brokered receiver is waiting for its answer (#647). */
    virtual bool
    localDeviceRequestPending() const {
        return false;
    }

    /** @brief The source ("usb", "airspy") the last access request asked for; empty before any request. */
    virtual QString
    localDeviceRequestedSource() const {
        return {};
    }

  public Q_SLOTS:

    /** Unsupported hosts still answer, so a caller cannot wait forever for a fix. */
    virtual void
    requestCurrentLocation(qint64 requestId) {
        Q_EMIT locationResult(requestId, false, 0, 0, 0, 0, false, QString(), QString(),
                              QStringLiteral("Location is not supported on this platform"));
    }

    /** Fresh coordinates for distance ranking; no postal-code lookup is needed. */
    virtual void
    requestSiteLocation(qint64 requestId) {
        requestCurrentLocation(requestId);
    }

    virtual void
    cancelLocationRequest(qint64 requestId) {
        (void)requestId;
    }

    /** Content, never an arbitrary path. Platform hosts own their narrow share cache. */
    virtual void
    shareDiagnostics(const QString& text, const QString& title) {
        (void)text;
        (void)title;
    }

    /** Platform lifecycle/USB diagnostics enter through the host, not a UI platform include. */
    virtual void
    hostDiagnostic(const QString& line) {
        (void)line;
    }

    /**
     * @brief Configure the engine with a CLI-shaped argv and start decoding.
     * @param argv Options without the program name; the host prepends it.
     * @return true when the engine accepted the configuration and started.
     */
    virtual bool start(const QStringList& argv) = 0;

    /** @brief Request a graceful stop. Returns immediately; watch @c running. */
    virtual void stop() = 0;

    /**
     * @brief The user dismissed the failure on screen (#782).
     *
     * A host that tracks its start attempts gives up on the one that failed, so it can never start later, and drops
     * a failure scoped to it. The default has nothing to give up.
     */
    virtual void
    dismissFailure() {}

    /**
     * @brief Send the UI to the background instead of destroying it.
     *
     * Closing the window is not a neutral act: on Android it finishes the Activity,
     * and Qt then terminates the process — which would take the decoder with it.
     * Hosts that can background themselves do so here and answer true; the default
     * answers false, and the caller must then let the close proceed.
     *
     * @return true when the host backgrounded itself and the close must be refused.
     */
    virtual bool
    moveToBackground() {
        return false;
    }

    /**
     * @brief Re-read platform state; called from the UI's single poll tick.
     *
     * The shared UI owns exactly one polling thread, so hosts must not start their
     * own timers to keep @c running and @c statusText fresh.
     */
    virtual void
    refresh() {}

    /**
     * @brief Turn a platform file reference into a path the engine can open.
     *
     * Hosts whose file pickers hand back opaque references (Android SAF content URIs)
     * copy the payload somewhere real and return that path. Hosts with real paths
     * return @p reference unchanged.
     *
     * @return Absolute filesystem path, or an empty string on failure.
     */
    virtual QString
    importContentUri(const QString& reference, const QString& fileName) {
        (void)fileName;
        return reference;
    }

    /**
     * @brief Materialize a picked document into durable app storage.
     *
     * Unlike importContentUri(), which serves a one-shot open, this copy must
     * outlive the session: saved systems keep the returned path. (The Android
     * host makes the same durable copy for both.) Collisions on @p fileName are
     * unique-ified, never overwritten — two different documents may share a
     * display name.
     *
     * @param reference   Platform file reference from the picker (file:// URL or
     *                    Android SAF content URI).
     * @param fileName    Display name to store the copy under.
     * @param replacePath Existing stored file to update in place, or empty for a
     *                    new copy. A path outside the app's imports directory is
     *                    not a write target and is treated as a new copy.
     * @return Absolute filesystem path of the stored copy, or empty on failure.
     */
    virtual QString importDocument(const QString& reference, const QString& fileName,
                                   const QString& replacePath = QString());

    /**
     * @brief Store a file this process already wrote, by plain filesystem path.
     *
     * This is importDocument()'s body without the picker: no URL parsing and no
     * platform brokering, because the source is ours. Deliberately NOT virtual -
     * a generated file must take the pure-Qt copy path on every platform, and on
     * Android the imports directory is ordinary filesDir storage, so there is
     * nothing for a SAF override to add.
     *
     * @param sourcePath  Absolute path of the file to copy in.
     * @param fileName    Display name to store the copy under.
     * @param replacePath Existing stored file to update in place, or empty for a
     *                    new copy. A path outside the imports directory is not a
     *                    write target and is treated as a new copy.
     * @return Absolute filesystem path of the stored copy, or empty on failure.
     */
    QString importLocalFile(const QString& sourcePath, const QString& fileName, const QString& replacePath = QString());

    /**
     * @brief Ask the platform for access to a directly attached SDR.
     *
     * May show a permission prompt, so the answer arrives later: watch
     * @c localDeviceReady and @c localDeviceStatus rather than a return value.
     * Hosts that do not broker access do nothing.
     */
    virtual QString
    localDeviceSerial() const {
        return {};
    }

    virtual QString
    localDeviceSource() const {
        return QStringLiteral("usb");
    }

    /**
     * @brief The kind of receiver plugged in, before any permission is asked.
     *
     * "usb" (RTL-SDR dongles only), "airspy" (Airspys only), "both", or empty when
     * no supported receiver is attached or the host cannot tell. Unlike
     * localDeviceSource(), which names the device access was granted for, this
     * reads the bus, so the UI can start on the receiver actually attached.
     * Hosts that do not broker USB devices answer empty.
     */
    virtual QString
    attachedDeviceSource() const {
        return {};
    }

    Q_INVOKABLE virtual void
    requestLocalDeviceAccessForSource(const QString& source, const QString& serial = QString()) {
        (void)source;
        (void)serial;
        requestLocalDeviceAccess();
    }

    virtual void
    requestLocalDeviceAccess() {}

    /**
     * @brief A start is about to touch the device: record it durably (issue #745).
     *
     * Called before a USB access request and before start(), so a phone that falls over seconds
     * after Play keeps the attempt and its start sequence in the persistent diagnostics. Hosts
     * with no persistent diagnostics do nothing.
     */
    virtual void
    noteStartAttempt(const QString& sourceType) {
        (void)sourceType;
    }

    /**
     * @brief Hand the platform the screen policy the user chose (#574).
     *
     * @param mode An AppPrefs::ScreenMode code.
     * @param delaySeconds One of AppPrefs::screenDelayChoices().
     *
     * A platform concern, not a UI one: on Android the policy has to act while
     * Qt's event loop is stopped (a call wakes a screen that is off), so it lives
     * in the platform layer. The shared UI wires the persisted preferences to this
     * at startup and on every change; hosts with no such concept ignore it (see
     * screenPolicySupported()).
     */
    virtual void
    setScreenPolicy(int mode, int delaySeconds) {
        (void)mode;
        (void)delaySeconds;
    }

  Q_SIGNALS:
    void backRequested();
    void typographyChanged();
    void keyboardChanged();
    void runResultChanged();
    void audioRouteChanged();
    void notificationPermissionChanged();
    void sessionInitialized();
    void locationResult(qint64 requestId, bool fixOk, double lat, double lon, double accuracyM, qint64 fixAtMs,
                        bool geocodeOk, const QString& postalCode, const QString& countryCode, const QString& error);
    /**
     * @brief A supported receiver was plugged in; each attachment is delivered once.
     *
     * @p attachment is "<kind>:<device name>": the kind is "usb" for an RTL-SDR or "airspy", and decides which
     * saved system or scan list may start on it (#647 D3a); the name is the device's own, for display only.
     */
    void localDeviceAttached(const QString& attachment);
    void runningChanged();
    void statusTextChanged();
    void sessionStateChanged();
    void sessionStateKnownChanged();
    void localDeviceChanged();
    void playbackChanged();
    void playbackPositionChanged();
    void recordingTransferChanged();
    /** @brief The export or save of @p selection ended (a RecordingTransferResult); @p missing files were gone. */
    void recordingTransferFinished(int selection, int result, int missing);
    /** @brief Playback, an export or a share found these call recordings gone: the store looks at them again. */
    void recordingFilesMissing(const QStringList& paths);

  protected:
    /** @brief Start playing @p path, a file that exists, published Preparing. The base host has no player. */
    virtual void beginPlayback(const QString& path);

    /**
     * @brief Begin writing @p paths into @p reference: one zip with entries named @p names when @p zip, else the one
     * file as it is. Nothing has looked at the files yet: the host skips the ones gone (all gone: TransferMissing),
     * publishes the total of the ones it writes through publishRecordingTransferProgress() (it starts as the
     * selection's), and reports its end once, with the files it found gone, through finishRecordingTransfer(), on the
     * GUI thread.
     *
     * The base host saves a single file synchronously through copyFileToDocument(), and has no zip.
     * @return false when it could not begin (finished as TransferFailed).
     */
    virtual bool beginRecordingTransfer(const QStringList& paths, const QStringList& names, const QString& reference,
                                        bool zip);

    /** @brief Share @p path, a recording the store manages, through the platform; the base host cannot. */
    virtual bool
    shareRecordingFile(const QString& path, const QString& title) {
        (void)path;
        (void)title;
        return false;
    }

    /** @brief What the player is doing now; emits the change signals for what changed. */
    void publishPlayback(const QString& path, int state, qint64 positionMs, qint64 durationMs, const QString& error);
    void publishRecordingTransferProgress(qint64 bytes, qint64 totalBytes);
    /** @brief The running transfer ended with @p result (a RecordingTransferResult); @p missing of its files were
     * gone (reported to the store, and counted in recordingTransferFinished()). */
    void finishRecordingTransfer(int result, const QStringList& missing);
    /** @brief Tell the recording store these call recordings were found gone. */
    void reportRecordingsMissing(const QStringList& paths);

    /** Whether abortDocumentTransfers() was called; document copies check it before every chunk. */
    bool
    documentTransfersAborted() const {
        return m_documentTransfersAborted.load();
    }

  private:
    bool beginTransfer(const QVariantMap& selection, const QString& reference, bool zip);
    /* recordingTransferFinished() for @p selection, queued: a caller sees its call return first. */
    void announceTransferEnd(int selection, int result, int missing);

    std::atomic<bool> m_documentTransfersAborted{false};
    QString m_playbackPath;
    int m_playbackState = PlaybackStopped;
    qint64 m_playbackPositionMs = 0;
    qint64 m_playbackDurationMs = 0;
    QString m_playbackError;
    /* The running transfer's selection, -1 with none. */
    int m_transferSelection = -1;
    qint64 m_transferBytes = 0;
    qint64 m_transferTotalBytes = 0;
    std::function<bool(const QString&)> m_recordingIndexed;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DECODER_HOST_H_ */
