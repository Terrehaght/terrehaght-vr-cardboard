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

#include "hello_cardboard_app.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <array>
#include <cmath>
#include <fstream>

#include "cardboard.h"

namespace ndk_hello_cardboard {

namespace {

// The objects are about 1 meter in radius, so the min/max target distance are
// set so that the objects are always within the room (which is about 5 meters
// across) and the reticle is always closer than any objects.
constexpr float kMinTargetDistance = 2.5f;
constexpr float kMaxTargetDistance = 3.5f;
constexpr float kMinTargetHeight = 0.5f;
constexpr float kMaxTargetHeight = kMinTargetHeight + 3.0f;

constexpr float kDefaultFloorHeight = -1.7f;

// 6 Hz cutoff frequency for the velocity filter of the head tracker.
constexpr int kVelocityFilterCutoffFrequency = 6;

constexpr uint64_t kPredictionTimeWithoutVsyncNanos = 50000000;

// Angle threshold for determining whether the controller is pointing at the
// object.
constexpr float kAngleLimit = 0.2f;

// Number of different possible targets
constexpr int kTargetMeshCount = 1;

// Degrees-to-radians conversion.
constexpr float kDegToRad = static_cast<float>(M_PI) / 180.0f;

// Simple shaders to render .obj files without any lighting.
// The fragment shader samples RGBA so PNG alpha transparency works correctly
// with the GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA blend that is enabled in
// OnDrawFrame — transparent pixels in the robot PNG will be invisible.
constexpr const char* kObjVertexShader =
    R"glsl(
    uniform mat4 u_MVP;
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;

    void main() {
      v_UV = a_UV;
      gl_Position = u_MVP * a_Position;
    })glsl";

constexpr const char* kObjFragmentShader =
    R"glsl(
    precision mediump float;

    uniform sampler2D u_Texture;
    varying vec2 v_UV;

    void main() {
      // The y coordinate of this sample's textures is reversed compared to
      // what OpenGL expects, so we invert the y coordinate.
      gl_FragColor = texture2D(u_Texture, vec2(v_UV.x, 1.0 - v_UV.y));
    })glsl";

// ---------------------------------------------------------------------------
// Billboard helper
// ---------------------------------------------------------------------------
// Zeroing out the upper-left 3×3 of a model-view matrix (the rotation block)
// while leaving the translation column (column 3) intact causes the geometry
// to always face the camera regardless of head orientation.  This is the
// classic "spherical billboard" trick; it works in both VR (per-eye) and
// finger-drag mode because we apply it after multiplying by the view matrix
// so the world-space position is already baked into the translation column.
//
// Matrix layout used throughout this file: m[row][col], row-major math but
// stored/uploaded column-major for OpenGL via ToGlArray().
static Matrix4x4 ApplyBillboard(const Matrix4x4& mv) {
  Matrix4x4 result = mv;
  // Replace rotation with identity; keep translation (column 3) untouched.
  result.m[0][0] = 1.0f;  result.m[0][1] = 0.0f;  result.m[0][2] = 0.0f;
  result.m[1][0] = 0.0f;  result.m[1][1] = 1.0f;  result.m[1][2] = 0.0f;
  result.m[2][0] = 0.0f;  result.m[2][1] = 0.0f;  result.m[2][2] = 1.0f;
  return result;
}

}  // anonymous namespace

// -----------------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------------

