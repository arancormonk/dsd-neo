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
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/runtime/scan_mode.h>
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
#include <dsd-neo/core/id_pattern.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/app_control/recording_view.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/app_control/squelch_view.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <stdint.h>

#include <atomic>
#include <cstring>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include "decode_mode_flag.h"
#include "decryption_profile_provider.h"
#include "recording_status.h"
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

/* The row a single-row edit names (B14): the listed row's table index, its kind and its key. tableIndex -1 is a
 * heard-only ID or a bounds-only edit; a pattern row (non-empty pattern, 0/0 bounds) always carries its index. The
 * pattern's characters are the decoder's to judge. */
struct TalkgroupRowToken {
    int32_t index = -1;
    uint8_t kind = DSD_APP_TG_ROW_EXACT;
    QByteArray pattern;
};

bool
makeTalkgroupRowToken(unsigned int start, unsigned int end, int tableIndex, const QString& pattern,
                      TalkgroupRowToken* out) {
    if (end < start || tableIndex < -1) {
        return false;
    }
    out->index = tableIndex;
    out->pattern = pattern.toUtf8();
    if (out->pattern.isEmpty()) {
        out->kind = start != end ? DSD_APP_TG_ROW_RANGE : DSD_APP_TG_ROW_EXACT;
        return true;
    }
    out->kind = DSD_APP_TG_ROW_PATTERN;
    return tableIndex >= 0 && start == 0 && end == 0 && out->pattern.size() < DSD_ID_PATTERN_TEXT_MAX
           && !out->pattern.contains('\0');
}

template <typename Payload>
void
applyTalkgroupRowToken(const TalkgroupRowToken& token, Payload* p) {
    p->table_index = token.index;
    p->kind = token.kind;
    DSD_MEMCPY(p->pattern, token.pattern.constData(), static_cast<size_t>(token.pattern.size()));
}

/* The row an edit names: its bounds, the list version it was captured at and its row token parts. */
struct TalkgroupRowRef {
    unsigned int start;
    unsigned int end;
    const QString& context;
    unsigned int generation;
    int tableIndex;
    const QString& pattern;
};

/* The values an edit sets; `fields` says which of them apply. */
struct TalkgroupRowValues {
    const QString& name;
    bool listen;
    int priority;
    bool preempt;
    uint32_t fields;
    bool record;
};

bool
submitTalkgroupRow(const TalkgroupRowRef& row, const TalkgroupRowValues& values) {
    bool ok = false;
    dsd_app_tg_row_payload p = {};
    p.policy_context = row.context.toULongLong(&ok);
    const QByteArray utf8 = values.name.toUtf8();
    TalkgroupRowToken token;
    if (!ok || !makeTalkgroupRowToken(row.start, row.end, row.tableIndex, row.pattern, &token) || values.priority < 0
        || values.priority > 100 || utf8.size() >= static_cast<qsizetype>(sizeof p.name) || utf8.contains('\0')) {
        return false;
    }
    applyTalkgroupRowToken(token, &p);
    p.id_start = row.start;
    p.id_end = row.end;
    p.policy_generation = row.generation;
    p.fields = values.fields;
    p.listen = values.listen;
    p.priority = values.priority;
    p.preempt = values.preempt;
    p.record = values.record ? 1U : 0U;
    DSD_MEMCPY(p.name, utf8.constData(), static_cast<size_t>(utf8.size()));
    return accepted(dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &p, sizeof p));
}

/* The row field one setTalkgroupPolicy() change edits; 0 for a key it does not know or a value of the wrong kind. */
uint32_t
talkgroup_change_field(const QString& key, const QVariant& value) {
    if (key == QStringLiteral("name") && value.typeId() == QMetaType::QString) {
        return DSD_APP_TG_FIELD_NAME;
    }
    if (key == QStringLiteral("listening") && value.typeId() == QMetaType::Bool) {
        return DSD_APP_TG_FIELD_LISTEN;
    }
    if (key == QStringLiteral("preempt") && value.typeId() == QMetaType::Bool) {
        return DSD_APP_TG_FIELD_PREEMPT;
    }
    if (key == QStringLiteral("record") && value.typeId() == QMetaType::Bool) {
        return DSD_APP_TG_FIELD_RECORD;
    }
    if (key == QStringLiteral("priority")) {
        bool ok = false;
        const double priority = value.toDouble(&ok);
        if (!ok || !(priority >= 0 && priority <= 100) || std::fmod(priority, 1.0) != 0.0) {
            return 0U;
        }
        return DSD_APP_TG_FIELD_PRIORITY;
    }
    return 0U;
}
} // namespace

