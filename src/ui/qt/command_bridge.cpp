// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QChar>
#include <QFile>
#include <QLatin1String>
#include <QMap>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>
#include <cmath>
#include <stddef.h>
#include <utility>
#include "command_bridge.h"

#include <QByteArray>
#include <QDir>
#include <QMetaType>
#include <QTemporaryFile>
#include "json_store.h"
// WP0's C export payload uses a flexible array; only its fixed header is read in C++.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <dsd-neo/app_control/commands.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>

#include <atomic>
#include <cstring>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include "decode_mode_flag.h"
#include "decryption_profile_provider.h"
#include "session_args.h"

namespace dsd_qt {

namespace {

/** COALESCED is success too: the queue merged this submit into a pending one. */
bool
accepted(int status) {
    return status == DSD_APP_COMMAND_SUBMIT_QUEUED || status == DSD_APP_COMMAND_SUBMIT_COALESCED;
}

} // namespace

CommandBridge::CommandBridge(QObject* parent) : QObject(parent) {}

CommandBridge::~CommandBridge() = default;

// WP-D1: typed policy edits; backend owns bounds, aliases and version validation.
namespace {
bool
submitTalkgroupRow(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                   const QString& name, bool listen, int priority, bool preempt, uint32_t fields) {
    bool ok = false;
    dsd_app_tg_row_payload p = {};
    p.policy_context = context.toULongLong(&ok);
    const QByteArray utf8 = name.toUtf8();
    if (!ok || end < start || priority < 0 || priority > 100 || utf8.size() >= static_cast<qsizetype>(sizeof p.name)
        || utf8.contains('\0')) {
        return false;
    }
    p.id_start = start;
    p.id_end = end;
    p.policy_generation = generation;
    p.fields = fields;
    p.listen = listen;
    p.priority = priority;
    p.preempt = preempt;
    DSD_MEMCPY(p.name, utf8.constData(), static_cast<size_t>(utf8.size()));
    return accepted(dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &p, sizeof p));
}
} // namespace

bool
CommandBridge::setTalkgroupPolicy(unsigned int idStart, unsigned int idEnd, const QString& context,
                                  unsigned int generation, const QVariantMap& changes) const {
    uint32_t fields = 0;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        if (it.key() == QStringLiteral("name") && it.value().typeId() == QMetaType::QString) {
            fields |= DSD_APP_TG_FIELD_NAME;
        } else if (it.key() == QStringLiteral("listening") && it.value().typeId() == QMetaType::Bool) {
            fields |= DSD_APP_TG_FIELD_LISTEN;
        } else if (it.key() == QStringLiteral("preempt") && it.value().typeId() == QMetaType::Bool) {
            fields |= DSD_APP_TG_FIELD_PREEMPT;
        } else if (it.key() == QStringLiteral("priority")) {
            bool ok = false;
            const double priority = it.value().toDouble(&ok);
            if (!ok || !(priority >= 0 && priority <= 100) || std::fmod(priority, 1.0) != 0.0) {
                return false;
            }
            fields |= DSD_APP_TG_FIELD_PRIORITY;
        } else {
            return false;
        }
    }
    if (!fields) {
        return false;
    }
    return submitTalkgroupRow(idStart, idEnd, context, generation, changes.value(QStringLiteral("name")).toString(),
                              changes.value(QStringLiteral("listening")).toBool(),
                              changes.value(QStringLiteral("priority")).toInt(),
                              changes.value(QStringLiteral("preempt")).toBool(), fields);
}

bool
CommandBridge::renameTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                               unsigned int generation, const QString& name) const {
    return submitTalkgroupRow(idStart, idEnd, context, generation, name, false, 0, false, DSD_APP_TG_FIELD_NAME);
}

bool
// cppcheck-suppress functionStatic // Q_INVOKABLE: QML submits additions through this context object.
CommandBridge::addTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context, unsigned int generation,
                            const QString& name, bool listen, int priority, bool preempt) const {
    return submitTalkgroupRow(idStart, idEnd, context, generation, name, listen, priority, preempt,
                              DSD_APP_TG_FIELD_NAME | DSD_APP_TG_FIELD_LISTEN | DSD_APP_TG_FIELD_PRIORITY
                                  | DSD_APP_TG_FIELD_PREEMPT);
}

