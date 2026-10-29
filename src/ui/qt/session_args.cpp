// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QPair>
#include <utility>
#include "decoder_host.h"
#include "decryption_profile_provider.h"
#include "radio_settings.h"
#include "recording_sessions.h"
#include "saved_systems_model.h"
#include "session_args.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <QByteArray>
#include <QChar>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QRegularExpression>
#include <QVariant>
#include <Qt>
#include <QtGlobal>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/tetra_rate.h>

#include "app_prefs.h"

namespace dsd_qt {

namespace {

/* Whether the short option @p letter takes a value, by the parser's own option string. */
bool
short_takes_value(QChar letter) {
    return letter.unicode() < 0x80 && dsd_cli_short_option_takes_value(static_cast<char>(letter.unicode())) != 0;
}

bool
short_option_grouped(const QString& token) {
    if (token.size() <= 2 || !token.startsWith(QLatin1Char('-')) || token.startsWith(QLatin1String("--"))) {
        return false;
    }
    // For one of the argument-taking letters, all remaining characters are its
    // argument (e.g. -ft or -GFT.csv), not more options. Otherwise refuse the
    // group outright, including unknown letters, in both frontend start paths.
    return !short_takes_value(token.at(1));
}

/* Whether getopt knows the short option @p letter, by the parser's own option string (its ':'s are no option). */
bool
short_known(QChar letter) {
    return letter != QLatin1Char(':') && QLatin1String(dsd_cli_short_options()).contains(letter);
}

/**
 * @brief The options the parser reads in @p tokens, found as the decoder finds them, in its two passes.
 *
 * First its long options come out of the whole list wherever they stand, a short option's value position included,
 * each with the value its arity takes (dsd_cli_long_option_arity()), up to the first "--" that is no long option's
 * value, as dsd_parse_args()' pre-scan and dsd_cli_compact_args() take them; that "--" and everything after it stay, as
 * the compaction keeps them. Then getopt reads what is left, by its own rules: an element starting with '-' is a group
 * of short options, and the first of them that takes a value takes the rest of the element, or the next element
 * whatever it is ("--" included); a "--" that is no value ends the options; an operand is stepped over (glibc's getopt
 * permutes past it; one that stops there reads fewer options, never more); an unknown option ends the parse with an
 * error, and nothing after it is read. A long option the first pass does not know reaches getopt as option '-', which
 * is unknown: it is returned as typed, so its name can still be refused, and ends the parse.
 *
 * Long options are returned as typed, short ones as "-" and the letter.
 */
QStringList
option_tokens(const QString& tokens) {
    const QStringList split = tokens.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QStringList options;
    QStringList rest;
    for (qsizetype i = 0; i < split.size(); ++i) {
        const QString& token = split.at(i);
        if (token == QLatin1String("--")) {
            rest.append(split.mid(i));
            break;
        }
        const dsd_cli_long_arity arity = dsd_cli_long_option_arity(token.toUtf8().constData());
        if (arity == DSD_CLI_LONG_UNKNOWN) {
            rest.append(token);
            continue;
        }
        options.append(token);
        const bool hasNext = i + 1 < split.size();
        const bool takesNext =
            arity == DSD_CLI_LONG_NEXT
            || (arity == DSD_CLI_LONG_NEXT_NON_OPTION && hasNext && !split.at(i + 1).startsWith(QLatin1Char('-')));
        if (takesNext && hasNext) {
            ++i;
        }
    }
    for (qsizetype i = 0; i < rest.size(); ++i) {
        const QString& token = rest.at(i);
        if (token == QLatin1String("--")) {
            break;
        }
        if (token.size() < 2 || !token.startsWith(QLatin1Char('-'))) {
            continue;
        }
        if (token.startsWith(QLatin1String("--"))) {
            options.append(token);
            break;
        }
        for (qsizetype at = 1; at < token.size(); ++at) {
            const QChar letter = token.at(at);
            if (!short_known(letter)) {
                return options;
            }
            options.append(QLatin1Char('-') + letter);
            if (short_takes_value(letter)) {
                if (at + 1 == token.size()) {
                    ++i;
                }
                break;
            }
        }
    }
    return options;
}

/** @brief Whether @p token is the long option @p name, alone or in its "=value" form. */
bool
long_option_is(const QString& token, QLatin1String name) {
    return token == name
           || (token.startsWith(name) && token.size() > name.size() && token.at(name.size()) == QLatin1Char('='));
}

/**
 * @brief Whether the extra options the user typed name the CRC policy themselves.
 *
 * -F or --strict-crc there wins over the app's Relaxed CRC preference: the app adds no -F of its own beside it.
 */
bool
extra_sets_crc(const QString& tokens) {
    const QStringList split = tokens.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    return split.contains(QStringLiteral("-F")) || split.contains(QStringLiteral("--strict-crc"));
}

/**
 * @brief Effective bias-tee setting from the per-system tri-state.
 *
 * -1 follows the app-wide preference, 0 is explicitly off, 1 explicitly on. An
 * explicit off must win over a global on: the wizard's switch is the operator
 * saying this dongle or antenna must not be fed the tee's 4.5 V. A legacy bool
 * (stores written before the tri-state) reads true as explicit-on and false as
 * follow — false was the untouched default, which always followed the pref.
 */
bool
bias_tee_effective(const QVariant& stored, bool prefDefault) {
    if (stored.typeId() == QMetaType::Bool) {
        return stored.toBool() || prefDefault;
    }
    const int mode = stored.isValid() ? stored.toInt() : -1;
    if (mode == 0) {
        return false;
    }
    if (mode == 1) {
        return true;
    }
    return prefDefault;
}

/** @brief The system's PPM override, or the app-wide default, '+' sign stripped. */
QString
normalized_ppm(const QVariantMap& system, const SessionArgPrefs& prefs) {
    QString ppm = system.value(QStringLiteral("ppm")).toString().trimmed();
    if (ppm.isEmpty()) {
        ppm = QString::number(prefs.ppm);
    }
    // The wizard's IntValidator accepts an explicit '+' sign that the shape
    // check would refuse; it means the same thing, so drop it rather than make
    // "+5" a saved system that can never start.
    if (ppm.startsWith(QLatin1Char('+'))) {
        ppm.remove(0, 1);
    }
    return ppm;
}

/** Resolve the optional system override before any argv is emitted. */
bool
resolve_hangtime(const QVariantMap& system, const SessionArgPrefs& prefs, double& seconds) {
    const QString overrideText = system.value(QStringLiteral("hangtime")).toString().trimmed();
    bool ok = true;
    seconds = overrideText.isEmpty() ? prefs.hangtimeSec : overrideText.toDouble(&ok);
    return ok && std::isfinite(seconds) && seconds >= 0.0 && seconds <= 30.0;
}

/** @brief Snap the shared bandwidth preference to the DSP bandwidths an Airspy takes (the narrower of two as near). */
int
airspy_bandwidth(int requested) {
    if (requested <= 0) {
        return DSD_ANALOG_RTL_DSP_BW_DEFAULT_KHZ;
    }
    return dsd_analog_rtl_dsp_bw_nearest(requested, DSD_ANALOG_AIRSPY_DSP_BW_MAX_KHZ);
}

/** @brief Append "-i <spec>" for the system's source type. */
void
append_input_args(QStringList& args, const QVariantMap& system, const QString& sourceType, const QString& tail,
                  bool bias) {
    if (sourceType == QLatin1String("airspy")) {
        const QStringList fields = tail.split(QLatin1Char(':'));
        args << QStringLiteral("-i")
             << QStringLiteral("airspy:%1:%2:%3:%4")
                    .arg(fields.value(1), QString::number(airspy_bandwidth(fields.value(4).toInt())), fields.value(5),
                         fields.value(6));
        const QVariantMap native = system.value(QStringLiteral("airspy")).toMap();
        const QStringList keys{
            QStringLiteral("serial"),           QStringLiteral("sample_rate"),    QStringLiteral("gain_mode"),
            QStringLiteral("sensitivity_gain"), QStringLiteral("linearity_gain"), QStringLiteral("lna_gain"),
            QStringLiteral("mixer_gain"),       QStringLiteral("vga_gain"),       QStringLiteral("lna_agc"),
            QStringLiteral("mixer_agc"),        QStringLiteral("bias_tee")};
        for (const auto& key : keys) {
            if (native.contains(key)) {
                QString flag = key;
                flag.replace(QLatin1Char('_'), QLatin1Char('-'));
                args << QStringLiteral("--airspy-") + flag << native.value(key).toString();
            }
        }
    } else if (sourceType == QLatin1String("usb")) {
        QString spec = QStringLiteral("rtl:0") + tail;
        if (bias) {
            spec += QLatin1String(":bias");
        }
        args << QStringLiteral("-i") << spec;
    } else if (sourceType == QLatin1String("rtltcp")) {
        // The engine parses a trailing bias token on rtltcp specs exactly as it
        // does on rtl ones; a remote dongle feeding an LNA needs it just as much.
        // The host is spliced into the ':'-delimited spec verbatim, so stray
        // whitespace from the soft keyboard or a paste must be trimmed here —
        // "10.0.2.2 " resolves to nothing and the start fails opaquely.
        QString spec = QStringLiteral("rtltcp:%1:%2")
                           .arg(system.value(QStringLiteral("host")).toString().trimmed())
                           .arg(system.value(QStringLiteral("port")).toInt())
                       + tail;
        if (bias) {
            spec += QLatin1String(":bias");
        }
        args << QStringLiteral("-i") << spec;
    } else if (sourceType == QLatin1String("udp")) {
        args << QStringLiteral("-i")
             << QStringLiteral("udp:0.0.0.0:%1").arg(system.value(QStringLiteral("port")).toInt());
    } else if (sourceType == QLatin1String("tcp")) {
        args << QStringLiteral("-i")
             << QStringLiteral("tcp:%1:%2")
                    .arg(system.value(QStringLiteral("host")).toString().trimmed())
                    .arg(system.value(QStringLiteral("port")).toInt());
    } else {
        args << QStringLiteral("-i") << system.value(QStringLiteral("filePath")).toString();
    }
}

/**
 * @brief Append the per-system CSV paths as discrete argv elements.
 *
 * Discrete, not folded into extraArgs: the extras field is whitespace-split
 * with no quoting, so an imported file whose display name carries a space
 * ("chan map.csv") only survives as its own element.
 */
void
append_csv_args(QStringList& args, const QVariantMap& system) {
    const QString chan = system.value(QStringLiteral("chanCsvPath")).toString();
    if (!chan.isEmpty()) {
        args << QStringLiteral("-C") << chan;
    }
    const QString group = system.value(QStringLiteral("groupCsvPath")).toString();
    if (!group.isEmpty()) {
        args << QStringLiteral("-G") << group;
    }
    const QString key = system.value(QStringLiteral("keyCsvPath")).toString();
    if (!key.isEmpty()) {
        args << (system.value(QStringLiteral("keyCsvHex")).toBool() ? QStringLiteral("-K") : QStringLiteral("-k"))
             << key;
    }
    const QString bandplan = system.value(QStringLiteral("p25BandplanCsvPath")).toString();
    if (!bandplan.isEmpty()) {
        args << QStringLiteral("--p25-bandplan") << bandplan;
    }
    const QString src = system.value(QStringLiteral("srcCsvPath")).toString();
    if (!src.isEmpty()) {
        args << QStringLiteral("--src-csv") << src;
    }
}

/**
 * @brief Append the call recording the app owns (issue #708), before the extra options: the store's folder and its
 * journal always, so a -P among the extra options records into a folder the app manages, then -P and
 * --record-unknown-only as the preferences say. Nothing without a folder.
 */
void
append_recording_args(QStringList& args, const SessionArgPrefs& prefs) {
    if (prefs.recordingFolder.isEmpty()) {
        return;
    }
    args << QStringLiteral("-7") << prefs.recordingFolder << QStringLiteral("--wav-journal")
         << prefs.recordingFolder + QLatin1Char('/') + QLatin1String(kRecordingJournalFileName);
    // Unknown-only travels with -P: with Record calls off, the switch Settings shows disabled must not filter a
    // recording the user's own extra options make (-w, an extra -P, MBE capture).
    if (prefs.recordCalls) {
        args << QStringLiteral("-P");
        if (prefs.recordUnknownOnly) {
            args << QStringLiteral("--record-unknown-only");
        }
    }
}

/** @brief Append the decode chip, trunking, policy flags, call recording, and extra CLI args. */
void
append_flag_args(QStringList& args, const QVariantMap& system, const SessionArgPrefs& prefs, double hangtime) {
    const QString decodeFlag = system.value(QStringLiteral("decodeFlag")).toString().trimmed();
    if (!decodeFlag.isEmpty()) {
        // A chip may carry several flags, so split rather than push whole.
        args << decodeFlag.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    }
    if (system.value(QStringLiteral("trunking")).toBool()) {
        args << QStringLiteral("-T");
    }
    args << (prefs.persistTgLockouts ? QStringLiteral("--tg-lockout-persist") : QStringLiteral("--tg-lockout-session"));
    if (prefs.skipEncrypted) {
        args << QStringLiteral("--enc-lockout");
    }
    if (prefs.autoPpm && system.value(QStringLiteral("sourceType")).toString() != QLatin1String("airspy")) {
        args << QStringLiteral("--auto-ppm");
    }
    args << QStringLiteral("-t") << QString::number(hangtime, 'f', 1);
    const QString extra =
        (system.value(QStringLiteral("extraArgs")).toString() + QLatin1Char(' ') + prefs.extraArgs).trimmed();
    if (prefs.relaxedCrc && !extra_sets_crc(extra)) {
        args << QStringLiteral("-F");
    }
    append_recording_args(args, prefs);
    if (!extra.isEmpty()) {
        args << extra.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    }
}

void
append_key_args(QStringList& args, int keyIndex, const QString& keyType, const QString& keyValue, int force) {
    if (keyIndex >= 0) {
        const QStringList flags{QStringLiteral("-b"),
                                QStringLiteral("-H"),
                                QStringLiteral("-1"),
                                QStringLiteral("-R"),
                                QStringLiteral("--m17-scrambler-key"),
                                QStringLiteral("--m17-aes-key")};
        args << flags[keyIndex] << session_args_key_value_normalize(keyType, keyValue);
    }
    if (force != 0) {
        args << (force == 1 ? QStringLiteral("-4") : QStringLiteral("-0"));
    }
}

SessionArgsError
validate_key_args(const QVariantMap& system, const QString& keyType, const QString& keyValue, int keyIndex) {
    if ((!keyType.isEmpty() || !keyValue.isEmpty())
        && !system.value(QStringLiteral("keyCsvPath")).toString().isEmpty()) {
        return SessionArgsError::KeyConflict;
    }
    if (!session_args_key_valid(keyType, keyValue)) {
        const SessionArgsError reasons[] = {SessionArgsError::KeyBasic,        SessionArgsError::KeyHex,
                                            SessionArgsError::KeyRc4,          SessionArgsError::KeyScrambler,
                                            SessionArgsError::KeyM17Scrambler, SessionArgsError::KeyM17Aes};
        return keyIndex < 0 ? SessionArgsError::KeyType : reasons[keyIndex];
    }
    bool forceOk = false;
    const int force = system.value(QStringLiteral("encForceKey"), 0).toInt(&forceOk);
    if (!forceOk || force < 0 || force > 2) {
        return SessionArgsError::ForceKey;
    }

    return SessionArgsError::None;
}

} // namespace

bool
session_args_extra_safe(const QString& tokens) {
    const auto split = tokens.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    return std::none_of(split.cbegin(), split.cend(), [](const QString& token) {
        return short_option_grouped(token) || token == QLatin1String("--show-keys")
               || token.startsWith(QLatin1String("--show-keys="));
    });
}

bool
session_args_scan_extra_safe(const QString& tokens) {
    if (!session_args_extra_safe(tokens)) {
        return false;
    }
    const auto parts = tokens.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const QStringList prefixes{"-C",
                               "-Y",
                               "-T",
                               "-i",
                               "-f",
                               "-m",
                               "--trunk-scan",
                               "--p25-bandplan",
                               "--iq-replay",
                               "--scan-voice-only",
                               "--no-scan-voice-only"};
    return std::none_of(parts.cbegin(), parts.cend(), [&prefixes](const QString& token) {
        return std::any_of(prefixes.cbegin(), prefixes.cend(),
                           [&token](const QString& prefix) { return token.startsWith(prefix); });
    });
}

SessionArgsError
session_args_recording_extra_error(const QString& tokens, bool recordCalls, bool noStorage) {
    const QStringList options = option_tokens(tokens);
    const bool owned = std::any_of(options.cbegin(), options.cend(), [](const QString& token) {
        return token == QLatin1String("-7") || long_option_is(token, QLatin1String("--wav-journal"))
               || long_option_is(token, QLatin1String("--rdio-api-delete-after-upload"));
    });
    if (owned) {
        return SessionArgsError::CallRecordingOption;
    }
    if (recordCalls && options.contains(QStringLiteral("-w"))) {
        return SessionArgsError::CallRecordingStaticWav;
    }
    return noStorage && options.contains(QStringLiteral("-P")) ? SessionArgsError::CallRecordingNoStorage
                                                               : SessionArgsError::None;
}

static QString
validateScanEndpoint(const QVariantMap& list) {
    if (list.value("sourceType") == "rtltcp") {
        const QString host = list.value("host").toString().trimmed();
        const int port = list.value("port").toInt();
        if (host.isEmpty() || host.contains(QRegularExpression(QStringLiteral("[:\\s]"))) || port < 1 || port > 65535) {
            return QStringLiteral("Enter an RTL-TCP host and port from 1 to 65535.");
        }
    }
    return {};
}

static QString
validateScanTuner(const QVariantMap& list) {
    for (const auto& field : {"gainDb", "bandwidthKhz", "biasTee"}) {
        const QVariant stored = list.value(field, -1);
        if (QString(field) == "biasTee" && stored.typeId() == QMetaType::Bool) {
            continue;
        }
        bool ok = false;
        const int value = stored.toString().toInt(&ok);
        if (!ok || value < -1 || (QString(field) == "gainDb" && value > 49)
            || (QString(field) == "biasTee" && value > 1)) {
            return QStringLiteral(
                "Enter whole-number tuner settings: gain -1..49, bandwidth -1 or nonnegative, bias tee -1..1.");
        }
    }
    return {};
}

static void
appendScanOptions(QStringList& args, const QVariantMap& list, const QString& csvPath, const SessionArgPrefs& prefs) {
    args << QStringLiteral("--trunk-scan") << csvPath;
    for (const auto& field : {"groupCsvPath", "srcCsvPath"}) {
        if (!list.value(field).toString().isEmpty()) {
            args << (QString(field) == "groupCsvPath" ? QStringLiteral("-G") : QStringLiteral("--src-csv"))
                 << list.value(field).toString();
        }
    }
    if (list.value("defaultDwellMs").toInt()) {
        args << QStringLiteral("--trunk-scan-dwell-ms") << list.value("defaultDwellMs").toString();
    }
    if (list.value("defaultHoldMs").toInt()) {
        args << QStringLiteral("--trunk-scan-activity-hold-ms") << list.value("defaultHoldMs").toString();
    }
    if (list.value("voiceOnly").toBool()) {
        args << QStringLiteral("--scan-voice-only");
    }
    if (prefs.skipEncrypted) {
        args << QStringLiteral("--enc-lockout");
    }
    if (prefs.autoPpm && list.value(QStringLiteral("sourceType")).toString() != QLatin1String("airspy")) {
        args << QStringLiteral("--auto-ppm");
    }
    /* The configured Relaxed CRC a row's own -F or --strict-crc runs over while it is on air. */
    if (prefs.relaxedCrc && !extra_sets_crc(prefs.extraArgs)) {
        args << QStringLiteral("-F");
    }
    args << prefs.extraArgs.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

QStringList
session_args_scan_build(const QVariantMap& list, const QString& firstFreqMhz, const QString& csvPath,
                        const SessionArgPrefs& prefs, QString* error) {
    if (error) {
        error->clear();
    }
    if (!session_args_scan_extra_safe(prefs.extraArgs)) {
        if (error) {
            *error = session_args_error_text(SessionArgsError::UnsafeOption);
        }
        return {};
    }
    const SessionArgsError recordingError =
        session_args_recording_extra_error(prefs.extraArgs, prefs.recordCalls, prefs.recordingUnavailable);
    if (recordingError != SessionArgsError::None) {
        if (error) {
            *error = session_args_error_text(recordingError);
        }
        return {};
    }
    QString validation = validateScanEndpoint(list);
    if (validation.isEmpty()) {
        validation = validateScanTuner(list);
    }
    if (!validation.isEmpty()) {
        if (error) {
            *error = std::move(validation);
        }
        return {};
    }
    QVariantMap tuner;
    for (const auto& field : {"sourceType", "host", "port", "gainDb", "ppm", "bandwidthKhz", "biasTee", "airspy"}) {
        if (list.contains(field)) {
            tuner[field] = list.value(field);
        }
    }
    tuner["freqMhz"] = firstFreqMhz;
    SessionArgsError reason;
    SessionArgPrefs tunerPrefs = prefs;
    tunerPrefs.skipEncrypted = false;
    tunerPrefs.autoPpm = false;
    tunerPrefs.relaxedCrc = false;
    tunerPrefs.extraArgs.clear();
    // The tuner's build names the call recording (the list's folder), before the scan options and the extra options.
    QStringList args = session_args_build(tuner, tunerPrefs, &reason);
    if (reason != SessionArgsError::None) {
        if (error) {
            *error = session_args_error_text(reason);
        }
        return {};
    }
    appendScanOptions(args, list, csvPath, prefs);
    return args;
}

bool
session_args_freq_valid(const QString& freqMhz) {
    bool ok = false;
    const double mhz = freqMhz.trimmed().toDouble(&ok);
    return ok && std::isfinite(mhz) && mhz > 0.0;
}

bool
session_args_am_fits_bandwidth(const QString& sourceType, int bandwidthKhz) {
    if (sourceType != QLatin1String("usb") && sourceType != QLatin1String("rtltcp")) {
        return true;
    }
    /* The engine reads the spec's bandwidth field this way: a value it does not run is the 48 kHz default. */
    const int khz = dsd_analog_rtl_dsp_bw_or_default(bandwidthKhz);
    return dsd_analog_width_check(DSD_ANALOG_DEMOD_AM, dsd_analog_width_default_hz(DSD_ANALOG_DEMOD_AM), khz * 1000,
                                  nullptr, 0U)
           == 0;
}

QString
session_args_am_bandwidth_choices() {
    char fits[64];
    (void)dsd_analog_width_fitting_rtl_bandwidths(dsd_analog_width_default_hz(DSD_ANALOG_DEMOD_AM), fits, sizeof fits);
    return QString::fromUtf8(fits);
}

QString
session_args_key_hex_normalize(const QString& value) {
    QString normalized;
    normalized.reserve(value.size());
    for (const QChar c : value) {
        if (!c.isSpace() && c != QLatin1Char('-') && c != QLatin1Char(':')) {
            normalized.append(c);
        }
    }
    if (normalized.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        normalized.remove(0, 2);
    }
    return normalized.toUpper();
}

bool
session_args_key_type_hex(const QString& type) {
    return type == QLatin1String("hex") || type == QLatin1String("rc4") || type == QLatin1String("m17Scrambler")
           || type == QLatin1String("m17Aes");
}

QString
session_args_key_value_normalize(const QString& type, const QString& value) {
    return session_args_key_type_hex(type) ? session_args_key_hex_normalize(value) : value.trimmed();
}

QString
session_args_key_protocol_error(const QString& protocol, const QString& type, const QString& value) {
    if (type != QLatin1String("hex")) {
        return {};
    }
    const auto width = session_args_key_hex_normalize(value).size();
    if (protocol == QLatin1String("p25") && width == 10) {
        return QObject::tr("P25 AES needs 32 or 64 hex digits.");
    }
    if (protocol == QLatin1String("nxdn") && width > 0 && width != 64) {
        return QObject::tr("NXDN AES needs 64 hex digits.");
    }
    return {};
}

static bool
decimal_key_valid(const QString& type, const QString& value) {
    const QString decimal = value.trimmed();
    if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(decimal).hasMatch()) {
        return false;
    }
    bool ok = false;
    const auto number = decimal.toUInt(&ok);
    return ok && number <= (type == QLatin1String("basic") ? 255U : 32767U);
}

static bool
hex_key_width_valid(const QString& type, const QString& hex) {
    const auto size = hex.size();
    if (type == QLatin1String("m17Scrambler")) {
        return (size == 2 || size == 4 || size == 6) && hex.count(QLatin1Char('0')) != size;
    }
    if (type == QLatin1String("m17Aes")) {
        return (size == 32 || size == 48 || size == 64) && hex.count(QLatin1Char('0')) != size;
    }
    return type == QLatin1String("rc4") ? size >= 1 && size <= 16 : size == 10 || size == 32 || size == 64;
}

bool
session_args_key_valid(const QString& type, const QString& value) {
    if (type.isEmpty()) {
        return value.isEmpty();
    }
    if (type == QLatin1String("basic") || type == QLatin1String("scrambler")) {
        return decimal_key_valid(type, value);
    }
    if (!session_args_key_type_hex(type)) {
        return false;
    }
    const QString hex = session_args_key_hex_normalize(value);
    if (!QRegularExpression(QStringLiteral("^[0-9A-F]+$")).match(hex).hasMatch()) {
        return false;
    }
    return hex_key_width_valid(type, hex);
}

namespace {

struct SessionArgsErrorText {
    SessionArgsError error;
    const char* text;
};

/* One row per SessionArgsError value, in the enum's order (None has no text), so a value is its row's index. */
constexpr SessionArgsErrorText k_error_texts[] = {
    {SessionArgsError::None, ""},
    {SessionArgsError::Frequency, "Enter a positive frequency in MHz."},
    {SessionArgsError::Ppm, "Enter a whole number for PPM."},
    {SessionArgsError::Hangtime, "Enter hang time in seconds from 0 to 30."},
    {SessionArgsError::KeyType, "Choose one encryption key type."},
    {SessionArgsError::KeyBasic, "Enter a basic key from 0 to 255."},
    {SessionArgsError::KeyHex, "Enter 10, 32, or 64 hexadecimal digits."},
    {SessionArgsError::KeyRc4, "Enter 1 to 16 hexadecimal digits."},
    {SessionArgsError::KeyScrambler, "Enter a scrambler key from 0 to 32767."},
    {SessionArgsError::KeyM17Scrambler, "Enter a nonzero M17 seed with 2, 4, or 6 hex digits."},
    {SessionArgsError::KeyM17Aes,
     "Enter an M17 AES key with 32, 48, or 64 hex digits; an all-zero key is unavailable to the decoder."},
    {SessionArgsError::KeyConflict, "Choose either a direct key or a key CSV file."},
    {SessionArgsError::ForceKey, "Choose force key mode 0, 1, or 2."},
    {SessionArgsError::UnsafeOption, "Extra options contain a prohibited option or grouped short options. "
                                     "Remove prohibited options and write each short option separately."},
    {SessionArgsError::AmNeedsRadio, "AM needs a radio source (USB, Airspy or rtl_tcp): network and file audio "
                                     "arrives already demodulated. Choose a radio source, or another decode mode."},
    /* %1: session_args_am_bandwidth_choices(). */
    {SessionArgsError::AmBandwidth, "AM needs a bandwidth of 8 kHz or more: its 6 kHz channel does not fit a "
                                    "narrower one. Set the bandwidth to %1 kHz, or choose another decode mode."},
    {SessionArgsError::TetraNeedsIq, "TETRA needs a radio source (USB, Airspy or rtl_tcp) or a symbol capture (.bin) "
                                     "file: network audio and audio files arrive already demodulated. Choose a radio "
                                     "source, or another decode mode."},
    {SessionArgsError::RecordingMissing,
     "This system's recording is missing. Edit the system and choose its recording again."},
    {SessionArgsError::CallRecordingOption,
     "The app decides where call recordings are saved and when they are deleted. Remove -7, --wav-journal and "
     "--rdio-api-delete-after-upload from the extra decoder arguments."},
    {SessionArgsError::CallRecordingStaticWav,
     "The -w option records to one file and can't run while Record calls is on. Remove it from the extra decoder "
     "arguments, or turn off Record calls in Settings."},
    {SessionArgsError::CallRecordingNoStorage,
     "There's no storage for call recordings right now, so -P can't record. Remove it from the extra decoder "
     "arguments to listen without recording."},
};

constexpr std::size_t k_error_text_count = sizeof k_error_texts / sizeof k_error_texts[0];

constexpr bool
error_texts_in_enum_order() {
    for (std::size_t i = 0; i < k_error_text_count; i++) {
        if (static_cast<std::size_t>(k_error_texts[i].error) != i) {
            return false;
        }
    }
    return true;
}

/* A value added to SessionArgsError needs its row: the last value (SessionArgsErrorLast) closes the table. */
static_assert(k_error_text_count == static_cast<std::size_t>(SessionArgsErrorLast) + 1U,
              "every SessionArgsError value needs a row in k_error_texts");
static_assert(error_texts_in_enum_order(), "k_error_texts rows must follow the SessionArgsError order");

} // namespace

QString
session_args_error_text(SessionArgsError error) {
    const auto index = static_cast<std::size_t>(error);
    if (error == SessionArgsError::None || index >= k_error_text_count) {
        return QString();
    }
    const QString text = QString::fromUtf8(k_error_texts[index].text);
    return error == SessionArgsError::AmBandwidth ? text.arg(session_args_am_bandwidth_choices()) : text;
}

bool
session_args_profile_compatible(const QVariantMap& system) {
    const QString protocol = system.value("decryptionProtocol").toString();
    if (protocol.isEmpty() || protocol == "mixed" || system.value("decryptionLegacy").toBool()) {
        return true;
    }
    const QString flags = system.value("decodeFlag").toString();
    // ProVoice (-fp) has no profile protocol of its own, and dPMR (-fm) also takes an NXDN profile's scrambler, so
    // both expect mixed: any profile.
    QString expected = "mixed";
    // -mq is the P25 Simulcast preset (Util.decryptionProtocol() reads it as P25 too).
    if (flags.contains("-ft") || flags.contains("-f1") || flags.contains("-f2") || flags.contains("-mq")) {
        expected = "p25";
    } else if (flags.contains("-fs")) {
        expected = "dmr";
    } else if (flags.contains("-fi") || flags.contains("-fn")) {
        expected = "nxdn";
    } else if (flags.contains("-fz")) {
        expected = "m17";
    } else if (flags.contains("-fd")) {
        expected = "dstar";
    } else if (flags.contains("-fy")) {
        expected = "ysf";
    }
    return expected == "mixed" || expected == protocol;
}

static void
append_profile_args(QStringList& args, const QVariantMap& system) {
    for (const auto& file : {qMakePair(QStringLiteral("keysHexCsvPath"), QStringLiteral("-K")),
                             qMakePair(QStringLiteral("keysDecCsvPath"), QStringLiteral("-k")),
                             qMakePair(QStringLiteral("dmrTgKeyCsvPath"), QStringLiteral("--dmr-tg-key-csv"))}) {
        if (!system.value(file.first).toString().isEmpty()) {
            args << file.second << system.value(file.first).toString();
        }
    }
    if (system.value("dmrTgKeyClear").toBool()) {
        args << QStringLiteral("--dmr-tg-key-clear");
    }
    if (system.value("decryptionClearKeys").toBool()) {
        args << QStringLiteral("--no-decryption-keys");
    }
    if (system.contains("decryptionForce")) {
        const int profileForce = system.value("decryptionForce").toInt();
        if (profileForce == 0) {
            args << QStringLiteral("--no-force-key");
        } else if (profileForce == 1) {
            args << QStringLiteral("-4");
        } else if (profileForce > 1) {
            args << QStringLiteral("--dmr-force-algid") << QString::number(profileForce, 16);
        }
    }
}

/* Whether a saved decode flag selects the preset @p flag names (-fM for AM, issue #524; -fT for TETRA), alone or among
   other tokens. */
static bool
decode_flag_names(const QString& decodeFlag, const QString& flag) {
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    return decodeFlag.split(whitespace, Qt::SkipEmptyParts).contains(flag);
}

static bool
is_radio_source(const QString& source) {
    return source == QLatin1String("usb") || source == QLatin1String("airspy") || source == QLatin1String("rtltcp");
}

/* The DSP bandwidth, in kHz, a system's radio spec carries: its own override, else 72 kHz for TETRA, which runs only
   at a multiple of 18 kHz (as -fT takes it on the command line), else the app-wide default. */
static int
session_args_bandwidth_khz(const QVariantMap& system, const SessionArgPrefs& prefs) {
    const int bwOverride = system.value(QStringLiteral("bandwidthKhz"), -1).toInt();
    if (bwOverride > 0) {
        return bwOverride;
    }
    return decode_flag_names(system.value(QStringLiteral("decodeFlag")).toString(), QStringLiteral("-fT"))
               ? DSD_TETRA_DSP_BW_KHZ
               : prefs.bandwidthKhz;
}

/* What the source allows. A radio source needs a frequency it can tune. Issue #524: -fM demodulates AM from the radio's
   I/Q, and the engine refuses it on audio that arrives already demodulated; the wizard keeps the pair from being saved,
   and a system saved before that fails here with a reason rather than at engine startup. The same holds for an AM
   channel the radio's DSP bandwidth cannot filter (session_args_am_fits_bandwidth()), and for -fT, whose symbols the
   engine reads from a radio's I/Q or a symbol capture file only (dsd_opts_audio_in_dev_is_symbol_capture()). A file
   source needs its recording first (issue #580: a restored backup leaves every replay system without one), so the
   reason it cannot start names that, not what the decode mode would need of the file. */
static SessionArgsError
session_args_source_error(const QVariantMap& system, const QString& sourceType, int bandwidthKhz,
                          const QString& freqMhz) {
    const QString decodeFlag = system.value(QStringLiteral("decodeFlag")).toString();
    const bool am = decode_flag_names(decodeFlag, QStringLiteral("-fM"));
    if (!is_radio_source(sourceType)) {
        if (am) {
            return SessionArgsError::AmNeedsRadio;
        }
        if (sourceType == QLatin1String("file")
            && system.value(QStringLiteral("filePath")).toString().trimmed().isEmpty()) {
            return SessionArgsError::RecordingMissing;
        }
        const QByteArray path = system.value(QStringLiteral("filePath")).toString().toUtf8();
        const bool symbols =
            sourceType == QLatin1String("file") && dsd_opts_audio_in_dev_is_symbol_capture(path.constData());
        return decode_flag_names(decodeFlag, QStringLiteral("-fT")) && !symbols ? SessionArgsError::TetraNeedsIq
                                                                                : SessionArgsError::None;
    }
    if (!session_args_freq_valid(freqMhz)) {
        return SessionArgsError::Frequency;
    }
    return am && !session_args_am_fits_bandwidth(sourceType, bandwidthKhz) ? SessionArgsError::AmBandwidth
                                                                           : SessionArgsError::None;
}

QStringList
session_args_build(const QVariantMap& system, const SessionArgPrefs& prefs, SessionArgsError* error) {
    if (error != nullptr) {
        *error = SessionArgsError::None;
    }
    const auto fail = [error](SessionArgsError reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return QStringList();
    };

    // WP-F5: never permit a saved/pasted option to reveal keys in any sink.
    const QString tokens = system.value(QStringLiteral("extraArgs")).toString() + QLatin1Char(' ') + prefs.extraArgs
                           + QLatin1Char(' ') + system.value(QStringLiteral("decodeFlag")).toString();
    if (!session_args_extra_safe(tokens)) {
        return fail(SessionArgsError::UnsafeOption);
    }
    const SessionArgsError recordingError =
        session_args_recording_extra_error(tokens, prefs.recordCalls, prefs.recordingUnavailable);
    if (recordingError != SessionArgsError::None) {
        return fail(recordingError);
    }
    if (!radio_settings_valid(system.value(QStringLiteral("radioSettings")))) {
        return fail(SessionArgsError::UnsafeOption);
    }
    const auto radioSettings = system.value(QStringLiteral("radioSettings")).toMap();
    if (radioSettings.contains(QStringLiteral("decodeFlag"))
        && radioSettings.value(QStringLiteral("decodeFlag")) != system.value(QStringLiteral("decodeFlag"))) {
        return fail(SessionArgsError::UnsafeOption);
    }

    const QString sourceType = system.value(QStringLiteral("sourceType")).toString();
    const bool radioSource = is_radio_source(sourceType);
    const QString freqMhz = system.value(QStringLiteral("freqMhz")).toString().trimmed();
    const int bw = session_args_bandwidth_khz(system, prefs);
    const SessionArgsError sourceError = session_args_source_error(system, sourceType, bw, freqMhz);
    if (sourceError != SessionArgsError::None) {
        return fail(sourceError);
    }

    // PPM is the one override persisted as a raw string, and it is spliced
    // verbatim into the ':'-delimited spec below — like the frequency, a
    // malformed value must fail here, not downstream as a silently unapplied
    // correction.
    const QString ppm = normalized_ppm(system, prefs);
    static const QRegularExpression ppmShape(QStringLiteral("^-?\\d+$"));
    if (radioSource && !ppmShape.match(ppm).hasMatch()) {
        return fail(SessionArgsError::Ppm);
    }

    double hangtime = 0.0;
    if (!resolve_hangtime(system, prefs, hangtime)) {
        return fail(SessionArgsError::Hangtime);
    }

    if (!session_args_profile_compatible(system)) {
        return fail(SessionArgsError::KeyType);
    }
    const QString keyType = system.value(QStringLiteral("encKeyType")).toString();
    const QString keyValue = system.value(QStringLiteral("encKeyValue")).toString();
    const QStringList keyTypes{QStringLiteral("basic"),     QStringLiteral("hex"),          QStringLiteral("rc4"),
                               QStringLiteral("scrambler"), QStringLiteral("m17Scrambler"), QStringLiteral("m17Aes")};
    const int keyIndex = keyTypes.indexOf(keyType);
    const SessionArgsError keyError = validate_key_args(system, keyType, keyValue, keyIndex);
    if (keyError != SessionArgsError::None) {
        return fail(keyError);
    }
    const int force = system.value(QStringLiteral("encForceKey"), 0).toInt();

    const int gainOverride = system.value(QStringLiteral("gainDb"), -1).toInt();
    const int gain = gainOverride >= 0 ? gainOverride : prefs.gainDb;
    const bool bias = bias_tee_effective(system.value(QStringLiteral("biasTee")), prefs.biasTee);
    const QString tail = QStringLiteral(":%1M:%2:%3:%4:0:2").arg(freqMhz).arg(gain).arg(ppm).arg(bw);

    QStringList args{QStringLiteral("--frontend"), QStringLiteral("none")};
    append_input_args(args, system, sourceType, tail, bias);
    args << QStringLiteral("-o") << QStringLiteral("pulse");
    append_csv_args(args, system);
    append_profile_args(args, system);
    append_key_args(args, keyIndex, keyType, keyValue, force);
    args << system.value("decryptionVendorArgs").toStringList();
    if (!system.value("decryptionProfileRef").toString().isEmpty()) {
        args << "--key-profile-ref" << system.value("decryptionProfileRef").toString();
    }
    append_flag_args(args, system, prefs, hangtime);
    radio_settings_append(args, radioSettings);
    return args;
}

SessionArgsBuilder::SessionArgsBuilder(const AppPrefs* prefs, QObject* parent) : QObject(parent), m_prefs(prefs) {}

SessionArgsBuilder::~SessionArgsBuilder() = default;

void
SessionArgsBuilder::setSavedSystems(const SavedSystemsModel* systems) {
    m_systems = systems;
}

SessionArgPrefs
session_args_prefs(const AppPrefs* prefs) {
    SessionArgPrefs out;
    if (prefs != nullptr) {
        out.gainDb = prefs->gainDb();
        out.ppm = prefs->ppm();
        out.bandwidthKhz = prefs->bandwidthKhz();
        out.biasTee = prefs->biasTee();
        out.skipEncrypted = prefs->skipEncrypted();
        out.persistTgLockouts = prefs->persistTgLockouts();
        out.autoPpm = prefs->autoPpm();
        out.relaxedCrc = prefs->relaxedCrc();
        out.hangtimeSec = prefs->hangtimeSec();
        out.extraArgs = prefs->extraArgs();
        out.recordCalls = prefs->recordCalls();
        out.recordUnknownOnly = prefs->recordUnknownOnly();
    }
    return out;
}

QStringList
SessionArgsBuilder::buildArgs(const QVariantMap& system, SessionArgsError* error, const QString& recordingFolder,
                              bool recordingUnavailable) const {
    SessionArgPrefs prefs = session_args_prefs(m_prefs);
    prefs.recordingFolder = recordingFolder;
    prefs.recordingUnavailable = recordingUnavailable;
    QVariantMap input = system;
    if (m_systems && !input.contains(QStringLiteral("encKeyValue"))) {
        input.insert(QStringLiteral("encKeyValue"),
                     m_systems->keyValueForUid(input.value(QStringLiteral("uid")).toString()));
    }
    const QString profile = input.value(QStringLiteral("decryptionProfileUid")).toString();
    if (!profile.isEmpty()) {
        QString profileError;
        if (!m_profiles) {
            if (error) {
                *error = SessionArgsError::KeyType;
            }
            return {};
        }
        const auto configuration = m_profiles->configuration(profile, &profileError);
        if (!profileError.isEmpty() || configuration.isEmpty()) {
            if (error) {
                *error = SessionArgsError::KeyType;
            }
            return {};
        }
        for (auto i = configuration.cbegin(); i != configuration.cend(); ++i) {
            input.insert(i.key(), i.value());
        }
    }
    return session_args_build(input, prefs, error);
}

static QVariantMap
validationResult(SessionArgsError error) {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), error == SessionArgsError::None);
    result.insert(QStringLiteral("error"),
                  error == SessionArgsError::Frequency          ? QStringLiteral("frequency")
                  : error == SessionArgsError::Ppm              ? QStringLiteral("ppm")
                  : error == SessionArgsError::Hangtime         ? QStringLiteral("hangtime")
                  : error == SessionArgsError::UnsafeOption     ? QStringLiteral("unsafe-option")
                  : error == SessionArgsError::AmNeedsRadio     ? QStringLiteral("am-needs-radio")
                  : error == SessionArgsError::AmBandwidth      ? QStringLiteral("am-bandwidth")
                  : error == SessionArgsError::TetraNeedsIq     ? QStringLiteral("tetra-needs-iq")
                  : error == SessionArgsError::RecordingMissing ? QStringLiteral("recording-missing")
                  : error == SessionArgsError::CallRecordingOption || error == SessionArgsError::CallRecordingNoStorage
                      ? QStringLiteral("call-recording-option")
                  : error == SessionArgsError::CallRecordingStaticWav ? QStringLiteral("call-recording-static-wav")
                  : error == SessionArgsError::None                   ? QString()
                                                                      : QStringLiteral("encryption"));
    result.insert(QStringLiteral("errorText"), session_args_error_text(error));
    if (error == SessionArgsError::AmBandwidth) {
        result.insert(QStringLiteral("bandwidthChoices"), session_args_am_bandwidth_choices());
    }
    return result;
}

