# SPDX-License-Identifier: GPL-3.0-or-later

# Qt 6.11.2 ships plain JARs without consumer keep rules. Its native libraries
# use FindClass, RegisterNatives and named upcalls. Re-audit on every Qt upgrade.
# Native registration requires even methods with no Java caller to survive.
-keepclasseswithmembers,includedescriptorclasses class org.qtproject.qt.android.** {
    native <methods>;
}

# Honor Qt's CLASS-retained JNI annotations. A type annotation marks its whole
# interface; member annotations keep only that member and its declaring class.
-keep,includedescriptorclasses @org.qtproject.qt.android.UsedFromNativeCode class * {
    *;
}
-keepclasseswithmembers,includedescriptorclasses class org.qtproject.qt.android.** {
    @org.qtproject.qt.android.UsedFromNativeCode <methods>;
}
-keepclasseswithmembers,includedescriptorclasses class org.qtproject.qt.android.** {
    @org.qtproject.qt.android.UsedFromNativeCode <fields>;
}

# Constructors used by native code are not consistently annotated (QtWindow,
# QtClipboardManager, etc.). Only constructors of surviving Qt types are kept.
-keepclassmembers,includedescriptorclasses class org.qtproject.qt.android.** {
    <init>(...);
}

# Unannotated native interfaces in the pinned Qt kit. Keep these individual
# bridge/data types, not the whole Qt, AndroidX or Kotlin dependency trees.
-keep class org.qtproject.qt.android.QtActivityBase
-keep class org.qtproject.qt.android.QtServiceBase
-keep class org.qtproject.qt.android.QtNative {
    static org.qtproject.qt.android.QtThread getQtThread();
    static void setStarted(boolean);
}
-keep class org.qtproject.qt.android.QtThread {
    void exit();
}
-keep class org.qtproject.qt.android.QtLoader {
    !private *;
    private static boolean isUncompressedNativeLibs();
}
-keep class org.qtproject.qt.android.QtWindow {
    void setParent(org.qtproject.qt.android.QtWindow);
}
-keep class org.qtproject.qt.android.QtView {
    void createWindow(long);
}
-keep class org.qtproject.qt.android.extras.QtAndroidBinder {
    void setId(long);
}
-keep class org.qtproject.qt.android.extras.QtAndroidServiceConnection {
    void setId(long);
}
-keep class org.qtproject.qt.android.QtContentFileEngine { *; }
# The uncompressed-library path used by Play reads these unannotated fields
# through JNI while discovering plugins inside APK splits. R8 otherwise renames
# relativePath and removes size/isDir, so Qt cannot find its Android platform plugin.
-keep class org.qtproject.qt.android.QtApkFileEngine$JFileInfo {
    java.lang.String relativePath;
    long size;
    boolean isDir;
}
-keep class org.qtproject.qt.android.QtExtractedText { *; }
-keep class org.qtproject.qt.android.QtAbstractItemModel { *; }
-keep class org.qtproject.qt.android.QtAndroidItemModelProxy { *; }
-keep class org.qtproject.qt.android.QtModelIndex { *; }
-keep class org.qtproject.qt.android.QtQuickView { *; }
-keep class org.qtproject.qt.android.network.QtNetwork {
    static void registerReceiver(android.content.Context);
    static void unregisterReceiver(android.content.Context);
    static android.net.ConnectivityManager getConnectivityManager(android.content.Context);
    static android.net.ProxyInfo getProxyInfo(android.content.Context);
}
-keep class org.qtproject.qt.android.networkinformation.QtAndroidNetworkInformation {
    static void registerReceiver(android.content.Context);
    static void unregisterReceiver(android.content.Context);
}

# Qt opens/shares documents through this AndroidX method from C++.
-keep class androidx.core.content.FileProvider {
    public static android.net.Uri getUriForFile(android.content.Context, java.lang.String, java.io.File);
}
