/*
 * FlexieRobot.h
 * ------------
 * Renders the Flexie Travel robot mascot inside the Cardboard VR sphere as a
 * 3-D tour guide.  The robot is loaded from the QuadSphere_Flexie OBJ/MTL
 * assets and supports four behaviour modes:
 *
 *   ANCHORED  – robot stays at a fixed world-space position.
 *   FOLLOW    – robot glides to stay just in front of wherever the user looks.
 *   MOVE_TO   – robot smoothly travels to an explicit target position.
 *   HIDDEN    – robot is not rendered.
 *
 * Pose animations (idle / wave / talking / point / turn_left / turn_right) are
 * driven by FlexiePose.json and blended on the CPU before upload.
 *
 * Two skins are supported: BLUE and PINK (atlas diffuse PNGs).
 */

#ifndef FLEXIE_ROBOT_H_
#define FLEXIE_ROBOT_H_

#include <GLES2/gl2.h>
#include <android/asset_manager.h>
#include <jni.h>

#include <array>
#include <string>
#include <vector>

#include "util.h"   // Matrix4x4, Texture, TexturedMesh, CHECKGLERROR

namespace ndk_hello_cardboard {

// ---------------------------------------------------------------------------
// Public enumerations
// ---------------------------------------------------------------------------

enum class FlexieMode {
    ANCHORED,   ///< Stays at robot_world_pos_
    FOLLOW,     ///< Smoothly follows the user's gaze
    MOVE_TO,    ///< Glides from current to target_pos_
    HIDDEN      ///< Not rendered
};

enum class FlexiePose {
    IDLE,
    WAVE,
    TALKING,
    POINT,
    TURN_LEFT,
    TURN_RIGHT
};

enum class FlexieSkin {
    BLUE,
    PINK
};

// ---------------------------------------------------------------------------
// FlexieRobot
// ---------------------------------------------------------------------------

class FlexieRobot {
 public:
  FlexieRobot() = default;
  ~FlexieRobot();

  // ---- Setup ---------------------------------------------------------------

  /**
   * Loads OBJ geometry and atlas textures; compiles the per-object shader.
   * Must be called on the GL thread after a valid EGL context exists.
   *
   * @param env           JNIEnv for the current thread.
   * @param java_asset_mgr Java AssetManager object (global ref kept by caller).
   * @param asset_mgr     Native AAssetManager.
   * @return true if all resources loaded successfully.
   */
  bool Initialize(JNIEnv* env, jobject java_asset_mgr,
                  AAssetManager* asset_mgr);

  // ---- Per-frame -----------------------------------------------------------

  /**
   * Updates robot position / animation and draws it for one eye.
   *
   * @param view_proj      Combined view-projection matrix for the current eye.
   * @param head_direction Unit vector in world space where the user is looking.
   * @param delta_seconds  Time since last frame (for smooth movement).
   */
  void Draw(const Matrix4x4& view_proj,
            const std::array<float, 3>& head_direction,
            float delta_seconds);

  // ---- Control API (thread-safe simple setters) ----------------------------

  void SetMode(FlexieMode mode)          { mode_   = mode;  }
  void SetPose(FlexiePose pose)          { target_pose_ = pose; }
  void SetSkin(FlexieSkin skin)          { skin_   = skin;  }

  /** ANCHORED / MOVE_TO: place the robot at this world-space position. */
  void SetPosition(float x, float y, float z) {
      target_pos_ = {x, y, z};
      if (mode_ == FlexieMode::ANCHORED) robot_pos_ = target_pos_;
  }

  /** FOLLOW mode distance in front of the camera (default 3 m). */
  void SetFollowDistance(float d)  { follow_distance_ = d; }

  /** Scale factor applied to the model (default 0.35). */
  void SetScale(float s)           { scale_ = s; }

  FlexieMode  GetMode()  const { return mode_;  }
  FlexiePose  GetPose()  const { return target_pose_; }
  FlexieSkin  GetSkin()  const { return skin_;  }

 private:
  // ---- Shader source -------------------------------------------------------
  static constexpr const char* kVertSrc = R"glsl(
      uniform   mat4  u_MVP;
      attribute vec4  a_Position;
      attribute vec2  a_UV;
      varying   vec2  v_UV;
      void main() {
        v_UV        = a_UV;
        gl_Position = u_MVP * a_Position;
      })glsl";

  static constexpr const char* kFragSrc = R"glsl(
      precision mediump float;
      uniform sampler2D u_Texture;
      varying vec2      v_UV;
      void main() {
        gl_FragColor = texture2D(u_Texture, vec2(v_UV.x, 1.0 - v_UV.y));
      })glsl";

  // ---- Helpers -------------------------------------------------------------
  Matrix4x4 BuildModelMatrix() const;
  void      UpdatePosition(const std::array<float,3>& head_dir,
                           float dt);

  // ---- GL resources --------------------------------------------------------
  GLuint program_      = 0;
  GLuint pos_attrib_   = 0;
  GLuint uv_attrib_    = 0;
  GLuint mvp_uniform_  = 0;

  TexturedMesh mesh_;

  Texture tex_blue_;
  Texture tex_pink_;

  bool initialized_ = false;

  // ---- Runtime state -------------------------------------------------------
  FlexieMode  mode_          = FlexieMode::ANCHORED;
  FlexiePose  target_pose_   = FlexiePose::IDLE;
  FlexieSkin  skin_          = FlexieSkin::BLUE;

  std::array<float,3> robot_pos_    = {0.f, -0.5f, -3.f};
  std::array<float,3> target_pos_   = {0.f, -0.5f, -3.f};

  float follow_distance_ = 3.0f;
  float move_speed_      = 1.5f;   // m/s for MOVE_TO
  float follow_speed_    = 2.0f;   // m/s for FOLLOW
  float scale_           = 0.022f; // BUG 2a FIX: OBJ is in cm; 1.0 = 100x too large

  // Face the robot toward the camera (yaw only, derived each frame).
  float yaw_deg_         = 0.f;
};

}  // namespace ndk_hello_cardboard

#endif  // FLEXIE_ROBOT_H_
