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
 * CommandBridge::setDecodeMode() sends it. No QML or decoder dependency, so it is tested on its own.
 */

#ifndef DSD_NEO_SRC_UI_QT_SQUELCH_DEFAULT_POLICY_H_
#define DSD_NEO_SRC_UI_QT_SQUELCH_DEFAULT_POLICY_H_

#include <QObject>
#include <QString>

namespace dsd_qt {

class SquelchDefaultPolicy : public QObject {
    Q_OBJECT

  public:
    /** @brief What a decode switch asks the app to send. */
    enum class Action {
        None,        ///< Leave the squelch as it is.
        PushDefault, ///< Send defaultSetting() (the switch went analog with the app's squelch not in force).
        PushOff      ///< Send Off (the switch went digital with the app's squelch in force).
    };

    explicit SquelchDefaultPolicy(QObject* parent = nullptr);

    /**
     * @brief A session the app built started (SessionArgsBuilder::start()).
     *
     * @param appDefault The Settings squelch the session was built with ("off", "auto+N", "noise+N"). Off, or text
     *        that is none of those, leaves the app nothing to send: the session's squelch is then treated as its own.
     * @param defaultInSpec Whether the session's radio spec carries @p appDefault: it started analog.
     * @param sessionHasOwnSquelch Whether a saved system squelch or an Advanced --squelch / --squelch-db names the
     *        squelch: the app never touches it then.
     */
    void sessionStarted(const QString& appDefault, bool defaultInSpec, bool sessionHasOwnSquelch);

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
     * @brief The decode mode changed live to an analog (@p analog: NFM or AM) or a digital one.
     *
     * While the app owns the squelch: an analog switch with the default not in force asks for PushDefault (and the
     * default is then in force), a digital switch with it in force asks for PushOff (and it no longer is); anything
     * else, and any switch while the user or the session owns the squelch, asks for None.
     */
    Action decodeModeChanged(bool analog);

    /** @brief The squelch PushDefault sends: the running session's app default, empty when there is none. */
    QString defaultSetting() const;

  private:
    enum class Owner {
        None,    ///< No session the app started (a fresh object): never send anything.
        App,     ///< The app default follows decode switches.
        User,    ///< The user changed it this session.
        Session, ///< The session's own settings name it.
    };

    Owner m_owner = Owner::None;
    bool m_applied = false;
    QString m_default;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SQUELCH_DEFAULT_POLICY_H_ */