QVariantMap
SessionArgsBuilder::build(const QVariantMap& system) const {
    SessionArgsError error = SessionArgsError::None;
    (void)buildArgs(system, &error);
    return validationResult(error);
}

QVariantMap
SessionArgsBuilder::start(const QVariantMap& system, DecoderHost* host, const QString& kind) const {
    SessionArgsError error = SessionArgsError::None;
    QStringList args = buildArgs(system, &error);
    const bool recording = error == SessionArgsError::None && host && m_recordingSessions;
    if (recording) {
        // A saved system records into its own folder; Explore and Signal search share one, whatever uid the map
        // carries. The folder goes into the argv, so the system is built again with it once it is known valid.
        const QString uid = system.value(QStringLiteral("uid")).toString();
        const bool saved = kind == QLatin1String("saved") && !uid.isEmpty();
        const QString folder =
            m_recordingSessions->beginSession(saved ? QStringLiteral("saved") : QStringLiteral("explore"), uid,
                                              system.value(QStringLiteral("name")).toString());
        // Built again either way: with no folder, an extra -P would record outside the app's storage and is refused.
        args = buildArgs(system, &error, folder, folder.isEmpty());
    }
    auto result = validationResult(error);
    // Noted only for a start that goes to the host: the build with the folder can still refuse it, as a scan list's can.
    if (error == SessionArgsError::None && host) {
        host->noteStartAttempt(system.value(QStringLiteral("sourceType")).toString());
    }
    const bool started = error == SessionArgsError::None && host && host->start(args);
    if (recording && !started && !host->sessionActive()) {
        m_recordingSessions->endSession();
    }
    result.insert(QStringLiteral("started"), started);
    if (started && host->sessionActive() && m_profiles) {
        m_profiles->retainForSession(system.value("decryptionProfileUid").toString());
    }
    return result;
}

