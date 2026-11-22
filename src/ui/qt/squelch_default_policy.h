// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief When the app's default squelch follows a live decode switch (issue #899).
 *
 * The app gives an analog session the Settings squelch (AppPrefs::squelch()) in its radio spec. A session that starts
 * digital (Explore on Auto) runs Off, as it always has, so a live switch to NFM or AM has to bring the default in, and
 * a switch back to a digital mode has to take it out again, leaving the stream exactly as Off. That is only the app's
 * to do while the squelch is the app's: one the user set this session (the Radio sheet), one a saved system or the
 * Advanced arguments name, or a session whose rows or ranges own the squelch (a scan list, a signal search), is never
 * touched. This object holds who owns the squelch for the running session and answers what a decode switch asks for;
 * CommandBridge::setDecodeMode() sends it as DSD_APP_CMD_RTL_SQL_DEFAULT_FOLLOW, which the decoder applies only where
 * the switch left it and over the squelch the app expects (off going in, the default going out), so a refused switch or
 * a squelch the control API set meanwhile is never overwritten. No QML or decoder dependency, so it is tested on its
 * own.
 *
 * The owner is kept with the accepted start (record(), AppPrefs::acceptedStartSquelch, written on changed()), so a UI
 * recreated over a running session takes it back (adopt()) rather than starting from nothing.
 */

#ifndef DSD_NEO_SRC_UI_QT_SQUELCH_DEFAULT_POLICY_H_
#define DSD_NEO_SRC_UI_QT_SQUELCH_DEFAULT_POLICY_H_

#include <QObject>
#include <QString>

namespace dsd_qt {

class SquelchDefaultPolicy : public QObject {
    Q_OBJECT

  public:
    /** @brief What a decode switch asks the decoder to do with the squelch after it (DSD_APP_SQL_FOLLOW_*). */
    enum class Action {
        None,         ///< Nothing: the squelch is not the app's.
        DefaultIfOff, ///< A switch to NFM or AM: put defaultSetting() in if the configured squelch is off.
        OffIfDefault  ///< A switch to a digital mode: put it off if the configured squelch is defaultSetting().
    };

    explicit SquelchDefaultPolicy(QObject* parent = nullptr);

    /**
     * @brief A session the app built started (SessionArgsBuilder::start()).
     *
     * @param appDefault The Settings squelch the session was built with ("off", "auto+N", "noise+N"). Off, or text
     *        that is none of those, leaves the app nothing to send: the session's squelch is then treated as its own.
     * @param sessionHasOwnSquelch Whether a saved system squelch or an Advanced --squelch names the squelch: the app
     *        never touches it then.
     */
    void sessionStarted(const QString& appDefault, bool sessionHasOwnSquelch);

    /**
     * @brief A session started whose squelch the app's default never follows: a scan list (its rows), a signal
     * search (its ranges), an audio or file source, or Advanced arguments that pick the input themselves.
     */
    void sessionStartedWithoutDefault();

    /** @brief The user changed the squelch this session (Radio sheet, Squelch sheet): the app never sends it again. */
    void userChangedSquelch();

    /** @brief userChangedSquelch(), for QML: every user squelch edit calls it. */
    Q_INVOKABLE void noteUserSquelchChange();

    /**
     * @brief The owner as a record for the accepted start: "app:" and the default ("app:noise+10"), "user", "session",
     * or empty with no session seen.
     */
    QString record() const;

    /**
     * @brief Take the owner of a running session this UI did not start back from its accepted start's record, @p text.
     *
     * Only a record() of the app's own ("app:" and a default that is not off) or the user's is taken as it says;
     * anything else (none, unreadable, a session's) makes the squelch the session's, so nothing is ever sent for it.
     */
    Q_INVOKABLE void adopt(const QString& text);

    /**
     * @brief The decode mode changed live to an analog (@p analog: NFM or AM) or a digital one.
     *
     * While the app owns the squelch, DefaultIfOff for an analog switch and OffIfDefault for a digital one (the decoder
     * then checks the configured squelch); None while the user or the session owns it.
     */
    Action decodeModeChanged(bool analog) const;

    /** @brief The app's default for the running session ("auto+N" or "noise+N"), empty when the app owns none. */
    QString defaultSetting() const;

  Q_SIGNALS:
    /** @brief record() changed: a start, a user edit or an adopt. */
    void changed();

  private:
    enum class Owner {
        None,    ///< No session seen (a fresh object): never send anything.
        App,     ///< The app default follows decode switches.
        User,    ///< The user changed it this session.
        Session, ///< The session's own settings name it.
    };

    void setOwner(Owner owner, const QString& appDefault);

    Owner m_owner = Owner::None;
    QString m_default;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SQUELCH_DEFAULT_POLICY_H_ */
