// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <Qt>
#include <algorithm>
#include <array>
#include <cmath>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <utility>
#include "radio_settings.h"
#include "scan_entry_name.h"
#include "scan_list_targets.h"
#include "scan_protocol_catalog.h"
#include "session_args.h"
#include "stored_int.h"

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
    /* The catalog protocol its decode flags select (scan_protocol_for_flag()): its type, label, modulation and keys. */
    const ScanProtocol* protocol = nullptr;
    bool trunk = false;
    /* An nfm or am entry (issue #526): carrier squelch only, so no keys, talkgroups or modulation. */
    bool analog = false;
    /* A saved analog system's own channel width (scan_list_saved_system_width_hz()), 0 for none. */
    int widthHz = 0;
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
        const ScanProtocol* protocol = scan_protocol_by_id(entry.value("protocol").toString());
        sys["decodeFlag"] = protocol ? QString::fromLatin1(protocol->decodeFlag) : QString();
        sys["trunking"] = false;
    } else {
        return QStringLiteral("Choose a saved system or a frequency entry.");
    }
    return {};
}

/* The analog demodulator exactly -fA (NFM) or -fM (AM) runs, else -1: the only analog decode flags a scan list maps. */
int
analogKindForFlag(const QString& decodeFlag) {
    const QString flag = decodeFlag.simplified();
    if (flag == QStringLiteral("-fA")) {
        return DSD_ANALOG_DEMOD_FM;
    }
    return flag == QStringLiteral("-fM") ? DSD_ANALOG_DEMOD_AM : -1;
}

/* Whether a manual list can carry saved system @p sys's Extra arguments: none, or (issue #890) exactly the width option
   of its analog kind @p kind (-1 for a digital system), as a Signal search save keeps its measured width. That width
   becomes @p target's own. */
bool
carriesExtraArgs(const QVariantMap& sys, int kind, Target& target) {
    const QString extra = sys.value("extraArgs").toString();
    if (extra.trimmed().isEmpty()) {
        return true;
    }
    target.widthHz = kind >= 0 ? scan_list_saved_system_width_hz(extra, kind) : 0;
    return target.widthHz > 0;
}

