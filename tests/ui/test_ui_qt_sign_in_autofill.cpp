// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: SignInAutofill, the relay that describes the RadioReference
 * sign-in form to the platform's password manager support. A recording
 * DecoderHost stands in for the Android host; the form is two real TextInputs
 * in a real window under a clipping item; the account checks run through the
 * real RadioReferenceModel with a transport serving the captured getUserData
 * reply.
 *
 * The window runs at a device pixel ratio of 1.5 (QT_SCALE_FACTOR, set before
 * the application exists), so a rect scaled twice, or not at all, cannot pass;
 * the ratio-1 and other fractional cases run on the mapping function itself.
 *
 * House style, as test_ui_qt_radio_reference_model.cpp: no Qt6::Test, failures
 * counted with expect(). The timers are real, so every "it happened" wait has a
 * generous deadline, a "not yet" right after a timer starts is checked at once
 * with the timer seen running, and a "not before" is a lower bound on when it
 * happened: a timer never fires early. */

#include <QByteArray>
#include <QByteArrayView>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QLatin1String>
#include <QList>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>
#include <memory>
#include <qtenvironmentvariables.h>
#include <stdio.h>
#include <stdlib.h>
#include "../test_support/qt_test_paths.h"

#include "app_prefs.h"
#include "decoder_host.h"
#include "imported_files_model.h"
#include "radio_reference_model.h"
#include "sign_in_autofill.h"

#ifndef DSD_NEO_TEST_RR_FIXTURE_DIR
#error "DSD_NEO_TEST_RR_FIXTURE_DIR must be defined by the build"
#endif

namespace {

int g_failures = 0;

constexpr QLatin1String kPassword("SENTINEL_PW_c41");
constexpr QLatin1String kUsername("SENTINEL_USER_8e0");
constexpr QLatin1String kAppKey("SENTINEL_KEY_5a7");

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

void
expect_int(const char* what, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %d, want %d\n", what, got, want);
        g_failures++;
    }
}

void
expect_str(const char* what, const QString& got, const QString& want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", what, got.toUtf8().constData(),
                    want.toUtf8().constData());
        g_failures++;
    }
}

void
expect_rect(const char* what, const QRect& got, const QRect& want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %d,%d %dx%d, want %d,%d %dx%d\n", what, got.x(), got.y(), got.width(),
                    got.height(), want.x(), want.y(), want.width(), want.height());
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* Recording host                                                             */
/* ------------------------------------------------------------------------- */

enum OpKind { OpBegin, OpField, OpRequest, OpCommit, OpEnd };

struct Op {
    OpKind kind = OpBegin;
    int token = 0;
    int field = 0;
    QRect rect;
    bool visible = false;
    bool focused = false;
    QString value;
    QString username;
    QString password;
    qint64 atMs = 0;
};

class FakeHost : public dsd_qt::DecoderHost {
  public:
    QList<Op> ops;
    QElapsedTimer clock;
    /* A host with password manager support, as Android's; false stands in for the desktop. */
    bool supported = true;

    FakeHost() { clock.start(); }

    bool
    signInAutofillSupported() const override {
        return supported;
    }

    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {}

    void
    signInAutofillBegin(int token) override {
        record(OpBegin, token);
    }

    void
    signInAutofillField(int token, int field, const QRect& windowPixels, bool visible, bool focused,
                        const QString& structureValue) override {
        Op& op = record(OpField, token);
        op.field = field;
        op.rect = windowPixels;
        op.visible = visible;
        op.focused = focused;
        op.value = structureValue;
    }

    void
    signInAutofillRequest(int token, int field, const QRect& windowPixels) override {
        Op& op = record(OpRequest, token);
        op.field = field;
        op.rect = windowPixels;
    }

    void
    signInAutofillCommit(int token, const QString& username, const QString& password) override {
        Op& op = record(OpCommit, token);
        /* Deep copies: the caller scrubs its own once this returns. */
        op.username = QString(username.constData(), username.size());
        op.password = QString(password.constData(), password.size());
    }