HelloCardboardApp::HelloCardboardApp(JavaVM* vm, jobject obj,
                                     jobject asset_mgr_obj)
    : head_tracker_(nullptr),
      lens_distortion_(nullptr),
      distortion_renderer_(nullptr),
      screen_params_changed_(false),
      device_params_changed_(false),
      screen_width_(0),
      screen_height_(0),
      depthRenderBuffer_(0),
      framebuffer_(0),
      texture_(0),
      obj_program_(0),
      obj_position_param_(0),
      obj_uv_param_(0),
      obj_modelview_projection_param_(0),
      target_object_meshes_(kTargetMeshCount),
      target_object_not_selected_textures_(kTargetMeshCount),
      target_object_selected_textures_(kTargetMeshCount),
      cur_target_object_(RandomUniformInt(kTargetMeshCount)),
      // Finger mode starts disabled; yaw/pitch default to forward-facing (0,0).
      finger_mode_(false),
      touch_yaw_(0.0f),
      touch_pitch_(0.0f) {
  JNIEnv* env;
  vm->GetEnv((void**)&env, JNI_VERSION_1_6);
  java_asset_mgr_ = env->NewGlobalRef(asset_mgr_obj);
  asset_mgr_ = AAssetManager_fromJava(env, asset_mgr_obj);

  Cardboard_initializeAndroid(vm, obj);
  head_tracker_ = CardboardHeadTracker_create();
  CardboardHeadTracker_setLowPassFilter(head_tracker_,
                                        kVelocityFilterCutoffFrequency);
}

HelloCardboardApp::~HelloCardboardApp() {
  CardboardHeadTracker_destroy(head_tracker_);
  CardboardLensDistortion_destroy(lens_distortion_);
  CardboardDistortionRenderer_destroy(distortion_renderer_);
}

// -----------------------------------------------------------------------------
// Surface / screen
// -----------------------------------------------------------------------------

void HelloCardboardApp::OnSurfaceCreated(JNIEnv* env) {
  const int obj_vertex_shader =
      LoadGLShader(GL_VERTEX_SHADER, kObjVertexShader);
  const int obj_fragment_shader =
      LoadGLShader(GL_FRAGMENT_SHADER, kObjFragmentShader);

  obj_program_ = glCreateProgram();
  glAttachShader(obj_program_, obj_vertex_shader);
  glAttachShader(obj_program_, obj_fragment_shader);
  glLinkProgram(obj_program_);
  glUseProgram(obj_program_);

  CHECKGLERROR("Obj program");

  obj_position_param_ = glGetAttribLocation(obj_program_, "a_Position");
  obj_uv_param_ = glGetAttribLocation(obj_program_, "a_UV");
  obj_modelview_projection_param_ = glGetUniformLocation(obj_program_, "u_MVP");

  CHECKGLERROR("Obj program params");

  HELLOCARDBOARD_CHECK(room_.Initialize(obj_position_param_, obj_uv_param_,
                                        "CubeRoom.obj", asset_mgr_));
  HELLOCARDBOARD_CHECK(
      room_tex_.Initialize(env, java_asset_mgr_, "CubeRoom_BakedDiffuse.png"));

  // -------------------------------------------------------------------------
  // BILLBOARD TARGET — replaces the original Icosahedron with a flat quad.
  //
  // Steps to add your robot PNG:
  //   1. Export / save your robot image as a PNG with a transparent background
  //      (RGBA, not RGB).  The white product-photo background will show as
  //      a white rectangle; you need actual alpha transparency.
  //   2. Name the file  robot.png  and drop it in:
  //        app/src/main/assets/
  //   3. Rebuild.  Both "not selected" and "selected" states use the same
  //      texture here; add a second highlighted version later if desired.
  // -------------------------------------------------------------------------
  HELLOCARDBOARD_CHECK(target_object_meshes_[0].Initialize(
      obj_position_param_, obj_uv_param_, "RobotBillboard.obj", asset_mgr_));
  HELLOCARDBOARD_CHECK(target_object_not_selected_textures_[0].Initialize(
      env, java_asset_mgr_, "robot.png"));
  HELLOCARDBOARD_CHECK(target_object_selected_textures_[0].Initialize(
      env, java_asset_mgr_, "robot.png"));  // swap for a glowing version later

  // Target object first appears directly in front of user.
  model_target_ = GetTranslationMatrix({0.0f, 1.5f, kMinTargetDistance});

  CHECKGLERROR("OnSurfaceCreated");
}

