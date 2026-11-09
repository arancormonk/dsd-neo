// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Keeps the Qt UI's scale in step with Android's display size (density).
 *
 * Qt 6.11.3's Android plugin stores a new display density without telling QGuiApplication, and reads it only from a
 * display listener that is off while the activity is paused. The activity handles every configuration change in place
 * (#782), so nothing else repairs it: the window keeps its old logical size, the screen its old geometry, and the
 * font table its old ratio. PresentationScale runs once per UI tick. It has Qt re-announce screens and windows when
 * Qt's density moves (display_scale.h), hands the Activity's density to Qt when the two differ, and keeps the
 * sp-to-logical-pixel font table and the keyboard inset in step with the ratio. The platform is passed in on each
 * call, so tests drive it without JNI.
 */

#ifndef DSD_NEO_ANDROID_PRESENTATION_SCALE_H_
#define DSD_NEO_ANDROID_PRESENTATION_SCALE_H_

#include <QList>
#include <QtGlobal>

class QString;

namespace dsd_android {

/** @brief How one density push to Qt ended. */
enum class DensityPush {
    Pushed,      ///< Qt was given the Activity's density at the time the push ran.
    NoActivity,  ///< No Activity, or its density was unreadable; nothing was pushed.
    Unavailable, ///< Qt's density entry point is missing (a Qt upgrade renamed or removed it).
};

/** @brief True when two scales differ by more than a relative 1e-4; an unusable value differs from a usable one. */
bool scale_differs(qreal a, qreal b);

/** @brief What the font table depends on: Android's font scale and density. */
struct FontKey {
    qreal font_scale = 0;
    qreal density_dpi = 0;

    bool valid() const;
    bool sameAs(const FontKey& other) const;
};

class PresentationScale {
  public:
    /** @brief What PresentationScale reads and drives; JNI and QPA in the app, a fake in tests. */
    class Platform {
      public:
        virtual ~Platform() = default;
        /** @brief The current Activity's display density; 0 without an Activity or when unreadable. */
        virtual qreal activityDensity() = 0;
        /** @brief The density Qt's Android plugin holds (platform_density()); 0 without a screen. */
        virtual qreal qtDensity() = 0;
        /** @brief The primary screen's effective device pixel ratio; 0 without a screen. */
        virtual qreal effectiveRatio() = 0;
        /** @brief Has Qt re-announce screens and windows (resync_display_scale()). */
        virtual void resync() = 0;
        /** @brief Starts one asynchronous push; its outcome comes back through pushFinished(). */
        virtual void startDensityPush() = 0;
        /** @brief The current font key, cheap enough for every tick; invalid when unreadable. */
        virtual FontKey fontKey() = 0;
        /**
         * @brief One consistent read of the font key and the physical pixels for 0..kFontTableMaxSp sp.
         * False while it cannot be read whole (Android part-way through updating its resources, or a failed call).
         */
        virtual bool fontTable(FontKey* key, QVector<qreal>* pixels) = 0;
        /** @brief The keyboard's top edge in physical pixels (negative while hidden); false when unreadable. */
        virtual bool keyboardTopPixels(int* pixels) = 0;
        /** @brief One host diagnostic line. */
        virtual void diagnostic(const QString& line) = 0;
    };

    /** @brief The notifications one tick owes the UI. */
    struct Changes {
        bool typography = false;
        bool keyboard = false;
    };

    /** @brief The font table covers 0..kFontTableMaxSp sp in whole steps. */
    static constexpr int kFontTableMaxSp = 128;
    /** @brief Pushes of one density that Qt may take without reaching it. */
    static constexpr int kPushBudget = 3;

    /**
     * @brief One UI tick, in this order: follow Qt's density (resync when it moved), push the Activity's density
     * when Qt's differs, then rebuild the font table and the keyboard inset from the ratio read after the resync.
     * The font table waits while a push can still bring Qt to the Activity's density.
     */
    Changes tick(Platform& platform);
    /** @brief The outcome of the push startDensityPush() began, with the density it wrote; on the UI thread. */
    void pushFinished(DensityPush outcome, qreal pushed_density, Platform& platform);

    /** @brief Logical pixels for @p sp from the published table; false when there is none or @p sp is outside it. */
    bool fontPixelSize(qreal sp, qreal* pixels) const;

    int
    fontRevision() const {
        return m_font_revision;
    }

    /** @brief The keyboard's top edge in logical pixels; -1 while it is hidden. */
    qreal
    keyboardTop() const {
        return m_keyboard_top;
    }

    bool
    pushInFlight() const {
        return m_push_in_flight;
    }

  private:
    void followDensity(Platform& platform, qreal density);
    /** @brief Starts a push when one is due; true when the densities are settled (agree, or no push can help). */
    bool considerPush(Platform& platform, qreal activity, qreal qt);
    bool refreshFonts(Platform& platform, qreal ratio);
    bool refreshKeyboard(Platform& platform, qreal ratio);

    // Qt's density at the last completed resync; 0 before the first.
    qreal m_synced_density = 0;
    bool m_push_in_flight = false;
    // Set for the life of the process once Qt's entry point is missing.
    bool m_push_disabled = false;
    // The density the push budget counts, and the pushes of it Qt has not taken.
    qreal m_push_target = 0;
    int m_push_count = 0;
    bool m_budget_reported = false;

    // The published table and the key and ratio it was built for.
    QVector<qreal> m_font_sizes;
    FontKey m_font_key;
    qreal m_font_ratio = 0;
    int m_font_revision = 0;
    qreal m_keyboard_top = -1;
};

} // namespace dsd_android

#endif // DSD_NEO_ANDROID_PRESENTATION_SCALE_H_
