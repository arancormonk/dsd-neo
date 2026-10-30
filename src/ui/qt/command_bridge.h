// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Curated app-control command surface exposed to QML.
 *
 * Commands land in a 128-slot ring that only drains while the engine loop turns, so
 * the UI must not submit while the decoder is stopped.
 */

#ifndef DSD_NEO_SRC_UI_QT_COMMAND_BRIDGE_H_
#define DSD_NEO_SRC_UI_QT_COMMAND_BRIDGE_H_

// Complete types are needed by inline Qt container and metatype instantiations.
#include <QList>
#include <QObject>
#include <QSharedPointer> // IWYU pragma: keep
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <dsd-neo/app_control/recording_view.h>
class QTemporaryFile;

namespace dsd_qt {
class DecryptionProfileProvider;
class RecordingStatus;

class CommandBridge : public QObject {
    Q_OBJECT
    /* Issue #518: the session-edit fields and actions (runtime/scan_row_edit.h) "this channel" editors pass to
     * editScanRow(), and test against metrics.scanRowEditable / scanRowEdited / scanRowListed. */
    Q_PROPERTY(int scanRowFieldSquelch READ scanRowFieldSquelch CONSTANT)
    Q_PROPERTY(int scanRowFieldWidth READ scanRowFieldWidth CONSTANT)
    Q_PROPERTY(int scanRowFieldTone READ scanRowFieldTone CONSTANT)
    Q_PROPERTY(int scanRowFieldGain READ scanRowFieldGain CONSTANT)
    Q_PROPERTY(int scanRowEditSet READ scanRowEditSet CONSTANT)
    Q_PROPERTY(int scanRowEditInherit READ scanRowEditInherit CONSTANT)
    Q_PROPERTY(int scanRowEditReset READ scanRowEditReset CONSTANT)
    /* The squelch modes a row edit names (core/power.h dsd_squelch_mode) and the auto squelch's margin range, in
     * whole dB over the learned floor (issue #518 follow-up). */
    Q_PROPERTY(int squelchModeLevel READ squelchModeLevel CONSTANT)
    Q_PROPERTY(int squelchModeAuto READ squelchModeAuto CONSTANT)
    Q_PROPERTY(int squelchModeNoise READ squelchModeNoise CONSTANT)
    Q_PROPERTY(int squelchMarginMinDb READ squelchMarginMinDb CONSTANT)
    Q_PROPERTY(int squelchMarginMaxDb READ squelchMarginMaxDb CONSTANT)
    Q_PROPERTY(int squelchMarginDefaultDb READ squelchMarginDefaultDb CONSTANT)

  public:
    static int scanRowFieldSquelch();
    static int scanRowFieldWidth();
    static int scanRowFieldTone();
    static int scanRowFieldGain();
    static int scanRowEditSet();
    static int scanRowEditInherit();
    static int scanRowEditReset();
    static int squelchModeLevel();
    static int squelchModeAuto();
    static int squelchModeNoise();
    static int squelchMarginMinDb();
    static int squelchMarginMaxDb();
    static int squelchMarginDefaultDb();

    /**
     * @brief The scan row on air, captured when a "this channel" editor opens (issue #518): active, scanner, session,
     * row, mode, target and label. An edit names it, so it reaches that row however the scan has moved on since, and is
     * refused once that scan has ended. Empty-handed ({active: false}) with no row on air.
     */
    Q_INVOKABLE QVariantMap scanRowContext() const;
    Q_INVOKABLE QVariantMap receiveContext() const;
    Q_INVOKABLE QString receiveError(int kind, bool enabled, const QString& allow, const QString& block,
                                     const QVariantList& names) const;
    Q_INVOKABLE bool setReceive(int kind, bool enabled, const QString& allow, const QString& block,
                                const QVariantList& names, const QVariantMap& row, int action) const;
    Q_INVOKABLE int receiveField(int kind) const;
    /**
     * @brief Submit a session edit of one @p field (a scanRowField*) of the row @p context names, with @p action (a
     * scanRowEdit*). For SET, @p value carries the field's value: squelchDb (whole dB, -100..0; 0 = off) or
     * squelchMode squelchModeAuto with squelchMarginDb (an nfm or am row's auto squelch), squelchMode
     * squelchModeNoise with squelchMarginDb (an nfm row's noise squelch), widthHz, toneMode with
     * toneList, or gainDb (0 = AGC). Returns whether it was queued; the decoder refuses what the row
     * cannot take, with a message.
     */
    Q_INVOKABLE bool editScanRow(const QVariantMap& context, int field, int action, const QVariantMap& value) const;

