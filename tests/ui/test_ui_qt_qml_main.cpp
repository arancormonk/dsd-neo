// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Qt Quick Test entry point for the QML screens under `src/ui/qt/qml`.
 *
 * The screens are loaded from the source tree by absolute path (`uiDir`), not
 * from the app's qrc: they resolve their sibling components and the Theme
 * singleton through their own directory either way, and reading the files
 * directly keeps the test on exactly what ships without linking the frontend's
 * engine, service and audio stack. The test cases are the `tst_*.qml` files in
 * `tests/ui/qml`; the context they run against is in qml_test_context.h.
 */

#include <QtQuickTest>

#include "qml_test_context.h"

QUICK_TEST_MAIN_WITH_SETUP(dsd_neo_qml, Setup)
