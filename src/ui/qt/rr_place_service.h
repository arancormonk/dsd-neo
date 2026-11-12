// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RadioReference place names for saved systems, and their "Update from RadioReference" action.
 *
 * An import stores only RadioReference ids in a saved system's `place` (rr_site_place()). This service turns them into
 * names: getStateInfo(stid) answers a state's name and its counties' names, which it keeps for the process and applies
 * to every saved row that names those ids (SavedSystemsModel::applyPlaceNames()); rows added later take them from the
 * same cache before their save (SavedSystemsModel::setPlaceNameCache()). Update from RadioReference re-reads the saved
 * sites themselves (getTrsDetails and getTrsSites per system), refreshing site names, positions and place ids.
 *
 * Isolated from RadioReferenceModel on purpose: its own client, its own one-request-at-a-time queue and its own
 * accounting, so a lookup never touches the import screen's batch, `busy`, `systemChanged`, browse lists, preview or a
 * library refresh. It borrows only the account (RadioReferenceModel::fillAuth()) and follows its credential changes:
 * any change drops the cache and stops every request. Like the model, nothing here holds, logs or shows a credential.
 */

#ifndef DSD_NEO_SRC_UI_QT_RR_PLACE_SERVICE_H_
#define DSD_NEO_SRC_UI_QT_RR_PLACE_SERVICE_H_

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <type_traits> // IWYU pragma: keep -- QPointer conversion checks.

#include <dsd-neo/runtime/radioreference.h>

namespace dsd_qt {

class RadioReferenceModel;
class SavedSystemsModel;

class RrPlaceService : public QObject {
    Q_OBJECT
    /* An Update from RadioReference is running; progressText says how far. */
    Q_PROPERTY(bool updating READ updating NOTIFY updatingChanged)
    /* "Updating %1 of %2" (saved systems, counted as Listen's cards) while updating, else empty. */
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressTextChanged)
    /* The last update's outcome, "Updated %1 systems." with " %1 could not be reached." when any could not. */
    Q_PROPERTY(QString summaryText READ summaryText NOTIFY summaryTextChanged)
    /* Saved systems an update could place that show no place yet, counted as Listen's cards (a site group, or a row in
     * none): those with a row that is a RadioReference site, or has place ids, and has no named state. */
    Q_PROPERTY(int unplacedCount READ unplacedCount NOTIFY unplacedCountChanged)
    /* The notice for unplacedCount: "1 saved system has no place yet", "%1 saved systems have no place yet", or empty
     * at 0. */
    Q_PROPERTY(QString unplacedText READ unplacedText NOTIFY unplacedCountChanged)

  public:
    /** @brief At most this many states per resolveStates() call; the rest of a longer list stay unnamed. */
    static constexpr int kMaxStatesPerCall = 8;

    /**
     * @param rr      The account the requests are sent with; NULL means signed out for good.
     * @param systems The saved systems whose places this names; it gets this service's cache as its place-name fill.
     */
    RrPlaceService(RadioReferenceModel* rr, SavedSystemsModel* systems, QObject* parent = nullptr);
    ~RrPlaceService() override;

    RrPlaceService(const RrPlaceService&) = delete;
    RrPlaceService& operator=(const RrPlaceService&) = delete;

    /** @brief Replace the HTTP transport. Test seam; NULL restores the built-in one. Not Q_INVOKABLE. */
    void setTransportForTests(const dsd_rr_transport* transport);

    /**
     * @brief Name states @p stids (the first kMaxStatesPerCall positive ids, in order) and their counties in every saved
     *        row that names them.
     *
     * A state already known is applied from the cache with no request (which also retries a save that failed); any
     * other is looked up, one request at a time. A failed lookup is recorded and asked again only by a later call.
     * Nothing happens while signed out.
     */
    Q_INVOKABLE void resolveStates(const QVariantList& stids);

    /**
     * @brief Update the saved systems @p uids from RadioReference: each saved site's name, position and place ids
     *        (rows still on the site they were read for), then the names of any place left unnamed.
     *
     * @return false when signed out (the caller opens the account screen) or there are no saved systems; true
     *         otherwise, and updateFinished() follows exactly once: at once, with no change to `updating` or
     *         `progressText`, when none of @p uids can be updated; while an update runs, a second call changes nothing
     *         and the running one's signal is that answer.
     */
    Q_INVOKABLE bool updateFromRadioReference(const QStringList& uids);

    /** @brief The uids of every row of the cards unplacedCount counts, in row order: what the place notice updates. */
    Q_INVOKABLE QStringList unplacedUids() const;

    /** @brief Stop the running update and its own requests (state lookups a load asked for carry on); what was saved
     *         stays. No-op when no update runs. */
    Q_INVOKABLE void cancelUpdate();

    bool
    updating() const {
        return m_updating;
    }

    QString
    progressText() const {
        return m_progressText;
    }

    QString
    summaryText() const {
        return m_summaryText;
    }

    int
    unplacedCount() const {
        return m_unplacedCount;
    }

    QString unplacedText() const;

    /** @brief Whether a request is in flight or queued. */
    bool
    busy() const {
        return m_inFlight != 0U || !m_stateQueue.isEmpty();
    }