bool
CommandBridge::setTalkgroupPolicy(unsigned int idStart, unsigned int idEnd, const QString& context,
                                  unsigned int generation, const QVariantMap& changes, int tableIndex,
                                  const QString& pattern) const {
    uint32_t fields = 0;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        const uint32_t field = talkgroup_change_field(it.key(), it.value());
        if (field == 0U) {
            return false;
        }
        fields |= field;
    }
    if (!fields) {
        return false;
    }
    const QString name = changes.value(QStringLiteral("name")).toString();
    return submitTalkgroupRow(
        {idStart, idEnd, context, generation, tableIndex, pattern},
        {name, changes.value(QStringLiteral("listening")).toBool(), changes.value(QStringLiteral("priority")).toInt(),
         changes.value(QStringLiteral("preempt")).toBool(), fields, changes.value(QStringLiteral("record")).toBool()});
}

bool
CommandBridge::renameTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                               unsigned int generation, const QString& name, int tableIndex,
                               const QString& pattern) const {
    return submitTalkgroupRow({idStart, idEnd, context, generation, tableIndex, pattern},
                              {name, false, 0, false, DSD_APP_TG_FIELD_NAME, false});
}

bool
// cppcheck-suppress functionStatic // Q_INVOKABLE: QML submits additions through this context object.
CommandBridge::addTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context, unsigned int generation,
                            const QString& name, bool listen, int priority, bool preempt, bool record) const {
    const QString noPattern;
    return submitTalkgroupRow({idStart, idEnd, context, generation, -1, noPattern},
                              {name, listen, priority, preempt,
                               DSD_APP_TG_FIELD_NAME | DSD_APP_TG_FIELD_LISTEN | DSD_APP_TG_FIELD_PRIORITY
                                   | DSD_APP_TG_FIELD_PREEMPT
                                   | (listen ? static_cast<uint32_t>(DSD_APP_TG_FIELD_RECORD) : 0U),
                               record});
}

