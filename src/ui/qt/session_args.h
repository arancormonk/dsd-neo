// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Builds the CLI-shaped argv a saved system starts with.
 *
 * Reusing the CLI parser is what buys the whole option surface, but the specs it
 * consumes are ':'-delimited strings a typo can silently mistune — so their
 * assembly lives here, in host-testable C++, rather than in QML-side JavaScript.
 * The pure function takes plain values; the QObject wrapper reads the live
 * AppPrefs and is what QML calls.
 */

#ifndef DSD_NEO_SRC_UI_QT_SESSION_ARGS_H_
#define DSD_NEO_SRC_UI_QT_SESSION_ARGS_H_

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>

#include <dsd-neo/core/opts.h>
Q_MOC_INCLUDE("decoder_host.h")

namespace dsd_qt {

class AppPrefs;
class DecoderHost;
class SavedSystemsModel;
class DecryptionProfileProvider;
class RecordingSessions;

/** @brief The app-wide defaults a saved system's overrides fall back to. */
struct SessionArgPrefs {
    int gainDb = 30;
    int ppm = 0;
    int bandwidthKhz = 48;
    bool biasTee = false;
    bool skipEncrypted = true;
    bool persistTgLockouts = true;
    bool autoPpm = false;
    /* Relaxed CRC: -F for the session, unless the extra options name the CRC policy themselves. */
    bool relaxedCrc = false;
    double hangtimeSec = 2.0;
    QString extraArgs;
    /* Call recording (issue #708). The folder the session records into, from the recording store: with one, the argv
       names it (-7) and its journal (--wav-journal), records per call (-P) when recordCalls is on and records only
       unknown talkgroups (--record-unknown-only) when recordUnknownOnly is; without one (no storage to record to, or
       a validation-only build) the argv names no recording at all. */
    bool recordCalls = false;
    bool recordUnknownOnly = false;
    /* Issue #756: the longest pause one analog call recording keeps going through, in ms, sent with -P
       (--analog-record-max-gap-ms); 200 or less is off, the decoder's default. */
    int analogMaxGapMs = DSD_ANALOG_RECORD_MAX_GAP_DEFAULT_MS;
    /* Issue #668: how much of what was heard just before an NFM call each recording starts with, in ms, sent with -P
       (--analog-record-preroll-ms); 0 is off. */
    int analogPrerollMs = DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS;
    QString recordingFolder;
    /* The store was asked for a folder and had none (no storage available): an extra -P is refused, since it would
       record outside any folder the app manages. False for a validation-only build, which asks for none. */
    bool recordingUnavailable = false;
};

/** @brief The app-wide defaults @p prefs holds, as a session starts with them; the struct's defaults for nullptr. The
 *         one mapping a saved system, Explore, search and a scan list share. */
SessionArgPrefs session_args_prefs(const AppPrefs* prefs);

/** @brief Why session_args_build() refused; None means the argv is usable. */
/** Shared token gate: refuse key display and grouped short options; retain
 * single short options with attached arguments as defined by the CLI. */
bool session_args_extra_safe(const QString& tokens);
bool session_args_scan_extra_safe(const QString& tokens);
QStringList session_args_scan_build(const QVariantMap& list, const QString& firstFreqMhz, const QString& csvPath,
                                    const SessionArgPrefs& prefs, QString* error);
/**
 * @brief The argv that scans several lists in one session (issue #697): @p list's receiver and session options, one
 * --trunk-scan per path of @p csvPaths in their order (1 to DSD_TRUNK_SCAN_MAX_LISTS; list N is the Nth path), then
 * the app's options. session_args_scan_build() is this with one path.
 */
QStringList session_args_scan_build_lists(const QVariantMap& list, const QString& firstFreqMhz,
                                          const QStringList& csvPaths, const SessionArgPrefs& prefs, QString* error);
/**
 * @brief @p args, a scan session's argv, starting held on target @p targetId (issue #644): `--trunk-scan-hold
 * <targetId>` after the last list's `--trunk-scan`, ahead of the extra options (a "--" among them would make it an
 * operand). Empty when @p args scans no list or the decoder cannot keep the id: empty, a control character, or more
 * bytes than dsd_opts::trunk_scan_hold_id holds.
 *
 * The only way a frontend start carries the option: the extra-option gates refuse it, since extra options are saved
 * and the hold is for one start.
 */
QStringList session_args_with_scan_hold(const QStringList& args, const QString& targetId);

/**
 * @brief Whether a held start can name target @p targetId (issue #644): not empty, no control character, and fewer
 * bytes than dsd_opts::trunk_scan_hold_id holds. session_args_with_scan_hold() takes exactly these, so a frontend
 * offers a held start only for a target this accepts.
 */
bool session_args_scan_hold_id_valid(const QString& targetId);

/** @brief The engine's default trunk-scan dwell and activity hold, in ms: what a list's 0 runs at. */
constexpr int SessionArgsScanDwellDefaultMs = 3000;
constexpr int SessionArgsScanHoldDefaultMs = 1200;

/**
 * @brief Why scan list @p other cannot join a session started from @p primary, or empty when it can (issue #697).
 *
 * The session runs one receiver and one set of scan options, @p primary's, so every setting a list puts into its
 * argv has to come out the same for both once the app-wide defaults in @p prefs fill what a list leaves unset and side
 * files are resolved to absolute paths: radio source (source type and device: Airspy serial, host and port), gain,
 * PPM correction, DSP bandwidth (as the radio runs it), bias tee, every other Airspy setting, voice only, skip silent
 * channels (issue #834), default dwell and hold (0 runs the engine's default), fallback talkgroups and source aliases.
 * The first that differs, in that order, is named: "Different radio source", "Different gain", "Different PPM
 * correction", "Different bandwidth", "Different bias tee", "Different Airspy settings", "Different voice-only
 * setting", "Different skip-silent setting", "Different default dwell", "Different default hold", "Different fallback
 * talkgroups" or "Different source aliases".
 */
QString session_args_scan_join_conflict(const QVariantMap& primary, const QVariantMap& other,
                                        const SessionArgPrefs& prefs);

enum class SessionArgsError {
    None,
    Frequency,
    Ppm,
    Hangtime,
    KeyType,
    KeyBasic,
    KeyHex,
    KeyRc4,
    KeyScrambler,
    KeyM17Scrambler,
    KeyM17Aes,
    KeyConflict,
    ForceKey,
    UnsafeOption,
    /* Issue #524: an AM system (-fM) on a source whose audio arrives already demodulated (network or file). */
    AmNeedsRadio,
    /* Issue #524: an AM system on a radio whose DSP bandwidth cannot filter the AM channel. */
    AmBandwidth,
    /* A TETRA system (-fT) on a source that is neither a radio nor a symbol capture file: its audio arrives demodulated. */
    TetraNeedsIq,
    /* Issue #580: a file system whose recording is not set (a restored backup clears every replay recording). */
    RecordingMissing,
    /* Issue #708: extra options that say where call recordings go or delete them (-7, --wav-journal,
       --rdio-api-delete-after-upload): the app owns the folder, the journal and the deletion. */
    CallRecordingOption,
    /* Issue #708: a static WAV (-w) among the extra options while Record calls is on; the two cannot run together. */
    CallRecordingStaticWav,
    /* Issue #708: a per-call -P among the extra options while no storage is available for call recordings. */
    CallRecordingNoStorage,
    /* Issue #803: a TETRA system (-fT) on a radio whose DSP bandwidth is not a TETRA rate
       (session_args_tetra_runs_on()). */
    TetraRate
};

/* The last SessionArgsError value. A value added after it takes its place here, and session_args_error_text() then
 * needs its text: a static_assert there holds the text table to this value. */
constexpr SessionArgsError SessionArgsErrorLast = SessionArgsError::TetraRate;

/**
 * @brief Why the extra options @p tokens conflict with the app's call recording (issue #708), or None.
 *
 * The app chooses the folder and journal (-7, --wav-journal) and deletes recordings itself (its storage limit, never
 * --rdio-api-delete-after-upload, so no file vanishes under playback or an export), in either option form ("-7dir",
 * "--wav-journal=file"); a static WAV (-w) is refused only while @p recordCalls is on, and a per-call -P while
 * @p noStorage (no folder to record into: it would record outside the app's storage). Tokens are walked as the
 * decoder walks its argv (dsd_cli_long_option_arity(), dsd_cli_short_option_takes_value()): the value of an option is
 * never read as an option ("-c -7", "--input-level-warn-db -7"), and an option is never read as a value.
 */
SessionArgsError session_args_recording_extra_error(const QString& tokens, bool recordCalls, bool noStorage = false);

/**
 * @brief Whether the extra options @p tokens set the analog pause hold themselves (--analog-record-max-gap-ms, issue
 * #756), in either option form.
 *
 * The decoder takes a long option out wherever it stands, and the last one wins, so one there decides every session
 * whatever Settings holds. Tokens are walked as session_args_recording_extra_error() walks them: another option's
 * value ("--api-token --analog-record-max-gap-ms") and anything after a "--" that is no value set nothing.
 */
bool session_args_extra_sets_pause_hold(const QString& tokens);

/**
 * @brief Whether the extra options @p tokens set the NFM pre-roll themselves (--analog-record-preroll-ms, issue #668),
 * in either option form, read as session_args_extra_sets_pause_hold() reads the pause hold.
 */
bool session_args_extra_sets_preroll(const QString& tokens);

/** Every Unicode space (QChar::isSpace(): NBSP and the rest included), '-' and ':' grouping removed, then a leading
 * 0x or 0X stripped, uppercase hex. Only for hex material (session_args_key_type_hex() types and the profile model's
 * managed hex formats): decimal keys and vendor grammars keep their own rules. QString copies cannot promise erasure;
 * never expose returned key text in diagnostics. */
QString session_args_key_hex_normalize(const QString& value);
/** Whether a direct key of @p type is hex material: hex, rc4, m17Scrambler and m17Aes. */
bool session_args_key_type_hex(const QString& type);
/** Direct key material as the decoder takes it: hex types through session_args_key_hex_normalize(), the decimal
 * types (basic, scrambler) and any other trimmed. */
QString session_args_key_value_normalize(const QString& type, const QString& value);
bool session_args_key_valid(const QString& type, const QString& value);
/** The profile protocol's own rule for a direct key, or empty: a P25 hex key is never the 10-digit Hytera width, and
 * an NXDN hex key is 64 digits. Other protocols and types add nothing. */
QString session_args_key_protocol_error(const QString& protocol, const QString& type, const QString& value);
/** Safe, complete validation sentence for the wizard/session rejection UI. */
QString session_args_error_text(SessionArgsError error);

/**
 * @brief Whether a saved system's frequency field parses as a positive MHz value.
 *
 * Strict parse: trailing junk ("851.375M") must fail, because the value is
 * spliced verbatim into the rtl input spec where dsd_parse_freq_hz would read
 * garbage as 0 Hz and the session would come up silently mistuned.
 */
bool session_args_profile_compatible(const QVariantMap& system);
bool session_args_freq_valid(const QString& freqMhz);
/**
 * @brief The Hz a saved system's frequency field tunes, or 0 when session_args_freq_valid() refuses it.
 *
 * The start path's own reading: the trimmed text as "<text>M" through dsd_parse_freq_hz, so "+851.0125",
 * "8.510125e2" and "851.0125004" are all 851012500.
 */
quint32 session_args_freq_hz(const QString& freqMhz);

/**
 * @brief The DSP bandwidth, in kHz, a radio of @p sourceType runs at when asked for @p requestedKhz: the bandwidth its
 * -i spec carries (issue #806).
 *
 * A USB or rtl_tcp radio runs a value it does not select at its 48 kHz default (36 runs at 48). An Airspy runs the
 * nearest one it takes (25 runs at 24; of two as near, the narrower), and the 48 kHz default for 0 or less and for one
 * wider than 72 kHz, as a USB radio does. Any other source has no DSP bandwidth: the request comes back as it is. The
 * one normalization the session's spec, its width checks and the scan list's rate warning share.
 */
int session_args_dsp_bandwidth_khz(const QString& sourceType, int requestedKhz);

/**
 * @brief Whether an analog channel of @p kind and @p widthHz fits the DSP rate a radio of @p sourceType asked for
 * @p requestedKhz runs at, as the engine holds it before the stream opens (issue #806).
 *
 * Every live radio runs at its DSP bandwidth (dsd_input_dsp_rate_for() at session_args_dsp_bandwidth_khz()), an
 * Airspy's capture rate stepped to it exactly, so the width is held to that bandwidth up front on each of them. Any
 * other source runs no channel filter, so it always fits. On a refusal @p why (optional) gets the engine's reason
 * (dsd_analog_width_check_at()).
 */
bool session_args_analog_width_fits(const QString& sourceType, int requestedKhz, int kind, int widthHz, QString* why);

/**
 * @brief Whether a radio source of @p sourceType running at @p bandwidthKhz can filter the AM channel (issue #524).
 *
 * The session's AM channel is the 6 kHz default, and the engine holds it to the radio's DSP bandwidth before the device
 * opens (session_args_analog_width_fits()): at 4 or 6 kHz it refuses to start. Any other source runs no channel filter,
 * so it always fits here. An explicit --am-bandwidth-hz among the extra options is the engine's to check.
 */
bool session_args_am_fits_bandwidth(const QString& sourceType, int bandwidthKhz);

/** @brief The DSP bandwidths, in kHz, that filter the 6 kHz AM channel: "8, 12, 16, 24, 48 or 72". */
QString session_args_am_bandwidth_choices();

/**
 * @brief The DSP bandwidth, in kHz, a system with @p decodeFlag runs at on @p sourceType (issue #803).
 *
 * The bandwidth asked for is the system's own (@p systemKhz above 0), else 72 kHz for TETRA (-fT, which runs only at a
 * multiple of 18 kHz), else the app-wide default @p prefsKhz, run as the radio runs it
 * (session_args_dsp_bandwidth_khz()). The one rule the start check, the wizard's gate and its bandwidth placeholder
 * share.
 */
int session_args_effective_bandwidth_khz(const QString& decodeFlag, const QString& sourceType, int systemKhz,
                                         int prefsKhz);

/**
 * @brief Whether TETRA decodes on a radio of @p sourceType running at @p bandwidthKhz (issue #803).
 *
 * A radio decodes it only where its DSP bandwidth, as the engine runs it (session_args_dsp_bandwidth_khz()), is a
 * TETRA rate (dsd_tetra_dsp_rate_runs(): 72 kHz among the selectable ones). Since issue #806 an Airspy's capture rate
 * is stepped to that bandwidth, so an Airspy is judged as a USB or rtl_tcp radio is. Any other source is TetraNeedsIq's
 * to judge, so it passes here.
 */
bool session_args_tetra_runs_on(const QString& sourceType, int bandwidthKhz);

/**
 * @brief Why TETRA cannot decode on @p sourceType at @p bandwidthKhz, or empty when it can (issue #803).
 *
 * The text is session_args_error_text(SessionArgsError::TetraRate), the one fix on every radio: a 72 kHz bandwidth. A
 * start refused with TetraRate carries it as its "errorText".
 */
QString session_args_tetra_rate_error(const QString& sourceType, int bandwidthKhz);

/**
 * @brief The CLI-shaped argv for @p system, or an empty list with @p error set.
 *
 * Per-system overrides fall back to the app-wide defaults (-1 / empty string
 * mean "no override"; biasTee is -1 follow / 0 off / 1 on). A malformed
 * frequency, PPM or hang time must fail here, not downstream as a silently mistuned or
 * uncorrected session.
 */
QStringList session_args_build(const QVariantMap& system, const SessionArgPrefs& prefs, SessionArgsError* error);

/** @brief QML-facing wrapper binding the pure builder to the live AppPrefs. */
class SessionArgsBuilder : public QObject {
    Q_OBJECT
    /** @brief Whether the app-wide Extra decoder arguments the builder starts every session with set the analog pause
     *  hold themselves, so Settings' row changes nothing (issue #756); see session_args_extra_sets_pause_hold(). */
    Q_PROPERTY(bool extraArgsSetPauseHold READ extraArgsSetPauseHold NOTIFY extraArgsSetPauseHoldChanged)
    /** @brief The same for the NFM pre-roll (issue #668); see session_args_extra_sets_preroll(). */
    Q_PROPERTY(bool extraArgsSetPreroll READ extraArgsSetPreroll NOTIFY extraArgsSetPrerollChanged)