    /** @brief Whether the lookup of @p stid failed and has not been asked for again since. */
    bool
    stateFailed(int stid) const {
        return m_failedStates.contains(stid);
    }

    /** @brief Fill @p place's names and county links from what this service already knows. No request. */
    void fillPlaceNames(QVariantMap& place) const;

  Q_SIGNALS:
    void updatingChanged();
    void progressTextChanged();
    void summaryTextChanged();
    void unplacedCountChanged();
    /** @brief An update ended, counted as Listen's cards: @p updated those it refreshed any row of, @p unreachable those
     *         it refreshed none of because RadioReference did not answer, no longer lists the site, or the save
     *         failed. */
    void updateFinished(int updated, int unreachable);

  private:
    enum Fetch { FetchState, FetchDetails, FetchSites };

    /** @brief One completion, converted on the worker thread. */
    struct Reply;
    /** @brief Per-request context handed to the C callback. */
    struct Request;

    struct StateEntry {
        QString name;
        QHash<int, QString> counties;
    };

    /** @brief One site of a getTrsSites reply. */
    struct SiteRecord {
        int siteId = 0;
        int countyId = 0;
        QString name;
        bool positioned = false;
        double lat = 0;
        double lon = 0;
    };

    /** @brief The update in progress. */
    struct Update {
        QList<int> sids;                  /* distinct RadioReference systems, in the order asked */
        QHash<int, QStringList> siteUids; /* rows per system */
        QStringList placeUids;            /* rows with place ids and no site */
        QStringList uids;                 /* every row above, in the order asked */
        QHash<QString, QString> cardOf;   /* each row's card (card_key()) as the update began */
        int next = 0;                     /* index into sids of the system being read */
        bool detailsRead = false;         /* its getTrsDetails answered; getTrsSites is next */
        QVariantList stateIds;            /* that system's Trs.sState, once its details arrived */
        bool namesPhase = false;
        QSet<int> awaitedStates;
        QSet<int> namedStates;     /* states the names step applied (from the cache or an answer) */
        int total = 0;             /* cards */
        QSet<QString> doneCards;   /* cards whose system has been read */
        QSet<QString> updated;     /* cards any row of which was refreshed */
        QSet<QString> unreachable; /* cards with a row that could not be */
    };

    /** @brief One system's rows after its read: those refreshed, and those that could not be. */
    struct RowsOutcome {
        QStringList updated;
        QStringList failed;
    };

    /** @brief C completion callback. RUNS ON THE CLIENT'S WORKER THREAD. */
    static void onFetchDone(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result);
    static void convertResult(Fetch kind, void* result, Reply* reply);

    void applyReply(const Reply& reply);
    void applyStateReply(const Reply& reply);
    void applySitesReply(int sid, bool ok, const QList<SiteRecord>& sites);
    /** @brief Send the next request when none is in flight: the update's systems first, then queued states. */
    void submitNext();
    bool submit(Fetch kind, int key);
    void queueState(int stid);
    /** @brief The cached names of @p stid into the saved rows; a failed save is kept for retryUnapplied(). */
    void applyCachedState(int stid);
    void retryUnapplied();
    /** @brief Refresh the update's rows of system @p sid from its getTrsSites reply @p sites. */
    RowsOutcome refreshSystemRows(int sid, const QList<SiteRecord>& sites);
    /** @brief After the sites: look up the states any of the update's places still lacks names for. */
    void beginNamesPhase();
    /** @brief A state lookup ended (answered or not); the update ends with the last one it waits for. */
    void noteStateSettled(int stid);
    /** @brief End the update, counting its rows with no site when @p namesDone, and say so once. */
    void finishUpdate(bool namesDone);
    /** @brief Stop every request: cancel the one in flight, drop the queue and any late reply. */
    void stopRequests();
    /** @brief Stop only the running update's requests: its system reads and the states no load asked for. */
    void stopUpdateRequests();
    void setSummary(int updated, int unreachable);
    void onCredentialsChanged();
    /** @brief The cards (card_key()) with a row an update could place that shows no place. */
    QSet<QString> unplacedCards() const;
    void recountUnplaced();
    void setProgress();
    bool signedIn() const;

    QPointer<RadioReferenceModel> m_rr;
    QPointer<SavedSystemsModel> m_systems;
    dsd_rr_client* m_client = nullptr;

    /* Bumped by every stop, so a reply sent before it is dropped. */
    quint64 m_generation = 1;
    quint64 m_inFlight = 0;
    Fetch m_inFlightKind = FetchState;
    int m_inFlightKey = 0;
    QList<int> m_stateQueue;
    /* States a resolveStates() call (a load) asked for and that have not been answered yet. */
    QSet<int> m_resolveAsked;

    /* For the process; dropped at any credential change. */
    QHash<int, StateEntry> m_states;
    QSet<int> m_failedStates;
    /* States whose names a failed save kept out of the rows; retried at the next resolve. */
    QSet<int> m_unappliedStates;

    bool m_updating = false;
    Update m_update;
    /* Bumped whenever an update begins or ends, so a step that saved rows (whose signals reach QML) can tell the
     * update it belongs to has gone. */
    quint64 m_updateSerial = 0;
    QString m_progressText;
    QString m_summaryText;
    int m_unplacedCount = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RR_PLACE_SERVICE_H_ */
