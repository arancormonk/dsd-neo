// SPDX-License-Identifier: GPL-3.0-or-later
#include "scan_list_targets.h"
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <utility>
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
        QVariantMap sys;
        if (entry.value("kind") == "system") {
            for (const auto& candidate : systems) {
                if (candidate.toMap().value("uid") == entry.value("systemUid")) {
                    sys = candidate.toMap();
                    break;
                }
            }
            if (sys.isEmpty()) {
                return fail(QStringLiteral("A saved system in this list no longer exists."));
            }
        } else if (entry.value("kind") == "freq") {
            sys = entry;
            const QMap<QString, QString> protocols{{"p25", "-ft"}, {"dmr", "-fs"}, {"nxdn48", "-fi"}, {"nxdn", "-fn"}};
            sys["decodeFlag"] = protocols.value(entry.value("protocol").toString());
            sys["trunking"] = false;
        } else {
            return fail(QStringLiteral("Choose a saved system or a frequency entry."));
        }
        const QString name = sys.value("name").toString();
        const auto problem = [&fail, &name](const QString& text) { return fail(name + QStringLiteral(": ") + text); };
        const QString decode = sys.value("decodeFlag").toString().trimmed();
        const QMap<QString, QString> types{{"-ft", "p25"}, {"-f1", "p25"},    {"-mq", "p25"}, {"-^", "p25"},
                                           {"-fs", "dmr"}, {"-fi", "nxdn48"}, {"-fn", "nxdn"}};
        if (!types.contains(decode)) {
            return problem(QStringLiteral("This decode mode cannot be used in a scan list."));
        }
        if (!sys.value("extraArgs").toString().trimmed().isEmpty()) {
            return problem(QStringLiteral("Remove extra options; they cannot be scoped safely."));
        }
        const bool trunk = sys.value("trunking").toBool();
        const QString type = types.value(decode) + (trunk ? QStringLiteral("-trunk") : QStringLiteral("-conventional"));
        const QString freq = sys.value("freqMhz").toString().trimmed();
        const double hz = freq.toDouble() * 1000000.0;
        if (!session_args_freq_valid(freq) || !std::isfinite(hz) || hz < 1 || hz > 4294967295.0) {
            return problem(QStringLiteral("Enter a valid frequency in MHz."));
        }
        const QString hzText = QString::number(static_cast<quint64>(std::llround(hz)));
        if (seen.contains(type + hzText)) {
            return problem(QStringLiteral("Duplicate target type and frequency."));
        }
        seen.insert(type + hzText);
        const QString id = entry.value("uid").toString();
        if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,63}$")).match(id).hasMatch() || ids.contains(id)) {
            return problem(QStringLiteral("Entry IDs must be unique letters, digits, underscores or hyphens (1..63)."));
        }
        ids.insert(id);
        const QString chan = sys.value("chanCsvPath").toString();
        const QString band = sys.value("p25BandplanCsvPath").toString();
        if (!chan.isEmpty() && !trunk) {
            return problem(QStringLiteral("remove the channel map or use a trunked type"));
        }
        if (!band.isEmpty() && type != "p25-trunk") {
            return problem(QStringLiteral("A P25 band plan requires a P25 trunked type."));
        }
        for (const auto& field : {"chanCsvPath", "groupCsvPath", "srcCsvPath", "keyCsvPath", "p25BandplanCsvPath"}) {
            const QString path = sys.value(field).toString();
            if (!safePath(path)) {
                return problem(QStringLiteral("CSV paths cannot contain comma, quote, CR or LF."));
            }
            if (!path.isEmpty()) {
                out.paths << path;
            }
        }
        const QString src = sys.value("srcCsvPath").toString();
        if (!src.isEmpty() && src != list.value("srcCsvPath").toString()) {
            out.warnings << name + QStringLiteral(": the list's source aliases replace this system's source aliases.");
        }
        const QString keyType = sys.value("encKeyType").toString();
        const QString key = sys.value("encKeyValue").toString();
        const QString keyCsv = sys.value("keyCsvPath").toString();
        if ((!keyType.isEmpty() || !key.isEmpty()) && !keyCsv.isEmpty()) {
            return problem(session_args_error_text(SessionArgsError::KeyConflict));
        }
        if (!session_args_key_valid(keyType, key)) {
            return problem(QStringLiteral("Invalid direct key shape for the selected type."));
        }
        bool forceOk = false;
        const int force = sys.value("encForceKey", 0).toInt(&forceOk);
        if (!forceOk || force < 0 || force > 2) {
            return problem(session_args_error_text(SessionArgsError::ForceKey));
        }
        QStringList options;
        QString group = sys.value("groupCsvPath").toString();
        if (!group.isEmpty()) {
            options << QStringLiteral("-G") << (QStringLiteral("\"") + group + QStringLiteral("\""));
        }
        if (!keyType.isEmpty()) {
            const QMap<QString, QString> flags{{"basic", "-b"}, {"hex", "-H"}, {"rc4", "-1"}, {"scrambler", "-R"}};
            options << flags.value(keyType)
                    << ((keyType == "hex" || keyType == "rc4") ? session_args_key_hex_normalize(key) : key.trimmed());
        }
        if (force) {
            options << (force == 1 ? QStringLiteral("-4") : QStringLiteral("-0"));
        }
        for (const auto& field : {"dwellMs", "holdMs"}) {
            if (!timing(entry.value(field, 0))) {
                return problem(QStringLiteral("Dwell and hold must be 250..600000 ms, or 0 to inherit."));
            }
        }
        QString modulation = entry.value("modulation").toString();
        if (modulation.isEmpty() && decode == "-mq") {
            modulation = QStringLiteral("cqpsk");
        }
        if (!modulation.isEmpty() && !QStringList{"c4fm", "cqpsk", "gfsk"}.contains(modulation)) {
            return problem(QStringLiteral("Choose c4fm, cqpsk or gfsk modulation."));
        }
        if (!validGain(entry.value("gainDb", -1)) || !validGain(sys.value("gainDb", -1))) {
            return problem(QStringLiteral("Gain must be 0..49 dB, or -1 to inherit."));
        }
        const int gain =
            entry.value("gainDb", -1).toInt() >= 0 ? entry.value("gainDb").toInt() : sys.value("gainDb", -1).toInt();
        const int dwell = entry.value("dwellMs", 0).toInt();
        const int hold = entry.value("holdMs", 0).toInt();
        const QStringList cells{id,
                                type,
                                hzText,
                                chan,
                                dwell ? QString::number(dwell) : QString(),
                                hold ? QString::number(hold) : QString(),
                                QString(),
                                sys.value("keyCsvHex").toBool() ? keyCsv : QString(),
                                sys.value("keyCsvHex").toBool() ? QString() : keyCsv,
                                band,
                                modulation,
                                gain >= 0 ? QString::number(gain) : QString(),
                                options.join(QLatin1Char(' '))};
        // The parser strips outer CSV quotes but does not unescape doubled
        // quotes. Keep the scoped path quotes directly in the options cell;
        // commas/newlines in paths have already been refused.
        csv += cells.join(QLatin1Char(',')) + QLatin1Char('\n');
        if (!out.targetCount) {
            out.firstFreqMhz = freq;
        }
        ++out.targetCount;
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
