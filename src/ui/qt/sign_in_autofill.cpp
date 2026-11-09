// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "sign_in_autofill.h"

#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSizeF>
#include <Qt>
#include <limits>

#include "app_prefs.h"
#include "decoder_host.h"
#include "radio_reference_model.h"

namespace dsd_qt {

namespace {

/*
 * The input's scene rect, cut by every clipping ancestor and by the window. The input's own box is its geometry, not
 * boundingRect(): a TextInput's bounding rect is the extent of its text (a cursor's width when empty). An ancestor
 * clips to its clipRect(), the area the scene graph clips its children to.
 */
QRectF
clipped_scene_rect(const QQuickItem* input, const QQuickWindow* window) {
    QRectF rect = input->mapRectToScene(QRectF(0.0, 0.0, input->width(), input->height()));
    for (const QQuickItem* ancestor = input->parentItem(); ancestor != nullptr; ancestor = ancestor->parentItem()) {
        if (ancestor->clip()) {
            rect = rect.intersected(ancestor->mapRectToScene(ancestor->clipRect()));
        }
    }
    return rect.intersected(QRectF(QPointF(0.0, 0.0), QSizeF(window->size())));
}

} // namespace

SignInAutofill::SignInAutofill(DecoderHost* host, RadioReferenceModel* model, AppPrefs* prefs, QObject* parent)
    : QObject(parent), m_host(host), m_model(model), m_prefs(prefs), m_sampler(this), m_settle(this) {
    m_sampler.setObjectName(QStringLiteral("signInAutofillSampler"));
    m_sampler.setInterval(kSampleIntervalMs);
    connect(&m_sampler, &QTimer::timeout, this, &SignInAutofill::sample);
    m_settle.setObjectName(QStringLiteral("signInAutofillSettle"));
    m_settle.setSingleShot(true);
    m_settle.setInterval(kSettleMs);
    m_settle.setTimerType(Qt::PreciseTimer);
    connect(&m_settle, &QTimer::timeout, this, &SignInAutofill::settled);

    if (m_model) {
        /* Used during the call only: the model scrubs these once it returns. */
        m_model->setVerifiedSink(
            [this](const QString& username, const QString& password) { onVerified(username, password); });
    }
    if (m_host) {
        m_host->setSignInFillSink(
            [this](int token, bool hasUsername, const QString& username, bool hasPassword, const QString& password) {
                onFill(token, hasUsername, username, hasPassword, password);
            });
    }
}

SignInAutofill::~SignInAutofill() {
    if (m_model) {
        m_model->setVerifiedSink(nullptr);
    }
    if (m_host) {
        m_host->setSignInFillSink(nullptr);
    }
}

QRect
SignInAutofill::toWindowPixels(const QRectF& sceneRect, qreal ratio) {
    if (sceneRect.isEmpty()) {
        return QRect();
    }
    const QRectF pixels(sceneRect.x() * ratio, sceneRect.y() * ratio, sceneRect.width() * ratio,
                        sceneRect.height() * ratio);
    return pixels.toAlignedRect();
}

int
SignInAutofill::begin() {
    retire();
    if (m_lastToken == std::numeric_limits<int>::max()) {
        return 0;
    }
    m_lastToken++;
    m_token = m_lastToken;
    if (m_host) {
        m_host->signInAutofillBegin(m_token);
    }
    return m_token;
}

void
SignInAutofill::end(int token) {
    if (isLive(token)) {
        retire();
    }
}

void
SignInAutofill::track(int token, int field, QQuickItem* input, bool visible) {
    Slot* slot = slotFor(field);
    if (!isLive(token) || slot == nullptr) {
        return;
    }
    if (slot->input.data() != input) {
        disconnect(slot->focusConnection);
        slot->input = input;
        if (input != nullptr) {
            slot->focusConnection = connect(input, &QQuickItem::activeFocusChanged, this, [this, field] {
                refresh(field, false);
                updateSampler();
            });
        }
    }
    slot->shown = visible;
    m_formShown = m_formShown || (visible && input != nullptr);
    refresh(field, true);
    updateSampler();
}

void
SignInAutofill::note(int token, int field, const QString& text) {
    if (!isLive(token) || field != FieldUsername) {
        return;
    }
    m_username.value = text;
    refresh(FieldUsername, true);
}

bool
SignInAutofill::isLive(int token) const {
    return token > 0 && token == m_token;
}

SignInAutofill::Slot*
SignInAutofill::slotFor(int field) {
    if (field == FieldUsername) {
        return &m_username;
    }
    if (field == FieldPassword) {
        return &m_password;
    }
    return nullptr;
}

void
SignInAutofill::retire() {
    if (m_token == 0) {
        return;
    }
    const int old = m_token;
    m_token = 0;
    m_formShown = false;
    m_sampler.stop();
    m_settle.stop();
    m_settleField = 0;
    clearSlot(m_username);
    clearSlot(m_password);
    if (m_host) {
        m_host->signInAutofillEnd(old);
    }
}

void
SignInAutofill::clearSlot(Slot& slot) {
    disconnect(slot.focusConnection);
    slot = Slot();
}

SignInAutofill::Snapshot
SignInAutofill::snapshotOf(const Slot& slot) const {
    Snapshot now;
    const QQuickItem* input = slot.input.data();
    if (input == nullptr) {
        return now;
    }
    now.focused = input->hasActiveFocus();
    const QQuickWindow* window = input->window();
    if (!slot.shown || window == nullptr || !input->isVisible()) {
        return now;
    }
    now.rect = toWindowPixels(clipped_scene_rect(input, window), window->devicePixelRatio());
    now.visible = !now.rect.isEmpty();
    return now;
}

void
SignInAutofill::refresh(int field, bool force) {
    Slot* slot = slotFor(field);
    if (m_token == 0 || slot == nullptr) {
        return;
    }
    const Snapshot now = snapshotOf(*slot);
    const bool changed =
        !slot->sent || now.rect != slot->rect || now.visible != slot->visible || now.focused != slot->focused;
    updateSettle(field, *slot, now);
    slot->sent = true;
    slot->rect = now.rect;
    slot->visible = now.visible;
    slot->focused = now.focused;
    if ((changed || force) && m_host) {
        /* The password's structure value is always empty. */
        const QString value = field == FieldUsername ? slot->value : QString();
        m_host->signInAutofillField(m_token, field, now.rect, now.visible, now.focused, value);
    }
}

/* Compares @p now with what was last sent for the field, so it runs before refresh() records it. */
void
SignInAutofill::updateSettle(int field, Slot& slot, const Snapshot& now) {
    if (now.focused && !slot.focused) {
        /* A new focus entry: it gets its own request. */
        slot.requested = false;
        m_settleField = field;
        m_settle.start();
    } else if (!now.focused && slot.focused) {
        slot.requested = false;
        if (m_settleField == field) {
            m_settle.stop();
            m_settleField = 0;
        }
    } else if (now.focused && !slot.requested && now.rect != slot.rect) {
        /* Still moving (the keyboard, a scroll to the field): wait for it to stop. */
        m_settleField = field;
        m_settle.start();
    }
}

void
SignInAutofill::updateSampler() {
    const bool wanted =
        m_token != 0
        && ((!m_username.input.isNull() && m_username.shown) || (!m_password.input.isNull() && m_password.shown));
    if (!wanted) {
        m_sampler.stop();
    } else if (!m_sampler.isActive()) {
        m_sampler.start();
    }
}

void
SignInAutofill::sample() {
    refresh(FieldUsername, false);
    refresh(FieldPassword, false);
    updateSampler();
}

void
SignInAutofill::settled() {
    const int field = m_settleField;
    m_settleField = 0;
    Slot* slot = slotFor(field);
    if (m_token == 0 || slot == nullptr || slot->requested) {
        return;
    }
    const Snapshot now = snapshotOf(*slot);
    if (now.rect != slot->rect || now.visible != slot->visible || now.focused != slot->focused) {
        /* It changed since the last sample: send that, and wait again if it is still focused. */
        refresh(field, false);
        return;
    }
    if (!now.focused || !now.visible) {
        return;
    }
    slot->requested = true;
    if (m_host) {
        m_host->signInAutofillRequest(m_token, field, now.rect);
    }
}

void
SignInAutofill::onVerified(const QString& username, const QString& password) {
    if (m_token == 0 || !m_formShown || !m_host) {
        return;
    }
    m_host->signInAutofillCommit(m_token, username, password);
}

void
SignInAutofill::onFill(int token, bool hasUsername, const QString& username, bool hasPassword,
                       const QString& password) {
    if (!isLive(token)) {
        return;
    }
    /* An empty value fills nothing: it would only clear what the person entered. */
    const bool applyUsername = hasUsername && !username.isEmpty() && m_prefs;
    const bool applyPassword = hasPassword && !password.isEmpty() && m_model;
    if (!applyUsername && !applyPassword) {
        return;
    }
    if (applyUsername) {
        m_prefs->setRrUsername(username);
        /* What that change set off may have ended the session. */
        if (!isLive(token)) {
            return;
        }
        m_username.value = username;
        refresh(FieldUsername, true);
    }
    if (applyPassword) {
        m_model->setPassword(password);
        if (!isLive(token)) {
            return;
        }
    }
    Q_EMIT filled(token, applyUsername, applyPassword);
}

} // namespace dsd_qt
