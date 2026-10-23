// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QHash>
#include <QIODevice>
#include <QJsonValue>
#include <QMetaType>
#include <Qt>
#include <QtGlobal>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <stdint.h>
#include <utility>
#include "decryption_profiles_model.h"
#include "json_store.h"
#include "saved_systems_model.h"
#include "scan_list_targets.h"
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
#include <dsd-neo/runtime/log.h>

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

/* Whether a scan entry is analog (issue #526): a frequency entry's nfm or am protocol, or a saved system whose decode
   flags are -fA or -fM (scan_list_entry_analog_kind()). An analog entry carries no decryption: the editor hides the
   choice and the starter resolves none, so a profile it still names is not one it uses. */
bool
scanEntryAnalog(const QVariantMap& entry, const SavedSystemsModel* systems) {
    const QString flag = entry.value("kind").toString() == QStringLiteral("system") && systems
                             ? systems->getByUid(entry.value("systemUid").toString()).value("decodeFlag").toString()
                             : QString();
    return scan_list_entry_analog_kind(entry, flag) >= 0;
}

/**
 * Whether @p path lies below <data root>/decryption with no symlink or junction among its existing components from the
 * data root down, the path itself included. Key files are written in the clear, so a planted link there (a restored
 * setup keeps what it does not own under decryption/, issue #580) must never steer them anywhere else.
 */
bool
reachedThroughNoLink(const QString& path) {
    const QString root = QDir::cleanPath(json_store_root());
    if (root.size() < 2 || !QDir::isAbsolutePath(root) || QDir::cleanPath(path) != path
        || !path.startsWith(root + QStringLiteral("/decryption/"))) {
        return false;
    }
    QString current = root;
    for (const QString& part : path.mid(root.size() + 1).split(QLatin1Char('/'))) {
        current += QLatin1Char('/') + part;
        const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) {
            return false;
        }
        if (!info.exists()) {
            break; // nothing exists below a missing component
        }
    }
    return true;
}

/**
 * Write a key file privately (owner-only, through QSaveFile). Refused, with nothing written, unless the file and its
 * directories lie inside <data root>/decryption, reached through no link, before and after its directories are made.
 */
bool
privateWrite(const QString& path, const QByteArray& bytes) {
    const QString directory = QFileInfo(path).absolutePath();
    if (!reachedThroughNoLink(path) || !QDir().mkpath(directory) || !reachedThroughNoLink(path)) {
        return false;
    }
    const QString inside = QFileInfo(json_store_path(QStringLiteral("decryption"))).canonicalFilePath();
    const QString canonicalRoot = QFileInfo(json_store_root()).canonicalFilePath();
    const QString canonicalDirectory = QFileInfo(directory).canonicalFilePath();
    if (canonicalRoot.isEmpty() || inside != canonicalRoot + QStringLiteral("/decryption")
        || !canonicalDirectory.startsWith(inside + QLatin1Char('/'))) {
        return false;
    }
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
           && file.write(bytes) == bytes.size() && file.commit();
}

QString
protocolFor(const QString& flags) {
    // -mq is the P25 Simulcast preset, as Util.decryptionProtocol() and session_args_profile_compatible() read it.
    if (flags.contains("-ft") || flags.contains("-f1") || flags.contains("-f2") || flags.contains("-mq")) {
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
    // -fp is ProVoice, not dPMR, and no profile protocol describes it. dPMR (-fm) migrates as mixed too: a legacy key
    // of any type stays usable, as before.
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

bool
hasCompiled(const QVariantMap& row) {
    return row.value("compiled").typeId() == QMetaType::QVariantMap;
}

void
removeFiles(const QStringList& paths) {
    for (const auto& path : paths) {
        QFile::remove(path);
    }
}
} // namespace

bool
DecryptionProfilesModel::validUid(const QString& uid) {
    return validId(uid);
}

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
    compileLoadedRows();
}

/* Issue #580: a restored store carries profile definitions only, never a compiled form, so a row stored without one is
   compiled here through the editor's own validation into fresh files under decryption/<uid>/. The compiled rows are
   persisted together; if that fails their files are removed and they stay uncompiled, to be tried again on the next
   load. A row that cannot compile stays as stored, and configuration() asks for it to be saved again. Both failures
   are logged by profile reference and reason, never material. A row that already has a compiled form is kept as
   stored. */
