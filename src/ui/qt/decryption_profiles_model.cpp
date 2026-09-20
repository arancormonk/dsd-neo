// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QHash>
#include <QIODevice>
#include <QJsonValue>
#include <Qt>
#include <QtGlobal>
#include <initializer_list>
#include <stdint.h>
#include <utility>
#include "decryption_profiles_model.h"
#include "json_store.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "session_args.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/dmr_key_map.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/dmr_keystream.h>

namespace dsd_qt {
namespace {
QString
uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool
validId(const QString& id) {
    return QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,63}$")).match(id).hasMatch();
}

QVariantMap
failure(const QString& text) {
    return {{"ok", false}, {"error", text}};
}

bool
privateWrite(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly)
           && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) && file.write(bytes) == bytes.size()
           && file.commit();
}

QString
protocolFor(const QString& flags) {
    if (flags.contains("-ft") || flags.contains("-f1") || flags.contains("-f2")) {
        return "p25";
    }
    if (flags.contains("-fs")) {
        return "dmr";
    }
    if (flags.contains("-fi") || flags.contains("-fn")) {
        return "nxdn";
    }
    if (flags.contains("-fz")) {
        return "m17";
    }
    if (flags.contains("-fp")) {
        return "dpmr";
    }
    return "mixed";
}

QVariantList
withoutMaterial(const QVariantList& entries) {
    QVariantList result;
    for (const auto& value : entries) {
        auto entry = value.toMap();
        entry.insert("configured", !entry.value("material").toString().isEmpty());
        entry.remove("material");
        result.append(entry);
    }
    return result;
}
} // namespace

DecryptionProfilesModel::DecryptionProfilesModel(QObject* parent) : QAbstractListModel(parent) {
    QSet<QString> ids;
    for (const auto& value : json_store_load_array(QStringLiteral("decryption_profiles.json"))) {
        const auto row = value.toObject().toVariantMap();
        const auto id = row.value("uid").toString();
        if (validId(id) && !ids.contains(id)) {
            m_rows.append(row);
            ids.insert(id);
        }
    }
}

int
DecryptionProfilesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QHash<int, QByteArray>
DecryptionProfilesModel::roleNames() const {
    return {{Qt::UserRole + 1, "uid"},
            {Qt::UserRole + 2, "label"},
            {Qt::UserRole + 3, "protocol"},
            {Qt::UserRole + 4, "mode"}};
}

QVariant
DecryptionProfilesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    return metadata(m_rows[index.row()]).value(QString::fromUtf8(roleNames().value(role)));
}

int
DecryptionProfilesModel::rowForUid(const QString& uid) const {
    for (int i = 0; i < count(); ++i) {
        if (m_rows[i].value("uid").toString() == uid) {
            return i;
        }
    }
    return -1;
}

void
DecryptionProfilesModel::setReferences(SavedSystemsModel* systems, ScanListsModel* scans) {
    m_systems = systems;
    m_scans = scans;
}

QString
DecryptionProfilesModel::newReference() const {
    QString reference;
    do {
        reference = uuid();
    } while (rowForUid(reference) >= 0);
    return reference;
}

QVariantMap
DecryptionProfilesModel::metadata(QVariantMap row) {
    row.insert("keyConfigured", !row.value("directValue").toString().isEmpty());
    row.remove("directValue");
    row.insert("vendorConfigured", !row.value("vendorValue").toString().isEmpty());
    row.remove("vendorValue");
    row.remove("compiled");
    row.remove("pastRuntimeRefs");
    row.insert("keys", withoutMaterial(row.value("keys").toList()));
    row.insert("keyCount", row.value("keySource").toString() == "csv" ? -1 : row.value("keys").toList().size());
    return row;
}

QVariantMap
DecryptionProfilesModel::get(const QString& uid) const {
    const int i = rowForUid(uid);
    if (i < 0) {
        return {};
    }
    auto result = metadata(m_rows[i]);
    result.insert("useCount", useCount(uid));
    return result;
}

