// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Issue #782: ScreenReaderState follows Qt's accessibility activation, as the Android bridge and a desktop
 * assistive technology turn it on and off, without polling: off at start when nothing is attached, on and off again
 * through QAccessible::setActive() (what the offscreen platform does for a platform bridge), each change one
 * activeChanged, delivered on the object's thread. */

#include <QAccessible>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QObject>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include "screen_reader_state.h"

namespace {
int failures = 0;

void
check(bool ok, const char* label) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++failures;
    }
}

void
settle() {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
}
} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    check(!QAccessible::isActive(), "no assistive technology at start");
    dsd_qt::ScreenReaderState state;
    int changes = 0;
    QObject::connect(&state, &dsd_qt::ScreenReaderState::activeChanged, [&]() { ++changes; });
    check(!state.active(), "off when Qt's accessibility is off");

    QAccessible::setActive(true);
    check(QAccessible::isActive(), "the offscreen platform takes the activation");
    settle();
    check(state.active() && changes == 1, "on once Qt's accessibility is on");

    QAccessible::setActive(true);
    settle();
    check(changes == 1, "a repeated activation is not a change");

    QAccessible::setActive(false);
    settle();
    check(!state.active() && changes == 2, "off again");

    // Made while it is on: it starts on.
    QAccessible::setActive(true);
    dsd_qt::ScreenReaderState late;
    check(late.active(), "a state made while a screen reader is on starts on");
    QAccessible::setActive(false);
    settle();
    check(!late.active(), "and follows it off");

    if (failures) {
        return EXIT_FAILURE;
    }
    std::puts("UI_QT_SCREEN_READER_STATE: ok");
    return EXIT_SUCCESS;
}
