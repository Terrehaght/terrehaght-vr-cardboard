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

// ---------------------------------------------------------------------------
// Game-mode constants (unchanged)
// ---------------------------------------------------------------------------
constexpr float kMinTargetDistance = 2.5f;
constexpr float kMaxTargetDistance = 3.5f;
constexpr float kMinTargetHeight   = 0.5f;
constexpr float kMaxTargetHeight   = kMinTargetHeight + 3.0f;
constexpr float kDefaultFloorHeight = -1.7f;
constexpr int   kVelocityFilterCutoffFrequency = 6;
constexpr uint64_t kPredictionTimeWithoutVsyncNanos = 50000000;
constexpr float kAngleLimit      = 0.2f;
constexpr int   kTargetMeshCount = 1;
constexpr float kDegToRad = static_cast<float>(M_PI) / 180.0f;

// ---------------------------------------------------------------------------
// Sphere geometry constants
// ---------------------------------------------------------------------------
// Radius is large enough to fully surround the viewer.
constexpr float kSphereRadius = 50.0f;
// Subdivision quality: more stacks/slices = smoother sphere, more vertices.
constexpr int kSphereStacks = 48;
constexpr int kSphereSlices = 64;

// ---------------------------------------------------------------------------
// Shader sources
// ---------------------------------------------------------------------------

// ---- Standard obj shader (room + robot billboard) -------------------------
// The vertex shader remaps raw [0,1] UVs into the active sprite-sheet cell
// via u_UVRect.  For non-robot geometry pass u_UVRect = (0,0,1,1).
constexpr const char* kObjVertexShader =
    R"glsl(
    uniform mat4 u_MVP;
    uniform vec4 u_UVRect;   // (u0, v0, u1, v1) — sprite cell rect
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;

    void main() {
      v_UV = u_UVRect.xy + a_UV * (u_UVRect.zw - u_UVRect.xy);
      gl_Position = u_MVP * a_Position;
    })glsl";

// FIX: Removed the erroneous "1.0 - v_UV.y" V-flip that was causing all
// textures sampled through this shader (including the 360 image sphere)
// to appear upside down. UVs are now passed straight through to texture2D.
constexpr const char* kObjFragmentShader =
    R"glsl(
    precision mediump float;
    uniform sampler2D u_Texture;
    varying vec2 v_UV;

    void main() {
      gl_FragColor = texture2D(u_Texture, v_UV);
    })glsl";

// ---- 360 still-image sphere shader (GL_TEXTURE_2D) -----------------------
// No sprite remapping — UV maps directly over the full equirectangular image.
constexpr const char* kSphereVertexShader =
    R"glsl(
    uniform mat4 u_MVP;
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;

    void main() {
      v_UV = a_UV;
      gl_Position = u_MVP * a_Position;
    })glsl";

constexpr const char* kSphereFragmentShader =
    R"glsl(
    precision mediump float;
    uniform sampler2D u_Texture;
    varying vec2 v_UV;

    void main() {
      gl_FragColor = texture2D(u_Texture, v_UV);
    })glsl";

// ---- 360 video sphere shader (GL_TEXTURE_EXTERNAL_OES) -------------------
// Must use the samplerExternalOES extension and sampler type.
constexpr const char* kOesSphereVertexShader =
    R"glsl(
    uniform mat4 u_MVP;
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;

    void main() {
      v_UV = a_UV;
      gl_Position = u_MVP * a_Position;
    })glsl";

constexpr const char* kOesSphereFragmentShader =
    R"glsl(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    uniform samplerExternalOES u_Texture;
    varying vec2 v_UV;

    void main() {
      gl_FragColor = texture2D(u_Texture, v_UV);
    })glsl";

// ---------------------------------------------------------------------------
// Billboard helper (unchanged)
// ---------------------------------------------------------------------------
static Matrix4x4 ApplyBillboard(const Matrix4x4& mv) {
  Matrix4x4 result = mv;
  result.m[0][0] = 1.0f; result.m[0][1] = 0.0f; result.m[0][2] = 0.0f;
  result.m[1][0] = 0.0f; result.m[1][1] = 1.0f; result.m[1][2] = 0.0f;
  result.m[2][0] = 0.0f; result.m[2][1] = 0.0f; result.m[2][2] = 1.0f;
  return result;
}

}  // anonymous namespace

