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
 * This is a sample app for the Cardboard SDK. It loads a simple environment and
 * objects that you can click on.
 *
 * It supports two rendering modes:
 *   - VR Mode (default): Uses CardboardHeadTracker for head pose, renders
 *     split-screen with lens distortion for use inside a Cardboard headset.
 *   - Finger Mode: Uses touch drag input for camera control (yaw/pitch),
 *     renders a single full-screen view with no lens distortion. Designed for
 *     devices without a gyroscope sensor.
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
   * Initializes any GL-related objects. This should be called on the rendering
   * thread with a valid GL context.
   *
   * @param env The JNI environment.
   */
  void OnSurfaceCreated(JNIEnv* env);

  /**
   * Sets screen parameters.
   *
   * @param width Screen width
   * @param height Screen height
   */
  void SetScreenParams(int width, int height);

  /**
   * Draws the scene. This should be called on the rendering thread.
   */
  void OnDrawFrame();

  /**
   * Hides the target object if it's being targeted.
   */
  void OnTriggerEvent();

  /**
   * Pauses head tracking.
   */
  void OnPause();

  /**
   * Resumes head tracking.
   */
  void OnResume();

  /**
   * Allows user to switch viewer.
   */
  void SwitchViewer();

  /**
   * Enables or disables finger (touch drag) mode.
   *
   * In finger mode the Cardboard head tracker is paused, lens distortion and
   * split-screen rendering are disabled, and the camera orientation is driven
   * entirely by accumulated touch drag deltas.
   *
   * @param enabled true to activate finger mode, false to return to VR mode.
   */
  void SetFingerMode(bool enabled);

  /**
   * Applies an incremental touch drag to update the camera orientation.
   * Only has an effect when finger mode is active.
   *
   * @param dx Horizontal drag delta in pixels (positive = drag right).
   * @param dy Vertical drag delta in pixels (positive = drag down).
   */
  void OnTouchDrag(float dx, float dy);

  /**
   * Switches the robot billboard to display a different pose from the
   * sprite sheet.
   *
   * The sprite sheet is a 4-column × 3-row grid (SPRITE_COLS × SPRITE_ROWS).
   * poseIndex is a row-major index: 0 = top-left cell, 11 = bottom-right.
   * The UV rectangle for the selected cell is computed once here and stored
   * in sprite_u0_, sprite_v0_, sprite_u1_, sprite_v1_ for use in DrawTarget().
   *
   * @param poseIndex 0-based index in [0, SPRITE_COLS * SPRITE_ROWS).
   */
  void SetRobotPose(int poseIndex);

 private:
  /**
   * Default near clip plane z-axis coordinate.
   */
  static constexpr float kZNear = 0.1f;

  /**
   * Default far clip plane z-axis coordinate.
   */
  static constexpr float kZFar = 100.f;

  /**
   * Sensitivity scalar: degrees of rotation per pixel of drag.
   * Tune this value to taste (0.15 feels natural on most screen sizes).
   */
  static constexpr float kTouchRotationDegPerPixel = 0.15f;

  /**
   * Maximum pitch angle in degrees to prevent flipping upside-down.
   */
  static constexpr float kMaxPitchDeg = 85.0f;

  /**
   * Updates device parameters, if necessary.
   *
   * @return true if device parameters were successfully updated.
   */
  bool UpdateDeviceParams();

  /**
   * Initializes GL environment.
   */
  void GlSetup();

  /**
   * Deletes GL environment.
   */
  void GlTeardown();

  /**
   * Gets head's pose as a 4x4 matrix.
   *
   * In VR mode this queries the Cardboard head tracker.
   * In finger mode this builds a rotation matrix from touch_yaw_ / touch_pitch_.
   *
   * @return matrix containing head's pose.
   */
  Matrix4x4 GetPose();

  /**
   * Builds a rotation-only Matrix4x4 for the given yaw and pitch angles
   * (in radians). Used exclusively in finger mode.
   *
   * @param yaw   Rotation around the Y axis (left/right look), radians.
   * @param pitch Rotation around the X axis (up/down look), radians.
   * @return Combined rotation matrix.
   */
  Matrix4x4 GetFingerPoseMatrix(float yaw, float pitch);

  /**
   * Draws all world-space objects for the current eye / finger view.
   */
  void DrawWorld();

  /**
   * Draws the target object.
   */
  void DrawTarget();

  /**
   * Draws the room.
   */
  void DrawRoom();

  /**
   * Finds a new random position for the target object.
   */
  void HideTarget();

  /**
   * Checks if user is pointing or looking at the target object by calculating
   * whether the angle between the user's gaze and the vector pointing towards
   * the object is lower than some threshold.
   *
   * @return true if the user is pointing at the target object.
   */
  bool IsPointingAtTarget();

  // -------------------------------------------------------------------------
  // Core Cardboard / Android references
  // -------------------------------------------------------------------------
  jobject java_asset_mgr_;
  AAssetManager* asset_mgr_;

  CardboardHeadTracker* head_tracker_;
  CardboardLensDistortion* lens_distortion_;
  CardboardDistortionRenderer* distortion_renderer_;

  CardboardEyeTextureDescription left_eye_texture_description_;
  CardboardEyeTextureDescription right_eye_texture_description_;

  // -------------------------------------------------------------------------
  // Screen / device state
  // -------------------------------------------------------------------------
  bool screen_params_changed_;
  bool device_params_changed_;
  int screen_width_;
  int screen_height_;

  float projection_matrices_[2][16];
  float eye_matrices_[2][16];

  // -------------------------------------------------------------------------
  // OpenGL resources
  // -------------------------------------------------------------------------
  GLuint depthRenderBuffer_;
  GLuint framebuffer_;
  GLuint texture_;

  GLuint obj_program_;
  GLuint obj_position_param_;
  GLuint obj_uv_param_;
  GLuint obj_modelview_projection_param_;
  GLint  obj_uv_rect_param_;   // u_UVRect uniform — sprite cell selector

  // -------------------------------------------------------------------------
  // Scene matrices
  // -------------------------------------------------------------------------
  Matrix4x4 head_view_;
  Matrix4x4 model_target_;

  Matrix4x4 modelview_projection_target_;
  Matrix4x4 modelview_projection_room_;

  // -------------------------------------------------------------------------
  // Scene geometry
  // -------------------------------------------------------------------------
  TexturedMesh room_;
  Texture room_tex_;

  std::vector<TexturedMesh> target_object_meshes_;
  std::vector<Texture> target_object_not_selected_textures_;
  std::vector<Texture> target_object_selected_textures_;
  int cur_target_object_;

  // -------------------------------------------------------------------------
  // Finger mode state
  // -------------------------------------------------------------------------

  /** True when finger (touch drag) mode is active. */
  bool finger_mode_;

  /**
   * Accumulated yaw angle driven by horizontal touch drag, in radians.
   * Positive = looking right.
   */
  float touch_yaw_;

  /**
   * Accumulated pitch angle driven by vertical touch drag, in radians.
   * Positive = looking up. Clamped to ±kMaxPitchDeg.
   */
  float touch_pitch_;

  // -------------------------------------------------------------------------
  // Sprite-sheet pose state
  // -------------------------------------------------------------------------

  /** Number of columns in the sprite sheet grid. */
  static constexpr int kSpriteCols = 4;

  /** Number of rows in the sprite sheet grid. */
  static constexpr int kSpriteRows = 3;

  /** Total number of poses = kSpriteCols * kSpriteRows = 12. */
  static constexpr int kSpritePoseCount = kSpriteCols * kSpriteRows;

  /**
   * OpenGL texture handle for the full sprite sheet
   * (flexie_tour_poses_transparent.png).
   * Loaded once in OnSurfaceCreated; used every frame in DrawTarget().
   */
  Texture sprite_sheet_texture_;

  /**
   * UV rectangle of the currently selected pose cell within the sprite sheet.
   * Updated by SetRobotPose(). DrawTarget() passes these to the vertex shader
   * via a uniform so only the active cell is sampled.
   *
   * All four values are in [0, 1] (normalised texture coordinates).
   */
  float sprite_u0_;   ///< Left   U of the active cell.
  float sprite_u1_;   ///< Right  U of the active cell.
  float sprite_v0_;   ///< Top    V of the active cell (OpenGL origin = bottom).
  float sprite_v1_;   ///< Bottom V of the active cell.
};

}  // namespace ndk_hello_cardboard

#endif  // HELLO_CARDBOARD_ANDROID_SRC_MAIN_JNI_HELLO_CARDBOARD_APP_H_