QVariantMap
DecryptionProfilesModel::forRuntimeReference(const QString& reference) const {
    if (reference.isEmpty()) {
        return {};
    }
    for (const auto& row : m_rows) {
        const bool current = row.value("runtimeRef").toString() == reference;
        if (current || row.value("pastRuntimeRefs").toStringList().contains(reference)
            || row.value("uid").toString() == reference) {
            auto result = get(row.value("uid").toString());
            result.insert("outdated", !current);
            if (!current) {
                result.insert("keys", QVariantList());
                result.insert("keyCount", -1);
            }
            return result;
        }
    }
    return {};
}

void
DecryptionProfilesModel::retainForSession(const QString& uid) {
    if (rowForUid(uid) < 0 || m_sessionReferences.contains(uid)) {
        return;
    }
    m_sessionReferences.insert(uid);
    Q_EMIT countChanged();
}

void
DecryptionProfilesModel::releaseSessionReferences() {
    if (m_sessionReferences.isEmpty()) {
        return;
    }
    m_sessionReferences.clear();
    Q_EMIT countChanged();
}

QVariantList
DecryptionProfilesModel::entries() const {
    QVariantList result;
    for (const auto& row : m_rows) {
        result.append(get(row.value("uid").toString()));
    }
    return result;
}

QStringList
DecryptionProfilesModel::profilesReferencingPath(const QString& path) const {
    QStringList names;
    if (path.isEmpty()) {
        return names;
    }
    for (const auto& row : m_rows) {
        for (const auto* field : {"legacyCsvPath", "mappingCsvPath", "vendorCsvPath"}) {
            if (row.value(field).toString() == path) {
                names.append(row.value("label").toString());
                break;
            }
        }
    }
    return names;
}

int
DecryptionProfilesModel::useCount(const QString& uid) const {
    int uses = m_sessionReferences.contains(uid) ? 1 : 0;
    if (m_systems) {
        for (int i = 0; i < m_systems->count(); ++i) {
            uses += m_systems->get(i).value("decryptionProfileUid").toString() == uid;
        }
    }
    if (m_scans) {
        for (int i = 0; i < m_scans->count(); ++i) {
            const auto scanEntries = m_scans->get(i).value("entries").toList();
            uses +=
                static_cast<int>(std::count_if(scanEntries.cbegin(), scanEntries.cend(), [&uid](const QVariant& entry) {
                    return entry.toMap().value("decryptionProfileUid").toString() == uid;
                }));
        }
    }
    return uses;
}

QVariantList
DecryptionProfilesModel::directTypes(const QString& protocol) const {
    const auto type = [](const char* id, const char* label) { return QVariantMap{{"id", id}, {"label", label}}; };
    if (protocol == "m17") {
        return {type("m17Scrambler", "M17 scrambler (8/16/24 bit)"), type("m17Aes", "M17 AES (128/192/256 bit)")};
    }
    if (protocol == "dpmr") {
        return {type("scrambler", "dPMR scrambler")};
    }
    if (protocol == "dstar" || protocol == "ysf") {
        return {};
    }
    QVariantList result;
    if (protocol == "dmr" || protocol == "mixed") {
        result << type("basic", "Motorola Basic Privacy");
    }
    result << type("rc4", "RC4 / DES") << type("hex", "Hytera / AES");
    if (protocol == "nxdn" || protocol == "mixed") {
        result << type("scrambler", "NXDN / dPMR scrambler");
    }
    return result;
}