bool
CommandBridge::removeTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                               unsigned int generation) const {
    bool ok = false;
    dsd_app_tg_range_payload p = {};
    p.policy_context = context.toULongLong(&ok);
    if (!ok || idEnd < idStart) {
        return false;
    }
    p.id_start = idStart;
    p.id_end = idEnd;
    p.policy_generation = generation;
    return accepted(dsd_app_command_submit(DSD_APP_CMD_TG_ROW_REMOVE, &p, sizeof p));
}

bool
CommandBridge::saveTalkgroupList(const QString& context, unsigned int generation, const QString& path) const {
    bool ok = false;
    const uint64_t version = context.toULongLong(&ok);
    const QByteArray utf8 = path.toUtf8();
    if (!ok || utf8.isEmpty() || utf8.size() >= 1024 || utf8.contains('\0')) {
        return false;
    }
    QByteArray payload(static_cast<qsizetype>(offsetof(dsd_app_tg_export_payload, path)) + utf8.size() + 1, '\0');
    DSD_MEMCPY(payload.data() + offsetof(dsd_app_tg_export_payload, policy_context), &version, sizeof version);
    DSD_MEMCPY(payload.data() + offsetof(dsd_app_tg_export_payload, policy_generation), &generation, sizeof generation);
    DSD_MEMCPY(payload.data() + offsetof(dsd_app_tg_export_payload, path), utf8.constData(),
               static_cast<size_t>(utf8.size()));
    return accepted(
        dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, payload.constData(), static_cast<size_t>(payload.size())));
}

// End WP-D1.

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::toggleMute() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_TOGGLE_MUTE));
}

bool
CommandBridge::holdTalkgroup(unsigned int talkgroup) const {
    return accepted(dsd_app_command_set_u32(DSD_APP_CMD_TG_HOLD_SET, static_cast<uint32_t>(talkgroup)));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::lockoutSlot(int slot) const {
    const uint8_t index = (slot > 0) ? 1U : 0U;
    return accepted(dsd_app_command_set_u8(DSD_APP_CMD_LOCKOUT_SLOT, index));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::setTalkgroupListening(unsigned int idStart, unsigned int idEnd, bool listen) const {
    dsd_app_tg_listen_payload payload;
    DSD_MEMSET(&payload, 0, sizeof(payload));
    payload.id_start = idStart;
    payload.id_end = idEnd;
    payload.listen = listen ? 1 : 0;
    return accepted(dsd_app_command_set_tg_listen(&payload));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE entry point.
CommandBridge::setPersistTgLockouts(bool persist) const {
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET, persist ? 1 : 0));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE entry point.
CommandBridge::clearTemporaryTgAvoids(const QString& context) const {
    bool valid = false;
    const uint64_t value = context.toULongLong(&valid);
    return valid && accepted(dsd_app_command_submit(DSD_APP_CMD_TG_SESSION_AVOID_CLEAR, &value, sizeof value));
}

bool
CommandBridge::setAllTalkgroupsListening(bool listen, const QString& tag) const {
    dsd_app_tg_listen_all_payload payload;
    DSD_MEMSET(&payload, 0, sizeof(payload));
    payload.listen = listen ? 1 : 0;
    DSD_SNPRINTF(payload.tags, sizeof(payload.tags), "%s", tag.toUtf8().constData());
    return accepted(dsd_app_command_set_tg_listen_all(&payload));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::clearEncLockouts() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_ENC_LOCKOUT_CLEAR));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::toggleScanHold() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_SCAN_HOLD_TOGGLE));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::avoidCurrentChannel() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::clearScanAvoids() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_SCAN_AVOID_CLEAR));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::nextChannel() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_CHANNEL_CYCLE));
}

bool
CommandBridge::tuneHz(unsigned int hz) const {
    return accepted(dsd_app_command_set_u32(DSD_APP_CMD_RTL_SET_FREQ, static_cast<uint32_t>(hz)));
}

bool
CommandBridge::manualTuneHz(unsigned int hz) const {
    return accepted(dsd_app_command_set_u32(DSD_APP_CMD_MANUAL_TUNE, static_cast<uint32_t>(hz)));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::releaseTuner() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_TUNER_RELEASE));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::setTrunking(bool on) const {
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_TRUNK_SET, on ? 1 : 0));
}

bool
CommandBridge::setTunerGain(int gain_db) const {
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_GAIN, static_cast<int32_t>(gain_db)));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::setSquelchDb(double db) const {
    return accepted(dsd_app_command_set_double(DSD_APP_CMD_RTL_SET_SQL_DB, db));
}

