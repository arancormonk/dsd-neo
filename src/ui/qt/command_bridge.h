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
#include <QString>

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

    /**
     * @brief Stop trunking and scanner mode from moving the tuner.
     *
     * Idempotent, and deliberately not a toggle: a view only learns that
     * something owns the tuner, never which of the two, so it cannot safely
     * flip either flag on its own. Also tears down the follow state the trunker
     * left behind, so the decoder can acquire whatever it is pointed at next.
     */
    Q_INVOKABLE bool releaseTuner() const;

    /** @brief Set tuner gain in dB (0 selects automatic gain). */
    Q_INVOKABLE bool setTunerGain(int gain_db) const;

    /** @brief Set the RTL power squelch threshold, in dB. */
    Q_INVOKABLE bool setSquelchDb(double db) const;

    /** @brief Set the dongle's crystal correction, in parts per million. */
    Q_INVOKABLE bool setPpm(int ppm) const;

    /**
     * @brief Choose the demodulator: 0 for C4FM, 1 for QPSK, 2 for GFSK.
     *
     * A setter, not the hotkey's cycle: a control showing them as choices has to
     * be able to ask for the one it is not on without guessing at a state the
     * engine may have moved since. Anything outside 0..2 is refused here rather
     * than clamped, so a caller's mistake does not silently land on C4FM.
     */
    Q_INVOKABLE bool setModulation(int modulation) const;

    /**
     * @brief Switch which protocols are decoded, live.
     *
     * @param mode A dsdneoUserDecodeMode. Use decodeModeForFlag() to get one from
     *        the same decode-chip flag a saved system stores, so the mapping from
     *        flag to preset exists once and is the CLI's own.
     */
    Q_INVOKABLE bool setDecodeMode(int mode) const;

    /**
     * @brief The decode mode a `-f<x>` chip flag selects, or -1 when it selects none.
     *
     * "-mq" is a modulation chip, not a decode one, and answers -1: it says how to
     * demodulate, not what to look for.
     */
    Q_INVOKABLE int decodeModeForFlag(const QString& flag) const;

    /** @brief Cycle the event-history display mode. */
    Q_INVOKABLE int cycleHistoryMode() const;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_COMMAND_BRIDGE_H_ */
