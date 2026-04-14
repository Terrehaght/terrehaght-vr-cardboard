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

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "cardboard.h"
#include "util.h"

namespace ndk_hello_cardboard {

// ---------------------------------------------------------------------------
// SceneObject — a textured .obj mesh placed inside the 360 world.
// ---------------------------------------------------------------------------
struct SceneObject {
  std::string id;        // unique identifier, e.g. "robot_guide"
  TexturedMesh mesh;
  Texture texture;
  Matrix4x4 transform;   // position + rotation + scale baked in
  bool gaze_interactive; // true → highlights and fires OnTriggerEvent callback
  bool visible;
};

// ---------------------------------------------------------------------------
// HelloCardboardApp
// ---------------------------------------------------------------------------
class HelloCardboardApp {
 public:
  HelloCardboardApp(JavaVM* vm, jobject obj, jobject asset_mgr_obj);
  ~HelloCardboardApp();

  // Called on the GL thread once the surface is ready.
  void OnSurfaceCreated(JNIEnv* env);

  void SetScreenParams(int width, int height);
  void OnDrawFrame();
  void OnTriggerEvent();
  void OnPause();
  void OnResume();
  void SwitchViewer();

  // ---- Media ---------------------------------------------------------------
  // Called from Java (main thread) via JNI; safe before or after GL init.
  void SetMedia(JNIEnv* env, const std::string& filename, bool is_video);

  // Called from GL thread each frame to update the OES video texture.
  // Returns the OES texture ID so Java can create a SurfaceTexture on it.
  GLuint GetVideoTextureId() const { return video_texture_id_; }

  // Java calls this each frame to let C++ call updateTexImage().
  void UpdateVideoTexture(JNIEnv* env);

  // Store the Java SurfaceTexture object so C++ can call updateTexImage().
  void SetVideoSurfaceTexture(JNIEnv* env, jobject surface_texture);

  // ---- Scene objects -------------------------------------------------------
  bool AddSceneObject(JNIEnv* env,
                      const std::string& id,
                      const std::string& obj_asset,
                      const std::string& texture_asset,
                      std::array<float, 3> position,
                      std::array<float, 3> rotation_deg,
                      float scale,
                      bool gaze_interactive);

  void RemoveSceneObject(const std::string& id);
  void SetSceneObjectVisible(const std::string& id, bool visible);
  void SetSceneObjectTransform(const std::string& id,
                               std::array<float, 3> position,
                               std::array<float, 3> rotation_deg,
                               float scale);

  // ---- JNI callback support ------------------------------------------------
  // Must be called once after construction so C++ can call back up to Java.
  void SetJavaActivity(JNIEnv* env, jobject activity);

 private:
  static constexpr float kZNear = 0.1f;
  static constexpr float kZFar  = 100.f;

  bool UpdateDeviceParams();
  void GlSetup();
  void GlTeardown();
  Matrix4x4 GetPose();

  // Builds the world for one eye.
  void DrawWorld();

  // 360 sphere (rendered inside-out).
  void BuildSphereMesh();   // called once on GL thread
  void DrawSphere();

  // Gaze helpers.
  bool IsPointingAtObject(const Matrix4x4& model) const;
  int  GazedObjectIndex() const;  // -1 if none

  // Uniform matrix helpers.
  Matrix4x4 MakeTransform(std::array<float, 3> position,
                           std::array<float, 3> rotation_deg,
                           float scale) const;

  // ---- Cardboard / GL state ------------------------------------------------
  jobject java_asset_mgr_;
  AAssetManager* asset_mgr_;

  CardboardHeadTracker*      head_tracker_;
  CardboardLensDistortion*   lens_distortion_;
  CardboardDistortionRenderer* distortion_renderer_;

  CardboardEyeTextureDescription left_eye_texture_description_;
  CardboardEyeTextureDescription right_eye_texture_description_;

  bool screen_params_changed_;
  bool device_params_changed_;
  int  screen_width_;
  int  screen_height_;

  float projection_matrices_[2][16];
  float eye_matrices_[2][16];

  GLuint depthRenderBuffer_;
  GLuint framebuffer_;
  GLuint texture_;           // Cardboard render target texture

  // ---- Object shader (shared by scene objects) -----------------------------
  GLuint obj_program_;
  GLuint obj_position_param_;
  GLuint obj_uv_param_;
  GLuint obj_modelview_projection_param_;
  GLuint obj_tint_param_;    // u_Tint — highlight multiplier

  // ---- Sphere shader (360 background) -------------------------------------
  // Two variants: one for GL_TEXTURE_2D (image), one for GL_TEXTURE_EXTERNAL_OES (video).
  GLuint sphere_program_image_;
  GLuint sphere_program_video_;
  GLuint sphere_position_param_image_;
  GLuint sphere_uv_param_image_;
  GLuint sphere_mvp_param_image_;
  GLuint sphere_position_param_video_;
  GLuint sphere_uv_param_video_;
  GLuint sphere_mvp_param_video_;

  // Sphere geometry (generated procedurally, 64×32 segments).
  std::vector<GLfloat>  sphere_vertices_;
  std::vector<GLfloat>  sphere_uvs_;
  std::vector<GLushort> sphere_indices_;
  bool sphere_built_ = false;

  // ---- Media state ---------------------------------------------------------
  std::string media_filename_;
  bool        is_video_         = false;
  bool        media_dirty_      = false;  // set on main thread, consumed on GL thread
  std::mutex  media_mutex_;

  // Image path texture (GL_TEXTURE_2D).
  Texture image_texture_;
  bool    image_texture_loaded_ = false;

  // Video OES texture.
  GLuint   video_texture_id_  = 0;
  jobject  surface_texture_   = nullptr;  // global ref to Java SurfaceTexture
  jmethodID update_tex_image_method_ = nullptr;
  bool     video_texture_ready_ = false;

  // ---- Head / frame matrices -----------------------------------------------
  Matrix4x4 head_view_;
  Matrix4x4 modelview_projection_sphere_;  // identity model → full-sphere MVP

  // ---- Scene objects -------------------------------------------------------
  std::vector<SceneObject> scene_objects_;
  std::mutex               scene_objects_mutex_;

  // ---- Java callback -------------------------------------------------------
  jobject    java_activity_   = nullptr;  // global ref
  jmethodID  on_trigger_method_ = nullptr;
};

}  // namespace ndk_hello_cardboard

#endif  // HELLO_CARDBOARD_ANDROID_SRC_MAIN_JNI_HELLO_CARDBOARD_APP_H_