bool
CommandBridge::removeTalkgroup(unsigned int idStart, unsigned int idEnd, const QString& context,
                               unsigned int generation, int tableIndex, const QString& pattern) const {
    bool ok = false;
    dsd_app_tg_range_payload p = {};
    p.policy_context = context.toULongLong(&ok);
    TalkgroupRowToken token;
    if (!ok || !makeTalkgroupRowToken(idStart, idEnd, tableIndex, pattern, &token)) {
        return false;
    }
    applyTalkgroupRowToken(token, &p);
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
CommandBridge::skipSlot(int slot) const {
    const uint8_t index = (slot > 0) ? 1U : 0U;
    return accepted(dsd_app_command_set_u8(DSD_APP_CMD_SKIP_SLOT, index));
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::setTalkgroupListening(unsigned int idStart, unsigned int idEnd, bool listen, const QString& context,
                                     unsigned int generation, int tableIndex, const QString& pattern) const {
    dsd_app_tg_listen_payload payload;
    DSD_MEMSET(&payload, 0, sizeof(payload));
    TalkgroupRowToken token;
    bool ok = context.isEmpty();
    payload.policy_context = ok ? 0U : context.toULongLong(&ok);
    if (!ok || !makeTalkgroupRowToken(idStart, idEnd, tableIndex, pattern, &token)
        || (tableIndex >= 0 && context.isEmpty())) {
        return false;
    }
    applyTalkgroupRowToken(token, &payload);
    payload.policy_generation = context.isEmpty() ? 0U : generation;
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
CommandBridge::setRelaxedCrc(bool on) const {
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_AGGR_SYNC_TOGGLE, on ? 1 : 0));
}

void
CommandBridge::noteRecording(const QString& refusal) const {
    if (m_recordingStatus != nullptr) {
        if (refusal.isEmpty()) {
            m_recordingStatus->noteSent();
        } else {
            m_recordingStatus->noteRefusal(refusal);
        }
    }
}

bool
CommandBridge::recordingSessionLive(dsd_app_recording_view* view) const {
    // A view stays readable after its run ends, so whether a session is on comes from the host (through the status).
    if (m_recordingStatus != nullptr && !m_recordingStatus->sessionActive()) {
        noteRecording(QStringLiteral("no-session"));
        return false;
    }
    // Until the run is up the view is the last run's, whose folder and mode say nothing of this session's. A
    // process's first run publishes none until then: that session is starting too, not missing.
    if (m_recordingStatus != nullptr && !m_recordingStatus->runUp()) {
        noteRecording(QStringLiteral("starting"));
        return false;
    }
    if (dsd_app_recording_view_get(view) != 1) {
        noteRecording(QStringLiteral("no-session"));
        return false;
    }
    return true;
}

bool
CommandBridge::setRecordCalls(bool on, bool unknownOnly) const {
    dsd_app_recording_view view{};
    if (!recordingSessionLive(&view)) {
        return false;
    }
    // Never record-on into a session the app gave no folder: it would record outside the app's storage.
    if (on && view.journal_named == 0U) {
        noteRecording(QStringLiteral("storage-unavailable"));
        return false;
    }
    // Nothing either way in a session recording to the user's own static WAV: a start would close it, and so would a
    // stop. The preference applies at the next start.
    if (view.mode == DSD_APP_RECORDING_MODE_STATIC) {
        noteRecording(QStringLiteral("static-wav"));
        return false;
    }
    // On: unknown-only first, so the first call recorded already follows it, and the start last. Off: the stop first,
    // so a refused second submit never leaves the session recording more than it did (unknown-only cleared, the stop
    // refused). The stop is the switch: once it is in, unknown-only follows best effort, as it applies only while
    // calls record and a later start sends it first, and the status keeps describing the stop.
    if (on) {
        const bool sent = accepted(dsd_app_command_set_i32(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET, unknownOnly ? 1 : 0))
                          && accepted(dsd_app_command_submit(DSD_APP_CMD_WAV_START, nullptr, 0));
        if (sent) {
            noteRecording(QString());
        }
        return sent;
    }
    if (!accepted(dsd_app_command_submit(DSD_APP_CMD_WAV_STOP, nullptr, 0))) {
        return false;
    }
    noteRecording(QString());
    if (accepted(dsd_app_command_set_i32(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET, 0)) && m_recordingStatus != nullptr) {
        m_recordingStatus->noteFollowUp(DSD_APP_CMD_WAV_STOP, DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET);
    }
    return true;
}

bool
CommandBridge::setRecordUnknownOnly(bool on) const {
    dsd_app_recording_view view{};
    if (!recordingSessionLive(&view)) {
        return false;
    }
    // Whether the session records each call, a start or stop still applying counted as done.
    const bool pending = view.command_state == DSD_APP_RECORDING_COMMAND_PENDING;
    const bool recording = pending && view.command_id == DSD_APP_CMD_WAV_START  ? true
                           : pending && view.command_id == DSD_APP_CMD_WAV_STOP ? false
                                                                                : view.per_call != 0U;
    if (!recording) {
        return true;
    }
    // Sent, it is the command the status shows applying, or refused (under Signal search), like Record calls.
    const bool sent = accepted(dsd_app_command_set_i32(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET, on ? 1 : 0));
    if (sent) {
        noteRecording(QString());
    }
    return sent;
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
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::resumeScan() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_SCAN_RESUME));
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

namespace {

/* A dynamic setting for the configured default: AUTO with its margin, or NOISE with its quieting. */
bool
submitDynamicSquelch(int mode, int marginDb) {
    if (marginDb < DSD_SQUELCH_MARGIN_MIN_DB || marginDb > DSD_SQUELCH_MARGIN_MAX_DB) {
        return false;
    }
    dsd_app_squelch_setting_payload payload = {};
    payload.mode = mode;
    payload.margin_db = marginDb;
    return accepted(dsd_app_command_submit(DSD_APP_CMD_RTL_SET_SQL_SETTING, &payload, sizeof payload));
}

} // namespace

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::setSquelchAuto(int marginDb) const {
    return submitDynamicSquelch(DSD_SQUELCH_MODE_AUTO, marginDb);
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::setSquelchNoise(int marginDb) const {
    return submitDynamicSquelch(DSD_SQUELCH_MODE_NOISE, marginDb);
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::restoreSquelchLevel() const {
    dsd_app_squelch_view view{};
    if (dsd_app_squelch_view_get(dsd_app_get_latest_opts_snapshot(), dsd_app_get_latest_snapshot(), &view) != 0) {
        return false;
    }
    dsd_app_squelch_setting_payload payload = {};
    payload.mode = DSD_SQUELCH_MODE_LEVEL;
    payload.level = view.configured_level;
    return accepted(dsd_app_command_submit(DSD_APP_CMD_RTL_SET_SQL_SETTING, &payload, sizeof payload));
}

bool
CommandBridge::setNfmBandwidthHz(int hz) const {
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_NFM_BANDWIDTH_SET, static_cast<int32_t>(hz)));
}

