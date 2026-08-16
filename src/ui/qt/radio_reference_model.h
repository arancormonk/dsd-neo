// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief QML-facing brain for the RadioReference.com import.
 *
 * Owns a dsd_rr_client, drives the browse pipeline (zip or country/state/county
 * to systems to sites), previews what an import would produce, and writes the
 * generated CSVs into the imported-files library with provenance.
 *
 * Two rules shape this header and must survive edits:
 *
 * 1. No USE_EXPAT / USE_CURL conditional and no curl or expat type appears here.
 *    dsd-neo_ui_qt links dsd-neo_runtime PRIVATE, so the Android JNI sources
 *    compile without those defines; a conditional member would be an ODR violation that
 *    links cleanly and crashes at run time. Availability is answered at run time
 *    by dsd_rr_available().
 * 2. The password lives in a plain member for the process lifetime and is never
 *    persisted, never logged, and never bound into a QML property. No credential
 *    may reach statusText, errorText, or any other user-visible string.
 */

#ifndef DSD_NEO_SRC_UI_QT_RADIO_REFERENCE_MODEL_H_
#define DSD_NEO_SRC_UI_QT_RADIO_REFERENCE_MODEL_H_

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>

namespace dsd_qt {

class AppPrefs;
class DecoderHost;
class ImportedFilesModel;

class RadioReferenceModel : public QObject {
    Q_OBJECT
    /* Whether the SHIPPED transport and parser exist in this build. A UI gate,
     * not a functional one: with an injected transport the model works while
     * this is false. */
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool hasAppKey READ hasAppKey NOTIFY credentialsChanged)
    Q_PROPERTY(bool credentialsReady READ credentialsReady NOTIFY credentialsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY statusChanged)
    /* An opaque int for C++ and tests. QML branches on the two derived booleans
     * instead: Q_ENUM does not make the names reachable from QML for a context
     * property, and this tree registers no QML types beyond the two Quick items. */
    Q_PROPERTY(int errorKind READ errorKind NOTIFY statusChanged)
    Q_PROPERTY(bool errorIsAuth READ errorIsAuth NOTIFY statusChanged)
    Q_PROPERTY(bool errorIsSubscription READ errorIsSubscription NOTIFY statusChanged)
    Q_PROPERTY(QVariantList countries READ countries NOTIFY listsChanged)
    Q_PROPERTY(QVariantList states READ states NOTIFY listsChanged)
    Q_PROPERTY(QVariantList counties READ counties NOTIFY listsChanged)
    Q_PROPERTY(QVariantList systems READ systems NOTIFY listsChanged)
    Q_PROPERTY(QVariantList sites READ sites NOTIFY systemChanged)
    Q_PROPERTY(QVariantMap systemDetails READ systemDetails NOTIFY systemChanged)
    Q_PROPERTY(QVariantMap talkgroupSummary READ talkgroupSummary NOTIFY systemChanged)
    /* So the site list can switch between single- and multi-select without
     * knowing the protocol enum, for the same reason errorIsAuth exists. */
    Q_PROPERTY(bool conventional READ conventional NOTIFY systemChanged)

  public:
    /** @brief What went wrong, as QML-agnostic integers. */
    enum ErrorKind {
        NoError = 0,
        AuthError,
        SubscriptionError,
        NetworkError,
        ServerError,
        ParseError,
        CancelledError,
        UnsupportedError,
        ConfigError
    };
    Q_ENUM(ErrorKind)

    RadioReferenceModel(AppPrefs* prefs, ImportedFilesModel* importedFiles, DecoderHost* host,
                        QObject* parent = nullptr);
    ~RadioReferenceModel() override;

    bool available() const;
    bool hasAppKey() const;
    bool credentialsReady() const;

    bool
    busy() const {
        return m_outstanding > 0;
    }

    QString
    statusText() const {
        return m_statusText;
    }

    QString
    errorText() const {
        return m_errorText;
    }

    int
    errorKind() const {
        return m_errorKind;
    }

    bool
    errorIsAuth() const {
        return m_errorKind == AuthError;
    }

    bool
    errorIsSubscription() const {
        return m_errorKind == SubscriptionError;
    }

    QVariantList
    countries() const {
        return m_countries;
    }

    QVariantList
    states() const {
        return m_states;
    }

    QVariantList
    counties() const {
        return m_counties;
    }

