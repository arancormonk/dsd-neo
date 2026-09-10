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

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>
#include <stdint.h>

#include "decoder_host.h"
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

    int
    inputFailureKind() const override {
        return m_input_failure;
    }

    int
    inputFailureCode() const override {
        return m_input_error;
    }

    int
    terminalReason() const override {
        return m_terminal_reason;
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
    QString localDeviceStatus() const override;
    int localDeviceFailureKind() const override;
    void hostDiagnostic(const QString& line) override;

    bool
    shareSupported() const override {
        return true;
    }

    void shareDiagnostics(const QString& text, const QString& title) override;

    /** @brief True: FLAG_KEEP_SCREEN_ON on the Activity window is available. */
    bool
    keepScreenAwakeSupported() const override {
        return true;
    }

    bool start(const QStringList& argv) override;
    void stop() override;
    bool moveToBackground() override;
    void refresh() override;
    void requestLocalDeviceAccess() override;
    void setKeepScreenAwake(bool on) override;

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

  private:
    // JNI supplies one coherent record; desktop tests exercise the same publication path.
    void applyLifecycleStatus(const QJsonObject& status, bool running);
    bool acknowledgeFailure(const QJsonObject& status);

    QJsonObject readLifecycleStatus() const;
    static bool readEngineRunning();
    void requestServiceStop();
    QString serviceFailureText() const;
    /** @brief Record a start that never reached the service. Always returns false. */
    bool failStart(const QString& reason);

    void setStatus(const QString& text);
    void refreshLocation();
    void refreshPresentation();
    void refreshLocalDevice();
    /** @brief Publish a phase; @p reason overrides the failure text when non-empty. */
    void setSessionPhase(SessionPhase phase, const QString& reason = QString());

    bool m_primed = false;
    uint64_t m_adopted_idle_session = 0;
    uint64_t m_initialized_session = 0;
    int m_device_error = 0;
    // WP-D5: Retry acknowledges only the retained error of this session.
    uint64_t m_device_error_session = 0;
    uint64_t m_retried_device_session = 0;
    LocalDeviceState m_usb;
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
    QString m_audio_route = QStringLiteral("System default");
};

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_DECODER_HOST_ANDROID_H_ */