QVariantList
DecryptionProfilesModel::vendorTypes() const {
    return {QVariantMap{{"id", "tytBp"}, {"label", "TYT Basic · 16-bit hex · simplex only"}},
            QVariantMap{{"id", "tytPc4"}, {"label", "TYT PC4 · 32 or 64 hex digits"}},
            QVariantMap{{"id", "tytEp"}, {"label", "TYT Enhanced · 32 hex digits"}},
            QVariantMap{{"id", "retevis"}, {"label", "Retevis RC2 · 32 or 64 hex digits"}},
            QVariantMap{{"id", "baofeng"}, {"label", "Baofeng PC5 · 32 or 64 hex digits"}},
            QVariantMap{{"id", "csi"}, {"label", "Connect Systems EE72 · 18 hex digits"}},
            QVariantMap{{"id", "kenwood"}, {"label", "Kenwood · decimal 0–32767"}},
            QVariantMap{{"id", "anytone"}, {"label", "Anytone Basic · 16-bit hex"}},
            QVariantMap{{"id", "static"}, {"label", "Static keystream · bits:hex[:offset:step]"}},
            QVariantMap{{"id", "vertex"}, {"label", "Vertex · key to keystream CSV"}}};
}

static bool
vendorHex(const QString& type, QString& value) {
    if (!QRegularExpression("^[0-9A-F]+$").match(value).hasMatch()) {
        return false;
    }
    const int n = value.size();
    if (type == "tytBp" || type == "anytone") {
        if (n > 4) {
            return false;
        }
    } else if (type == "csi") {
        if (n != 18) {
            return false;
        }
    } else if (type == "tytEp") {
        if (n != 32) {
            return false;
        }
        value.insert(16, ' ');
    } else if (n != 32 && n != 64) {
        return false;
    }
    return true;
}

static bool
compileVendor(QVariantMap& row, QVariantMap& compiled) {
    const QString type = row.value("vendorType").toString();
    const QString raw = row.value("vendorValue").toString();
    QString value = session_args_key_hex_normalize(raw);
    const QMap<QString, QString> flags{{"tytBp", "-2"},
                                       {"tytPc4", "-!"},
                                       {"tytEp", "-5"},
                                       {"retevis", "-@"},
                                       {"baofeng", "--dmr-baofeng-pc5"},
                                       {"csi", "--dmr-csi-ee72"},
                                       {"kenwood", "-9"},
                                       {"anytone", "-A"},
                                       {"static", "-S"},
                                       {"vertex", "--dmr-vertex-ks-csv"}};
    if (!flags.contains(type)) {
        return false;
    }
    if (type == "vertex") {
        value = row.value("vendorCsvPath").toString();
        dsd_csv_validation counts = {};
        if (dsd_csv_validate_vertex_file(value.toUtf8().constData(), &counts) != 0 || !counts.accepted) {
            return false;
        }
    } else if (type == "static") {
        uint8_t bits[882] = {};
        int mod, frame, offset, step;
        const bool valid =
            raw.size() < 512
            && dmr_parse_static_keystream_spec(raw.toUtf8().constData(), bits, &mod, &frame, &offset, &step, nullptr, 0)
                   == 1;
        DSD_SECURE_ZERO(bits, sizeof(bits));
        if (!valid) {
            return false;
        }
        value = raw.trimmed();
    } else if (type == "kenwood") {
        value = raw.trimmed();
        if (!QRegularExpression("^[0-9]{1,5}$").match(value).hasMatch() || value.toUInt() > 32767) {
            return false;
        }
    } else {
        if (!vendorHex(type, value)) {
            return false;
        }
    }
    compiled.insert("decryptionVendorArgs", QStringList{flags.value(type), value});
    return true;
}

bool
DecryptionProfilesModel::persist(const QList<QVariantMap>& rows) const {
    QJsonArray array;
    for (const auto& row : rows) {
        array.append(QJsonObject::fromVariantMap(row));
    }
    return json_store_save_array(QStringLiteral("decryption_profiles.json"), array);
}

