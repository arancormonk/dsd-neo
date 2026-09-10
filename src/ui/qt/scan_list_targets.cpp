// SPDX-License-Identifier: GPL-3.0-or-later
#include <QChar>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>
#include "scan_list_targets.h"
#include "session_args.h"

namespace dsd_qt {
namespace {
bool
safePath(const QString& path) {
    return !path.contains(QRegularExpression(QStringLiteral("[,\"\\r\\n]"))) && !path.contains(QChar(0));
}

bool
timing(const QVariant& value) {
    bool ok = false;
    const int ms = value.toString().toInt(&ok);
    return ok && (ms == 0 || (ms >= 250 && ms <= 600000));
}

bool
validGain(const QVariant& value) {
    bool ok = false;
    const int gain = value.toString().toInt(&ok);
    return ok && gain >= -1 && gain <= 49;
}

struct Target {
    QString decode, type, freq, hzText, id, modulation;
    bool trunk = false;
};

QString
resolveSystem(const QVariantMap& entry, const QVariantList& systems, QVariantMap& sys) {
    if (entry.value("kind") == "system") {
        const auto found = std::find_if(systems.cbegin(), systems.cend(), [&entry](const QVariant& candidate) {
            return candidate.toMap().value("uid") == entry.value("systemUid");
        });
        if (found != systems.cend()) {
            sys = found->toMap();
        }
        if (sys.isEmpty()) {
            return QStringLiteral("A saved system in this list no longer exists.");
        }
    } else if (entry.value("kind") == "freq") {
        sys = entry;
        const QMap<QString, QString> protocols{{"p25", "-ft"}, {"dmr", "-fs"}, {"nxdn48", "-fi"}, {"nxdn", "-fn"}};
        sys["decodeFlag"] = protocols.value(entry.value("protocol").toString());
        sys["trunking"] = false;
    } else {
        return QStringLiteral("Choose a saved system or a frequency entry.");
    }
    return {};
}

QString
validateIdentity(const QVariantMap& sys, const QVariantMap& entry, QSet<QString>& seen, QSet<QString>& ids,
                 Target& target) {
    target.decode = sys.value("decodeFlag").toString().simplified();
    const QMap<QString, QString> types{{"-ft", "p25"},    {"-f1", "p25"},    {"-mq", "p25"},   {"-^", "p25"},
                                       {"-fs", "dmr"},    {"-fi", "nxdn48"}, {"-fn", "nxdn"},  {"-ft -^", "p25"},
                                       {"-mq -^", "p25"}, {"-^ -ft", "p25"}, {"-^ -mq", "p25"}};
    if (!types.contains(target.decode)) {
        return QStringLiteral("This decode mode cannot be used in a scan list.");
    }
    if (!sys.value("extraArgs").toString().trimmed().isEmpty()) {
        return QStringLiteral("Remove extra options; they cannot be scoped safely.");
    }
    target.trunk = sys.value("trunking").toBool();
    target.type =
        types.value(target.decode) + (target.trunk ? QStringLiteral("-trunk") : QStringLiteral("-conventional"));
    target.freq = sys.value("freqMhz").toString().trimmed();
    const double hz = target.freq.toDouble() * 1000000.0;
    if (!session_args_freq_valid(target.freq) || !std::isfinite(hz) || hz < 1 || hz > 4294967295.0) {
        return QStringLiteral("Enter a valid frequency in MHz.");
    }
    target.hzText = QString::number(static_cast<quint64>(std::llround(hz)));
    if (seen.contains(target.type + target.hzText)) {
        return QStringLiteral("Duplicate target type and frequency.");
    }
    seen.insert(target.type + target.hzText);
    target.id = entry.value("uid").toString();
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,63}$")).match(target.id).hasMatch()
        || ids.contains(target.id)) {
        return QStringLiteral("Entry IDs must be unique letters, digits, underscores or hyphens (1..63).");
    }
    ids.insert(target.id);
    return {};
}

QString
collectPaths(const QVariantMap& sys, const Target& target, ScanListTargets& out) {
    const QString chan = sys.value("chanCsvPath").toString();
    const QString band = sys.value("p25BandplanCsvPath").toString();
    if (!chan.isEmpty() && !target.trunk) {
        return QStringLiteral("remove the channel map or use a trunked type");
    }
    if (!band.isEmpty() && target.type != "p25-trunk") {
        return QStringLiteral("A P25 band plan requires a P25 trunked type.");
    }
    for (const auto& field : {"chanCsvPath", "groupCsvPath", "keyCsvPath", "p25BandplanCsvPath", "keysHexCsvPath",
                              "keysDecCsvPath", "dmrTgKeyCsvPath"}) {
        const QString path = sys.value(field).toString();
        if (!safePath(path)) {
            return QStringLiteral("CSV paths cannot contain comma, quote, CR or LF.");
        }
        if (!path.isEmpty()) {
            out.paths << path;
        }
    }
    return {};
}