// =============================================================================
// Construction / destruction
// =============================================================================

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
      obj_uv_rect_param_(-1),
      oes_program_(0),
      oes_position_param_(0),
      oes_uv_param_(0),
      oes_mvp_param_(0),
      target_object_meshes_(kTargetMeshCount),
      target_object_not_selected_textures_(kTargetMeshCount),
      target_object_selected_textures_(kTargetMeshCount),
      cur_target_object_(RandomUniformInt(kTargetMeshCount)),
      finger_mode_(false),
      touch_yaw_(0.0f),
      touch_pitch_(0.0f),
      sprite_sheet_texture_(),
      sprite_u0_(0.0f),
      sprite_u1_(1.0f / static_cast<float>(kSpriteCols)),
      sprite_v0_(0.0f),
      sprite_v1_(1.0f / static_cast<float>(kSpriteRows)),
      // 360 media
      media_mode_(MediaMode::kNone),
      sphere_vbo_(0),
      sphere_uvo_(0),
      sphere_ibo_(0),
      sphere_index_count_(0),
      video_oes_texture_id_(0) {
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
  // Release sphere GPU buffers.
  if (sphere_vbo_) { glDeleteBuffers(1, &sphere_vbo_); }
  if (sphere_uvo_) { glDeleteBuffers(1, &sphere_uvo_); }
  if (sphere_ibo_) { glDeleteBuffers(1, &sphere_ibo_); }

  // Release OES texture.
  if (video_oes_texture_id_) {
    glDeleteTextures(1, &video_oes_texture_id_);
  }

  // Release OES program.
  if (oes_program_) { glDeleteProgram(oes_program_); }

  CardboardHeadTracker_destroy(head_tracker_);
  CardboardLensDistortion_destroy(lens_distortion_);
  CardboardDistortionRenderer_destroy(distortion_renderer_);
}

// =============================================================================
// Surface created
// =============================================================================

void HelloCardboardApp::OnSurfaceCreated(JNIEnv* env) {
  // ---- Compile the standard object shader (room + robot) ------------------
  const int obj_vertex_shader   = LoadGLShader(GL_VERTEX_SHADER,   kObjVertexShader);
  const int obj_fragment_shader = LoadGLShader(GL_FRAGMENT_SHADER, kObjFragmentShader);

  obj_program_ = glCreateProgram();
  glAttachShader(obj_program_, obj_vertex_shader);
  glAttachShader(obj_program_, obj_fragment_shader);
  glLinkProgram(obj_program_);
  glUseProgram(obj_program_);
  CHECKGLERROR("Obj program");

  obj_position_param_             = glGetAttribLocation(obj_program_,  "a_Position");
  obj_uv_param_                   = glGetAttribLocation(obj_program_,  "a_UV");
  obj_modelview_projection_param_ = glGetUniformLocation(obj_program_, "u_MVP");
  obj_uv_rect_param_              = glGetUniformLocation(obj_program_, "u_UVRect");
  CHECKGLERROR("Obj program params");

  // ---- Load game-mode assets (room + game object) -------------------------
  // These are always loaded so the game scene is available as a fallback even
  // if the user later calls SetMediaAsset.
  HELLOCARDBOARD_CHECK(room_.Initialize(obj_position_param_, obj_uv_param_,
                                        "CubeRoom.obj", asset_mgr_));
  HELLOCARDBOARD_CHECK(
      room_tex_.Initialize(env, java_asset_mgr_, "CubeRoom_BakedDiffuse.png"));

  HELLOCARDBOARD_CHECK(target_object_meshes_[0].Initialize(
      obj_position_param_, obj_uv_param_, "RobotBillboard.obj", asset_mgr_));

  // ---- Load robot sprite sheet --------------------------------------------
  HELLOCARDBOARD_CHECK(sprite_sheet_texture_.Initialize(
      env, java_asset_mgr_, "flexie_tour_poses_transparent.png"));

  HELLOCARDBOARD_CHECK(target_object_not_selected_textures_[0].Initialize(
      env, java_asset_mgr_, "flexie_tour_poses_transparent.png"));
  HELLOCARDBOARD_CHECK(target_object_selected_textures_[0].Initialize(
      env, java_asset_mgr_, "flexie_tour_poses_transparent.png"));

  // Place robot directly in front of the viewer.
  model_target_ = GetTranslationMatrix({0.0f, 1.5f, kMinTargetDistance});

  CHECKGLERROR("OnSurfaceCreated");
}

