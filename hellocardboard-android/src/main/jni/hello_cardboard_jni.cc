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

}  // anonymous namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  javaVm = vm;
  return JNI_VERSION_1_6;
}

// ---- Core lifecycle --------------------------------------------------------

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

// ---- Finger mode -----------------------------------------------------------

/**
 * Enables or disables finger (touch drag) mode.
 *
 * @param native_app  Pointer to the HelloCardboardApp instance.
 * @param enabled     JNI_TRUE to enter finger mode, JNI_FALSE for VR mode.
 */
JNI_METHOD(void, nativeSetFingerMode)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app, jboolean enabled) {
  native(native_app)->SetFingerMode(enabled == JNI_TRUE);
}

/**
 * Forwards a touch drag delta to the native camera controller.
 * Should only be called while finger mode is active.
 *
 * @param native_app  Pointer to the HelloCardboardApp instance.
 * @param dx          Horizontal drag delta in pixels.
 * @param dy          Vertical drag delta in pixels.
 */
JNI_METHOD(void, nativeOnTouchDrag)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app, jfloat dx, jfloat dy) {
  native(native_app)->OnTouchDrag(dx, dy);
}

// ---- Robot pose ------------------------------------------------------------

/**
 * Switches the sprite-sheet pose shown on the robot billboard.
 *
 * @param native_app  Pointer to the HelloCardboardApp instance.
 * @param pose_index  0-11, row-major index into the 4×3 sprite grid.
 */
JNI_METHOD(void, nativeSetRobotPose)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app, jint pose_index) {
  native(native_app)->SetRobotPose(static_cast<int>(pose_index));
}

// ---- 360 media -------------------------------------------------------------

/**
 * Tells the native renderer which 360° equirectangular asset to display.
 *
 * For still images (isVideo == false):
 *   The native side loads the PNG/JPG from assets and maps it onto the inner
 *   surface of a procedural sphere, replacing the CubeRoom background.
 *
 * For videos (isVideo == true):
 *   The native side creates a GL_TEXTURE_EXTERNAL_OES texture.  Java then
 *   wraps this texture id in a SurfaceTexture and feeds decoded frames from
 *   MediaPlayer before each nativeOnDrawFrame call.
 *
 * Must be called on the GL thread, i.e. from within onSurfaceCreated in the
 * GLSurfaceView.Renderer.
 *
 * @param native_app  Pointer to the HelloCardboardApp instance.
 * @param asset_path  Java String: asset-relative path (e.g. "360/scene.mp4").
 * @param is_video    JNI_TRUE for video, JNI_FALSE for still image.
 */
JNI_METHOD(void, nativeSetMediaAsset)
(JNIEnv* env, jobject /*obj*/, jlong native_app,
 jstring asset_path, jboolean is_video) {
  const char* path_chars = env->GetStringUTFChars(asset_path, nullptr);
  std::string path(path_chars);
  env->ReleaseStringUTFChars(asset_path, path_chars);

  native(native_app)->SetMediaAsset(env, path, is_video == JNI_TRUE);
}

/**
 * Returns the GL_TEXTURE_EXTERNAL_OES texture id created for video mode.
 *
 * Java wraps this id in a SurfaceTexture to let MediaPlayer write decoded
 * frames into it.  Only valid after nativeSetMediaAsset has been called with
 * isVideo == true.
 *
 * @param native_app  Pointer to the HelloCardboardApp instance.
 * @return            GL texture name, or 0 if not in video mode.
 */
JNI_METHOD(jint, nativeGetVideoTextureId)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
  return static_cast<jint>(native(native_app)->GetVideoTextureId());
}

// ---- Gaze detection --------------------------------------------------------

/**
 * Returns true if the user is currently looking at the robot billboard.
 * Safe to call every frame from onDrawFrame.
 */
JNI_METHOD(jboolean, nativeIsLookingAtRobot)
(JNIEnv* /*env*/, jobject /*obj*/, jlong native_app) {
  return native(native_app)->IsPointingAtTarget() ? JNI_TRUE : JNI_FALSE;
}
 
}  // extern "C"