namespace {
bool
profileError(QString* error, const QString& message) {
    *error = message;
    return false;
}

bool
managedShape(const QString& kind, const QString& material) {
    if (!QRegularExpression("^[0-9A-F]+$").match(material).hasMatch()) {
        return false;
    }
    const QMap<QString, int> widths{{"aes128", 32}, {"tdea", 48}, {"aes256", 64}};
    return kind == "scalar" ? material.size() <= 16 : widths.value(kind, -1) == material.size();
}

bool
managedKindAllowed(const QString& kind, const QString& protocol, bool destination) {
    if (destination && protocol == "p25") {
        return false;
    }
    if (protocol == "mixed") {
        return true;
    }
    if (kind == "basic") {
        return protocol == "dmr";
    }
    if (kind == "scrambler") {
        return protocol == "nxdn";
    }
    if (kind == "tdea") {
        return protocol == "p25";
    }
    return kind != "aes128" || protocol != "nxdn";
}

QStringList
managedWords(const QVariantMap& key, QString* error) {
    const QString kind = key.value("kind", "scalar").toString();
    const QString raw = key.value("material").toString();
    if (kind == "basic" || kind == "scrambler") {
        if (!session_args_key_valid(kind, raw)) {
            profileError(error, QObject::tr("Invalid decimal key material."));
            return {};
        }
        return {QString::number(raw.toULongLong(), 16)};
    }
    const QString material = session_args_key_hex_normalize(raw);
    if (!managedShape(kind, material)) {
        profileError(error, QObject::tr("A key has an unsupported format or width."));
        return {};
    }
    QStringList words;
    for (int at = 0; at < material.size(); at += 16) {
        words << material.mid(at, 16);
    }
    return words;
}

struct ManagedCollection {
    QByteArray hex{"keyid,value0,value1,value2,value3\n"}, dec{"keyid,value\n"};
    bool haveHex = false, haveDec = false;
    QSet<quint32> occupied;
    QSet<QString> references;
};

bool
reserveManagedKey(ManagedCollection& collection, const QString& ref, quint32 base, int count, QString* error) {
    if (!validId(ref) || collection.references.contains(ref)) {
        return profileError(error, QObject::tr("Key references must be unique."));
    }
    collection.references.insert(ref);
    static const quint32 offsets[] = {0, 0x101, 0x201, 0x301};
    for (int i = 0; i < count; ++i) {
        if (collection.occupied.contains(base + offsets[i])) {
            return profileError(
                error,
                QObject::tr(
                    "These keys collide in the decoder keyring. Use separate profiles or one shared key entry."));
        }
        collection.occupied.insert(base + offsets[i]);
    }
    return true;
}

bool
managedIdValid(bool parsed, quint32 id, quint32 maximum, bool destination) {
    return parsed && id <= maximum && (!destination || id != 0);
}

bool
appendManagedKey(ManagedCollection& collection, const QVariantMap& key, const QString& protocol, QString* error) {
    bool ok = false;
    const quint32 id = key.value("keyId").toUInt(&ok);
    const bool destination = key.value("lookup").toString() == "destination";
    const quint32 maximum = destination ? 0xFFFFFFU : protocol == "dmr" ? 255U : 65535U;
    if (!managedIdValid(ok, id, maximum, destination)) {
        return profileError(error, QObject::tr("A key identifier is outside the protocol's supported range."));
    }
    if (!managedKindAllowed(key.value("kind", "scalar").toString(), protocol, destination)) {
        return profileError(error, QObject::tr("This key format or lookup is not supported by the profile protocol."));
    }
    const QStringList words = managedWords(key, error);
    if (words.isEmpty()) {
        return false;
    }
    if (destination && words.size() != 1) {
        return profileError(error, QObject::tr("Destination lookup supports scalar legacy keys only."));
    }
    const quint32 base = destination ? keyring_destination_index(id) : id;
    if (!reserveManagedKey(collection, key.value("uid").toString(), base, words.size(), error)) {
        return false;
    }
    if (destination) {
        const auto scalar = words[0].toULongLong(&ok, 16);
        if (!ok || scalar > 0xFFFFFFFFFFULL) {
            return profileError(error, QObject::tr("Legacy destination imports support at most 40-bit material."));
        }
        collection.dec += QByteArray::number(id) + ',' + QByteArray::number(scalar) + '\n';
        collection.haveDec = true;
    } else {
        collection.hex += QByteArray::number(id, 16) + ',' + words.join(',').toUtf8() + '\n';
        collection.haveHex = true;
    }
    return true;
}

struct ProfileCompiler {
    const QVariantMap& row;
    QString protocol, mode, prefix;
    QVariantMap compiled;
    QString* error;
    QStringList* staged;

