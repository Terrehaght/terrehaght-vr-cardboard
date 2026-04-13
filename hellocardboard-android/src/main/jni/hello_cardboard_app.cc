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
#include <GLES2/gl2ext.h>   // GL_TEXTURE_EXTERNAL_OES, GL_OES_EGL_image_external

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

#include "cardboard.h"

namespace ndk_hello_cardboard {

namespace {

constexpr uint64_t kPredictionTimeWithoutVsyncNanos = 50000000;
constexpr int      kVelocityFilterCutoffFrequency   = 6;

// ---------------------------------------------------------------------------
// Image path – standard sampler2D
// The Y-flip in the fragment shader corrects for GLUtils.texImage2D uploading
// the Android bitmap with row-0 at the top (inverted relative to GL's origin).
// ---------------------------------------------------------------------------
constexpr const char* kImgVertexShader = R"glsl(
    uniform mat4 u_MVP;
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;
    void main() {
      v_UV = a_UV;
      gl_Position = u_MVP * a_Position;
    })glsl";

constexpr const char* kImgFragmentShader = R"glsl(
    precision mediump float;
    uniform sampler2D u_Texture;
    varying vec2 v_UV;
    void main() {
      // Flip Y: Android bitmap row-0 == top of image, but GL textures expect
      // row-0 at the bottom, so north-pole (v=0) must sample at gl_v=1.
      gl_FragColor = texture2D(u_Texture, vec2(v_UV.x, 1.0 - v_UV.y));
    })glsl";

// ---------------------------------------------------------------------------
// Video path – GL_OES_EGL_image_external / samplerExternalOES
// SurfaceTexture feeds frames in the EGL-image convention (Y-up), so the same
// V-flip keeps north-pole at the top of the equirectangular video.
// ---------------------------------------------------------------------------
constexpr const char* kOesVertexShader = R"glsl(
    uniform mat4 u_MVP;
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;
    void main() {
      v_UV = a_UV;
      gl_Position = u_MVP * a_Position;
    })glsl";

constexpr const char* kOesFragmentShader = R"glsl(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    uniform samplerExternalOES u_Texture;
    varying vec2 v_UV;
    void main() {
      gl_FragColor = texture2D(u_Texture, vec2(v_UV.x, 1.0 - v_UV.y));
    })glsl";

// ---------------------------------------------------------------------------
// Helper: build a 4x4 column-major translation matrix stored as float[16]
// ---------------------------------------------------------------------------
static inline void MakeTranslation(float tx, float ty, float tz, float out[16]) {
  // Column-major identity + translation
  out[ 0]=1; out[ 1]=0; out[ 2]=0; out[ 3]=0;
  out[ 4]=0; out[ 5]=1; out[ 6]=0; out[ 7]=0;
  out[ 8]=0; out[ 9]=0; out[10]=1; out[11]=0;
  out[12]=tx; out[13]=ty; out[14]=tz; out[15]=1;
}

// ---------------------------------------------------------------------------
// Helper: build a uniform-scale 4x4 matrix stored as float[16]
// ---------------------------------------------------------------------------
static inline void MakeScale(float s, float out[16]) {
  memset(out, 0, 16 * sizeof(float));
  out[0]=s; out[5]=s; out[10]=s; out[15]=1;
}

// ---------------------------------------------------------------------------
// Helper: multiply two column-major 4x4 matrices: result = a * b
// ---------------------------------------------------------------------------
static inline void MatMul4x4(const float a[16], const float b[16], float r[16]) {
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.f;
      for (int k = 0; k < 4; ++k) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      r[col * 4 + row] = sum;
    }
  }
}

// ---------------------------------------------------------------------------
// Helper: normalise a 3-vector in-place; returns the original length.
// ---------------------------------------------------------------------------
static inline float Normalize3(float v[3]) {
  float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
  if (len > 1e-6f) { v[0]/=len; v[1]/=len; v[2]/=len; }
  return len;
}