void HelloCardboardApp::SetScreenParams(int width, int height) {
  screen_width_ = width;
  screen_height_ = height;
  screen_params_changed_ = true;
}

// -----------------------------------------------------------------------------
// Per-frame rendering
// -----------------------------------------------------------------------------

void HelloCardboardApp::OnDrawFrame() {

  // ------------------------------------------------------------------
  // FINGER MODE — single full-screen render, no distortion, no split
  // ------------------------------------------------------------------
  if (finger_mode_) {
    // Get the camera pose from accumulated touch yaw/pitch.
    head_view_ = GetPose();
    head_view_ =
        head_view_ * GetTranslationMatrix({0.0f, kDefaultFloorHeight, 0.0f});

    // Render directly to the default framebuffer (no offscreen FBO).
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screen_width_, screen_height_);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Build a standard perspective projection for a single eye / full screen.
    // 70° vertical FOV is comfortable for handheld use.
    const float aspect =
        static_cast<float>(screen_width_) / static_cast<float>(screen_height_);
    const float fov_y     = 70.0f * kDegToRad;
    const float tan_half  = std::tan(fov_y * 0.5f);

    // Column-major perspective matrix (same convention as Cardboard SDK).
    Matrix4x4 finger_projection;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        finger_projection.m[r][c] = 0.0f;

    finger_projection.m[0][0] = 1.0f / (aspect * tan_half);
    finger_projection.m[1][1] = 1.0f / tan_half;
    finger_projection.m[2][2] = -(kZFar + kZNear) / (kZFar - kZNear);
    finger_projection.m[2][3] = -1.0f;
    finger_projection.m[3][2] = -(2.0f * kZFar * kZNear) / (kZFar - kZNear);

    // Billboard: cancel view rotation so the quad always faces the camera.
    Matrix4x4 modelview_target =
        ApplyBillboard(head_view_ * model_target_);
    modelview_projection_target_ = finger_projection * modelview_target;
    modelview_projection_room_   = finger_projection * head_view_;

    DrawWorld();

    CHECKGLERROR("onDrawFrame (finger mode)");
    return;
  }

  // ------------------------------------------------------------------
  // VR MODE — split-screen with Cardboard distortion (original path)
  // ------------------------------------------------------------------
  if (!UpdateDeviceParams()) {
    return;
  }

  // Update Head Pose.
  head_view_ = GetPose();

  // Incorporate the floor height into the head_view
  head_view_ =
      head_view_ * GetTranslationMatrix({0.0f, kDefaultFloorHeight, 0.0f});

  // Bind buffer
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Draw eyes views
  for (int eye = 0; eye < 2; ++eye) {
    glViewport(eye == kLeft ? 0 : screen_width_ / 2, 0, screen_width_ / 2,
               screen_height_);

    Matrix4x4 eye_matrix = GetMatrixFromGlArray(eye_matrices_[eye]);
    Matrix4x4 eye_view = eye_matrix * head_view_;

    Matrix4x4 projection_matrix =
        GetMatrixFromGlArray(projection_matrices_[eye]);

    // Billboard: apply view transform to get eye-space position, then cancel
    // the rotation so the quad always faces this eye's camera.
    Matrix4x4 modelview_target =
        ApplyBillboard(eye_view * model_target_);
    modelview_projection_target_ = projection_matrix * modelview_target;
    modelview_projection_room_ = projection_matrix * eye_view;

    // Draw room and target
    DrawWorld();
  }

  // Render
  CardboardDistortionRenderer_renderEyeToDisplay(
      distortion_renderer_, /* target_display = */ 0, /* x = */ 0, /* y = */ 0,
      screen_width_, screen_height_, &left_eye_texture_description_,
      &right_eye_texture_description_);

  CHECKGLERROR("onDrawFrame");
}

// -----------------------------------------------------------------------------
// Input events
// -----------------------------------------------------------------------------

void HelloCardboardApp::OnTriggerEvent() {
  if (IsPointingAtTarget()) {
    HideTarget();
  }
}