// =============================================================================
// Screen params
// =============================================================================

void HelloCardboardApp::SetScreenParams(int width, int height) {
  screen_width_          = width;
  screen_height_         = height;
  screen_params_changed_ = true;
}

// =============================================================================
// 360 media setup
// =============================================================================

void HelloCardboardApp::SetMediaAsset(JNIEnv* env,
                                      const std::string& assetPath,
                                      bool isVideo) {
  // Build sphere geometry (only once — reuse the same buffers for image/video).
  if (sphere_vbo_ == 0) {
    BuildSphereMesh(kSphereStacks, kSphereSlices);
    UploadSphereMesh();
  }

  if (isVideo) {
    // ---- Video mode: create OES texture, compile OES shader ---------------
    media_mode_ = MediaMode::kVideo;

    // Generate the OES texture that SurfaceTexture will write to.
    glGenTextures(1, &video_oes_texture_id_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, video_oes_texture_id_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    CHECKGLERROR("Create OES texture");

    // Compile the OES sphere shader (uses samplerExternalOES).
    const int oes_vert = LoadGLShader(GL_VERTEX_SHADER,   kOesSphereVertexShader);
    const int oes_frag = LoadGLShader(GL_FRAGMENT_SHADER, kOesSphereFragmentShader);
    oes_program_ = glCreateProgram();
    glAttachShader(oes_program_, oes_vert);
    glAttachShader(oes_program_, oes_frag);
    glLinkProgram(oes_program_);
    CHECKGLERROR("OES sphere program");

    oes_position_param_ = glGetAttribLocation(oes_program_,  "a_Position");
    oes_uv_param_       = glGetAttribLocation(oes_program_,  "a_UV");
    oes_mvp_param_      = glGetUniformLocation(oes_program_, "u_MVP");
    CHECKGLERROR("OES sphere params");

    LOGD("SetMediaAsset: video mode — OES texture id=%u path=%s",
         video_oes_texture_id_, assetPath.c_str());

  } else {
    // ---- Image mode: load texture from assets, reuse obj shader -----------
    media_mode_ = MediaMode::kImage;

    // Compile the dedicated 2D sphere shader (no sprite remapping).
    // We compile a separate minimal program so the sphere draw path is clean.
    // If you prefer to reuse obj_program_, pass u_UVRect=(0,0,1,1) there.
    // For clarity we keep a separate program here.
    //
    // NOTE: We reuse the obj shader here for simplicity.  The only difference
    // is we pass u_UVRect = (0, 0, 1, 1) so UVs are not remapped.
    // The sphere_image_texture_ is loaded below.
    HELLOCARDBOARD_CHECK(sphere_image_texture_.Initialize(
        env, java_asset_mgr_, assetPath));

    LOGD("SetMediaAsset: image mode — path=%s", assetPath.c_str());
  }
}

// ---------------------------------------------------------------------------
// Sphere mesh builder
// ---------------------------------------------------------------------------

void HelloCardboardApp::BuildSphereMesh(int stacks, int slices) {
  sphere_vertices_.clear();
  sphere_uvs_.clear();
  sphere_indices_.clear();

  // Generate vertices ring by ring (top = +Y pole, bottom = -Y pole).
  for (int stack = 0; stack <= stacks; ++stack) {
    const float phi = static_cast<float>(M_PI) *
                      (static_cast<float>(stack) / static_cast<float>(stacks));
    const float sinPhi = std::sin(phi);
    const float cosPhi = std::cos(phi);

    for (int slice = 0; slice <= slices; ++slice) {
      const float theta = 2.0f * static_cast<float>(M_PI) *
                          (static_cast<float>(slice) / static_cast<float>(slices));
      const float sinTheta = std::sin(theta);
      const float cosTheta = std::cos(theta);

      // Position on the unit sphere × radius.
      const float x =  cosTheta * sinPhi * kSphereRadius;
      const float y =  cosPhi           * kSphereRadius;
      const float z =  sinTheta * sinPhi * kSphereRadius;
      sphere_vertices_.push_back(x);
      sphere_vertices_.push_back(y);
      sphere_vertices_.push_back(z);

      // Equirectangular UV: U = longitude fraction, V = latitude fraction.
      // U goes 0→1 left-to-right; V goes 0→1 top-to-bottom.
      const float u = 1.0f - static_cast<float>(slice) / static_cast<float>(slices);
      const float v =        static_cast<float>(stack) / static_cast<float>(stacks);
      sphere_uvs_.push_back(u);
      sphere_uvs_.push_back(v);
    }
  }

  // Generate indices (two triangles per quad, winding flipped so normals point
  // inward — the camera is inside the sphere and must see the inner surface).
  for (int stack = 0; stack < stacks; ++stack) {
    for (int slice = 0; slice < slices; ++slice) {
      const uint16_t tl = static_cast<uint16_t>( stack      * (slices + 1) + slice);
      const uint16_t tr = static_cast<uint16_t>( stack      * (slices + 1) + slice + 1);
      const uint16_t bl = static_cast<uint16_t>((stack + 1) * (slices + 1) + slice);
      const uint16_t br = static_cast<uint16_t>((stack + 1) * (slices + 1) + slice + 1);

      // Flipped winding (inward-facing normals).
      sphere_indices_.push_back(tl);
      sphere_indices_.push_back(bl);
      sphere_indices_.push_back(tr);

      sphere_indices_.push_back(tr);
      sphere_indices_.push_back(bl);
      sphere_indices_.push_back(br);
    }
  }

  sphere_index_count_ = static_cast<GLsizei>(sphere_indices_.size());
  LOGD("BuildSphereMesh: %zu vertices, %zu indices",
       sphere_vertices_.size() / 3, sphere_indices_.size());
}

void HelloCardboardApp::UploadSphereMesh() {
  if (sphere_vbo_ == 0) {
    glGenBuffers(1, &sphere_vbo_);
    glGenBuffers(1, &sphere_uvo_);
    glGenBuffers(1, &sphere_ibo_);
  }

  glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_);
  glBufferData(GL_ARRAY_BUFFER,
               sphere_vertices_.size() * sizeof(float),
               sphere_vertices_.data(), GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, sphere_uvo_);
  glBufferData(GL_ARRAY_BUFFER,
               sphere_uvs_.size() * sizeof(float),
               sphere_uvs_.data(), GL_STATIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               sphere_indices_.size() * sizeof(uint16_t),
               sphere_indices_.data(), GL_STATIC_DRAW);

  // Unbind.
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  CHECKGLERROR("UploadSphereMesh");
}

