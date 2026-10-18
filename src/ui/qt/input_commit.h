// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Commits an input method's composing (preedit) text at action boundaries.
 *
 * An Android keyboard holds the word being typed as preedit text, which
 * TextInput.text leaves out until the keyboard finishes it. A form that reads
 * `text` on Save, Cancel or Back would otherwise save a name without its last
 * word, judge a typed edit unchanged, or reject a field as empty. Native
 * Android's EditText.getText() includes the composing span; this helper gives
 * the QML forms the same answer by finishing the word first.
 *
 * It is called from each control's activate() (the path shared by pointer
 * taps, keys and accessibility actions) and from sheet dismissal and Back, not
 * from a window-wide press filter: a press filter would commit before Qt routes
 * a tap inside the composing text to the keyboard, and break cursor taps,
 * selection handles and drags over a field.
 */

#ifndef DSD_NEO_SRC_UI_QT_INPUT_COMMIT_H_
#define DSD_NEO_SRC_UI_QT_INPUT_COMMIT_H_

#include <QObject>
#include <QPointer>
#include <type_traits> // IWYU pragma: keep -- QPointer conversion traits instantiate in followFocusObject().

namespace dsd_qt {

/**
 * @brief Finishes the focused field's composing text; exposed to QML as `inputCommit`.
 *
 * Construct it after the QGuiApplication exists: it follows the application's
 * focus object to publish `composing`.
 */
class InputCommit : public QObject {
    Q_OBJECT
    /**
     * Whether the focused field holds composing text right now. A button gated
     * on a field's committed text stays tappable while this is true, so a tap
     * can commit the word and then recheck the gate.
     */
    Q_PROPERTY(bool composing READ composing NOTIFY composingChanged)

  public:
    explicit InputCommit(QObject* parent = nullptr);

    bool composing() const;

    /**
     * @brief Commit the focused field's composing text into its `text`.
     *
     * Asks the platform input method to commit first; on Android that finishes
     * composing and delivers the word to the focus item. When no platform input
     * method took the commit (offscreen and test platforms) and the focus item
     * still reports composing text, the item is sent that text as a commit and
     * the input method is reset so no stale composing state is left behind.
     * Idempotent, and a no-op without composing text.
     *
     * @param finishEditing Also take focus off a focused text field (and hide
     *        the keyboard), so fields that publish on editingFinished update
     *        their drafts before the action reads them.
     */
    Q_INVOKABLE void commitComposing(bool finishEditing = false);

  Q_SIGNALS:
    void composingChanged();

  private:
    /* Invokable so the focus object's inputMethodComposingChanged() can reach it by meta-method. */
    Q_INVOKABLE void updateComposing();
    void followFocusObject(QObject* object);

    QPointer<QObject> m_focus;
    QMetaObject::Connection m_focus_connection;
    bool m_composing = false;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_INPUT_COMMIT_H_ */
