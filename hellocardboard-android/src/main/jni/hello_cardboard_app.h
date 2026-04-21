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
#include <GLES2/gl2ext.h>   // GL_TEXTURE_EXTERNAL_OES
#include "cardboard.h"
#include "util.h"

namespace ndk_hello_cardboard {

/**
 * Main application class for the Cardboard VR sample.
 *
 * Scene modes
 * -----------
 *  • Default (game) mode – CubeRoom + game objects + robot billboard.
 *    Activated when no media asset has been supplied.
 *
 *  • 360-image mode – equirectangular sphere with a still PNG/JPG mapped
 *    on its inner surface + robot billboard on top.
 *
 *  • 360-video mode – equirectangular sphere whose texture is an
 *    GL_TEXTURE_EXTERNAL_OES texture updated each frame by a SurfaceTexture
 *    driven by Android MediaPlayer (Java side) + robot billboard on top.
 *
 * The robot billboard is ALWAYS rendered regardless of scene mode; it sits
 * on top of whichever background is active.
 *
 * Rendering modes
 * ---------------
 *  • VR Mode (default): CardboardHeadTracker, split-screen + lens distortion.
 *  • Finger Mode: touch-drag yaw/pitch, full-screen, no distortion.
 */
class HelloCardboardApp {
 public:
  HelloCardboardApp(JavaVM* vm, jobject obj, jobject asset_mgr_obj);
  ~HelloCardboardApp();

  // ---- Lifecycle ----------------------------------------------------------
  void OnSurfaceCreated(JNIEnv* env);
  void SetScreenParams(int width, int height);
  void OnDrawFrame();
  void OnTriggerEvent();
  void OnPause();
  void OnResume();
  void SwitchViewer();

  // ---- Input --------------------------------------------------------------
  void SetFingerMode(bool enabled);
  void OnTouchDrag(float dx, float dy);

  // ---- Robot pose ---------------------------------------------------------
  void SetRobotPose(int poseIndex);
  bool IsPointingAtTarget();
  // ---- 360 media ----------------------------------------------------------

  /**
   * Configures the scene to display a 360° equirectangular asset.
   *
   * For still images (isVideo == false):
   *   Loads the asset PNG/JPG from the asset manager and uploads it as a
   *   GL_TEXTURE_2D bound to the sphere mesh.  Replaces the CubeRoom.
   *
   * For videos (isVideo == true):
   *   Creates a GL_TEXTURE_EXTERNAL_OES texture name and stores it in
   *   video_oes_texture_id_.  The Java side wraps this in a SurfaceTexture
   *   and feeds decoded frames via MediaPlayer.  The sphere samples this
   *   OES texture each frame via the dedicated OES shader.
   *
   * Must be called on the GL thread (from onSurfaceCreated).
   *
   * @param env       JNI environment for the GL thread.
   * @param assetPath Asset-relative path (e.g. "360/my_scene.jpg").
   * @param isVideo   true = video (OES), false = still image (TEX_2D).
   */
  void SetMediaAsset(JNIEnv* env, const std::string& assetPath, bool isVideo);

  /**
   * Returns the GL_TEXTURE_EXTERNAL_OES texture id created for video mode.
   * Only valid after SetMediaAsset(..., true) has been called.
   * Returns 0 if not in video mode.
   */
  GLuint GetVideoTextureId() const { return video_oes_texture_id_; }

 private:
  // ---- Clip planes --------------------------------------------------------
  static constexpr float kZNear = 0.1f;
  static constexpr float kZFar  = 100.f;

  // ---- Finger-mode constants ----------------------------------------------
  static constexpr float kTouchRotationDegPerPixel = 0.15f;
  static constexpr float kMaxPitchDeg              = 85.0f;

  // ---- Sprite-sheet constants (robot) ------------------------------------
  static constexpr int kSpriteCols      = 4;
  static constexpr int kSpriteRows      = 3;
  static constexpr int kSpritePoseCount = kSpriteCols * kSpriteRows;

  // ---- Internal helpers ---------------------------------------------------
  bool     UpdateDeviceParams();
  void     GlSetup();
  void     GlTeardown();
  Matrix4x4 GetPose();
  Matrix4x4 GetFingerPoseMatrix(float yaw, float pitch);

  /** Draws the active background (CubeRoom OR 360 sphere). */
  void DrawBackground();

  /** Draws the robot billboard (always on top of whatever background). */
  void DrawRobot();

  /** Draws all world objects for the current view. */
  void DrawWorld();

  // -- Game-mode only -------------------------------------------------------
  void DrawRoom();
  void DrawTarget();
  void HideTarget();
  

  // -- 360-sphere helpers ---------------------------------------------------

  /**
   * Generates sphere mesh vertices / UVs / indices into sphere_* arrays.
   * Call once from SetMediaAsset before uploading to GPU.
   *
   * @param stacks Number of horizontal rings (latitude bands).
   * @param slices Number of vertical segments per ring (longitude bands).
   */
  void BuildSphereMesh(int stacks, int slices);