    ProfileCompiler(const QVariantMap& input, QString* why, QStringList* files)
        : row(input), protocol(input.value("protocol", "mixed").toString()),
          mode(input.value("mode", "automatic").toString()),
          prefix(json_store_path("decryption/" + input.value("uid").toString() + "/" + uuid())), error(why),
          staged(files) {}

    bool
    fail(const QString& message) {
        return profileError(error, message);
    }

    bool
    store(const QString& suffix, const QByteArray& bytes, const QString& field) {
        const QString path = prefix + suffix;
        if (!privateWrite(path, bytes)) {
            return fail(QObject::tr("Could not store the key collection or overrides."));
        }
        staged->append(path);
        compiled.insert(field, path);
        return true;
    }

    bool
    direct(const QVariantList& types) {
        const QString type = row.value("directType").toString();
        const QString value = row.value("directValue").toString();
        const bool legacy = row.value("legacy").toBool();
        const bool allowed = std::any_of(types.cbegin(), types.cend(),
                                         [&type](const QVariant& item) { return item.toMap().value("id") == type; });
        if ((!allowed && !legacy) || !session_args_key_valid(type, value)) {
            return fail(QObject::tr("The direct key does not match this protocol and key format."));
        }
        if (!legacy && type == "hex") {
            const int width = session_args_key_hex_normalize(value).size();
            if (protocol == "p25" && width == 10) {
                return fail(QObject::tr("P25 AES requires 32 or 64 hex digits."));
            }
            if (protocol == "nxdn" && width != 64) {
                return fail(QObject::tr("NXDN AES requires 64 hex digits."));
            }
        }
        compiled.insert("encKeyType", type);
        return true;
    }

    bool
    automatic() {
        if (!row.value("legacy").toBool() && QStringList{"m17", "dpmr", "dstar", "ysf"}.contains(protocol)) {
            return fail(
                QObject::tr("This protocol uses a channel/stream profile rather than received-key-ID selection."));
        }
        const QString legacy = row.value("legacyCsvPath").toString();
        const bool csvSource = row.value("keySource", legacy.isEmpty() ? "managed" : "csv").toString() == "csv";
        if (csvSource) {
            if (legacy.isEmpty()) {
                return fail(QObject::tr("Select the attached key CSV."));
            }
            if (!QFileInfo(legacy).isFile() || !QFileInfo(legacy).isReadable()) {
                return fail(QObject::tr("The attached key CSV is missing. Reselect it in Imported files."));
            }
            compiled.insert(row.value("legacyCsvHex").toBool() ? "keysHexCsvPath" : "keysDecCsvPath", legacy);
            return true;
        }
        ManagedCollection collection;
        const auto keys = row.value("keys").toList();
        if (!std::all_of(keys.cbegin(), keys.cend(), [&](const QVariant& key) {
                return appendManagedKey(collection, key.toMap(), protocol, error);
            })) {
            return false;
        }
        if (collection.haveHex && !store(".hex.csv", collection.hex, "keysHexCsvPath")) {
            return false;
        }
        return !collection.haveDec || store(".dec.csv", collection.dec, "keysDecCsvPath");
    }

