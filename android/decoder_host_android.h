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

#include "decoder_host.h"
#include "session_state_map.h"

namespace dsd_android {

class DecoderHostAndroid : public dsd_qt::DecoderHost {
  public:
    explicit DecoderHostAndroid(QObject* parent = nullptr);
    ~DecoderHostAndroid() override;

    bool isRunning() const override;
    QString statusText() const override;
    SessionState sessionState() const override;
    QString failureText() const override;

    /** @brief True: an app has to obtain the USB descriptor from Java. */
    bool
    localDeviceBrokered() const override {
        return true;
    }

    bool localDeviceReady() const override;
    QString localDeviceStatus() const override;

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

    /** @brief Materialize a SAF content URI into cacheDir; returns "" on failure. */
    QString importContentUri(const QString& reference, const QString& fileName) override;

  private:
    /** @brief Record a start that never reached the service. Always returns false. */
    bool failStart(const QString& reason);

    void setStatus(const QString& text);
    void setLocalDeviceState(bool ready, const QString& text);
    /** @brief Publish a phase; @p reason overrides the failure text when non-empty. */
    void setSessionPhase(SessionPhase phase, const QString& reason = QString());

    bool m_running = false;
    QString m_status = QStringLiteral("Idle");
    SessionPhaseTracker m_phase;
    SessionPhase m_published_phase = kSessionIdle;
    QString m_failure;
    bool m_usb_ready = false;
    QString m_usb_status;
};

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_DECODER_HOST_ANDROID_H_ */
