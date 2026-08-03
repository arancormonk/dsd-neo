// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Metrics and per-slot call summary exposed to QML.
 *
 * Reads only the app-control boundary: live metrics through the frontend API and
 * call identity through the published state snapshot. Refreshed from the single UI
 * poll tick (see ui_controller.h) — never from another thread.
 */

#ifndef DSD_NEO_SRC_UI_QT_METRICS_MODEL_H_
#define DSD_NEO_SRC_UI_QT_METRICS_MODEL_H_

#include <QObject>
#include <QString>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

namespace dsd_qt {

class MetricsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool streamActive READ streamActive NOTIFY changed)
    Q_PROPERTY(double snrDb READ snrDb NOTIFY changed)
    Q_PROPERTY(int symbolRateHz READ symbolRateHz NOTIFY changed)
    Q_PROPERTY(int outputRateHz READ outputRateHz NOTIFY changed)
    Q_PROPERTY(bool carrierLock READ carrierLock NOTIFY changed)
    Q_PROPERTY(double cfoHz READ cfoHz NOTIFY changed)
    Q_PROPERTY(QString tunerGainText READ tunerGainText NOTIFY changed)
    Q_PROPERTY(QString slot1Text READ slot1Text NOTIFY changed)
    Q_PROPERTY(QString slot2Text READ slot2Text NOTIFY changed)
    Q_PROPERTY(QString messageText READ messageText NOTIFY changed)

  public:
    explicit MetricsModel(QObject* parent = nullptr);
    ~MetricsModel() override;

    bool
    streamActive() const {
        return m_stream_active;
    }

    double
    snrDb() const {
        return m_snr_db;
    }

    int
    symbolRateHz() const {
        return m_symbol_rate_hz;
    }

    int
    outputRateHz() const {
        return m_output_rate_hz;
    }

    bool
    carrierLock() const {
        return m_carrier_lock;
    }

    double
    cfoHz() const {
        return m_cfo_hz;
    }

    const QString&
    tunerGainText() const {
        return m_tuner_gain_text;
    }

    const QString&
    slot1Text() const {
        return m_slot_text[0];
    }

    const QString&
    slot2Text() const {
        return m_slot_text[1];
    }

    const QString&
    messageText() const {
        return m_message_text;
    }

    /**
     * @brief Re-read the boundary. Call from the UI poll tick only.
     *
     * Takes the snapshots rather than fetching them so that one frame is built from
     * one generation: fetching here as well would consume a second time, and a
     * publish in between would leave the readings and the call lines disagreeing.
     *
     * @param opts_snapshot Options snapshot, or nullptr before the first publish.
     * @param snapshot      State snapshot, or nullptr before the first publish.
     */
    void refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot);

    /**
     * @brief Return every reading to its unknown state.
     *
     * Nothing upstream invalidates on stop: the telemetry hooks are torn down, so the
     * redraw flag never rises again and refresh() is never called, leaving the last
     * live SNR and carrier lock on screen for a decoder that is no longer running.
     * Showing nothing is honest; showing the readings from a minute ago is not.
     */
    void clear();

  Q_SIGNALS:
    void changed();

  private:
    bool m_stream_active = false;
    double m_snr_db = 0.0;
    int m_symbol_rate_hz = 0;
    int m_output_rate_hz = 0;
    bool m_carrier_lock = false;
    double m_cfo_hz = 0.0;
    QString m_tuner_gain_text;
    QString m_slot_text[2];
    QString m_message_text;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_METRICS_MODEL_H_ */