void
appendProfileOptions(const QVariantMap& sys, QStringList& options) {
    for (const auto& file : {qMakePair(QStringLiteral("keysHexCsvPath"), QStringLiteral("-K")),
                             qMakePair(QStringLiteral("keysDecCsvPath"), QStringLiteral("-k")),
                             qMakePair(QStringLiteral("dmrTgKeyCsvPath"), QStringLiteral("--dmr-tg-key-csv"))}) {
        if (!sys.value(file.first).toString().isEmpty()) {
            options << file.second << ('"' + sys.value(file.first).toString() + '"');
        }
    }
    if (sys.value("dmrTgKeyClear").toBool() && sys.value("decodeFlag").toString() == "-fs") {
        options << "--dmr-tg-key-clear";
    }
    if (sys.value("decryptionClearKeys").toBool()) {
        options << "--no-decryption-keys";
    }
    if (sys.contains("decryptionForce")) {
        const int value = sys.value("decryptionForce").toInt();
        if (value == 0) {
            options << "--no-force-key";
        } else if (value == 1) {
            options << "-4";
        } else if (value > 1) {
            options << "--dmr-force-algid" << QString::number(value, 16);
        }
    }
    if (!sys.value("decryptionProfileRef").toString().isEmpty()) {
        options << "--key-profile-ref" << sys.value("decryptionProfileRef").toString();
    }
}

QString
validateDirectKeyChoice(const QVariantMap& sys, const QString& keyType, const QString& key) {
    const QString keyCsv = sys.value("keyCsvPath").toString() + sys.value("keysHexCsvPath").toString()
                           + sys.value("keysDecCsvPath").toString();
    if ((!keyType.isEmpty() || !key.isEmpty()) && !keyCsv.isEmpty()) {
        return session_args_error_text(SessionArgsError::KeyConflict);
    }
    if (!session_args_key_valid(keyType, key)) {
        return QStringLiteral("Invalid direct key shape for the selected type.");
    }
    return {};
}

QString
buildOptions(const QVariantMap& sys, QStringList& options) {
    if (sys.value("decodeFlag").toString().simplified().split(QLatin1Char(' ')).contains(QStringLiteral("-^"))) {
        options << QStringLiteral("-^");
    }
    if (!session_args_profile_compatible(sys)) {
        return QStringLiteral("The decryption profile protocol does not match this scan entry.");
    }
    if (sys.value("decryptionSelectionMode").toString() == "vendor") {
        return QStringLiteral(
            "Vendor keystream profiles require a standalone session; scan restoration is unavailable.");
    }
    const QString keyType = sys.value("encKeyType").toString();
    const QString key = sys.value("encKeyValue").toString();
    QString keyError = validateDirectKeyChoice(sys, keyType, key);
    if (!keyError.isEmpty()) {
        return keyError;
    }
    bool forceOk = false;
    const int force = sys.value("encForceKey", 0).toInt(&forceOk);
    if (!forceOk || force < 0 || force > 2) {
        return session_args_error_text(SessionArgsError::ForceKey);
    }
    QString group = sys.value("groupCsvPath").toString();
    if (!group.isEmpty()) {
        options << QStringLiteral("-G") << (QStringLiteral("\"") + group + QStringLiteral("\""));
    }
    if (!keyType.isEmpty()) {
        const QMap<QString, QString> flags{{"basic", "-b"}, {"hex", "-H"}, {"rc4", "-1"}, {"scrambler", "-R"}};
        if (!flags.contains(keyType)) {
            return QStringLiteral("This key type cannot be scoped in a scan list.");
        }
        options << flags.value(keyType)
                << ((keyType == "hex" || keyType == "rc4") ? session_args_key_hex_normalize(key) : key.trimmed());
    }
    appendProfileOptions(sys, options);
    if (force) {
        options << (force == 1 ? QStringLiteral("-4") : QStringLiteral("-0"));
    }
    return {};
}

QString
validateTuning(const QVariantMap& entry, const QVariantMap& sys, Target& target) {
    for (const auto& field : {"dwellMs", "holdMs"}) {
        if (!timing(entry.value(field, 0))) {
            return QStringLiteral("Dwell and hold must be 250..600000 ms, or 0 to inherit.");
        }
    }
    target.modulation = entry.value("modulation").toString();
    if (target.modulation.isEmpty() && target.decode.split(QLatin1Char(' ')).contains(QStringLiteral("-mq"))) {
        target.modulation = QStringLiteral("cqpsk");
    }
    if (!target.modulation.isEmpty() && !QStringList{"c4fm", "cqpsk", "gfsk"}.contains(target.modulation)) {
        return QStringLiteral("Choose c4fm, cqpsk or gfsk modulation.");
    }
    if (!validGain(entry.value("gainDb", -1)) || !validGain(sys.value("gainDb", -1))) {
        return QStringLiteral("Gain must be 0..49 dB, or -1 to inherit.");
    }
    return {};
}

