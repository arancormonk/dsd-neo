// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression test: an Android display-size (density) change reaches the Qt UI.
 *
 * Qt 6.11.3's Android plugin stores a new density and raises no event, so Qt's scale factor moves while the window
 * keeps its old device pixel ratio and logical size and the screen its old logical geometry: after a change on a
 * phone, the UI kept drawing into the top-left part of the screen, or stayed at the new size with text scaled for
 * the old one.
 *
 * The offscreen platform reproduces the silent change exactly: a new logicalBaseDpi is stored without any event
 * (only a warning), and Qt's factor is logicalDpi / logicalBaseDpi, as on Android. The first half drives
 * resync_display_scale() against real windows. The second half drives PresentationScale, the per-tick state machine
 * that decides when to resync, when to hand Qt the Activity's density, and when to rebuild the font table, with a
 * fake platform in place of JNI.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <QByteArray>
#include <QByteArrayView>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMargins>
#include <QObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWindow>
#include <Qt>
#include <QtGlobal>
#include <assert.h>
#include <cmath>
#include <float.h>
#include <qevent.h>
#include <qpa/qplatformnativeinterface.h>
#include <qtenvironmentvariables.h>
#include <stdio.h>
#include <stdlib.h>
#include <utility>

#include "display_scale.h"
#include "presentation_scale.h"

namespace {

using dsd_android::DensityPush;
using dsd_android::PresentationScale;

bool
near(double a, double b, double tolerance = 1e-6) {
    return std::fabs(a - b) <= tolerance;
}

void
drain() {
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
    }
}

// The offscreen plugin's runtime configuration, through its native interface.
using GetConfiguration = QJsonObject (*)(QPlatformNativeInterface*);
using SetConfiguration = void (*)(const QJsonObject&, QPlatformNativeInterface*);

QJsonObject
configuration() {
    QPlatformNativeInterface* native = QGuiApplication::platformNativeInterface();
    auto get = reinterpret_cast<GetConfiguration>(native->nativeResourceForIntegration("configuration"));
    assert(get != nullptr);
    return get(native);
}

void
configure(const QJsonObject& config) {
    QPlatformNativeInterface* native = QGuiApplication::platformNativeInterface();
    auto set = reinterpret_cast<SetConfiguration>(native->nativeResourceForIntegration("setConfiguration"));
    assert(set != nullptr);
    set(config, native);
}

QJsonObject
screen_config(const QString& name, int x, int width, int height, int logical_dpi, int base_dpi) {
    return QJsonObject{{"name", name},
                       {"x", x},
                       {"y", 0},
                       {"width", width},
                       {"height", height},
                       {"logicalDpi", logical_dpi},
                       {"logicalBaseDpi", base_dpi},
                       {"dpr", 1.0}};
}

/* Two screens, as an Android external display would add; Qt's density is global there, so both move together. */
void
configure_screens(int logical_dpi, int base_dpi) {
    QJsonObject config = configuration();
    config["screens"] = QJsonArray{screen_config(QStringLiteral("main"), 0, 800, 800, logical_dpi, base_dpi),
                                   screen_config(QStringLiteral("side"), 800, 600, 400, logical_dpi, base_dpi)};
    configure(config);
}

/** @brief A window that counts what Qt tells it. */
class ProbeWindow : public QWindow {
  public:
    explicit ProbeWindow(QWindow* parent = nullptr) : QWindow(parent) {
        QObject::connect(this, &QWindow::safeAreaMarginsChanged, this, [this](QMargins) { ++safe_area_signals; });
    }

    void
    resetCounts() {
        ratio_events = 0;
        resizes = 0;
        safe_area_signals = 0;
    }

    int ratio_events = 0;
    int resizes = 0;
    int safe_area_signals = 0;
    QSize last_resize;

  protected:
    bool
    event(QEvent* event) override {
        if (event->type() == QEvent::DevicePixelRatioChange) {
            ++ratio_events;
        }
        return QWindow::event(event);
    }

    void
    resizeEvent(QResizeEvent* event) override {
        ++resizes;
        last_resize = event->size();
        QWindow::resizeEvent(event);
    }
};

QScreen*
screen_named(const QString& name) {
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->name() == name) {
            return screen;
        }
    }
    return nullptr;
}

