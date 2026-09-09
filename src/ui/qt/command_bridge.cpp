// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QChar>
#include <QLatin1String>
#include <QtGlobal>
#include <stddef.h>
#include "command_bridge.h"

#include <QByteArray>
#include <QMetaType>
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

#include "decode_mode_flag.h"
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
CommandBridge::setTalkgroupPolicy(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                                  const QVariantMap& changes) const {
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
            if (!ok || !(priority >= 0 && priority <= 100) || priority != static_cast<int>(priority)) {
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
    return submitTalkgroupRow(start, end, context, generation, changes.value(QStringLiteral("name")).toString(),
                              changes.value(QStringLiteral("listening")).toBool(),
                              changes.value(QStringLiteral("priority")).toInt(),
                              changes.value(QStringLiteral("preempt")).toBool(), fields);
}

bool
CommandBridge::renameTalkgroup(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                               const QString& name) const {
    return submitTalkgroupRow(start, end, context, generation, name, false, 0, false, DSD_APP_TG_FIELD_NAME);
}

bool
CommandBridge::addTalkgroup(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                            const QString& name, bool listen, int priority, bool preempt) const {
    return submitTalkgroupRow(start, end, context, generation, name, listen, priority, preempt,
                              DSD_APP_TG_FIELD_NAME | DSD_APP_TG_FIELD_LISTEN | DSD_APP_TG_FIELD_PRIORITY
                                  | DSD_APP_TG_FIELD_PREEMPT);
}

bool
CommandBridge::removeTalkgroup(unsigned int start, unsigned int end, const QString& context,
                               unsigned int generation) const {
    bool ok = false;
    dsd_app_tg_range_payload p = {};
    p.policy_context = context.toULongLong(&ok);
    if (!ok || end < start) {
        return false;
    }
    p.id_start = start;
    p.id_end = end;
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
    } else {
        return false;
    }
    QString normalized = (payload.key_type == DSD_APP_KEY_TYPE_HEX || payload.key_type == DSD_APP_KEY_TYPE_RC4)
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

} // namespace dsd_qt