QString
validateIdentity(const QVariantMap& sys, const QVariantMap& entry, QSet<QString>& seen, QSet<QString>& ids,
                 Target& target) {
    target.decode = sys.value("decodeFlag").toString().simplified();
    target.protocol = scan_protocol_for_flag(target.decode);
    if (target.protocol == nullptr) {
        return QStringLiteral("This decode mode cannot be used in a scan list.");
    }
    const int kind = analogKindForFlag(target.decode);
    target.analog = target.protocol->analog;
    if (!carriesExtraArgs(sys, kind, target)) {
        return QStringLiteral("This saved system has standalone Extra arguments. Remove them to use it in a manual "
                              "list, or import a target CSV with supported switches in its options column.");
    }
    if (!radio_settings_valid(sys.value("radioSettings"))) {
        return QStringLiteral("This saved system has invalid Radio settings.");
    }
    target.trunk = sys.value("trunking").toBool();
    if (target.analog && target.trunk) {
        return QStringLiteral("Analog entries are conventional only; turn trunking off for this saved system.");
    }
    /* Issue #890: YSF, D-STAR, dPMR, M17 and TETRA DMO have no trunked target type. */
    if (target.trunk && !target.protocol->trunking) {
        return QStringLiteral("%1 entries are conventional only; turn trunking off for this saved system.")
            .arg(QLatin1String(target.protocol->label));
    }
    target.type = scan_protocol_type(*target.protocol, target.trunk);
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
    if (!chan.isEmpty() && target.analog) {
        /* Analog entries are conventional only, so no trunked type would take the map either. */
        return QStringLiteral("Analog entries take no channel map; remove it from this saved system.");
    }
    if (!chan.isEmpty() && !target.protocol->trunking) {
        return QStringLiteral("%1 entries take no channel map; remove it from this saved system.")
            .arg(QLatin1String(target.protocol->label));
    }
    if (!chan.isEmpty() && !target.trunk) {
        return QStringLiteral("remove the channel map or use a trunked type");
    }
    if (!band.isEmpty() && target.type != "p25-trunk") {
        return QStringLiteral("A P25 band plan requires a P25 trunked type.");
    }
    /* An analog entry has neither map (both refused above) and passes no key, talkgroup or key-map file
       (appendTarget()), so none of its saved paths reaches the list: none has to exist or fit a CSV cell. */
    if (target.analog) {
        return {};
    }
    /* Issue #890: a protocol that decrypts nothing (YSF, D-STAR, TETRA DMO) passes no key or key-map file
       (buildOptions(), targetCells()), so none of those paths reaches the list either. */
    QStringList fields{QStringLiteral("chanCsvPath"), QStringLiteral("groupCsvPath"),
                       QStringLiteral("p25BandplanCsvPath")};
    if (target.protocol->keys != ScanProtocolKeys::None) {
        fields << QStringLiteral("keyCsvPath") << QStringLiteral("keysHexCsvPath") << QStringLiteral("keysDecCsvPath")
               << QStringLiteral("dmrTgKeyCsvPath");
    }
    for (const QString& field : fields) {
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
    int value = 0;
    if (stored_int(sys.value("decryptionForce"), &value)) {
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

/* Whether saved system @p sys (its decryption profile merged in) carries any key setting a row would pass: a direct
   key, a key or key-map file, a profile, a force choice, or the clear-keys choice. */
bool
carriesKeys(const QVariantMap& sys) {
    static const std::array<const char*, 8> fields{
        "encKeyType",     "encKeyValue",     "keyCsvPath",           "keysHexCsvPath",
        "keysDecCsvPath", "dmrTgKeyCsvPath", "decryptionProfileRef", "decryptionSelectionMode"};
    if (std::any_of(fields.cbegin(), fields.cend(),
                    [&sys](const char* field) { return !sys.value(field).toString().isEmpty(); })) {
        return true;
    }
    int force = 0;
    return sys.value("decryptionClearKeys").toBool() || (stored_int(sys.value("encForceKey", 0), &force) && force != 0)
           || (sys.contains("decryptionForce") && stored_int(sys.value("decryptionForce"), &force) && force != 0);
}

/* Whether @p sys forces an algorithm, which only a DMR row takes: the direct key's force choice 2 (-0), or its
   profile's algorithm fallback (decryptionForce above 1, --dmr-force-algid). A value past int's range adds no option
   (appendProfileOptions()), so it forces nothing here either. */
bool
forcesAlgorithm(const QVariantMap& sys) {
    int force = 0;
    return (stored_int(sys.value("encForceKey", 0), &force) && force == 2)
           || (sys.contains("decryptionForce") && stored_int(sys.value("decryptionForce"), &force) && force > 1);
}

/* The refusal for a row that would force a DMR algorithm (forcesAlgorithm()): only privacy forcing (-4) and no forcing
   (--no-force-key) reach it. */
QString
algorithmForceError(const ScanProtocol& protocol) {
    return QStringLiteral("%1 entries take privacy forcing only, not an algorithm fallback. Change the Force key "
                          "setting of this saved system or its decryption profile.")
        .arg(QLatin1String(protocol.label));
}

/* dPMR's row (ScanProtocolKeys::Scrambler) takes only its scoped -R scrambler key, of the direct or a profile's key, so
   a legacy system saved with the NXDN / dPMR scrambler keeps working; never a key or key-map file, nor an algorithm
   fallback. */
QString
scramblerRowError(const QVariantMap& sys, const ScanProtocol& protocol) {
    const QString keyType = sys.value("encKeyType").toString();
    if (!keyType.isEmpty() && keyType != QLatin1String("scrambler")) {
        return QStringLiteral("%1 entries take only a scrambler key.").arg(QLatin1String(protocol.label));
    }
    for (const auto& field : {"keyCsvPath", "keysHexCsvPath", "keysDecCsvPath", "dmrTgKeyCsvPath"}) {
        if (!sys.value(field).toString().isEmpty()) {
            return QStringLiteral("%1 entries take only a scrambler key, not a key file.")
                .arg(QLatin1String(protocol.label));
        }
    }
    return forcesAlgorithm(sys) ? algorithmForceError(protocol) : QString();
}

/* An NXDN48 or NXDN96 row takes what the engine's scoped options give its class (scan_options.c): -H, -1, -R, the key
   files -K and -k, -G, --no-decryption-keys, --key-profile-ref, -4 and --no-force-key. Basic Privacy (-b), a DMR
   talkgroup key map (--dmr-tg-key-csv) and an algorithm fallback (-0, --dmr-force-algid) are DMR's only, so a saved
   NXDN system carrying one would make the engine refuse the whole list: it is refused here, by name, first. */
QString
nxdnRowError(const QVariantMap& sys) {
    if (sys.value("encKeyType").toString() == QLatin1String("basic")) {
        return QStringLiteral("NXDN entries take no Motorola Basic Privacy key, which is DMR's. Choose another key "
                              "type for this saved system or its decryption profile.");
    }
    if (!sys.value("dmrTgKeyCsvPath").toString().isEmpty()) {
        return QStringLiteral("NXDN entries take no DMR talkgroup key map. Remove it from this saved system's "
                              "decryption profile.");
    }
    return forcesAlgorithm(sys) ? QStringLiteral("NXDN entries take privacy forcing only, not an algorithm fallback. "
                                                 "Change the Force key setting of this saved system or its "
                                                 "decryption profile.")
                                : QString();
}

/* Issue #890: why @p protocol's row cannot take the keys @p sys carries (its key matrix, ScanProtocolKeys, and the
   switches the engine's scoped options give its class), or empty. M17 takes none of its own: the session's keys
   apply. */
QString
keyMatrixError(const QVariantMap& sys, const ScanProtocol& protocol) {
    if (protocol.keys == ScanProtocolKeys::Session && carriesKeys(sys)) {
        return QStringLiteral("%1 entries use the session's keys, never their own. Remove the key or decryption "
                              "profile from this saved system, or set the key in the app's decoder options.")
            .arg(QLatin1String(protocol.label));
    }
    if (protocol.keys == ScanProtocolKeys::Scrambler) {
        return scramblerRowError(sys, protocol);
    }
    return QLatin1String(protocol.profileProtocol) == QLatin1String("nxdn") ? nxdnRowError(sys) : QString();
}

/* The saved system's talkgroup file as a scoped -G option, quoted (its path holds no comma, quote or line break). */
void
appendGroupOption(const QVariantMap& sys, QStringList& options) {
    const QString group = sys.value("groupCsvPath").toString();
    if (!group.isEmpty()) {
        options << QStringLiteral("-G") << (QStringLiteral("\"") + group + QStringLiteral("\""));
    }
}

/* The key options of a row whose protocol takes keys (P25, DMR, NXDN, and dPMR's scrambler, whose other material
   keyMatrixError() refused): its profile and direct key checked, then -G, the direct key, the profile's options and the
   force choice. */
QString
buildKeyOptions(const QVariantMap& sys, QStringList& options) {
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
    int force = 0;
    const bool forceOk = stored_int(sys.value("encForceKey", 0), &force);
    if (!forceOk || force < 0 || force > 2) {
        return session_args_error_text(SessionArgsError::ForceKey);
    }
    appendGroupOption(sys, options);
    if (!keyType.isEmpty()) {
        const QMap<QString, QString> flags{{"basic", "-b"}, {"hex", "-H"}, {"rc4", "-1"}, {"scrambler", "-R"}};
        if (!flags.contains(keyType)) {
            return QStringLiteral("This key type cannot be scoped in a scan list.");
        }
        options << flags.value(keyType) << session_args_key_value_normalize(keyType, key);
    }
    appendProfileOptions(sys, options);
    if (force) {
        options << (force == 1 ? QStringLiteral("-4") : QStringLiteral("-0"));
    }
    return {};
}

QString
buildOptions(const QVariantMap& sys, const ScanProtocol& protocol, QStringList& options) {
    if (sys.value("decodeFlag").toString().simplified().split(QLatin1Char(' ')).contains(QStringLiteral("-^"))) {
        options << QStringLiteral("-^");
    }
    QString matrixError = keyMatrixError(sys, protocol);
    if (!matrixError.isEmpty()) {
        return matrixError;
    }
    /* A protocol with no keys of its own (YSF, D-STAR, TETRA DMO, M17) passes only the talkgroup file: whatever key
       setting a saved system still holds has nothing to act on there (M17's were refused above). */
    if (protocol.keys == ScanProtocolKeys::None || protocol.keys == ScanProtocolKeys::Session) {
        appendGroupOption(sys, options);
        return {};
    }
    return buildKeyOptions(sys, options);
}

/* The modulation @p target's row writes: the entry's own, else the saved system's Radio settings', else CQPSK for the
   P25 Simulcast preset (-mq). An analog channel has no digital modulation: the editor hides the choice, so one left
   from another protocol is not the entry's; issue #890: nor has a protocol whose class runs its own (dPMR, D-STAR, YSF,
   M17, TETRA DMO), whose system's saved modulation is refused. */
QString
resolveModulation(const QVariantMap& entry, const QVariantMap& sys, Target& target) {
    const bool offered = target.protocol->modulation;
    target.modulation = offered ? entry.value("modulation").toString() : QString();
    const auto radio = sys.value("radioSettings").toMap();
    if (!radio.contains("modulation")) {
        return {};
    }
    if (!offered) {
        return target.analog ? QString()
                             : QStringLiteral("%1 entries take no modulation: clear the modulation saved in this "
                                              "system's Radio settings.")
                                   .arg(QLatin1String(target.protocol->label));
    }
    if (target.modulation.isEmpty()) {
        target.modulation = QStringList{"c4fm", "cqpsk", "gfsk"}.at(radio.value("modulation").toInt());
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
    QString modulationError = resolveModulation(entry, sys, target);
    if (!modulationError.isEmpty()) {
        return modulationError;
    }
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

/* Issue #782: the name a person knows the target by, in the CSV's `label` column, which names the channel being heard
   (a call with no name of its own is named for it, and so are its history row and recording); the id is the entry's
   internal UID, which never shows. The entry's name as the scan list shows it: the saved system's or frequency entry's
   name, else its protocol and frequency ("P25 · 851.5 MHz"), never empty. The monitor's scan header shows the same
   name, from the engine's publication. A quote inside is written as an apostrophe and a line break as a space, so the
   cell stays one plain quoted cell; the engine sanitizes and truncates the rest. */
QString
targetLabel(const QVariantMap& sys, const Target& target) {
    QString label = scan_entry_name(sys.value("name").toString(), QLatin1String(target.protocol->id), target.freq);
    label.replace(QLatin1Char('"'), QLatin1Char('\''));
    label.replace(QLatin1Char('\r'), QLatin1Char(' '));
    label.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return QLatin1Char('"') + label + QLatin1Char('"');
}

QStringList
targetCells(const QVariantMap& entry, const QVariantMap& sys, const Target& target, const QStringList& options) {
    const int gain =
        entry.value("gainDb", -1).toInt() >= 0 ? entry.value("gainDb").toInt() : sys.value("gainDb", -1).toInt();
    const int dwell = entry.value("dwellMs", 0).toInt();
    const int hold = entry.value("holdMs", 0).toInt();
    /* The legacy key file columns follow the key matrix: only a protocol with full keys takes one (dPMR's and M17's are
       refused, buildOptions()). */
    const QString keyCsv =
        target.protocol->keys == ScanProtocolKeys::Full ? sys.value("keyCsvPath").toString() : QString();
    const QStringList cells{target.id,
                            target.type,
                            target.hzText,
                            sys.value("chanCsvPath").toString(),
                            dwell ? QString::number(dwell) : QString(),
                            hold ? QString::number(hold) : QString(),
                            QString(),
                            sys.value("keyCsvHex").toBool() ? keyCsv : QString(),
                            sys.value("keyCsvHex").toBool() ? QString() : keyCsv,
                            sys.value("p25BandplanCsvPath").toString(),
                            target.modulation,
                            gain >= 0 ? QString::number(gain) : QString(),
                            targetLabel(sys, target),
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
    /* An analog channel carries no keys, talkgroups or encryption (issue #526): the editor hides decryption for it, and
       a saved analog system's key and group settings have nothing to act on, so its options stay empty. */
    if (error.isEmpty() && !target.analog) {
        error = buildOptions(sys, *target.protocol, options);
    }
    if (error.isEmpty()) {
        error = validateTuning(entry, sys, target);
    }
    if (!error.isEmpty()) {
        return sys.value("name").toString() + QStringLiteral(": ") + error;
    }
    if (target.widthHz > 0) {
        options << scan_list_width_option_name(analogKindForFlag(target.decode)) << QString::number(target.widthHz);
    }
    const auto radio = sys.value("radioSettings").toMap();
    if (radio.contains("squelch")) {
        options << QStringLiteral("--squelch") << radio.value("squelch").toString();
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

QString
scan_list_settings_error(const QVariantMap& list) {
    if (list.value("sourceType") != "usb" && list.value("sourceType") != "airspy"
        && list.value("sourceType") != "rtltcp") {
        return QStringLiteral("Choose RTL USB, Airspy or RTL-TCP for the scan list.");
    }
    for (const auto& field : {"defaultDwellMs", "defaultHoldMs"}) {
        if (!timing(list.value(field, 0))) {
            return QStringLiteral("Dwell and hold must be 250..600000 ms, or 0 to inherit.");
        }
    }
    for (const auto& field : {"groupCsvPath", "srcCsvPath"}) {
        const QString path = list.value(field).toString();
        if (!safePath(path)) {
            return QStringLiteral("CSV paths cannot contain comma, quote, CR or LF.");
        }
    }
    return {};
}

QString
scan_list_width_option_name(int kind) {
    return kind == DSD_ANALOG_DEMOD_AM ? QStringLiteral("--am-bandwidth-hz") : QStringLiteral("--nfm-bandwidth-hz");
}

QList<ScanListWidthOption>
scan_list_width_options(const QString& extraArgs, int kind, qsizetype* tokenCount) {
    static const QRegularExpression wholeHz(QStringLiteral("^[0-9]{1,9}$"));
    const QString option = scan_list_width_option_name(kind);
    const QStringList tokens = extraArgs.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QList<ScanListWidthOption> found;
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        QString value;
        qsizetype span = 1;
        if (tokens.at(i) == option) {
            span = i + 1 < tokens.size() ? 2 : 1;
            value = span == 2 ? tokens.at(i + 1) : QString();
        } else if (tokens.at(i).startsWith(option + QLatin1Char('='))) {
            value = tokens.at(i).mid(option.size() + 1);
        } else {
            continue;
        }
        found.append({wholeHz.match(value).hasMatch() ? value.toInt() : 0, span});
    }
    if (tokenCount != nullptr) {
        *tokenCount = tokens.size();
    }
    return found;
}

int
scan_list_saved_system_width_hz(const QString& extraArgs, int kind) {
    if (kind != DSD_ANALOG_DEMOD_FM && kind != DSD_ANALOG_DEMOD_AM) {
        return 0;
    }
    /* Exactly one option, which is every token: a quoted value is no width here either, as in a standalone session. */
    qsizetype tokenCount = 0;
    const QList<ScanListWidthOption> found = scan_list_width_options(extraArgs, kind, &tokenCount);
    if (found.size() != 1 || found.front().tokens != tokenCount || found.front().hz <= 0) {
        return 0;
    }
    /* The bounds are the target options parser's own, which reads the row this width is written to. */
    const QByteArray text =
        (scan_list_width_option_name(kind) + QLatin1Char(' ') + QString::number(found.front().hz)).toUtf8();
    const unsigned int mode = kind == DSD_ANALOG_DEMOD_AM ? DSD_SCAN_MODE_AM : DSD_SCAN_MODE_NFM;
    dsd_scan_options parsed;
    DSD_MEMSET(&parsed, 0, sizeof parsed);
    char error[192];
    const int rc = dsd_scan_options_parse(text.constData(), mode, 1, &parsed, error, sizeof error);
    const int hz = rc == 0 && parsed.values.present == DSD_SCAN_OPT_BANDWIDTH && parsed.values.channel_bw_kind == kind
                       ? parsed.values.channel_bw_hz
                       : 0;
    return hz > 0 ? hz : 0;
}

int
scan_list_entry_analog_kind(const QVariantMap& entry, const QString& systemDecodeFlag) {
    if (entry.value("kind") == "freq") {
        const QString protocol = entry.value("protocol").toString();
        if (protocol == QStringLiteral("nfm")) {
            return DSD_ANALOG_DEMOD_FM;
        }
        return protocol == QStringLiteral("am") ? DSD_ANALOG_DEMOD_AM : -1;
    }
    return entry.value("kind") == "system" ? analogKindForFlag(systemDecodeFlag) : -1;
}

const ScanProtocol*
scan_list_entry_protocol(const QVariantMap& entry, const QString& systemDecodeFlag) {
    if (entry.value("kind") == "freq") {
        return scan_protocol_by_id(entry.value("protocol").toString());
    }
    return entry.value("kind") == "system" ? scan_protocol_for_flag(systemDecodeFlag) : nullptr;
}

bool
scan_list_entry_takes_keys(const QVariantMap& entry, const QString& systemDecodeFlag) {
    const ScanProtocol* protocol = scan_list_entry_protocol(entry, systemDecodeFlag);
    return protocol == nullptr || protocol->keys == ScanProtocolKeys::Full
           || protocol->keys == ScanProtocolKeys::Scrambler;
}

ScanListTargets
scan_list_targets(const QVariantMap& list, const QVariantList& systems) {
    ScanListTargets out;
    const auto fail = [&out](const QString& reason) {
        out.error = reason;
        out.ok = false;
        return std::move(out);
    };
    const auto settingsError = scan_list_settings_error(list);
    if (!settingsError.isEmpty()) {
        return fail(settingsError);
    }
    for (const auto& field : {"groupCsvPath", "srcCsvPath"}) {
        const QString path = list.value(field).toString();
        if (!path.isEmpty()) {
            out.paths << path;
        }
    }
    QString csv = QStringLiteral("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv,keys_dec_"
                                 "csv,p25_bandplan_csv,modulation,rtl_gain,label,options\n");
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
