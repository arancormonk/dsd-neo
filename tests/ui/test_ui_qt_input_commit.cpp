// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief InputCommit: composing (preedit) text committed at action boundaries (issue #718).
 *
 * A real TextInput in a real window gets composing text the way an Android
 * keyboard sends it mid-word: a QInputMethodEvent carrying only a preedit
 * string. The offscreen platform has no input method to finish the word, so
 * these cases run the helper's own fallback, the path the QML suites rely on.
 */

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "input_commit.h"

namespace {

void
expect(bool condition, int line) {
    if (!condition) {
        (void)std::fprintf(stderr, "input commit assertion failed at line %d\n", line);
        std::abort();
    }
}

#define check(condition) expect(!!(condition), __LINE__)

QString
text_of(const QObject* field) {
    return field->property("text").toString();
}

QString
preedit_of(const QObject* field) {
    return field->property("preeditText").toString();
}

void
compose(QObject* field, const QString& preedit) {
    QInputMethodEvent event(preedit, {});
    QCoreApplication::sendEvent(field, &event);
}

} // namespace

int
main(int argc, char** argv) {
    const QGuiApplication app(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "Window {\n"
                                        "    width: 240; height: 120; visible: true\n"
                                        "    property int finished: 0\n"
                                        "    TextInput { objectName: \"field\"; width: 240; height: 40;\n"
                                        "        onEditingFinished: parent.Window.window.finished++ }\n"
                                        "    Rectangle { objectName: \"plain\"; y: 60; width: 40; height: 40;\n"
                                        "        activeFocusOnTab: true }\n"
                                        "}\n"),
                      QUrl());
    std::unique_ptr<QObject> root(component.create());
    if (!root) {
        (void)std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 1;
    }
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    check(window != nullptr);
    window->requestActivate();
    for (int i = 0; i < 200 && QGuiApplication::focusWindow() != window; ++i) {
        QCoreApplication::processEvents();
    }
    check(QGuiApplication::focusWindow() == window);
    auto* field = root->findChild<QQuickItem*>(QStringLiteral("field"));
    auto* plain = root->findChild<QQuickItem*>(QStringLiteral("plain"));
    check(field != nullptr);
    check(plain != nullptr);

    dsd_qt::InputCommit commit;
    int composing_changes = 0;
    QObject::connect(&commit, &dsd_qt::InputCommit::composingChanged, [&] { ++composing_changes; });

    field->forceActiveFocus();
    check(QGuiApplication::focusObject() == field);
    field->setProperty("text", QStringLiteral("Hello "));
    field->setProperty("cursorPosition", 6);

    /* Without composing text the commit changes nothing. */
    commit.commitComposing();
    check(text_of(field) == QStringLiteral("Hello "));
    check(!commit.composing());
    check(field->hasActiveFocus());
    check(composing_changes == 0);

    /* Mid-word, `text` leaves the word out; the commit puts it in. */
    compose(field, QStringLiteral("Wor"));
    check(preedit_of(field) == QStringLiteral("Wor"));
    check(text_of(field) == QStringLiteral("Hello "));
    check(commit.composing());
    check(composing_changes == 1);
    commit.commitComposing();
    check(text_of(field) == QStringLiteral("Hello Wor"));
    check(preedit_of(field).isEmpty());
    check(!field->property("inputMethodComposing").toBool());
    check(!commit.composing());
    check(composing_changes == 2);
    check(field->hasActiveFocus());
    check(root->property("finished").toInt() == 0);

    /* Idempotent: a second commit has nothing left to add. */
    commit.commitComposing();
    check(text_of(field) == QStringLiteral("Hello Wor"));
    check(composing_changes == 2);

    /* finishEditing commits, then takes focus off the field so editingFinished runs. */
    compose(field, QStringLiteral("ld"));
    check(commit.composing());
    commit.commitComposing(true);
    check(text_of(field) == QStringLiteral("Hello World"));
    check(!field->hasActiveFocus());
    check(root->property("finished").toInt() == 1);
    check(!commit.composing());

    /* finishEditing leaves focus alone on anything that is not a text field. */
    plain->forceActiveFocus();
    check(plain->hasActiveFocus());
    commit.commitComposing(true);
    check(plain->hasActiveFocus());

    /* No focus object: both forms are no-ops. */
    plain->setFocus(false);
    window->contentItem()->setFocus(false);
    commit.commitComposing();
    commit.commitComposing(true);
    check(text_of(field) == QStringLiteral("Hello World"));

    /* Composing state follows the focus object: a field left mid-word for another
     * item is no longer "the focused field composing". */
    field->forceActiveFocus();
    compose(field, QStringLiteral("x"));
    check(commit.composing());
    plain->forceActiveFocus();
    QCoreApplication::processEvents();
    check(!commit.composing());

    std::puts("input commit: ok");
    return 0;
}
