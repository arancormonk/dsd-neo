// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Effective talkgroup policy plus the talkgroups heard on the system on air.
 *
 * Refreshed from the single UI poll tick (see ui_controller.h), on the Qt main
 * thread — never from another thread or a second snapshot reader.
 *
 * Issue #903: the heard cards are the viewed owner's remembered talkgroups (HeardTalkgroupStore) and this session's
 * history fragments that owner owns, one card per talkgroup number. The viewed owner is the saved system a call heard
 * now would be owned by (HeardTalkgroupSource::heardOwnerOnAir()): the running saved system, or under a scan list the
 * saved system of the target on air; none for Explore, an operator tune or a target no single saved system owns, whose
 * cards are then the session's fragments no saved system owns. Updates are granular: a changed last heard is one
 * dataChanged, a card heard or forgotten one row insert or remove, and a tick that changes nothing reads nothing.
 */

#ifndef DSD_NEO_SRC_UI_QT_TALKGROUP_LIST_MODEL_H_
#define DSD_NEO_SRC_UI_QT_TALKGROUP_LIST_MODEL_H_

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <array>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <memory>
#include <optional>
#include <stdint.h>
// Qt template conversions require this in some consumers; IWYU's answer depends on instantiation.
#include <type_traits> // IWYU pragma: keep

#include "heard_talkgroup_source.h"

template <class T>
class QSet;

namespace dsd_qt {

class HeardTalkgroupStore;
class TalkgroupCoverage;

class TalkgroupListModel : public QAbstractListModel {
    Q_OBJECT
    // WP-D1: keep 64-bit context lossless across JavaScript's number boundary.
    Q_PROPERTY(QString policyContext READ policyContext NOTIFY policyChanged)
    Q_PROPERTY(unsigned int policyGeneration READ policyGeneration NOTIFY policyChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int notTunedCount READ notTunedCount NOTIFY countChanged)
    Q_PROPERTY(QStringList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(bool allowListMode READ allowListMode NOTIFY policyChanged)
    Q_PROPERTY(bool persistent READ persistent NOTIFY policyChanged)
    /* The decoder's talkgroup list file (empty: none), which makes the list persistent. */
    Q_PROPERTY(QString groupFile READ groupFile NOTIFY policyChanged)
    Q_PROPERTY(QString persistenceScope READ persistenceScope NOTIFY policyChanged)
    Q_PROPERTY(qint64 historySession READ historySession WRITE setHistorySession NOTIFY historySessionChanged)
    /* Issue #903: the saved system whose talkgroups the heard cards are (empty: none, the Explore context). */
    Q_PROPERTY(QString viewedOwner READ viewedOwner NOTIFY viewedOwnerChanged)
    /* Issue #903: the viewed owner learns the talkgroups it hears (it exists with rememberTalkgroups on). */
    Q_PROPERTY(bool remembering READ remembering NOTIFY rememberingChanged)
    /* Issue #903: the viewed owner has a remembered entry (what Clear would remove), learning or not. */
    Q_PROPERTY(bool hasRemembered READ hasRemembered NOTIFY hasRememberedChanged)

  public:
    enum Role {
        IdStartRole = Qt::UserRole + 1,
        IdEndRole,
        IdTextRole,
        NameRole,
        TagsRole,
        ListeningRole,
        ListedRole,
        PriorityRole,
        PreemptRole,
        TableIndexRole, /**< The row's index in the effective table, -1 for a heard talkgroup: part of its edit token. */
        TemporaryAvoidCountRole,
        RecordRole,     /**< The row's record flag (issue #708): off while not listening, as the decoder keeps it. */
        KindRole,       /**< "exact", "range" or "pattern" */
        PatternRole,    /**< A pattern row's canonical text (its ID); empty on other rows. */
        LastHeardRole,  /**< A heard card's last heard (decode-domain wall seconds): the remembered entry's, or this
                            session's newest fragment's when newer; 0 for none and on every listed row. */
        RememberedRole, /**< A heard card the viewed owner remembers (any variant of its number). */
    };

    explicit TalkgroupListModel(QAbstractItemModel* history, QObject* parent = nullptr);
    ~TalkgroupListModel() override;
    TalkgroupListModel(const TalkgroupListModel&) = delete;
    TalkgroupListModel& operator=(const TalkgroupListModel&) = delete;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return static_cast<int>(m_rows.size());
    }

    int
    notTunedCount() const {
        return m_notTunedCount;
    }

    QStringList
    categories() const {
        return m_categories;
    }

    bool
    allowListMode() const {
        return m_allowListMode;
    }

