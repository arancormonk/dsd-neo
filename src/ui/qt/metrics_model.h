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
#include <QTimer>
#include <QtTypes>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

namespace dsd_qt {

class MetricsModel : public QObject {
    Q_OBJECT
    /* NOTIFY is grouped by what moves together, not one shared signal: the
     * per-second call timer and SNR jitter would otherwise re-evaluate every
     * binding in the status card on every poll tick. A signal-strip change must
     * not repaint the hero canvas, and a ticking call must not re-lay-out the
     * signal strip. */
    Q_PROPERTY(double snrDb READ snrDb NOTIFY tunerChanged)
    Q_PROPERTY(bool snrValid READ snrValid NOTIFY tunerChanged)
    Q_PROPERTY(bool carrierLock READ carrierLock NOTIFY tunerChanged)
    Q_PROPERTY(double cfoHz READ cfoHz NOTIFY tunerChanged)
    Q_PROPERTY(QString tunerGainText READ tunerGainText NOTIFY tunerChanged)
    Q_PROPERTY(bool radioInput READ radioInput NOTIFY tunerChanged)
    Q_PROPERTY(bool streamActive READ streamActive NOTIFY tunerChanged)
    Q_PROPERTY(int slot1CallState READ slot1CallState NOTIFY slot1Changed)
    Q_PROPERTY(int slot2CallState READ slot2CallState NOTIFY slot2Changed)
    Q_PROPERTY(QString slot1CallName READ slot1CallName NOTIFY slot1Changed)
    Q_PROPERTY(QString slot2CallName READ slot2CallName NOTIFY slot2Changed)
    Q_PROPERTY(QString slot1TgText READ slot1TgText NOTIFY slot1Changed)
    Q_PROPERTY(QString slot2TgText READ slot2TgText NOTIFY slot2Changed)
    Q_PROPERTY(qulonglong slot1TgId READ slot1TgId NOTIFY slot1Changed)
    Q_PROPERTY(qulonglong slot2TgId READ slot2TgId NOTIFY slot2Changed)
    Q_PROPERTY(QString slot1SrcText READ slot1SrcText NOTIFY slot1Changed)
    Q_PROPERTY(QString slot2SrcText READ slot2SrcText NOTIFY slot2Changed)
    Q_PROPERTY(bool slot1CallEnc READ slot1CallEnc NOTIFY slot1Changed)
    Q_PROPERTY(bool slot2CallEnc READ slot2CallEnc NOTIFY slot2Changed)
    Q_PROPERTY(QString slot1EncText READ slot1EncText NOTIFY slot1Changed)
    Q_PROPERTY(QString slot2EncText READ slot2EncText NOTIFY slot2Changed)
    Q_PROPERTY(int slot1CallSeconds READ slot1CallSeconds NOTIFY slot1Changed)
    Q_PROPERTY(int slot2CallSeconds READ slot2CallSeconds NOTIFY slot2Changed)
    Q_PROPERTY(bool audioMuted READ audioMuted NOTIFY controlChanged)
    Q_PROPERTY(qulonglong heldTg READ heldTg NOTIFY controlChanged)
    Q_PROPERTY(QString uiMessage READ uiMessage NOTIFY uiMessageChanged)

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

    /** @brief Whether the RTL sample stream is delivering right now (RTL inputs only). */
    bool
    streamActive() const {
        return m_view.stream_active;
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

    /**
     * @brief "ALG 84 · KID 0001" for an encrypted call whose header decoded.
     *
     * Empty when the call is clear or the algorithm was never learned; the ENC
     * tag alone covers that case. What the terminal UI's slot line showed, so
     * an operator can tell AES from RC4 at a glance.
     */
    const QString&
    slot1EncText() const {
        return m_view.slot_call[0].enc_text;
    }

    const QString&
    slot2EncText() const {
        return m_view.slot_call[1].enc_text;
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
     * @brief The engine's transient command acknowledgement, empty when none.
     *
     * Commands only enqueue a request; this is the engine saying what actually
     * happened ("Output: Muted", "Output: open failed"). Carried through the
     * snapshot with its expiry stamp, and cleared here when that stamp passes —
     * the display must not depend on the engine publishing again to take an
     * expired message down.
     */
    const QString&
    uiMessage() const {
        return m_view.ui_message;
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
    void tunerChanged();
    void slot1Changed();
    void slot2Changed();
    void controlChanged();
    void uiMessageChanged();

  private:
    /**
     * @brief Everything the model publishes, as one comparable value.
     *
     * Grouped so a refresh can build the new frame, compare it group by group and
     * signal only the groups that moved -- a decoder sitting idle publishes
     * nothing new for minutes at a time, and on a phone every needless binding
     * re-evaluation is work the battery pays for.
     *
     * It also gives clear() a single definition of "unknown": default-construct one
     * and assign it, rather than resetting members by hand and leaving the next
     * field added to be forgotten in one of the two places.
     */
    /** @brief One slot's structured call identity; see the slotNCall* properties. */
    struct SlotCall {
        int state = 0; // CallLineState values: 0 none, 1 idle, 2 active, 3 ended
        QString name;
        QString tg_text;
        QString src_text;
        QString enc_text;     // "ALG 84 · KID 0001", empty when clear or unlearned
        qulonglong tg_id = 0; // numeric talkgroup, 0 when the call has none
        bool enc = false;
        int seconds = 0;

        bool
        operator==(const SlotCall& other) const {
            return state == other.state && name == other.name && tg_text == other.tg_text && src_text == other.src_text
                   && enc_text == other.enc_text && tg_id == other.tg_id && enc == other.enc
                   && seconds == other.seconds;
        }
    };

    struct View {
        double snr_db = 0.0;
        bool snr_valid = false;
        bool carrier_lock = false;
        double cfo_hz = 0.0;
        QString tuner_gain_text;
        bool radio_input = false;
        bool stream_active = false;
        bool audio_muted = false;
        qulonglong held_tg = 0;
        QString ui_message;
        SlotCall slot_call[2];

        /* Exact comparison is right for the two doubles: they are carried through
         * unmodified from the metrics boundary, so "unchanged" means the identical
         * bits arrived again, not that two computations landed close together. A
         * tolerance here would suppress small real movements instead. */
        bool
        tunerEquals(const View& other) const {
            return snr_db == other.snr_db && snr_valid == other.snr_valid && carrier_lock == other.carrier_lock
                   && cfo_hz == other.cfo_hz && tuner_gain_text == other.tuner_gain_text
                   && radio_input == other.radio_input && stream_active == other.stream_active;
        }

        bool
        controlEquals(const View& other) const {
            return audio_muted == other.audio_muted && held_tg == other.held_tg;
        }
    };

    /** @brief Replace the published frame, signalling only the groups that moved. */
    void publish(const View& next);

    /** @brief Build one slot's structured call identity from the snapshot. */
    static SlotCall slotCallView(const dsd_state* snapshot, quint8 slot, double now_m);

    View m_view;
    /* Armed for a live ui_message's expiry stamp, so the message leaves the screen
     * on time even when the idle engine never publishes another frame. */
    QTimer m_messageTimer;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_METRICS_MODEL_H_ */
