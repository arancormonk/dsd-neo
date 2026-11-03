// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The trunk-scan lists of the running session, each switched on or off from the Scan lists sheet (issue #697).
 *
 * One row per list of the app-control scan list view, in list order. A switch sends DSD_APP_CMD_SCAN_LIST_SET with a
 * request id from the app-control counter, and the row waits until the retained result for that id arrives. What a
 * row shows comes from two places, as the result API asks (commands.h), by the terminal's rule
 * (src/ui/terminal/scan_list_modal.c): the result says what the request did, and the view's lists_seq says what the
 * lists are. A switch that went through shows the state it asked for while the view's lists_seq is behind the result's;
 * a refusal shows the view's state with its reason until the view's lists_seq moves past the lists it described, or a
 * paused refusal until a resume this model saw follow the pause. A new session drops every request and reason.
 *
 * The app-control calls go through a Backend, so a test (and the QML suite) can answer requests itself.
 */

#ifndef DSD_NEO_SRC_UI_QT_SCAN_LIST_TOGGLE_MODEL_H_
#define DSD_NEO_SRC_UI_QT_SCAN_LIST_TOGGLE_MODEL_H_

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>
#include <Qt>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/core/trunk_scan_lists.h>
#include <functional>
#include <stdint.h>

namespace dsd_qt {

class ScanListToggleModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /** @brief Lists can be switched now: the scan runs, is not paused, and has two lists or more. */
    Q_PROPERTY(bool switchable READ switchable NOTIFY stateChanged)
    /** @brief The operator paused the scan: every switch would be refused until it resumes. */
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    /** @brief The lists shown on, and the targets they hold (avoided ones included), as the rows show them. */
    Q_PROPERTY(int listsOn READ listsOn NOTIFY stateChanged)
    Q_PROPERTY(int targetsOn READ targetsOn NOTIFY stateChanged)

  public:
    enum Role {
        NumberRole = Qt::UserRole + 1, /**< 1-based list number, what a switch names */
        NameRole,                      /**< the engine's name: the file name without folder and extension */
        OnRole,                        /**< what the switch shows: the state asked for while it is pending */
        TargetsRole,                   /**< targets the list holds */
        OnAirRole,                     /**< the target on air is one of this list's */
        PendingRole,                   /**< a switch was sent and has no answer yet */
        NoteRole,                      /**< "reason": why the last switch was not applied, or empty */
    };

    /** @brief What the model asks of app-control; tests replace it. */
    struct Backend {
        std::function<uint32_t()> requestNext;
        std::function<int(const dsd_app_scan_list_set_payload&)> submit;
        std::function<int(uint64_t after, dsd_app_scan_list_result* out, int cap)> resultsSince;
        std::function<uint64_t()> latestSequence;
    };

    /** @brief The real app-control calls. */
    static Backend appControlBackend();

    explicit ScanListToggleModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setBackend(Backend backend);

    /** @brief Take the frame's view and every result published since the last call. */
    void refresh(const dsd_app_scan_list_view& view);
    /** @brief No session: no rows, no requests. */
    void clear();

    /**
     * @brief Switch list @p number on or off. False when nothing was sent: no such list, the lists cannot be switched
     * now, the row is still waiting, or the queue refused it (the row then says so).
     */
    Q_INVOKABLE bool setEnabled(int number, bool on);

    int
    count() const {
        return static_cast<int>(m_view.list_count);
    }

    bool
    switchable() const {
        return m_view.toggleable != 0U;
    }

    bool
    paused() const {
        return m_view.paused != 0U;
    }

    int listsOn() const;
    int targetsOn() const;

  Q_SIGNALS:
    void countChanged();
    void stateChanged();

  private:
    enum class Note : uint8_t { None, NotSent, Answer };

    struct Row {
        uint32_t request = 0;    /* the request this row waits on; 0 = none */
        bool want = false;       /* the state that request asked for */
        Note note = Note::None;  /* what the row says once answered */
        int32_t code = 0;        /* the answer's dsd_app_scan_list_set_code, for Note::Answer */
        bool held = false;       /* show heldWant while the view's lists_seq is behind heldSeq */
        bool heldWant = false;   /* the state an answered switch left the list in */
        uint32_t heldSeq = 0;    /* the switch's result's lists_seq */
        uint32_t noteSeq = 0;    /* the lists the note describes: the later of the result's and submitSeq */
        uint32_t submitSeq = 0;  /* the view's lists_seq at the switch */
        bool seenPaused = false; /* a PAUSED note has been shown with the scan paused, so a resume can retire it */
    };

    bool rowOn(int i) const;
    QString noteText(const Row& row) const;
    void takeResults();
    void answer(const dsd_app_scan_list_result& result);
    void expireNotes();
    bool noteExpired(Row& row) const;
    /* Signal what moved since @p before (the rows as they were shown) and @p beforeCount. */
    void announce(const QList<QList<QVariant>>& before, int beforeCount);
    QList<QList<QVariant>> shownRows() const;

    Backend m_backend;
    dsd_app_scan_list_view m_view{};
    uint64_t m_after = 0;
    bool m_cursorSet = false;
    Row m_rows[DSD_TRUNK_SCAN_MAX_LISTS];
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SCAN_LIST_TOGGLE_MODEL_H_ */