bool
// cppcheck-suppress functionStatic // Q_INVOKABLE: QML calls this on the sessionArgs context object.
SessionArgsBuilder::freqValid(const QString& freqMhz) const {
    return session_args_freq_valid(freqMhz);
}

QString
SessionArgsBuilder::amBandwidthError(const QString& sourceType, int bandwidthKhz) const {
    int khz = bandwidthKhz;
    if (khz <= 0) {
        khz = m_prefs != nullptr ? m_prefs->bandwidthKhz() : SessionArgPrefs().bandwidthKhz;
    }
    return session_args_am_fits_bandwidth(sourceType, khz) ? QString()
                                                           : session_args_error_text(SessionArgsError::AmBandwidth);
}

// cppcheck-suppress functionStatic // Q_INVOKABLE: QML validation uses the shared startup rules.
QString
SessionArgsBuilder::keyError(const QString& type, const QString& value, const QString& csvPath, int force,
                             const QString& protocol) const {
    QString rule = session_args_key_protocol_error(protocol, type, value);
    if (!rule.isEmpty()) {
        return rule;
    }
    const QStringList types{QStringLiteral("basic"),     QStringLiteral("hex"),          QStringLiteral("rc4"),
                            QStringLiteral("scrambler"), QStringLiteral("m17Scrambler"), QStringLiteral("m17Aes")};
    const QVariantMap fields{{QStringLiteral("keyCsvPath"), csvPath}, {QStringLiteral("encForceKey"), force}};
    return session_args_error_text(validate_key_args(fields, type, value, types.indexOf(type)));
}

} // namespace dsd_qt