    QVariantList
    systems() const {
        return m_systems;
    }

    QVariantList
    sites() const {
        return m_sites;
    }

    QVariantMap
    systemDetails() const {
        return m_systemDetails;
    }

    QVariantMap
    talkgroupSummary() const {
        return m_talkgroupSummary;
    }

    bool
    conventional() const {
        return dsd_rr_protocol_is_conventional(m_protocol) != 0;
    }

    /** @brief Hold the account password for this process only. Never persisted. */
    Q_INVOKABLE void setPassword(const QString& password);

    /** @brief Overwrite and drop the held password. */
    Q_INVOKABLE void clearPassword();

    /** @brief Verify the credentials and the premium subscription via getUserData. */
    Q_INVOKABLE void checkAccount();

    /** @brief Resolve a ZIP to its county, then list that county's systems. */
    Q_INVOKABLE void lookupZip(const QString& zip);

    Q_INVOKABLE void loadCountries();
    Q_INVOKABLE void loadCountryStates(int coid);
    Q_INVOKABLE void loadStateCounties(int stid);
    Q_INVOKABLE void loadStateSystems(int stid);
    Q_INVOKABLE void loadCountySystems(int ctid);

    /**
     * @brief Fetch everything one system needs: details, sites, talkgroups and
     *        talkgroup categories, plus the support tables on first use.
     */
    Q_INVOKABLE void loadSystem(int sid);

    /** @brief Best-effort cancel of everything in flight. */
    Q_INVOKABLE void cancel();

    /**
     * @brief Preview an import. Pure: no network, no files.
     *
     * @param siteIndexes Indexes into sites(). A list, not an index, because a
     *                    Conventional Networked import selects several repeaters;
     *                    a trunked protocol uses the first and warns about the rest.
     * @param options     partialEncAsDe, simulcast, esk. simulcast and esk default
     *                    to what the RadioReference record says when absent.
     * @return {ok, protocol, protocolName, conventional, scanList, siteCount,
     *          decodeFlag, trunking, freqMhz, groupCsvText, chanCsvText, chanNeed,
     *          warnings, blockedReason}.
     */
    Q_INVOKABLE QVariantMap buildImportPlan(const QVariantList& siteIndexes, const QVariantMap& options);

    /**
     * @brief Write a plan's CSVs into the imported-files library.
     *
     * @param plan       A map from buildImportPlan().
     * @param systemName Name for the saved system.
     * @param savedRow   Existing saved-system row to update, or -1 for a new one.
     * @return The saved-system field map for QML to merge into a wizard-shaped
     *         map, plus {ok, error}. Never calls savedSystems.add() itself.
     */
    Q_INVOKABLE QVariantMap performImport(const QVariantMap& plan, const QString& systemName, int savedRow);

    /**
     * @brief Re-fetch a generated library row and replace its file in place.
     *
     * Asynchronous: the answer arrives as refreshFinished(). The stored path is
     * preserved, so every saved system referencing it stays valid, and the
     * staging file is validated before the stored copy is touched.
     *
     * The site selection is recovered from the row's `rrSiteNumbers` provenance
     * (falling back to the singular `rrSiteNumber` for a row written before that
     * key existed) and matched by site NUMBER, never by index: RadioReference is
     * free to reorder getTrsSites, and an index would refresh the wrong repeater.
     *
     * Two things provenance does not record and this therefore cannot restore:
     * the "treat partly encrypted as encrypted" answer, which falls back to the
     * UI default of on, and whatever system the RadioReference screen currently
     * has loaded, which this replaces.
     *
     * @param row Library row.
     * @return false when the refresh could not even be started; the reason is in
     *         errorText. true means refreshFinished() will follow.
     */
    Q_INVOKABLE bool refreshRow(int row);

    /**
     * @brief Replace the HTTP transport. Test seam; NULL restores the built-in one.
     *
     * Not Q_INVOKABLE: it names a C struct QML has no way to build.
     */
    void setTransportForTests(const dsd_rr_transport* transport);

  Q_SIGNALS:
    void credentialsChanged();
    void busyChanged();
    void statusChanged();
    void listsChanged();
    void systemChanged();
    /** @brief One refreshRow() outcome; @p result has importFile()'s shape. */
    void refreshFinished(int row, const QVariantMap& result);

