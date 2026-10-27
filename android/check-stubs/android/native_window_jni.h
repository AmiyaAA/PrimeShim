#ifndef PRIMESHIM_STUB_ANW_JNI_H
#define PRIMESHIM_STUB_ANW_JNI_H
#include <jni.h>
#include <android/native_window.h>
ANativeWindow *ANativeWindow_fromSurface(JNIEnv *env, jobject surface);
#endif