    void
    setDecryptionProfiles(DecryptionProfileProvider* profiles) {
        m_profiles = profiles;
    }

    void acknowledgeDecryptionResult(quint64 requestId);
    Q_INVOKABLE QVariantMap decryptionContext(const QString& targetId, const QString& keyEpoch) const;
    Q_INVOKABLE QString applyDecryptionDraft(const QString& type, const QString& value, bool forceChanged, int force,
                                             const QVariantMap& context);
    Q_INVOKABLE QString applyDecryptionProfile(const QString& uid, int scope, const QVariantMap& context);
    Q_INVOKABLE QString applyDmrKeyMap(const QString& path, int scope, const QVariantMap& context);
    Q_INVOKABLE bool setTalkgroupSelection(bool listening, const QString& context, unsigned int generation,
                                           const QVariantList& rows);
    void clearSessionInputs();
    explicit CommandBridge(QObject* parent = nullptr);
    ~CommandBridge() override;

    // WP-D1: callers capture the model version when opening an edit sheet.
    // B14: a listed row's edit also names the row (its tableIndex and, on a pattern row, its pattern with 0/0 bounds),
    // so a duplicate or a pattern row is edited as listed; tableIndex -1 is the bounds-only edit.
    /** Changed fields only: name, listening, priority, preempt, record. One map is one atomic edit; the decoder applies
     *  record after listening, and a row that does not listen records nothing. */
    Q_INVOKABLE bool setTalkgroupPolicy(unsigned int idStart, unsigned int idEnd, const QString& context,
                                        unsigned int generation, const QVariantMap& changes, int tableIndex = -1,
                                        const QString& pattern = QString()) const;
    Q_INVOKABLE bool renameTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                                     unsigned int generation, const QString& name, int tableIndex = -1,
                                     const QString& pattern = QString()) const;
    /** Adds a heard talkgroup (never a listed row) to the list; @p record (issue #708) is its record flag, sent only for
     *  a row that listens (one that does not records nothing). */
    Q_INVOKABLE bool addTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                                  unsigned int generation, const QString& name, bool listen, int priority, bool preempt,
                                  bool record = true) const;
    Q_INVOKABLE bool removeTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                                     unsigned int generation, int tableIndex = -1,
                                     const QString& pattern = QString()) const;
    Q_INVOKABLE bool saveTalkgroupList(const QString& context, unsigned int generation, const QString& path) const;

    /** @brief Toggle audio mute. @return true when the command was accepted. */
    Q_INVOKABLE bool toggleMute() const;

    /** @brief Hold decoding on one talkgroup; 0 releases the hold. */
    Q_INVOKABLE bool holdTalkgroup(unsigned int talkgroup) const;

    /** @brief Lock out the target currently active on @p slot (0 or 1). */
    Q_INVOKABLE bool lockoutSlot(int slot) const;
    /** @brief Skip the call currently on @p slot (0 or 1) without editing the talkgroup list. */
    Q_INVOKABLE bool skipSlot(int slot) const;
    Q_INVOKABLE bool setPersistTgLockouts(bool persist) const;
    /** @brief Set Relaxed CRC (-F) on or off: the configured value, beneath a scan row's own while one is on air. */
    Q_INVOKABLE bool setRelaxedCrc(bool on) const;
    /**
     * @brief Start or stop recording each call in the running session (issue #708, D8), into the folder the session
     * was started with. On sends @p unknownOnly and then WAV start; off sends unknown-only off and then WAV stop
     * (unknown-only travels with Record calls: an off switch filters no recording of the user's own options).
     *
     * Refused without sending anything, and said in RecordingStatus::refusal(), when no session is on ("no-session"),
     * while its run is not up yet ("starting"),
     * when on would record into a session with no folder ("storage-unavailable"), or either way when the session
     * records to one static WAV from its extra options ("static-wav": a start or a stop would close the user's file);
     * the preference then applies at the next start. Queued like every command; the decoder holds it while a carrier loop
     * of its own runs and refuses it during Signal search, which RecordingStatus shows.
     */
    Q_INVOKABLE bool setRecordCalls(bool on, bool unknownOnly) const;
    /**
     * @brief Record only calls no talkgroup row knows, or every call (issue #708). Sent only while the session records
     * each call (or a Record calls on is still applying); otherwise nothing is sent and it answers true: the switch
     * travels with the next Record calls on.
     */
    Q_INVOKABLE bool setRecordUnknownOnly(bool on) const;

    /** @brief Where a refused recording switch says why; nullptr: nowhere. */
    void
    setRecordingStatus(RecordingStatus* status) {
        m_recordingStatus = status;
    }

    Q_INVOKABLE bool clearTemporaryTgAvoids(const QString& context) const;

    /**
     * @brief Listen to or stop tuning one talkgroup or range; a missing exact id is added.
     *
     * A listed row passes the captured @p context / @p generation and its row token (@p tableIndex, @p pattern); a
     * stale capture is refused by the decoder. Without a context the edit is bounds-only and unchecked.
     */
    Q_INVOKABLE bool setTalkgroupListening(unsigned int idStart, unsigned int idEnd, bool listen,
                                           const QString& context = QString(), unsigned int generation = 0,
                                           int tableIndex = -1, const QString& pattern = QString()) const;

    /** @brief Edit every listed talkgroup, or only those tagged @p tag when non-empty. */
    Q_INVOKABLE bool setAllTalkgroupsListening(bool listen, const QString& tag) const;

    /** @brief Forget every encrypted-target lockout. */
    Q_INVOKABLE bool clearEncLockouts() const;

    /**
     * @brief On-the-fly scan controls (#380), for the -Y scan list or the --trunk-scan
     * target list, whichever is running. The engine answers with a message in
     * MetricsModel::uiMessage when it declines (nothing on air, last usable entry).
     */
    /** @brief Pause or resume the rotation on the channel/target on air. */
    Q_INVOKABLE bool toggleScanHold() const;
    /** @brief Avoid the channel/target on air for the session and step to the next one. */
    Q_INVOKABLE bool avoidCurrentChannel() const;
    /** @brief Put every avoided channel/target back into the rotation. */
    Q_INVOKABLE bool clearScanAvoids() const;
    /** @brief Step to the next channel now (next trunk-scan target under --trunk-scan). */
    Q_INVOKABLE bool nextChannel() const;
    /**
     * @brief Resume a scan the operator paused (issue #671): the -Y list from its kept cursor, or the trunk scan on
     * the target it left. The engine refuses a running scan or none with a message in MetricsModel::uiMessage.
     */
    Q_INVOKABLE bool resumeScan() const;

    /**
     * @brief Tune the radio front end to a frequency the operator entered (the spectrum's Go to).
     *
     * The override tune: it wins over a running -Y scan or trunk scan, which pauses until resumeScan(), and a paused
     * scan stays paused (issue #671). Nothing else that owns the tuner gives way to it.
     */
    Q_INVOKABLE bool tuneHz(unsigned int hz) const;
    Q_INVOKABLE bool setAirspy(const QString& key, const QString& value) const;

    /**
     * @brief Retune from a spectrum tap.
     *
     * Distinct from tuneHz(): taps coalesce only with taps, the decoder resets
     * its auto-modulation votes and call state so it re-acquires on the new
     * frequency, and the engine refuses the tune outright while trunking owns
     * the tuner.
     */
    Q_INVOKABLE bool manualTuneHz(unsigned int hz) const;

    /**
     * @brief Stop trunking from moving the tuner, and pause a running scan.
     *
     * Idempotent, and deliberately not a toggle: a view only learns that
     * something owns the tuner, never which of the two, so it cannot safely
     * flip either flag on its own. Also tears down the follow state the trunker
     * left behind, so the decoder can acquire whatever it is pointed at next.
     * A -Y scan or trunk scan pauses rather than stops (issue #671), keeping
     * what resumeScan() picks up again.
     */
    Q_INVOKABLE bool releaseTuner() const;

    /**
     * @brief Hand the tuner to trunking (@a on true), or stop it following.
     *
     * The inverse of releaseTuner() for the one flag a view can name. Turning it
     * on also clears scanner mode engine-side, since both cannot drive the tuner.
     * Off is the narrow half — it leaves scanner mode alone, so releaseTuner()
     * stays the way to ask for the tuner back without knowing what holds it.
     */
    Q_INVOKABLE bool setTrunking(bool on) const;

    /** @brief Set tuner gain in dB (0 selects automatic gain). */
    Q_INVOKABLE bool setTunerGain(int gain_db) const;

    /** @brief Set the RTL power squelch threshold, in dB. */
    Q_INVOKABLE bool setSquelchDb(double db) const;

    /**
     * @brief Set the auto squelch: open @p marginDb (squelchMarginMinDb..squelchMarginMaxDb) over the noise floor
     * the demodulator learns on each channel. Like setSquelchDb() it edits the configured default.
     */
    Q_INVOKABLE bool setSquelchAuto(int marginDb) const;

    /**
     * @brief Set the noise squelch: open an FM channel when its discriminator noise above voice quiets by @p marginDb
     * (squelchMarginMinDb..squelchMarginMaxDb). Like setSquelchDb() it edits the configured default; the AM monitor
     * refuses it.
     */
    Q_INVOKABLE bool setSquelchNoise(int marginDb) const;

    /**
     * @brief Put the configured default back on the level it kept beneath a dynamic squelch, exactly as stored (a
     * legacy linear threshold included, which a whole-dB reading cannot carry).
     */
    Q_INVOKABLE bool restoreSquelchLevel() const;

    /** @brief Set the dongle's crystal correction, in parts per million. */
    Q_INVOKABLE bool setPpm(int ppm) const;

    /**
     * @brief Set the NFM channel-filter width in Hz (8000..25000), or 0 for the default (issue #525).
     *
     * The engine refuses a width the front end cannot filter at its DSP rate, with a message, and keeps
     * the one it had; a view reads the width back rather than trusting the request.
     */
    Q_INVOKABLE bool setNfmBandwidthHz(int hz) const;

    /**
     * @brief Set the AM channel-filter width in Hz (5000..20000), or 0 for the 6 kHz default (issue #524).
     *
     * The engine refuses a width the front end cannot filter at its DSP rate, with a message, and keeps the one it
     * had; a view reads the width back rather than trusting the request.
     */
    Q_INVOKABLE bool setAmBandwidthHz(int hz) const;

    /**
     * @brief Set the configured CTCSS/DCS tone filter (issue #527): @p mode 0 off, 1 allow, 2 block, with its
     * '/'-separated list of tones and codes as typed ("67.0/100.0/D023N"; empty for none, which only off takes).
     *
     * The decoder checks the list with the same parser as toneFilterError() and refuses what it cannot apply, with a
     * message, keeping the policy it had. The edit sets the configured policy: a scan row with its own tone options
     * keeps them while on air. A list longer than the command carries is refused here, without submitting anything.
     */
    Q_INVOKABLE bool setToneFilter(int mode, const QString& list) const;

    /**
     * @brief Why the decoder would refuse setToneFilter() with @p mode and @p list, or an empty string when it would
     * apply it: the list parser's own message, naming entries by number and never repeating the text, for the editor to
     * show inline.
     */
    Q_INVOKABLE QString toneFilterError(int mode, const QString& list) const;

    /**
     * @brief Choose the demodulator: 0 for C4FM, 1 for QPSK, 2 for GFSK.
     *
     * A setter, not the hotkey's cycle: a control showing them as choices has to
     * be able to ask for the one it is not on without guessing at a state the
     * engine may have moved since. Anything outside 0..2 is refused here rather
     * than clamped, so a caller's mistake does not silently land on C4FM.
     */
    Q_INVOKABLE bool setModulation(int modulation) const;

    /**
     * @brief Switch which protocols are decoded, live.
     *
     * @param mode A dsdneoUserDecodeMode. Use decodeModeForFlag() to get one from
     *        the same decode-chip flag a saved system stores, so the mapping from
     *        flag to preset exists once and is the CLI's own.
     */
    Q_INVOKABLE bool setDecodeMode(int mode) const;

    /**
     * @brief The decode mode a `-f<x>` chip flag selects, or -1 when it selects none.
     *
     * "-mq" is a modulation chip, not a decode one, and answers -1: it says how to
     * demodulate, not what to look for.
     */
    Q_INVOKABLE int decodeModeForFlag(const QString& flag) const;

    /** @brief Cycle the event-history display mode. */
    Q_INVOKABLE int cycleHistoryMode() const;

    /**
     * @brief Import a channel map CSV into the running session.
     *
     * The runtime handler replaces the previous map atomically; a file that
     * cannot be opened leaves the live map untouched. Like every command here,
     * the queue only drains while the engine runs — callers gate on
     * decoderHost.running and treat an idle-time import as library-only.
     */
    Q_INVOKABLE bool importChannelMap(const QString& path) const;

    /** @brief Import a talkgroup list CSV into the running session (atomic swap). */
    Q_INVOKABLE bool importGroupList(const QString& path) const;

    // WP-D2: secret input is only copied into an erased queue payload.
    Q_INVOKABLE bool applyEncryptionKey(const QString& type, const QString& value) const;
    Q_INVOKABLE bool setForceKeyMode(int mode) const;

    /** @brief Import an encryption key CSV; @p hex picks -K semantics over -k. */
    Q_INVOKABLE bool importKeys(const QString& path, bool hex) const;

    /**
     * @brief Import a P25 band plan CSV (--p25-bandplan) into the running session.
     *
     * Replaces the operator-supplied identifier table the P25 tuner falls back
     * on when the control channel's own IDEN_UP has not been heard. There is
     * deliberately no clear counterpart: a band plan learned off the air is not
     * something a session can be asked to forget, so "None" only takes effect
     * at the next start.
     */
    Q_INVOKABLE bool importP25Bandplan(const QString& path) const;

    /** @brief Import source radio ID aliases (--src-csv) into the running session. */
    Q_INVOKABLE bool importSrcList(const QString& path) const;

    /*
     * Unloading. A system that clears its CSV selection has to say so: the
     * import calls above all reject an empty path, so re-importing cannot
     * express "none" and the previous file would stay live for the session.
     */
    /** @brief Drop the running session's channel map, LCN list and trust bytes. */
    Q_INVOKABLE bool clearChannelMap() const;

    /** @brief Drop the running session's talkgroup list. */
    Q_INVOKABLE bool clearGroupList() const;

    /** @brief Drop the running session's keyring (both CSV kinds share one). */
    Q_INVOKABLE bool clearKeys() const;
    /** @brief Unload the running session's source radio ID aliases. */
    Q_INVOKABLE bool clearSrcList() const;

  private:
    QString submitDecryption(const QVariantMap& fields, int scope, const QVariantMap& context);
    /* Why a recording switch was refused, or (empty) that it was sent. */
    void noteRecording(const QString& refusal) const;
    /* The recording view of the session on, or false (refused as "no-session") with none. */
    bool recordingSessionLive(dsd_app_recording_view* view) const;

    DecryptionProfileProvider* m_profiles = nullptr;
    RecordingStatus* m_recordingStatus = nullptr;
    quint64 m_pendingKeyRequest = 0;
    quint64 m_pendingKeySession = 0;
    QList<QSharedPointer<QTemporaryFile>> m_selectionFiles;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_COMMAND_BRIDGE_H_ */