/*
 * Windows of every kind the app can have on Android: the top-level window, a native child, a popup with a transient
 * parent, a Quick window, and one never shown (it must not get a platform window). Native sizes are fixed; the
 * logical sizes follow the factor.
 */
struct Scene {
    ProbeWindow main;
    ProbeWindow child{&main};
    ProbeWindow popup;
    QQuickWindow quick;
    QWindow never_shown;

    explicit Scene(qreal factor) {
        const int w = static_cast<int>(400 / factor);
        const int h = static_cast<int>(300 / factor);
        main.resize(w, h);
        main.show();
        child.setGeometry(0, 0, static_cast<int>(100 / factor), static_cast<int>(80 / factor));
        child.show();
        popup.setFlags(Qt::Popup);
        popup.setTransientParent(&main);
        popup.resize(static_cast<int>(120 / factor), static_cast<int>(60 / factor));
        popup.show();
        quick.resize(w, h);
        quick.show();
        drain();
        main.resetCounts();
        child.resetCounts();
        popup.resetCounts();
    }
};

void
expect_scene(const Scene& scene, qreal factor) {
    // Logical sizes as Qt rounds native geometry: native / factor.
    const QSize main_size(qRound(400 / factor), qRound(300 / factor));
    assert(near(scene.main.devicePixelRatio(), factor));
    assert(near(scene.child.devicePixelRatio(), factor));
    assert(near(scene.popup.devicePixelRatio(), factor));
    assert(near(scene.quick.devicePixelRatio(), factor));
    assert(std::abs(scene.main.last_resize.width() - main_size.width()) <= 1);
    assert(std::abs(scene.main.last_resize.height() - main_size.height()) <= 1);
    assert(std::abs(scene.child.last_resize.width() - qRound(100 / factor)) <= 1);
    assert(std::abs(scene.popup.last_resize.width() - qRound(120 / factor)) <= 1);
    // The Quick content follows the window: this is what the user sees fill the screen again.
    assert(std::fabs(scene.quick.contentItem()->width() - 400 / factor) <= 1.0);
    assert(std::fabs(scene.quick.contentItem()->height() - 300 / factor) <= 1.0);
    assert(scene.never_shown.handle() == nullptr);
}

void
expect_screens(qreal factor) {
    QScreen* main = screen_named(QStringLiteral("main"));
    QScreen* side = screen_named(QStringLiteral("side"));
    assert(main != nullptr && side != nullptr);
    assert(std::abs(main->geometry().width() - qRound(800 / factor)) <= 1);
    assert(std::abs(main->availableGeometry().height() - qRound(800 / factor)) <= 1);
    assert(std::abs(side->geometry().width() - qRound(600 / factor)) <= 1);
    assert(std::abs(side->geometry().height() - qRound(400 / factor)) <= 1);
}

/*
 * A start at factor 1, where Qt's scaling is off: after the silent change even QScreen::devicePixelRatio() stays 1,
 * so only platform_density() shows it, and only the DPI announcement turns scaling on. Runs first: Qt keeps scaling
 * on once it has been turned on.
 */
void
test_inactive_start() {
    configure_screens(96, 96);
    drain();
    Scene scene(1);
    QScreen* main = screen_named(QStringLiteral("main"));
    int geometry_signals = 0;
    int available_signals = 0;
    const QObject scope; // the screen outlives this test; the connections must not
    QObject::connect(main, &QScreen::geometryChanged, &scope,
                     [&geometry_signals](const QRect&) { ++geometry_signals; });
    QObject::connect(main, &QScreen::availableGeometryChanged, &scope,
                     [&available_signals](const QRect&) { ++available_signals; });

    configure_screens(96, 48); // silent: factor 2
    drain();
    assert(near(dsd_android::platform_density(main), 2));
    assert(near(main->devicePixelRatio(), 1));
    assert(near(scene.main.devicePixelRatio(), 1));
    assert(scene.main.resizes == 0 && scene.main.ratio_events == 0);
    assert(main->geometry().width() == 800);

    dsd_android::resync_display_scale();
    drain();
    expect_scene(scene, 2);
    expect_screens(2);
    assert(geometry_signals >= 1 && available_signals >= 1);
    assert(scene.main.ratio_events == 1 && scene.main.resizes == 1);
    assert(scene.child.resizes == 1 && scene.popup.resizes == 1);
    assert(scene.main.safe_area_signals >= 1 && scene.child.safe_area_signals >= 1);

    // Nothing stale: no further resize or ratio change.
    dsd_android::resync_display_scale();
    drain();
    assert(scene.main.ratio_events == 1 && scene.main.resizes == 1);
    assert(scene.child.resizes == 1 && scene.popup.resizes == 1);

    // And back, as `wm density reset` does.
    configure_screens(96, 96);
    dsd_android::resync_display_scale();
    drain();
    expect_scene(scene, 1);
    expect_screens(1);
    assert(scene.main.ratio_events == 2 && scene.main.resizes == 2);
}