// ---------------------------------------------------------------------------
// Helper: dot product of two 3-vectors.
// ---------------------------------------------------------------------------
static inline float Dot3(const float a[3], const float b[3]) {
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

// ---------------------------------------------------------------------------
// Helper: cross product c = a × b
// ---------------------------------------------------------------------------
static inline void Cross3(const float a[3], const float b[3], float c[3]) {
  c[0] = a[1]*b[2] - a[2]*b[1];
  c[1] = a[2]*b[0] - a[0]*b[2];
  c[2] = a[0]*b[1] - a[1]*b[0];
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

HelloCardboardApp::HelloCardboardApp(JavaVM* vm, jobject obj,
                                     jobject asset_mgr_obj)
    : java_vm_(vm),
      java_asset_mgr_(nullptr),
      asset_mgr_(nullptr),
      head_tracker_(nullptr),
      lens_distortion_(nullptr),
      distortion_renderer_(nullptr),
      screen_params_changed_(false),
      device_params_changed_(false),
      screen_width_(0),
      screen_height_(0),
      depthRenderBuffer_(0),
      framebuffer_(0),
      texture_(0),
      img_program_(0),
      img_position_param_(0),
      img_uv_param_(0),
      img_mvp_param_(0),
      oes_program_(0),
      oes_position_param_(0),
      oes_uv_param_(0),
      oes_mvp_param_(0),
      sphere_vbo_pos_(0),
      sphere_vbo_uv_(0),
      sphere_ibo_(0),
      sphere_index_count_(0),
      video_texture_id_(0),
      surface_texture_ref_(nullptr),
      is_video_(false),
      media_initialized_(false),
      robot_mode_(RobotMode::ROBOT_FOLLOW) {
  JNIEnv* env;
  vm->GetEnv((void**)&env, JNI_VERSION_1_6);
  java_asset_mgr_ = env->NewGlobalRef(asset_mgr_obj);
  asset_mgr_ = AAssetManager_fromJava(env, asset_mgr_obj);

  robot_anchor_position_[0] = 0.f;
  robot_anchor_position_[1] = 0.f;
  robot_anchor_position_[2] = -kRobotDistance;

  Cardboard_initializeAndroid(vm, obj);
  head_tracker_ = CardboardHeadTracker_create();
  CardboardHeadTracker_setLowPassFilter(head_tracker_,
                                        kVelocityFilterCutoffFrequency);
}

HelloCardboardApp::~HelloCardboardApp() {
  CardboardHeadTracker_destroy(head_tracker_);
  CardboardLensDistortion_destroy(lens_distortion_);
  CardboardDistortionRenderer_destroy(distortion_renderer_);

  // Release the global JNI reference to the Java SurfaceTexture.
  if (surface_texture_ref_) {
    JNIEnv* env = nullptr;
    jint result = java_vm_->GetEnv((void**)&env, JNI_VERSION_1_6);
    bool attached = false;
    if (result == JNI_EDETACHED) {
      java_vm_->AttachCurrentThread(&env, nullptr);
      attached = true;
    }
    if (env) {
      env->DeleteGlobalRef(surface_texture_ref_);
    }
    if (attached) {
      java_vm_->DetachCurrentThread();
    }
    surface_texture_ref_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Media setup (called before GL init, from the main thread via JNI)
// ---------------------------------------------------------------------------

void HelloCardboardApp::SetMedia(const std::string& filename, bool is_video) {
  media_filename_    = filename;
  is_video_          = is_video;
  media_initialized_ = false;
}

// ---------------------------------------------------------------------------
// Video GL resources (called on the GL thread)
// ---------------------------------------------------------------------------

GLuint HelloCardboardApp::GetVideoTextureId() {
  if (video_texture_id_ == 0) {
    glGenTextures(1, &video_texture_id_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, video_texture_id_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
                    GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
                    GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
                    GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
                    GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    CHECKGLERROR("GetVideoTextureId");
  }
  return video_texture_id_;
}

void HelloCardboardApp::SetSurfaceTexture(JNIEnv* env, jobject surface_texture) {
  // Replace any existing global ref.
  if (surface_texture_ref_) {
    env->DeleteGlobalRef(surface_texture_ref_);
  }
  surface_texture_ref_ = surface_texture
                           ? env->NewGlobalRef(surface_texture)
                           : nullptr;
}

void HelloCardboardApp::UpdateVideoTexture(JNIEnv* env) {
  if (!surface_texture_ref_) return;

  jclass    cls = env->GetObjectClass(surface_texture_ref_);
  jmethodID mid = env->GetMethodID(cls, "updateTexImage", "()V");
  if (mid) {
    env->CallVoidMethod(surface_texture_ref_, mid);
  } else {
    LOGE("UpdateVideoTexture: updateTexImage method not found");
  }
  env->DeleteLocalRef(cls);
}

// ---------------------------------------------------------------------------
// Robot guide API
// ---------------------------------------------------------------------------

void HelloCardboardApp::SetRobotMode(RobotMode mode) {
  robot_mode_ = mode;
}

void HelloCardboardApp::SetRobotAnchorPosition() {
  // Extract camera forward direction from head_view_.
  // head_view_ is a view matrix: the camera's world-space forward is the
  // negated third column (or equivalently the third row of the transpose).
  // In column-major Matrix4x4: column 2 is (m[8], m[9], m[10], m[11]).
  // The view matrix R|t transforms world→camera; the camera forward in world
  // space is the inverse rotation applied to (0,0,-1), which is:
  //   forward_world = -R^T * (0,0,1) = (-R[0][2], -R[1][2], -R[2][2])
  // In our row-major Matrix4x4 (row i, col j = m[i][j]):
  //   forward_world = (-m[0][2], -m[1][2], -m[2][2])
  // We use GetMatrixFromGlArray convention – head_view_.m[row][col].

  float fwd[3] = {
    -head_view_.m[0][2],
    -head_view_.m[1][2],
    -head_view_.m[2][2]
  };
  // Normalise (should already be unit length, but be safe).
  Normalize3(fwd);

  robot_anchor_position_[0] = fwd[0] * kRobotDistance;
  robot_anchor_position_[1] = fwd[1] * kRobotDistance;
  robot_anchor_position_[2] = fwd[2] * kRobotDistance;
}

bool HelloCardboardApp::IsGazingAtRobot() const {
  if (robot_mode_ == RobotMode::ROBOT_HIDDEN) return false;

  // User's gaze forward vector in world space (negated third column of view).
  float gaze[3] = {
    -head_view_.m[0][2],
    -head_view_.m[1][2],
    -head_view_.m[2][2]
  };
  float gaze_len = sqrtf(gaze[0]*gaze[0] + gaze[1]*gaze[1] + gaze[2]*gaze[2]);
  if (gaze_len < 1e-6f) return false;
  gaze[0] /= gaze_len;
  gaze[1] /= gaze_len;
  gaze[2] /= gaze_len;

  // Robot's world-space direction from origin (camera stays at origin in
  // a 360 viewer centred on the sphere).
  float robot_dir[3] = {
    robot_anchor_position_[0],
    robot_anchor_position_[1],
    robot_anchor_position_[2]
  };
  float robot_len = sqrtf(robot_dir[0]*robot_dir[0] +
                           robot_dir[1]*robot_dir[1] +
                           robot_dir[2]*robot_dir[2]);
  if (robot_len < 1e-6f) return false;
  robot_dir[0] /= robot_len;
  robot_dir[1] /= robot_len;
  robot_dir[2] /= robot_len;

  float cos_angle = Dot3(gaze, robot_dir);
  // Clamp to [-1, 1] to guard against floating-point drift.
  if (cos_angle >  1.f) cos_angle =  1.f;
  if (cos_angle < -1.f) cos_angle = -1.f;
  float angle = acosf(cos_angle);
  return angle < kGazeThreshold;
}

void HelloCardboardApp::SetRobotSpeaking(bool speaking) {
  if (speaking) {
    flexie_animator_.BlendToPose("talking", 0.3f);
  } else {
    flexie_animator_.BlendToPose("idle", 0.5f);
  }
}

// ---------------------------------------------------------------------------
// OnSurfaceCreated – build shaders + sphere geometry + image texture + robot
// ---------------------------------------------------------------------------

void HelloCardboardApp::OnSurfaceCreated(JNIEnv* env) {
  // ---- Image shader program (sampler2D) ------------------------------------
  GLuint img_vert = LoadGLShader(GL_VERTEX_SHADER,   kImgVertexShader);
  GLuint img_frag = LoadGLShader(GL_FRAGMENT_SHADER, kImgFragmentShader);
  img_program_ = glCreateProgram();
  glAttachShader(img_program_, img_vert);
  glAttachShader(img_program_, img_frag);
  glLinkProgram(img_program_);
  img_position_param_ = glGetAttribLocation (img_program_, "a_Position");
  img_uv_param_       = glGetAttribLocation (img_program_, "a_UV");
  img_mvp_param_      = glGetUniformLocation(img_program_, "u_MVP");
  CHECKGLERROR("img_program");

  // ---- OES video shader program (samplerExternalOES) -----------------------
  GLuint oes_vert = LoadGLShader(GL_VERTEX_SHADER,   kOesVertexShader);
  GLuint oes_frag = LoadGLShader(GL_FRAGMENT_SHADER, kOesFragmentShader);
  oes_program_ = glCreateProgram();
  glAttachShader(oes_program_, oes_vert);
  glAttachShader(oes_program_, oes_frag);
  glLinkProgram(oes_program_);
  oes_position_param_ = glGetAttribLocation (oes_program_, "a_Position");
  oes_uv_param_       = glGetAttribLocation (oes_program_, "a_UV");
  oes_mvp_param_      = glGetUniformLocation(oes_program_, "u_MVP");
  CHECKGLERROR("oes_program");

  // ---- UV sphere geometry --------------------------------------------------
  GenerateSphere(kSphereSectors, kSphereStacks);

  // ---- Image texture -------------------------------------------------------
  // The OES texture for video is created lazily in GetVideoTextureId(), which
  // Java calls immediately after OnSurfaceCreated on the same GL thread.
  if (!is_video_ && !media_filename_.empty()) {
    HELLOCARDBOARD_CHECK(
        sphere_image_tex_.Initialize(env, java_asset_mgr_, media_filename_));
    media_initialized_ = true;
    LOGD("360 image loaded: %s", media_filename_.c_str());
  }

  // ---- Robot guide — FlexieAnimator ----------------------------------------
  bool flexie_ok = flexie_animator_.Initialize(
      asset_mgr_,
      img_position_param_,
      img_uv_param_,
      "flexie/QuadSphere_Flexie.obj",
      "flexie/FlexiePose.json",
      "flexie/flexie_atlas_diffuse_blue.png",
      "flexie/flexie_atlas_diffuse_pink.png");
  if (flexie_ok) {
    LOGD("FlexieAnimator loaded successfully");
  } else {
    LOGE("FlexieAnimator load failed");
  }

  CHECKGLERROR("OnSurfaceCreated");
}

// ---------------------------------------------------------------------------
// GenerateSphere
// ---------------------------------------------------------------------------

void HelloCardboardApp::GenerateSphere(int sectors, int stacks) {
  std::vector<float>    vertices, uvs;
  std::vector<GLushort> indices;

  const float R          = kSphereRadius;
  const float sectorStep = 2.0f * static_cast<float>(M_PI) / sectors;
  const float stackStep  = static_cast<float>(M_PI) / stacks;
  const float halfPi     = static_cast<float>(M_PI) * 0.5f;

  // Reserve to avoid repeated allocation
  const int vertexCount = (stacks + 1) * (sectors + 1);
  vertices.reserve(vertexCount * 3);
  uvs.reserve(vertexCount * 2);
  indices.reserve(stacks * sectors * 6);

  for (int i = 0; i <= stacks; ++i) {
    // stackAngle descends from +π/2 (north pole) to −π/2 (south pole)
    float stackAngle = halfPi - i * stackStep;
    float xzRadius   = R * cosf(stackAngle);
    float y          = R * sinf(stackAngle);

    for (int j = 0; j <= sectors; ++j) {
      float sectorAngle = j * sectorStep + halfPi;
      float x = xzRadius * cosf(sectorAngle);
      float z = xzRadius * sinf(sectorAngle);

      vertices.push_back(x);
      vertices.push_back(y);
      vertices.push_back(z);

      float u = 1.0f - static_cast<float>(j) / sectors;
      float v = static_cast<float>(i) / stacks;
      uvs.push_back(u);
      uvs.push_back(v);
    }
  }

  for (int i = 0; i < stacks; ++i) {
    int k1 = i * (sectors + 1);
    int k2 = k1 + sectors + 1;

    for (int j = 0; j < sectors; ++j, ++k1, ++k2) {
      if (i != 0) {
        indices.push_back(static_cast<GLushort>(k1));
        indices.push_back(static_cast<GLushort>(k1 + 1));
        indices.push_back(static_cast<GLushort>(k2));
      }
      if (i != stacks - 1) {
        indices.push_back(static_cast<GLushort>(k1 + 1));
        indices.push_back(static_cast<GLushort>(k2 + 1));
        indices.push_back(static_cast<GLushort>(k2));
      }
    }
  }

  sphere_index_count_ = static_cast<int>(indices.size());

  if (sphere_vbo_pos_ == 0) glGenBuffers(1, &sphere_vbo_pos_);
  glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_pos_);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
               vertices.data(), GL_STATIC_DRAW);

  if (sphere_vbo_uv_ == 0) glGenBuffers(1, &sphere_vbo_uv_);
  glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_uv_);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(uvs.size() * sizeof(float)),
               uvs.data(), GL_STATIC_DRAW);

  if (sphere_ibo_ == 0) glGenBuffers(1, &sphere_ibo_);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(indices.size() * sizeof(GLushort)),
               indices.data(), GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  LOGD("GenerateSphere: %d vertices, %d indices",
       vertexCount, sphere_index_count_);
  CHECKGLERROR("GenerateSphere");
}

// ---------------------------------------------------------------------------
// SetScreenParams
// ---------------------------------------------------------------------------

void HelloCardboardApp::SetScreenParams(int width, int height) {
  screen_width_          = width;
  screen_height_         = height;
  screen_params_changed_ = true;
}

// ---------------------------------------------------------------------------
// OnDrawFrame (stereo loop – Cardboard pipeline untouched)
// ---------------------------------------------------------------------------

void HelloCardboardApp::OnDrawFrame() {
  if (!UpdateDeviceParams()) return;

  // Update head pose.
  head_view_ = GetPose();

  // In ROBOT_FOLLOW mode, update the robot's world position every frame so it
  // trails 25° below and 30° right of the current gaze direction.
  if (robot_mode_ == RobotMode::ROBOT_FOLLOW) {
    // Camera forward in world space (negated third column of view matrix).
    float fwd[3] = {
      -head_view_.m[0][2],
      -head_view_.m[1][2],
      -head_view_.m[2][2]
    };
    Normalize3(fwd);

    // Camera up vector in world space (second column of view matrix).
    float up[3] = {
      head_view_.m[0][1],
      head_view_.m[1][1],
      head_view_.m[2][1]
    };
    Normalize3(up);

    // Camera right vector = forward × up  (left-hand cross for right-hand coords)
    float right[3];
    Cross3(fwd, up, right);
    Normalize3(right);

    // Apply pitch offset (downward from gaze) using Rodrigues' rotation formula.
    // Rotate fwd around 'right' axis by -kRobotPitchOffsetDeg.
    const float pitchRad = kRobotPitchOffsetDeg * static_cast<float>(M_PI) / 180.f;
    const float cp = cosf(-pitchRad), sp = sinf(-pitchRad);
    float cross_p[3];
    Cross3(right, fwd, cross_p);
    float fwd_pitched[3] = {
      fwd[0]*cp + cross_p[0]*sp + right[0]*Dot3(right, fwd)*(1.f - cp),
      fwd[1]*cp + cross_p[1]*sp + right[1]*Dot3(right, fwd)*(1.f - cp),
      fwd[2]*cp + cross_p[2]*sp + right[2]*Dot3(right, fwd)*(1.f - cp)
    };
    Normalize3(fwd_pitched);

    // Rebuild up for the pitched forward.
    float up_pitched[3];
    Cross3(right, fwd_pitched, up_pitched);
    Normalize3(up_pitched);

    // Apply yaw offset (rightward) around the pitched-up axis.
    const float yawRad = kRobotYawOffsetDeg * static_cast<float>(M_PI) / 180.f;
    const float cy = cosf(yawRad), sy = sinf(yawRad);
    float cross_y[3];
    Cross3(up_pitched, fwd_pitched, cross_y);
    float dir[3] = {
      fwd_pitched[0]*cy + cross_y[0]*sy + up_pitched[0]*Dot3(up_pitched, fwd_pitched)*(1.f - cy),
      fwd_pitched[1]*cy + cross_y[1]*sy + up_pitched[1]*Dot3(up_pitched, fwd_pitched)*(1.f - cy),
      fwd_pitched[2]*cy + cross_y[2]*sy + up_pitched[2]*Dot3(up_pitched, fwd_pitched)*(1.f - cy)
    };
    Normalize3(dir);

    robot_anchor_position_[0] = dir[0] * kRobotDistance;
    robot_anchor_position_[1] = dir[1] * kRobotDistance;
    robot_anchor_position_[2] = dir[2] * kRobotDistance;
  }

  // Bind the offscreen framebuffer for the stereo render.
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Draw each eye's view.
  for (int eye = 0; eye < 2; ++eye) {
    glViewport(eye == kLeft ? 0 : screen_width_ / 2, 0,
               screen_width_ / 2, screen_height_);

    Matrix4x4 eye_matrix  = GetMatrixFromGlArray(eye_matrices_[eye]);
    Matrix4x4 eye_view    = eye_matrix * head_view_;
    Matrix4x4 proj_matrix = GetMatrixFromGlArray(projection_matrices_[eye]);

    // Sphere model is identity – centred at the world origin.
    modelview_projection_sphere_ = proj_matrix * eye_view;

    DrawSphere();

    // Draw robot guide after the sphere (per-eye for correct billboard IPD).
    if (robot_mode_ != RobotMode::ROBOT_HIDDEN) {
      DrawRobotGuide(eye_view, proj_matrix);
    }
  }

  // Lens-distortion composite pass (Cardboard, unchanged).
  CardboardDistortionRenderer_renderEyeToDisplay(
      distortion_renderer_, /* target_display = */ 0,
      /* x = */ 0, /* y = */ 0,
      screen_width_, screen_height_,
      &left_eye_texture_description_,
      &right_eye_texture_description_);

  CHECKGLERROR("OnDrawFrame");
}

// ---------------------------------------------------------------------------
// DrawSphere
// ---------------------------------------------------------------------------

void HelloCardboardApp::DrawSphere() {
  const GLuint program    = is_video_ ? oes_program_        : img_program_;
  const GLuint pos_param  = is_video_ ? oes_position_param_ : img_position_param_;
  const GLuint uv_param   = is_video_ ? oes_uv_param_       : img_uv_param_;
  const GLuint mvp_param  = is_video_ ? oes_mvp_param_      : img_mvp_param_;

  glUseProgram(program);

  std::array<float, 16> mvp = modelview_projection_sphere_.ToGlArray();
  glUniformMatrix4fv(mvp_param, 1, GL_FALSE, mvp.data());

  glActiveTexture(GL_TEXTURE0);
  if (is_video_) {
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, video_texture_id_);
  } else {
    sphere_image_tex_.Bind();
  }
  glUniform1i(glGetUniformLocation(program, "u_Texture"), 0);

  glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_pos_);
  glEnableVertexAttribArray(pos_param);
  glVertexAttribPointer(pos_param, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_uv_);
  glEnableVertexAttribArray(uv_param);
  glVertexAttribPointer(uv_param, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

  glDisable(GL_CULL_FACE);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo_);
  glDrawElements(GL_TRIANGLES, sphere_index_count_,
                 GL_UNSIGNED_SHORT, nullptr);

  glEnable(GL_CULL_FACE);

  glDisableVertexAttribArray(pos_param);
  glDisableVertexAttribArray(uv_param);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  CHECKGLERROR("DrawSphere");
}

