/* Minimal jni.h stand-in used only for host-side syntax checking with a glibc
 * cross compiler.  The real build uses the NDK's own <jni.h>. */
#ifndef PRIMESHIM_STUB_JNI_H
#define PRIMESHIM_STUB_JNI_H
#include <stdint.h>
typedef void *jobject;
typedef void *jstring;
typedef void *jclass;
typedef void *JavaVM;
#define JNI_VERSION_1_6 0x00010006
typedef int32_t jint;
typedef int32_t jsize;
typedef void *jintArray;
struct JNINativeInterface_;
typedef const struct JNINativeInterface_ *JNIEnv;
struct JNINativeInterface_ {
    const char *(*GetStringUTFChars)(JNIEnv *, jstring, unsigned char *);
    void (*ReleaseStringUTFChars)(JNIEnv *, jstring, const char *);
    jstring (*NewStringUTF)(JNIEnv *, const char *);
    jintArray (*NewIntArray)(JNIEnv *, jsize);
    void (*SetIntArrayRegion)(JNIEnv *, jintArray, jsize, jsize, const jint *);
};
#define JNIEXPORT
#define JNICALL
#endif
