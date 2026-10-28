// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Test capture for the Android teardown stage markers (src/ui/qt/teardown_trace.h, issue #782).
 *
 * While a TeardownCapture lives, stage markers are on: each record is kept, and each clock reading is kTickMs after
 * the one before, so a stage that reads the clock twice reports exactly kTickMs.
 */

#ifndef DSD_NEO_TESTS_UI_TEARDOWN_TRACE_CAPTURE_H_
#define DSD_NEO_TESTS_UI_TEARDOWN_TRACE_CAPTURE_H_

#include <QString>
#include <QStringList>
#include <stdint.h>
#include "teardown_trace.h"

class TeardownCapture {
  public:
    static constexpr uint64_t kTickMs = 7;

    TeardownCapture() {
        lines().clear();
        dsd_qt::teardownTraceInstall(&hooks());
    }

    ~TeardownCapture() { dsd_qt::teardownTraceInstall(nullptr); }

    TeardownCapture(const TeardownCapture&) = delete;
    TeardownCapture& operator=(const TeardownCapture&) = delete;

    /** What the stages logged; kept after the capture ends, so a test can show nothing more arrived. */
    static QStringList&
    lines() {
        static QStringList captured;
        return captured;
    }

  private:
    static void
    log(const QString& line) {
        lines().append(line);
    }

    static uint64_t
    now() {
        static uint64_t clock = 1000;
        clock += kTickMs;
        return clock;
    }

    static const dsd_qt::TeardownTraceHooks&
    hooks() {
        static const dsd_qt::TeardownTraceHooks installed{&log, &now};
        return installed;
    }
};

#endif /* DSD_NEO_TESTS_UI_TEARDOWN_TRACE_CAPTURE_H_ */
