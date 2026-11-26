// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The protocols a Qt scan list scans (issue #890): one catalog for every place that names, converts or measures
 * a scan-list protocol.
 *
 * Each protocol has its frequency-entry id ("p25", "tetra_dmo", ...), the decode flag a saved system selects it with
 * (compared exactly: -fm is dPMR and -fM is AM, -ft is P25 and -fT is TETRA), the engine's target type prefix
 * ("tetra-dmo" for tetra-dmo-conventional), the scan class a channel map's mode column names, the label the editor
 * shows, its channel width, and what a target of it takes: a trunked type, a modulation choice, and which keys
 * (keys()). The scan list's editor (ScanListScreen.qml, ScanEntryRow.qml) reads the same list through
 * ScanListsModel::protocols, in picker order: P25, DMR, NXDN96, NXDN48, dPMR, D-STAR, YSF, M17, TETRA DMO, then NFM and
 * AM.
 *
 * The engine's CSV type names themselves belong to the engine (trunk_scan.c) and to app-control's document inspection
 * (trunk_scan_document.cpp), which reads them without Qt; this catalog spells the same names.
 */

#ifndef DSD_NEO_SRC_UI_QT_SCAN_PROTOCOL_CATALOG_H_
#define DSD_NEO_SRC_UI_QT_SCAN_PROTOCOL_CATALOG_H_

#include <QLatin1String>
#include <QList> // IWYU pragma: keep -- QVariantList is a QList; IWYU asks for it in some units only.
#include <QMap>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace dsd_qt {

/** Which keys a scan target of a protocol takes in its own row (issue #890 key matrix). */
enum class ScanProtocolKeys {
    /** Profiles, direct keys, key CSVs and force options (P25, DMR, NXDN). */
    Full,
    /** Only a scoped -R scrambler key (dPMR); a legacy NXDN-scrambler system keeps working. */
    Scrambler,
    /** None of its own: the session's keys apply (M17). A target that carries some is refused. */
    Session,
    /** None at all: the protocol decrypts nothing (YSF, D-STAR, TETRA DMO, NFM, AM). */
    None,
};

/**
 * The key and force switches a scan target's row can carry, as the engine's scoped options spell them
 * (src/runtime/scan_options.c), one bit each: what ScanProtocol::keySwitches says a protocol's row takes.
 */
enum ScanKeySwitch : unsigned {
    ScanKeyBasic = 1U << 0,         /**< -b: a Motorola Basic Privacy key. */
    ScanKeyHex = 1U << 1,           /**< -H: a Hytera / AES key. */
    ScanKeyRc4 = 1U << 2,           /**< -1: an RC4 / DES key. */
    ScanKeyScrambler = 1U << 3,     /**< -R: a scrambler key. */
    ScanKeyHexFile = 1U << 4,       /**< -K, or the keys_hex_csv column: a hex key file. */
    ScanKeyDecFile = 1U << 5,       /**< -k, or the keys_dec_csv column: a decimal key file. */
    ScanKeyDmrMap = 1U << 6,        /**< --dmr-tg-key-csv: a DMR talkgroup key map. */
    ScanKeyDmrMapClear = 1U << 7,   /**< --dmr-tg-key-clear: no DMR talkgroup key map. */
    ScanKeyClear = 1U << 8,         /**< --no-decryption-keys: no keys. */
    ScanKeyProfileRef = 1U << 9,    /**< --key-profile-ref: the decryption profile's reference. */
    ScanKeyForcePrivacy = 1U << 10, /**< -4: privacy forcing. */
    ScanKeyForceBasic = 1U << 11,   /**< -0: the Basic Privacy algorithm fallback. */
    ScanKeyForceAlgid = 1U << 12,   /**< --dmr-force-algid: an algorithm fallback. */
    ScanKeyNoForce = 1U << 13,      /**< --no-force-key: no forcing. */
    ScanKeyGroup = 1U << 14,        /**< -G: the talkgroup file. */
    /** Past the last switch: a new one goes before it, so kScanKeyAllSwitches takes it in. */
    ScanKeySwitchEnd = 1U << 15,
};

/** Every ScanKeySwitch bit. */
constexpr unsigned kScanKeyAllSwitches = ScanKeySwitchEnd - 1U;

/* The engine's class masks for those switches (scan_options.c, with the issue #890 key matrix): P25 takes the hex,
   RC4 and file keys; DMR every DMR switch; NXDN48 and NXDN96 the hex, RC4, scrambler and file keys and privacy
   forcing; dPMR its scrambler key and privacy forcing; every keyed class the no-keys, profile-reference and no-forcing
   switches; every digital class the talkgroup file; the analog classes nothing. */
