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

// Complete types are needed by inline Qt container and metatype instantiations.
#include <QList> // IWYU pragma: keep
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant> // IWYU pragma: keep
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <cmath>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/p25_metrics.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>

namespace dsd_qt {

class MetricsModel : public QObject {
    Q_OBJECT
    // WP-F1: copied site identity, with one notification group.
    Q_PROPERTY(QString siteProtocol READ siteProtocol NOTIFY siteChanged)
    Q_PROPERTY(bool p25NacValid READ p25NacValid NOTIFY siteChanged)
    Q_PROPERTY(int p25Nac READ p25Nac NOTIFY siteChanged)
    Q_PROPERTY(bool p25WacnValid READ p25WacnValid NOTIFY siteChanged)
    Q_PROPERTY(int p25Wacn READ p25Wacn NOTIFY siteChanged)
    Q_PROPERTY(bool p25SysIdValid READ p25SysIdValid NOTIFY siteChanged)
    Q_PROPERTY(int p25SysId READ p25SysId NOTIFY siteChanged)
    Q_PROPERTY(int p25Rfss READ p25Rfss NOTIFY siteChanged)
    Q_PROPERTY(int p25Site READ p25Site NOTIFY siteChanged)
    Q_PROPERTY(bool p25LraValid READ p25LraValid NOTIFY siteChanged)
    Q_PROPERTY(int p25Lra READ p25Lra NOTIFY siteChanged)
    Q_PROPERTY(bool p25Phase2ParamsReady READ p25Phase2ParamsReady NOTIFY siteChanged)
    Q_PROPERTY(int dmrColorCode READ dmrColorCode NOTIFY siteChanged)
    Q_PROPERTY(QString dmrSiteText READ dmrSiteText NOTIFY siteChanged)
    Q_PROPERTY(int dmrRestLsn READ dmrRestLsn NOTIFY siteChanged)
    Q_PROPERTY(int nxdnRan READ nxdnRan NOTIFY siteChanged)
    Q_PROPERTY(QString nxdnLocationCategory READ nxdnLocationCategory NOTIFY siteChanged)
    Q_PROPERTY(int nxdnSysCode READ nxdnSysCode NOTIFY siteChanged)
    Q_PROPERTY(int nxdnSiteCode READ nxdnSiteCode NOTIFY siteChanged)
    Q_PROPERTY(QString edacsSiteText READ edacsSiteText NOTIFY siteChanged)
    Q_PROPERTY(double ccFreqHz READ ccFreqHz NOTIFY siteChanged)
    Q_PROPERTY(double vcFreqHz READ vcFreqHz NOTIFY siteChanged)
    Q_PROPERTY(QString siteLine READ siteLine NOTIFY siteChanged)
    Q_PROPERTY(bool siteConfirmed READ siteConfirmed NOTIFY siteChanged)

