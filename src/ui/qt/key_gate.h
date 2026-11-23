// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_KEY_GATE_H_
#define DSD_NEO_UI_QT_KEY_GATE_H_
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickItem>

namespace dsd_qt {
/*
 * Issue #902: the gate between the keyboard and the controls that can turn unreachable (hidden, covered, a caller's
 * own state), registered as the `DsdNeo` singleton `PressGate`. One application event filter; no object per control.
 * A control takes part by declaring `readonly property bool inReach` (its reachability for the keyboard). The gate
 * reads only the control with the focus: its `inReach` index is looked up as it takes the focus, from its own type,
 * and its change signal is connected only while it has the focus, so a control at rest costs nothing.
 *
 * - Closed: the control with the focus, out of reach, takes no key at all (press, release, shortcut check), a key
 *   already queued included, and no input method event but one: if it was composing a word as it turned unreachable,
 *   the commit that finishes it when the app asks for one (InputCommit::committing()), with no new composing text, no
 *   replacement and no offset, once.
 * - Stray: the control with the focus turning unreachable, taking the focus while unreachable, or losing the focus
 *   while unreachable or disabled (an ancestor disabled under it), is told to Navigation (`strayed`) at once, which
 *   commits an editor's word before anything can publish the edit and hands the focus on; a control losing the focus
 *   to anything but the window leaving is told too (`focusLeft`, once the change has settled), for Qt dropping it to
 *   the window root.
 *
 * Presses in progress are not followed: a tap, drag or held key already under way when a control turns unreachable may
 * still complete on its release, as before (docs/code_map.md).
 */
class PressGate : public QObject {
    Q_OBJECT

  public:
    static PressGate* instance();
    /*
     * For a window that is away (its root holds no active focus) and would hand its focus back to @p from when it
     * returns: hands that stored focus to @p to instead, each focus scope above it holding it, and makes nothing
     * active. Returns whether it did; with the window active, or its focus stored elsewhere, it leaves the focus be.
     */
    Q_INVOKABLE bool passStoredFocus(QQuickItem* from, QQuickItem* to) const;
    /* Lets @p control go of a press it holds, unacted: its own mouse and touch grabs dropped, a key's press cancelled
     * (a cancelled touch, which is what makes a Qt Quick control let go of one). A combo's open list closing under its
     * pressed option. */
    Q_INVOKABLE void cancelPress(QQuickItem* control) const;
    bool eventFilter(QObject* watched, QEvent* event) override;

  Q_SIGNALS:
    /* @p item, with the focus, turned unreachable, took the focus while unreachable, or is losing it while
     * unreachable or disabled. */
    void strayed(QQuickItem* item);
    /* @p item lost the focus (not to the window leaving), told once that change has settled. */
    void focusLeft(QQuickItem* item);

  private Q_SLOTS:
    /* The control with the focus changed its reachability. */
    void recheck();

  private:
    explicit PressGate(QObject* parent = nullptr);
    bool focusInReach() const;
    void setFocusItem(QQuickItem* item, int index);
    void filterFocus(QQuickItem* item, const QEvent* event);
    bool filterClosed(QEvent* event);

    /* The control with the focus (kept while its window is away), its `inReach` property index, its last
     * reachability, and whether it was composing a word as it turned unreachable (the one commit still to let
     * through). */
    QPointer<QQuickItem> m_focus;
    int m_focus_index = -1;
    bool m_focus_in_reach = true;
    bool m_composing = false;
    QMetaObject::Connection m_watch;
};
} // namespace dsd_qt

#endif // DSD_NEO_UI_QT_KEY_GATE_H_