void HelloCardboardApp::OnPause() {
  CardboardHeadTracker_pause(head_tracker_);
}

void HelloCardboardApp::OnResume() {
  // Only resume the hardware tracker if we are in VR mode.
  if (!finger_mode_) {
    CardboardHeadTracker_resume(head_tracker_);
  }

  // Parameters may have changed.
  device_params_changed_ = true;

  // Check for device parameters existence in external storage. If they're
  // missing, we must scan a Cardboard QR code and save the obtained parameters.
  uint8_t* buffer;
  int size;
  CardboardQrCode_getSavedDeviceParams(&buffer, &size);
  if (size == 0) {
    SwitchViewer();
  }
  CardboardQrCode_destroy(buffer);
}

void HelloCardboardApp::SwitchViewer() {
  CardboardQrCode_scanQrCodeAndSaveDeviceParams();
}

// -----------------------------------------------------------------------------
// Finger mode control
// -----------------------------------------------------------------------------

void HelloCardboardApp::SetFingerMode(bool enabled) {
  if (finger_mode_ == enabled) return;  // No change, nothing to do.

  finger_mode_ = enabled;

  if (finger_mode_) {
    // Entering finger mode: pause the IMU-based head tracker and reset the
    // camera to a neutral forward-facing orientation.
    CardboardHeadTracker_pause(head_tracker_);
    touch_yaw_   = 0.0f;
    touch_pitch_ = 0.0f;
    LOGD("Finger mode ENABLED — head tracker paused.");
  } else {
    // Returning to VR mode: resume the head tracker so sensor fusion restarts.
    CardboardHeadTracker_resume(head_tracker_);
    // Force a device-params refresh so the distortion renderer is ready.
    device_params_changed_ = true;
    LOGD("Finger mode DISABLED — head tracker resumed.");
  }
}

void HelloCardboardApp::OnTouchDrag(float dx, float dy) {
  if (!finger_mode_) return;

  // Convert pixel deltas to radians.
  //   dx > 0  → dragging right  → look right → increase yaw
  //   dy > 0  → dragging down   → look down  → decrease pitch
  touch_yaw_   += dx * kTouchRotationDegPerPixel * kDegToRad;
  touch_pitch_ -= dy * kTouchRotationDegPerPixel * kDegToRad;

  // Clamp pitch so the camera cannot flip past straight up or straight down.
  const float kMaxPitchRad = kMaxPitchDeg * kDegToRad;
  if (touch_pitch_ >  kMaxPitchRad) touch_pitch_ =  kMaxPitchRad;
  if (touch_pitch_ < -kMaxPitchRad) touch_pitch_ = -kMaxPitchRad;

  // Wrap yaw to [-π, π] to avoid floating-point drift over time.
  const float kTwoPi = 2.0f * static_cast<float>(M_PI);
  while (touch_yaw_ >  static_cast<float>(M_PI)) touch_yaw_ -= kTwoPi;
  while (touch_yaw_ < -static_cast<float>(M_PI)) touch_yaw_ += kTwoPi;
}

// -----------------------------------------------------------------------------
// Device / GL parameter management
// -----------------------------------------------------------------------------