constexpr unsigned kScanKeysEveryKeyed = ScanKeyClear | ScanKeyProfileRef | ScanKeyNoForce | ScanKeyGroup;
constexpr unsigned kScanKeysP25 = ScanKeyHex | ScanKeyRc4 | ScanKeyHexFile | ScanKeyDecFile | kScanKeysEveryKeyed;
constexpr unsigned kScanKeysDmr = ScanKeyBasic | ScanKeyHex | ScanKeyRc4 | ScanKeyHexFile | ScanKeyDecFile
                                  | ScanKeyDmrMap | ScanKeyDmrMapClear | ScanKeyForcePrivacy | ScanKeyForceBasic
                                  | ScanKeyForceAlgid | kScanKeysEveryKeyed;
constexpr unsigned kScanKeysNxdn = ScanKeyHex | ScanKeyRc4 | ScanKeyScrambler | ScanKeyHexFile | ScanKeyDecFile
                                   | ScanKeyForcePrivacy | kScanKeysEveryKeyed;
constexpr unsigned kScanKeysDpmr = ScanKeyScrambler | ScanKeyForcePrivacy | kScanKeysEveryKeyed;

/** One scan-list protocol. Every string is ASCII. */
struct ScanProtocol {
    /** The frequency-entry id a scan list stores ("p25"). */
    const char* id;
    /** What the editor calls it ("P25", "YSF (C4FM)"). */
    const char* label;
    /** The decode flag a saved system selects it with, and a frequency entry runs ("-ft"). */
    const char* decodeFlag;
    /** The engine's target type prefix: <prefix>-conventional, and <prefix>-trunk where trunking is true. */
    const char* typePrefix;
    /** The scan class a channel map's mode column names it by (dsd_scan_mode_name()). */
    const char* scanClass;
    /** The decryption profile protocol its profile selector offers (DecryptionProfilesModel), empty for none. */
    const char* profileProtocol;
    /** Its channel width in Hz: the 6.25, 12.5 or 25 kHz channel the receiver runs, AM's 6 kHz default. */
    int widthHz;
    /** Whether it has a trunked type: else conventional only. */
    bool trunking;
    /** Whether a target of it takes a modulation (c4fm, cqpsk or gfsk): else its class runs its own. */
    bool modulation;
    /** NFM or AM: carrier squelch, no frames (issue #526). */
    bool analog;
    ScanProtocolKeys keys;
    /** The key and force switches its row takes (ScanKeySwitch bits), the engine's class masks. */
    unsigned keySwitches;
};

/* NXDN48, dPMR and D-STAR run the receiver's 6.25 kHz channel; TETRA a 25 kHz carrier (EN 300 392-2 4.5.1, EN 300
   396-2 4.5 and 6.2); every other digital mode a 12.5 kHz one. */
inline const std::array<ScanProtocol, 11>&
scan_protocols() {
    static constexpr std::array<ScanProtocol, 11> catalog{{
        {"p25", "P25", "-ft", "p25", "p25", "p25", 12500, true, true, false, ScanProtocolKeys::Full, kScanKeysP25},
        {"dmr", "DMR", "-fs", "dmr", "dmr", "dmr", 12500, true, true, false, ScanProtocolKeys::Full, kScanKeysDmr},
        {"nxdn", "NXDN96", "-fn", "nxdn", "nxdn96", "nxdn", 12500, true, true, false, ScanProtocolKeys::Full,
         kScanKeysNxdn},
        {"nxdn48", "NXDN48", "-fi", "nxdn48", "nxdn48", "nxdn", 6250, true, true, false, ScanProtocolKeys::Full,
         kScanKeysNxdn},
        {"dpmr", "dPMR", "-fm", "dpmr", "dpmr", "dpmr", 6250, false, false, false, ScanProtocolKeys::Scrambler,
         kScanKeysDpmr},
        {"dstar", "D-STAR", "-fd", "dstar", "dstar", "", 6250, false, false, false, ScanProtocolKeys::None,
         ScanKeyGroup},
        {"ysf", "YSF (C4FM)", "-fy", "ysf", "ysf", "", 12500, false, false, false, ScanProtocolKeys::None,
         ScanKeyGroup},
        {"m17", "M17", "-fz", "m17", "m17", "", 12500, false, false, false, ScanProtocolKeys::Session, ScanKeyGroup},
        {"tetra_dmo", "TETRA DMO", "-fT", "tetra-dmo", "tetra-dmo", "", 25000, false, false, false,
         ScanProtocolKeys::None, ScanKeyGroup},
        {"nfm", "NFM (analog)", "-fA", "nfm", "nfm", "", 12500, false, false, true, ScanProtocolKeys::None, 0U},
        {"am", "AM (analog)", "-fM", "am", "am", "", 6000, false, false, true, ScanProtocolKeys::None, 0U},
    }};
    return catalog;
}

/** The first catalog protocol @p matches takes, or nullptr. */
template <typename Match>
inline const ScanProtocol*
scan_protocol_find(Match matches) {
    const auto& catalog = scan_protocols();
    const auto found = std::find_if(catalog.cbegin(), catalog.cend(), matches);
    return found != catalog.cend() ? &*found : nullptr;
}

