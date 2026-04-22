# Keep JNI entry points
-keepclasseswithmembernames class * {
    native <methods>;
}

# libsu
-keep class com.topjohnwu.superuser.** { *; }

# AIDL-exposed root service interface
-keep class com.qdiag.bandlock.root.** { *; }
