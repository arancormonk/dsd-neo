// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief How the QML engine's garbage collector runs in every DSD-neo process.
 *
 * Since Qt 6.8 the QML collector is incremental: a collection runs in time slices (5 ms by default)
 * between allocations and event-loop turns. Qt 6.11.2 can lose a QML object's function storage this
 * way while the object is still being created (QQmlVMEMetaObject holds that storage through a weak
 * reference only). When the object is a `Connections` element with a `function onX()` handler,
 * QQmlConnections::connectSignalsToMethods() then dereferences null and the process crashes. A Qt
 * build with qtdeclarative 42c76d7acd (QTBUG-148459) null-checks there instead and skips the
 * handler, so a sheet's `onVisibleChanged` cleanup would silently not run.
 *
 * Every Qt since 6.8 runs a collection to completion when QV4_GC_TIMELIMIT is 0, as every collection
 * ran before 6.8. On Qt 6.11.2, eight parallel runs of the session-tools QML suite on a loaded host
 * crashed 14 times in 24 with the default slices and passed 24 of 24 with a limit of 0.
 */

#ifndef DSD_NEO_SRC_UI_QT_QML_GC_POLICY_H_
#define DSD_NEO_SRC_UI_QT_QML_GC_POLICY_H_

#include <QByteArray>
#include <qtenvironmentvariables.h>

namespace dsd_qt {

/**
 * @brief Make QML engines built after this call run each garbage collection to completion.
 *
 * Call it before the first QQmlEngine is constructed (each engine reads the limit once, in its
 * constructor), and before any other thread can read the environment. A QV4_GC_TIMELIMIT that Qt
 * would honour (an integer) is kept, so a developer can still bring the time slices back to look at
 * them; anything else, including unset, becomes 0.
 *
 * @return true when this call set the limit to 0. false when the environment's own value was kept,
 *         or when Qt could not write the environment (then the collector keeps its time slices).
 */
inline bool
applyQmlGcPolicy() {
    bool ok = false;
    (void)qEnvironmentVariableIntValue("QV4_GC_TIMELIMIT", &ok);
    if (ok) {
        return false;
    }
    return qputenv("QV4_GC_TIMELIMIT", QByteArrayLiteral("0"));
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_QML_GC_POLICY_H_ */