/*
 * A start at factor 2, as on a phone: the silent change to 3 moves the live screen ratio at once, while the window's
 * cached ratio and the sizes it was last told stay old.
 */
void
test_active_start() {
    configure_screens(192, 96);
    drain();
    Scene scene(2);
    QScreen* main = screen_named(QStringLiteral("main"));
    assert(near(scene.main.devicePixelRatio(), 2));

    configure_screens(192, 64); // silent: factor 3
    drain();
    assert(near(dsd_android::platform_density(main), 3));
    assert(near(main->devicePixelRatio(), 3));
    assert(near(scene.main.devicePixelRatio(), 2));
    assert(scene.main.resizes == 0);
    assert(std::fabs(scene.quick.contentItem()->width() - 200) <= 1.0);

    dsd_android::resync_display_scale();
    drain();
    expect_scene(scene, 3);
    expect_screens(3);
    assert(scene.main.ratio_events == 1 && scene.main.resizes == 1);

    configure_screens(96, 96);
    dsd_android::resync_display_scale();
    drain();
    expect_scene(scene, 1);
}

void
test_platform_density_without_screen() {
    assert(dsd_android::platform_density(nullptr) == 0);
}

// ------------------------------------------------------------------ PresentationScale ----

/** @brief The app as PresentationScale sees it; pushes complete only when the test says so. */
class FakePlatform final : public PresentationScale::Platform {
  public:
    qreal
    activityDensity() override {
        return activity;
    }

    qreal
    qtDensity() override {
        return qt;
    }

    qreal
    effectiveRatio() override {
        return ratio;
    }

    void
    resync() override {
        ++resyncs;
        if (usable(listener_during_resync)) {
            qtMovesTo(listener_during_resync); // Qt's listener wrote while the announcements were made
            listener_during_resync = 0;
        }
    }

    void
    startDensityPush() override {
        ++pushes;
    }

    dsd_android::FontKey
    fontKey() override {
        ++key_reads;
        return key;
    }

    bool
    fontTable(dsd_android::FontKey* table_key, QVector<qreal>* pixels) override {
        ++table_reads;
        if (table_unreadable) {
            return false;
        }
        *table_key = moved_key.valid() ? moved_key : key;
        pixels->clear();
        for (int sp = 0; sp <= PresentationScale::kFontTableMaxSp; ++sp) {
            // A failed read is 0, the default a failed JNI float call returns.
            pixels->append(near(sp, zero_at_sp) ? 0.0 : sp * key.density_dpi / 160 * key.font_scale);
        }
        return true;
    }

    bool
    keyboardTopPixels(int* pixels) override {
        if (!keyboard_readable) {
            return false;
        }
        *pixels = keyboard;
        return true;
    }

    void
    diagnostic(const QString& line) override {
        diagnostics.append(line);
    }

    // Qt's listener and the push move qt and ratio together, as Qt's factor follows its density.
    void
    qtMovesTo(qreal density) {
        qt = density;
        ratio = density;
    }

    // Android moves first: the Activity's density and the font key.
    void
    androidMovesTo(qreal density) {
        activity = density;
        key.density_dpi = density * 160;
    }

    static bool
    usable(qreal value) {
        return std::isfinite(value) && value > 0;
    }

    qreal activity = 2.625;
    qreal qt = 2.625;
    qreal ratio = 2.625;
    qreal listener_during_resync = 0;
    dsd_android::FontKey key{1.0, 420};
    dsd_android::FontKey moved_key;
    bool table_unreadable = false;
    qreal zero_at_sp = -1;
    bool keyboard_readable = true;
    int keyboard = -1;
    int resyncs = 0;
    int pushes = 0;
    int key_reads = 0;
    int table_reads = 0;
    QStringList diagnostics;
};