bool HelloCardboardApp::UpdateDeviceParams() {
  // Checks if screen or device parameters changed
  if (!screen_params_changed_ && !device_params_changed_) {
    return true;
  }

  // Get saved device parameters
  uint8_t* buffer;
  int size;
  CardboardQrCode_getSavedDeviceParams(&buffer, &size);

  // If there are no parameters saved yet, returns false.
  if (size == 0) {
    return false;
  }

  CardboardLensDistortion_destroy(lens_distortion_);
  lens_distortion_ = CardboardLensDistortion_create(buffer, size, screen_width_,
                                                    screen_height_);

  CardboardQrCode_destroy(buffer);

  GlSetup();

  CardboardDistortionRenderer_destroy(distortion_renderer_);
  const CardboardOpenGlEsDistortionRendererConfig config{kGlTexture2D};
  distortion_renderer_ = CardboardOpenGlEs2DistortionRenderer_create(&config);

  CardboardMesh left_mesh;
  CardboardMesh right_mesh;
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kLeft,
                                            &left_mesh);
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kRight,
                                            &right_mesh);

  CardboardDistortionRenderer_setMesh(distortion_renderer_, &left_mesh, kLeft);
  CardboardDistortionRenderer_setMesh(distortion_renderer_, &right_mesh,
                                      kRight);

  // Get eye matrices
  CardboardLensDistortion_getEyeFromHeadMatrix(lens_distortion_, kLeft,
                                               eye_matrices_[0]);
  CardboardLensDistortion_getEyeFromHeadMatrix(lens_distortion_, kRight,
                                               eye_matrices_[1]);
  CardboardLensDistortion_getProjectionMatrix(lens_distortion_, kLeft, kZNear,
                                              kZFar, projection_matrices_[0]);
  CardboardLensDistortion_getProjectionMatrix(lens_distortion_, kRight, kZNear,
                                              kZFar, projection_matrices_[1]);

  screen_params_changed_ = false;
  device_params_changed_ = false;

  CHECKGLERROR("UpdateDeviceParams");

  return true;
}

void HelloCardboardApp::GlSetup() {
  LOGD("GL SETUP");

  if (framebuffer_ != 0) {
    GlTeardown();
  }

  // Create render texture.
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, screen_width_, screen_height_, 0,
               GL_RGB, GL_UNSIGNED_BYTE, 0);

  left_eye_texture_description_.texture = texture_;
  left_eye_texture_description_.left_u = 0;
  left_eye_texture_description_.right_u = 0.5;
  left_eye_texture_description_.top_v = 1;
  left_eye_texture_description_.bottom_v = 0;

  right_eye_texture_description_.texture = texture_;
  right_eye_texture_description_.left_u = 0.5;
  right_eye_texture_description_.right_u = 1;
  right_eye_texture_description_.top_v = 1;
  right_eye_texture_description_.bottom_v = 0;

  // Generate depth buffer to perform depth test.
  glGenRenderbuffers(1, &depthRenderBuffer_);
  glBindRenderbuffer(GL_RENDERBUFFER, depthRenderBuffer_);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, screen_width_,
                        screen_height_);
  CHECKGLERROR("Create Render buffer");

  // Create render target.
  glGenFramebuffers(1, &framebuffer_);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         texture_, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depthRenderBuffer_);

  CHECKGLERROR("GlSetup");
}

void HelloCardboardApp::GlTeardown() {
  if (framebuffer_ == 0) {
    return;
  }
  glDeleteRenderbuffers(1, &depthRenderBuffer_);
  depthRenderBuffer_ = 0;
  glDeleteFramebuffers(1, &framebuffer_);
  framebuffer_ = 0;
  glDeleteTextures(1, &texture_);
  texture_ = 0;

  CHECKGLERROR("GlTeardown");
}

// -----------------------------------------------------------------------------
// Pose / camera matrix
// -----------------------------------------------------------------------------

Matrix4x4 HelloCardboardApp::GetPose() {
  if (finger_mode_) {
    // Build pose entirely from accumulated touch yaw and pitch.
    return GetFingerPoseMatrix(touch_yaw_, touch_pitch_);
  }

  // Original VR path: query the Cardboard head tracker.
  std::array<float, 4> out_orientation;
  std::array<float, 3> out_position;
  CardboardHeadTracker_getPose(
      head_tracker_, GetBootTimeNano() + kPredictionTimeWithoutVsyncNanos,
      kLandscapeLeft, &out_position[0], &out_orientation[0]);
  return GetTranslationMatrix(out_position) *
         Quatf::FromXYZW(&out_orientation[0]).ToMatrix();
}

