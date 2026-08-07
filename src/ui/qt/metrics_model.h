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
    Q_PROPERTY(double snrDb READ snrDb NOTIFY changed)
    Q_PROPERTY(bool snrValid READ snrValid NOTIFY changed)
    Q_PROPERTY(bool carrierLock READ carrierLock NOTIFY changed)
    Q_PROPERTY(double cfoHz READ cfoHz NOTIFY changed)
    Q_PROPERTY(QString tunerGainText READ tunerGainText NOTIFY changed)
    Q_PROPERTY(bool radioInput READ radioInput NOTIFY changed)
    Q_PROPERTY(int slot1CallState READ slot1CallState NOTIFY changed)
    Q_PROPERTY(int slot2CallState READ slot2CallState NOTIFY changed)
    Q_PROPERTY(QString slot1CallName READ slot1CallName NOTIFY changed)
    Q_PROPERTY(QString slot2CallName READ slot2CallName NOTIFY changed)
    Q_PROPERTY(QString slot1TgText READ slot1TgText NOTIFY changed)
    Q_PROPERTY(QString slot2TgText READ slot2TgText NOTIFY changed)
    Q_PROPERTY(qulonglong slot1TgId READ slot1TgId NOTIFY changed)
    Q_PROPERTY(qulonglong slot2TgId READ slot2TgId NOTIFY changed)
    Q_PROPERTY(QString slot1SrcText READ slot1SrcText NOTIFY changed)
    Q_PROPERTY(QString slot2SrcText READ slot2SrcText NOTIFY changed)
    Q_PROPERTY(bool slot1CallEnc READ slot1CallEnc NOTIFY changed)
    Q_PROPERTY(bool slot2CallEnc READ slot2CallEnc NOTIFY changed)
    Q_PROPERTY(int slot1CallSeconds READ slot1CallSeconds NOTIFY changed)
    Q_PROPERTY(int slot2CallSeconds READ slot2CallSeconds NOTIFY changed)
    Q_PROPERTY(bool audioMuted READ audioMuted NOTIFY changed)
    Q_PROPERTY(qulonglong heldTg READ heldTg NOTIFY changed)

  public:
    explicit MetricsModel(QObject* parent = nullptr);
    ~MetricsModel() override;

    double
    snrDb() const {
        return m_view.snr_db;
    }

    /** @brief Whether an estimator has reported; @c snrDb means nothing when false. */
    bool
    snrValid() const {
        return m_view.snr_valid;
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

    /**
     * @brief Structured call identity per slot, for the monitor's hero panel.
     *
     * The state values mirror CallLineState (0 none, 1 idle, 2 active, 3 recently
     * ended); the strings are display-ready so QML never parses a slot line.
     */
    int
    slot1CallState() const {
        return m_view.slot_call[0].state;
    }

    int
    slot2CallState() const {
        return m_view.slot_call[1].state;
    }

    const QString&
    slot1CallName() const {
        return m_view.slot_call[0].name;
    }

    const QString&
    slot2CallName() const {
        return m_view.slot_call[1].name;
    }

    const QString&
    slot1TgText() const {
        return m_view.slot_call[0].tg_text;
    }

    const QString&
    slot2TgText() const {
        return m_view.slot_call[1].tg_text;
    }

    const QString&
    slot1SrcText() const {
        return m_view.slot_call[0].src_text;
    }

    const QString&
    slot2SrcText() const {
        return m_view.slot_call[1].src_text;
    }

    /**
     * @brief Numeric talkgroup id per slot, 0 when the call has none.
     *
     * The tg_text properties are display strings — for M17/D-STAR/YSF/dPMR they
     * carry callsigns or dial strings that no command can act on. Commands that
     * need a talkgroup number (hold) read this instead and disable themselves
     * when it is 0, rather than parse a string that was never a number.
     */
    qulonglong
    slot1TgId() const {
        return m_view.slot_call[0].tg_id;
    }

    qulonglong
    slot2TgId() const {
        return m_view.slot_call[1].tg_id;
    }

    bool
    slot1CallEnc() const {
        return m_view.slot_call[0].enc;
    }

    bool
    slot2CallEnc() const {
        return m_view.slot_call[1].enc;
    }

    int
    slot1CallSeconds() const {
        return m_view.slot_call[0].seconds;
    }

    int
    slot2CallSeconds() const {
        return m_view.slot_call[1].seconds;
    }

    /**
     * @brief Whether the engine's audio output is muted.
     *
     * Engine truth, not a UI-side toggle mirror: the mute command only enqueues a
     * request, and the Android service (which owns the state) outlives the
     * Activity. A relaunched UI binds its Mute button to this and stays correct.
     */
    bool
    audioMuted() const {
        return m_view.audio_muted;
    }

    /** @brief Talkgroup the engine is holding on, 0 when no hold is set. */
    qulonglong
    heldTg() const {
        return m_view.held_tg;
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
    /** @brief One slot's structured call identity; see the slotNCall* properties. */
    struct SlotCall {
        int state = 0; // CallLineState values: 0 none, 1 idle, 2 active, 3 ended
        QString name;
        QString tg_text;
        QString src_text;
        qulonglong tg_id = 0; // numeric talkgroup, 0 when the call has none
        bool enc = false;
        int seconds = 0;

        bool
        operator==(const SlotCall& other) const {
            return state == other.state && name == other.name && tg_text == other.tg_text && src_text == other.src_text
                   && tg_id == other.tg_id && enc == other.enc && seconds == other.seconds;
        }
    };

    struct View {
        double snr_db = 0.0;
        bool snr_valid = false;
        bool carrier_lock = false;
        double cfo_hz = 0.0;
        QString tuner_gain_text;
        bool radio_input = false;
        bool audio_muted = false;
        qulonglong held_tg = 0;
        SlotCall slot_call[2];

        bool
        operator==(const View& other) const {
            /* Exact comparison is right for the two doubles: they are carried through
             * unmodified from the metrics boundary, so "unchanged" means the identical
             * bits arrived again, not that two computations landed close together. A
             * tolerance here would suppress small real movements instead. */
            return snr_db == other.snr_db && snr_valid == other.snr_valid && carrier_lock == other.carrier_lock
                   && cfo_hz == other.cfo_hz && tuner_gain_text == other.tuner_gain_text
                   && radio_input == other.radio_input && audio_muted == other.audio_muted && held_tg == other.held_tg
                   && slot_call[0] == other.slot_call[0] && slot_call[1] == other.slot_call[1];
        }
    };

    /** @brief Replace the published frame, signalling only if it actually moved. */
    void publish(const View& next);

    /** @brief Build one slot's structured call identity from the snapshot. */
    static SlotCall slotCallView(const dsd_state* snapshot, quint8 slot, double now_m);

    View m_view;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_METRICS_MODEL_H_ */
