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
    Q_PROPERTY(bool snrValid READ snrValid NOTIFY changed)
    Q_PROPERTY(int symbolRateHz READ symbolRateHz NOTIFY changed)
    Q_PROPERTY(int outputRateHz READ outputRateHz NOTIFY changed)
    Q_PROPERTY(bool carrierLock READ carrierLock NOTIFY changed)
    Q_PROPERTY(double cfoHz READ cfoHz NOTIFY changed)
    Q_PROPERTY(QString tunerGainText READ tunerGainText NOTIFY changed)
    Q_PROPERTY(bool radioInput READ radioInput NOTIFY changed)
    Q_PROPERTY(QString slot1Text READ slot1Text NOTIFY changed)
    Q_PROPERTY(QString slot2Text READ slot2Text NOTIFY changed)
    Q_PROPERTY(QString messageText READ messageText NOTIFY changed)

  public:
    explicit MetricsModel(QObject* parent = nullptr);
    ~MetricsModel() override;

    bool
    streamActive() const {
        return m_view.stream_active;
    }

    double
    snrDb() const {
        return m_view.snr_db;
    }

    /** @brief Whether an estimator has reported; @c snrDb means nothing when false. */
    bool
    snrValid() const {
        return m_view.snr_valid;
    }

    int
    symbolRateHz() const {
        return m_view.symbol_rate_hz;
    }

    int
    outputRateHz() const {
        return m_view.output_rate_hz;
    }

    bool
    carrierLock() const {
        return m_view.carrier_lock;
    }

    double
    cfoHz() const {
        return m_view.cfo_hz;
    }

    const QString&
    tunerGainText() const {
        return m_view.tuner_gain_text;
    }

    /**
     * @brief Whether a tuner sits under this session.
     *
     * Signal quality, carrier lock, frequency offset and tuner gain only mean
     * something when one does. For a PCM feed or a file there is no estimator and no
     * tuner to report, so a frontend should leave those rows out rather than render
     * a row of dashes that reads as a fault.
     */
    bool
    radioInput() const {
        return m_view.radio_input;
    }

    const QString&
    slot1Text() const {
        return m_view.slot_text[0];
    }

    const QString&
    slot2Text() const {
        return m_view.slot_text[1];
    }

    const QString&
    messageText() const {
        return m_view.message_text;
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
    /**
     * @brief Everything the model publishes, as one comparable value.
     *
     * Grouped so a refresh can build the new frame, compare it whole and signal only
     * when something moved. Every property above is NOTIFY changed, so emitting
     * unconditionally re-evaluates every binding in the status card on each poll
     * tick -- most of them onto the value they already held. A decoder sitting idle
     * publishes nothing new for minutes at a time, and on a phone that is work the
     * battery pays for.
     *
     * It also gives clear() a single definition of "unknown": default-construct one
     * and assign it, rather than resetting eleven members by hand and leaving the
     * next field added to be forgotten in one of the two places.
     */
    struct View {
        bool stream_active = false;
        double snr_db = 0.0;
        bool snr_valid = false;
        int symbol_rate_hz = 0;
        int output_rate_hz = 0;
        bool carrier_lock = false;
        double cfo_hz = 0.0;
        QString tuner_gain_text;
        bool radio_input = false;
        QString slot_text[2];
        QString message_text;

        bool
        operator==(const View& other) const {
            /* Exact comparison is right for the two doubles: they are carried through
             * unmodified from the metrics boundary, so "unchanged" means the identical
             * bits arrived again, not that two computations landed close together. A
             * tolerance here would suppress small real movements instead. */
            return stream_active == other.stream_active && snr_db == other.snr_db && snr_valid == other.snr_valid
                   && symbol_rate_hz == other.symbol_rate_hz && output_rate_hz == other.output_rate_hz
                   && carrier_lock == other.carrier_lock && cfo_hz == other.cfo_hz
                   && tuner_gain_text == other.tuner_gain_text && radio_input == other.radio_input
                   && slot_text[0] == other.slot_text[0] && slot_text[1] == other.slot_text[1]
                   && message_text == other.message_text;
        }
    };

    /** @brief Replace the published frame, signalling only if it actually moved. */
    void publish(const View& next);

    View m_view;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_METRICS_MODEL_H_ */