// ---------------------------------------------------------------------------
// DrawRobotGuide
// ---------------------------------------------------------------------------
//
// Renders the FlexieAnimator robot at robot_anchor_position_ with a billboard
// transform so it always faces the camera.  Uses img_program_ (sampler2D).
//
// The billboard is built per-eye so the slight IPD camera shift is handled
// correctly in stereo.
// ---------------------------------------------------------------------------

void HelloCardboardApp::DrawRobotGuide(Matrix4x4 eye_view,
                                       Matrix4x4 proj_matrix) {
  // ---- Extract camera position in world space from the eye view matrix ----
  // eye_view = R | t  (view = R*(world - cam_pos)  →  cam_pos = -R^T * t)
  // Camera position in world space:
  //   cam_x = -(m[0][0]*m[0][3] + m[1][0]*m[1][3] + m[2][0]*m[2][3])
  // Using row-major Matrix4x4 m[row][col]:
  float cam_pos[3] = {
    -(eye_view.m[0][0]*eye_view.m[0][3] +
      eye_view.m[1][0]*eye_view.m[1][3] +
      eye_view.m[2][0]*eye_view.m[2][3]),
    -(eye_view.m[0][1]*eye_view.m[0][3] +
      eye_view.m[1][1]*eye_view.m[1][3] +
      eye_view.m[2][1]*eye_view.m[2][3]),
    -(eye_view.m[0][2]*eye_view.m[0][3] +
      eye_view.m[1][2]*eye_view.m[1][3] +
      eye_view.m[2][2]*eye_view.m[2][3])
  };

  // Robot position in world space.
  float rp[3] = {
    robot_anchor_position_[0],
    robot_anchor_position_[1],
    robot_anchor_position_[2]
  };

  // ---- Build billboard axes -----------------------------------------------
  // forward = normalise(camera_pos – robot_pos)  (robot faces camera)
  float fwd[3] = {
    cam_pos[0] - rp[0],
    cam_pos[1] - rp[1],
    cam_pos[2] - rp[2]
  };
  Normalize3(fwd);

  // World up hint = (0, 1, 0); fall back to (0, 0, 1) if nearly parallel.
  float world_up[3] = {0.f, 1.f, 0.f};
  if (fabsf(Dot3(fwd, world_up)) > 0.99f) {
    world_up[0] = 0.f; world_up[1] = 0.f; world_up[2] = 1.f;
  }

  // right = normalise(forward × up)
  float right[3];
  Cross3(fwd, world_up, right);
  Normalize3(right);

  // up = right × forward  (recompute for orthogonality)
  float up[3];
  Cross3(right, fwd, up);
  Normalize3(up);

  // ---- Build model matrix: billboard rotation + translation + scale --------
  // Column-major layout for GL:
  //   col0 = right * scale
  //   col1 = up    * scale
  //   col2 = fwd   * scale
  //   col3 = robot_pos (translation)

  // Scale matrix (uniform)
  float scale_m[16];
  MakeScale(kRobotScale, scale_m);

  // Rotation matrix (column-major, no scale yet)
  float rot_m[16] = {
    right[0], right[1], right[2], 0.f,  // col 0
    up[0],    up[1],    up[2],    0.f,  // col 1
    fwd[0],   fwd[1],   fwd[2],  0.f,  // col 2
    0.f,      0.f,      0.f,     1.f   // col 3
  };

  // Translation matrix
  float trans_m[16];
  MakeTranslation(rp[0], rp[1], rp[2], trans_m);

  // model = trans * rot * scale
  float rot_scale[16];
  MatMul4x4(rot_m, scale_m, rot_scale);

  float model_m[16];
  MatMul4x4(trans_m, rot_scale, model_m);

  // ---- Compute MVP --------------------------------------------------------
  std::array<float, 16> eye_view_gl = eye_view.ToGlArray();
  std::array<float, 16> proj_gl     = proj_matrix.ToGlArray();

  // mv = eye_view * model
  float mv[16];
  MatMul4x4(eye_view_gl.data(), model_m, mv);

  // mvp = proj * mv
  float mvp[16];
  MatMul4x4(proj_gl.data(), mv, mvp);

  // ---- Advance animation and draw via FlexieAnimator ----------------------

  // Compute seconds elapsed since the previous DrawRobotGuide call.
  static auto last_robot_time = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  float delta = std::chrono::duration<float>(now - last_robot_time).count();
  last_robot_time = now;

  flexie_animator_.Update(delta);

  // Enable face culling for the robot (outward-facing mesh segments).
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);

  flexie_animator_.Draw(mvp, img_program_, img_mvp_param_,
                        img_position_param_, img_uv_param_);

  CHECKGLERROR("DrawRobotGuide");
}