    /* WP-F3: one notification group for copied decode-quality readings. */
    Q_PROPERTY(bool qualityValid READ qualityValid NOTIFY qualityChanged)
    Q_PROPERTY(bool voiceErrsValid READ voiceErrsValid NOTIFY qualityChanged)
    Q_PROPERTY(double voiceErrsPerFrame READ voiceErrsPerFrame NOTIFY qualityChanged)
    Q_PROPERTY(int voiceErrsSamples READ voiceErrsSamples NOTIFY qualityChanged)
    Q_PROPERTY(bool slot1VoiceErrsValid READ slot1VoiceErrsValid NOTIFY qualityChanged)
    Q_PROPERTY(double slot1VoiceErrsPerFrame READ slot1VoiceErrsPerFrame NOTIFY qualityChanged)
    Q_PROPERTY(int slot1VoiceErrsSamples READ slot1VoiceErrsSamples NOTIFY qualityChanged)
    Q_PROPERTY(bool slot2VoiceErrsValid READ slot2VoiceErrsValid NOTIFY qualityChanged)
    Q_PROPERTY(double slot2VoiceErrsPerFrame READ slot2VoiceErrsPerFrame NOTIFY qualityChanged)
    Q_PROPERTY(int slot2VoiceErrsSamples READ slot2VoiceErrsSamples NOTIFY qualityChanged)
    Q_PROPERTY(bool ccFecValid READ ccFecValid NOTIFY qualityChanged)
    Q_PROPERTY(double ccFecOkPct READ ccFecOkPct NOTIFY qualityChanged)
    Q_PROPERTY(bool voiceFecValid READ voiceFecValid NOTIFY qualityChanged)
    Q_PROPERTY(double voiceFecOkPct READ voiceFecOkPct NOTIFY qualityChanged)
    Q_PROPERTY(bool rsValid READ rsValid NOTIFY qualityChanged)
    Q_PROPERTY(double rsOkPct READ rsOkPct NOTIFY qualityChanged)
    Q_PROPERTY(qulonglong ccFecOk READ ccFecOk NOTIFY qualityChanged)
    Q_PROPERTY(qulonglong ccFecErr READ ccFecErr NOTIFY qualityChanged)
    Q_PROPERTY(bool lastFrameErrsValid READ lastFrameErrsValid NOTIFY qualityChanged)
    Q_PROPERTY(int lastFrameErrs READ lastFrameErrs NOTIFY qualityChanged)
    Q_PROPERTY(int lastFrameErrs2 READ lastFrameErrs2 NOTIFY qualityChanged)

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
    Q_PROPERTY(int analogBandwidthHz READ analogBandwidthHz NOTIFY tunerChanged)
    Q_PROPERTY(bool analogBandwidthDspLimited READ analogBandwidthDspLimited NOTIFY tunerChanged)
    Q_PROPERTY(int analogBandwidthMaxHz READ analogBandwidthMaxHz NOTIFY tunerChanged)
    Q_PROPERTY(int analogBandwidthConfiguredHz READ analogBandwidthConfiguredHz NOTIFY controlChanged)
    Q_PROPERTY(QString analogBandwidthReading READ analogBandwidthReading NOTIFY tunerChanged)
    /* Issue #526: an analog scan row on air, and whether it sets its own width over the configured one. */
    Q_PROPERTY(bool analogBandwidthRowActive READ analogBandwidthRowActive NOTIFY tunerChanged)
    Q_PROPERTY(bool analogBandwidthRowOverride READ analogBandwidthRowOverride NOTIFY tunerChanged)
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
    Q_PROPERTY(bool slot1CallEmergency READ slot1CallEmergency NOTIFY slot1Changed)
    Q_PROPERTY(int slot1CallPriority READ slot1CallPriority NOTIFY slot1Changed)
    Q_PROPERTY(bool slot2CallEmergency READ slot2CallEmergency NOTIFY slot2Changed)
    Q_PROPERTY(int slot2CallPriority READ slot2CallPriority NOTIFY slot2Changed)
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
    Q_PROPERTY(bool persistTgLockouts READ persistTgLockouts NOTIFY controlChanged)
    Q_PROPERTY(qulonglong temporaryTgAvoidCount READ temporaryTgAvoidCount NOTIFY controlChanged)
    Q_PROPERTY(qulonglong callSkipCount READ callSkipCount NOTIFY controlChanged)
    Q_PROPERTY(QString tgPolicyContext READ tgPolicyContext NOTIFY controlChanged)
    Q_PROPERTY(bool tunerControlled READ tunerControlled NOTIFY controlChanged)
    Q_PROPERTY(bool trunkingEnabled READ trunkingEnabled NOTIFY controlChanged)
    Q_PROPERTY(bool scannerMode READ scannerMode NOTIFY controlChanged)
    Q_PROPERTY(bool optionsKnown READ optionsKnown NOTIFY controlChanged)
    Q_PROPERTY(bool scanRotationActive READ scanRotationActive NOTIFY controlChanged)
    Q_PROPERTY(int configuredForce READ configuredForce NOTIFY controlChanged)
    Q_PROPERTY(int effectiveForce READ effectiveForce NOTIFY controlChanged)
    Q_PROPERTY(QString keyProfileRef READ keyProfileRef NOTIFY controlChanged)
    Q_PROPERTY(QString keyEpoch READ keyEpoch NOTIFY controlChanged)
    Q_PROPERTY(bool automaticKeys READ automaticKeys NOTIFY controlChanged)
    Q_PROPERTY(bool directKeys READ directKeys NOTIFY controlChanged)
    Q_PROPERTY(QVariantList decryptionSlots READ decryptionSlots NOTIFY controlChanged)
    // WP-S1: copied from the tick's held snapshot.
    Q_PROPERTY(QString scanTargetId READ scanTargetId NOTIFY controlChanged)
    Q_PROPERTY(int scanTargetOrdinal READ scanTargetOrdinal NOTIFY controlChanged)
    Q_PROPERTY(int scanTargetCount READ scanTargetCount NOTIFY controlChanged)
    Q_PROPERTY(bool scanHold READ scanHold NOTIFY controlChanged)
    Q_PROPERTY(int scanAvoidCount READ scanAvoidCount NOTIFY controlChanged)
    Q_PROPERTY(bool scanTargetAvoided READ scanTargetAvoided NOTIFY controlChanged)
    /* #508: why the rotation is staying on the row on air, and how long is left.
       One group, one signal: they are read together by one row. */
    Q_PROPERTY(bool scanTimingVisible READ scanTimingVisible NOTIFY scanTimingChanged)
    Q_PROPERTY(int scanStayReason READ scanStayReason NOTIFY scanTimingChanged)
    Q_PROPERTY(QString scanStayPhrase READ scanStayPhrase NOTIFY scanTimingChanged)
    Q_PROPERTY(bool scanTimerLive READ scanTimerLive NOTIFY scanTimingChanged)
    Q_PROPERTY(int scanTimerRemainingDs READ scanTimerRemainingDs NOTIFY scanTimingChanged)
    Q_PROPERTY(quint32 scanTimerSpanMs READ scanTimerSpanMs NOTIFY scanTimingChanged)
    Q_PROPERTY(int scanDwellMs READ scanDwellMs NOTIFY scanTimingChanged)
    Q_PROPERTY(int scanDwellState READ scanDwellState NOTIFY scanTimingChanged)
    Q_PROPERTY(int scanHoldMs READ scanHoldMs NOTIFY scanTimingChanged)
    Q_PROPERTY(quint32 scanHangMs READ scanHangMs NOTIFY scanTimingChanged)
    /* #507: the ceiling on the whole visit, which rides the same row. */
    Q_PROPERTY(int scanVisitMs READ scanVisitMs NOTIFY scanTimingChanged)
    Q_PROPERTY(bool scanVisitLive READ scanVisitLive NOTIFY scanTimingChanged)
    Q_PROPERTY(int scanVisitRemainingDs READ scanVisitRemainingDs NOTIFY scanTimingChanged)
    /* #522: the sub-audible tone the analog FM monitor hears, from the shared app-control
       view, and the configured tone policy beside it -- never derived from each other. */
    Q_PROPERTY(bool rxToneVisible READ rxToneVisible NOTIFY rxToneChanged)
    Q_PROPERTY(int rxToneStatus READ rxToneStatus NOTIFY rxToneChanged)
    Q_PROPERTY(QString rxToneText READ rxToneText NOTIFY rxToneChanged)
    Q_PROPERTY(int rxToneKind READ rxToneKind NOTIFY rxToneChanged)
    Q_PROPERTY(int rxToneTenthsHz READ rxToneTenthsHz NOTIFY rxToneChanged)
    Q_PROPERTY(bool rxToneCarrier READ rxToneCarrier NOTIFY rxToneChanged)
    /* Its own signal: configuration, not something received, so a received-tone change never
       announces it and a policy change (#527) never announces the received tone. */
    Q_PROPERTY(QString rxToneConfiguredText READ rxToneConfiguredText NOTIFY rxToneConfiguredTextChanged)
    Q_PROPERTY(bool syncedHere READ syncedHere NOTIFY tunerChanged)
    Q_PROPERTY(QString syncLabel READ syncLabel NOTIFY tunerChanged)
    Q_PROPERTY(bool trunkableSync READ trunkableSync NOTIFY tunerChanged)
    Q_PROPERTY(int decodeMode READ decodeMode NOTIFY controlChanged)
    Q_PROPERTY(QString scanMode READ scanMode NOTIFY controlChanged)
    Q_PROPERTY(int modulation READ modulation NOTIFY controlChanged)
    Q_PROPERTY(QVariantMap airspy READ airspy NOTIFY controlChanged)
    Q_PROPERTY(int tunerGainDb READ tunerGainDb NOTIFY controlChanged)
    Q_PROPERTY(double squelchDb READ squelchDb NOTIFY controlChanged)
    Q_PROPERTY(bool squelchOff READ squelchOff NOTIFY controlChanged)
    /* #521: a scan row's --squelch-db. rtl_sql convention: 0 is off, otherwise dB; the off
     * flags and the readout text come from the app-control squelch view. */
    Q_PROPERTY(double configuredSquelchDb READ configuredSquelchDb NOTIFY controlChanged)
    Q_PROPERTY(double effectiveSquelchDb READ effectiveSquelchDb NOTIFY controlChanged)
    Q_PROPERTY(bool configuredSquelchOff READ configuredSquelchOff NOTIFY controlChanged)
    Q_PROPERTY(bool effectiveSquelchOff READ effectiveSquelchOff NOTIFY controlChanged)
    Q_PROPERTY(bool squelchRowOverride READ squelchRowOverride NOTIFY controlChanged)
    Q_PROPERTY(QString squelchReadout READ squelchReadout NOTIFY controlChanged)
    Q_PROPERTY(int ppm READ ppm NOTIFY controlChanged)
    Q_PROPERTY(QString uiMessage READ uiMessage NOTIFY uiMessageChanged)

