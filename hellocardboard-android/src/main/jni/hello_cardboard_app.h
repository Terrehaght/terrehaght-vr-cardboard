/*
 * hello_cardboard_app.h  (modified – Flexie robot tour-guide integration)
 *
 * Changes from original:
 *   + Included FlexieRobot.h
 *   + Added SetFlexieMode / SetFlexiePose / SetFlexieSkin / SetFlexiePosition
 *     public methods (called from new JNI bridge entries).
 *   + Added flexie_robot_ member + last_frame_time_ for delta-time tracking.
 *   + DrawSphere() split from new DrawFlexie() helper.
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
#include "FlexieRobot.h"   // ← NEW

namespace ndk_hello_cardboard {

class HelloCardboardApp {
 public:
  HelloCardboardApp(JavaVM* vm, jobject obj, jobject asset_mgr_obj);
  ~HelloCardboardApp();

  void OnSurfaceCreated(JNIEnv* env);
  void SetScreenParams(int width, int height);
  void OnDrawFrame();
  void OnTriggerEvent();
  void OnPause();
  void OnResume();
  void SwitchViewer();

  // ---- 360 media API -------------------------------------------------------
  void  SetMedia(const std::string& filename, bool is_video);
  GLuint GetVideoTextureId();
  void  SetSurfaceTexture(JNIEnv* env, jobject surface_texture);
  void  UpdateVideoTexture(JNIEnv* env);

  // ---- Flexie robot API (NEW) ----------------------------------------------

  /**
   * Sets the robot behaviour mode.
   * @param mode  0=ANCHORED, 1=FOLLOW, 2=MOVE_TO, 3=HIDDEN
   */
  void SetFlexieMode(int mode);

  /**
   * Sets the robot pose/animation.
   * @param pose  0=IDLE, 1=WAVE, 2=TALKING, 3=POINT, 4=TURN_LEFT, 5=TURN_RIGHT
   */
  void SetFlexiePose(int pose);

  /**
   * Sets the robot skin.
   * @param skin  0=BLUE, 1=PINK
   */
  void SetFlexieSkin(int skin);

  /**
   * Places the robot at explicit world-space coordinates.
   * In ANCHORED mode the robot snaps there immediately.
   * In MOVE_TO mode it glides there.
   */
  void SetFlexiePosition(float x, float y, float z);

  /**
   * Sets the distance the robot maintains in FOLLOW mode (default 3 m).
   */
  void SetFlexieFollowDistance(float d);

  /**
   * Sets the uniform scale of the robot model (default 0.35).
   */
  void SetFlexieScale(float s);

 private:
  // ---- Cardboard clip planes -----------------------------------------------
  static constexpr float kZNear = 0.1f;
  static constexpr float kZFar  = 100.f;

  // ---- Sphere parameters ---------------------------------------------------
  static constexpr float kSphereRadius  = 50.f;
  static constexpr int   kSphereSectors = 64;
  static constexpr int   kSphereStacks  = 32;

  // ---- Cardboard pipeline helpers ------------------------------------------
  bool      UpdateDeviceParams();
  void      GlSetup();
  void      GlTeardown();
  Matrix4x4 GetPose();

  // ---- 360 rendering -------------------------------------------------------
  void DrawSphere();
  void GenerateSphere(int sectors, int stacks);

  // ---- JVM / asset manager -------------------------------------------------
  JavaVM*        java_vm_;
  jobject        java_asset_mgr_;
  AAssetManager* asset_mgr_;

  // ---- Cardboard pipeline objects ------------------------------------------
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

  GLuint depthRenderBuffer_;
  GLuint framebuffer_;
  GLuint texture_;

  // ---- Per-frame matrices --------------------------------------------------
  Matrix4x4 head_view_;
  Matrix4x4 modelview_projection_sphere_;

  // ---- Image shader (sampler2D) --------------------------------------------
  GLuint img_program_;
  GLuint img_position_param_;
  GLuint img_uv_param_;
  GLuint img_mvp_param_;

  // ---- OES video shader (samplerExternalOES) --------------------------------
  GLuint oes_program_;
  GLuint oes_position_param_;
  GLuint oes_uv_param_;
  GLuint oes_mvp_param_;

  // ---- Sphere geometry VBOs -----------------------------------------------
  GLuint sphere_vbo_pos_;
  GLuint sphere_vbo_uv_;
  GLuint sphere_ibo_;
  int    sphere_index_count_;

  // ---- Image-mode texture --------------------------------------------------
  Texture sphere_image_tex_;

  // ---- Video-mode OES texture ----------------------------------------------
  GLuint  video_texture_id_;
  jobject surface_texture_ref_;

  // ---- Media state ---------------------------------------------------------
  std::string media_filename_;
  bool        is_video_;
  bool        media_initialized_;

  // ---- Flexie robot (NEW) --------------------------------------------------
  FlexieRobot flexie_;
  int64_t     last_frame_ns_;   // for per-frame delta-time
};

}  // namespace ndk_hello_cardboard

#endif  // HELLO_CARDBOARD_ANDROID_SRC_MAIN_JNI_HELLO_CARDBOARD_APP_H_