// ---------------------------------------------------------------------------
// Sphere draw
// ---------------------------------------------------------------------------

void HelloCardboardApp::DrawSphere(Matrix4x4 mvp) {
  const bool isVideo = (media_mode_ == MediaMode::kVideo);

  if (isVideo) {
    // Use OES shader + OES texture.
    glUseProgram(oes_program_);

    std::array<float, 16> mvp_array = mvp.ToGlArray();
    glUniformMatrix4fv(oes_mvp_param_, 1, GL_FALSE, mvp_array.data());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, video_oes_texture_id_);

    GLint tex_uniform = glGetUniformLocation(oes_program_, "u_Texture");
    glUniform1i(tex_uniform, 0);

    glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_);
    glEnableVertexAttribArray(oes_position_param_);
    glVertexAttribPointer(oes_position_param_, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, sphere_uvo_);
    glEnableVertexAttribArray(oes_uv_param_);
    glVertexAttribPointer(oes_uv_param_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo_);
    glDrawElements(GL_TRIANGLES, sphere_index_count_, GL_UNSIGNED_SHORT, nullptr);

    glDisableVertexAttribArray(oes_position_param_);
    glDisableVertexAttribArray(oes_uv_param_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    CHECKGLERROR("DrawSphere (video)");

  } else {
    // Use the standard obj shader + GL_TEXTURE_2D.
    glUseProgram(obj_program_);

    std::array<float, 16> mvp_array = mvp.ToGlArray();
    glUniformMatrix4fv(obj_modelview_projection_param_, 1, GL_FALSE,
                       mvp_array.data());

    // Full UV rect — no sprite remapping for the sphere.
    glUniform4f(obj_uv_rect_param_, 0.0f, 0.0f, 1.0f, 1.0f);

    sphere_image_texture_.Bind();

    glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_);
    glEnableVertexAttribArray(obj_position_param_);
    glVertexAttribPointer(obj_position_param_, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, sphere_uvo_);
    glEnableVertexAttribArray(obj_uv_param_);
    glVertexAttribPointer(obj_uv_param_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo_);
    glDrawElements(GL_TRIANGLES, sphere_index_count_, GL_UNSIGNED_SHORT, nullptr);

    glDisableVertexAttribArray(obj_position_param_);
    glDisableVertexAttribArray(obj_uv_param_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    CHECKGLERROR("DrawSphere (image)");
  }
}