Matrix4x4 HelloCardboardApp::GetFingerPoseMatrix(float yaw, float pitch) {
  // Rotation order: Yaw (Y axis) then Pitch (X axis).
  // This matches a standard first-person "look around" camera.
  //
  // Ry = rotation about Y by yaw
  // Rx = rotation about X by pitch
  // pose = Ry * Rx

  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);

  // Ry (column-major):
  //  [ cy  0  sy  0 ]
  //  [  0  1   0  0 ]
  //  [-sy  0  cy  0 ]
  //  [  0  0   0  1 ]
  Matrix4x4 ry;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      ry.m[r][c] = 0.0f;
  ry.m[0][0] =  cy;
  ry.m[0][2] =  sy;
  ry.m[1][1] =  1.0f;
  ry.m[2][0] = -sy;
  ry.m[2][2] =  cy;
  ry.m[3][3] =  1.0f;

  // Rx (column-major):
  //  [ 1   0   0  0 ]
  //  [ 0  cp  -sp  0 ]
  //  [ 0  sp   cp  0 ]
  //  [ 0   0    0  1 ]
  Matrix4x4 rx;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      rx.m[r][c] = 0.0f;
  rx.m[0][0] =  1.0f;
  rx.m[1][1] =  cp;
  rx.m[1][2] =  sp;
  rx.m[2][1] = -sp;
  rx.m[2][2] =  cp;
  rx.m[3][3] =  1.0f;

  // Combined: yaw first, then pitch.
  return ry * rx;
}

// -----------------------------------------------------------------------------
// Scene drawing
// -----------------------------------------------------------------------------

void HelloCardboardApp::DrawWorld() {
  DrawRoom();
  DrawTarget();
}

void HelloCardboardApp::DrawTarget() {
  glUseProgram(obj_program_);

  // The billboard is a flat quad — disable back-face culling so it renders
  // correctly regardless of winding order, then restore for the room.
  glDisable(GL_CULL_FACE);

  std::array<float, 16> target_array = modelview_projection_target_.ToGlArray();
  glUniformMatrix4fv(obj_modelview_projection_param_, 1, GL_FALSE,
                     target_array.data());

  if (IsPointingAtTarget()) {
    target_object_selected_textures_[cur_target_object_].Bind();
  } else {
    target_object_not_selected_textures_[cur_target_object_].Bind();
  }
  target_object_meshes_[cur_target_object_].Draw();

  glEnable(GL_CULL_FACE);  // Restore culling for subsequent room draw.

  CHECKGLERROR("DrawTarget");
}

void HelloCardboardApp::DrawRoom() {
  glUseProgram(obj_program_);

  std::array<float, 16> room_array = modelview_projection_room_.ToGlArray();
  glUniformMatrix4fv(obj_modelview_projection_param_, 1, GL_FALSE,
                     room_array.data());

  room_tex_.Bind();
  room_.Draw();

  CHECKGLERROR("DrawRoom");
}

void HelloCardboardApp::HideTarget() {
  cur_target_object_ = RandomUniformInt(kTargetMeshCount);

  float angle = RandomUniformFloat(-M_PI, M_PI);
  float distance = RandomUniformFloat(kMinTargetDistance, kMaxTargetDistance);
  float height = RandomUniformFloat(kMinTargetHeight, kMaxTargetHeight);
  std::array<float, 3> target_position = {std::cos(angle) * distance, height,
                                          std::sin(angle) * distance};

  model_target_ = GetTranslationMatrix(target_position);
}

bool HelloCardboardApp::IsPointingAtTarget() {
  // Compute vectors pointing towards the reticle and towards the target object
  // in head space.
  Matrix4x4 head_from_target = head_view_ * model_target_;

  const std::array<float, 4> unit_quaternion = {0.f, 0.f, 0.f, 1.f};
  const std::array<float, 4> point_vector = {0.f, 0.f, -1.f, 0.f};
  const std::array<float, 4> target_vector = head_from_target * unit_quaternion;

  float angle = AngleBetweenVectors(point_vector, target_vector);
  return angle < kAngleLimit;
}

}  // namespace ndk_hello_cardboard
