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

#include <QObject>
#include <QString>
#include <QStringList>

namespace dsd_qt {

class DecoderHost : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(SessionState sessionState READ sessionState NOTIFY sessionStateChanged)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionStateChanged)
    Q_PROPERTY(bool transitioning READ transitioning NOTIFY sessionStateChanged)
    Q_PROPERTY(QString failureText READ failureText NOTIFY sessionStateChanged)
    Q_PROPERTY(bool localDeviceBrokered READ localDeviceBrokered CONSTANT)
    Q_PROPERTY(bool localDeviceReady READ localDeviceReady NOTIFY localDeviceChanged)
    Q_PROPERTY(QString localDeviceStatus READ localDeviceStatus NOTIFY localDeviceChanged)
    Q_PROPERTY(bool keepScreenAwakeSupported READ keepScreenAwakeSupported CONSTANT)

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

    explicit DecoderHost(QObject* parent = nullptr);
    ~DecoderHost() override;

    /** @brief Whether the engine is configured and decoding. */
    virtual bool isRunning() const = 0;

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
     * @brief Whether setKeepScreenAwake() does anything on this platform.
     *
     * The Settings screen hides the toggle when it does not: a switch that
     * persists but changes nothing is worse than no switch.
     */
    virtual bool
    keepScreenAwakeSupported() const {
        return false;
    }

  public Q_SLOTS:
    /**
     * @brief Configure the engine with a CLI-shaped argv and start decoding.
     * @param argv Options without the program name; the host prepends it.
     * @return true when the engine accepted the configuration and started.
     */
    virtual bool start(const QStringList& argv) = 0;

    /** @brief Request a graceful stop. Returns immediately; watch @c running. */
    virtual void stop() = 0;

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
     * Unlike importContentUri(), which serves a one-shot open (its Android copy
     * lands in the evictable cache), this copy must outlive the session: saved
     * systems keep the returned path. Collisions on @p fileName are unique-ified,
     * never overwritten — two different documents may share a display name.
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
    virtual void
    requestLocalDeviceAccess() {}

    /**
     * @brief Keep the display from sleeping while the app is foreground.
     *
     * A platform concern, not a UI one: on Android it is a window flag that has
     * to be flipped on the Android main thread. The shared UI wires the persisted
     * preference to this at startup and on every toggle; hosts with no such
     * concept ignore it (see keepScreenAwakeSupported()).
     */
    virtual void
    setKeepScreenAwake(bool on) {
        (void)on;
    }

  Q_SIGNALS:
    void runningChanged();
    void statusTextChanged();
    void sessionStateChanged();
    void localDeviceChanged();
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DECODER_HOST_H_ */
