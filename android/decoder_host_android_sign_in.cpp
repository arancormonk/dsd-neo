// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief A password manager's fill of the RadioReference sign-in form, on its way from the Android main thread
 * (nativeSignInFilled, android/dsdneo_jni.cpp) to the GUI thread's relay.
 *
 * The calls out to SignInAutofill.kt, the signInAutofill*() overrides, are JNI only and live in
 * decoder_host_android.cpp.
 */

#include <QMetaObject>
#include <QString>
#include <Qt>
#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "decoder_host_android.h"
#include "qstring_scrub.h"

namespace dsd_android {

namespace {

/** @brief One fill on its way to the GUI thread. Scrubbed when it goes, delivered or not. */
struct SignInFill {
    int token = 0;
    bool hasUsername = false;
    bool hasPassword = false;
    QString username;
    QString password;

    SignInFill() = default;
    SignInFill(const SignInFill&) = delete;
    SignInFill& operator=(const SignInFill&) = delete;

    ~SignInFill() {
        dsd_qt::scrub_qstring(username);
        dsd_qt::scrub_qstring(password);
    }
};

/* The live hosts, newest last, which postSignInFill() reaches. One in the app; the host tests may make several. */
std::mutex g_fill_lock;

std::vector<DecoderHostAndroid*>&
fill_hosts() {
    static std::vector<DecoderHostAndroid*> hosts;
    return hosts;
}

} // namespace

DecoderHostAndroid::SignInFillLink::SignInFillLink(DecoderHostAndroid* host) : m_host(host) {
    const std::lock_guard<std::mutex> lock(g_fill_lock);
    fill_hosts().push_back(m_host);
}

DecoderHostAndroid::SignInFillLink::~SignInFillLink() {
    const std::lock_guard<std::mutex> lock(g_fill_lock);
    std::vector<DecoderHostAndroid*>& hosts = fill_hosts();
    hosts.erase(std::remove(hosts.begin(), hosts.end(), m_host), hosts.end());
}

void
DecoderHostAndroid::postSignInFill(int token, bool hasUsername, QString& username, bool hasPassword,
                                   QString& password) {
    const auto fill = std::make_shared<SignInFill>();
    fill->token = token;
    fill->hasUsername = hasUsername;
    fill->hasPassword = hasPassword;
    fill->username = std::exchange(username, QString());
    fill->password = std::exchange(password, QString());
    /* Under the lock, so the host cannot go between the lookup and the post. A host that goes before the call runs
     * takes it with it, unrun, and the fill is scrubbed as it is dropped. */
    const std::lock_guard<std::mutex> lock(g_fill_lock);
    if (fill_hosts().empty()) {
        return;
    }
    DecoderHostAndroid* host = fill_hosts().back();
    QMetaObject::invokeMethod(
        host,
        [host, fill] {
            host->deliverSignInFill(fill->token, fill->hasUsername, fill->username, fill->hasPassword, fill->password);
        },
        Qt::QueuedConnection);
}

} // namespace dsd_android
