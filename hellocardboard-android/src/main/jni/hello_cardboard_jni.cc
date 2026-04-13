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

#include "hello_cardboard_app.h"

// ---------------------------------------------------------------------------
// JNI_METHOD macro – all methods are on com.google.cardboard.VrActivity.
// This naming convention is kept exactly as in the original file.
// ---------------------------------------------------------------------------
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
// Original bridge methods (unchanged signatures)
// ---------------------------------------------------------------------------

JNI_METHOD(jlong, nativeOnCreate)
(JNIEnv* /*env*/, jobject obj, jobject asset_mgr) {
  return jptr(
      new ndk_hello_cardboard::HelloCardboardApp(javaVm, obj, asset_mgr));
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

// ---------------------------------------------------------------------------
// New 360-viewer bridge methods
// ---------------------------------------------------------------------------

/**
 * Tells the native layer which asset to display and whether it is a video.
 *
 * @param filename  Asset path relative to the assets/ root, e.g. "360/tour.jpg"
 * @param isVideo   true for video (OES/MediaPlayer path), false for image.
 *
 * Thread: main thread, called in VrActivity.onCreate before the GL surface
 * exists, so no GL operations are performed here.
 */
JNI_METHOD(void, nativeSetMedia)
(JNIEnv* env, jobject /*obj*/, jlong native_app,
 jstring filename, jboolean isVideo) {
  const char* fname_cstr = env->GetStringUTFChars(filename, nullptr);
  std::string fname(fname_cstr);
  env->ReleaseStringUTFChars(filename, fname_cstr);
  native(native_app)->SetMedia(fname, isVideo == JNI_TRUE);
}

/**
 * Creates a GL_TEXTURE_EXTERNAL_OES texture on the GL thread and returns its
 * name so that Java can attach a SurfaceTexture to it.
 *
 * @return OpenGL texture name for the OES texture.
 *
 * Thread: GL thread (called from Renderer.onSurfaceCreated).
 */
JNI_METHOD(jint, nativeGetVideoTextureId)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
  return static_cast<jint>(native(native_app)->GetVideoTextureId());
}

/**
 * Stores a global JNI reference to the Java SurfaceTexture so that
 * nativeUpdateVideoTexture() can call updateTexImage() on it.
 *
 * @param surfaceTexture  The android.graphics.SurfaceTexture wrapping the OES
 *                        texture created by nativeGetVideoTextureId.
 *
 * Thread: main thread (called after SurfaceTexture is created in setupVideoPlayback).
 */
JNI_METHOD(void, nativeSetSurfaceTexture)
(JNIEnv* env, jobject /*obj*/, jlong native_app, jobject surfaceTexture) {
  native(native_app)->SetSurfaceTexture(env, surfaceTexture);
}

/**
 * Calls surfaceTexture.updateTexImage() via JNI, pushing the latest decoded
 * video frame into the OES texture that the sphere shader samples.
 *
 * This is queued onto the GL thread by the SurfaceTexture.OnFrameAvailableListener
 * in VrActivity so that updateTexImage() is always called on the correct thread.
 *
 * Thread: GL thread.
 */
JNI_METHOD(void, nativeUpdateVideoTexture)
(JNIEnv* env, jobject /*obj*/, jlong native_app) {
  native(native_app)->UpdateVideoTexture(env);
}

}  // extern "C"