  public:
    explicit SessionArgsBuilder(const AppPrefs* prefs, QObject* parent = nullptr);
    ~SessionArgsBuilder() override;
    void setSavedSystems(const SavedSystemsModel* systems);

    void
    setDecryptionProfiles(DecryptionProfileProvider* profiles) {
        m_profiles = profiles;
    }

    /** @brief Where start() asks for the session's call recording folder (issue #708); nullptr: nowhere. */
    void
    setRecordingSessions(RecordingSessions* sessions) {
        m_recordingSessions = sessions;
    }

    /**
     * @brief Validate one saved-system field map without returning key-bearing argv.
     * @return {"ok": bool, "error": category, "errorText": sentence}, and for "am-bandwidth" "bandwidthChoices":
     *         session_args_am_bandwidth_choices().
     */
    Q_INVOKABLE QVariantMap build(const QVariantMap& system) const;
    /**
     * Resolve retained keys and start entirely in C++; adds a non-secret "started" boolean.
     *
     * @p kind is what the start is (issue #708): "saved" for a saved system, which records into its own folder;
     * "explore" or "search" for Explore and Signal search, which share one. The map alone cannot tell: an edited
     * Explore start can carry the uid of the saved system it was copied from.
     */
    Q_INVOKABLE QVariantMap start(const QVariantMap& system, DecoderHost* host, const QString& kind) const;

