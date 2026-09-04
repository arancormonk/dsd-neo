// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "session_args.h"

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

} // namespace

bool
session_args_freq_valid(const QString& freqMhz) {
    bool ok = false;
    const double mhz = freqMhz.trimmed().toDouble(&ok);
    return ok && mhz > 0.0;
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
    append_flag_args(args, system, prefs);
    return args;
}

SessionArgsBuilder::SessionArgsBuilder(const AppPrefs* prefs, QObject* parent) : QObject(parent), m_prefs(prefs) {}

SessionArgsBuilder::~SessionArgsBuilder() = default;

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
    const QStringList args = session_args_build(system, prefs, &error);
    QVariantMap result;
    result.insert(QStringLiteral("ok"), error == SessionArgsError::None);
    result.insert(QStringLiteral("args"), args);
    result.insert(QStringLiteral("error"), error == SessionArgsError::Frequency ? QStringLiteral("frequency")
                                           : error == SessionArgsError::Ppm     ? QStringLiteral("ppm")
                                                                                : QString());
    return result;
}

bool
// cppcheck-suppress functionStatic // Q_INVOKABLE: QML calls this on the sessionArgs context object.
SessionArgsBuilder::freqValid(const QString& freqMhz) const {
    return session_args_freq_valid(freqMhz);
}

} // namespace dsd_qt