// ---------------------------------------------------------------------------
// Trigger event – no-op for the viewer
// ---------------------------------------------------------------------------

void HelloCardboardApp::OnTriggerEvent() {
  // Intentionally empty.
}

// ---------------------------------------------------------------------------
// Lifecycle (Cardboard pipeline, unchanged)
// ---------------------------------------------------------------------------

void HelloCardboardApp::OnPause() {
  CardboardHeadTracker_pause(head_tracker_);
}

void HelloCardboardApp::OnResume() {
  CardboardHeadTracker_resume(head_tracker_);
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

// ---------------------------------------------------------------------------
// UpdateDeviceParams (Cardboard pipeline, unchanged)
// ---------------------------------------------------------------------------

bool HelloCardboardApp::UpdateDeviceParams() {
  if (!screen_params_changed_ && !device_params_changed_) return true;

  uint8_t* buffer;
  int size;
  CardboardQrCode_getSavedDeviceParams(&buffer, &size);
  if (size == 0) return false;

  CardboardLensDistortion_destroy(lens_distortion_);
  lens_distortion_ = CardboardLensDistortion_create(
      buffer, size, screen_width_, screen_height_);
  CardboardQrCode_destroy(buffer);

  GlSetup();

  CardboardDistortionRenderer_destroy(distortion_renderer_);
  const CardboardOpenGlEsDistortionRendererConfig config{kGlTexture2D};
  distortion_renderer_ = CardboardOpenGlEs2DistortionRenderer_create(&config);

  CardboardMesh left_mesh, right_mesh;
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kLeft,  &left_mesh);
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kRight, &right_mesh);
  CardboardDistortionRenderer_setMesh(distortion_renderer_, &left_mesh,  kLeft);
  CardboardDistortionRenderer_setMesh(distortion_renderer_, &right_mesh, kRight);

  CardboardLensDistortion_getEyeFromHeadMatrix(
      lens_distortion_, kLeft,  eye_matrices_[0]);
  CardboardLensDistortion_getEyeFromHeadMatrix(
      lens_distortion_, kRight, eye_matrices_[1]);
  CardboardLensDistortion_getProjectionMatrix(
      lens_distortion_, kLeft,  kZNear, kZFar, projection_matrices_[0]);
  CardboardLensDistortion_getProjectionMatrix(
      lens_distortion_, kRight, kZNear, kZFar, projection_matrices_[1]);

  screen_params_changed_ = false;
  device_params_changed_ = false;

  CHECKGLERROR("UpdateDeviceParams");
  return true;
}

