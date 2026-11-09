// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Relays the RadioReference sign-in form to the platform's password manager support.
 *
 * Android's autofill framework cannot see inside a Qt Quick window, so the
 * platform host describes the form to it as a virtual structure: two fields,
 * 1 the username and 2 the password, each with its rect in window pixels, its
 * visibility, its focus and (for the username only) its text. This relay keeps
 * that description current from the QML form and hands the host the
 * operations: begin and end a form session, the field snapshots, a fill
 * request once a focused field has settled, and the commit of a verified
 * sign-in so a password manager can offer to save it.
 *
 * Every session has a token; every call carries it, and a call or fill for any
 * other token is dropped. Credentials never pass through QML: a commit carries
 * the credentials RadioReferenceModel actually verified (its C++ verified
 * sink), and a fill is applied here, to AppPrefs and the model, with QML told
 * only which fields were filled.
 */

#ifndef DSD_NEO_SRC_UI_QT_SIGN_IN_AUTOFILL_H_
#define DSD_NEO_SRC_UI_QT_SIGN_IN_AUTOFILL_H_

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QtGlobal>

class QQuickItem;

namespace dsd_qt {

class AppPrefs;
class DecoderHost;
class RadioReferenceModel;

/** @brief The `signInAutofill` context object; see the file comment. */
class SignInAutofill : public QObject {
    Q_OBJECT

  public:
    /** @brief The virtual structure's field ids. */
    enum Field {
        FieldUsername = 1,
        FieldPassword = 2,
    };

    /** @brief How often the tracked fields' rects are sampled while a form is shown. */
    static constexpr int kSampleIntervalMs = 100;
    /** @brief How long a focused field's rect must stay still before a fill request. */
    static constexpr int kSettleMs = 300;

    /**
     * Installs itself as @p model's verified sink and as @p host's fill sink; the destructor clears both. None of
     * the three is owned. Each holds one sink, so the app makes exactly one relay.
     */
    SignInAutofill(DecoderHost* host, RadioReferenceModel* model, AppPrefs* prefs, QObject* parent = nullptr);
    ~SignInAutofill() override;

    SignInAutofill(const SignInAutofill&) = delete;
    SignInAutofill& operator=(const SignInAutofill&) = delete;

    /**
     * @brief Start a form session for a sign-in form that has just been shown.
     *
     * Retires the live session first (DecoderHost::signInAutofillEnd()), then opens the new one.
     * @return The session's token, larger than every earlier one; 0 once tokens are exhausted, which no call accepts.
     */
    Q_INVOKABLE int begin();

    /** @brief End the session @p token (its form left the screen). Any other token is ignored. */
    Q_INVOKABLE void end(int token);

    /**
     * @brief Follow @p input, the inner TextInput of field @p field, for the session @p token.
     *
     * @p visible is whether the form shows the field; the reported visibility also needs the item visible and some
     * of it on screen. The field is sent at once, then sampled while it is shown.
     */
    Q_INVOKABLE void track(int token, int field, QQuickItem* input, bool visible);

    /** @brief The username field's text is now @p text: the structure value sent for field 1. Field 2 never has one. */
    Q_INVOKABLE void note(int token, int field, const QString& text);

    /**
     * @brief A scene rect in window pixels: scaled by @p ratio (the window's device pixel ratio) once, then rounded
     *        outward. An empty rect maps to an empty QRect.
     */
    static QRect toWindowPixels(const QRectF& sceneRect, qreal ratio);

  Q_SIGNALS:
    /**
     * @brief A fill for the live session @p token was applied: the username to AppPrefs, the password to the model.
     *
     * Credential-free by design; the filled values never reach QML.
     */
    void filled(int token, bool hasUsername, bool hasPassword);

  private:
    /* One tracked field and what was last sent for it. */
    struct Slot {
        QPointer<QQuickItem> input;
        QMetaObject::Connection focusConnection;
        bool shown = false;
        QString value;
        bool sent = false;
        QRect rect;
        bool visible = false;
        bool focused = false;
        /* A fill request went out for the current focus entry. */
        bool requested = false;
    };

    /* A field as it is on screen now. */
    struct Snapshot {
        QRect rect;
        bool visible = false;
        bool focused = false;
    };

    bool isLive(int token) const;
    Slot* slotFor(int field);
    void retire();
    void clearSlot(Slot& slot);
    Snapshot snapshotOf(const Slot& slot) const;
    void refresh(int field, bool force);
    void updateSettle(int field, Slot& slot, const Snapshot& now);
    void updateSampler();
    void sample();
    void settled();
    void onAccountStateChanged();
    void onVerified(const QString& username, const QString& password);
    void onFill(int token, bool hasUsername, const QString& username, bool hasPassword, const QString& password);

    QPointer<DecoderHost> m_host;
    QPointer<RadioReferenceModel> m_model;
    QPointer<AppPrefs> m_prefs;
    QTimer m_sampler;
    QTimer m_settle;
    /* The field the settle wait is for; 0 with none. */
    int m_settleField = 0;
    int m_lastToken = 0;
    /* The live session's token; 0 with none. */
    int m_token = 0;
    /* The live session's form showed a field: only then is a verified sign-in committed. */
    bool m_formShown = false;
    /* The token live when the account check in flight was submitted; 0 with none or once that session ended. A
     * verified check commits only under it. */
    int m_checkToken = 0;
    Slot m_username;
    Slot m_password;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SIGN_IN_AUTOFILL_H_ */