    quint32
    mappingKeyId(const QVariantMap& mapping, bool* ok) const {
        const QString ref = mapping.value("keyRef").toString();
        if (ref.isEmpty()) {
            return mapping.value("keyId").toUInt(ok);
        }
        for (const auto& value : row.value("keys").toList()) {
            const auto key = value.toMap();
            if (key.value("uid").toString() == ref) {
                return key.value("keyId").toUInt(ok);
            }
        }
        *ok = false;
        return 0;
    }

    bool
    managedMappings(const QVariantList& mappings) {
        if (mappings.size() > DSD_DMR_TG_KEY_MAP_MAX) {
            return fail(QObject::tr("A DMR profile supports up to 256 talkgroup overrides."));
        }
        QByteArray csv("tg_dec,keyid_hex\n");
        QSet<quint32> groups;
        for (const auto& value : mappings) {
            const auto mapping = value.toMap();
            bool tgOk = false, kidOk = false;
            const auto tg = mapping.value("talkgroup").toUInt(&tgOk);
            const auto kid = mappingKeyId(mapping, &kidOk);
            if (!tgOk || !kidOk || !tg || tg > 0xFFFFFFU || kid > 255U || groups.contains(tg)) {
                return fail(QObject::tr("Overrides need unique talkgroup IDs and a key ID from 00 to FF."));
            }
            groups.insert(tg);
            csv += QByteArray::number(tg) + ',' + QByteArray::number(kid, 16) + '\n';
        }
        return store(".map.csv", csv, "dmrTgKeyCsvPath");
    }

    bool
    mappings() {
        if (mode != "automatic") {
            return true;
        }
        const auto mappings = row.value("mappings").toList();
        const QString path = row.value("mappingCsvPath").toString();
        if (mappings.isEmpty() && path.isEmpty()) {
            return true;
        }
        if (protocol != "dmr") {
            return fail(QObject::tr("Talkgroup overrides require a DMR automatic key collection."));
        }
        if (!mappings.isEmpty() && !path.isEmpty()) {
            return fail(QObject::tr("Choose a mapping CSV or managed talkgroup overrides."));
        }
        if (!path.isEmpty()) {
            dsd_dmr_key_map map = {};
            if (dsd_dmr_key_map_load(path.toUtf8().constData(), &map)) {
                return fail(QObject::tr("The DMR mapping CSV is invalid or unavailable."));
            }
            compiled.insert("dmrTgKeyCsvPath", path);
        } else if (!managedMappings(mappings)) {
            return false;
        }
        compiled.insert("dmrTgKeyClear", false);
        return true;
    }
};
} // namespace

static bool
validProfileForce(int force) {
    return force >= -1 && force <= 255 && (force <= 1 || force >= 32);
}

bool
DecryptionProfilesModel::compile(QVariantMap& row, QString* error, QStringList* staged) const {
    ProfileCompiler builder(row, error, staged);
    const auto& protocol = builder.protocol;
    const auto& mode = builder.mode;
    const int force = row.value("force", 0).toInt();
    if (!QStringList{"p25", "dmr", "nxdn", "dpmr", "m17", "mixed", "dstar", "ysf"}.contains(protocol)) {
        *error = tr("Choose a supported profile protocol.");
        return false;
    }
    if (!validProfileForce(force)) {
        *error = tr("Invalid force selection.");
        return false;
    }
    auto& compiled = builder.compiled;
    compiled = {{"encKeyType", ""},        {"encKeyValue", ""},
                {"encForceKey", 0},        {"keyCsvPath", ""},
                {"keysHexCsvPath", ""},    {"keysDecCsvPath", ""},
                {"dmrTgKeyCsvPath", ""},   {"dmrTgKeyClear", protocol == "dmr"},
                {"decryptionForce", force}};
    bool valid = mode == "none";
    if (mode == "vendor") {
        valid = (protocol == "dmr" || protocol == "mixed") && compileVendor(row, compiled);
    }
    if (mode == "direct") {
        valid = builder.direct(directTypes(protocol));
    }
    if (mode == "automatic") {
        valid = builder.automatic();
    }
    if (!valid) {
        if (error->isEmpty()) {
            *error = tr("Choose a supported selection mode and enter its exact key format.");
        }
        return false;
    }
    if (!builder.mappings()) {
        return false;
    }
    compiled.insert("decryptionClearKeys",
                    mode == "none"
                        || (mode == "automatic" && compiled.value("keysHexCsvPath").toString().isEmpty()
                            && compiled.value("keysDecCsvPath").toString().isEmpty()));
    compiled.insert("decryptionProfileRef", row.value("runtimeRef"));
    compiled.insert("decryptionSelectionMode", mode);
    compiled.insert("decryptionProtocol", protocol);
    compiled.insert("decryptionLegacy", row.value("legacy").toBool());
    row.insert("compiled", compiled);
    row.insert("schemaVersion", 1);
    return true;
}

