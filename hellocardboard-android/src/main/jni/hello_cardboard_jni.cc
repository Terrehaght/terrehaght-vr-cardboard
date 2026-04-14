/*
 * Copyright 2019 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android/log.h>
#include <jni.h>

#include <memory>
#include <string>

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

// Helper: convert jstring → std::string.
std::string JStringToString(JNIEnv* env, jstring js) {
  if (!js) return {};
  const char* chars = env->GetStringUTFChars(js, nullptr);
  std::string result(chars);
  env->ReleaseStringUTFChars(js, chars);
  return result;
}

}  // anonymous namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  javaVm = vm;
  return JNI_VERSION_1_6;
}

// ---- Original Cardboard lifecycle ----------------------------------------

JNI_METHOD(jlong, nativeOnCreate)
(JNIEnv* env, jobject obj, jobject asset_mgr) {
  auto* app = new ndk_hello_cardboard::HelloCardboardApp(javaVm, obj, asset_mgr);
  app->SetJavaActivity(env, obj);
  return jptr(app);
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
(JNIEnv* env, jobject obj, jlong native_app) {
  native(native_app)->OnTriggerEvent();

  // Fire per-object callbacks synchronously on the GL thread.
  // (The actual per-object firing is delegated back to Java here.)
  // VrActivity.onSceneObjectTrigger() is called via the stored method ref
  // inside HelloCardboardApp — we just need to supply the env.
  // For simplicity, the trigger loop in OnTriggerEvent logs the id;
  // to call back to Java we re-implement a small helper here since
  // we have the env available on this thread.
  (void)env; (void)obj;
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

// ---- Media ----------------------------------------------------------------

/**
 * nativeSetMedia(long nativeApp, String filename, boolean isVideo)
 * Must be called before or after surface creation; safe from any thread.
 */
JNI_METHOD(void, nativeSetMedia)
(JNIEnv* env, jobject /*obj*/, jlong native_app,
 jstring filename, jboolean is_video) {
  native(native_app)->SetMedia(env, JStringToString(env, filename),
                               is_video == JNI_TRUE);
}

/**
 * nativeGetVideoTextureId(long nativeApp) → int
 * Returns the GL_TEXTURE_EXTERNAL_OES texture ID for the SurfaceTexture.
 * Must be called from the GL thread (after OnSurfaceCreated).
 */
JNI_METHOD(jint, nativeGetVideoTextureId)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
  return static_cast<jint>(native(native_app)->GetVideoTextureId());
}

/**
 * nativeSetVideoSurfaceTexture(long nativeApp, SurfaceTexture st)
 * Stores the Java SurfaceTexture so C++ can call updateTexImage() each frame.
 */
JNI_METHOD(void, nativeSetVideoSurfaceTexture)
(JNIEnv* env, jobject /*obj*/, jlong native_app, jobject surface_texture) {
  native(native_app)->SetVideoSurfaceTexture(env, surface_texture);
}

/**
 * nativeUpdateVideoTexture(long nativeApp)
 * Called from the GL thread each frame to latch the latest video frame.
 */
JNI_METHOD(void, nativeUpdateVideoTexture)
(JNIEnv* env, jobject /*obj*/, jlong native_app) {
  native(native_app)->UpdateVideoTexture(env);
}

// ---- Scene objects --------------------------------------------------------

/**
 * nativeAddSceneObject(long nativeApp, String id, String objAsset,
 *   String textureAsset, float x, float y, float z,
 *   float rotX, float rotY, float rotZ, float scale, boolean gazeInteractive)
 */
JNI_METHOD(jboolean, nativeAddSceneObject)
(JNIEnv* env, jobject /*obj*/, jlong native_app,
 jstring id, jstring obj_asset, jstring texture_asset,
 jfloat x, jfloat y, jfloat z,
 jfloat rot_x, jfloat rot_y, jfloat rot_z,
 jfloat scale, jboolean gaze_interactive) {
  bool ok = native(native_app)->AddSceneObject(
      env,
      JStringToString(env, id),
      JStringToString(env, obj_asset),
      JStringToString(env, texture_asset),
      {x, y, z},
      {rot_x, rot_y, rot_z},
      scale,
      gaze_interactive == JNI_TRUE);
  return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * nativeRemoveSceneObject(long nativeApp, String id)
 */
JNI_METHOD(void, nativeRemoveSceneObject)
(JNIEnv* env, jobject /*obj*/, jlong native_app, jstring id) {
  native(native_app)->RemoveSceneObject(JStringToString(env, id));
}

/**
 * nativeSetSceneObjectVisible(long nativeApp, String id, boolean visible)
 */
JNI_METHOD(void, nativeSetSceneObjectVisible)
(JNIEnv* env, jobject /*obj*/, jlong native_app,
 jstring id, jboolean visible) {
  native(native_app)->SetSceneObjectVisible(JStringToString(env, id),
                                            visible == JNI_TRUE);
}

/**
 * nativeSetSceneObjectTransform(long nativeApp, String id,
 *   float x, float y, float z, float rotX, float rotY, float rotZ, float scale)
 */
JNI_METHOD(void, nativeSetSceneObjectTransform)
(JNIEnv* env, jobject /*obj*/, jlong native_app,
 jstring id,
 jfloat x, jfloat y, jfloat z,
 jfloat rot_x, jfloat rot_y, jfloat rot_z,
 jfloat scale) {
  native(native_app)->SetSceneObjectTransform(
      JStringToString(env, id),
      {x, y, z},
      {rot_x, rot_y, rot_z},
      scale);
}

}  // extern "C"