    bool
    persistent() const {
        return m_persistent;
    }

    QString
    groupFile() const {
        return m_groupFile;
    }

    /**
     * @brief The decode session whose history rows count as heard (the history's
     *        `session` role); 0 = the whole log.
     *
     * Selected by the session that logged a row, not by its stamps, which in a
     * replay are the capture's time.
     */
    qint64
    historySession() const {
        return m_historySession;
    }

    QString
    policyContext() const {
        return QString::number(m_contextId);
    }

    unsigned int
    policyGeneration() const {
        return m_generation;
    }

    /** @brief What was last observed of @p id's encryption on the air while @p id's calls were the viewed owner's. */
    Q_INVOKABLE QVariantMap
    observedDecryption(quint32 id) const {
        return m_observedCrypto.value(m_viewedOwner).value(id);
    }

    QString
    viewedOwner() const {
        return m_viewedOwner;
    }

    bool
    remembering() const {
        return m_remembering;
    }

    bool
    hasRemembered() const {
        return m_hasRemembered;
    }

    /** @brief The remembered talkgroups (not owned; it outlives this model, or is null). */
    void setHeardStore(HeardTalkgroupStore* store);

    /** @brief forgetTalkgroupOf() for the viewed owner. */
    Q_INVOKABLE bool forgetTalkgroup(quint32 id);
    /**
     * @brief Forget talkgroup number @p id for @p owner (the owner an edit sheet captured when it opened): every
     * variant the store holds (HeardTalkgroupStore::forget()), and this session's fragments of it heard so far. A later
     * call shows it again. An empty @p owner forgets the Explore context's fragments only.
     * @return false when the store refused the forget (History could not settle its spill): nothing is forgotten.
     */
    Q_INVOKABLE bool forgetTalkgroupOf(const QString& owner, quint32 id);
    /** @brief clearRememberedOf() for the viewed owner. */
    Q_INVOKABLE bool clearRemembered();
    /** @brief Clear everything @p owner remembers, and this session's fragments it heard so far (forgetTalkgroupOf()
     * for every number). @return as forgetTalkgroupOf(). */
    Q_INVOKABLE bool clearRememberedOf(const QString& owner);

    QString
    persistenceScope() const {
        return m_persistenceScope;
    }

    void setHistorySession(qint64 session);
    void refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot);
    /** @brief Drop snapshot state, retaining the session's history selection. */
    void clear();
    /** Re-read the new target without resetting the list view. */
    void invalidateForTarget();

#ifdef DSD_NEO_TEST_HOOKS
    /** @brief How often the listed rows were read from the policy, the store and the history were read for the heard
     * set, and the rows were replaced by a whole-list compare (not a patch of the heard cards that changed). */
    struct WorkCounts {
        int listedReads = 0;
        int rememberedReads = 0;
        int historyReads = 0;
        int wholeListUpdates = 0;
        /* Store entries taken in (a whole read visits each; an observe's change one), and numbers merged into a card. */
        int entriesRead = 0;
        int numbersMerged = 0;
    };

    const WorkCounts&
    workCountsForTest() const {
        return m_work;
    }
#endif

  Q_SIGNALS:
    void countChanged();
    void categoriesChanged();
    void policyChanged();
    void historySessionChanged();
    void viewedOwnerChanged();
    void rememberingChanged();
    void hasRememberedChanged();

  private:
    void updatePersistence(const dsd_opts* opts_snapshot, const dsd_state* snapshot, bool allowListChanged,
                           bool allowListedOnly);
    void observeDecryption(const dsd_state* snapshot);
    /** @brief The owner whose encryption cache the call slot @p slot holds (ring @p ring's epoch @p epoch, @p active or
     * ended) is recorded under, into @p owner: the viewed owner when the call was first seen active; false for an ended
     * call never seen active. */
    bool cryptoOwnerOf(quint64 ring, uint8_t slot, quint64 epoch, bool active, QString* owner);

    struct Row {
        uint32_t idStart = 0; /* 0/0 on a pattern row, which is matched by its pattern, never by these bounds. */
        uint32_t idEnd = 0;
        QString name;
        QString tags;
        bool listening = false;
        bool listed = false;
        int priority = 0;
        bool preempt = false;
        bool record = false;
        int tableIndex = -1;
        qulonglong temporaryAvoidCount = 0;
        uint8_t kind = 0; /* dsd_tg_policy_row_kind */
        QString pattern;
        qint64 lastHeard = 0; /* LastHeardRole */
        bool remembered = false;

        bool
        isPattern() const {
            return !pattern.isEmpty();
        }