// =============================================================================
// Per-frame rendering
// =============================================================================

void HelloCardboardApp::OnDrawFrame() {

  // ------------------------------------------------------------------
  // FINGER MODE — single full-screen render, no distortion, no split
  // ------------------------------------------------------------------
  if (finger_mode_) {
    head_view_ = GetPose();
    head_view_ =
        head_view_ * GetTranslationMatrix({0.0f, kDefaultFloorHeight, 0.0f});

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screen_width_, screen_height_);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect =
        static_cast<float>(screen_width_) / static_cast<float>(screen_height_);
    const float fov_y    = 70.0f * kDegToRad;
    const float tan_half = std::tan(fov_y * 0.5f);

    Matrix4x4 finger_projection;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        finger_projection.m[r][c] = 0.0f;
    finger_projection.m[0][0] =  1.0f / (aspect * tan_half);
    finger_projection.m[1][1] =  1.0f / tan_half;
    finger_projection.m[2][2] = -(kZFar + kZNear) / (kZFar - kZNear);
    finger_projection.m[2][3] = -1.0f;
    finger_projection.m[3][2] = -(2.0f * kZFar * kZNear) / (kZFar - kZNear);

    Matrix4x4 modelview_target =
        ApplyBillboard(head_view_ * model_target_);
    modelview_projection_target_ = finger_projection * modelview_target;
    modelview_projection_room_   = finger_projection * head_view_;

    DrawWorld();

    CHECKGLERROR("onDrawFrame (finger mode)");
    return;
  }

  // ------------------------------------------------------------------
  // VR MODE — split-screen with Cardboard distortion
  // ------------------------------------------------------------------
  if (!UpdateDeviceParams()) {
    return;
  }

  head_view_ = GetPose();
  head_view_ =
      head_view_ * GetTranslationMatrix({0.0f, kDefaultFloorHeight, 0.0f});

  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  for (int eye = 0; eye < 2; ++eye) {
    glViewport(eye == kLeft ? 0 : screen_width_ / 2, 0, screen_width_ / 2,
               screen_height_);

    Matrix4x4 eye_matrix    = GetMatrixFromGlArray(eye_matrices_[eye]);
    Matrix4x4 eye_view      = eye_matrix * head_view_;
    Matrix4x4 projection_matrix = GetMatrixFromGlArray(projection_matrices_[eye]);

    Matrix4x4 modelview_target =
        ApplyBillboard(eye_view * model_target_);
    modelview_projection_target_ = projection_matrix * modelview_target;
    modelview_projection_room_   = projection_matrix * eye_view;

    DrawWorld();
  }

  CardboardDistortionRenderer_renderEyeToDisplay(
      distortion_renderer_, 0, 0, 0,
      screen_width_, screen_height_,
      &left_eye_texture_description_,
      &right_eye_texture_description_);

  CHECKGLERROR("onDrawFrame");
}

