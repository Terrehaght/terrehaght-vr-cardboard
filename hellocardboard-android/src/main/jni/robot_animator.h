#pragma once
/*
 * RobotAnimator.h
 * ---------------
 * Drives all sprite-sheet animation, floating bob, and smooth position
 * movement for the 2D billboard robot.
 *
 * HOW IT FITS IN:
 *   1. Add  `RobotAnimator robot_animator_;`  to HelloCardboardApp in
 *      hello_cardboard_app.h
 *   2. Add  `int obj_uv_scale_param_, obj_uv_offset_param_;`  to the same
 *      class for the new shader uniforms.
 *   3. Call Update(dt) once at the top of OnDrawFrame().
 *   4. Call GetUVTransform() + GetFloatBobOffset() when building the MVP.
 *
 * SPRITE SHEET LAYOUT  (robot_spritesheet.png, 4 cols × 6 rows):
 *   Row 0 – IDLE     : 4 frames, gentle body sway
 *   Row 1 – TALKING  : 4 frames, mouth closed → half-open → open → half
 *   Row 2 – WAVING   : 4 frames, right-hand raise cycle
 *   Row 3 – TURN_LEFT: 3 frames, body angled left  (col 3 = padding)
 *   Row 4 – TURN_RIGHT:3 frames, body angled right (col 3 = padding)
 *   Row 5 – MOVING   : 4 frames, glide / hover thruster effect
 */

#include <array>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Animation state enum — keep values in sync with Java constants in
// VrActivity.java (ROBOT_STATE_* fields).
// ---------------------------------------------------------------------------
enum class RobotState : int {
  IDLE        = 0,
  TALKING     = 1,
  WAVING      = 2,
  TURN_LEFT   = 3,
  TURN_RIGHT  = 4,
  MOVING      = 5,
};

// ---------------------------------------------------------------------------
class RobotAnimator {
 public:
  // Sprite sheet dimensions
  static constexpr int kCols = 4;
  static constexpr int kRows = 6;

  struct AnimClip {
    int   row;
    int   numFrames;
    float fps;
    bool  loop;      // false → hold last frame when done
  };

  // -------------------------------------------------------------------------
  // State control
  // -------------------------------------------------------------------------
  void SetState(RobotState state) {
    if (state_ == state) return;
    state_         = state;
    current_frame_ = 0;
    frame_timer_   = 0.f;
  }

  RobotState GetState() const { return state_; }

  // -------------------------------------------------------------------------
  // Per-frame update — call once per OnDrawFrame with wall-clock delta seconds
  // -------------------------------------------------------------------------
  void Update(float dt) {
    total_time_ += dt;

    // ---- Advance sprite frame ----
    const AnimClip& clip = kClips[static_cast<int>(state_)];
    frame_timer_ += dt;
    const float frame_dur = 1.0f / clip.fps;
    if (frame_timer_ >= frame_dur) {
      frame_timer_ -= frame_dur;
      if (clip.loop) {
        current_frame_ = (current_frame_ + 1) % clip.numFrames;
      } else {
        current_frame_ = std::min(current_frame_ + 1, clip.numFrames - 1);
      }
    }

    // ---- Advance position lerp ----
    if (is_moving_) {
      move_elapsed_ += dt;
      float t = std::min(move_elapsed_ / move_duration_, 1.0f);
      // Smooth-step easing (no jerk at start/end)
      t = t * t * (3.f - 2.f * t);
      for (int i = 0; i < 3; ++i)
        pos_current_[i] = pos_from_[i] + t * (pos_to_[i] - pos_from_[i]);

      if (move_elapsed_ >= move_duration_) {
        is_moving_    = false;
        pos_current_  = pos_to_;
        SetState(RobotState::IDLE);   // auto-return to idle on arrival
      }
    }
  }

  // -------------------------------------------------------------------------
  // UV transform — pass results to glUniform2f each draw call
  //
  //   glUniform2f(uv_scale_loc,  sx, sy);
  //   glUniform2f(uv_offset_loc, ox, oy);
  // -------------------------------------------------------------------------
  void GetUVTransform(float& out_ox, float& out_oy,
                      float& out_sx, float& out_sy) const {
    out_sx = 1.0f / kCols;
    out_sy = 1.0f / kRows;
    const AnimClip& clip = kClips[static_cast<int>(state_)];
    out_ox = current_frame_ * out_sx;
    out_oy = clip.row       * out_sy;
  }

  // -------------------------------------------------------------------------
  // Floating bob — add the returned value to the robot's world-space Y
  // -------------------------------------------------------------------------
  float GetFloatBobOffset() const {
    return kBobAmplitude * std::sin(2.0f * static_cast<float>(M_PI)
                                    * kBobFrequency * total_time_);
  }

  // -------------------------------------------------------------------------
  // Smooth movement to a new world-space position
  //   target   = {x, y, z}  (world space, without bob — bob is added on top)
  //   duration = seconds for the glide
  // -------------------------------------------------------------------------
  void MoveTo(std::array<float, 3> target, float duration_seconds) {
    pos_from_     = pos_current_;
    pos_to_       = target;
    move_elapsed_ = 0.f;
    move_duration_= (duration_seconds > 0.f ? duration_seconds : 0.001f);
    is_moving_    = true;
    SetState(RobotState::MOVING);
  }

  // Current interpolated position (bob offset NOT included — add separately)
  std::array<float, 3> GetCurrentPosition() const { return pos_current_; }
  bool IsMoving() const { return is_moving_; }

 private:
  // ---- Bob parameters ----
  static constexpr float kBobAmplitude = 0.06f;  // metres
  static constexpr float kBobFrequency = 0.7f;   // Hz

  // ---- Animation state ----
  RobotState state_         = RobotState::IDLE;
  int        current_frame_ = 0;
  float      frame_timer_   = 0.f;
  float      total_time_    = 0.f;

  // ---- Position lerp ----
  // Default: directly in front of the user, slightly above eye level
  std::array<float, 3> pos_current_ = { 0.f,  1.5f, -2.5f};
  std::array<float, 3> pos_from_    = { 0.f,  1.5f, -2.5f};
  std::array<float, 3> pos_to_      = { 0.f,  1.5f, -2.5f};
  float move_elapsed_  = 0.f;
  float move_duration_ = 1.f;
  bool  is_moving_     = false;

  // ---- Clip table (must stay aligned with RobotState enum order) ----
  //                       row  frames  fps    loop
  static constexpr AnimClip kClips[] = {
    { 0,  4,  2.0f, true  },  // IDLE
    { 1,  4,  8.0f, true  },  // TALKING  (fast mouth flap)
    { 2,  4,  6.0f, true  },  // WAVING
    { 3,  3,  4.0f, false },  // TURN_LEFT  (hold last frame)
    { 4,  3,  4.0f, false },  // TURN_RIGHT (hold last frame)
    { 5,  4,  5.0f, true  },  // MOVING
  };
};

// constexpr arrays need out-of-class definition in C++14
inline constexpr RobotAnimator::AnimClip RobotAnimator::kClips[];