bool
CommandBridge::setAirspy(const QString& key, const QString& value) const {
    const QByteArray k = (QStringLiteral("airspy_") + key).toUtf8();
    const QByteArray v = value.toUtf8();
    dsd_app_airspy_setting_payload p{};
    if (k.contains('\0') || v.contains('\0') || k.size() >= (qsizetype)sizeof p.key
        || v.size() >= (qsizetype)sizeof p.value) {
        return false;
    }
    DSD_SNPRINTF(p.key, sizeof p.key, "%s", k.constData());
    DSD_SNPRINTF(p.value, sizeof p.value, "%s", v.constData());
    return accepted(dsd_app_command_submit(DSD_APP_CMD_AIRSPY_SET, &p, sizeof p));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::setPpm(int ppm) const {
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_RTL_SET_PPM, static_cast<int32_t>(ppm)));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::setModulation(int modulation) const {
    if (modulation < 0 || modulation > 2) {
        return false;
    }
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, static_cast<int32_t>(modulation)));
}

bool
CommandBridge::setDecodeMode(int mode) const {
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, static_cast<int32_t>(mode)));
}

int
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::decodeModeForFlag(const QString& flag) const {
    return decode_mode_for_flag(flag);
}

int
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::cycleHistoryMode() const {
    return dsd_app_frontend_history_cycle_mode();
}

bool
CommandBridge::importChannelMap(const QString& path) const {
    if (path.isEmpty()) {
        return false;
    }
    return accepted(dsd_app_command_set_string(DSD_APP_CMD_IMPORT_CHANNEL_MAP, path.toUtf8().constData()));
}

bool
CommandBridge::importP25Bandplan(const QString& path) const {
    if (path.isEmpty()) {
        return false;
    }
    return accepted(dsd_app_command_set_string(DSD_APP_CMD_IMPORT_P25_BANDPLAN, path.toUtf8().constData()));
}

bool
CommandBridge::importGroupList(const QString& path) const {
    if (path.isEmpty()) {
        return false;
    }
    return accepted(dsd_app_command_set_string(DSD_APP_CMD_IMPORT_GROUP_LIST, path.toUtf8().constData()));
}

bool
CommandBridge::importKeys(const QString& path, bool hex) const {
    if (path.isEmpty()) {
        return false;
    }
    return accepted(dsd_app_command_set_string(hex ? DSD_APP_CMD_IMPORT_KEYS_HEX : DSD_APP_CMD_IMPORT_KEYS_DEC,
                                               path.toUtf8().constData()));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::clearChannelMap() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_IMPORT_CHANNEL_MAP_CLEAR));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::clearGroupList() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::clearKeys() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_IMPORT_KEYS_CLEAR));
}

bool
CommandBridge::importSrcList(const QString& path) const {
    if (path.isEmpty()) {
        return false;
    }
    return accepted(dsd_app_command_set_string(DSD_APP_CMD_IMPORT_SRC_LIST, path.toUtf8().constData()));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::clearSrcList() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR));
}

// WP-D2: QString/QML copies cannot promise erasure. Wipe owned UTF-8 and wire copies.
bool
CommandBridge::applyEncryptionKey(const QString& type, const QString& value) const {
    dsd_app_key_direct_payload payload = {};
    if (type == QLatin1String("basic")) {
        payload.key_type = DSD_APP_KEY_TYPE_BASIC;
    } else if (type == QLatin1String("hex")) {
        payload.key_type = DSD_APP_KEY_TYPE_HEX;
    } else if (type == QLatin1String("rc4")) {
        payload.key_type = DSD_APP_KEY_TYPE_RC4;
    } else if (type == QLatin1String("scrambler")) {
        payload.key_type = DSD_APP_KEY_TYPE_SCRAMBLER;
    } else if (type == QLatin1String("m17Scrambler")) {
        payload.key_type = DSD_APP_KEY_TYPE_M17_SCRAMBLER;
    } else if (type == QLatin1String("m17Aes")) {
        payload.key_type = DSD_APP_KEY_TYPE_M17_AES;
    } else {
        return false;
    }
    QString normalized = (payload.key_type == DSD_APP_KEY_TYPE_HEX || payload.key_type == DSD_APP_KEY_TYPE_RC4
                          || payload.key_type >= DSD_APP_KEY_TYPE_M17_SCRAMBLER)
                             ? session_args_key_hex_normalize(value)
                             : value.trimmed();
    QByteArray bytes = normalized.toUtf8();
    DSD_SECURE_ZERO(normalized.data(), static_cast<size_t>(normalized.size()) * sizeof(QChar));
    const bool fits = bytes.size() < static_cast<qsizetype>(sizeof payload.value) && !bytes.contains('\0');
    int result = DSD_APP_COMMAND_SUBMIT_REJECTED;
    if (fits) {
        DSD_MEMCPY(payload.value, bytes.constData(), static_cast<size_t>(bytes.size()));
        result = dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof payload);
    }
    DSD_SECURE_ZERO(bytes.data(), static_cast<size_t>(bytes.size()));
    DSD_SECURE_ZERO(&payload, sizeof payload);
    return accepted(result);
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::setForceKeyMode(int mode) const {
    return mode >= 0 && mode <= 2 && accepted(dsd_app_command_set_i32(DSD_APP_CMD_FORCE_KEY_SET, mode));
}

