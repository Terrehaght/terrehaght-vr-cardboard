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

#ifndef HELLO_CARDBOARD_ANDROID_SRC_MAIN_JNI_HELLO_CARDBOARD_APP_H_
#define HELLO_CARDBOARD_ANDROID_SRC_MAIN_JNI_HELLO_CARDBOARD_APP_H_

#include <android/asset_manager.h>
#include <jni.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <GLES2/gl2.h>
#include "cardboard.h"
#include "util.h"

namespace ndk_hello_cardboard {

/**
 * 360-degree image and video viewer built on the Google Cardboard NDK pipeline.
 *
 * The entire Cardboard stereo pipeline (GlSetup, UpdateDeviceParams, GetPose,
 * OnDrawFrame stereo loop, CardboardDistortionRenderer_renderEyeToDisplay,
 * OnPause/Resume/SwitchViewer) is preserved exactly from the original demo.
 *
 * What changed:
 *   - Replaced DrawRoom / DrawTarget / game objects with DrawSphere().
 *   - DrawSphere renders a UV sphere from the inside (inverted winding) whose
 *     texture is either a static equirectangular PNG or an Android
 *     GL_TEXTURE_EXTERNAL_OES texture fed by a MediaPlayer SurfaceTexture.
 *   - Three new public methods expose the video path to JNI:
 *       SetMedia()          – store filename + media-type flag before GL init
 *       GetVideoTextureId() – create and return the OES texture (GL thread)
 *       SetSurfaceTexture() – store a global JNI ref to the Java SurfaceTexture
 *       UpdateVideoTexture()– call surfaceTexture.updateTexImage() via JNI
 */
class HelloCardboardApp {
 public:
  /**
   * Creates a HelloCardboardApp.
   *
   * @param vm JavaVM pointer.
   * @param obj Android activity object.
   * @param asset_mgr_obj The asset manager object.
   */
  HelloCardboardApp(JavaVM* vm, jobject obj, jobject asset_mgr_obj);

  ~HelloCardboardApp();

  /**
   * Initialises GL objects (shaders, sphere VBOs, image texture if applicable).
   * Must be called on the rendering thread with a valid GL context.
   */
  void OnSurfaceCreated(JNIEnv* env);

  /** Sets screen parameters. */
  void SetScreenParams(int width, int height);

  /** Renders one stereo frame. Must be called on the rendering thread. */
  void OnDrawFrame();

  /** Touch/trigger event – no-op for the viewer; reserved for future use. */
  void OnTriggerEvent();

  /** Pauses head tracking. */
  void OnPause();

  /** Resumes head tracking and forces a device-params refresh. */
  void OnResume();

  /** Opens the QR-code scanner to switch viewer. */
  void SwitchViewer();

  // -------------------------------------------------------------------------
  // 360 media API (called from JNI, all from the GL thread unless noted)
  // -------------------------------------------------------------------------

  /**
   * Stores the asset filename and media-type flag.
   * Called from Java before OnSurfaceCreated; thread-safe for simple writes.
   */
  void SetMedia(const std::string& filename, bool is_video);

  /**
   * Creates the GL_TEXTURE_EXTERNAL_OES texture that will receive video frames
   * and returns its texture ID.  Must be called on the GL thread.
   * Idempotent: returns the existing ID if already created.
   */
  GLuint GetVideoTextureId();

  /**
   * Stores a global JNI reference to the Java SurfaceTexture so that
   * UpdateVideoTexture() can call updateTexImage() on it.
   * May be called from any thread that holds a valid JNIEnv.
   */
  void SetSurfaceTexture(JNIEnv* env, jobject surface_texture);

  /**
   * Calls surfaceTexture.updateTexImage() via the stored JNI reference,
   * pushing the latest decoded video frame into the OES texture.
   * Must be called on the GL thread (queued via GLSurfaceView.queueEvent).
   */
  void UpdateVideoTexture(JNIEnv* env);

 private:
  // ---- Cardboard clip planes (unchanged) ----------------------------------
  static constexpr float kZNear = 0.1f;
  static constexpr float kZFar  = 100.f;

  // ---- Sphere parameters --------------------------------------------------
  static constexpr float kSphereRadius  = 50.f;   // safely inside kZFar
  static constexpr int   kSphereSectors = 64;      // longitude slices
  static constexpr int   kSphereStacks  = 32;      // latitude bands

  // ---- Cardboard pipeline helpers (unchanged signatures) ------------------
  bool      UpdateDeviceParams();
  void      GlSetup();
  void      GlTeardown();
  Matrix4x4 GetPose();

  // ---- 360 rendering ------------------------------------------------------
  /** Renders the inside-facing sphere for the current eye's MVP. */
  void DrawSphere();

  /**
   * Procedurally generates a UV sphere (kSphereSectors × kSphereStacks) with
   * inverted winding so the surface faces inward, and uploads it to VBOs.
   */
  void GenerateSphere(int sectors, int stacks);

  // ---- JVM / asset manager ------------------------------------------------
  JavaVM*        java_vm_;
  jobject        java_asset_mgr_;      // global ref
  AAssetManager* asset_mgr_;

  // ---- Cardboard pipeline objects (untouched) -----------------------------
  CardboardHeadTracker*        head_tracker_;
  CardboardLensDistortion*     lens_distortion_;
  CardboardDistortionRenderer* distortion_renderer_;

  CardboardEyeTextureDescription left_eye_texture_description_;
  CardboardEyeTextureDescription right_eye_texture_description_;

  bool screen_params_changed_;
  bool device_params_changed_;
  int  screen_width_;
  int  screen_height_;

  float projection_matrices_[2][16];
  float eye_matrices_[2][16];

  GLuint depthRenderBuffer_;   // depth renderbuffer
  GLuint framebuffer_;         // offscreen framebuffer
  GLuint texture_;             // colour attachment / distortion input

  // ---- Per-frame matrices -------------------------------------------------
  Matrix4x4 head_view_;
  Matrix4x4 modelview_projection_sphere_;

  // ---- Image shader program (sampler2D) -----------------------------------
  GLuint img_program_;
  GLuint img_position_param_;
  GLuint img_uv_param_;
  GLuint img_mvp_param_;

  // ---- OES video shader program (samplerExternalOES) ----------------------
  GLuint oes_program_;
  GLuint oes_position_param_;
  GLuint oes_uv_param_;
  GLuint oes_mvp_param_;

  // ---- Sphere geometry VBOs -----------------------------------------------
  GLuint sphere_vbo_pos_;       // vec3 position per vertex
  GLuint sphere_vbo_uv_;        // vec2 UV per vertex
  GLuint sphere_ibo_;           // GLushort index buffer
  int    sphere_index_count_;   // total index count

  // ---- Image-mode texture -------------------------------------------------
  Texture sphere_image_tex_;

  // ---- Video-mode OES texture ---------------------------------------------
  GLuint  video_texture_id_;        // GL_TEXTURE_EXTERNAL_OES texture name
  jobject surface_texture_ref_;     // global JNI ref to Java SurfaceTexture

  // ---- Media state --------------------------------------------------------
  std::string media_filename_;      // asset path, e.g. "360/tour.jpg"
  bool        is_video_;
  bool        media_initialized_;   // true after image texture is loaded
};

}  // namespace ndk_hello_cardboard

#endif  // HELLO_CARDBOARD_ANDROID_SRC_MAIN_JNI_HELLO_CARDBOARD_APP_H_
