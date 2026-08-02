// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "decoder_host_android.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>

#include "dsdneo_jni.h"

namespace dsd_android {

namespace {

constexpr const char* kServiceClass = "io/github/arancormonk/dsdneo/DecoderService";
constexpr const char* kSupportClass = "io/github/arancormonk/dsdneo/AppSupport";

QJniObject
android_context(void) {
    return QNativeInterface::QAndroidApplication::context();
}

/** @brief Builds a Java String[] from @p values. Local ref, valid for one call. */
jobjectArray
to_java_string_array(QJniEnvironment& env, const QStringList& values) {
    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr) {
        return nullptr;
    }
    jobjectArray array = env->NewObjectArray(static_cast<jsize>(values.size()), string_class, nullptr);
    if (array == nullptr) {
        return nullptr;
    }
    for (qsizetype i = 0; i < values.size(); i++) {
        QJniObject item = QJniObject::fromString(values.at(i));
        env->SetObjectArrayElement(array, static_cast<jsize>(i), item.object());
    }
    return array;
}

} // namespace

DecoderHostAndroid::DecoderHostAndroid(QObject* parent) : dsd_qt::DecoderHost(parent) {
    QJniObject context = android_context();
    if (context.isValid()) {
        QJniObject::callStaticMethod<void>(kSupportClass, "ensureNotificationPermission", "(Landroid/app/Activity;)V",
                                           context.object());
    }
}

DecoderHostAndroid::~DecoderHostAndroid() = default;

bool
DecoderHostAndroid::isRunning() const {
    return m_running;
}

QString
DecoderHostAndroid::statusText() const {
    return m_status;
}

bool
DecoderHostAndroid::start(const QStringList& argv) {
    QJniObject context = android_context();
    if (!context.isValid()) {
        setStatus(QStringLiteral("No Android context"));
        return false;
    }

    QJniEnvironment env;
    jobjectArray args = to_java_string_array(env, argv);
    if (args == nullptr) {
        setStatus(QStringLiteral("Could not marshal arguments"));
        return false;
    }

    QJniObject::callStaticMethod<void>(kServiceClass, "startDecoder", "(Landroid/content/Context;[Ljava/lang/String;)V",
                                       context.object(), args);
    env->DeleteLocalRef(args);

    setStatus(QStringLiteral("Starting…"));
    return true;
}

void
DecoderHostAndroid::stop() {
    QJniObject context = android_context();
    if (!context.isValid()) {
        return;
    }
    QJniObject::callStaticMethod<void>(kServiceClass, "stopDecoder", "(Landroid/content/Context;)V", context.object());
    setStatus(QStringLiteral("Stopping…"));
}

void
DecoderHostAndroid::moveToBackground() {
    /* Finishing the Activity would terminate the Qt process, and with it the service
     * that owns the engine. Backgrounding keeps both alive. */
    QJniObject activity = android_context();
    if (activity.isValid()) {
        (void)activity.callMethod<jboolean>("moveTaskToBack", "(Z)Z", JNI_TRUE);
    }
}

QString
DecoderHostAndroid::importContentUri(const QString& reference, const QString& fileName) {
    QJniObject context = android_context();
    if (!context.isValid()) {
        return QString();
    }
    QJniObject uri = QJniObject::fromString(reference);
    QJniObject name = QJniObject::fromString(fileName);
    QJniObject result = QJniObject::callStaticObjectMethod(
        kSupportClass, "copyContentUriToCache",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", context.object(),
        uri.object(), name.object());
    return result.isValid() ? result.toString() : QString();
}

void
DecoderHostAndroid::refresh() {
    const bool running = engine_is_running();
    if (running != m_running) {
        m_running = running;
        Q_EMIT runningChanged();
    }

    /* The service owns the transitions; native running-state alone cannot tell
     * "starting" from "idle". */
    QJniObject state = QJniObject::callStaticObjectMethod(kServiceClass, "stateName", "()Ljava/lang/String;");
    const QString name = state.isValid() ? state.toString() : QStringLiteral("IDLE");

    QString text;
    if (name == QLatin1String("RUNNING")) {
        text = running ? QStringLiteral("Decoding") : QStringLiteral("Starting…");
    } else if (name == QLatin1String("STARTING")) {
        text = QStringLiteral("Starting…");
    } else if (name == QLatin1String("STOPPING")) {
        text = QStringLiteral("Stopping…");
    } else {
        text = QStringLiteral("Idle");
    }
    setStatus(text);
}

void
DecoderHostAndroid::setStatus(const QString& text) {
    if (m_status == text) {
        return;
    }
    m_status = text;
    Q_EMIT statusTextChanged();
}

} // namespace dsd_android