// =============================================================================
// DrawWorld / DrawBackground / DrawRobot
// =============================================================================

void HelloCardboardApp::DrawWorld() {
  DrawBackground();
  DrawRobot();
}

void HelloCardboardApp::DrawBackground() {
  if (media_mode_ == MediaMode::kNone) {
    // Default game mode — render the original cube room and game target.
    DrawRoom();
    DrawTarget();
  } else {
    // 360 media mode — render the equirectangular sphere.
    // Disable back-face culling: the viewer is inside the sphere, so the
    // "back" face (from the GPU's perspective) is exactly what we need to see.
    glDisable(GL_CULL_FACE);

    // The sphere is centred at the origin in world space and is large enough to
    // surround the viewer, so its model matrix is the identity.  The MVP is
    // simply projection * view (no separate model translation needed).
    DrawSphere(modelview_projection_room_);

    // Restore culling for the robot billboard drawn on top.
    glEnable(GL_CULL_FACE);
  }
}

void HelloCardboardApp::DrawRobot() {
  // The robot billboard is always drawn on top of whatever background is active.
  glUseProgram(obj_program_);
  glDisable(GL_CULL_FACE);

  std::array<float, 16> target_array = modelview_projection_target_.ToGlArray();
  glUniformMatrix4fv(obj_modelview_projection_param_, 1, GL_FALSE,
                     target_array.data());

  // Set sprite UV rect for the currently selected pose.
  glUniform4f(obj_uv_rect_param_, sprite_u0_, sprite_v0_, sprite_u1_, sprite_v1_);

  if (IsPointingAtTarget()) {
    target_object_selected_textures_[cur_target_object_].Bind();
  } else {
    target_object_not_selected_textures_[cur_target_object_].Bind();
  }
  target_object_meshes_[cur_target_object_].Draw();

  glEnable(GL_CULL_FACE);
  CHECKGLERROR("DrawRobot");
}

// =============================================================================
// Game-mode draw helpers (used only when media_mode_ == kNone)
// =============================================================================

void HelloCardboardApp::DrawTarget() {
  glUseProgram(obj_program_);
  glDisable(GL_CULL_FACE);

  std::array<float, 16> target_array = modelview_projection_target_.ToGlArray();
  glUniformMatrix4fv(obj_modelview_projection_param_, 1, GL_FALSE,
                     target_array.data());

  glUniform4f(obj_uv_rect_param_, sprite_u0_, sprite_v0_, sprite_u1_, sprite_v1_);

  if (IsPointingAtTarget()) {
    target_object_selected_textures_[cur_target_object_].Bind();
  } else {
    target_object_not_selected_textures_[cur_target_object_].Bind();
  }
  target_object_meshes_[cur_target_object_].Draw();

  glEnable(GL_CULL_FACE);
  CHECKGLERROR("DrawTarget");
}

void HelloCardboardApp::DrawRoom() {
  glUseProgram(obj_program_);

  std::array<float, 16> room_array = modelview_projection_room_.ToGlArray();
  glUniformMatrix4fv(obj_modelview_projection_param_, 1, GL_FALSE,
                     room_array.data());

  // Full UV rect for the room texture.
  glUniform4f(obj_uv_rect_param_, 0.0f, 0.0f, 1.0f, 1.0f);

  room_tex_.Bind();
  room_.Draw();

  CHECKGLERROR("DrawRoom");
}

void HelloCardboardApp::HideTarget() {
  cur_target_object_ = RandomUniformInt(kTargetMeshCount);

  float angle    = RandomUniformFloat(-M_PI, M_PI);
  float distance = RandomUniformFloat(kMinTargetDistance, kMaxTargetDistance);
  float height   = RandomUniformFloat(kMinTargetHeight, kMaxTargetHeight);
  std::array<float, 3> target_position = {
      std::cos(angle) * distance, height, std::sin(angle) * distance
  };

  model_target_ = GetTranslationMatrix(target_position);
}

