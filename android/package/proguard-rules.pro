# SPDX-License-Identifier: GPL-3.0-or-later

# android/decoder_host_android.cpp looks these up by literal class, method and
# descriptor. Keep only that boundary; Kotlin companions and implementation
# helpers remain eligible for shrinking and obfuscation. Keep signatures intact
# while allowing R8 to optimize the method bodies.
-keep,allowoptimization class io.github.arancormonk.dsdneo.DecoderService {
    public static void startDecoder(android.content.Context, java.lang.String[]);
    public static void stopDecoder(android.content.Context);
    public static java.lang.String lifecycleStatus();
    public static void acknowledgeReceiverEnding(long);
    public static java.lang.String stateName();
    public static java.lang.String lastError();
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.AppSupport {
    public static int keyboardTopPixels();
    public static int takeBackRequests();
    public static java.lang.String fontConfiguration(android.content.Context);
    public static float fontPixels(android.content.Context, float);
    public static void setDarkAppearance(android.app.Activity, boolean);
    public static java.lang.String audioRouteName(android.content.Context, int);
    public static boolean takeNotificationPermissionRefresh();
    public static boolean notificationPermissionNeeded(android.content.Context);
    public static void ensureNotificationPermission(android.app.Activity);
    public static java.lang.String importDocumentToFiles(android.content.Context, java.lang.String, java.lang.String, java.lang.String);
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
    public static int copyDocumentToFile(android.content.ContentResolver, java.lang.String, java.lang.String, long);
    public static boolean copyFileToDocument(android.content.ContentResolver, java.lang.String, java.lang.String);
    public static void abort();
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.LocationSupport {
    public static void requestCurrentLocation(android.app.Activity, long);
    public static void requestSiteLocation(android.app.Activity, long);
    public static void cancelLocationRequest(long);
    public static java.lang.String pollResult();
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.DiagnosticsShare {
    public static void share(android.content.Context, java.lang.String, java.lang.String);
}

-keep,allowoptimization class io.github.arancormonk.dsdneo.ProcessFacts {
    public static java.lang.String processFacts();
    public static java.lang.String[] exitLines();
    public static java.lang.String attemptContext(java.lang.String);
}

# The exported Java_io_github_arancormonk_dsdneo_DsdNative_* symbols use these
# names. This also keeps any native entry point not reached from Kotlin today.
-keep,includedescriptorclasses class io.github.arancormonk.dsdneo.DsdNative {
    native <methods>;
}