qreal
font16(const PresentationScale& scale) {
    qreal pixels = -1;
    assert(scale.fontPixelSize(16, &pixels));
    return pixels;
}

/* The first sample is repaired too: Qt can have moved between creating the screen and the host's first tick. */
void
test_first_sample() {
    PresentationScale scale;
    FakePlatform platform;
    const auto first = scale.tick(platform);
    assert(platform.resyncs == 1 && platform.pushes == 0);
    assert(first.typography && scale.fontRevision() == 1);
    assert(near(font16(scale), 16));
    const auto second = scale.tick(platform);
    assert(!second.typography && !second.keyboard);
    assert(platform.resyncs == 1 && scale.fontRevision() == 1 && platform.table_reads == 1);
}

/* Qt's own listener moved first (the foreground case on most devices): repair, nothing to push. */
void
test_qt_moves_first() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.androidMovesTo(1.875);
    platform.qtMovesTo(1.875);
    const auto changes = scale.tick(platform);
    assert(platform.resyncs == 2 && platform.pushes == 0);
    assert(changes.typography && scale.fontRevision() == 2);
    assert(near(font16(scale), 16));
}

/*
 * Qt's density moved while the announcements were made, and back before the next tick: some announcements used the
 * other value, so the density must not count as synced, and the next tick announces again.
 */
void
test_density_moves_during_resync() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.androidMovesTo(1.875);
    platform.qtMovesTo(1.875);
    platform.listener_during_resync = 3.5;
    (void)scale.tick(platform);
    assert(platform.resyncs == 2);
    platform.qtMovesTo(1.875);
    (void)scale.tick(platform);
    assert(platform.resyncs == 3);
    (void)scale.tick(platform);
    assert(platform.resyncs == 3);

    // And back to the value synced before (1.875 -> 3.5 -> 1.875 during the announcements): the density the next tick
    // reads equals the last synced one, yet some announcements used 3.5, so it must repair again.
    platform.androidMovesTo(3.5);
    platform.qtMovesTo(3.5);
    platform.listener_during_resync = 1.875;
    (void)scale.tick(platform);
    assert(platform.resyncs == 4);
    (void)scale.tick(platform);
    assert(platform.resyncs == 5);
    (void)scale.tick(platform);
    assert(platform.resyncs == 5);
}

/* Android moved and Qt did not (paused listener, or a late one): push once, then repair when Qt takes it. */
void
test_activity_moves_first() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.androidMovesTo(1.875);
    const auto pushing = scale.tick(platform);
    assert(platform.pushes == 1 && scale.pushInFlight());
    assert(platform.resyncs == 1);
    // No table from Android's new pixels and Qt's old ratio while the push is on its way.
    assert(!pushing.typography && scale.fontRevision() == 1);
    assert(near(font16(scale), 16));
    (void)scale.tick(platform);
    assert(platform.pushes == 1); // one in flight at most

    platform.qtMovesTo(1.875);
    scale.pushFinished(DensityPush::Pushed, 1.875, platform);
    assert(!scale.pushInFlight());
    const auto changes = scale.tick(platform);
    assert(platform.resyncs == 2 && platform.pushes == 1);
    assert(changes.typography && scale.fontRevision() == 2);
    assert(near(font16(scale), 16));
    (void)scale.tick(platform);
    assert(platform.pushes == 1 && platform.resyncs == 2);
}

void
test_missing_entry_point_stops_pushing() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.androidMovesTo(1.875);
    (void)scale.tick(platform);
    assert(platform.pushes == 1);
    scale.pushFinished(DensityPush::Unavailable, 0, platform);
    assert(platform.diagnostics.size() == 1);
    for (int i = 0; i < 5; ++i) {
        (void)scale.tick(platform);
    }
    assert(platform.pushes == 1 && platform.diagnostics.size() == 1);
    // Qt's own listener still gets followed, and the fonts with it.
    platform.qtMovesTo(1.875);
    assert(scale.tick(platform).typography);
    assert(platform.resyncs == 2);
}

