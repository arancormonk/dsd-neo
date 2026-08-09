// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "command_bridge.h"

#include <QChar>
#include <QLatin1String>
#include <QList>
#include <QStringList>
#include <Qt>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <stdint.h>

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
CommandBridge::clearEncLockouts() const {
    return accepted(dsd_app_command_action(DSD_APP_CMD_ENC_LOCKOUT_CLEAR));
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
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_MOD_SET, (modulation != 0) ? 1 : 0));
}

bool
CommandBridge::setDecodeMode(int mode) const {
    return accepted(dsd_app_command_set_i32(DSD_APP_CMD_DECODE_MODE_SET, static_cast<int32_t>(mode)));
}

int
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::decodeModeForFlag(const QString& flag) const {
    /* The chip's flag is the CLI's own text, so the mapping is the CLI's own
     * function rather than a second table here that could drift from it. A chip
     * may carry several flags ("-f1 -mq"); the decode one is whichever is -f. */
    const QStringList tokens = flag.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (!token.startsWith(QLatin1String("-f")) || token.size() != 3) {
            continue;
        }
        dsdneoUserDecodeMode mode = DSDCFG_MODE_UNSET;
        if (dsd_decode_mode_from_cli_preset(token.at(2).toLatin1(), &mode) == 0) {
            return static_cast<int>(mode);
        }
    }
    /* An empty flag is the engine's own default, which is the auto preset. */
    return flag.trimmed().isEmpty() ? static_cast<int>(DSDCFG_MODE_AUTO) : -1;
}

int
// cppcheck-suppress functionStatic -- Q_INVOKABLE members cannot be static (Qt meta-object)
CommandBridge::cycleHistoryMode() const {
    return dsd_app_frontend_history_cycle_mode();
}

} // namespace dsd_qt
