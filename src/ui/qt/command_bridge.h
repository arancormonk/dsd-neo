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
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSharedPointer> // IWYU pragma: keep
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/recording_view.h>
#include <type_traits> // IWYU pragma: keep -- QPointer's conversion checks.
class QTemporaryFile;

namespace dsd_qt {
class DecryptionProfileProvider;
class RecordingStatus;
class SavedSystemsModel;
class ScanAvoidListModel;

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
    /* Discussion #900 F3: the answer to this bridge's newest answered setDmrChannel() (dmrChannelResultReceived()'s
       map; empty before one), and the request id of the newest one it queued (0 before one). */
    Q_PROPERTY(QVariantMap dmrChannelResult READ dmrChannelResult NOTIFY dmrChannelResultChanged)
    Q_PROPERTY(quint32 lastDmrChannelRequest READ lastDmrChannelRequest NOTIFY lastDmrChannelRequestChanged)

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
     * scanRowEdit*). For SET, @p value carries the field's value: squelchDb (whole dB, -120..0; 0 = off) or
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

    /**
     * @brief Set (@p op 1, to @p hz), hide (2) or restore (3) DMR Tier III channel @p lpcn (1..4094) in the running
     * session, against the row as the Channels sheet read it (discussion #900 F3): @p expectedHz its live frequency (0
     * for none or hidden), @p expectedOrigin its origin (DmrChannelsModel::channel()'s originCode), @p generation the
     * model's network generation and @p session its command session (empty: the one open now). The decoder refuses an
     * edit whose row, network or session moved on (stale), or one under a trunk scan.
     *
     * @p systemUid is the saved system the session runs as the edit is sent (empty: a session that keeps nothing, an
     * edit for this run only). It is captured here: once the decoder applied the edit, this bridge keeps it on that
     * system (SavedSystemsModel::applyDmrChannelEdit()), whatever the session runs by then.
     *
     * @return Whether it was queued; then lastDmrChannelRequest() names it, and dmrChannelResultReceived() carries its
     *         answer. False, sending nothing, for a channel, operation, SET frequency (100000..2147483647 Hz), origin
     *         (0..255) or session that cannot be one, or a queue that refused it.
     */
    Q_INVOKABLE bool setDmrChannel(int lpcn, int op, qint64 hz, qint64 expectedHz, int expectedOrigin,
                                   quint32 generation, const QString& systemUid = QString(),
                                   const QString& session = QString());

    /**
     * @brief Keep a channel edit the decoder applied on saved system @p systemUid again (the Channels sheet's "Try
     * saving again" after a save that failed): @p op, @p lpcn and @p hz as setDmrChannel() sent them, kept as
     * dmrChannelResultReceived() keeps an "ok" answer (SavedSystemsModel::applyDmrChannelEdit()).
     *
     * @return {saved, saveError}, with the answer's meanings: "no-system" also for an empty @p systemUid.
     */
    Q_INVOKABLE QVariantMap saveDmrChannelEdit(const QString& systemUid, int lpcn, int op, qint64 hz);

    /** @brief Where an applied channel edit is kept (the saved systems); nullptr: nowhere, saved is false. */
    void setSavedSystems(SavedSystemsModel* systems);

    QVariantMap
    dmrChannelResult() const {
        return m_dmrChannelResult;
    }

    quint32
    lastDmrChannelRequest() const {
        return m_lastDmrChannelRequest;
    }

    /**
     * @brief Take the channel-set answers published since the last call (UiController's tick and its session edges,
     * and the application's aboutToQuit, so an edit applied just before a teardown is still kept) and report each of
     * this bridge's requests once, in order. Another caller's answers are passed over.
     */
    void pollDmrChannelResults();

    /** @brief The key of a dsd_app_dmr_channel_set_code: "ok", "stale", "trunk-scan", "invalid", "full", "search",
     *  "dropped", "other-system" (the carrier names another system), "no-dmr" (the session no longer decodes DMR), or
     *  "unknown". */
    static QString dmrChannelResultKey(int code);

    /** @brief Toggle audio mute. @return true when the command was accepted. */
    Q_INVOKABLE bool toggleMute() const;

    /** @brief Hold decoding on one talkgroup; 0 releases the hold. Accepted, it is an operator intent (below). */
    Q_INVOKABLE bool holdTalkgroup(unsigned int talkgroup);

    /** @brief Lock out the target currently active on @p slot (0 or 1). */
    Q_INVOKABLE bool lockoutSlot(int slot) const;
    /** @brief Skip the call currently on @p slot (0 or 1) without editing the talkgroup list. */
    Q_INVOKABLE bool skipSlot(int slot) const;
    Q_INVOKABLE bool setPersistTgLockouts(bool persist) const;
    /** @brief Set Relaxed CRC (-F) on or off: the configured value, beneath a scan row's own while one is on air. */
    Q_INVOKABLE bool setRelaxedCrc(bool on) const;
    /**
     * @brief Start or stop recording each call in the running session (issue #708, D8), into the folder the session
     * was started with. On sends @p analogMaxGapMs (the analog pause hold, issue #756), @p analogPrerollMs (the NFM
     * pre-roll, issue #668), @p unknownOnly and then WAV start, where a negative hold or pre-roll is none to send (the
     * session keeps its own); off sends WAV stop and then unknown-only off (unknown-only travels with Record calls: an
     * off switch filters no recording of the user's own options).
     *
     * Refused without sending anything, and said in RecordingStatus::refusal(), when no session is on ("no-session"),
     * while its run is not up yet ("starting"),
     * when on would record into a session with no folder ("storage-unavailable"), or either way when the session
     * records to one static WAV from its extra options ("static-wav": a start or a stop would close the user's file);
     * the preference then applies at the next start. Queued like every command; the decoder holds it while a carrier loop
     * of its own runs and refuses it during Signal search, which RecordingStatus shows.
     */
    Q_INVOKABLE bool setRecordCalls(bool on, bool unknownOnly, int analogMaxGapMs, int analogPrerollMs) const;
    /**
     * @brief Record only calls no talkgroup row knows, or every call (issue #708). Sent only while the session records
     * each call (or a Record calls on is still applying); otherwise nothing is sent and it answers true: the switch
     * travels with the next Record calls on.
     */
    Q_INVOKABLE bool setRecordUnknownOnly(bool on) const;
    /**
     * @brief The longest pause in ms one analog call recording keeps going through (issue #756), sent as
     * setRecordUnknownOnly() is: only while the session records each call, otherwise it travels with the next Record
     * calls on. The decoder refuses a value outside 0..10000, which the status shows as failed.
     */
    Q_INVOKABLE bool setAnalogRecordMaxGap(int ms) const;
    /**
     * @brief How much in ms of what was heard just before an NFM call each recording starts with (issue #668), sent
     * as setAnalogRecordMaxGap() is. The decoder refuses a value outside 0..1000, which the status shows as failed.
     */
    Q_INVOKABLE bool setAnalogRecordPreroll(int ms) const;

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
    /**
     * @brief Put the one avoided channel or target @p key names back into the rotation (issue #672), by the identity
     * the avoid list holds for that key (never a list position), so a list that changed under the reader puts back the
     * entry that was shown or nothing. Never retunes. False for a key the list no longer holds or an identity that does
     * not parse; the decoder answers a stale one with a message in MetricsModel::uiMessage.
     */
    Q_INVOKABLE bool restoreScanAvoid(const QString& key) const;
    /**
     * @brief The put-back payload for an avoid list entry (ScanAvoidListModel::entry()): the 64-bit fields parsed from
     * their decimal strings, the row non-negative, the session nonzero and within 32 bits, the target id whole and
     * NUL-free. False, with @p out zeroed, for anything else.
     */
    static bool scanAvoidIdentity(const QVariantMap& entry, dsd_app_scan_avoid_remove_payload* out);

    /** @brief The avoid list restoreScanAvoid() reads (MetricsModel's); nullptr: every put back is refused. */
    void setScanAvoids(ScanAvoidListModel* list);

    /**
     * @brief Step back one channel in the same list Next walks (the previous trunk-scan target under --trunk-scan).
     * The rotation carries on forward from there.
     */
    Q_INVOKABLE bool previousChannel() const;
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
    Q_INVOKABLE bool manualTuneHz(unsigned int hz);

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
    Q_INVOKABLE bool releaseTuner();

    /**
     * @brief Hand the tuner to trunking (@a on true), or stop it following.
     *
     * The inverse of releaseTuner() for the one flag a view can name. Turning it
     * on also clears scanner mode engine-side, since both cannot drive the tuner.
     * Off is the narrow half — it leaves scanner mode alone, so releaseTuner()
     * stays the way to ask for the tuner back without knowing what holds it.
     */
    Q_INVOKABLE bool setTrunking(bool on);

    /** @brief Set tuner gain in dB (0 selects automatic gain). */
    Q_INVOKABLE bool setTunerGain(int gain_db) const;

    /**
     * @brief Set the configured default's power squelch threshold, in dB (0 or more is off).
     *
     * Issue #899: each squelch setter returns the request's tag, a positive int, once it is queued (0 when it is not),
     * which MetricsModel::configuredSquelchRequestAnswered shows once the decoder has handled that request, with
     * configuredSquelchRequestApplied saying whether it applied it: what lets a caller save the setting only on the
     * decoder's answer to it.
     */
    Q_INVOKABLE int setSquelchDb(double db) const;

    /**
     * @brief Set the auto squelch: open @p marginDb (squelchMarginMinDb..squelchMarginMaxDb) over the noise floor
     * the demodulator learns on each channel. Like setSquelchDb() it edits the configured default and returns the
     * request's tag (0 when not queued).
     */
    Q_INVOKABLE int setSquelchAuto(int marginDb) const;

    /**
     * @brief Set the noise squelch: open an FM channel when its discriminator noise above voice quiets by @p marginDb
     * (squelchMarginMinDb..squelchMarginMaxDb). Like setSquelchDb() it edits the configured default and returns the
     * request's tag (0 when not queued); the AM monitor of audio input refuses it.
     */
    Q_INVOKABLE int setSquelchNoise(int marginDb) const;

    /**
     * @brief Put the configured default back on the level it kept beneath a dynamic squelch, exactly as stored (a
     * legacy linear threshold included, which a whole-dB reading cannot carry).
     *
     * Issue #899: returns what was asked for, so a caller can save that level and no other -- {request, off,
     * whole, db}: the request's tag, which MetricsModel::configuredSquelchRequestAnswered shows once the decoder has
     * handled it (configuredSquelchRequestApplied: whether it applied it); whether the level is Off; whether it is a whole dB the app's controls make, and that dB (0 when it
     * is not). Null when nothing was submitted.
     */
    Q_INVOKABLE QVariant restoreSquelchLevel() const;

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

    /**
     * @brief Ask the running session to re-read its group list file.
     *
     * With an empty @p path, the configured group file. With a path, only when
     * that is the file the session uses: the decoder decides when it applies
     * the command, so a caller that replaced a file posts it without knowing
     * whether the session still uses it (another file is a no-op it logs).
     * True means queued, not reloaded; the decoder's toast says what happened.
     */
    Q_INVOKABLE bool reloadGroupList(const QString& path) const;

    /** @brief As reloadGroupList() for the channel map; @p path is required. */
    Q_INVOKABLE bool reloadChannelMap(const QString& path) const;

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

  Q_SIGNALS:
    /**
     * @brief GPS site roaming (#786): a command that changes what the session follows was queued. @p kind is "hold"
     * or "release" (holdTalkgroup()), "manualTune" (manualTuneHz()), "releaseTuner" (releaseTuner(), Explore from here
     * among its callers), "trunkingOn" or "trunkingOff" (setTrunking()). The decoder applies it later, or never; site
     * roaming waits until the decoder's snapshot shows it before it judges the session again.
     */
    void operatorIntentSubmitted(const QString& kind);
    /**
     * @brief The decoder answered one of this bridge's setDmrChannel() requests: {request, lpcn, op, hz (what a SET
     * asked for, else 0), code (dsd_app_dmr_channel_set_code), result (dmrChannelResultKey()), revision (the channel
     * revision after it; a model whose revision is behind does not show it yet), session (decimal), sequence
     * (decimal), systemUid (the one setDmrChannel() captured), saved (an "ok" edit was kept on that system),
     * saveError ("" when saved or nothing was to be kept: not "ok", or no system; else applyDmrChannelEdit()'s error,
     * "not-saved" for a failed write, "session-only" for a restore no saved entry made, or "no-store" with no saved
     * systems set)}.
     */
    void dmrChannelResultReceived(const QVariantMap& result);
    void dmrChannelResultChanged();
    void lastDmrChannelRequestChanged();

  private:
    QString submitDecryption(const QVariantMap& fields, int scope, const QVariantMap& context);
    /* Why a recording switch was refused, or (empty) that it was sent. */
    void noteRecording(const QString& refusal) const;
    /* The recording view of the session on, or false (refused as "no-session") with none. */
    bool recordingSessionLive(dsd_app_recording_view* view) const;
    /* Send the i32 @p command with @p value while the session records each call; answer true, sending nothing, while
       it does not. */
    bool sendWhileRecording(int command, int value) const;

    DecryptionProfileProvider* m_profiles = nullptr;
    RecordingStatus* m_recordingStatus = nullptr;
    QPointer<ScanAvoidListModel> m_scanAvoids;
    quint64 m_pendingKeyRequest = 0;
    quint64 m_pendingKeySession = 0;
    QList<QSharedPointer<QTemporaryFile>> m_selectionFiles;

    /* A queued channel set's request id with what it asked, until its answer comes (the oldest dropped past a bound,
       so requests never answered cannot grow it). */
    struct DmrChannelRequest {
        quint32 request = 0;
        int lpcn = 0;
        int op = 0;
        qint64 hz = 0;
        QString systemUid;
    };

    /* The answer map for @p r, the request @p q it answers; an "ok" one kept on its system first. */
    QVariantMap dmrChannelAnswer(const dsd_app_dmr_channel_set_result& r, const DmrChannelRequest& q);
    QPointer<SavedSystemsModel> m_savedSystems;

    QList<DmrChannelRequest> m_dmrChannelRequests;
    quint64 m_dmrChannelCursor = 0;
    quint32 m_lastDmrChannelRequest = 0;
    QVariantMap m_dmrChannelResult;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_COMMAND_BRIDGE_H_ */