void
test_push_budget() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.androidMovesTo(1.875);
    for (int i = 0; i < 10; ++i) {
        (void)scale.tick(platform);
        if (scale.pushInFlight()) {
            scale.pushFinished(DensityPush::Pushed, 1.875, platform); // Qt did not take it
        }
    }
    assert(platform.pushes == PresentationScale::kPushBudget);
    assert(platform.diagnostics.size() == 1);
    // Once no push can help, the table follows what Qt has rather than waiting forever.
    assert(scale.fontRevision() == 2);
    // A new display size gets a new budget.
    platform.androidMovesTo(3.5);
    (void)scale.tick(platform);
    assert(platform.pushes == PresentationScale::kPushBudget + 1);
    // No Activity (a Service context): no diagnostic, and the next tick may push again.
    scale.pushFinished(DensityPush::NoActivity, 0, platform);
    assert(platform.diagnostics.size() == 1 && !scale.pushInFlight());
}

/*
 * The push reads the Activity when it runs, so a quick change can make it write another density than the tick saw:
 * the budget is charged to the density written, and the one seen keeps its own.
 */
void
test_push_charged_to_density_written() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.androidMovesTo(1.875);
    for (int i = 0; i < PresentationScale::kPushBudget; ++i) {
        (void)scale.tick(platform);
        assert(scale.pushInFlight());
        scale.pushFinished(DensityPush::Pushed, 3.5, platform); // Android had moved on to 3.5 by then
    }
    // Android reaches 3.5, which Qt was given three times without taking: no further push, one diagnostic.
    platform.androidMovesTo(3.5);
    (void)scale.tick(platform);
    assert(platform.pushes == PresentationScale::kPushBudget && platform.diagnostics.size() == 1);
    // 1.875 was never written: it is still pushed.
    platform.androidMovesTo(1.875);
    (void)scale.tick(platform);
    assert(platform.pushes == PresentationScale::kPushBudget + 1 && scale.pushInFlight());
    scale.pushFinished(DensityPush::Pushed, 1.875, platform);
    // The budget follows the last density written: back to 3.5 is a new target with a new budget.
    platform.androidMovesTo(3.5);
    (void)scale.tick(platform);
    assert(platform.pushes == PresentationScale::kPushBudget + 2);
}

void
test_no_activity_or_screen() {
    PresentationScale scale;
    FakePlatform platform;
    platform.activity = 0;
    (void)scale.tick(platform);
    assert(platform.pushes == 0);
    platform.qt = 0;
    platform.ratio = 0;
    platform.keyboard = 500;
    const int revision = scale.fontRevision();
    const auto changes = scale.tick(platform);
    assert(platform.pushes == 0 && platform.resyncs == 1);
    assert(!changes.typography && scale.fontRevision() == revision);
    // Without a screen the keyboard edge is taken as logical pixels, as before.
    assert(changes.keyboard && near(scale.keyboardTop(), 500));
}

void
test_font_only_change() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.key.font_scale = 1.3;
    const auto changes = scale.tick(platform);
    assert(changes.typography && scale.fontRevision() == 2);
    qreal pixels = 0;
    assert(scale.fontPixelSize(10, &pixels) && near(pixels, 13));
    assert(platform.resyncs == 1 && platform.pushes == 0);
}

/*
 * A failed read used to be cached as a 0-pixel font until the next configuration change. Now the old table stays
 * and the next tick tries again, for the same key too.
 */
void
test_failed_font_read_keeps_previous_table() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.key.font_scale = 1.3;
    for (const bool unreadable : {true, false}) {
        platform.table_unreadable = unreadable;
        platform.zero_at_sp = unreadable ? -1 : 1;
        const auto changes = scale.tick(platform);
        assert(!changes.typography && scale.fontRevision() == 1);
        assert(near(font16(scale), 16)); // the old table still answers
    }
    platform.zero_at_sp = -1;
    assert(scale.tick(platform).typography && scale.fontRevision() == 2);
    assert(near(font16(scale), 16 * 1.3));
    // An unreadable key publishes nothing either.
    platform.key = dsd_android::FontKey{};
    assert(!scale.tick(platform).typography && scale.fontRevision() == 2);
}