  public:
    QString
    siteProtocol() const {
        return m_view.site.siteProtocol;
    }

    bool
    p25NacValid() const {
        return m_view.site.p25NacValid;
    }

    int
    p25Nac() const {
        return m_view.site.p25Nac;
    }

    bool
    p25WacnValid() const {
        return m_view.site.p25WacnValid;
    }

    int
    p25Wacn() const {
        return m_view.site.p25Wacn;
    }

    bool
    p25SysIdValid() const {
        return m_view.site.p25SysIdValid;
    }

    int
    p25SysId() const {
        return m_view.site.p25SysId;
    }

    int
    p25Rfss() const {
        return m_view.site.p25Rfss;
    }

    int
    p25Site() const {
        return m_view.site.p25Site;
    }

    bool
    p25LraValid() const {
        return m_view.site.p25LraValid;
    }

    int
    p25Lra() const {
        return m_view.site.p25Lra;
    }

    bool
    p25Phase2ParamsReady() const {
        return m_view.site.p25Phase2ParamsReady;
    }

    int
    dmrColorCode() const {
        return m_view.site.dmrColorCode;
    }

    QString
    dmrSiteText() const {
        return m_view.site.dmrSiteText;
    }

    int
    dmrRestLsn() const {
        return m_view.site.dmrRestLsn;
    }

    int
    nxdnRan() const {
        return m_view.site.nxdnRan;
    }

    QString
    nxdnLocationCategory() const {
        return m_view.site.nxdnLocationCategory;
    }

    int
    nxdnSysCode() const {
        return m_view.site.nxdnSysCode;
    }