bool
CommandBridge::setAmBandwidthHz(int hz) const {
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_AM_BANDWIDTH_SET, static_cast<int32_t>(hz)));
}

bool
CommandBridge::setToneFilter(int mode, const QString& list) const {
    const QByteArray utf8 = list.toUtf8();
    /* A list with a NUL in it would reach the decoder cut short; the setter refuses one too long to carry. */
    if (utf8.contains('\0')) {
        return false;
    }
    return accepted(dsd_app_command_set_tone_filter(static_cast<int32_t>(mode), utf8.constData()));
}

QString
CommandBridge::toneFilterError(int mode, const QString& list) const {
    const QByteArray utf8 = list.toUtf8();
    if (utf8.contains('\0')) {
        return tr("the list holds a NUL character");
    }
    dsd_tone_set set;
    char why[DSD_TONE_LIST_ERROR_SIZE] = "";
    if (dsd_tone_filter_check(mode, utf8.constData(), &set, why, sizeof why) == 0) {
        return {};
    }
    return QString::fromUtf8(why);
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
    QString normalized = session_args_key_value_normalize(type, value);
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

int
CommandBridge::scanRowFieldSquelch() {
    return DSD_SCAN_ROW_FIELD_SQUELCH;
}

int
CommandBridge::scanRowFieldWidth() {
    return DSD_SCAN_ROW_FIELD_WIDTH;
}

int
CommandBridge::scanRowFieldTone() {
    return DSD_SCAN_ROW_FIELD_TONE;
}

int
CommandBridge::scanRowFieldGain() {
    return DSD_SCAN_ROW_FIELD_GAIN;
}

int
CommandBridge::scanRowEditSet() {
    return DSD_SCAN_ROW_EDIT_SET;
}

int
CommandBridge::scanRowEditInherit() {
    return DSD_SCAN_ROW_EDIT_INHERIT;
}

int
CommandBridge::scanRowEditReset() {
    return DSD_SCAN_ROW_EDIT_RESET;
}

int
CommandBridge::squelchModeLevel() {
    return DSD_SQUELCH_MODE_LEVEL;
}

int
CommandBridge::squelchModeAuto() {
    return DSD_SQUELCH_MODE_AUTO;
}

int
CommandBridge::squelchModeNoise() {
    return DSD_SQUELCH_MODE_NOISE;
}

int
CommandBridge::squelchMarginMinDb() {
    return DSD_SQUELCH_MARGIN_MIN_DB;
}

int
CommandBridge::squelchMarginMaxDb() {
    return DSD_SQUELCH_MARGIN_MAX_DB;
}

int
CommandBridge::squelchMarginDefaultDb() {
    return DSD_SQUELCH_MARGIN_DEFAULT_DB;
}

namespace {
QVariantMap
receivePolicyMap(const dsd_receive_policy& policy) {
    QVariantList filters, names;
    for (int kind = 0; kind < DSD_RECEIVE_FILTER_COUNT; kind++) {
        char allow[DSD_RECEIVE_LIST_MAX + 1], block[DSD_RECEIVE_LIST_MAX + 1];
        (void)dsd_receive_set_format(kind, &policy.filters[kind].allow, allow, sizeof allow);
        (void)dsd_receive_set_format(kind, &policy.filters[kind].block, block, sizeof block);
        filters.append(QVariantMap{{"enabled", policy.filters[kind].enabled != 0},
                                   {"allow", QString::fromUtf8(allow)},
                                   {"block", QString::fromUtf8(block)}});
    }
    for (unsigned int i = 0; i < policy.names.count; i++) {
        const auto& rule = policy.names.rules[i];
        char value[32];
        (void)dsd_receive_value_format(rule.kind, rule.value, value, sizeof value);
        names.append(QVariantMap{{"kind", qMin(static_cast<int>(rule.kind), 4)},
                                 {"value", QString::fromUtf8(value)},
                                 {"name", QString::fromUtf8(rule.name)}});
    }
    return {{"filters", filters}, {"names", names}};
}

QString
receivePayload(int kind, bool enabled, const QString& allow, const QString& block, const QVariantList& names,
               dsd_app_receive_payload* out) {
    *out = {};
    out->kind = kind;
    out->enabled = enabled;
    const QByteArray a = allow.toUtf8(), b = block.toUtf8();
    if (a.contains('\0') || b.contains('\0') || a.size() >= static_cast<qsizetype>(sizeof out->allow)
        || b.size() >= static_cast<qsizetype>(sizeof out->block)) {
        return QStringLiteral("The identifier list is too long or contains a NUL character.");
    }
    DSD_MEMCPY(out->allow, a.constData(), static_cast<size_t>(a.size()));
    DSD_MEMCPY(out->block, b.constData(), static_cast<size_t>(b.size()));
    dsd_receive_policy checked{};
    char why[160] = "";
    if (kind == DSD_RECEIVE_FILTER_COUNT) {
        for (const QVariant& item : names) {
            const QVariantMap rule = item.toMap();
            const QByteArray text =
                (rule.value("value").toString() + QLatin1Char('=') + rule.value("name").toString()).toUtf8();
            if (text.contains('\0')
                || dsd_receive_name_add(&checked.names, rule.value("kind").toInt(), text.constData(), why,
                                        sizeof why)) {
                return why[0] ? QString::fromUtf8(why) : QStringLiteral("A name contains a NUL character.");
            }
        }
        if (dsd_receive_names_format(&checked.names, out->names, sizeof out->names) < 0) {
            return QStringLiteral("The naming table is too long.");
        }
    }
    return dsd_receive_edit_parse(&checked, kind, enabled, out->allow, out->block, out->names, why, sizeof why)
               ? QString::fromUtf8(why)
               : QString();
}
} // namespace

QVariantMap
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::receiveContext() const {
    dsd_receive_policy policy{};
    dsd_scan_mode_configured_receive(dsd_app_get_latest_opts_snapshot(), dsd_app_get_latest_snapshot(), &policy);
    return receivePolicyMap(policy);
}

int
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::receiveField(int kind) const {
    return kind >= 0 && kind <= DSD_RECEIVE_FILTER_COUNT ? DSD_SCAN_ROW_FIELD_SLOT << kind : 0;
}

QString
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::receiveError(int kind, bool enabled, const QString& allow, const QString& block,
                            const QVariantList& names) const {
    dsd_app_receive_payload payload{};
    return receivePayload(kind, enabled, allow, block, names, &payload);
}

bool
CommandBridge::setReceive(int kind, bool enabled, const QString& allow, const QString& block, const QVariantList& names,
                          const QVariantMap& row, int action) const {
    dsd_app_receive_payload payload{};
    if (action == DSD_SCAN_ROW_EDIT_SET && !receivePayload(kind, enabled, allow, block, names, &payload).isEmpty()) {
        return false;
    }
    if (row.value("active").toBool()) {
        dsd_app_scan_row_edit_payload edit{};
        edit.scanner = row.value("scanner").toInt();
        edit.session = static_cast<uint32_t>(row.value("session").toULongLong());
        edit.row = row.value("row").toInt();
        edit.mode = row.value("mode").toInt();
        edit.field = receiveField(kind);
        edit.action = action;
        edit.receive = payload;
        const auto target = row.value("target").toString().toUtf8();
        if (target.contains('\0') || target.size() >= static_cast<qsizetype>(sizeof edit.target_id)) {
            return false;
        }
        DSD_MEMCPY(edit.target_id, target.constData(), static_cast<size_t>(target.size()));
        return accepted(dsd_app_command_scan_row_edit(&edit));
    }
    return action == DSD_SCAN_ROW_EDIT_SET
           && accepted(dsd_app_command_submit(DSD_APP_CMD_RECEIVE_SET, &payload, sizeof payload));
}

QVariantMap
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::scanRowContext() const {
    const dsd_opts* opts = dsd_app_get_latest_opts_snapshot();
    dsd_app_scan_row_view view{};
    (void)dsd_app_scan_row_view_get(opts, dsd_app_get_latest_snapshot(), &view);
    /* The tone policy below comes from the options snapshot: only one taken under this row's scope is the row's. */
    if (!view.active || !view.opts_match) {
        return {{"active", false}};
    }
    /* The tone policy the row runs now, its own or the configured one it follows: what a "this channel" tone editor
       opens on. */
    char list[DSD_APP_TONE_FILTER_LIST_SIZE] = "";
    (void)dsd_tone_set_format(&opts->analog_tone_set, list, sizeof list);
    return {{"active", true},
            {"scanner", view.scanner},
            {"session", static_cast<qulonglong>(view.session)},
            {"row", view.row},
            {"mode", static_cast<int>(view.mode)},
            {"target", QString::fromUtf8(view.target_id)},
            {"label", QString::fromUtf8(view.label)},
            {"editable", static_cast<int>(view.editable)},
            {"key", QStringLiteral("%1:%2:%3").arg(view.scanner).arg(view.session).arg(view.row)},
            {"toneMode", opts->analog_tone_filter},
            {"receive", receivePolicyMap(opts->receive)},
            {"toneList", QString::fromUtf8(list)}};
}

bool
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::editScanRow(const QVariantMap& context, int field, int action, const QVariantMap& value) const {
    if (!context.value("active").toBool()) {
        return false;
    }
    dsd_app_scan_row_edit_payload payload = {};
    payload.session = static_cast<uint32_t>(context.value("session").toULongLong());
    payload.scanner = context.value("scanner").toInt();
    payload.row = context.value("row").toInt();
    payload.mode = context.value("mode").toInt();
    payload.field = field;
    payload.action = action;
    payload.squelch_db = value.value("squelchDb").toInt();
    payload.squelch_mode = value.value("squelchMode", static_cast<int>(DSD_SQUELCH_MODE_LEVEL)).toInt();
    payload.squelch_margin_db = value.value("squelchMarginDb").toInt();
    payload.width_hz = value.value("widthHz").toInt();
    payload.tone_mode = value.value("toneMode").toInt();
    payload.gain_db = value.value("gainDb").toInt();
    const QByteArray target = context.value("target").toString().toUtf8();
    const QByteArray list = value.value("toneList").toString().toUtf8();
    /* Text that would reach the decoder cut short, or not fit, is refused here. */
    if (target.contains('\0') || list.contains('\0') || static_cast<size_t>(target.size()) >= sizeof(payload.target_id)
        || static_cast<size_t>(list.size()) >= sizeof(payload.tone_list)) {
        return false;
    }
    std::memcpy(payload.target_id, target.constData(), static_cast<size_t>(target.size()));
    std::memcpy(payload.tone_list, list.constData(), static_cast<size_t>(list.size()));
    return accepted(dsd_app_command_scan_row_edit(&payload));
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
    const QString normalized = session_args_key_value_normalize(type, value);
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

/* One row per item: table_index,kind,start,end,pattern. A listed row (index >= 0) is named by its token, so a range
 * starting at 0 and a pattern row are rows like any; a heard-only item is one non-zero exact ID. */
static bool
write_talkgroup_selection(QTemporaryFile* file, const QVariantList& rows) {
    for (const auto& value : rows) {
        const auto row = value.toMap();
        bool firstOk = false, lastOk = false, indexOk = false;
        const auto first = row.value("first").toUInt(&firstOk);
        const auto last = row.value("last").toUInt(&lastOk);
        const auto index = row.value("index").toInt(&indexOk);
        TalkgroupRowToken token;
        if (!firstOk || !lastOk || !indexOk
            || !makeTalkgroupRowToken(first, last, index, row.value("pattern").toString(), &token)
            || (index < 0 && (!first || first != last))) {
            return false;
        }
        const QByteArray line = QByteArray::number(index) + ',' + QByteArray::number(token.kind) + ','
                                + QByteArray::number(first) + ',' + QByteArray::number(last) + ',' + token.pattern
                                + '\n';
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