bool HelloCardboardApp::IsPointingAtTarget() {
  Matrix4x4 head_from_target = head_view_ * model_target_;

  const std::array<float, 4> unit_quaternion = {0.f, 0.f, 0.f, 1.f};
  const std::array<float, 4> point_vector    = {0.f, 0.f, -1.f, 0.f};
  const std::array<float, 4> target_vector   = head_from_target * unit_quaternion;

  float angle = AngleBetweenVectors(point_vector, target_vector);
  return angle < kAngleLimit;
}

// =============================================================================
// Input events
// =============================================================================

void HelloCardboardApp::OnTriggerEvent() {
  if (media_mode_ == MediaMode::kNone && IsPointingAtTarget()) {
    HideTarget();
  }
  // In media mode the trigger has no default game effect; extend as needed.
}

void HelloCardboardApp::OnPause() {
  CardboardHeadTracker_pause(head_tracker_);
}

void HelloCardboardApp::OnResume() {
  if (!finger_mode_) {
    CardboardHeadTracker_resume(head_tracker_);
  }
  device_params_changed_ = true;

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

// =============================================================================
// Finger mode
// =============================================================================

void HelloCardboardApp::SetFingerMode(bool enabled) {
  if (finger_mode_ == enabled) return;

  finger_mode_ = enabled;

  if (finger_mode_) {
    CardboardHeadTracker_pause(head_tracker_);
    touch_yaw_   = 0.0f;
    touch_pitch_ = 0.0f;
    LOGD("Finger mode ENABLED");
  } else {
    CardboardHeadTracker_resume(head_tracker_);
    device_params_changed_ = true;
    LOGD("Finger mode DISABLED");
  }
}

void HelloCardboardApp::OnTouchDrag(float dx, float dy) {
  if (!finger_mode_) return;

  touch_yaw_   += dx * kTouchRotationDegPerPixel * kDegToRad;
  touch_pitch_ -= dy * kTouchRotationDegPerPixel * kDegToRad;

  const float kMaxPitchRad = kMaxPitchDeg * kDegToRad;
  if (touch_pitch_ >  kMaxPitchRad) touch_pitch_ =  kMaxPitchRad;
  if (touch_pitch_ < -kMaxPitchRad) touch_pitch_ = -kMaxPitchRad;

  const float kTwoPi = 2.0f * static_cast<float>(M_PI);
  while (touch_yaw_ >  static_cast<float>(M_PI)) touch_yaw_ -= kTwoPi;
  while (touch_yaw_ < -static_cast<float>(M_PI)) touch_yaw_ += kTwoPi;
}

// =============================================================================
// Sprite-sheet pose
// =============================================================================

void HelloCardboardApp::SetRobotPose(int poseIndex) {
  if (poseIndex < 0) poseIndex = 0;
  if (poseIndex >= kSpritePoseCount) poseIndex = kSpritePoseCount - 1;

  const int col = poseIndex % kSpriteCols;
  const int row = poseIndex / kSpriteCols;

  const float cellW = 1.0f / static_cast<float>(kSpriteCols);
  const float cellH = 1.0f / static_cast<float>(kSpriteRows);

  sprite_u0_ = col * cellW;
  sprite_u1_ = sprite_u0_ + cellW;
  sprite_v0_ = row * cellH;
  sprite_v1_ = sprite_v0_ + cellH;

  LOGD("SetRobotPose: index=%d  col=%d  row=%d  U=[%.3f,%.3f]  V=[%.3f,%.3f]",
       poseIndex, col, row, sprite_u0_, sprite_u1_, sprite_v0_, sprite_v1_);
}

// =============================================================================
// Device / GL parameter management (unchanged)
// =============================================================================

bool HelloCardboardApp::UpdateDeviceParams() {
  if (!screen_params_changed_ && !device_params_changed_) {
    return true;
  }

  uint8_t* buffer;
  int size;
  CardboardQrCode_getSavedDeviceParams(&buffer, &size);

  if (size == 0) {
    return false;
  }

  CardboardLensDistortion_destroy(lens_distortion_);
  lens_distortion_ = CardboardLensDistortion_create(buffer, size,
                                                    screen_width_,
                                                    screen_height_);
  CardboardQrCode_destroy(buffer);

  GlSetup();

  CardboardDistortionRenderer_destroy(distortion_renderer_);
  const CardboardOpenGlEsDistortionRendererConfig config{kGlTexture2D};
  distortion_renderer_ = CardboardOpenGlEs2DistortionRenderer_create(&config);

  CardboardMesh left_mesh;
  CardboardMesh right_mesh;
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kLeft,  &left_mesh);
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kRight, &right_mesh);

  CardboardDistortionRenderer_setMesh(distortion_renderer_, &left_mesh,  kLeft);
  CardboardDistortionRenderer_setMesh(distortion_renderer_, &right_mesh, kRight);

  CardboardLensDistortion_getEyeFromHeadMatrix(lens_distortion_, kLeft,  eye_matrices_[0]);
  CardboardLensDistortion_getEyeFromHeadMatrix(lens_distortion_, kRight, eye_matrices_[1]);
  CardboardLensDistortion_getProjectionMatrix(lens_distortion_, kLeft,
                                              kZNear, kZFar, projection_matrices_[0]);
  CardboardLensDistortion_getProjectionMatrix(lens_distortion_, kRight,
                                              kZNear, kZFar, projection_matrices_[1]);

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

  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, screen_width_, screen_height_,
               0, GL_RGB, GL_UNSIGNED_BYTE, 0);

  left_eye_texture_description_.texture  = texture_;
  left_eye_texture_description_.left_u   = 0;
  left_eye_texture_description_.right_u  = 0.5;
  left_eye_texture_description_.top_v    = 1;
  left_eye_texture_description_.bottom_v = 0;

  right_eye_texture_description_.texture  = texture_;
  right_eye_texture_description_.left_u   = 0.5;
  right_eye_texture_description_.right_u  = 1;
  right_eye_texture_description_.top_v    = 1;
  right_eye_texture_description_.bottom_v = 0;

  glGenRenderbuffers(1, &depthRenderBuffer_);
  glBindRenderbuffer(GL_RENDERBUFFER, depthRenderBuffer_);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                        screen_width_, screen_height_);
  CHECKGLERROR("Create Render buffer");

  glGenFramebuffers(1, &framebuffer_);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, texture_, 0);
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