static QString
mergeProfileDraft(QVariantMap& row, const QVariantMap& draft, bool existing) {
    const auto previous = row;
    if (existing && draft.contains("directType") && draft.value("directType") != row.value("directType")
        && !draft.contains("directValue")) {
        return QObject::tr("Enter replacement material when changing the direct key type.");
    }
    for (const auto* field :
         {"label", "protocol", "mode", "force", "directType", "directValue", "legacyCsvPath", "legacyCsvHex",
          "mappingCsvPath", "mappings", "legacy", "keySource", "vendorType", "vendorValue", "vendorCsvPath"}) {
        if (draft.contains(field)) {
            row.insert(field, draft.value(field));
        }
    }
    if (existing && draft.contains("vendorType") && draft.value("vendorType") != previous.value("vendorType")
        && !draft.contains("vendorValue") && draft.value("vendorType") != "vertex") {
        return QObject::tr("Enter replacement material when changing the vendor mode.");
    }
    if (row.value("label").toString().trimmed().isEmpty()) {
        return QObject::tr("Name the decryption profile.");
    }
    return {};
}

static void
mergeProfileKeys(QVariantMap& row, const QVariantMap& draft) {
    if (draft.contains("keys")) {
        QMap<QString, QString> previous;
        for (const auto& value : row.value("keys").toList()) {
            const auto key = value.toMap();
            previous.insert(key.value("uid").toString(), key.value("material").toString());
        }
        QVariantList keys;
        for (const auto& value : draft.value("keys").toList()) {
            auto key = value.toMap();
            if (key.value("uid").toString().isEmpty()) {
                key.insert("uid", uuid());
            }
            if (!key.contains("material")) {
                key.insert("material", previous.value(key.value("uid").toString()));
            }
            keys.append(key);
        }
        row.insert("keys", keys);
    }
}

QVariantMap
DecryptionProfilesModel::saveProfile(const QVariantMap& draft) {
    const QString uid = draft.value("uid").toString().isEmpty() ? uuid() : draft.value("uid").toString();
    if (!validId(uid)) {
        return failure(tr("Invalid profile reference."));
    }
    const int at = rowForUid(uid);
    QVariantMap row =
        at >= 0 ? m_rows[at] : QVariantMap{{"uid", uid}, {"protocol", "mixed"}, {"mode", "automatic"}, {"force", 0}};
    const QString draftError = mergeProfileDraft(row, draft, at >= 0);
    if (!draftError.isEmpty()) {
        return failure(draftError);
    }
    mergeProfileKeys(row, draft);
    QString error;
    QStringList past = row.value("pastRuntimeRefs").toStringList();
    if (!row.value("runtimeRef").toString().isEmpty()) {
        past.append(row.value("runtimeRef").toString());
    }
    row.insert("pastRuntimeRefs", past);
    row.insert("runtimeRef", uuid());
    QStringList staged;
    if (!compile(row, &error, &staged)) {
        for (const auto& path : staged) {
            QFile::remove(path);
        }
        return failure(error);
    }
    auto next = m_rows;
    if (at < 0) {
        next.append(row);
    } else {
        next[at] = row;
    }
    if (!persist(next)) {
        for (const auto& path : staged) {
            QFile::remove(path);
        }
        return failure(tr("Could not save the profile. The previous configuration is unchanged."));
    }
    if (at < 0) {
        beginInsertRows({}, count(), count());
    }
    m_rows = std::move(next);
    if (at < 0) {
        endInsertRows();
    } else {
        Q_EMIT dataChanged(index(at), index(at));
    }
    Q_EMIT countChanged();
    return {{"ok", true}, {"uid", uid}, {"error", ""}};
}