    /** @brief Frequency validity for the wizard's step gating; see session_args_freq_valid(). */
    Q_INVOKABLE bool freqValid(const QString& freqMhz) const;

    /**
     * @brief Why AM cannot run on @p sourceType at @p bandwidthKhz (0 or less: the app-wide default), or empty.
     *
     * The wizard's step gating for an AM pick; see session_args_am_fits_bandwidth().
     */
    Q_INVOKABLE QString amBandwidthError(const QString& sourceType, int bandwidthKhz) const;

    /**
     * @brief The DSP bandwidth a system with @p decodeFlag runs at on @p sourceType when it asks for @p bandwidthKhz
     *        (0 or less: none of its own), with the live app-wide default; see session_args_effective_bandwidth_khz().
     *
     * The wizard's bandwidth placeholder: 72 for a blank TETRA field.
     */
    Q_INVOKABLE int effectiveBandwidthKhz(const QString& decodeFlag, const QString& sourceType, int bandwidthKhz) const;

    /**
     * @brief Why a system with @p decodeFlag cannot decode TETRA on @p sourceType at @p bandwidthKhz (0 or less: none
     *        of its own), or empty: always empty unless the flag names -fT.
     *
     * The wizard's step gating for a TETRA pick; see session_args_tetra_rate_error().
     */
    Q_INVOKABLE QString tetraRateError(const QString& decodeFlag, const QString& sourceType, int bandwidthKhz) const;

