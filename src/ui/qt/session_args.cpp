// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QPair>
#include <initializer_list>
#include <utility>
#include "decoder_host.h"
#include "decryption_profile_provider.h"
#include "saved_systems_model.h"
#include "session_args.h"

#include <algorithm>
#include <cmath>

#include <QChar>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QRegularExpression>
#include <QVariant>
#include <Qt>

#include "app_prefs.h"

namespace dsd_qt {

namespace {

bool
short_option_grouped(const QString& token) {
    if (token.size() <= 2 || !token.startsWith(QLatin1Char('-')) || token.startsWith(QLatin1String("--"))) {
        return false;
    }
    // Argument-taking letters from src/runtime/cli/args.c's getopt optstring.
    // For one of these leading options, all remaining characters are its
    // argument (e.g. -ft or -GFT.csv), not more options. Otherwise refuse the
    // group outright, including unknown letters, in both frontend start paths.
    const QString argumentOptions = QStringLiteral("stvziodcgnwBCRfmxASMGDLVUKbHXQ@!12567_9kIJ");
    return !argumentOptions.contains(token.at(1));
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

/** @brief Append "-i <spec>" for the system's source type. */
void
append_input_args(QStringList& args, const QVariantMap& system, const QString& sourceType, const QString& tail,
                  bool bias) {
    if (sourceType == QLatin1String("usb")) {
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

/** @brief Append the decode chip, trunking, policy flags, and extra CLI args. */
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
    if (prefs.skipEncrypted) {
        args << QStringLiteral("--enc-lockout");
    }
    if (prefs.autoPpm) {
        args << QStringLiteral("--auto-ppm");
    }
    args << QStringLiteral("-t") << QString::number(hangtime, 'f', 1);
    const QString extra =
        (system.value(QStringLiteral("extraArgs")).toString() + QLatin1Char(' ') + prefs.extraArgs).trimmed();
    if (!extra.isEmpty()) {
        args << extra.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    }
}

void
append_key_args(QStringList& args, int keyIndex, const QString& keyValue, int force) {
    if (keyIndex >= 0) {
        const QStringList flags{QStringLiteral("-b"),
                                QStringLiteral("-H"),
                                QStringLiteral("-1"),
                                QStringLiteral("-R"),
                                QStringLiteral("--m17-scrambler-key"),
                                QStringLiteral("--m17-aes-key")};
        args << flags[keyIndex]
             << ((keyIndex == 1 || keyIndex == 2 || keyIndex >= 4) ? session_args_key_hex_normalize(keyValue)
                                                                   : keyValue.trimmed());
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
    if (prefs.autoPpm) {
        args << QStringLiteral("--auto-ppm");
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
    QString validation = validateScanEndpoint(list);
    if (validation.isEmpty()) {
        validation = validateScanTuner(list);
    }
    if (!validation.isEmpty()) {
        if (error) {
            *error = validation;
        }
        return {};
    }
    QVariantMap tuner;
    for (const auto& field : {"sourceType", "host", "port", "gainDb", "ppm", "bandwidthKhz", "biasTee"}) {
        if (list.contains(field)) {
            tuner[field] = list.value(field);
        }
    }
    tuner["freqMhz"] = firstFreqMhz;
    SessionArgsError reason;
    SessionArgPrefs tunerPrefs = prefs;
    tunerPrefs.skipEncrypted = false;
    tunerPrefs.autoPpm = false;
    tunerPrefs.extraArgs.clear();
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

QString
session_args_key_hex_normalize(const QString& value) {
    QString normalized = value;
    normalized.remove(QRegularExpression(QStringLiteral("[ \\t\\n\\r\\f\\v]")));
    if (normalized.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        normalized.remove(0, 2);
    }
    return normalized.toUpper();
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
    if (type != QLatin1String("hex") && type != QLatin1String("rc4") && type != QLatin1String("m17Scrambler")
        && type != QLatin1String("m17Aes")) {
        return false;
    }
    const QString hex = session_args_key_hex_normalize(value);
    if (!QRegularExpression(QStringLiteral("^[0-9A-F]+$")).match(hex).hasMatch()) {
        return false;
    }
    return hex_key_width_valid(type, hex);
}

QString
session_args_error_text(SessionArgsError error) {
    switch (error) {
        case SessionArgsError::None: return {};
        case SessionArgsError::KeyM17Scrambler:
            return QStringLiteral("Enter a nonzero M17 seed with 2, 4, or 6 hex digits.");
        case SessionArgsError::KeyM17Aes:
            return QStringLiteral(
                "Enter an M17 AES key with 32, 48, or 64 hex digits; an all-zero key is unavailable to the decoder.");
        case SessionArgsError::Frequency: return QStringLiteral("Enter a positive frequency in MHz.");
        case SessionArgsError::Ppm: return QStringLiteral("Enter a whole number for PPM.");
        case SessionArgsError::Hangtime: return QStringLiteral("Enter hang time in seconds from 0 to 30.");
        case SessionArgsError::KeyType: return QStringLiteral("Choose one encryption key type.");
        case SessionArgsError::KeyBasic: return QStringLiteral("Enter a basic key from 0 to 255.");
        case SessionArgsError::KeyHex: return QStringLiteral("Enter 10, 32, or 64 hexadecimal digits.");
        case SessionArgsError::KeyRc4: return QStringLiteral("Enter 1 to 16 hexadecimal digits.");
        case SessionArgsError::KeyScrambler: return QStringLiteral("Enter a scrambler key from 0 to 32767.");
        case SessionArgsError::KeyConflict: return QStringLiteral("Choose either a direct key or a key CSV file.");
        case SessionArgsError::ForceKey: return QStringLiteral("Choose force key mode 0, 1, or 2.");
        case SessionArgsError::UnsafeOption:
            return QStringLiteral("Extra options contain a prohibited option or grouped short options. "
                                  "Remove prohibited options and write each short option separately.");
    }
    return {};
}

bool
session_args_profile_compatible(const QVariantMap& system) {
    const QString protocol = system.value("decryptionProtocol").toString();
    if (protocol.isEmpty() || protocol == "mixed" || system.value("decryptionLegacy").toBool()) {
        return true;
    }
    const QString flags = system.value("decodeFlag").toString();
    QString expected = "mixed";
    if (flags.contains("-ft") || flags.contains("-f1") || flags.contains("-f2")) {
        expected = "p25";
    } else if (flags.contains("-fs")) {
        expected = "dmr";
    } else if (flags.contains("-fi") || flags.contains("-fn")) {
        expected = "nxdn";
    } else if (flags.contains("-fp")) {
        expected = "dpmr";
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

    const QString sourceType = system.value(QStringLiteral("sourceType")).toString();
    const bool radioSource = sourceType == QLatin1String("usb") || sourceType == QLatin1String("rtltcp");
    const QString freqMhz = system.value(QStringLiteral("freqMhz")).toString().trimmed();
    if (radioSource && !session_args_freq_valid(freqMhz)) {
        return fail(SessionArgsError::Frequency);
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
    const int bwOverride = system.value(QStringLiteral("bandwidthKhz"), -1).toInt();
    const int bw = bwOverride > 0 ? bwOverride : prefs.bandwidthKhz;
    const bool bias = bias_tee_effective(system.value(QStringLiteral("biasTee")), prefs.biasTee);
    const QString tail = QStringLiteral(":%1M:%2:%3:%4:0:2").arg(freqMhz).arg(gain).arg(ppm).arg(bw);

    QStringList args{QStringLiteral("--frontend"), QStringLiteral("none")};
    append_input_args(args, system, sourceType, tail, bias);
    args << QStringLiteral("-o") << QStringLiteral("pulse");
    append_csv_args(args, system);
    append_profile_args(args, system);
    append_key_args(args, keyIndex, keyValue, force);
    args << system.value("decryptionVendorArgs").toStringList();
    if (!system.value("decryptionProfileRef").toString().isEmpty()) {
        args << "--key-profile-ref" << system.value("decryptionProfileRef").toString();
    }
    append_flag_args(args, system, prefs, hangtime);
    return args;
}

SessionArgsBuilder::SessionArgsBuilder(const AppPrefs* prefs, QObject* parent) : QObject(parent), m_prefs(prefs) {}

SessionArgsBuilder::~SessionArgsBuilder() = default;

void
SessionArgsBuilder::setSavedSystems(const SavedSystemsModel* systems) {
    m_systems = systems;
}

QStringList
SessionArgsBuilder::buildArgs(const QVariantMap& system, SessionArgsError* error) const {
    SessionArgPrefs prefs;
    if (m_prefs != nullptr) {
        prefs.gainDb = m_prefs->gainDb();
        prefs.ppm = m_prefs->ppm();
        prefs.bandwidthKhz = m_prefs->bandwidthKhz();
        prefs.biasTee = m_prefs->biasTee();
        prefs.skipEncrypted = m_prefs->skipEncrypted();
        prefs.autoPpm = m_prefs->autoPpm();
        prefs.hangtimeSec = m_prefs->hangtimeSec();
        prefs.extraArgs = m_prefs->extraArgs();
    }
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
    result.insert(QStringLiteral("error"), error == SessionArgsError::Frequency      ? QStringLiteral("frequency")
                                           : error == SessionArgsError::Ppm          ? QStringLiteral("ppm")
                                           : error == SessionArgsError::Hangtime     ? QStringLiteral("hangtime")
                                           : error == SessionArgsError::UnsafeOption ? QStringLiteral("unsafe-option")
                                           : error == SessionArgsError::None         ? QString()
                                                                                     : QStringLiteral("encryption"));
    result.insert(QStringLiteral("errorText"), session_args_error_text(error));
    return result;
}

QVariantMap
SessionArgsBuilder::build(const QVariantMap& system) const {
    SessionArgsError error = SessionArgsError::None;
    (void)buildArgs(system, &error);
    return validationResult(error);
}

QVariantMap
SessionArgsBuilder::start(const QVariantMap& system, DecoderHost* host) const {
    SessionArgsError error = SessionArgsError::None;
    const QStringList args = buildArgs(system, &error);
    auto result = validationResult(error);
    const bool started = error == SessionArgsError::None && host && host->start(args);
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

// cppcheck-suppress functionStatic // Q_INVOKABLE: QML validation uses the shared startup rules.
QString
SessionArgsBuilder::keyError(const QString& type, const QString& value, const QString& csvPath, int force) const {
    const QStringList types{QStringLiteral("basic"),     QStringLiteral("hex"),          QStringLiteral("rc4"),
                            QStringLiteral("scrambler"), QStringLiteral("m17Scrambler"), QStringLiteral("m17Aes")};
    const QVariantMap fields{{QStringLiteral("keyCsvPath"), csvPath}, {QStringLiteral("encForceKey"), force}};
    return session_args_error_text(validate_key_args(fields, type, value, types.indexOf(type)));
}

} // namespace dsd_qt
