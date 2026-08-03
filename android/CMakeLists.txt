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
set_property(
    TARGET dsd-neo-app
    PROPERTY QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
)
set_property(TARGET dsd-neo-app PROPERTY QT_ANDROID_VERSION_CODE 1)
set_property(TARGET dsd-neo-app PROPERTY QT_ANDROID_ABIS "arm64-v8a")
