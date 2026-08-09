// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Curated app-control command surface exposed to QML.
 *
 * Commands land in a 128-slot ring that only drains while the engine loop turns, so
 * the UI must not submit while the decoder is stopped.
 */

#ifndef DSD_NEO_SRC_UI_QT_COMMAND_BRIDGE_H_
#define DSD_NEO_SRC_UI_QT_COMMAND_BRIDGE_H_

#include <QObject>

namespace dsd_qt {

class CommandBridge : public QObject {
    Q_OBJECT

  public:
    explicit CommandBridge(QObject* parent = nullptr);
    ~CommandBridge() override;

    /** @brief Toggle audio mute. @return true when the command was accepted. */
    Q_INVOKABLE bool toggleMute() const;

    /** @brief Hold decoding on one talkgroup; 0 releases the hold. */
    Q_INVOKABLE bool holdTalkgroup(unsigned int talkgroup) const;

    /** @brief Lock out the target currently active on @p slot (0 or 1). */
    Q_INVOKABLE bool lockoutSlot(int slot) const;

    /** @brief Forget every encrypted-target lockout. */
    Q_INVOKABLE bool clearEncLockouts() const;

    /** @brief Retune the radio front end. */
    Q_INVOKABLE bool tuneHz(unsigned int hz) const;

    /**
     * @brief Retune from a spectrum tap.
     *
     * Distinct from tuneHz(): taps coalesce only with taps, the decoder resets
     * its auto-modulation votes and call state so it re-acquires on the new
     * frequency, and the engine refuses the tune outright while trunking owns
     * the tuner.
     */
    Q_INVOKABLE bool manualTuneHz(unsigned int hz) const;

    /** @brief Set tuner gain in dB (0 selects automatic gain). */
    Q_INVOKABLE bool setTunerGain(int gain_db) const;

    /** @brief Cycle the event-history display mode. */
    Q_INVOKABLE int cycleHistoryMode() const;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_COMMAND_BRIDGE_H_ */