    /**
     * Safe encryption-only validation; never builds or returns secret argv. A non-empty @p protocol (a profile
     * protocol: p25, nxdn, ...) applies session_args_key_protocol_error() first, as a saved direct profile does.
     */
    Q_INVOKABLE QString keyError(const QString& type, const QString& value, const QString& csvPath, int force,
                                 const QString& protocol = QString()) const;

    bool extraArgsSetPauseHold() const;
    bool extraArgsSetPreroll() const;

  Q_SIGNALS:
    void extraArgsSetPauseHoldChanged();
    void extraArgsSetPrerollChanged();

  private:
    QStringList buildArgs(const QVariantMap& system, SessionArgsError* error,
                          const QString& recordingFolder = QString(), bool recordingUnavailable = false) const;
    /* start()'s build: with the session's call recording folder when @p host can record it (@p recording set, the
       folder's session begun). */
    QStringList buildForStart(const QVariantMap& system, const DecoderHost* host, const QString& kind,
                              SessionArgsError* error, bool* recording) const;
    /* The recording session buildForStart() began ends when the start did not happen and no session runs. */
    void endUnstartedRecording(bool recording, bool started, const DecoderHost* host) const;
    const AppPrefs* m_prefs;
    const SavedSystemsModel* m_systems = nullptr;
    DecryptionProfileProvider* m_profiles = nullptr;
    RecordingSessions* m_recordingSessions = nullptr;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SESSION_ARGS_H_ */
