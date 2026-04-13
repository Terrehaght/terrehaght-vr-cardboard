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
      media_initialized_(false) {
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
// OnSurfaceCreated – build shaders + sphere geometry + image texture
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

  CHECKGLERROR("OnSurfaceCreated");
}

// ---------------------------------------------------------------------------
// GenerateSphere
//
// Produces a UV sphere with:
//   • kSphereRadius = 50 m (well inside kZFar = 100 m)
//   • CCW winding ORDER as seen from the INSIDE (= CW from outside)
//     → back-face culling is disabled during DrawSphere to keep things simple.
//   • Equirectangular UV mapping:
//       u = 0 .. 1 mirrored so scanning left-to-right inside matches the image
//       v = 0 at north pole, 1 at south pole
//   • A π/2 sector-angle offset aligns u=0.5 (image centre) with the camera's
//     default forward direction (−Z axis in OpenGL).
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
    float xzRadius   = R * cosf(stackAngle);   // equatorial radius at this latitude
    float y          = R * sinf(stackAngle);   // vertical component

    for (int j = 0; j <= sectors; ++j) {
      // Add π/2 so that when j == sectors/2 (u == 0.5, image centre) the
      // point lands at (x=0, z=−R) which is the camera's −Z forward direction.
      float sectorAngle = j * sectorStep + halfPi;
      float x = xzRadius * cosf(sectorAngle);
      float z = xzRadius * sinf(sectorAngle);

      vertices.push_back(x);
      vertices.push_back(y);
      vertices.push_back(z);

      // Mirror U for inside-view: left side of image should be on the left
      // when you are standing inside the sphere looking forward.
      float u = 1.0f - static_cast<float>(j) / sectors;
      float v = static_cast<float>(i) / stacks;  // 0=north, 1=south
      uvs.push_back(u);
      uvs.push_back(v);
    }
  }

  // Index generation – CCW as seen from INSIDE (= reversed winding).
  // Degenerates at the poles are avoided by skipping the upper triangle on
  // the first row and the lower triangle on the last row.
  for (int i = 0; i < stacks; ++i) {
    int k1 = i * (sectors + 1);        // first vertex in current stack band
    int k2 = k1 + sectors + 1;         // first vertex in next stack band

    for (int j = 0; j < sectors; ++j, ++k1, ++k2) {
      // Upper triangle of the quad (skip the north-pole row)
      if (i != 0) {
        indices.push_back(static_cast<GLushort>(k1));
        indices.push_back(static_cast<GLushort>(k1 + 1));
        indices.push_back(static_cast<GLushort>(k2));
      }
      // Lower triangle of the quad (skip the south-pole row)
      if (i != stacks - 1) {
        indices.push_back(static_cast<GLushort>(k1 + 1));
        indices.push_back(static_cast<GLushort>(k2 + 1));
        indices.push_back(static_cast<GLushort>(k2));
      }
    }
  }

  sphere_index_count_ = static_cast<int>(indices.size());

  // Upload positions
  if (sphere_vbo_pos_ == 0) glGenBuffers(1, &sphere_vbo_pos_);
  glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_pos_);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
               vertices.data(), GL_STATIC_DRAW);

  // Upload UVs
  if (sphere_vbo_uv_ == 0) glGenBuffers(1, &sphere_vbo_uv_);
  glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_uv_);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(uvs.size() * sizeof(float)),
               uvs.data(), GL_STATIC_DRAW);

  // Upload indices
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

  // Update head pose.  No floor-height offset for a 360 viewer: the camera
  // must stay at the exact centre of the sphere at all times.
  head_view_ = GetPose();

  // Bind the offscreen framebuffer for the stereo render.
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Draw each eye's view of the sphere.
  for (int eye = 0; eye < 2; ++eye) {
    glViewport(eye == kLeft ? 0 : screen_width_ / 2, 0,
               screen_width_ / 2, screen_height_);

    Matrix4x4 eye_matrix  = GetMatrixFromGlArray(eye_matrices_[eye]);
    Matrix4x4 eye_view    = eye_matrix * head_view_;
    Matrix4x4 proj_matrix = GetMatrixFromGlArray(projection_matrices_[eye]);

    // Sphere model is identity – centred at the world origin.
    modelview_projection_sphere_ = proj_matrix * eye_view;

    DrawSphere();
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
  // Select the correct shader program and attribute/uniform locations.
  const GLuint program    = is_video_ ? oes_program_        : img_program_;
  const GLuint pos_param  = is_video_ ? oes_position_param_ : img_position_param_;
  const GLuint uv_param   = is_video_ ? oes_uv_param_       : img_uv_param_;
  const GLuint mvp_param  = is_video_ ? oes_mvp_param_      : img_mvp_param_;

  glUseProgram(program);

  // Upload the combined model-view-projection matrix.
  std::array<float, 16> mvp = modelview_projection_sphere_.ToGlArray();
  glUniformMatrix4fv(mvp_param, 1, GL_FALSE, mvp.data());

  // Bind the texture.
  glActiveTexture(GL_TEXTURE0);
  if (is_video_) {
    // OES external texture fed by SurfaceTexture / MediaPlayer.
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, video_texture_id_);
  } else {
    // Standard 2D texture loaded from the asset PNG.
    sphere_image_tex_.Bind();
  }
  glUniform1i(glGetUniformLocation(program, "u_Texture"), 0);

  // Stream vertex positions from the VBO.
  glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_pos_);
  glEnableVertexAttribArray(pos_param);
  glVertexAttribPointer(pos_param, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  // Stream UV coordinates from the VBO.
  glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_uv_);
  glEnableVertexAttribArray(uv_param);
  glVertexAttribPointer(uv_param, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

  // The sphere has inverted winding for inside rendering.  Disabling face
  // culling is the simplest way to handle this without duplicating geometry.
  glDisable(GL_CULL_FACE);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo_);
  glDrawElements(GL_TRIANGLES, sphere_index_count_,
                 GL_UNSIGNED_SHORT, nullptr);

  glEnable(GL_CULL_FACE);

  // Clean up attribute state.
  glDisableVertexAttribArray(pos_param);
  glDisableVertexAttribArray(uv_param);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  CHECKGLERROR("DrawSphere");
}

// ---------------------------------------------------------------------------
// Trigger event – no-op for the viewer
// ---------------------------------------------------------------------------

void HelloCardboardApp::OnTriggerEvent() {
  // Intentionally empty.  The Java layer handles touch events (e.g. back
  // navigation or play/pause toggling) without going through C++.
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