        QString
        idText() const {
            if (isPattern()) {
                return pattern;
            }
            return idStart == idEnd ? QString::number(idStart) : QStringLiteral("%1–%2").arg(idStart).arg(idEnd);
        }

        /** @brief This row's value for @p role (a Roles value); none for any other role. */
        QVariant value(int role) const;
        /** @brief value() for the roles the policy entry gives (name, tags, listening and the other fields). */
        QVariant policyValue(int role) const;
        /** @brief Whether anything but the bounds differs from @p other. */
        bool fieldsDiffer(const Row& other) const;

        /* The same listed row: kind, bounds and pattern (a heard row has no pattern). */
        bool
        sameRow(const Row& other) const {
            return kind == other.kind && idStart == other.idStart && idEnd == other.idEnd && pattern == other.pattern;
        }
    };

    /** A heard card's merged state: what the store and the history give its number. */
    struct Heard {
        qint64 last = 0;
        bool remembered = false;

        bool
        operator==(const Heard& other) const {
            return last == other.last && remembered == other.remembered;
        }
    };

    static QVector<Row> listedRows(const dsd_state* snapshot, QSet<QString>& categoryTags);
    /** @brief Read what is dirty (the policy's rows, the store, the history) and move the rows to it; with
     * @p newTable (another policy context) by a reset. */
    void readDirty(const dsd_state* snapshot, bool allowListedOnly, bool newTable);
    /** @brief Read the listed rows, their coverage and what an uncovered heard card shows. */
    void readListed(const dsd_state* snapshot, bool allowListedOnly, QSet<QString>& categoryTags);
    /** @brief The heard set read through the history's role names, for a history without HeardTalkgroupSource. */
    QList<HeardTalkgroupRow> heardFromRoles() const;
    /** One heard number's new state: its card, or none. */
    using HeardChanges = QHash<uint32_t, std::optional<Heard>>;

    /** @brief Read the viewed owner's store entries whole (HeardTalkgroupStore::visitEntries()). */
    void readRemembered();
    /** @brief hasRemembered() from the remembered numbers held now, signalled when it changes. */
    void noteHasRemembered();
    /** @brief Take in entry @p key (present with @p last, or gone); @return its number (0: none). */
    uint32_t noteRemembered(quint64 key, bool present, qint64 last);
    /** @brief Look up the entries an observe named (one store lookup each), adding their numbers to @p named. */
    void patchRemembered(QSet<uint32_t>& named);
    /** @brief Read the history's heard set, adding the numbers whose fragments moved to @p named. */
    void readTransient(QSet<uint32_t>& named);
    /** @brief Number @p id's card now: its remembered entries and this session's fragments, less what a forget or
     * clear hid; none when neither is left. */
    std::optional<Heard> heardOf(uint32_t id) const;
    /** @brief The heard set: the remembered numbers and this session's (heardOf() for each). */
    QHash<uint32_t, Heard> mergeHeard() const;
    /** @brief A card for heard number @p id. */
    Row heardRow(uint32_t id, const Heard& heard, const dsd_state* snapshot) const;
    /** @brief The listed rows and a card for each uncovered number of @p heard, in list order. */
    QVector<Row> wholeList(const QHash<uint32_t, Heard>& heard, const dsd_state* snapshot) const;
    /** @brief Apply @p changes to the heard cards row by row, and to the heard set; false (nothing changed) when too
     * many cards come or go for that. */
    bool applyHeard(const HeardChanges& changes, const dsd_state* snapshot);
    /** @brief Sort @p changes into the cards that go, come and change; a number a listed row covers has no card. */
    void sortHeardChanges(const HeardChanges& changes, QVector<uint32_t>& gone, QVector<uint32_t>& added,
                          QVector<uint32_t>& changed) const;
    /** @brief Make the heard set what @p changes say. */
    void takeHeardChanges(const HeardChanges& changes);
    /** @brief The row of heard card @p id, or -1. */
    int heardRowIndex(uint32_t id) const;
    void removeRowAt(int row);
    /** @brief Move to @p rows: by a reset with @p reset (another table), else as diffRows() can. */
    void replaceRows(QVector<Row> rows, bool reset);
    /** @brief Move to @p rows by inserts, removes and changes, keeping the rows both have; false when the rows both
     * have are in another order (nothing changed). */
    bool diffRows(QVector<Row>& rows);
    /** @brief Each of @p rows' old row (by identity, -1 for none) into @p match; false when the old rows it names are
     * not in their old order. */
    bool matchRows(const QVector<Row>& rows, QVector<int>& match) const;
    /** @brief Remove the rows @p drop marks, a run at a time from the end. */
    void dropRows(const QVector<bool>& drop);
    void updateCategories(const QSet<QString>& categoryTags);
    void updateCounts(int oldCount);
    /** @brief The owner a call heard now would have (HeardTalkgroupSource::heardOwnerOnAir()), none without a typed
     * history; @p target takes the scan target it would be heard on (its channel label, and for a -Y row its tuned
     * frequency; none off any scan target). */
    QString ownerOnAir(const dsd_opts* opts_snapshot, const dsd_state* snapshot, HeardTarget* target) const;
    /** @brief View @p owner's remembered talkgroups, and the session's heard on scan target @p target under it. */
    void setViewedOwner(const QString& owner, const HeardTarget& target = HeardTarget());
    /** @brief A forget of @p owner's @p number (0: a clear) hides what History took in by now, there too
     * (HeardTalkgroupSource::heardForgotten()); @return the ingest serial it hides through, 0 without a typed history. */
    quint64 hideHeardThroughNow(const QString& owner, uint32_t number);
    /** @brief Drop @p owner's heard cards from the list now (a forget or clear for the viewed owner). */
    void dropHeard(const QString& owner, const QSet<uint32_t>& ids, bool all);