    int
    nxdnSiteCode() const {
        return m_view.site.nxdnSiteCode;
    }

    QString
    edacsSiteText() const {
        return m_view.site.edacsSiteText;
    }

    double
    ccFreqHz() const {
        return m_view.site.ccFreqHz;
    }

    double
    vcFreqHz() const {
        return m_view.site.vcFreqHz;
    }

    QString
    siteLine() const {
        return m_view.site.siteLine;
    }

    bool
    siteConfirmed() const {
        return m_view.site.siteConfirmed;
    }

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
     * SNR, carrier lock, frequency offset and tuner gain only mean
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
     * @brief The analog channel width in force, in Hz (issue #525); 0 outside the configured analog preset with no
     * analog scan row on air, and on PCM input, where no channel filter runs.
     *
     * While a running stream runs the analog monitor, the width it reports (the configured width while its channel
     * filter runs, otherwise the width the DSP rate leaves: see analogBandwidthDspLimited()). Otherwise -- a stream not
     * running, a typed digital scan row filtering with its own profile -- the configured width, the kind's default when
     * none is set. App-control's analog width view decides it, for the terminal's "Analog:" status field too.
     */
    int
    analogBandwidthHz() const {
        return m_view.analog_bandwidth_hz;
    }

    /** @brief Whether the DSP rate, not the channel filter, bounds analogBandwidthHz(). */
    bool
    analogBandwidthDspLimited() const {
        return m_view.analog_bandwidth_dsp_limited;
    }

    /**
     * @brief The widest analog channel width the DSP rate filters, in Hz; 0 when unknown.
     *
     * Published under the analog preset on a radio input, from the demod rate a running stream reports
     * (dsd_analog_width_max_for_rate()), so the width control offers only steps the engine would take. With no
     * stream it is the rate an RTL-SDR or rtl_tcp input's DSP bandwidth gives the next start, which the engine holds
     * a width to; 0 on an input whose device or capture sets the rate.
     */
    int
    analogBandwidthMaxHz() const {
        return m_view.analog_bandwidth_max_hz;
    }

    /**
     * @brief The configured analog channel width in Hz, 0 for the default: what the width control edits.
     *
     * Configuration, not a reading, so it is published for any input; the control is only enabled on a radio.
     */
    int
    analogBandwidthConfiguredHz() const {
        return m_view.analog_bandwidth_configured_hz;
    }

    /**
     * @brief The analog width reading, as every frontend spells it: "12.5 kHz", "16 kHz (default)",
     * "12 kHz (DSP-limited)", "12.5 kHz (row; default 16 kHz)" while a scan row sets its own width, or "not used on PCM
     * input" (dsd_app_analog_width_view_format()); empty outside the configured analog preset with no analog scan row
     * on air.
     */
    QString
    analogBandwidthReading() const {
        return m_view.analog_bandwidth_reading;
    }

    /**
     * @brief Whether an analog (nfm) scan row is on air (issue #526): it runs the analog family, and so a width,
     * whatever the configured preset, digital sessions included.
     */
    bool
    analogBandwidthRowActive() const {
        return m_view.analog_bandwidth_row_active;
    }

    /**
     * @brief Whether the scan row on air sets its own width (--nfm-bandwidth-hz, issue #526): analogBandwidthHz() is
     * then the row's, in force until it leaves, while analogBandwidthConfiguredHz() stays what the width control
     * edits.
     */
    bool
    analogBandwidthRowOverride() const {
        return m_view.analog_bandwidth_row_override;
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
     * @brief The configured squelch default, which the panel's buttons edit (issue #521).
     *
     * In the rtl_sql convention: 0 when off, otherwise the threshold in dB. Equal to
     * effectiveSquelchDb() unless a scan row or target overrides the squelch.
     */
    double
    configuredSquelchDb() const {
        return m_view.configured_squelch_db;
    }

    /** @brief The squelch in force on the row on air; 0 when off, otherwise dB. */
    double
    effectiveSquelchDb() const {
        return m_view.effective_squelch_db;
    }

    /**
     * @brief Whether the configured default gates nothing.
     *
     * The view's own decision (dsd_squelch_is_off()), not configuredSquelchDb() >= 0: a legacy
     * linear default at full scale also reads 0 dB, and it gates everything.
     */
    bool
    configuredSquelchOff() const {
        return m_view.configured_squelch_off;
    }

    /** @brief Whether the squelch in force on the row on air gates nothing. */
    bool
    effectiveSquelchOff() const {
        return m_view.effective_squelch_off;
    }

    /** @brief Whether the row or target on air sets its own squelch (--squelch-db). */
    bool
    squelchRowOverride() const {
        return m_view.squelch_row_override;
    }

    /**
     * @brief The squelch readout as the terminal prints it, from dsd_app_squelch_view_format():
     * "-60.0 dB (row; default -80.0 dB)" under a row override, otherwise "-80.0 dB" or "off".
     * Empty without a radio input.
     */
    QString
    squelchReadout() const {
        return m_view.squelch_readout;
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
        return m_view.lead_slot + 1;
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
    slot1CallEmergency() const {
        return m_view.slot_call[0].emergency;
    }

    int
    slot1CallPriority() const {
        return m_view.slot_call[0].priority;
    }

    bool
    slot2CallEmergency() const {
        return m_view.slot_call[1].emergency;
    }

    int
    slot2CallPriority() const {
        return m_view.slot_call[1].priority;
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

    bool
    persistTgLockouts() const {
        return m_view.persist_tg_lockouts;
    }

    qulonglong
    callSkipCount() const {
        return m_view.call_skip_count;
    }

    qulonglong
    temporaryTgAvoidCount() const {
        return m_view.temporary_tg_avoid_count;
    }

    QString
    tgPolicyContext() const {
        return m_view.tg_policy_context;
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

    /** @brief Effective options have arrived; cleared scan flags alone are not authoritative. */
    bool
    optionsKnown() const {
        return m_view.options_known;
    }

    /** @brief The operator hold on the channel or target on air, read from the engine. */
    bool
    scanHold() const {
        return m_view.scan_hold;
    }

    /** @brief Active trunk-scan identity, copied from the held snapshot. */
    QString
    scanTargetId() const {
        return m_view.scan_target_id;
    }

    int
    scanTargetOrdinal() const {
        return m_view.scan_target_ordinal;
    }

    int
    scanTargetCount() const {
        return m_view.scan_target_count;
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
     * @brief Whether there is a scan stay reason to show at all (#508).
     *
     * False whenever no rotation is running, and false for a rotation that has
     * published nothing yet. Decided in app-control, not here, so this panel, the
     * terminal row and Android cannot disagree about when the row appears.
     */
    bool
    scanTimingVisible() const {
        return m_view.scan_timing_visible;
    }

    /** @brief dsd_scan_stay_reason for the row on air; the phrase is its label. */
    int
    scanStayReason() const {
        return m_view.scan_stay_reason;
    }

    /** @brief "Following call", "Idle dwell", "Manual hold" -- why it is staying. */
    const QString&
    scanStayPhrase() const {
        return m_view.scan_stay_phrase;
    }

    /** @brief A window is counting down; without it the stay has no deadline to show. */
    bool
    scanTimerLive() const {
        return m_view.scan_timer_live;
    }

    /**
     * @brief Time left in the running window, in tenths of a second.
     *
     * scanTimingChanged fires only when the rendered tenth changes. Countdown
     * updates have their own signal so they do not refresh unrelated controls.
     * Truncated, so it never claims more time than is left.
     */
    int
    scanTimerRemainingDs() const {
        return m_view.scan_timer_remaining_ds;
    }

    /** @brief Full width of the running window, so the countdown reads as a fraction. */
    quint32
    scanTimerSpanMs() const {
        return m_view.scan_timer_span_ms;
    }

    /** @brief Effective idle dwell for this row, 0 when it is not worth showing. */
    int
    scanDwellMs() const {
        return m_view.scan_dwell_ms;
    }

    /** @brief DSD_APP_SCAN_DWELL_*: hidden, suspended under a hold, or paused by the operator. */
    int
    scanDwellState() const {
        return m_view.scan_dwell_state;
    }

    /** @brief Effective activity hold; 0 on trunked rows, which have none. */
    int
    scanHoldMs() const {
        return m_view.scan_hold_ms;
    }

    /** @brief The active protocol's effective hangtime budget for the current stay. */
    quint32
    scanHangMs() const {
        return m_view.scan_hang_ms;
    }

    /**
     * @brief Effective per-visit cap for this row (#507), 0 when no cap applies.
     *
     * Unlike the dwell, hold and hang budgets this one is not chosen by the stay
     * reason: it caps the visit whatever is on the air, so it reads out beside the
     * reason rather than in place of it.
     */
    int
    scanVisitMs() const {
        return m_view.scan_visit_ms;
    }

    /**
     * @brief Whether the cap is counting down right now.
     *
     * False while a hold suspends it and before the visit has anchored. The row has
     * to say so in words: a zero countdown reads as a visit that just ran out.
     */
    bool
    scanVisitLive() const {
        return m_view.scan_visit_live;
    }

    /** @brief Time left on the cap, in tenths; see scanTimerRemainingDs() for why. */
    int
    scanVisitRemainingDs() const {
        return m_view.scan_visit_remaining_ds;
    }

    /**
     * @brief Whether the received-tone row belongs on screen (#522).
     *
     * True while the analog FM monitor runs, which is when detection runs. Decided in
     * app-control, so this row and the terminal's Call Info line appear together.
     */
    bool
    rxToneVisible() const {
        return m_view.rx_tone_visible;
    }

    /** @brief DSD_APP_RX_TONE_*: hidden, no carrier, detecting, locked or none. */
    int
    rxToneStatus() const {
        return m_view.rx_tone_status;
    }

    /** @brief "CTCSS 100.0 Hz", "detecting", "none" or an em dash; empty when hidden. */
    const QString&
    rxToneText() const {
        return m_view.rx_tone_text;
    }

    /** @brief dsd_analog_tone_kind of a locked tone, 0 otherwise. */
    int
    rxToneKind() const {
        return m_view.rx_tone_kind;
    }

    /** @brief The locked CTCSS tone in tenths of a hertz, 0 otherwise. */
    int
    rxToneTenthsHz() const {
        return m_view.rx_tone_tenths_hz;
    }

    /** @brief A carrier is open (held through the decoder's short hangover). */
    bool
    rxToneCarrier() const {
        return m_view.rx_tone_carrier;
    }

    /**
     * @brief The configured tone policy, kept apart from what is received.
     *
     * Reads "off" until tone filtering exists (#527). The monitor's reserved Tone filter row
     * binds to this, and nothing about the received tone ever changes it.
     */
    const QString&
    rxToneConfiguredText() const {
        return m_view.rx_tone_configured_text;
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

    /** Corrected errors per voice frame, never a bit-error percentage. */
    bool
    qualityValid() const {
        return m_view.quality.valid;
    }

    bool
    voiceErrsValid() const {
        return m_view.voice_errs.valid;
    }

    double
    voiceErrsPerFrame() const {
        return m_view.voice_errs.errs_per_frame;
    }

    int
    voiceErrsSamples() const {
        return m_view.voice_errs.samples;
    }

    bool
    slot1VoiceErrsValid() const {
        return m_view.quality.p2_voice[0].valid;
    }

    double
    slot1VoiceErrsPerFrame() const {
        return m_view.quality.p2_voice[0].errs_per_frame;
    }

    int
    slot1VoiceErrsSamples() const {
        return m_view.quality.p2_voice[0].samples;
    }

    bool
    slot2VoiceErrsValid() const {
        return m_view.quality.p2_voice[1].valid;
    }

    double
    slot2VoiceErrsPerFrame() const {
        return m_view.quality.p2_voice[1].errs_per_frame;
    }

    int
    slot2VoiceErrsSamples() const {
        return m_view.quality.p2_voice[1].samples;
    }

    bool
    ccFecValid() const {
        return m_view.quality.cc_fec.valid;
    }

    double
    ccFecOkPct() const {
        return m_view.quality.cc_fec.ok_pct;
    }

    bool
    voiceFecValid() const {
        return m_view.quality.voice_fec.valid;
    }

    double
    voiceFecOkPct() const {
        return m_view.quality.voice_fec.ok_pct;
    }

    bool
    rsValid() const {
        return m_view.quality.rs.valid;
    }

    double
    rsOkPct() const {
        return m_view.quality.rs.ok_pct;
    }

    qulonglong
    ccFecOk() const {
        return m_view.quality.cc_fec.ok;
    }

    qulonglong
    ccFecErr() const {
        return m_view.quality.cc_fec.err;
    }

    bool
    lastFrameErrsValid() const {
        return m_view.last_frame.valid;
    }

    int
    lastFrameErrs() const {
        return m_view.last_frame.errs;
    }

    int
    lastFrameErrs2() const {
        return m_view.last_frame.errs2;
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
    QVariantMap
    airspy() const {
        return m_view.airspy;
    }

    void refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot);

    int
    configuredForce() const {
        return m_view.configured_force;
    }

    int
    effectiveForce() const {
        return m_view.effective_force;
    }

    QString
    keyProfileRef() const {
        return m_view.key_profile_ref;
    }

    QString
    keyEpoch() const {
        return QString::number(m_view.key_epoch);
    }

    bool
    automaticKeys() const {
        return m_view.automatic_keys;
    }

    bool
    directKeys() const {
        return m_view.direct_keys;
    }

    QVariantList
    decryptionSlots() const {
        return m_view.decryption_slots;
    }

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
    void siteChanged();
    void qualityChanged();
    void tunerChanged();
    void slot1Changed();
    void slot2Changed();
    void leadSlotChanged();
    void controlChanged();
    void scanTimingChanged();
    void rxToneChanged();
    void rxToneConfiguredTextChanged();
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
        bool emergency = false;
        int priority = 0;
        int seconds = 0;
        double started_m = 0.0; // Lead ranking only; displayed duration equality uses seconds.

        bool
        operator==(const SlotCall& other) const {
            return state == other.state && name == other.name && tg_text == other.tg_text && src_text == other.src_text
                   && channel == other.channel && enc_text == other.enc_text && tg_id == other.tg_id && enc == other.enc
                   && seconds == other.seconds && emergency == other.emergency && priority == other.priority;
        }
    };

    struct SiteView {
        QString siteProtocol = {};
        bool p25NacValid = false;
        int p25Nac = 0;
        bool p25WacnValid = false;
        int p25Wacn = 0;
        bool p25SysIdValid = false;
        int p25SysId = 0;
        int p25Rfss = 0;
        int p25Site = 0;
        bool p25LraValid = false;
        int p25Lra = 0;
        bool p25Phase2ParamsReady = false;
        int dmrColorCode = -1;
        QString dmrSiteText = {};
        int dmrRestLsn = 0;
        int nxdnRan = -1;
        QString nxdnLocationCategory = {};
        int nxdnSysCode = 0;
        int nxdnSiteCode = 0;
        QString edacsSiteText = {};
        double ccFreqHz = 0;
        double vcFreqHz = 0;
        QString siteLine = {};
        bool siteConfirmed = false;
        bool operator==(const SiteView& other) const;
    };

    struct View {
        QVariantMap airspy;
        SiteView site;
        dsd_app_p25_quality quality{};
        dsd_app_voice_errs voice_errs{};
        dsd_app_frame_errs last_frame{};

        bool operator==(const View& other) const;
        bool qualityEquals(const View& other) const;

        /* Group equally aligned fields; this private value is copied on each
         * refresh and is never serialized or initialized by member position. */
        double snr_db = 0.0;
        double cfo_hz = 0.0;
        double center_freq_hz = 0.0;
        double squelch_db = 0.0;
        double configured_squelch_db = 0.0;
        double effective_squelch_db = 0.0;
        qulonglong held_tg = 0;
        qulonglong temporary_tg_avoid_count = 0;
        qulonglong call_skip_count = 0;
        QString tg_policy_context;
        QString tuner_gain_text;
        QString sync_label;
        QString scan_mode;
        QString ui_message;
        QString squelch_readout;
        /* Slot views and lead ranking share the canonical slot count. */
        SlotCall slot_call[DSD_CALL_STATE_SLOT_COUNT];
        int lead_slot = -1;
        int channel_bandwidth_hz = 0;
        int analog_bandwidth_hz = 0;
        int analog_bandwidth_configured_hz = 0;
        int analog_bandwidth_max_hz = 0;
        QString analog_bandwidth_reading;
        int decode_mode = 0;
        int configured_force = 0;
        int effective_force = 0;
        QString key_profile_ref;
        quint64 key_epoch = 0;
        bool automatic_keys = false;
        bool direct_keys = false;
        QVariantList decryption_slots;
        int modulation = 0;
        int tuner_gain_db = 0;
        int ppm = 0;
        int enc_lockout_count = 0;
        int scan_avoid_count = 0;
        bool persist_tg_lockouts = true;
        bool snr_valid = false;
        bool carrier_lock = false;
        bool radio_input = false;
        bool stream_active = false;
        bool analog_bandwidth_dsp_limited = false;
        bool analog_bandwidth_row_active = false;
        bool analog_bandwidth_row_override = false;
        bool synced_here = false;
        bool trunkable_sync = false;
        bool squelch_off = false;
        bool squelch_row_override = false;
        bool configured_squelch_off = false;
        bool effective_squelch_off = false;
        bool audio_muted = false;
        bool tuner_controlled = false;
        bool trunking_enabled = false;
        bool scanner_mode = false;
        bool options_known = false;
        bool scan_rotation_active = false;
        QString scan_target_id;
        int scan_target_ordinal = 0;
        int scan_target_count = 0;
        bool scan_hold = false;
        bool scan_target_avoided = false;
        /* #508: the stay reason and the live window, copied from the app-control view. */
        QString scan_stay_phrase;
        int scan_stay_reason = 0;
        int scan_timer_remaining_ds = 0;
        quint32 scan_timer_span_ms = 0;
        int scan_dwell_ms = 0;
        int scan_dwell_state = 0;
        int scan_hold_ms = 0;
        quint32 scan_hang_ms = 0;
        /* #507: the per-visit cap for the row on air, and whether it is counting. */
        int scan_visit_ms = 0;
        int scan_visit_remaining_ds = 0;
        bool scan_timing_visible = false;
        bool scan_timer_live = false;
        bool scan_visit_live = false;
        /* #522: the received tone and, separately, the configured policy. */
        QString rx_tone_text;
        QString rx_tone_configured_text;
        int rx_tone_status = 0;
        int rx_tone_kind = 0;
        int rx_tone_tenths_hz = 0;
        bool rx_tone_visible = false;
        bool rx_tone_carrier = false;

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
                   && analogChannelEquals(other) && synced_here == other.synced_here && sync_label == other.sync_label
                   && trunkable_sync == other.trunkable_sync;
        }

        /* The analog channel width in force (#525) rides tunerChanged with the rest; split out only so the
           comparison stays under the complexity ceiling. */
        bool
        analogChannelEquals(const View& other) const {
            return analog_bandwidth_hz == other.analog_bandwidth_hz
                   && analog_bandwidth_dsp_limited == other.analog_bandwidth_dsp_limited
                   && analog_bandwidth_max_hz == other.analog_bandwidth_max_hz
                   && analog_bandwidth_reading == other.analog_bandwidth_reading
                   && analog_bandwidth_row_active == other.analog_bandwidth_row_active
                   && analog_bandwidth_row_override == other.analog_bandwidth_row_override;
        }

        /* The scan controls (#380) ride controlChanged with the rest; split out only so
           neither comparison outgrows the complexity ceiling as readings are added. */
        bool
        scanControlEquals(const View& other) const {
            return options_known == other.options_known && scan_target_id == other.scan_target_id
                   && scan_target_ordinal == other.scan_target_ordinal && scan_target_count == other.scan_target_count
                   && scan_rotation_active == other.scan_rotation_active && scan_hold == other.scan_hold
                   && scan_avoid_count == other.scan_avoid_count && scan_target_avoided == other.scan_target_avoided;
        }

        /* Countdown updates notify only the scan timing row. */
        bool
        scanTimingEquals(const View& other) const {
            return scan_timing_visible == other.scan_timing_visible && scan_stay_reason == other.scan_stay_reason
                   && scan_stay_phrase == other.scan_stay_phrase && scan_timer_live == other.scan_timer_live
                   && scan_timer_remaining_ds == other.scan_timer_remaining_ds
                   && scan_timer_span_ms == other.scan_timer_span_ms && scan_dwell_ms == other.scan_dwell_ms
                   && scan_dwell_state == other.scan_dwell_state && scan_hold_ms == other.scan_hold_ms
                   && scan_hang_ms == other.scan_hang_ms && scan_visit_ms == other.scan_visit_ms
                   && scan_visit_live == other.scan_visit_live
                   && scan_visit_remaining_ds == other.scan_visit_remaining_ds;
        }

        /* The received group only; the configured text has its own signal and comparison. */
        bool
        rxToneEquals(const View& other) const {
            return rx_tone_visible == other.rx_tone_visible && rx_tone_status == other.rx_tone_status
                   && rx_tone_text == other.rx_tone_text && rx_tone_kind == other.rx_tone_kind
                   && rx_tone_tenths_hz == other.rx_tone_tenths_hz && rx_tone_carrier == other.rx_tone_carrier;
        }

        bool
        decryptionEquals(const View& other) const {
            return configured_force == other.configured_force && effective_force == other.effective_force
                   && key_profile_ref == other.key_profile_ref && key_epoch == other.key_epoch
                   && automatic_keys == other.automatic_keys && direct_keys == other.direct_keys
                   && decryption_slots == other.decryption_slots;
        }

        bool
        radioControlsEqual(const View& other) const {
            return modulation == other.modulation && tuner_gain_db == other.tuner_gain_db
                   && squelch_db == other.squelch_db && squelch_off == other.squelch_off && ppm == other.ppm
                   && airspy == other.airspy && squelchOverrideEquals(other)
                   && analog_bandwidth_configured_hz == other.analog_bandwidth_configured_hz;
        }

        /* The configured/effective pair is whole-dB configuration, not a measurement, so a
         * difference below a millionth of a dB is the same setting. */
        bool
        squelchOverrideEquals(const View& other) const {
            return std::fabs(configured_squelch_db - other.configured_squelch_db) < 1e-6
                   && std::fabs(effective_squelch_db - other.effective_squelch_db) < 1e-6
                   && squelch_row_override == other.squelch_row_override
                   && configured_squelch_off == other.configured_squelch_off
                   && effective_squelch_off == other.effective_squelch_off && squelch_readout == other.squelch_readout;
        }

        bool
        tgLockoutsEqual(const View& other) const {
            return persist_tg_lockouts == other.persist_tg_lockouts
                   && temporary_tg_avoid_count == other.temporary_tg_avoid_count
                   && call_skip_count == other.call_skip_count && tg_policy_context == other.tg_policy_context;
        }

        bool
        controlEquals(const View& other) const {
            return audio_muted == other.audio_muted && held_tg == other.held_tg
                   && enc_lockout_count == other.enc_lockout_count && tuner_controlled == other.tuner_controlled
                   && trunking_enabled == other.trunking_enabled && scanner_mode == other.scanner_mode
                   && scanControlEquals(other) && scan_mode == other.scan_mode && decode_mode == other.decode_mode
                   && decryptionEquals(other) && radioControlsEqual(other) && tgLockoutsEqual(other);
        }
    };

    /** @brief Replace the published frame, signalling only the groups that moved. */
    void publish(const View& next);
    static void fillP25Identity(SiteView& site, const dsd_state* snapshot);
    static QStringList p25SiteParts(const SiteView& site);
    static QStringList fillDmrSite(SiteView& site, const dsd_state* snapshot);
    static QStringList fillNxdnSite(SiteView& site, const dsd_state* snapshot);
    static QStringList fillEdacsSite(SiteView& site, const dsd_state* snapshot);
    void fillSiteView(View& next, const dsd_state* snapshot) const;
    static void fillSlotCalls(View& next, const dsd_state* snapshot, double now_m);
    static void fillQualityView(View& next, const dsd_state* snapshot);

    /** @brief Build one slot's structured call identity from the snapshot. */
    static SlotCall slotCallView(const dsd_state* snapshot, quint8 slot, double now_m);

    /** @brief Fill in sync state and the live decoder/front-end settings. */
    void fillDecoderView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot, double now_m);
    /** @brief The configured/effective squelch pair and the row badge (#521). */
    static void fillSquelchOverride(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot);
    /** @brief The analog channel width in force and the configured one (#525), and the nfm scan row on air (#526). */
    static void fillAnalogChannel(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot,
                                  const dsd_frontend_metrics& metrics);
    /** @brief Listening settings, talkgroup Hold, and lockout state from the held snapshot. */
    static void fillListeningControlView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot);
    /** @brief Scan hold and avoids (#380), read from whichever rotation is running. */
    static void fillScanControlView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot);
    /** @brief Why the rotation is staying on this row and how long is left (#508). */
    void fillScanTimingView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot, double now_m) const;
    /** @brief The received sub-audible tone and the configured tone policy (#522). */
    void fillRxToneView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot, double now_m) const;

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
    static QVariantMap airspyView(const dsd_opts* opts_snapshot);
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
