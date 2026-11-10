# SPDX-License-Identifier: GPL-3.0-or-later

# android/decoder_host_android.cpp looks these up by literal class, method and
# descriptor. Keep only that boundary; Kotlin companions and implementation
# helpers remain eligible for shrinking and obfuscation. Keep signatures intact
# while allowing R8 to optimize the method bodies.
-keep,allowoptimization class io.github.arancormonk.dsdneo.DecoderService {
    public static void startDecoder(android.content.Context, java.lang.String[], long);
    public static boolean retireAttempts(long);
    public static void stopDecoder(android.content.Context);
    public static java.lang.String lifecycleStatus();
    public static void acknowledgeReceiverEnding(long);
    public static java.lang.String stateName();
    public static java.lang.String lastError();
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.AppSupport {
    public static int keyboardTopPixels();
    public static int takeBackRequests();
    public static float[] fontKey(android.content.Context);
    public static float[] fontTable(android.content.Context);
    public static float fontPixels(android.content.Context, float);
    public static float activityDensity(android.content.Context);
    public static void setDarkAppearance(android.app.Activity, boolean);
    public static java.lang.String audioRouteName(android.content.Context, int);
    public static boolean takeNotificationPermissionRefresh();
    public static boolean notificationPermissionNeeded(android.content.Context);
    public static void ensureNotificationPermission(android.app.Activity);
    public static void finishApp(android.content.Context);
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.UsbSourceManager {
    public static void requestAccess(android.content.Context);
    public static void requestAccessForSource(android.content.Context, java.lang.String, java.lang.String);
    public static java.lang.String deviceStatus();
    public static java.lang.String attachedSource();
    public static java.lang.String takeAttachment();
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.ScreenSupport {
    public static void configure(android.content.Context, int, int);
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.DocumentCopies {
    public static boolean copyFileToDocument(android.content.ContentResolver, java.lang.String, java.lang.String);
    public static void abort();
    # Imports (#832), from the import worker.
    public static int copyImportDocument(android.content.ContentResolver, long, java.lang.String, java.lang.String, long);
    public static void cancelImport(long);
    public static long[] importProgress(long);
    public static java.lang.String importName(long);
    public static boolean importNoSpace(long);
    public static void forgetImport(long);
    # Documents opened from other apps (#831): the probe, from the import worker.
    public static java.lang.String probeImportDocument(android.content.ContentResolver, long, java.lang.String, long);
}

# Call recordings (#708): playback, export/save and share.
-keep,allowoptimization class io.github.arancormonk.dsdneo.RecordingPlayback {
    public static int play(android.content.Context, java.lang.String);
    public static void pause();
    public static void resume();
    public static void seek(long);
    public static void stop();
    public static java.lang.String status();
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.RecordingExport {
    public static java.lang.String[] transfer(android.content.ContentResolver, long, java.lang.String[], java.lang.String[], java.lang.String, boolean);
    public static long progress(long);
    public static void cancel(long);
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.RecordingShare {
    public static java.lang.String[] roots(android.content.Context);
    public static boolean canShare(android.content.Context, java.lang.String);
    public static java.lang.String stage(android.content.Context, java.lang.String);
    public static boolean offer(android.content.Context, java.lang.String, java.lang.String, java.lang.String);
    public static void clean(android.content.Context);
    public static java.lang.String takeFailure();
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.LocationSupport {
    public static void requestCurrentLocation(android.app.Activity, long);
    public static void requestSiteLocation(android.app.Activity, long);
    public static void cancelLocationRequest(long);
    public static java.lang.String pollResult();
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.DocumentPicker {
    public static boolean open(android.app.Activity, long);
    public static boolean create(android.app.Activity, long, java.lang.String, java.lang.String);
    public static void cancel(long);
    public static java.lang.String pollResult();
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.DownloadsSave {
    public static void start(android.content.ContentResolver);
    public static java.lang.String publishedName(java.lang.String);
    public static void discard(java.lang.String);
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.DiagnosticsShare {
    public static void share(android.content.Context, java.lang.String, java.lang.String);
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.ProcessFacts {
    public static java.lang.String processFacts();
    public static java.lang.String[] exitLines();
    public static java.lang.String[] exitsMarker();
    public static java.lang.String attemptContext(java.lang.String);
}

# The RadioReference sign-in form for password managers: DecoderHostAndroid's
# signInAutofill* overrides.
-keep,allowoptimization class io.github.arancormonk.dsdneo.SignInAutofill {
    public static void begin(int);
    public static void field(int, int, int, int, int, int, boolean, boolean, java.lang.String);
    public static void request(int, int, int, int, int, int);
    public static void commit(int, java.lang.String, java.lang.String);
    public static void end(int);
}

# The exported Java_io_github_arancormonk_dsdneo_DsdNative_* symbols use these
# names. This also keeps any native entry point not reached from Kotlin today.
-keep,includedescriptorclasses class io.github.arancormonk.dsdneo.DsdNative {
    native <methods>;
}

# Line numbers for stack traces, the main-looper watchdog's incidents included
# (#782): retrace maps them with the build's mapping.txt. At minSdk 29 R8 encodes
# them as instruction offsets, so the DEX does not grow (it shrank 5.8 KB; the
# mapping grew 57 KB). Source file names stay out of the APK.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