    QPointer<QAbstractItemModel> m_history;
    /* m_history's typed read, when it has one; only used while m_history is alive. */
    HeardTalkgroupSource* m_heardSource = nullptr;
    int m_tgRole = -1;
    int m_sessionRole = -1;
    int m_kindRole = -1;
    /* The history's "gi" role (who a call was to: 1 a radio, a private call, whose target is no talkgroup); -1 when the
     * history has none, and then every target counts, as before. */
    int m_giRole = -1;
    QVector<Row> m_rows;
    QStringList m_categories;
    int m_notTunedCount = 0;
    bool m_allowListMode = false;
    bool m_persistent = false;
    QString m_groupFile;
    qint64 m_historySession = 0;
    uint64_t m_contextId = 0;
    unsigned int m_generation = 0;
    /* Issue #903. The viewed owner (viewedOwner()), the scan target on air (the session's heard cards are that
       target's), and the store the remembered cards come from. */
    QPointer<HeardTalkgroupStore> m_store;
    QString m_viewedOwner;
    HeardTarget m_viewedTarget;
    bool m_remembering = false;
    bool m_hasRemembered = false;
    /* What must be read again: the policy's rows, the viewed owner's store entries, the history's fragments. */
    bool m_listedDirty = true;
    /* The policy context the listed rows were read from (invalidateForTarget() keeps it; clear() drops it). */
    uint64_t m_shownContextId = 0;
    bool m_rememberedDirty = true;
    bool m_transientDirty = true;
    /* The listed rows in list order, their coverage, and what an uncovered heard card shows. */
    QVector<Row> m_listed;
    std::unique_ptr<TalkgroupCoverage> m_coverage;
    bool m_heardListening = true;
    bool m_heardRecord = true;
    /* The viewed owner's remembered numbers (HeardTalkgroupStore::policyId()) and their newest last heard, the
       entries behind each (key -> last), and the entries an observe changed since they were read. */
    QHash<uint32_t, qint64> m_remembered;
    QHash<uint32_t, QHash<quint64, qint64>> m_rememberedKeys;
    QVector<quint64> m_rememberedChanged;
    /* This session's numbers the viewed owner owns, with their newest time and ingest serial. */
    QHash<uint32_t, QPair<qint64, quint64>> m_transient;
    /* The heard set the rows show (mergeHeard()). */
    QHash<uint32_t, Heard> m_heard;
    /* Per owner: the ingest serial each forgotten number, and a clear, hid this session's fragments through. */
    QHash<QString, QHash<uint32_t, quint64>> m_forgotten;
    QHash<QString, quint64> m_clearedThrough;
    /* Per viewed owner: what was last observed of each talkgroup's encryption. */
    QHash<QString, QHash<quint32, QVariantMap>> m_observedCrypto;

    /* Per call slot: the call last seen on the air (its ring and epoch) and the owner it was seen under. */
    struct CryptoCall {
        bool known = false;
        quint64 ring = 0U;
        quint64 epoch = 0U;
        QString owner;
    };

    std::array<CryptoCall, 2> m_cryptoCalls;
    QString m_persistenceScope = QStringLiteral("session");
#ifdef DSD_NEO_TEST_HOOKS
    mutable WorkCounts m_work;
#endif
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_TALKGROUP_LIST_MODEL_H_ */