void
DecryptionProfilesModel::compileLoadedRows() {
    auto next = m_rows;
    QStringList staged;
    bool compiled = false;
    for (auto& row : next) {
        if (hasCompiled(row)) {
            continue;
        }
        auto candidate = row;
        if (candidate.value("runtimeRef").toString().isEmpty()) {
            candidate.insert("runtimeRef", uuid());
        }
        QString error;
        QStringList files;
        if (compile(candidate, &error, &files)) {
            row = std::move(candidate);
            staged << files;
            compiled = true;
        } else {
            removeFiles(files);
            LOG_WARN("Decryption profile %s could not be compiled at load: %s\n",
                     qUtf8Printable(row.value("uid").toString()), qUtf8Printable(error));
        }
    }
    if (!compiled) {
        return;
    }
    if (!persist(next)) {
        removeFiles(staged);
        LOG_WARN(
            "Decryption profiles compiled at load could not be persisted; they compile again on the next start.\n");
        return;
    }
    m_rows = std::move(next);
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
            uses += static_cast<int>(
                std::count_if(scanEntries.cbegin(), scanEntries.cend(), [this, &uid](const QVariant& value) {
                    const auto entry = value.toMap();
                    return entry.value("decryptionProfileUid").toString() == uid && !scanEntryAnalog(entry, m_systems);
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

/* Vendor hex keeps its own grammar: ASCII whitespace and a leading 0x dropped, nothing else. The wider grouping
   session_args_key_hex_normalize() accepts is for direct and managed hex keys only. */
static QString
vendorHexNormalize(const QString& raw) {
    QString value = raw;
    value.remove(QRegularExpression(QStringLiteral("[ \\t\\n\\r\\f\\v]")));
    if (value.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        value.remove(0, 2);
    }
    return value.toUpper();
}

static bool
compileVendor(QVariantMap& row, QVariantMap& compiled) {
    const QString type = row.value("vendorType").toString();
    const QString raw = row.value("vendorValue").toString();
    QString value = vendorHexNormalize(raw);
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

/* The managed key formats in the order the editor lists them: id, short name, width (hex digits; 16 means up to 16,
   0 a decimal key), and the label's width note. */
struct ManagedKind {
    const char* id;
    const char* name;
    int hexDigits;
    const char* detail;
};

const ManagedKind kManagedKinds[] = {
    {"scalar", QT_TRANSLATE_NOOP("DecryptionProfilesModel", "DES / RC4 / ADP"), 16,
     QT_TRANSLATE_NOOP("DecryptionProfilesModel", "up to 16 hex digits")},
    {"aes128", QT_TRANSLATE_NOOP("DecryptionProfilesModel", "AES-128"), 32,
     QT_TRANSLATE_NOOP("DecryptionProfilesModel", "32 hex digits")},
    {"aes256", QT_TRANSLATE_NOOP("DecryptionProfilesModel", "AES-256"), 64,
     QT_TRANSLATE_NOOP("DecryptionProfilesModel", "64 hex digits")},
    {"tdea", QT_TRANSLATE_NOOP("DecryptionProfilesModel", "Triple DES"), 48,
     QT_TRANSLATE_NOOP("DecryptionProfilesModel", "48 hex digits")},
    {"basic", QT_TRANSLATE_NOOP("DecryptionProfilesModel", "Basic Privacy"), 0, "0–255"},
    {"scrambler", QT_TRANSLATE_NOOP("DecryptionProfilesModel", "Scrambler"), 0, "0–32767"},
};

const ManagedKind*
managedKind(const QString& id) {
    for (const auto& kind : kManagedKinds) {
        if (id == QLatin1String(kind.id)) {
            return &kind;
        }
    }
    return nullptr;
}

QString
kindName(const ManagedKind& kind) {
    return QCoreApplication::translate("DecryptionProfilesModel", kind.name);
}

/* Keyring cells a key of @p kind fills: its ID, then ID+0x101, +0x201 and +0x301 for each further 16 hex digits. */
int
managedWordCount(const ManagedKind& kind) {
    return kind.hexDigits > 16 ? kind.hexDigits / 16 : 1;
}

QString
keyKindOf(const QVariantMap& key) {
    return key.value("kind", "scalar").toString();
}

bool
managedKindAllowed(const QString& kind, const QString& protocol) {
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

/* How a key reads in a message: "Dispatch" (ID 1A), "Dispatch" (destination 1234), or ID 1A with no label. */
QString
keyDescription(const QVariantMap& key) {
    bool ok = false;
    const quint32 id = key.value("keyId").toUInt(&ok);
    const bool destination = key.value("lookup").toString() == "destination";
    const QString label = key.value("label").toString().trimmed();
    if (!ok) {
        return label.isEmpty() ? QString() : QStringLiteral("\"%1\"").arg(label);
    }
    const QString where = destination ? QObject::tr("destination %1").arg(id)
                                      : QObject::tr("ID %1").arg(QString::number(id, 16).toUpper());
    return label.isEmpty() ? where : QStringLiteral("\"%1\" (%2)").arg(label, where);
}

/* A save-time error that names the key it is about: Key "Dispatch" (ID 1): <message>. */
QString
namedKeyError(const QVariantMap& key, const QString& message) {
    const QString name = keyDescription(key);
    return name.isEmpty() ? message : QObject::tr("Key %1: %2").arg(name, message);
}

/* The material checks for @p raw as a key of @p kind, with the 16-hex-digit words it imports as on success. */
QString
managedMaterialError(const ManagedKind& kind, const QString& protocol, const QString& raw, QStringList* words) {
    if (raw.trimmed().isEmpty()) {
        return QObject::tr("Enter the key material.");
    }
    if (kind.hexDigits == 0) {
        if (!session_args_key_valid(QLatin1String(kind.id), raw)) {
            return QObject::tr("%1 keys are decimal %2.")
                .arg(kindName(kind), QCoreApplication::translate("DecryptionProfilesModel", kind.detail));
        }
        *words = {QString::number(raw.trimmed().toULongLong(), 16)};
        return {};
    }
    const QString material = session_args_key_hex_normalize(raw);
    if (!QRegularExpression("^[0-9A-F]+$").match(material).hasMatch()) {
        return QObject::tr("Key material is hex: use only 0-9 and A-F.");
    }
    const int digits = static_cast<int>(material.size());
    if (kind.hexDigits == 16 && digits > 16) {
        for (const auto& wider : kManagedKinds) {
            if (wider.hexDigits == digits && managedKindAllowed(QLatin1String(wider.id), protocol)) {
                return QLatin1String(wider.id) == "tdea"
                           ? QObject::tr("%1 hex digits is a %2 key. Choose %2.").arg(digits).arg(kindName(wider))
                           : QObject::tr("%1 hex digits is an %2 key. Choose %2.").arg(digits).arg(kindName(wider));
            }
        }
        return QObject::tr("%1 takes up to 16 hex digits; %2 entered.").arg(kindName(kind)).arg(digits);
    }
    if (kind.hexDigits > 16 && digits != kind.hexDigits) {
        return QObject::tr("%1 needs %2 hex digits; %3 entered.").arg(kindName(kind)).arg(kind.hexDigits).arg(digits);
    }
    words->clear();
    for (int at = 0; at < digits; at += 16) {
        *words << material.mid(at, 16);
    }
    return {};
}

/* A key's ID and lookup as the keyring sees them. */
struct ManagedSlot {
    quint32 id = 0;
    bool destination = false;
    quint32 base = 0;
};

/* The ID, lookup and format checks for one key, in the order compile() applies them; @p idParsed / @p id is its key
   ID as parsed. Fills @p slot on success. */
/* The lookup and ID range checks of managedSlotError(). */
QString
managedIdError(bool destination, bool idParsed, quint32 id, const QString& protocol) {
    if (destination && protocol == "p25") {
        return QObject::tr("P25 selects keys by the received key ID; destination lookup is not available.");
    }
    if (destination && (!idParsed || id == 0 || id > 0xFFFFFFU)) {
        return QObject::tr("Destination IDs are decimal 1 to 16777215.");
    }
    if (!destination && (!idParsed || id > (protocol == "dmr" ? 0xFFU : 0xFFFFU))) {
        return protocol == "dmr" ? QObject::tr("DMR key IDs are hex 0 to FF.")
                                 : QObject::tr("Key IDs are hex 0 to FFFF.");
    }
    return {};
}

QString
managedSlotError(const QVariantMap& key, bool idParsed, quint32 id, const QString& protocol, ManagedSlot* slot) {
    const ManagedKind* kind = managedKind(keyKindOf(key));
    if (!kind) {
        return QObject::tr("Choose a key format.");
    }
    const bool destination = key.value("lookup").toString() == "destination";
    const QString idError = managedIdError(destination, idParsed, id, protocol);
    if (!idError.isEmpty()) {
        return idError;
    }
    if (!managedKindAllowed(QLatin1String(kind->id), protocol)) {
        return QObject::tr("%1 keys are not available for this profile protocol.").arg(kindName(*kind));
    }
    if (destination && managedWordCount(*kind) != 1) {
        return QObject::tr("Destination lookup supports scalar legacy keys only.");
    }
    slot->id = id;
    slot->destination = destination;
    slot->base = destination ? keyring_destination_index(id) : id;
    return {};
}

QString
legacyDestinationError(const ManagedSlot& slot, const QStringList& words) {
    bool ok = false;
    if (slot.destination && (words.size() != 1 || words[0].toULongLong(&ok, 16) > 0xFFFFFFFFFFULL || !ok)) {
        return QObject::tr("Legacy destination lookup takes keys of at most 40 bits (10 hex digits).");
    }
    return {};
}

struct ManagedCollection {
    QByteArray hex{"keyid,value0,value1,value2,value3\n"}, dec{"keyid,value\n"};
    bool haveHex = false, haveDec = false;
    QHash<quint32, QString> occupied; // keyring cell -> the key description that holds it
    QSet<QString> references;
};

const quint32 kSegmentOffsets[] = {0, 0x101, 0x201, 0x301};

/* Why the @p count keyring cells from @p base collide with a key already in @p collection, or empty. */
QString
managedCollisionError(const ManagedCollection& collection, quint32 base, int count) {
    for (int i = 0; i < count; ++i) {
        const auto held = collection.occupied.constFind(base + kSegmentOffsets[i]);
        if (held != collection.occupied.cend()) {
            const QString other =
                held.value().isEmpty() ? QObject::tr("another key") : QObject::tr("key %1").arg(*held);
            return QObject::tr("Uses the same keyring cells as %1. Keys wider than 16 hex digits also take ID+101, "
                               "ID+201 and ID+301 in turn.")
                .arg(other);
        }
    }
    return {};
}

void
occupyManagedKey(ManagedCollection& collection, const QString& ref, quint32 base, int count, const QString& owner) {
    collection.references.insert(ref);
    for (int i = 0; i < count; ++i) {
        if (!collection.occupied.contains(base + kSegmentOffsets[i])) {
            collection.occupied.insert(base + kSegmentOffsets[i], owner);
        }
    }
}

bool
reserveManagedKey(ManagedCollection& collection, const QVariantMap& key, quint32 base, int count, QString* error) {
    const QString ref = key.value("uid").toString();
    if (!validId(ref) || collection.references.contains(ref)) {
        return profileError(error, QObject::tr("Key references must be unique."));
    }
    const QString collision = managedCollisionError(collection, base, count);
    if (!collision.isEmpty()) {
        return profileError(error, collision);
    }
    occupyManagedKey(collection, ref, base, count, keyDescription(key));
    return true;
}

/* Every check for one key in its collection (ID and lookup, format, material, then its keyring cells), appending its
   import rows on success. keyEntryError() runs the same helpers in the same order. */
bool
appendManagedKey(ManagedCollection& collection, const QVariantMap& key, const QString& protocol, QString* error) {
    bool ok = false;
    const quint32 id = key.value("keyId").toUInt(&ok);
    ManagedSlot slot;
    QString problem = managedSlotError(key, ok, id, protocol, &slot);
    QStringList words;
    if (problem.isEmpty()) {
        problem =
            managedMaterialError(*managedKind(keyKindOf(key)), protocol, key.value("material").toString(), &words);
    }
    if (problem.isEmpty()) {
        problem = legacyDestinationError(slot, words);
    }
    if (problem.isEmpty()) {
        (void)reserveManagedKey(collection, key, slot.base, static_cast<int>(words.size()), &problem);
    }
    if (!problem.isEmpty()) {
        return profileError(error, namedKeyError(key, problem));
    }
    if (slot.destination) {
        collection.dec += QByteArray::number(id) + ',' + QByteArray::number(words[0].toULongLong(&ok, 16)) + '\n';
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
        const QString rule = legacy ? QString() : session_args_key_protocol_error(protocol, type, value);
        if (!rule.isEmpty()) {
            return fail(rule);
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
    managedMappings(const QVariantList& mappingRows) {
        if (mappingRows.size() > DSD_DMR_TG_KEY_MAP_MAX) {
            return fail(QObject::tr("A DMR profile supports up to 256 talkgroup overrides."));
        }
        QByteArray csv("tg_dec,keyid_hex\n");
        QSet<quint32> groups;
        for (const auto& value : mappingRows) {
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

/* Blank material keeps a key's stored material, found by key UID, only while its format is unchanged: material kept
   across a format change would be read with the new format's meaning (scalar "10" as Basic Privacy 10). */
static QString
mergeProfileKeys(QVariantMap& row, const QVariantMap& draft) {
    if (!draft.contains("keys")) {
        return {};
    }
    QMap<QString, QVariantMap> previous;
    for (const auto& value : row.value("keys").toList()) {
        const auto key = value.toMap();
        previous.insert(key.value("uid").toString(), key);
    }
    QVariantList keys;
    for (const auto& value : draft.value("keys").toList()) {
        auto key = value.toMap();
        if (key.value("uid").toString().isEmpty()) {
            key.insert("uid", uuid());
        }
        if (key.value("material").toString().isEmpty()) {
            const auto stored = previous.value(key.value("uid").toString());
            if (!stored.isEmpty() && keyKindOf(stored) != keyKindOf(key)) {
                return namedKeyError(key, QObject::tr("Enter replacement material when changing the key format."));
            }
            key.insert("material", stored.value("material").toString());
        }
        keys.append(key);
    }
    row.insert("keys", keys);
    return {};
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
    const QString keysError = mergeProfileKeys(row, draft);
    if (!keysError.isEmpty()) {
        return failure(keysError);
    }
    QString error;
    QStringList past = row.value("pastRuntimeRefs").toStringList();
    if (!row.value("runtimeRef").toString().isEmpty()) {
        past.append(row.value("runtimeRef").toString());
    }
    row.insert("pastRuntimeRefs", past);
    row.insert("runtimeRef", uuid());
    QStringList staged;
    if (!compile(row, &error, &staged)) {
        removeFiles(staged);
        return failure(error);
    }
    auto next = m_rows;
    if (at < 0) {
        next.append(std::move(row));
    } else {
        next[at] = std::move(row);
    }
    if (!persist(next)) {
        removeFiles(staged);
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
    if (!hasCompiled(m_rows[row])) {
        if (error) {
            *error = tr("Open this decryption profile and save it again.");
        }
        return {};
    }
    auto result = m_rows[row].value("compiled").toMap();
    if (m_rows[row].value("mode").toString() == "direct") {
        // The stored row keeps what was typed; the session and a live apply take the material as the decoder does.
        result.insert("encKeyValue", session_args_key_value_normalize(result.value("encKeyType").toString(),
                                                                      m_rows[row].value("directValue").toString()));
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

QVariantList
DecryptionProfilesModel::keyKinds(const QString& protocol) const {
    QVariantList result;
    for (const auto& kind : kManagedKinds) {
        if (managedKindAllowed(QLatin1String(kind.id), protocol.isEmpty() ? QStringLiteral("mixed") : protocol)) {
            result << QVariantMap{
                {"id", kind.id},
                {"label", QStringLiteral("%1 · %2").arg(
                              kindName(kind), QCoreApplication::translate("DecryptionProfilesModel", kind.detail))}};
        }
    }
    return result;
}

/* The key ID the dialog holds, read as its lookup does: typed text is hex for a received key ID and decimal for a
   destination; a number (a draft row's keyId) is taken as is. An ID that does not parse leaves *parsed false, and
   managedSlotError() then names the range. */
static QString
entryKeyId(const QVariant& raw, bool destination, bool* parsed, quint32* id) {
    *parsed = false;
    *id = 0;
    if (!raw.isValid() || (raw.typeId() == QMetaType::QString && raw.toString().isEmpty())) {
        return QObject::tr("Enter a key ID.");
    }
    if (raw.typeId() != QMetaType::QString) {
        *id = raw.toUInt(parsed);
        return {};
    }
    const QString text = raw.toString();
    static const QRegularExpression hex(QStringLiteral("^[0-9A-Fa-f]+$"));
    static const QRegularExpression decimal(QStringLiteral("^[0-9]+$"));
    if (!(destination ? decimal : hex).match(text).hasMatch()) {
        return destination ? QObject::tr("Destination IDs are decimal digits.")
                           : QObject::tr("Key IDs are hex digits: use only 0-9 and A-F.");
    }
    bool ok = false;
    const qulonglong value = text.toULongLong(&ok, destination ? 10 : 16);
    *parsed = ok && value <= 0xFFFFFFFFULL;
    *id = *parsed ? static_cast<quint32>(value) : 0U;
    return {};
}

/* The protocol a typed key is judged under: the draft's, else the stored profile's, else mixed. */
static QString
entryProtocol(const QVariantMap& entry, const QVariantMap* storedRow) {
    const QString protocol = entry.value("protocol").toString();
    if (!protocol.isEmpty()) {
        return protocol;
    }
    return storedRow ? storedRow->value("protocol", "mixed").toString() : QStringLiteral("mixed");
}

/* The typed key with a reference: its own, else the edited row's, else a fresh one (a new key). */
static QVariantMap
entryWithUid(const QVariantMap& entry, const QVariantMap& edited) {
    QVariantMap key = entry;
    if (key.value("uid").toString().isEmpty()) {
        key.insert("uid", edited.value("uid").toString().isEmpty() ? uuid() : edited.value("uid").toString());
    }
    return key;
}

/* What blank material keeps for the edited row: an unsaved row's own material, else the stored key's with @p uid
   (saveProfile() refills it by key UID). *keptKind is the format it was entered for. */
static QString
retainedMaterial(const QVariantMap& edited, const QVariantMap* storedRow, const QString& uid, QString* keptKind) {
    if (!edited.value("material").toString().isEmpty()) {
        *keptKind = keyKindOf(edited);
        return edited.value("material").toString();
    }
    if (storedRow) {
        for (const auto& value : storedRow->value("keys").toList()) {
            const auto storedKey = value.toMap();
            if (storedKey.value("uid").toString() == uid) {
                *keptKind = keyKindOf(storedKey);
                return storedKey.value("material").toString();
            }
        }
    }
    return {};
}

/* The other draft rows, as compile() reserves them: each format's cells follow from its width. */
static ManagedCollection
otherDraftKeys(const QVariantList& draftKeys, int index, const QString& protocol) {
    ManagedCollection others;
    for (int i = 0; i < draftKeys.size(); ++i) {
        if (i == index) {
            continue;
        }
        const auto other = draftKeys[i].toMap();
        bool ok = false;
        const quint32 otherId = other.value("keyId").toUInt(&ok);
        ManagedSlot otherSlot;
        const ManagedKind* otherKind = managedKind(keyKindOf(other));
        if (otherKind && managedSlotError(other, ok, otherId, protocol, &otherSlot).isEmpty()) {
            occupyManagedKey(others, other.value("uid").toString(), otherSlot.base, managedWordCount(*otherKind),
                             keyDescription(other));
        } else {
            others.references.insert(other.value("uid").toString());
        }
    }
    return others;
}

QString
DecryptionProfilesModel::keyEntryError(const QString& profileUid, const QVariantList& draftKeys, int index,
                                       const QVariantMap& entry) const {
    const int stored = rowForUid(profileUid);
    const QString protocol = entryProtocol(entry, stored >= 0 ? &m_rows[stored] : nullptr);
    const QVariantMap edited = index >= 0 && index < draftKeys.size() ? draftKeys[index].toMap() : QVariantMap();
    const QVariantMap key = entryWithUid(entry, edited);
    bool parsed = false;
    quint32 id = 0;
    QString problem = entryKeyId(entry.value("keyId"), key.value("lookup").toString() == "destination", &parsed, &id);
    ManagedSlot slot;
    if (problem.isEmpty()) {
        problem = managedSlotError(key, parsed, id, protocol, &slot);
    }
    if (!problem.isEmpty()) {
        return problem;
    }

    QString material = entry.value("material").toString();
    bool retained = false;
    if (material.isEmpty() && !edited.isEmpty()) {
        QString keptKind;
        material =
            retainedMaterial(edited, stored >= 0 ? &m_rows[stored] : nullptr, key.value("uid").toString(), &keptKind);
        retained = !material.isEmpty();
        if (retained && keptKind != keyKindOf(key)) {
            return QObject::tr("Enter replacement material when changing the key format.");
        }
    }
    QStringList words;
    problem = managedMaterialError(*managedKind(keyKindOf(key)), protocol, material, &words);
    if (!problem.isEmpty()) {
        return retained ? QObject::tr("The kept material does not fit this key. Enter replacement material.") : problem;
    }
    problem = legacyDestinationError(slot, words);
    if (!problem.isEmpty()) {
        return problem;
    }

    ManagedCollection others = otherDraftKeys(draftKeys, index, protocol);
    (void)reserveManagedKey(others, key, slot.base, static_cast<int>(words.size()), &problem);
    return problem;
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
