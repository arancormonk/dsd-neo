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

  public:
    explicit DecoderHost(QObject* parent = nullptr);
    ~DecoderHost() override;

    /** @brief Whether the engine is configured and decoding. */
    virtual bool isRunning() const = 0;

    /** @brief Short human-readable host state (also used for platform notifications). */
    virtual QString statusText() const = 0;

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
     * Hosts that can background themselves do so here; the default does nothing,
     * leaving the close to proceed.
     */
    virtual void
    moveToBackground() {}

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

  Q_SIGNALS:
    void runningChanged();
    void statusTextChanged();
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DECODER_HOST_H_ */
