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
class QTemporaryFile;

namespace dsd_qt {
class DecryptionProfileProvider;

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

  public:
    static int scanRowFieldSquelch();
    static int scanRowFieldWidth();
    static int scanRowFieldTone();
    static int scanRowFieldGain();
    static int scanRowEditSet();
    static int scanRowEditInherit();
    static int scanRowEditReset();

    /**
     * @brief The scan row on air, captured when a "this channel" editor opens (issue #518): active, scanner, session,
     * row, mode, target and label. An edit names it, so it reaches that row however the scan has moved on since, and is
     * refused once that scan has ended. Empty-handed ({active: false}) with no row on air.
     */
    Q_INVOKABLE QVariantMap scanRowContext() const;
    /**
     * @brief Submit a session edit of one @p field (a scanRowField*) of the row @p context names, with @p action (a
     * scanRowEdit*). For SET, @p value carries the field's value: squelchDb (whole dB, -100..0; 0 = off), widthHz,
     * toneMode with toneList, or gainDb (0 = AGC). Returns whether it was queued; the decoder refuses what the row
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
    /** Changed fields only: name, listening, priority, preempt. One map is one atomic edit. */
    Q_INVOKABLE bool setTalkgroupPolicy(unsigned int idStart, unsigned int idEnd, const QString& context,
                                        unsigned int generation, const QVariantMap& changes) const;
    Q_INVOKABLE bool renameTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                                     unsigned int generation, const QString& name) const;
    Q_INVOKABLE bool addTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                                  unsigned int generation, const QString& name, bool listen, int priority,
                                  bool preempt) const;
    Q_INVOKABLE bool removeTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                                     unsigned int generation) const;
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
    Q_INVOKABLE bool clearTemporaryTgAvoids(const QString& context) const;

    /** @brief Listen to or stop tuning one talkgroup or range; a missing exact id is added. */
    Q_INVOKABLE bool setTalkgroupListening(unsigned int idStart, unsigned int idEnd, bool listen) const;

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

    /** @brief Retune the radio front end. */
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
     * @brief Stop trunking and scanner mode from moving the tuner.
     *
     * Idempotent, and deliberately not a toggle: a view only learns that
     * something owns the tuner, never which of the two, so it cannot safely
     * flip either flag on its own. Also tears down the follow state the trunker
     * left behind, so the decoder can acquire whatever it is pointed at next.
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
    DecryptionProfileProvider* m_profiles = nullptr;
    quint64 m_pendingKeyRequest = 0;
    quint64 m_pendingKeySession = 0;
    QList<QSharedPointer<QTemporaryFile>> m_selectionFiles;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_COMMAND_BRIDGE_H_ */