    void
    signInAutofillEnd(int token) override {
        record(OpEnd, token);
    }

    /* What the Android host does with a password manager's fill. */
    void
    fill(int token, bool hasUsername, QString& username, bool hasPassword, QString& password) {
        deliverSignInFill(token, hasUsername, username, hasPassword, password);
    }

    int
    count(OpKind kind) const {
        int n = 0;
        for (const Op& op : ops) {
            n += op.kind == kind ? 1 : 0;
        }
        return n;
    }

    int
    fieldCount(int field) const {
        int n = 0;
        for (const Op& op : ops) {
            n += op.kind == OpField && op.field == field ? 1 : 0;
        }
        return n;
    }

    /* The last op of @p kind (for @p field, when non-zero); a default Op with none. */
    Op
    last(OpKind kind, int field = 0) const {
        for (int i = ops.size() - 1; i >= 0; i--) {
            if (ops[i].kind == kind && (field == 0 || ops[i].field == field)) {
                return ops[i];
            }
        }
        return Op();
    }

  private:
    Op&
    record(OpKind kind, int token) {
        Op op;
        op.kind = kind;
        op.token = token;
        op.atMs = clock.elapsed();
        ops.append(op);
        return ops.last();
    }
};

/* ------------------------------------------------------------------------- */
/* Transport: every request is an account check                               */
/* ------------------------------------------------------------------------- */

struct Fake {
    QByteArray userData;
    QByteArray lastRequest;
};

int
fake_perform(void* ctx, const dsd_rr_request* req, dsd_rr_response* resp) {
    auto* fake = static_cast<Fake*>(ctx);
    DSD_MEMSET(resp, 0, sizeof(*resp));
    fake->lastRequest = QByteArray(req->body, static_cast<int>(req->body_len));
    const QByteArray& payload = fake->userData;
    resp->body = static_cast<char*>(malloc(static_cast<size_t>(payload.size()) + 1U));
    if (resp->body == nullptr) {
        resp->status = DSD_RR_ERR_NOMEM;
        return -1;
    }
    DSD_MEMCPY(resp->body, payload.constData(), static_cast<size_t>(payload.size()));
    resp->body[payload.size()] = '\0';
    resp->body_len = static_cast<size_t>(payload.size());
    resp->http_status = 200;
    resp->status = DSD_RR_OK;
    return 0;
}

QByteArray
read_user_data() {
    QFile file(QString::fromUtf8(DSD_NEO_TEST_RR_FIXTURE_DIR) + QStringLiteral("/user_data.xml"));
    if (!file.open(QIODevice::ReadOnly)) {
        expect("the getUserData fixture opens", false);
        return QByteArray();
    }
    return file.readAll();
}

/* ------------------------------------------------------------------------- */
/* Event loop                                                                 */
/* ------------------------------------------------------------------------- */

