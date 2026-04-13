/*
 * hello_cardboard_jni.cc  (modified – Flexie robot JNI bridge additions)
 *
 * All original bridge methods are PRESERVED UNCHANGED.
 * New Flexie methods are added at the bottom.
 */

#include <android/log.h>
#include <jni.h>

#include <memory>

#include "hello_cardboard_app.h"

#define JNI_METHOD(return_type, method_name) \
  JNIEXPORT return_type JNICALL              \
      Java_com_google_cardboard_VrActivity_##method_name

namespace {

inline jlong jptr(ndk_hello_cardboard::HelloCardboardApp* native_app) {
    return reinterpret_cast<intptr_t>(native_app);
}

inline ndk_hello_cardboard::HelloCardboardApp* native(jlong ptr) {
    return reinterpret_cast<ndk_hello_cardboard::HelloCardboardApp*>(ptr);
}

JavaVM* javaVm;

}  // anonymous namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    javaVm = vm;
    return JNI_VERSION_1_6;
}

// ---------------------------------------------------------------------------
// Original bridge methods (UNCHANGED)
// ---------------------------------------------------------------------------

JNI_METHOD(jlong, nativeOnCreate)
(JNIEnv* /*env*/, jobject obj, jobject asset_mgr) {
    return jptr(new ndk_hello_cardboard::HelloCardboardApp(javaVm, obj, asset_mgr));
}

JNI_METHOD(void, nativeOnDestroy)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
    delete native(native_app);
}

JNI_METHOD(void, nativeOnSurfaceCreated)
(JNIEnv* env, jobject /*obj*/, jlong native_app) {
    native(native_app)->OnSurfaceCreated(env);
}

JNI_METHOD(void, nativeOnDrawFrame)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
    native(native_app)->OnDrawFrame();
}

JNI_METHOD(void, nativeOnTriggerEvent)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
    native(native_app)->OnTriggerEvent();
}

JNI_METHOD(void, nativeOnPause)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
    native(native_app)->OnPause();
}

JNI_METHOD(void, nativeOnResume)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
    native(native_app)->OnResume();
}

JNI_METHOD(void, nativeSetScreenParams)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app, jint width, jint height) {
    native(native_app)->SetScreenParams(width, height);
}

JNI_METHOD(void, nativeSwitchViewer)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
    native(native_app)->SwitchViewer();
}

// 360-viewer additions (UNCHANGED)
JNI_METHOD(void, nativeSetMedia)
(JNIEnv* env, jobject /*obj*/, jlong native_app,
 jstring filename, jboolean isVideo) {
    const char* fname_cstr = env->GetStringUTFChars(filename, nullptr);
    std::string fname(fname_cstr);
    env->ReleaseStringUTFChars(filename, fname_cstr);
    native(native_app)->SetMedia(fname, isVideo == JNI_TRUE);
}

JNI_METHOD(jint, nativeGetVideoTextureId)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
    return static_cast<jint>(native(native_app)->GetVideoTextureId());
}

JNI_METHOD(void, nativeSetSurfaceTexture)
(JNIEnv* env, jobject /*obj*/, jlong native_app, jobject surfaceTexture) {
    native(native_app)->SetSurfaceTexture(env, surfaceTexture);
}

JNI_METHOD(void, nativeUpdateVideoTexture)
(JNIEnv* env, jobject /*obj*/, jlong native_app) {
    native(native_app)->UpdateVideoTexture(env);
}

// ---------------------------------------------------------------------------
// Flexie robot bridge methods (NEW)
// ---------------------------------------------------------------------------

/**
 * Sets the robot behaviour mode.
 * @param mode  0=ANCHORED  1=FOLLOW  2=MOVE_TO  3=HIDDEN
 * Thread: any (simple atomic-ish setter, safe to call from Java UI thread).
 */
JNI_METHOD(void, nativeSetFlexieMode)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app, jint mode) {
    native(native_app)->SetFlexieMode(static_cast<int>(mode));
}

/**
 * Sets the robot pose/animation.
 * @param pose  0=IDLE 1=WAVE 2=TALKING 3=POINT 4=TURN_LEFT 5=TURN_RIGHT
 */
JNI_METHOD(void, nativeSetFlexiePose)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app, jint pose) {
    native(native_app)->SetFlexiePose(static_cast<int>(pose));
}

/**
 * Sets the robot skin texture.
 * @param skin  0=BLUE  1=PINK
 */
JNI_METHOD(void, nativeSetFlexieSkin)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app, jint skin) {
    native(native_app)->SetFlexieSkin(static_cast<int>(skin));
}

/**
 * Moves the robot to a world-space position.
 * In ANCHORED mode the robot snaps immediately; in MOVE_TO it glides.
 */
JNI_METHOD(void, nativeSetFlexiePosition)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app,
 jfloat x, jfloat y, jfloat z) {
    native(native_app)->SetFlexiePosition(x, y, z);
}

/**
 * Sets the distance the robot maintains ahead of the user in FOLLOW mode.
 * Default is 3.0 metres.
 */
JNI_METHOD(void, nativeSetFlexieFollowDistance)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app, jfloat distance) {
    native(native_app)->SetFlexieFollowDistance(distance);
}

/**
 * Sets the uniform scale of the robot model.
 * Default is 0.35 (robot ~70 cm tall in world units).
 */
JNI_METHOD(void, nativeSetFlexieScale)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app, jfloat scale) {
    native(native_app)->SetFlexieScale(scale);
}

}  // extern "C"
