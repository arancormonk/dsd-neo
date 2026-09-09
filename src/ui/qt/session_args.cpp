// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "session_args.h"
#include "saved_systems_model.h"

#include <algorithm>

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
append_flag_args(QStringList& args, const QVariantMap& system, const SessionArgPrefs& prefs) {
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
    const QString extra =
        (system.value(QStringLiteral("extraArgs")).toString() + QLatin1Char(' ') + prefs.extraArgs).trimmed();
    if (!extra.isEmpty()) {
        args << extra.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    }
}

void
append_key_args(QStringList& args, int keyIndex, const QString& keyValue, int force) {
    if (keyIndex >= 0) {
        const QStringList flags{QStringLiteral("-b"), QStringLiteral("-H"), QStringLiteral("-1"), QStringLiteral("-R")};
        args << flags[keyIndex]
             << ((keyIndex == 1 || keyIndex == 2) ? session_args_key_hex_normalize(keyValue) : keyValue.trimmed());
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
        const SessionArgsError reasons[] = {SessionArgsError::KeyBasic, SessionArgsError::KeyHex,
                                            SessionArgsError::KeyRc4, SessionArgsError::KeyScrambler};
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
        return token == QLatin1String("--show-keys") || token.startsWith(QLatin1String("--show-keys="));
    });
}

bool
session_args_freq_valid(const QString& freqMhz) {
    bool ok = false;
    const double mhz = freqMhz.trimmed().toDouble(&ok);
    return ok && mhz > 0.0;
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

bool
session_args_key_valid(const QString& type, const QString& value) {
    if (type.isEmpty()) {
        return value.isEmpty();
    }
    if (type == QLatin1String("basic") || type == QLatin1String("scrambler")) {
        const QString decimal = value.trimmed();
        if (!QRegularExpression(QStringLiteral("^[0-9]+$")).match(decimal).hasMatch()) {
            return false;
        }
        bool ok = false;
        const auto number = decimal.toUInt(&ok);
        return ok && number <= (type == QLatin1String("basic") ? 255U : 32767U);
    }
    if (type != QLatin1String("hex") && type != QLatin1String("rc4")) {
        return false;
    }
    const QString hex = session_args_key_hex_normalize(value);
    if (!QRegularExpression(QStringLiteral("^[0-9A-F]+$")).match(hex).hasMatch()) {
        return false;
    }
    const auto size = hex.size();
    return type == QLatin1String("rc4") ? size >= 1 && size <= 16 : size == 10 || size == 32 || size == 64;
}

QString
session_args_error_text(SessionArgsError error) {
    switch (error) {
        case SessionArgsError::None: return {};
        case SessionArgsError::Frequency: return QStringLiteral("Enter a positive frequency in MHz.");
        case SessionArgsError::Ppm: return QStringLiteral("Enter a whole number for PPM.");
        case SessionArgsError::KeyType: return QStringLiteral("Choose one encryption key type.");
        case SessionArgsError::KeyBasic: return QStringLiteral("Enter a basic key from 0 to 255.");
        case SessionArgsError::KeyHex: return QStringLiteral("Enter 10, 32, or 64 hexadecimal digits.");
        case SessionArgsError::KeyRc4: return QStringLiteral("Enter 1 to 16 hexadecimal digits.");
        case SessionArgsError::KeyScrambler: return QStringLiteral("Enter a scrambler key from 0 to 32767.");
        case SessionArgsError::KeyConflict: return QStringLiteral("Choose either a direct key or a key CSV file.");
        case SessionArgsError::ForceKey: return QStringLiteral("Choose force key mode 0, 1, or 2.");
        case SessionArgsError::UnsafeOption:
            return QStringLiteral("Extra options contain a prohibited option. Remove it before starting.");
    }
    return {};
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

    const QString keyType = system.value(QStringLiteral("encKeyType")).toString();
    const QString keyValue = system.value(QStringLiteral("encKeyValue")).toString();
    const QStringList keyTypes{QStringLiteral("basic"), QStringLiteral("hex"), QStringLiteral("rc4"),
                               QStringLiteral("scrambler")};
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
    append_key_args(args, keyIndex, keyValue, force);
    append_flag_args(args, system, prefs);
    return args;
}

SessionArgsBuilder::SessionArgsBuilder(const AppPrefs* prefs, QObject* parent) : QObject(parent), m_prefs(prefs) {}

SessionArgsBuilder::~SessionArgsBuilder() = default;

void
SessionArgsBuilder::setSavedSystems(const SavedSystemsModel* systems) {
    m_systems = systems;
}

QVariantMap
SessionArgsBuilder::build(const QVariantMap& system) const {
    SessionArgPrefs prefs;
    if (m_prefs != nullptr) {
        prefs.gainDb = m_prefs->gainDb();
        prefs.ppm = m_prefs->ppm();
        prefs.bandwidthKhz = m_prefs->bandwidthKhz();
        prefs.biasTee = m_prefs->biasTee();
        prefs.skipEncrypted = m_prefs->skipEncrypted();
        prefs.autoPpm = m_prefs->autoPpm();
        prefs.extraArgs = m_prefs->extraArgs();
    }
    SessionArgsError error = SessionArgsError::None;
    QVariantMap input = system;
    if (m_systems && !input.contains(QStringLiteral("encKeyValue"))) {
        input.insert(QStringLiteral("encKeyValue"),
                     m_systems->keyValueForUid(input.value(QStringLiteral("uid")).toString()));
    }
    const QStringList args = session_args_build(input, prefs, &error);
    QVariantMap result;
    result.insert(QStringLiteral("ok"), error == SessionArgsError::None);
    result.insert(QStringLiteral("args"), args);
    result.insert(QStringLiteral("error"), error == SessionArgsError::Frequency      ? QStringLiteral("frequency")
                                           : error == SessionArgsError::Ppm          ? QStringLiteral("ppm")
                                           : error == SessionArgsError::UnsafeOption ? QStringLiteral("unsafe-option")
                                           : error == SessionArgsError::None         ? QString()
                                                                                     : QStringLiteral("encryption"));
    result.insert(QStringLiteral("errorText"), session_args_error_text(error));
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
    const QStringList types{QStringLiteral("basic"), QStringLiteral("hex"), QStringLiteral("rc4"),
                            QStringLiteral("scrambler")};
    const QVariantMap fields{{QStringLiteral("keyCsvPath"), csvPath}, {QStringLiteral("encForceKey"), force}};
    return session_args_error_text(validate_key_args(fields, type, value, types.indexOf(type)));
}

} // namespace dsd_qt