void
wait_ms(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

template <typename Pred>
bool
wait_until(Pred done, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return done();
}

void
pump(const dsd_qt::RadioReferenceModel& model) {
    if (!wait_until([&model] { return !model.busy(); }, 15000)) {
        expect("the account check settled before the deadline", false);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

/* ------------------------------------------------------------------------- */
/* The form                                                                   */
/* ------------------------------------------------------------------------- */

/* Scene layout: the clipper spans y 40..240; the content scrolls inside it.
 * The username's scene rect is (10, 61) 200x30, the password's (10, 110) 200x30. */
QByteArray
form_qml() {
    return QByteArrayLiteral("import QtQuick\n"
                             "Window {\n"
                             "    width: 320; height: 480; visible: true\n"
                             "    Item { objectName: \"clipper\"; y: 40; width: 320; height: 200;\n"
                             "        clip: true\n"
                             "        Item { objectName: \"content\"; width: 320; height: 600\n"
                             "            TextInput { objectName: \"username\"; x: 10; y: 21;\n"
                             "                width: 200; height: 30 }\n"
                             "            TextInput { objectName: \"password\"; x: 10; y: 70;\n"
                             "                width: 200; height: 30;\n"
                             "                echoMode: TextInput.Password }\n"
                             "        }\n"
                             "    }\n"
                             "    Rectangle { objectName: \"elsewhere\"; y: 400; width: 40;\n"
                             "        height: 40; activeFocusOnTab: true }\n"
                             "}\n");
}

/* The username and password rects at the window's 1.5 ratio, content unscrolled. */
const QRect kUsernamePixels(15, 91, 300, 46);
const QRect kPasswordPixels(15, 165, 300, 45);

struct Form {
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QQuickWindow* window = nullptr;
    QQuickItem* content = nullptr;
    QQuickItem* clipper = nullptr;
    QQuickItem* username = nullptr;
    QQuickItem* password = nullptr;
    QQuickItem* elsewhere = nullptr;

    Form() {
        QQmlComponent component(&engine);
        component.setData(form_qml(), QUrl());
        root.reset(component.create());
        if (!root) {
            DSD_FPRINTF(stderr, "%s\n", component.errorString().toUtf8().constData());
            expect("the form loads", false);
            return;
        }
        window = qobject_cast<QQuickWindow*>(root.get());
        content = root->findChild<QQuickItem*>(QStringLiteral("content"));
        clipper = root->findChild<QQuickItem*>(QStringLiteral("clipper"));
        username = root->findChild<QQuickItem*>(QStringLiteral("username"));
        password = root->findChild<QQuickItem*>(QStringLiteral("password"));
        elsewhere = root->findChild<QQuickItem*>(QStringLiteral("elsewhere"));
        window->requestActivate();
        (void)wait_until([this] { return QGuiApplication::focusWindow() == window; });
        expect("the form's window is active", QGuiApplication::focusWindow() == window);
    }

    bool
    ok() const {
        return window != nullptr && content != nullptr && clipper != nullptr && username != nullptr
               && password != nullptr && elsewhere != nullptr;
    }
};

bool
sampler_active(const dsd_qt::SignInAutofill& relay) {
    const auto* timer = relay.findChild<QTimer*>(QStringLiteral("signInAutofillSampler"));
    return timer != nullptr && timer->isActive();
}

bool
settle_active(const dsd_qt::SignInAutofill& relay) {
    const auto* timer = relay.findChild<QTimer*>(QStringLiteral("signInAutofillSettle"));
    return timer != nullptr && timer->isActive();
}

/**
 * @brief The relay over the real model, wired to the fake transport with usable credentials.
 *
 * Declaration order is load-bearing: the relay goes first, so it clears its sinks while the model and host still
 * exist; the transport outlives the model, whose destructor joins the client's worker.
 */
struct Harness {
    FakeHost host;
    dsd_qt::AppPrefs prefs;
    dsd_qt::ImportedFilesModel library{&host};
    Fake fake;
    dsd_rr_transport transport{&fake_perform, &fake};
    dsd_qt::RadioReferenceModel model{&prefs, &library, &host};
    dsd_qt::SignInAutofill relay{&host, &model, &prefs};
    int filledCalls = 0;
    int filledToken = 0;
    bool filledUsername = false;
    bool filledPassword = false;

    Harness() {
        fake.userData = read_user_data();
        prefs.setRrUsername(kUsername);
        prefs.setRrAppKey(kAppKey);
        model.setPassword(kPassword);
        model.setTransportForTests(&transport);
        QObject::connect(&relay, &dsd_qt::SignInAutofill::filled, [this](int token, bool hasUser, bool hasPass) {
            filledCalls++;
            filledToken = token;
            filledUsername = hasUser;
            filledPassword = hasPass;
        });
    }

    /* One account check, run to its verdict. */
    void
    check() {
        model.checkAccount();
        pump(model);
    }
};

/* ------------------------------------------------------------------------- */
/* Cases                                                                      */
/* ------------------------------------------------------------------------- */

void
test_window_pixels(void) {
    const QRectF scene(10.0, 61.0, 200.0, 30.0);
    expect_rect("ratio 1 keeps the scene rect", dsd_qt::SignInAutofill::toWindowPixels(scene, 1.0),
                QRect(10, 61, 200, 30));
    /* 61 * 1.5 = 91.5 and 91 * 1.5 = 136.5: rounded outward to 91..137. */
    expect_rect("ratio 1.5 scales once and rounds outward", dsd_qt::SignInAutofill::toWindowPixels(scene, 1.5),
                kUsernamePixels);
    /* 26.25..551.25 by 160.125..238.875. */
    expect_rect("ratio 2.625 too", dsd_qt::SignInAutofill::toWindowPixels(scene, 2.625), QRect(26, 160, 526, 79));
    expect("an empty scene rect is an empty QRect",
           dsd_qt::SignInAutofill::toWindowPixels(QRectF(10.0, 10.0, 0.0, 30.0), 1.5).isNull());
}

void
test_tokens(void) {
    Form form;
    if (!form.ok()) {
        return;
    }
    FakeHost host;
    dsd_qt::SignInAutofill relay(&host, nullptr, nullptr);

    const int first = relay.begin();
    expect("the first token is positive", first > 0);
    expect_int("begin opens a session", host.count(OpBegin), 1);
    expect_int("with its token", host.last(OpBegin).token, first);
    expect_int("and there was nothing to end", host.count(OpEnd), 0);

    const int second = relay.begin();
    expect("tokens only grow", second > first);
    expect_int("begin retires the live session", host.count(OpEnd), 1);
    expect_int("the older one", host.last(OpEnd).token, first);
    expect("before the new one opens",
           host.ops.size() == 3 && host.ops[1].kind == OpEnd && host.ops[2].kind == OpBegin);

    relay.end(first);
    expect_int("a retired token's end is ignored", host.count(OpEnd), 1);
    relay.track(first, 1, form.username, true);
    relay.note(first, 1, QStringLiteral("stale"));
    expect_int("as are its track and note", host.count(OpField), 0);

    relay.track(second, 3, form.username, true);
    expect_int("an unknown field is ignored", host.count(OpField), 0);

    relay.end(second);
    expect_int("end retires the live session", host.count(OpEnd), 2);
    expect_int("its own token", host.last(OpEnd).token, second);
    relay.end(second);
    expect_int("only once", host.count(OpEnd), 2);
    relay.track(second, 1, form.username, true);
    relay.note(second, 1, QStringLiteral("late"));
    expect_int("an ended session takes no track or note", host.count(OpField), 0);
    expect("and samples nothing", !sampler_active(relay));

    const int third = relay.begin();
    expect("a token after an end still grows", third > second);
    expect_int("with nothing live, begin ends nothing", host.count(OpEnd), 2);
}

void
test_rects_and_clipping(void) {
    Form form;
    if (!form.ok()) {
        return;
    }
    expect("the window runs at ratio 1.5", qAbs(form.window->devicePixelRatio() - 1.5) < 1e-9);
    FakeHost host;
    dsd_qt::SignInAutofill relay(&host, nullptr, nullptr);
    expect("no session, no sampling", !sampler_active(relay));
    const int token = relay.begin();

    relay.track(token, 1, form.username, true);
    expect_int("track sends the field at once", host.fieldCount(1), 1);
    Op op = host.last(OpField, 1);
    expect_int("under the session's token", op.token, token);
    expect_rect("the username's rect, scaled once", op.rect, kUsernamePixels);
    expect("visible", op.visible);
    expect("not focused", !op.focused);
    expect("no structure value yet", op.value.isEmpty());

    relay.track(token, 2, form.password, true);
    op = host.last(OpField, 2);
    expect_rect("the password's rect", op.rect, kPasswordPixels);
    expect("sampling while a field is shown", sampler_active(relay));

    relay.note(token, 1, QStringLiteral("typed_name"));
    op = host.last(OpField, 1);
    expect_int("note re-sends the username", host.fieldCount(1), 2);
    expect_str("with its text", op.value, QStringLiteral("typed_name"));
    relay.note(token, 2, QStringLiteral("secret"));
    expect_int("the password takes no note", host.fieldCount(2), 1);

    host.ops.clear();
    wait_ms(350);
    expect_int("a still form sends nothing", host.count(OpField), 0);

    /* Scrolled by 30: the username's top 9 px are under the clipper's edge at y 40. */
    form.content->setY(-30.0);
    expect("the scroll is sampled", wait_until([&host] { return host.fieldCount(1) > 0 && host.fieldCount(2) > 0; }));
    expect_rect("the username is cut by its clipping ancestor", host.last(OpField, 1).rect, QRect(15, 60, 300, 32));
    expect("and still visible", host.last(OpField, 1).visible);
    expect_str("the structure value rides along", host.last(OpField, 1).value, QStringLiteral("typed_name"));
    expect_rect("the password moved whole", host.last(OpField, 2).rect, QRect(15, 120, 300, 45));
    expect("the password's structure value is empty", host.last(OpField, 2).value.isEmpty());

    /* Scrolled out of the clipper: clipped to nothing is not visible. */
    host.ops.clear();
    form.content->setY(-100.0);
    expect("the scroll out is sampled",
           wait_until([&host] { return host.fieldCount(1) > 0 && host.fieldCount(2) > 0; }));
    expect("a field clipped to nothing is not visible", !host.last(OpField, 1).visible);
    expect("and has an empty rect", host.last(OpField, 1).rect.isEmpty());
    expect("an edge-only overlap is nothing too", !host.last(OpField, 2).visible);
    expect("sampling goes on, so the fields can come back", sampler_active(relay));

    /* Past the window's right edge: 250..450 cut at 320. */
    host.ops.clear();
    form.content->setY(0.0);
    form.username->setX(250.0);
    expect("the move is sampled", wait_until([&host] { return host.fieldCount(1) > 0; }));
    expect_rect("cut by the window", host.last(OpField, 1).rect, QRect(375, 91, 105, 46));

    host.ops.clear();
    form.username->setX(10.0);
    form.username->setVisible(false);
    expect("hiding is sampled", wait_until([&host] { return host.fieldCount(1) > 0; }));
    expect("a hidden item is not visible", !host.last(OpField, 1).visible);
    form.username->setVisible(true);

    /* The form hides both fields: sent at once, and sampling stops. */
    host.ops.clear();
    relay.track(token, 1, form.username, false);
    relay.track(token, 2, form.password, false);
    expect("the card hiding marks the username not visible", !host.last(OpField, 1).visible);
    expect("and the password", !host.last(OpField, 2).visible);
    expect("nothing shown, nothing sampled", !sampler_active(relay));
    relay.track(token, 1, form.username, true);
    expect("shown again", host.last(OpField, 1).visible);
    expect_rect("where it is", host.last(OpField, 1).rect, kUsernamePixels);
    expect("sampling again", sampler_active(relay));

    relay.end(token);
    expect("end stops the sampler", !sampler_active(relay));
    host.ops.clear();
    form.content->setY(-30.0);
    wait_ms(350);
    expect_int("and nothing is sent after it", static_cast<int>(host.ops.size()), 0);
}

/* The time from @p fromMs to the next request after it, or -1 with none. */
qint64
request_after(const FakeHost& host, qint64 fromMs) {
    for (const Op& op : host.ops) {
        if (op.kind == OpRequest && op.atMs >= fromMs) {
            return op.atMs - fromMs;
        }
    }
    return -1;
}

void
test_settle(void) {
    Form form;
    if (!form.ok()) {
        return;
    }
    FakeHost host;
    dsd_qt::SignInAutofill relay(&host, nullptr, nullptr);
    const int token = relay.begin();
    relay.track(token, 1, form.username, true);
    relay.track(token, 2, form.password, true);

    /* Focus, then stillness: one request after the settle wait. */
    qint64 focusAt = host.clock.elapsed();
    form.username->forceActiveFocus();
    expect("focus is sent at once", host.last(OpField, 1).focused);
    /* Checked at once: the wait has only just started, so nothing can have fired. The lower bound below proves it
     * lasts. */
    expect("focus starts the settle wait", host.count(OpRequest) == 0 && settle_active(relay));
    expect("then one", wait_until([&host] { return host.count(OpRequest) == 1; }));
    Op op = host.last(OpRequest);
    expect_int("for the username", op.field, 1);
    expect_int("under the session's token", op.token, token);
    expect_rect("at its rect", op.rect, kUsernamePixels);
    expect("no sooner than the wait", request_after(host, focusAt) >= 300);
    wait_ms(500);
    expect_int("once per focus entry", host.count(OpRequest), 1);
    /* No re-anchoring: a later move of the focused field asks again for nothing. */
    form.content->setY(-5.0);
    expect("the move is sampled",
           wait_until([&host] { return host.last(OpField, 1).rect.y() != kUsernamePixels.y(); }));
    wait_ms(500);
    expect_int("a move after the request asks for nothing", host.count(OpRequest), 1);
    form.content->setY(0.0);
    expect("the way back is sampled", wait_until([&host] { return host.last(OpField, 1).rect == kUsernamePixels; }));

    /* Movement restarts the wait. */
    form.elsewhere->forceActiveFocus();
    expect("focus loss is sent", !host.last(OpField, 1).focused);
    focusAt = host.clock.elapsed();
    form.password->forceActiveFocus();
    /* A short gap, so a stall here cannot outlast the first wait: a wait not restarted would end 250 ms after the
     * move at most, which the lower bound below tells apart. */
    wait_ms(50);
    const qint64 movedAt = host.clock.elapsed();
    form.content->setY(-5.0);
    /* The sample that reports the move restarts the wait (or the first wait, ending first, finds the move and
     * restarts itself); right after that nothing can have fired. */
    expect("the move is sampled",
           wait_until([&host] { return host.last(OpField, 2).rect == QRect(15, 157, 300, 46); }));
    expect("a move restarts the wait", request_after(host, focusAt) < 0 && settle_active(relay));
    expect("the request comes once the field is still", wait_until([&host] { return host.count(OpRequest) == 2; }));
    op = host.last(OpRequest);
    expect_int("for the password", op.field, 2);
    expect_rect("at where it settled", op.rect, QRect(15, 157, 300, 46));
    expect("a full wait after the move", request_after(host, movedAt) >= 300);

    /* Focus lost before the wait ends: no request. */
    form.username->forceActiveFocus();
    expect("the focus starts a wait", settle_active(relay));
    form.elsewhere->forceActiveFocus();
    expect("leaving the field stops it", !settle_active(relay));
    wait_ms(600);
    expect_int("leaving the field cancels its request", host.count(OpRequest), 2);

    /* Focused but hidden when the wait ends: none until it shows and settles. */
    form.username->forceActiveFocus();
    relay.track(token, 1, form.username, false);
    wait_ms(600);
    expect_int("a hidden field is not requested", host.count(OpRequest), 2);
    relay.track(token, 1, form.username, true);
    expect("shown, it is requested after the wait", wait_until([&host] { return host.count(OpRequest) == 3; }));
    expect_int("the username", host.last(OpRequest).field, 1);

    /* end stops a wait in flight. */
    form.elsewhere->forceActiveFocus();
    form.password->forceActiveFocus();
    relay.end(token);
    expect("end stops the settle wait", !settle_active(relay));
    wait_ms(600);
    expect_int("no request after end", host.count(OpRequest), 3);
}

void
test_commit(void) {
    Form form;
    if (!form.ok()) {
        return;
    }
    Harness h;
    h.check();
    expect_int("no session, no commit", h.host.count(OpCommit), 0);

    const int token = h.relay.begin();
    h.check();
    expect_int("a session whose form never showed commits nothing", h.host.count(OpCommit), 0);
    h.relay.track(token, 1, form.username, false);
    h.relay.track(token, 2, form.password, false);
    h.check();
    expect_int("nor one tracking only hidden fields", h.host.count(OpCommit), 0);

    /* What the fields show is not what was checked. */
    form.username->setProperty("text", QStringLiteral("typed_other"));
    h.relay.track(token, 1, form.username, true);
    h.relay.track(token, 2, form.password, true);
    h.relay.note(token, 1, QStringLiteral("typed_other"));
    h.check();
    expect_int("a verified check commits", h.host.count(OpCommit), 1);
    Op op = h.host.last(OpCommit);
    expect_int("under the live token", op.token, token);
    expect_str("the username the check sent", op.username, kUsername);
    expect_str("the password the check sent", op.password, kPassword);

    /* The card hides while the screen stays up: a check still commits. */
    h.relay.track(token, 1, form.username, false);
    h.relay.track(token, 2, form.password, false);
    h.check();
    expect_int("a hidden card still commits", h.host.count(OpCommit), 2);

    h.relay.end(token);
    h.check();
    expect_int("an ended session commits nothing", h.host.count(OpCommit), 2);

    /* Submitted under one session, verified under the next: the save belongs to neither form. */
    const int first = h.relay.begin();
    h.relay.track(first, 1, form.username, true);
    h.model.checkAccount();
    const int second = h.relay.begin();
    h.relay.track(second, 1, form.username, true);
    pump(h.model);
    expect_int("a check submitted under another session commits nothing", h.host.count(OpCommit), 2);

    /* Submitted with no session at all, verified once a form is up. */
    h.relay.end(second);
    h.model.checkAccount();
    const int third = h.relay.begin();
    h.relay.track(third, 1, form.username, true);
    pump(h.model);
    expect_int("a check submitted with no session commits nothing", h.host.count(OpCommit), 2);

    h.check();
    expect_int("a check submitted under the live session still commits", h.host.count(OpCommit), 3);
    expect_int("under that session", h.host.last(OpCommit).token, third);
}

/* A fill's two strings, owned by the caller the way the Android host owns its copies. */
struct FillStrings {
    QString username;
    QString password;
};

void
test_fill(void) {
    Form form;
    if (!form.ok()) {
        return;
    }
    Harness h;
    const int token = h.relay.begin();
    h.relay.track(token, 1, form.username, true);
    h.relay.track(token, 2, form.password, true);
    h.host.ops.clear();

    FillStrings both{QStringLiteral("filled_user"), QStringLiteral("filled_pw")};
    h.host.fill(token, true, both.username, true, both.password);
    expect_str("the username goes to the settings", h.prefs.rrUsername(), QStringLiteral("filled_user"));
    expect_int("filled is emitted once", h.filledCalls, 1);
    expect_int("for the token", h.filledToken, token);
    expect("with the username flag", h.filledUsername);
    expect("and the password flag", h.filledPassword);
    expect("the caller's username is scrubbed", both.username.isEmpty());
    expect("and its password", both.password.isEmpty());
    expect_str("the structure value follows the fill", h.host.last(OpField, 1).value, QStringLiteral("filled_user"));
    h.check();
    expect("the model checks the filled password", h.fake.lastRequest.contains("filled_pw"));
    expect_str("and commits the filled pair", h.host.last(OpCommit).username, QStringLiteral("filled_user"));
    expect_str("both of it", h.host.last(OpCommit).password, QStringLiteral("filled_pw"));

    FillStrings user{QStringLiteral("only_user"), QString()};
    h.host.fill(token, true, user.username, false, user.password);
    expect_str("a username-only fill sets the username", h.prefs.rrUsername(), QStringLiteral("only_user"));
    expect_int("and is emitted", h.filledCalls, 2);
    expect("with the username flag", h.filledUsername);
    expect("without the password flag", !h.filledPassword);
    h.check();
    expect("the session password stays", h.fake.lastRequest.contains("filled_pw"));

    FillStrings pass{QString(), QStringLiteral("only_pw")};
    h.host.fill(token, false, pass.username, true, pass.password);
    expect_str("a password-only fill leaves the username", h.prefs.rrUsername(), QStringLiteral("only_user"));
    expect_int("and is emitted", h.filledCalls, 3);
    expect("without the username flag", !h.filledUsername);
    expect("with the password flag", h.filledPassword);
    h.check();
    expect("the model checks the new password", h.fake.lastRequest.contains("only_pw"));

    FillStrings empty{QString(), QString()};
    h.host.fill(token, true, empty.username, true, empty.password);
    expect_int("an empty fill fills nothing", h.filledCalls, 3);
    expect_str("and clears nothing", h.prefs.rrUsername(), QStringLiteral("only_user"));

    const int next = h.relay.begin();
    FillStrings stale{QStringLiteral("stale_user"), QStringLiteral("stale_pw")};
    h.host.fill(token, true, stale.username, true, stale.password);
    expect_str("a stale token's fill applies nothing", h.prefs.rrUsername(), QStringLiteral("only_user"));
    expect_int("and emits nothing", h.filledCalls, 3);
    expect("its strings are scrubbed", stale.username.isEmpty() && stale.password.isEmpty());
    h.relay.track(next, 1, form.username, true);
    h.check();
    expect("nor reaches the model", h.fake.lastRequest.contains("only_pw"));
}

void
test_no_sink(void) {
    FakeHost host;
    FillStrings strings{QStringLiteral("someone"), QStringLiteral("secret")};
    host.fill(1, true, strings.username, true, strings.password);
    expect("with no sink, a fill is scrubbed", strings.username.isEmpty() && strings.password.isEmpty());

    {
        dsd_qt::SignInAutofill relay(&host, nullptr, nullptr);
        (void)relay.begin();
    }
    FillStrings after{QStringLiteral("someone"), QStringLiteral("secret")};
    host.fill(1, true, after.username, true, after.password);
    /* A sink left behind would call into the destroyed relay: a use after free the sanitizer builds catch. */
    expect("a destroyed relay leaves no sink behind", after.username.isEmpty() && after.password.isEmpty());
}

/* A host with no password manager support (the desktop) gets the same calls, which do nothing there, but the relay
 * runs neither timer for it: no 10 Hz wakeups while a sign-in form is shown, and no request wait. */
void
test_unsupported_host_runs_no_timers(void) {
    Form form;
    if (!form.ok()) {
        return;
    }
    FakeHost host;
    host.supported = false;
    dsd_qt::SignInAutofill relay(&host, nullptr, nullptr);
    const int token = relay.begin();
    relay.track(token, 1, form.username, true);
    relay.track(token, 2, form.password, true);
    expect_int("the fields are still described", host.fieldCount(1), 1);
    expect("a shown form is not sampled", !sampler_active(relay));
    form.username->forceActiveFocus();
    expect("focus is still sent", host.last(OpField, 1).focused);
    expect("focus starts no settle wait", !settle_active(relay));
    expect("and still no sampler", !sampler_active(relay));
    relay.end(token);
}

} // namespace

int
main(int argc, char** argv) {
    /* Before the application exists: the window then runs at a fractional ratio. */
    qputenv("QT_SCALE_FACTOR", "1.5");
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-sign-in-autofill-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    /* AppPrefs hardcodes its own QSettings scope, so redirecting the standard paths alone would still write a real
     * profile. */
    QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    test_window_pixels();
    test_tokens();
    test_rects_and_clipping();
    test_settle();
    test_commit();
    test_fill();
    test_no_sink();
    test_unsupported_host_runs_no_timers();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    (void)puts("sign-in autofill: ok");
    return 0;
}