void
CommandBridge::acknowledgeDecryptionResult(quint64 requestId) {
    if (requestId == m_pendingKeyRequest) {
        m_pendingKeyRequest = 0;
    }
}

QVariantMap
CommandBridge::decryptionContext(const QString& targetId, const QString& keyEpoch) const {
    return {{"session", QString::number(dsd_app_command_session_generation())},
            {"tuning", QString::number(dsd_trunk_tuning_generation())},
            {"target", targetId},
            {"keyEpoch", keyEpoch}};
}

QString
CommandBridge::submitDecryption(const QVariantMap& fields, int scope, const QVariantMap& context) {
    const quint64 session = context.value("session").toString().toULongLong();
    if (m_pendingKeyRequest && m_pendingKeySession != dsd_app_command_session_generation()) {
        m_pendingKeyRequest = 0;
    }
    if (m_pendingKeyRequest || !session) {
        return {};
    }
    static std::atomic<quint64> nextRequest{1};
    dsd_app_decryption_payload payload = {};
    payload.request_id = nextRequest.fetch_add(1);
    if (!payload.request_id) {
        payload.request_id = nextRequest.fetch_add(1);
    }
    payload.session_generation = session;
    payload.tune_generation = context.value("tuning").toString().toULongLong();
    payload.key_epoch = context.value("keyEpoch").toString().toULongLong();
    payload.scope = scope;
    payload.fields = fields.value("fields").toUInt();
    payload.source = fields.value("source").toInt();
    payload.force = fields.value("force").toInt();
    const QStringList types{"basic", "hex", "rc4", "scrambler", "m17Scrambler", "m17Aes"};
    payload.key_type = types.indexOf(fields.value("type").toString());
    const auto copy = [](char* to, size_t capacity, const QString& from) {
        auto bytes = from.toUtf8();
        const bool valid = !bytes.contains('\0') && static_cast<size_t>(bytes.size()) < capacity;
        if (valid) {
            std::memcpy(to, bytes.constData(), static_cast<size_t>(bytes.size()));
        }
        DSD_SECURE_ZERO(bytes.data(), static_cast<size_t>(bytes.size()));
        return valid;
    };
    const bool valid = copy(payload.target_id, sizeof(payload.target_id), context.value("target").toString())
                       && copy(payload.profile_ref, sizeof(payload.profile_ref), fields.value("profile").toString())
                       && copy(payload.keys_hex, sizeof(payload.keys_hex), fields.value("hex").toString())
                       && copy(payload.keys_dec, sizeof(payload.keys_dec), fields.value("dec").toString())
                       && copy(payload.map_file, sizeof(payload.map_file), fields.value("map").toString())
                       && copy(payload.value, sizeof(payload.value), fields.value("value").toString());
    const quint64 id = payload.request_id;
    const bool submitted = valid && dsd_app_command_submit(DSD_APP_CMD_DECRYPTION_APPLY, &payload, sizeof(payload)) > 0;
    DSD_SECURE_ZERO(&payload, sizeof(payload));
    if (!submitted) {
        return {};
    }
    m_pendingKeyRequest = id;
    m_pendingKeySession = session;
    return QString::number(id);
}

QString
CommandBridge::applyDecryptionDraft(const QString& type, const QString& value, bool forceChanged, int force,
                                    const QVariantMap& context) {
    if (!type.isEmpty() && !session_args_key_valid(type, value)) {
        return {};
    }
    const uint32_t fields = (type.isEmpty() ? 0U : static_cast<uint32_t>(DSD_APP_DECRYPTION_MATERIAL))
                            | (forceChanged ? static_cast<uint32_t>(DSD_APP_DECRYPTION_FORCE) : 0U);
    if (!fields) {
        return {};
    }
    const QString normalized =
        type == "basic" || type == "scrambler" ? value.trimmed() : session_args_key_hex_normalize(value);
    return submitDecryption({{"fields", fields},
                             {"source", DSD_APP_KEY_SOURCE_DIRECT_OVERLAY},
                             {"type", type},
                             {"value", normalized},
                             {"force", force}},
                            DSD_APP_KEY_SCOPE_DEFAULTS, context);
}