// =============================================================================
// Pose / camera matrix
// =============================================================================

Matrix4x4 HelloCardboardApp::GetPose() {
  if (finger_mode_) {
    return GetFingerPoseMatrix(touch_yaw_, touch_pitch_);
  }

  std::array<float, 4> out_orientation;
  std::array<float, 3> out_position;
  CardboardHeadTracker_getPose(
      head_tracker_, GetBootTimeNano() + kPredictionTimeWithoutVsyncNanos,
      kLandscapeLeft, &out_position[0], &out_orientation[0]);
  return GetTranslationMatrix(out_position) *
         Quatf::FromXYZW(&out_orientation[0]).ToMatrix();
}

Matrix4x4 HelloCardboardApp::GetFingerPoseMatrix(float yaw, float pitch) {
  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);

  Matrix4x4 ry;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      ry.m[r][c] = 0.0f;
  ry.m[0][0] =  cy; ry.m[0][2] =  sy;
  ry.m[1][1] =  1.0f;
  ry.m[2][0] = -sy; ry.m[2][2] =  cy;
  ry.m[3][3] =  1.0f;

  Matrix4x4 rx;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      rx.m[r][c] = 0.0f;
  rx.m[0][0] =  1.0f;
  rx.m[1][1] =  cp; rx.m[1][2] =  sp;
  rx.m[2][1] = -sp; rx.m[2][2] =  cp;
  rx.m[3][3] =  1.0f;

  return ry * rx;
}

}  // namespace ndk_hello_cardboard