QStringList
targetCells(const QVariantMap& entry, const QVariantMap& sys, const Target& target, const QStringList& options) {
    const int gain =
        entry.value("gainDb", -1).toInt() >= 0 ? entry.value("gainDb").toInt() : sys.value("gainDb", -1).toInt();
    const int dwell = entry.value("dwellMs", 0).toInt();
    const int hold = entry.value("holdMs", 0).toInt();
    const QStringList cells{target.id,
                            target.type,
                            target.hzText,
                            sys.value("chanCsvPath").toString(),
                            dwell ? QString::number(dwell) : QString(),
                            hold ? QString::number(hold) : QString(),
                            QString(),
                            sys.value("keyCsvHex").toBool() ? sys.value("keyCsvPath").toString() : QString(),
                            sys.value("keyCsvHex").toBool() ? QString() : sys.value("keyCsvPath").toString(),
                            sys.value("p25BandplanCsvPath").toString(),
                            target.modulation,
                            gain >= 0 ? QString::number(gain) : QString(),
                            options.join(QLatin1Char(' '))};
    return cells;
}

QString
appendTarget(const QVariantMap& entry, const QVariantList& systems, const QVariantMap& list, QSet<QString>& seen,
             QSet<QString>& ids, QString& csv, ScanListTargets& out) {
    QVariantMap sys;
    QString error = resolveSystem(entry, systems, sys);
    if (!error.isEmpty()) {
        return error;
    }
    const auto decryption = entry.value("decryptionConfiguration").toMap();
    for (auto i = decryption.cbegin(); i != decryption.cend(); ++i) {
        sys.insert(i.key(), i.value());
    }
    Target target;
    QStringList options;
    error = validateIdentity(sys, entry, seen, ids, target);
    if (error.isEmpty()) {
        error = collectPaths(sys, target, out);
    }
    if (error.isEmpty()) {
        error = buildOptions(sys, options);
    }
    if (error.isEmpty()) {
        error = validateTuning(entry, sys, target);
    }
    if (!error.isEmpty()) {
        return sys.value("name").toString() + QStringLiteral(": ") + error;
    }
    const QString src = sys.value("srcCsvPath").toString();
    if (!src.isEmpty() && src != list.value("srcCsvPath").toString()) {
        out.warnings << sys.value("name").toString()
                            + QStringLiteral(": the list's source aliases replace this system's source aliases.");
    }
    const auto cells = targetCells(entry, sys, target, options);
    // The parser strips outer CSV quotes but does not unescape doubled
    // quotes. Keep the scoped path quotes directly in the options cell;
    // commas/newlines in paths have already been refused.
    csv += cells.join(QLatin1Char(',')) + QLatin1Char('\n');
    if (!out.targetCount) {
        out.firstFreqMhz = target.freq;
    }
    ++out.targetCount;
    return {};
}
} // namespace

ScanListTargets
scan_list_targets(const QVariantMap& list, const QVariantList& systems) {
    ScanListTargets out;
    const auto fail = [&out](const QString& reason) {
        out.error = reason;
        out.ok = false;
        return std::move(out);
    };
    if (list.value("sourceType") != "usb" && list.value("sourceType") != "rtltcp") {
        return fail(QStringLiteral("Choose USB or RTL-TCP for the scan list."));
    }
    for (const auto& field : {"defaultDwellMs", "defaultHoldMs"}) {
        if (!timing(list.value(field, 0))) {
            return fail(QStringLiteral("Dwell and hold must be 250..600000 ms, or 0 to inherit."));
        }
    }
    for (const auto& field : {"groupCsvPath", "srcCsvPath"}) {
        const QString path = list.value(field).toString();
        if (!safePath(path)) {
            return fail(QStringLiteral("CSV paths cannot contain comma, quote, CR or LF."));
        }
        if (!path.isEmpty()) {
            out.paths << path;
        }
    }
    QString csv = QStringLiteral("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv,keys_dec_"
                                 "csv,p25_bandplan_csv,modulation,rtl_gain,options\n");
    QSet<QString> seen;
    QSet<QString> ids;
    for (const auto& value : list.value("entries").toList()) {
        const auto entry = value.toMap();
        if (!entry.value("enabled", true).toBool()) {
            continue;
        }
        const QString error = appendTarget(entry, systems, list, seen, ids, csv, out);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }
    if (!out.targetCount) {
        return fail(QStringLiteral("Enable at least one scan-list entry."));
    }
    // Convert only once: QByteArray growth would free intermediate key-bearing
    // buffers without erasure. QString/QML storage has the documented limitation.
    out.csv = csv.toUtf8();
    out.paths.removeDuplicates();
    out.ok = true;
    return out;
}
} // namespace dsd_qt