QVariantMap
DecryptionProfilesModel::configuration(const QString& uid, QString* error) const {
    if (error) {
        error->clear();
    }
    const int row = rowForUid(uid);
    if (row < 0) {
        if (error) {
            *error = tr("The decryption profile is missing. Select another profile.");
        }
        return {};
    }
    auto result = m_rows[row].value("compiled").toMap();
    if (m_rows[row].value("mode").toString() == "direct") {
        result.insert("encKeyValue", m_rows[row].value("directValue"));
    }
    for (const auto* key : {"keysHexCsvPath", "keysDecCsvPath", "dmrTgKeyCsvPath"}) {
        const QString path = result.value(key).toString();
        if (!path.isEmpty() && (!QFileInfo(path).isFile() || !QFileInfo(path).isReadable())) {
            if (error) {
                *error = tr("A profile file is missing. Edit the profile and reselect its key source.");
            }
            return {};
        }
    }
    return result;
}

QVariantMap
DecryptionProfilesModel::removeProfile(const QString& uid) {
    const int at = rowForUid(uid);
    if (at < 0) {
        return failure(tr("This profile was already removed."));
    }
    if (useCount(uid)) {
        return failure(tr("Reassign the systems and scan entries using this profile before removing it."));
    }
    auto next = m_rows;
    next.removeAt(at);
    if (!persist(next)) {
        return failure(tr("Could not remove the profile."));
    }
    beginRemoveRows({}, at, at);
    m_rows = std::move(next);
    endRemoveRows();
    QDir(json_store_path("decryption/" + uid)).removeRecursively();
    Q_EMIT countChanged();
    return {{"ok", true}};
}

bool
DecryptionProfilesModel::migrateLegacySystems() {
    if (!m_systems) {
        return true;
    }
    for (int i = 0; i < m_systems->count(); ++i) {
        const auto system = m_systems->get(i);
        if (!system.value("decryptionProfileUid").toString().isEmpty()) {
            continue;
        }
        const QString type = system.value("encKeyType").toString();
        const QString csv = system.value("keyCsvPath").toString();
        if (type.isEmpty() && csv.isEmpty()) {
            continue;
        }
        const QString uid = system.value("uid").toString();
        if (rowForUid(uid) < 0) {
            const int oldForce = system.value("encForceKey").toInt();
            const auto result = saveProfile({{"uid", uid},
                                             {"label", system.value("name")},
                                             {"protocol", protocolFor(system.value("decodeFlag").toString())},
                                             {"legacy", true},
                                             {"mode", type.isEmpty() ? "automatic" : "direct"},
                                             {"directType", type},
                                             {"directValue", m_systems->keyValueForUid(uid)},
                                             {"keySource", "csv"},
                                             {"legacyCsvPath", csv},
                                             {"legacyCsvHex", system.value("keyCsvHex")},
                                             {"force", oldForce == 2   ? 0x21
                                                       : oldForce == 1 ? 1
                                                                       : -1}});
            if (!result.value("ok").toBool()) {
                return false;
            }
        }
        if (!m_systems->update(i, {{"decryptionProfileUid", uid}, {"encKeyValue", ""}})) {
            return false;
        }
    }
    return true;
}
} // namespace dsd_qt