/* The first table can fail too: nothing is published until one reads whole. */
void
test_first_table_unreadable() {
    PresentationScale scale;
    FakePlatform platform;
    platform.table_unreadable = true;
    assert(!scale.tick(platform).typography);
    qreal pixels = 0;
    assert(!scale.fontPixelSize(16, &pixels));
    platform.table_unreadable = false;
    assert(scale.tick(platform).typography && near(font16(scale), 16));
}

/* The key moved between the cheap read and the table: publish the table under its own key, rebuild next tick. */
void
test_key_moves_during_table_read() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.key.font_scale = 1.3;
    platform.moved_key = dsd_android::FontKey{1.15, 420};
    assert(scale.tick(platform).typography && scale.fontRevision() == 2);
    platform.moved_key = dsd_android::FontKey{};
    assert(scale.tick(platform).typography && scale.fontRevision() == 3);
    assert(!scale.tick(platform).typography);
}

void
test_keyboard_inset() {
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.keyboard = 1050;
    assert(scale.tick(platform).keyboard && near(scale.keyboardTop(), 400));
    assert(!scale.tick(platform).keyboard);
    // The same physical edge at a new ratio is a new logical edge.
    platform.androidMovesTo(1.875);
    platform.qtMovesTo(1.875);
    assert(scale.tick(platform).keyboard && near(scale.keyboardTop(), 560));
    // An unreadable keyboard keeps the inset the UI has.
    platform.keyboard_readable = false;
    platform.keyboard = 0;
    assert(!scale.tick(platform).keyboard && near(scale.keyboardTop(), 560));
    platform.keyboard_readable = true;
    platform.keyboard = -1;
    assert(scale.tick(platform).keyboard && near(scale.keyboardTop(), -1));
}

void
test_font_lookup_bounds() {
    PresentationScale scale;
    qreal pixels = 0;
    assert(!scale.fontPixelSize(16, &pixels)); // nothing published yet
    FakePlatform platform;
    (void)scale.tick(platform);
    assert(scale.fontPixelSize(12.5, &pixels) && near(pixels, 12.5));
    assert(scale.fontPixelSize(128, &pixels) && near(pixels, 128));
    assert(!scale.fontPixelSize(128.5, &pixels));
    assert(!scale.fontPixelSize(-1, &pixels));
    assert(!scale.fontPixelSize(NAN, &pixels));
    assert(!scale.fontPixelSize(INFINITY, &pixels));
}

/* Built with IEEE semantics (-fno-fast-math) so these hold under the global fast-math option too. */
void
test_non_finite_values() {
    assert(!dsd_android::scale_differs(2.625, 2.625 + 1e-6));
    assert(dsd_android::scale_differs(2.625, 1.875));
    assert(dsd_android::scale_differs(0, 2.625));
    assert(!dsd_android::scale_differs(0, NAN));
    assert(dsd_android::scale_differs(NAN, 2.625));
    assert(dsd_android::scale_differs(INFINITY, 2.625));
    assert(!(dsd_android::FontKey{NAN, 420}.valid()));
    assert(!(dsd_android::FontKey{1.0, INFINITY}.valid()));
    // Non-finite densities from the platform are ignored.
    PresentationScale scale;
    FakePlatform platform;
    (void)scale.tick(platform);
    platform.qt = NAN;
    platform.activity = INFINITY;
    (void)scale.tick(platform);
    assert(platform.resyncs == 1 && platform.pushes == 0);
}

} // namespace

int
main(int argc, char** argv) {
    // Scale overrides from the environment would hide the platform's factor.
    for (const char* name : {"QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_ENABLE_HIGHDPI_SCALING",
                             "QT_SCALE_FACTOR_ROUNDING_POLICY", "QT_USE_PHYSICAL_DPI"}) {
        qunsetenv(name);
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);

    test_platform_density_without_screen();
    test_inactive_start();
    test_active_start();

    test_first_sample();
    test_qt_moves_first();
    test_density_moves_during_resync();
    test_activity_moves_first();
    test_missing_entry_point_stops_pushing();
    test_push_budget();
    test_push_charged_to_density_written();
    test_no_activity_or_screen();
    test_font_only_change();
    test_failed_font_read_keeps_previous_table();
    test_first_table_unreadable();
    test_key_moves_during_table_read();
    test_keyboard_inset();
    test_font_lookup_bounds();
    test_non_finite_values();

    puts("UI_QT_DISPLAY_SCALE: OK");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
