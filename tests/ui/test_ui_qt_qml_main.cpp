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
#include <dsd-neo/core/state_fwd.h>

#include "qml_test_context.h"

/* Pulled in by the DMR library that backs the imports library's CSV validation.
 * Nothing here decodes anything, so the vocoder descrambler is a stub -- the
 * same arrangement UI_QT_IMPORTED_FILES uses. External linkage is the point: the
 * library's undefined reference is what this definition satisfies, so it cannot
 * be made static. */
void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

QUICK_TEST_MAIN_WITH_SETUP(dsd_neo_qml, Setup)