// ---------------------------------------------------------------------------
// GlSetup / GlTeardown (Cardboard pipeline, unchanged)
// ---------------------------------------------------------------------------

void HelloCardboardApp::GlSetup() {
  LOGD("GL SETUP");

  if (framebuffer_ != 0) GlTeardown();

  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
               screen_width_, screen_height_, 0,
               GL_RGB, GL_UNSIGNED_BYTE, nullptr);

  left_eye_texture_description_.texture   = texture_;
  left_eye_texture_description_.left_u    = 0;
  left_eye_texture_description_.right_u   = 0.5;
  left_eye_texture_description_.top_v     = 1;
  left_eye_texture_description_.bottom_v  = 0;

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
  if (framebuffer_ == 0) return;
  glDeleteRenderbuffers(1, &depthRenderBuffer_);
  depthRenderBuffer_ = 0;
  glDeleteFramebuffers(1, &framebuffer_);
  framebuffer_ = 0;
  glDeleteTextures(1, &texture_);
  texture_ = 0;
  CHECKGLERROR("GlTeardown");
}

// ---------------------------------------------------------------------------
// GetPose (Cardboard pipeline, unchanged)
// ---------------------------------------------------------------------------

Matrix4x4 HelloCardboardApp::GetPose() {
  std::array<float, 4> out_orientation;
  std::array<float, 3> out_position;
  CardboardHeadTracker_getPose(
      head_tracker_,
      GetBootTimeNano() + kPredictionTimeWithoutVsyncNanos,
      kLandscapeLeft,
      &out_position[0], &out_orientation[0]);
  return GetTranslationMatrix(out_position) *
         Quatf::FromXYZW(&out_orientation[0]).ToMatrix();
}

}  // namespace ndk_hello_cardboard
