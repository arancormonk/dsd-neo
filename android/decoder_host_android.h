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

namespace dsd_android {

class DecoderHostAndroid : public dsd_qt::DecoderHost {
  public:
    explicit DecoderHostAndroid(QObject* parent = nullptr);
    ~DecoderHostAndroid() override;

    bool isRunning() const override;
    QString statusText() const override;

    /** @brief True: an app has to obtain the USB descriptor from Java. */
    bool
    localDeviceBrokered() const override {
        return true;
    }

    bool localDeviceReady() const override;
    QString localDeviceStatus() const override;

    bool start(const QStringList& argv) override;
    void stop() override;
    void moveToBackground() override;
    void refresh() override;
    void requestLocalDeviceAccess() override;

    /** @brief Materialize a SAF content URI into cacheDir; returns "" on failure. */
    QString importContentUri(const QString& reference, const QString& fileName) override;

  private:
    void setStatus(const QString& text);
    void setLocalDeviceState(bool ready, const QString& text);

    bool m_running = false;
    QString m_status = QStringLiteral("Idle");
    bool m_usb_ready = false;
    QString m_usb_status;
};

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_DECODER_HOST_ANDROID_H_ */