  private:
    /** @brief Which call a completion belongs to. */
    enum Fetch {
        FetchUserData,
        FetchZip,
        FetchCountries,
        FetchStates,
        FetchCounties,
        FetchSystems,
        FetchDetails,
        FetchSites,
        FetchTalkgroups,
        FetchTalkgroupCats
    };

    /** @brief One completion, already converted off the worker thread. */
    struct Reply;

    /** @brief Per-request context handed to the C callback as its user pointer. */
    struct Request;

    /** @brief C completion callback. RUNS ON THE CLIENT'S WORKER THREAD. */
    static void onFetchDone(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result);

    /** @brief Take ownership of a C result into a reply. WORKER THREAD ONLY. */
    static void convertResult(const Request& request, void* result, Reply* reply);

    /** @brief Apply one completion. GUI thread only. */
    void applyReply(const Reply& reply);

    /** @brief Apply a browse-list completion. @return true when it was one. */
    bool applyListReply(const Reply& reply);

    /** @brief Apply one of loadSystem()'s four completions. @return true when it was one. */
    bool applySystemReply(const Reply& reply);

    /**
     * @brief Open a new request batch: cancel what is in flight, retire older
     *        replies by bumping the generation, and clear the error.
     */
    void startBatch(const QString& status);

    /** @brief Build a request context, or report why one could not be made. */
    Request* beginFetch(Fetch kind);

    /** @brief Account for a submitted request; frees the context if it failed. */
    void endFetch(quint64 id, Request* request);

    /** @brief Fill @p auth from prefs and the held password. Caller scrubs it. */
    bool fillAuth(dsd_rr_auth* auth) const;

    void setStatus(const QString& status);
    void setError(int kind, const QString& text);
    void clearSystem();
    void finishSystemLoad();

    /** @brief Shallow copies of the selected sites, in selection order. */
    QList<dsd_rr_site> selectedSites(const QVariantList& siteIndexes, QVariantList* warnings) const;

    /**
     * @brief Run both generators for @p chosen into @p plan.
     *
     * @return false only on a hard generator failure; an empty channel map is a
     *         valid outcome, not an error.
     */
    bool generateFiles(const QList<dsd_rr_site>& chosen, bool partialEncAsDe, QVariantMap* plan,
                       QVariantList* warnings) const;

    /** @brief Regenerate and commit the pending refresh. GUI thread only. */
    void completeRefresh();

    /** @brief Report a refresh outcome and forget the pending state. */
    void endRefresh(const QVariantMap& result);

    AppPrefs* m_prefs = nullptr;
    ImportedFilesModel* m_importedFiles = nullptr;
    DecoderHost* m_host = nullptr;
    dsd_rr_client* m_client = nullptr;

    /* Session-only, never persisted, never shown. */
    QString m_password;

    QString m_statusText;
    QString m_errorText;
    int m_errorKind = NoError;
    int m_outstanding = 0;
    /* Bumped on every new request batch and on cancel, so a reply that arrives
     * after the user moved on is dropped instead of overwriting fresh state. */
    quint64 m_generation = 1;
    QList<quint64> m_pendingIds;

    QVariantList m_countries;
    QVariantList m_states;
    QVariantList m_counties;
    QVariantList m_systems;
    QVariantList m_sites;
    QVariantMap m_systemDetails;
    QVariantMap m_talkgroupSummary;

    /* The fetched C data, kept because the generators read the structs, not the
     * QVariant views. Freed here and nowhere else. */
    int m_sid = 0;
    dsd_rr_protocol m_protocol = DSD_RR_PROTO_UNSUPPORTED;
    bool m_recordSaysSimulcast = false;
    bool m_recordSaysEsk = false;
    dsd_rr_site_list m_siteData;
    dsd_rr_talkgroup_list m_talkgroupData;
    /* How many of the four system calls are still outstanding, so the preview is
     * assembled once rather than four times. */
    int m_systemPending = 0;

    /* The library row a refresh is fetching for, or -1. Cleared by startBatch(),
     * so any other action the user takes retires a refresh still in flight. */
    int m_refreshRow = -1;
    QString m_refreshKind;
    /* The sites to regenerate from: siteIds normally, siteNumbers for a row
     * written before those were recorded (see m_refreshByNumber). */
    QList<int> m_refreshSites;
    bool m_refreshByNumber = false;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RADIO_REFERENCE_MODEL_H_ */