QString
CommandBridge::applyDecryptionProfile(const QString& uid, int scope, const QVariantMap& context) {
    if (!m_profiles) {
        return {};
    }
    QString error;
    const auto config = m_profiles->configuration(uid, &error);
    if (!error.isEmpty() || config.isEmpty() || config.value("decryptionSelectionMode").toString() == "vendor") {
        return {};
    }
    const QString type = config.value("encKeyType").toString();
    const int source = config.value("decryptionSelectionMode").toString() == "automatic" ? DSD_APP_KEY_SOURCE_COLLECTION
                       : type.isEmpty()                                                  ? DSD_APP_KEY_SOURCE_NONE
                                                                                         : DSD_APP_KEY_SOURCE_DIRECT;
    const int force = config.value("decryptionForce", -1).toInt();
    const QString request =
        submitDecryption({{"fields", DSD_APP_DECRYPTION_MATERIAL | DSD_APP_DECRYPTION_MAP
                                         | (force >= 0 ? static_cast<uint32_t>(DSD_APP_DECRYPTION_FORCE) : 0U)},
                          {"source", source},
                          {"type", type},
                          {"value", config.value("encKeyValue")},
                          {"force", force},
                          {"hex", config.value("keysHexCsvPath")},
                          {"dec", config.value("keysDecCsvPath")},
                          {"map", config.value("dmrTgKeyCsvPath")},
                          {"profile", config.value("decryptionProfileRef", uid)}},
                         scope, context);
    if (!request.isEmpty()) {
        m_profiles->retainForSession(uid);
    }
    return request;
}

QString
CommandBridge::applyDmrKeyMap(const QString& path, int scope, const QVariantMap& context) {
    return submitDecryption({{"fields", DSD_APP_DECRYPTION_MAP}, {"map", path}}, scope, context);
}

void
CommandBridge::clearSessionInputs() {
    m_selectionFiles.clear();
}

static bool
write_talkgroup_selection(QTemporaryFile* file, const QVariantList& rows) {
    for (const auto& value : rows) {
        const auto row = value.toMap();
        bool firstOk = false, lastOk = false, indexOk = false;
        const auto first = row.value("first").toUInt(&firstOk);
        const auto last = row.value("last").toUInt(&lastOk);
        const auto index = row.value("index").toInt(&indexOk);
        if (!firstOk || !lastOk || !indexOk || !first || first > last || index < -1) {
            return false;
        }
        const QByteArray line =
            QByteArray::number(index) + ',' + QByteArray::number(first) + ',' + QByteArray::number(last) + '\n';
        if (file->write(line) != line.size()) {
            return false;
        }
    }
    return true;
}

bool
CommandBridge::setTalkgroupSelection(bool listening, const QString& context, unsigned int generation,
                                     const QVariantList& rows) {
    if (rows.isEmpty() || static_cast<quint64>(rows.size()) > UINT32_MAX) {
        return false;
    }
    const QString directory = json_store_path(QStringLiteral("pending-selections"));
    if (!QDir().mkpath(directory)) {
        return false;
    }
    auto file = QSharedPointer<QTemporaryFile>::create(directory + QStringLiteral("/tg-XXXXXX.csv"));
    if (!file->open() || !file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return false;
    }
    if (!write_talkgroup_selection(file.data(), rows)) {
        return false;
    }
    if (!file->flush()) {
        return false;
    }
    dsd_app_tg_selection_payload payload = {};
    bool contextOk = false;
    payload.policy_context = context.toULongLong(&contextOk);
    payload.policy_generation = generation;
    payload.count = static_cast<uint32_t>(rows.size());
    payload.listening = listening ? 1 : 0;
    const auto path = file->fileName().toUtf8();
    if (!contextOk || static_cast<size_t>(path.size()) >= sizeof(payload.selection_path)) {
        return false;
    }
    std::memcpy(payload.selection_path, path.constData(), static_cast<size_t>(path.size()));
    file->close();
    if (dsd_app_command_submit(DSD_APP_CMD_TG_SELECTION_SET, &payload, sizeof(payload)) <= 0) {
        return false;
    }
    m_selectionFiles.append(file);
    return true;
}

} // namespace dsd_qt