/** The protocol frequency-entry id @p id names, exactly, or nullptr. */
inline const ScanProtocol*
scan_protocol_by_id(const QString& id) {
    return scan_protocol_find([&id](const ScanProtocol& p) { return id == QLatin1String(p.id); });
}

/** The protocol a saved system's decode flags select, blanks collapsed and case significant, or nullptr: its own flag,
    or one of P25's spellings (-f1, -mq, -^, and the -^ pairs). */
inline const ScanProtocol*
scan_protocol_for_flag(const QString& decodeFlag) {
    const QString flag = decodeFlag.simplified();
    static const std::array<const char*, 7> p25{"-f1", "-mq", "-^", "-ft -^", "-mq -^", "-^ -ft", "-^ -mq"};
    if (std::any_of(p25.cbegin(), p25.cend(), [&flag](const char* alias) { return flag == QLatin1String(alias); })) {
        return &scan_protocols()[0];
    }
    return scan_protocol_find([&flag](const ScanProtocol& p) { return flag == QLatin1String(p.decodeFlag); });
}

/** The engine target type of @p protocol: "<prefix>-trunk" when @p trunk, else "<prefix>-conventional". */
inline QString
scan_protocol_type(const ScanProtocol& protocol, bool trunk) {
    return QLatin1String(protocol.typePrefix) + (trunk ? QLatin1String("-trunk") : QLatin1String("-conventional"));
}

/** The protocol engine target type @p type names ("tetra-dmo-conventional", "dmr-trunk"), without case, or nullptr: a
    trunked spelling only for a protocol that has one. Never split at a hyphen, since a prefix holds one. */
inline const ScanProtocol*
scan_protocol_for_type(const QString& type) {
    return scan_protocol_find([&type](const ScanProtocol& p) {
        return type.compare(scan_protocol_type(p, false), Qt::CaseInsensitive) == 0
               || (p.trunking && type.compare(scan_protocol_type(p, true), Qt::CaseInsensitive) == 0);
    });
}

/** The protocol @p mode names wherever a channel's mode is written, or nullptr: a decode flag, compared exactly; else,
    without case, an engine target type, a channel map's scan class ("nxdn96", "tetra-dmo"), a frequency-entry id, or
    "d-star". */
inline const ScanProtocol*
scan_protocol_for_mode(const QString& mode) {
    if (const ScanProtocol* flag = scan_protocol_for_flag(mode)) {
        return flag;
    }
    if (const ScanProtocol* type = scan_protocol_for_type(mode)) {
        return type;
    }
    if (const ScanProtocol* named = scan_protocol_find([&mode](const ScanProtocol& p) {
            return mode.compare(QLatin1String(p.scanClass), Qt::CaseInsensitive) == 0
                   || mode.compare(QLatin1String(p.id), Qt::CaseInsensitive) == 0;
        })) {
        return named;
    }
    return mode.compare(QLatin1String("d-star"), Qt::CaseInsensitive) == 0
               ? scan_protocol_by_id(QStringLiteral("dstar"))
               : nullptr;
}

/** @p keys as QML reads it: "full", "scrambler", "session" or "none". */
inline QString
scan_protocol_keys_name(ScanProtocolKeys keys) {
    switch (keys) {
        case ScanProtocolKeys::Full: return QStringLiteral("full");
        case ScanProtocolKeys::Scrambler: return QStringLiteral("scrambler");
        case ScanProtocolKeys::Session: return QStringLiteral("session");
        case ScanProtocolKeys::None: break;
    }
    return QStringLiteral("none");
}

/** @p p as QML reads it: {id, label, decodeFlag, type (its conventional type), trunking, modulation, analog, keys
    (scan_protocol_keys_name()), profileProtocol, widthHz}; an empty map for nullptr. */
inline QVariantMap
scan_protocol_variant(const ScanProtocol* p) {
    if (p == nullptr) {
        return {};
    }
    return QVariantMap{{QStringLiteral("id"), QString::fromLatin1(p->id)},
                       {QStringLiteral("label"), QString::fromLatin1(p->label)},
                       {QStringLiteral("decodeFlag"), QString::fromLatin1(p->decodeFlag)},
                       {QStringLiteral("type"), scan_protocol_type(*p, false)},
                       {QStringLiteral("trunking"), p->trunking},
                       {QStringLiteral("modulation"), p->modulation},
                       {QStringLiteral("analog"), p->analog},
                       {QStringLiteral("keys"), scan_protocol_keys_name(p->keys)},
                       {QStringLiteral("profileProtocol"), QString::fromLatin1(p->profileProtocol)},
                       {QStringLiteral("widthHz"), p->widthHz}};
}

/** The catalog as QML reads it (ScanListsModel::protocols), in picker order: scan_protocol_variant() of each. */
inline QVariantList
scan_protocols_variant() {
    const auto& catalog = scan_protocols();
    QVariantList out;
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        out.append(scan_protocol_variant(&catalog[i]));
    }
    return out;
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SCAN_PROTOCOL_CATALOG_H_ */