  /**
   * Uploads sphere_vertices_ / sphere_uvs_ / sphere_indices_ to GPU buffers
   * sphere_vbo_, sphere_uvo_, sphere_ibo_.  Generates buffers if they do not
   * yet exist.
   */
  void UploadSphereMesh();

  /**
   * Renders the sphere using whichever shader / texture is active for the
   * current media mode (OES video shader OR standard obj shader for images).
   *
   * @param mvp  Combined model-view-projection matrix for this eye / view.
   */
  void DrawSphere(Matrix4x4 mvp);

  // ---- Core references ----------------------------------------------------
  jobject       java_asset_mgr_;
  AAssetManager* asset_mgr_;

  CardboardHeadTracker*         head_tracker_;
  CardboardLensDistortion*      lens_distortion_;
  CardboardDistortionRenderer*  distortion_renderer_;

  CardboardEyeTextureDescription left_eye_texture_description_;
  CardboardEyeTextureDescription right_eye_texture_description_;

  // ---- Screen / device state ----------------------------------------------
  bool screen_params_changed_;
  bool device_params_changed_;
  int  screen_width_;
  int  screen_height_;

  float projection_matrices_[2][16];
  float eye_matrices_[2][16];

  // ---- GL resources (shared framebuffer) ----------------------------------
  GLuint depthRenderBuffer_;
  GLuint framebuffer_;
  GLuint texture_;          ///< Cardboard off-screen render target.

  // ---- Object shader (used for both room and robot billboard) -------------
  GLuint obj_program_;
  GLuint obj_position_param_;
  GLuint obj_uv_param_;
  GLuint obj_modelview_projection_param_;
  GLint  obj_uv_rect_param_;   ///< u_UVRect uniform for sprite-sheet

  // ---- OES video shader (used only for video-mode sphere) -----------------
  GLuint oes_program_;                   ///< Compiled in SetMediaAsset for video.
  GLuint oes_position_param_;
  GLuint oes_uv_param_;
  GLuint oes_mvp_param_;

  // ---- Scene matrices -----------------------------------------------------
  Matrix4x4 head_view_;
  Matrix4x4 model_target_;
  Matrix4x4 modelview_projection_target_;
  Matrix4x4 modelview_projection_room_;

  // ---- Game-mode geometry (used only when media_mode_ == kNone) -----------
  TexturedMesh room_;
  Texture      room_tex_;

  std::vector<TexturedMesh> target_object_meshes_;
  std::vector<Texture>      target_object_not_selected_textures_;
  std::vector<Texture>      target_object_selected_textures_;
  int                       cur_target_object_;

  // ---- Finger-mode state --------------------------------------------------
  bool  finger_mode_;
  float touch_yaw_;
  float touch_pitch_;

  // ---- Sprite-sheet pose state (robot) ------------------------------------
  Texture sprite_sheet_texture_;
  float   sprite_u0_, sprite_u1_, sprite_v0_, sprite_v1_;

  // =========================================================================
  // 360 media fields
  // =========================================================================

  /** Identifies which background type is currently active. */
  enum class MediaMode {
    kNone,   ///< Default: CubeRoom game scene.
    kImage,  ///< 360° equirectangular still image on a sphere (GL_TEXTURE_2D).
    kVideo,  ///< 360° equirectangular video on a sphere (GL_TEXTURE_EXTERNAL_OES).
  };
  MediaMode media_mode_;

  // ---- Sphere GPU buffers -------------------------------------------------
  GLuint sphere_vbo_;   ///< Vertex position buffer.
  GLuint sphere_uvo_;   ///< UV coordinate buffer.
  GLuint sphere_ibo_;   ///< Index buffer.
  GLsizei sphere_index_count_;  ///< Number of indices to draw.

  // CPU-side sphere mesh (built once, uploaded, then may be discarded).
  std::vector<float>    sphere_vertices_;  ///< x,y,z interleaved.
  std::vector<float>    sphere_uvs_;       ///< u,v interleaved.
  std::vector<uint16_t> sphere_indices_;

  // ---- Image-mode texture (GL_TEXTURE_2D) ---------------------------------
  Texture sphere_image_texture_;

  // ---- Video-mode OES texture ---------------------------------------------
  /**
   * GL_TEXTURE_EXTERNAL_OES texture name.
   * Created by SetMediaAsset(isVideo=true); Java wraps it in a SurfaceTexture
   * and calls updateTexImage() each frame before nativeOnDrawFrame.
   */
  GLuint video_oes_texture_id_;
};

}  // namespace ndk_hello_cardboard

#endif  // HELLO_CARDBOARD_ANDROID_SRC_MAIN_JNI_HELLO_CARDBOARD_APP_H_
