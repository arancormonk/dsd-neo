# Android app shell: Qt Quick UI + JNI lifecycle glue around the engine.
# Built only for ANDROID with DSD_ENABLE_QT_UI=ON (see the root CMakeLists).

find_package(
    Qt6
    REQUIRED
    COMPONENTS Core Gui Qml Quick QuickControls2
)

qt_policy(SET QTP0002 NEW)

qt_add_executable(
    dsd-neo-app
    main.cpp
    decoder_host_android.cpp
    dsdneo_jni.cpp
)

set_target_properties(dsd-neo-app PROPERTIES AUTOMOC ON)

# The Kotlin service binds to this same library, so the service and the UI share one
# instance of the engine's globals (ground rule 4: one decoder per process).
target_link_libraries(
    dsd-neo-app
    PRIVATE
        dsd-neo_ui_qt
        dsd-neo_engine
        dsd-neo_feature_rtlsdr
        dsd-neo_feature_codec2
        ${LIBS}
        log
)

target_include_directories(dsd-neo-app SYSTEM PRIVATE ${_PUBLIC_INCLUDES})
target_include_directories(dsd-neo-app PRIVATE "${PROJECT_SOURCE_DIR}/include")

# Statically linked third-party code (OpenSSL, libsndfile, ...) must not export its
# symbols into the app process.
target_link_options(dsd-neo-app PRIVATE "-Wl,--exclude-libs,ALL")

# 16 KB LOAD alignment is a Play requirement for targetSdk 35+. NDK r28+ does this by
# default; r27 (the NDK Qt 6.11 is qualified with) needs it spelled out.
target_link_options(dsd-neo-app PRIVATE "-Wl,-z,max-page-size=16384")

# Every shipped asset carries the license/notice set (docs/release-assets-checklist.md),
# and androiddeployqt packages nothing but QT_ANDROID_PACKAGE_SOURCE_DIR. So point that at
# a staged copy in the build tree and add the license files under assets/, which Gradle
# packages into the APK. Staging runs on every build rather than at configure time, so
# edits to android/package/ still reach the APK without re-running CMake.
set(dsd_neo_android_package_dir "${CMAKE_CURRENT_BINARY_DIR}/package")

# Stage once at configure time as well, so the directory androiddeployqt is pointed at
# already exists when Qt writes the deployment settings.
execute_process(
    COMMAND
        ${CMAKE_COMMAND} -DREPO_DIR=${PROJECT_SOURCE_DIR}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package
        -DSTAGE_DIR=${dsd_neo_android_package_dir} -P
        ${PROJECT_SOURCE_DIR}/cmake/stage_android_package.cmake
    COMMAND_ERROR_IS_FATAL ANY
)

add_custom_target(
    dsd-neo-app_stage_package
    COMMAND
        ${CMAKE_COMMAND} -DREPO_DIR=${PROJECT_SOURCE_DIR}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package
        -DSTAGE_DIR=${dsd_neo_android_package_dir} -P
        ${PROJECT_SOURCE_DIR}/cmake/stage_android_package.cmake
    COMMENT "Staging the Android package source with license files"
    VERBATIM
)

add_dependencies(dsd-neo-app dsd-neo-app_stage_package)

set_property(
    TARGET dsd-neo-app
    PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR "${dsd_neo_android_package_dir}"
)
set_property(TARGET dsd-neo-app PROPERTY QT_ANDROID_MIN_SDK_VERSION 29)
set_property(TARGET dsd-neo-app PROPERTY QT_ANDROID_TARGET_SDK_VERSION 36)

# Without these two androiddeployqt substitutes the CMake target name into
# android:label and drops android:icon outright, so the app installs as
# "dsd-neo-app" with the stock Android robot. Both are resource references
# resolved out of android/package/res.
set_property(TARGET dsd-neo-app PROPERTY QT_ANDROID_APP_NAME "@string/app_name")
set_property(
    TARGET dsd-neo-app
    PROPERTY QT_ANDROID_APP_ICON "@mipmap/ic_launcher"
)

# The CLI banner and the terminal UI header both print GIT_TAG (set in the root
# CMakeLists), but the Qt UI shows no version at all, so android:versionName is
# the app's only version surface. Use the same string, so a nightly APK
# identifies itself the way a nightly desktop build does. Android's versionName
# carries no "v" prefix, and on a tag GIT_TAG is exactly "vX.Y.Z", so this
# resolves to plain PROJECT_VERSION for releases and for git-less tarballs.
string(REGEX REPLACE "^v" "" dsd_neo_android_version_name "${GIT_TAG}")
set_property(
    TARGET dsd-neo-app
    PROPERTY QT_ANDROID_VERSION_NAME "${dsd_neo_android_version_name}"
)

# Android compares version codes, not names, so the hardcoded 1 this replaces
# made every build look like the same release and blocked upgrade installs.
#
# PROJECT_VERSION alone is not enough either: it only moves at release time and
# the tag sits on the bump commit itself, so every nightly in a release window
# would carry the same code as the release it follows. Scale the release code up
# and add git describe's commit distance - tagged builds stay round, nightlies
# increment, and the next release clears every nightly before it.
#
# Minor and patch are assumed to stay below 100 and a release window below 1000
# commits. The scheme tops out at major version 210, against the platform's
# 2100000000 ceiling.
if(GIT_TAG MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+-([0-9]+)-g[0-9a-f]+$")
    set(dsd_neo_android_commit_distance "${CMAKE_MATCH_1}")
else()
    # A tag, or a tarball with no git metadata: this is the release itself.
    set(dsd_neo_android_commit_distance 0)
endif()

if(dsd_neo_android_commit_distance GREATER 999)
    message(
        FATAL_ERROR
        "${dsd_neo_android_commit_distance} commits since the last release tag "
        "overflows the Android version code's nightly field. Cut a release, or "
        "widen the multiplier below and in the android-ci launcher-identity check."
    )
endif()

math(
    EXPR
    dsd_neo_android_version_code
    "(${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}) * 1000 + ${dsd_neo_android_commit_distance}"
)
set_property(
    TARGET dsd-neo-app
    PROPERTY QT_ANDROID_VERSION_CODE "${dsd_neo_android_version_code}"
)
set_property(TARGET dsd-neo-app PROPERTY QT_ANDROID_ABIS "arm64-v8a")
