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
#include <QtGlobal>
#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>

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
    Q_PROPERTY(double centerFreqHz READ centerFreqHz NOTIFY tunerChanged)
    Q_PROPERTY(int channelBandwidthHz READ channelBandwidthHz NOTIFY tunerChanged)
    Q_PROPERTY(int slot1CallState READ slot1CallState NOTIFY slot1Changed)
    Q_PROPERTY(int slot2CallState READ slot2CallState NOTIFY slot2Changed)
    Q_PROPERTY(QString slot1CallName READ slot1CallName NOTIFY slot1Changed)
    Q_PROPERTY(QString slot2CallName READ slot2CallName NOTIFY slot2Changed)
    Q_PROPERTY(QString slot1Channel READ slot1Channel NOTIFY slot1Changed)
    Q_PROPERTY(QString slot2Channel READ slot2Channel NOTIFY slot2Changed)
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
    /* Its own signal rather than slot1Changed: it reads both slots, and Q_PROPERTY takes
       only one NOTIFY -- bound to either slot alone it would go stale when the other moved. */
    Q_PROPERTY(int leadSlot READ leadSlot NOTIFY leadSlotChanged)
    Q_PROPERTY(bool audioMuted READ audioMuted NOTIFY controlChanged)
    Q_PROPERTY(qulonglong heldTg READ heldTg NOTIFY controlChanged)
    Q_PROPERTY(int encLockoutCount READ encLockoutCount NOTIFY controlChanged)
    Q_PROPERTY(bool tunerControlled READ tunerControlled NOTIFY controlChanged)
    Q_PROPERTY(bool trunkingEnabled READ trunkingEnabled NOTIFY controlChanged)
    Q_PROPERTY(bool scannerMode READ scannerMode NOTIFY controlChanged)
    Q_PROPERTY(bool scanRotationActive READ scanRotationActive NOTIFY controlChanged)
    Q_PROPERTY(bool scanHold READ scanHold NOTIFY controlChanged)
    Q_PROPERTY(int scanAvoidCount READ scanAvoidCount NOTIFY controlChanged)
    Q_PROPERTY(bool scanTargetAvoided READ scanTargetAvoided NOTIFY controlChanged)
    Q_PROPERTY(bool syncedHere READ syncedHere NOTIFY tunerChanged)
    Q_PROPERTY(QString syncLabel READ syncLabel NOTIFY tunerChanged)
    Q_PROPERTY(bool trunkableSync READ trunkableSync NOTIFY tunerChanged)
    Q_PROPERTY(int decodeMode READ decodeMode NOTIFY controlChanged)
    Q_PROPERTY(QString scanMode READ scanMode NOTIFY controlChanged)
    Q_PROPERTY(int modulation READ modulation NOTIFY controlChanged)
    Q_PROPERTY(int tunerGainDb READ tunerGainDb NOTIFY controlChanged)
    Q_PROPERTY(double squelchDb READ squelchDb NOTIFY controlChanged)
    Q_PROPERTY(bool squelchOff READ squelchOff NOTIFY controlChanged)
    Q_PROPERTY(int ppm READ ppm NOTIFY controlChanged)
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
     * @brief The frequency the front end is tuned to, in Hz; 0 when there is none.
     *
     * A double rather than an integer because QML has no 64-bit integer type, and
     * every frequency a tuner reaches is exact in a double. Good for a readout and
     * for deciding what to ask for next — but a spectrum axis must use the center
     * carried inside the spectrum frame itself, which is the one that provably
     * matches those bins.
     */
    double
    centerFreqHz() const {
        return m_view.center_freq_hz;
    }

    /**
     * @brief Full width in Hz of the channel the demodulator is filtering.
     *
     * 12500 for the 12.5 kHz modes, 6250 for the narrow ones, 16000 wide — a
     * profile the demodulator has not yet narrowed reads as wide, not as zero.
     * 0 means there is nothing to draw at all: the input is not a radio, or no
     * metrics frame has been read yet. The channel, not the filter: see
     * dsd_channel_lpf_protected_edge_hz().
     */
    int
    channelBandwidthHz() const {
        return m_view.channel_bandwidth_hz;
    }

    /**
     * @brief Whether an automatic controller owns the tuner.
     *
     * True under trunking and under conventional scanner mode alike: both move
     * the front end on their own, and the engine refuses manual retunes under
     * either. Deliberately not named for trunking — a view that gated only on
     * that would offer a control which appears to work and is then undone by
     * the scanner's next step.
     */
    bool
    tunerControlled() const {
        return m_view.tuner_controlled;
    }

    /**
     * @brief Which owner is holding the tuner. For wording a message, never for gating.
     *
     * A control that is unavailable is unavailable for either reason, and anything
     * that gates on one of these alone offers something the other owner then undoes --
     * that is what tunerControlled() exists to prevent, and it stays the only reading
     * an affordance may bind to. These two are here so a message can name the reason
     * rather than say "something".
     */
    bool
    trunkingEnabled() const {
        return m_view.trunking_enabled;
    }

    bool
    scannerMode() const {
        return m_view.scanner_mode;
    }

    /**
     * @brief Whether the decoder currently has frame sync, held briefly.
     *
     * Held rather than sampled, because sync drops and returns between the 250 ms
     * polls and an instantaneous reading flickers; and held rather than latched,
     * because a lock that outlives the thing that produced it is worse than one
     * that flickers -- see kSyncHoldSeconds.
     *
     * Deliberately not call activity: a control channel carries no calls, and an
     * accepted retune ends both call slots anyway, so anything watching those
     * would read its own retune as a find.
     */
    bool
    syncedHere() const {
        return m_view.synced_here;
    }

    /**
     * @brief What the decoder locked onto here — "P25p1", "DMR", … — or empty.
     *
     * The protocol, not merely that there is one: on a band being walked, "DMR"
     * where P25 was expected is the answer to why nothing is being heard, and a
     * plain lock light would leave that invisible.
     */
    const QString&
    syncLabel() const {
        return m_view.sync_label;
    }

    /**
     * @brief Whether the lock here is on a protocol trunking can follow.
     *
     * What a control offering to hand the tuner to trunking needs: on M17 or
     * D-STAR there is no trunking to hand it to, and on noise there is nothing to
     * follow yet, so the offer would be a button that does nothing. Sync on a
     * trunked protocol is the weakest honest claim available here — the signalling
     * decides whether this carrier is really a control channel.
     */
    bool
    trunkableSync() const {
        return m_view.trunkable_sync;
    }

    /**
     * @brief Live front-end and decoder settings, for a panel that can change them.
     *
     * Read from the same options snapshot the commands mutate, so a control shows
     * where the engine actually is rather than what the UI last asked for — the
     * engine can refuse, and on Android the service outlives this process.
     *
     * decodeMode is a dsdneoUserDecodeMode; modulation is 0 for C4FM, 1 for QPSK
     * and 2 for GFSK, matching DSD_APP_CMD_MOD_SET's payload and dsd_state::rf_mod.
     */
    QString
    scanMode() const {
        return m_view.scan_mode;
    }

    int
    decodeMode() const {
        return m_view.decode_mode;
    }

    int
    modulation() const {
        return m_view.modulation;
    }

    int
    tunerGainDb() const {
        return m_view.tuner_gain_db;
    }

    double
    squelchDb() const {
        return m_view.squelch_db;
    }

    /**
     * @brief Whether the squelch is switched off rather than set low.
     *
     * squelchDb() bottoms out at the -120 dB display floor, which a threshold
     * genuinely set that low shares with a squelch that is not gating at all.
     * The panel needs to name the second case rather than print a number for it.
     */
    bool
    squelchOff() const {
        return m_view.squelch_off;
    }

    /**
     * @brief The dongle's crystal correction, in parts per million.
     *
     * Adjustable live because a wrong one is not obvious at the point it is
     * entered: the symptom is a frequency offset the decoder cannot close, which
     * only shows up once there is a signal to look at.
     */
    int
    ppm() const {
        return m_view.ppm;
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

    /**
     * @brief Which slot the hero should show: 1, 2, or 0 when nothing is on the air.
     *
     * One-based to match the slotN* properties QML reads it against, so 0 falls out as
     * "neither". The rule itself lives in dsd_app_lead_slot() and is shared with the
     * Android notification, which has the same one-call-at-a-time problem.
     */
    int
    leadSlot() const {
        int states[DSD_CALL_STATE_SLOT_COUNT];
        for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
            states[slot] = m_view.slot_call[slot].state;
        }
        return dsd_app_lead_slot(states, static_cast<unsigned>(DSD_CALL_STATE_SLOT_COUNT)) + 1;
    }

    const QString&
    slot1CallName() const {
        return m_view.slot_call[0].name;
    }

    const QString&
    slot2CallName() const {
        return m_view.slot_call[1].name;
    }

    /** @brief The scan channel the slot's call was heard on; empty when not scanning. */
    const QString&
    slot1Channel() const {
        return m_view.slot_call[0].channel;
    }

    const QString&
    slot2Channel() const {
        return m_view.slot_call[1].channel;
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
     * @brief Targets the encrypted lockout is currently skipping.
     *
     * The ledger's size at the current key epoch, which is a count of targets
     * confirmed undecryptable from voice — not a count of refusals. A control
     * channel repeats a grant update every few hundred ms for a call in
     * progress, so counting refused grants reported hundreds where a handful of
     * transmissions had happened. This exists because a site that is almost
     * entirely encrypted otherwise presents as a decoder that stopped: the
     * control channel decodes, every grant is declined, and no call is logged.
     */
    int
    encLockoutCount() const {
        return m_view.enc_lockout_count;
    }

    /**
     * @brief Whether a scan rotation is running that hold and avoid can act on.
     *
     * The -Y scan list or the --trunk-scan target list. Plain trunking follows one
     * system and is not a rotation, and tunerControlled() is true for it too, so the
     * scan controls gate on this rather than on the tuner gate.
     */
    bool
    scanRotationActive() const {
        return m_view.scan_rotation_active;
    }

    /** @brief The operator hold on the channel or target on air, read from the engine. */
    bool
    scanHold() const {
        return m_view.scan_hold;
    }

    /** @brief Channels or targets avoided for the session, whichever rotation is running. */
    int
    scanAvoidCount() const {
        return m_view.scan_avoid_count;
    }

    /**
     * @brief The receiver is parked on a --trunk-scan target the operator avoided,
     * because every alternate failed to retune. Never true under -Y.
     */
    bool
    scanTargetAvoided() const {
        return m_view.scan_target_avoided;
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
    void leadSlotChanged();
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
        QString channel;      // scan channel the call was heard on, empty when not scanning
        QString enc_text;     // "ALG 84 · KID 0001", empty when clear or unlearned
        qulonglong tg_id = 0; // numeric talkgroup, 0 when the call has none
        bool enc = false;
        int seconds = 0;

        bool
        operator==(const SlotCall& other) const {
            return state == other.state && name == other.name && tg_text == other.tg_text && src_text == other.src_text
                   && channel == other.channel && enc_text == other.enc_text && tg_id == other.tg_id && enc == other.enc
                   && seconds == other.seconds;
        }
    };

    struct View {
        /* Group equally aligned fields; this private value is copied on each
         * refresh and is never serialized or initialized by member position. */
        double snr_db = 0.0;
        double cfo_hz = 0.0;
        double center_freq_hz = 0.0;
        double squelch_db = 0.0;
        qulonglong held_tg = 0;
        QString tuner_gain_text;
        QString sync_label;
        QString scan_mode;
        QString ui_message;
        /* Sized from the canonical constant rather than a literal 2: leadSlot() ranks the
         * whole array through dsd_app_lead_slot(), so the two must agree or the ranking
         * would read past the end the day a third slot appears. */
        SlotCall slot_call[DSD_CALL_STATE_SLOT_COUNT];
        int channel_bandwidth_hz = 0;
        int decode_mode = 0;
        int modulation = 0;
        int tuner_gain_db = 0;
        int ppm = 0;
        int enc_lockout_count = 0;
        int scan_avoid_count = 0;
        bool snr_valid = false;
        bool carrier_lock = false;
        bool radio_input = false;
        bool stream_active = false;
        bool synced_here = false;
        bool trunkable_sync = false;
        bool squelch_off = false;
        bool audio_muted = false;
        bool tuner_controlled = false;
        bool trunking_enabled = false;
        bool scanner_mode = false;
        bool scan_rotation_active = false;
        bool scan_hold = false;
        bool scan_target_avoided = false;

        /* Exact comparison is right for the two doubles: they are carried through
         * unmodified from the metrics boundary, so "unchanged" means the identical
         * bits arrived again, not that two computations landed close together. A
         * tolerance here would suppress small real movements instead. */
        bool
        tunerEquals(const View& other) const {
            return snr_db == other.snr_db && snr_valid == other.snr_valid && carrier_lock == other.carrier_lock
                   && cfo_hz == other.cfo_hz && tuner_gain_text == other.tuner_gain_text
                   && radio_input == other.radio_input && stream_active == other.stream_active
                   && center_freq_hz == other.center_freq_hz && channel_bandwidth_hz == other.channel_bandwidth_hz
                   && synced_here == other.synced_here && sync_label == other.sync_label
                   && trunkable_sync == other.trunkable_sync;
        }

        /* The scan controls (#380) ride controlChanged with the rest; split out only so
           neither comparison outgrows the complexity ceiling as readings are added. */
        bool
        scanControlEquals(const View& other) const {
            return scan_rotation_active == other.scan_rotation_active && scan_hold == other.scan_hold
                   && scan_avoid_count == other.scan_avoid_count && scan_target_avoided == other.scan_target_avoided;
        }

        bool
        controlEquals(const View& other) const {
            return audio_muted == other.audio_muted && held_tg == other.held_tg
                   && enc_lockout_count == other.enc_lockout_count && tuner_controlled == other.tuner_controlled
                   && trunking_enabled == other.trunking_enabled && scanner_mode == other.scanner_mode
                   && scanControlEquals(other) && scan_mode == other.scan_mode && decode_mode == other.decode_mode
                   && modulation == other.modulation && tuner_gain_db == other.tuner_gain_db
                   && squelch_db == other.squelch_db && squelch_off == other.squelch_off && ppm == other.ppm;
        }
    };

    /** @brief Replace the published frame, signalling only the groups that moved. */
    void publish(const View& next);

    /** @brief Build one slot's structured call identity from the snapshot. */
    static SlotCall slotCallView(const dsd_state* snapshot, quint8 slot, double now_m);

    /** @brief Fill in sync state and the live decoder/front-end settings. */
    void fillDecoderView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot, double now_m);
    /** @brief Scan hold and avoids (#380), read from whichever rotation is running. */
    static void fillScanControlView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot);

  public:
#ifdef DSD_NEO_TEST_HOOKS
    /**
     * @brief Age the sync hold out, as if kSyncHoldSeconds had passed.
     *
     * The expiry is the part worth testing and the only part a test cannot reach,
     * short of sleeping for it in a suite that runs in milliseconds.
     */
    void
    expireSyncForTest() {
        m_sync_seen_m = 0.0;
    }
#endif

  private:
    View m_view;
    /* Held across frames rather than derived from one: frame sync comes and goes
     * between 250 ms polls, so a single sample answers "is it synced right now",
     * which is not the question. The hold decays on its own rather than being
     * cleared on a retune or a mode change — fillDecoderView() documents why a
     * one-shot reset was a race. See syncedHere(). */
    int m_sync_type_here = DSD_SYNC_NONE;
    double m_sync_seen_m = 0.0;
    /* Armed for a live ui_message's expiry stamp, so the message leaves the screen
     * on time even when the idle engine never publishes another frame. */
    QTimer m_messageTimer;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_METRICS_MODEL_H_ */
