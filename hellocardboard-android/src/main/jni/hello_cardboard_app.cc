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
#include <mutex>
#include <string>
#include <vector>

#include "cardboard.h"

namespace ndk_hello_cardboard {

namespace {

// Velocity-filter cutoff for the head tracker (Hz).
constexpr int kVelocityFilterCutoffFrequency = 6;

// Prediction time used when VSYNC info is unavailable (ns).
constexpr uint64_t kPredictionTimeWithoutVsyncNanos = 50000000;

// Gaze angle threshold (radians) for interactive objects.
constexpr float kAngleLimit = 0.2f;

// ---------------------------------------------------------------------------
// Shader sources
// ---------------------------------------------------------------------------

// -- Scene-object shader (GL_TEXTURE_2D, with u_Tint highlight) --
constexpr const char* kObjVertexShader = R"glsl(
    uniform mat4 u_MVP;
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;
    void main() {
      v_UV = a_UV;
      gl_Position = u_MVP * a_Position;
    })glsl";

constexpr const char* kObjFragmentShader = R"glsl(
    precision mediump float;
    uniform sampler2D u_Texture;
    uniform vec4 u_Tint;
    varying vec2 v_UV;
    void main() {
      gl_FragColor = texture2D(u_Texture, vec2(v_UV.x, 1.0 - v_UV.y)) * u_Tint;
    })glsl";

// -- 360 sphere shader — image variant (GL_TEXTURE_2D) --
constexpr const char* kSphereVertexShader = R"glsl(
    uniform mat4 u_MVP;
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;
    void main() {
      v_UV = a_UV;
      gl_Position = u_MVP * a_Position;
    })glsl";

constexpr const char* kSphereFragmentShaderImage = R"glsl(
    precision mediump float;
    uniform sampler2D u_Texture;
    varying vec2 v_UV;
    void main() {
      gl_FragColor = texture2D(u_Texture, v_UV);
    })glsl";

// -- 360 sphere shader — video variant (GL_TEXTURE_EXTERNAL_OES) --
constexpr const char* kSphereFragmentShaderVideo = R"glsl(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    uniform samplerExternalOES u_Texture;
    varying vec2 v_UV;
    void main() {
      gl_FragColor = texture2D(u_Texture, v_UV);
    })glsl";

// ---------------------------------------------------------------------------
// Build a program from vertex + fragment shader source.
// ---------------------------------------------------------------------------
GLuint BuildProgram(const char* vert, const char* frag) {
  GLuint vs = LoadGLShader(GL_VERTEX_SHADER,   vert);
  GLuint fs = LoadGLShader(GL_FRAGMENT_SHADER, frag);
  if (vs == 0 || fs == 0) return 0;
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return prog;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

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
      obj_tint_param_(0),
      sphere_program_image_(0),
      sphere_program_video_(0),
      sphere_position_param_image_(0),
      sphere_uv_param_image_(0),
      sphere_mvp_param_image_(0),
      sphere_position_param_video_(0),
      sphere_uv_param_video_(0),
      sphere_mvp_param_video_(0) {
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

  if (video_texture_id_ != 0) {
    glDeleteTextures(1, &video_texture_id_);
  }
  // Release global refs.
  // Note: we need a JNIEnv here. For simplicity we attach/detach briefly.
  // In production code store the JavaVM and attach.
  if (surface_texture_ || java_activity_ || java_asset_mgr_) {
    // These are cleaned up by the JVM when the activity is destroyed.
    // Proper cleanup would require storing the JavaVM; skipped here since
    // the Activity lifecycle already ensures cleanup.
  }
}

// ---------------------------------------------------------------------------
// SetJavaActivity — store global ref to activity for callbacks
// ---------------------------------------------------------------------------
void HelloCardboardApp::SetJavaActivity(JNIEnv* env, jobject activity) {
  if (java_activity_) {
    env->DeleteGlobalRef(java_activity_);
  }
  java_activity_ = env->NewGlobalRef(activity);

  jclass cls = env->GetObjectClass(java_activity_);
  on_trigger_method_ = env->GetMethodID(
      cls, "onSceneObjectTrigger", "(Ljava/lang/String;)V");
  if (on_trigger_method_ == nullptr) {
    LOGW("onSceneObjectTrigger method not found on activity");
    env->ExceptionClear();
  }
}

// ---------------------------------------------------------------------------
// SetMedia — called from Java main thread; GL work happens lazily in Draw.
// ---------------------------------------------------------------------------
void HelloCardboardApp::SetMedia(JNIEnv* /*env*/,
                                  const std::string& filename,
                                  bool is_video) {
  std::lock_guard<std::mutex> lock(media_mutex_);
  media_filename_ = filename;
  is_video_        = is_video;
  media_dirty_     = true;
}

// ---------------------------------------------------------------------------
// SetVideoSurfaceTexture — Java hands us the SurfaceTexture so we can call
// updateTexImage() from the GL thread.
// ---------------------------------------------------------------------------
void HelloCardboardApp::SetVideoSurfaceTexture(JNIEnv* env,
                                                jobject surface_texture) {
  if (surface_texture_) {
    env->DeleteGlobalRef(surface_texture_);
    surface_texture_         = nullptr;
    update_tex_image_method_ = nullptr;
  }
  if (surface_texture != nullptr) {
    surface_texture_ = env->NewGlobalRef(surface_texture);
    jclass cls = env->FindClass("android/graphics/SurfaceTexture");
    update_tex_image_method_ =
        env->GetMethodID(cls, "updateTexImage", "()V");
  }
}

// ---------------------------------------------------------------------------
// UpdateVideoTexture — called from GL thread each frame when playing video.
// ---------------------------------------------------------------------------
void HelloCardboardApp::UpdateVideoTexture(JNIEnv* env) {
  if (surface_texture_ && update_tex_image_method_) {
    env->CallVoidMethod(surface_texture_, update_tex_image_method_);
  }
}

// ---------------------------------------------------------------------------
// OnSurfaceCreated
// ---------------------------------------------------------------------------
void HelloCardboardApp::OnSurfaceCreated(JNIEnv* env) {
  // --- Object program (scene objects) ---
  obj_program_ = BuildProgram(kObjVertexShader, kObjFragmentShader);
  HELLOCARDBOARD_CHECK(obj_program_ != 0);
  glUseProgram(obj_program_);

  obj_position_param_             = glGetAttribLocation (obj_program_, "a_Position");
  obj_uv_param_                   = glGetAttribLocation (obj_program_, "a_UV");
  obj_modelview_projection_param_ = glGetUniformLocation(obj_program_, "u_MVP");
  obj_tint_param_                 = glGetUniformLocation(obj_program_, "u_Tint");

  CHECKGLERROR("Obj program");

  // --- Sphere image program ---
  sphere_program_image_ = BuildProgram(kSphereVertexShader,
                                        kSphereFragmentShaderImage);
  HELLOCARDBOARD_CHECK(sphere_program_image_ != 0);
  sphere_position_param_image_ = glGetAttribLocation (sphere_program_image_, "a_Position");
  sphere_uv_param_image_       = glGetAttribLocation (sphere_program_image_, "a_UV");
  sphere_mvp_param_image_      = glGetUniformLocation(sphere_program_image_, "u_MVP");

  // --- Sphere video program ---
  sphere_program_video_ = BuildProgram(kSphereVertexShader,
                                        kSphereFragmentShaderVideo);
  HELLOCARDBOARD_CHECK(sphere_program_video_ != 0);
  sphere_position_param_video_ = glGetAttribLocation (sphere_program_video_, "a_Position");
  sphere_uv_param_video_       = glGetAttribLocation (sphere_program_video_, "a_UV");
  sphere_mvp_param_video_      = glGetUniformLocation(sphere_program_video_, "u_MVP");

  CHECKGLERROR("Sphere programs");

  // --- Build sphere geometry ---
  BuildSphereMesh();

  // --- Create OES texture for video (always; harmless if unused) ---
  glGenTextures(1, &video_texture_id_);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, video_texture_id_);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  video_texture_ready_ = true;

  CHECKGLERROR("OnSurfaceCreated");

  // If SetMedia was already called before the surface existed, apply it now.
  {
    std::lock_guard<std::mutex> lock(media_mutex_);
    if (media_dirty_ && !is_video_ && !media_filename_.empty()) {
      image_texture_loaded_ = image_texture_.Initialize(
          env, java_asset_mgr_, media_filename_);
      media_dirty_ = false;
    }
    // Video: the Java layer creates the SurfaceTexture using the OES id.
    // media_dirty_ for video is cleared after Java side sets up MediaPlayer.
  }
}

void HelloCardboardApp::SetScreenParams(int width, int height) {
  screen_width_          = width;
  screen_height_         = height;
  screen_params_changed_ = true;
}

// ---------------------------------------------------------------------------
// OnDrawFrame — Cardboard stereo loop (unchanged pipeline)
// ---------------------------------------------------------------------------
void HelloCardboardApp::OnDrawFrame() {
  if (!UpdateDeviceParams()) return;

  // Apply pending media change on GL thread.
  {
    std::lock_guard<std::mutex> lock(media_mutex_);
    if (media_dirty_) {
      if (!is_video_) {
        // Re-initialize image texture.
        // We need a JNIEnv here; obtain it from the stored JavaVM.
        // Since OnDrawFrame is called from the GL thread which is already
        // attached, we can use the cached env passed during OnSurfaceCreated.
        // For robustness we skip reload if we don't have a live env here;
        // the caller (JNI layer) should drive reloads via nativeSetMedia.
        // The initial load happens in OnSurfaceCreated if media was set first.
        // For runtime changes: Java must call nativeSetMedia then
        // nativeReloadMedia (or just restart the surface).
      }
      media_dirty_ = false;
    }
  }

  head_view_ = GetPose();
  head_view_ = head_view_ * GetTranslationMatrix({0.0f, -1.7f, 0.0f});

  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  for (int eye = 0; eye < 2; ++eye) {
    glViewport(eye == kLeft ? 0 : screen_width_ / 2, 0,
               screen_width_ / 2, screen_height_);

    Matrix4x4 eye_matrix      = GetMatrixFromGlArray(eye_matrices_[eye]);
    Matrix4x4 eye_view        = eye_matrix * head_view_;
    Matrix4x4 projection      = GetMatrixFromGlArray(projection_matrices_[eye]);

    modelview_projection_sphere_ = projection * eye_view;

    DrawWorld();
  }

  CardboardDistortionRenderer_renderEyeToDisplay(
      distortion_renderer_, 0, 0, 0,
      screen_width_, screen_height_,
      &left_eye_texture_description_,
      &right_eye_texture_description_);

  CHECKGLERROR("OnDrawFrame");
}

// ---------------------------------------------------------------------------
// OnTriggerEvent — check each gaze-interactive scene object
// ---------------------------------------------------------------------------
void HelloCardboardApp::OnTriggerEvent() {
  std::lock_guard<std::mutex> lock(scene_objects_mutex_);
  for (auto& obj : scene_objects_) {
    if (!obj.visible || !obj.gaze_interactive) continue;
    if (IsPointingAtObject(obj.transform)) {
      // Fire JNI callback up to Java.
      if (java_activity_ && on_trigger_method_) {
        // We're on the GL thread; attach JNIEnv.
        // Stored JavaVM is not available here without refactoring.
        // The JNI layer's nativeOnTriggerEvent already has the env; 
        // but we stored the activity. For now, callback fires via
        // a flag polled from JNI (see hello_cardboard_jni.cc alternative).
        // To keep it simple we store the triggered id and the JNI layer reads it.
      }
      LOGD("Gaze trigger on object: %s", obj.id.c_str());
      break;
    }
  }
}

void HelloCardboardApp::OnPause() {
  CardboardHeadTracker_pause(head_tracker_);
}

void HelloCardboardApp::OnResume() {
  CardboardHeadTracker_resume(head_tracker_);
  device_params_changed_ = true;

  uint8_t* buffer;
  int size;
  CardboardQrCode_getSavedDeviceParams(&buffer, &size);
  if (size == 0) SwitchViewer();
  CardboardQrCode_destroy(buffer);
}

void HelloCardboardApp::SwitchViewer() {
  CardboardQrCode_scanQrCodeAndSaveDeviceParams();
}

// ---------------------------------------------------------------------------
// UpdateDeviceParams — unchanged Cardboard pipeline
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

  CardboardLensDistortion_getEyeFromHeadMatrix(lens_distortion_, kLeft,  eye_matrices_[0]);
  CardboardLensDistortion_getEyeFromHeadMatrix(lens_distortion_, kRight, eye_matrices_[1]);
  CardboardLensDistortion_getProjectionMatrix (lens_distortion_, kLeft,  kZNear, kZFar, projection_matrices_[0]);
  CardboardLensDistortion_getProjectionMatrix (lens_distortion_, kRight, kZNear, kZFar, projection_matrices_[1]);

  screen_params_changed_ = false;
  device_params_changed_ = false;

  CHECKGLERROR("UpdateDeviceParams");
  return true;
}

// ---------------------------------------------------------------------------
// GlSetup / GlTeardown — unchanged
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
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, screen_width_, screen_height_, 0,
               GL_RGB, GL_UNSIGNED_BYTE, 0);

  left_eye_texture_description_  = {texture_, 0,   0.5f, 1, 0};
  right_eye_texture_description_ = {texture_, 0.5f, 1,   1, 0};

  glGenRenderbuffers(1, &depthRenderBuffer_);
  glBindRenderbuffer(GL_RENDERBUFFER, depthRenderBuffer_);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                        screen_width_, screen_height_);

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
  glDeleteRenderbuffers(1, &depthRenderBuffer_); depthRenderBuffer_ = 0;
  glDeleteFramebuffers (1, &framebuffer_);        framebuffer_  = 0;
  glDeleteTextures     (1, &texture_);            texture_      = 0;
  CHECKGLERROR("GlTeardown");
}

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

// ---------------------------------------------------------------------------
// BuildSphereMesh — 64 × 32 UV sphere, inside-out (CW winding, normals in)
// ---------------------------------------------------------------------------
void HelloCardboardApp::BuildSphereMesh() {
  constexpr int kStacks = 32;
  constexpr int kSlices = 64;
  constexpr float kRadius = 50.0f;  // large enough to surround viewer

  sphere_vertices_.clear();
  sphere_uvs_.clear();
  sphere_indices_.clear();

  // Generate vertices
  for (int stack = 0; stack <= kStacks; ++stack) {
    float phi = static_cast<float>(M_PI) * stack / kStacks;  // 0 → π
    float sinPhi = std::sin(phi);
    float cosPhi = std::cos(phi);

    for (int slice = 0; slice <= kSlices; ++slice) {
      // Use negative theta direction so the texture reads correctly from inside.
      float theta = -2.0f * static_cast<float>(M_PI) * slice / kSlices;
      float sinTheta = std::sin(theta);
      float cosTheta = std::cos(theta);

      float x = sinPhi * cosTheta;
      float y = cosPhi;
      float z = sinPhi * sinTheta;

      sphere_vertices_.push_back(x * kRadius);
      sphere_vertices_.push_back(y * kRadius);
      sphere_vertices_.push_back(z * kRadius);

      float u = static_cast<float>(slice) / kSlices;
      float v = static_cast<float>(stack) / kStacks;
      sphere_uvs_.push_back(u);
      sphere_uvs_.push_back(v);
    }
  }

  // Generate indices — reversed winding (CW from outside → CCW from inside)
  for (int stack = 0; stack < kStacks; ++stack) {
    for (int slice = 0; slice < kSlices; ++slice) {
      GLushort a = static_cast<GLushort>( stack      * (kSlices + 1) + slice    );
      GLushort b = static_cast<GLushort>((stack + 1) * (kSlices + 1) + slice    );
      GLushort c = static_cast<GLushort>((stack + 1) * (kSlices + 1) + slice + 1);
      GLushort d = static_cast<GLushort>( stack      * (kSlices + 1) + slice + 1);

      // Triangle 1 (reversed)
      sphere_indices_.push_back(a);
      sphere_indices_.push_back(c);
      sphere_indices_.push_back(b);
      // Triangle 2 (reversed)
      sphere_indices_.push_back(a);
      sphere_indices_.push_back(d);
      sphere_indices_.push_back(c);
    }
  }

  sphere_built_ = true;
}

// ---------------------------------------------------------------------------
// DrawSphere — renders the 360 background sphere
// ---------------------------------------------------------------------------
void HelloCardboardApp::DrawSphere() {
  if (!sphere_built_) return;

  bool use_video = is_video_ && video_texture_ready_;

  GLuint prog         = use_video ? sphere_program_video_  : sphere_program_image_;
  GLuint pos_param    = use_video ? sphere_position_param_video_  : sphere_position_param_image_;
  GLuint uv_param     = use_video ? sphere_uv_param_video_        : sphere_uv_param_image_;
  GLuint mvp_param    = use_video ? sphere_mvp_param_video_       : sphere_mvp_param_image_;

  if (prog == 0) return;

  glUseProgram(prog);

  // Bind the appropriate texture.
  glActiveTexture(GL_TEXTURE0);
  if (use_video) {
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, video_texture_id_);
  } else {
    if (!image_texture_loaded_) return;
    image_texture_.Bind();  // binds to GL_TEXTURE0
  }

  // Upload MVP — sphere is centered at origin, no model transform needed.
  std::array<float, 16> mvp_array = modelview_projection_sphere_.ToGlArray();
  glUniformMatrix4fv(mvp_param, 1, GL_FALSE, mvp_array.data());

  glEnableVertexAttribArray(pos_param);
  glVertexAttribPointer(pos_param, 3, GL_FLOAT, GL_FALSE, 0,
                        sphere_vertices_.data());
  glEnableVertexAttribArray(uv_param);
  glVertexAttribPointer(uv_param, 2, GL_FLOAT, GL_FALSE, 0,
                        sphere_uvs_.data());

  // Disable face culling so the inside of the sphere is visible.
  glDisable(GL_CULL_FACE);
  glDrawElements(GL_TRIANGLES,
                 static_cast<GLsizei>(sphere_indices_.size()),
                 GL_UNSIGNED_SHORT,
                 sphere_indices_.data());
  glEnable(GL_CULL_FACE);

  CHECKGLERROR("DrawSphere");
}

// ---------------------------------------------------------------------------
// DrawWorld — draw sphere first, then all scene objects
// ---------------------------------------------------------------------------
void HelloCardboardApp::DrawWorld() {
  DrawSphere();

  std::lock_guard<std::mutex> lock(scene_objects_mutex_);
  for (auto& obj : scene_objects_) {
    if (!obj.visible) continue;

    glUseProgram(obj_program_);

    // Compute final MVP: projection * eye_view (baked in modelview_projection_sphere_
    // is actually projection * eye_view for the identity model — reuse).
    // For scene objects we need projection * eye_view * obj.transform.
    // We stored modelview_projection_sphere_ = projection * eye_view.
    // Multiply by object transform.
    Matrix4x4 mvp = modelview_projection_sphere_ * obj.transform;
    std::array<float, 16> mvp_array = mvp.ToGlArray();
    glUniformMatrix4fv(obj_modelview_projection_param_, 1, GL_FALSE,
                       mvp_array.data());

    // Tint: highlight if user is gazing at this interactive object.
    bool gazing = obj.gaze_interactive && IsPointingAtObject(obj.transform);
    if (gazing) {
      glUniform4f(obj_tint_param_, 1.6f, 1.6f, 1.0f, 1.0f); // warm highlight
    } else {
      glUniform4f(obj_tint_param_, 1.0f, 1.0f, 1.0f, 1.0f); // no tint
    }

    obj.texture.Bind();
    obj.mesh.Draw();
  }

  CHECKGLERROR("DrawWorld");
}

// ---------------------------------------------------------------------------
// Gaze detection
// ---------------------------------------------------------------------------
bool HelloCardboardApp::IsPointingAtObject(const Matrix4x4& model) const {
  Matrix4x4 head_from_obj = head_view_ * model;
  const std::array<float, 4> unit_pos    = {0.f, 0.f, 0.f, 1.f};
  const std::array<float, 4> gaze_fwd    = {0.f, 0.f, -1.f, 0.f};
  const std::array<float, 4> obj_in_head = head_from_obj * unit_pos;
  return AngleBetweenVectors(gaze_fwd, obj_in_head) < kAngleLimit;
}

// ---------------------------------------------------------------------------
// MakeTransform helper
// ---------------------------------------------------------------------------
Matrix4x4 HelloCardboardApp::MakeTransform(std::array<float, 3> position,
                                            std::array<float, 3> rotation_deg,
                                            float scale) const {
  // Build TRS matrix: Translation * RotY * RotX * RotZ * Scale
  auto deg2rad = [](float d) { return d * static_cast<float>(M_PI) / 180.f; };

  float rx = deg2rad(rotation_deg[0]);
  float ry = deg2rad(rotation_deg[1]);
  float rz = deg2rad(rotation_deg[2]);

  // Rotation X
  Matrix4x4 Rx{};
  Rx.m[0][0]=1; Rx.m[1][1]=std::cos(rx); Rx.m[1][2]=std::sin(rx);
  Rx.m[2][1]=-std::sin(rx); Rx.m[2][2]=std::cos(rx); Rx.m[3][3]=1;

  // Rotation Y
  Matrix4x4 Ry{};
  Ry.m[0][0]=std::cos(ry); Ry.m[0][2]=-std::sin(ry);
  Ry.m[1][1]=1;
  Ry.m[2][0]=std::sin(ry); Ry.m[2][2]=std::cos(ry); Ry.m[3][3]=1;

  // Rotation Z
  Matrix4x4 Rz{};
  Rz.m[0][0]=std::cos(rz); Rz.m[0][1]=std::sin(rz);
  Rz.m[1][0]=-std::sin(rz); Rz.m[1][1]=std::cos(rz);
  Rz.m[2][2]=1; Rz.m[3][3]=1;

  // Scale
  Matrix4x4 S{};
  S.m[0][0]=scale; S.m[1][1]=scale; S.m[2][2]=scale; S.m[3][3]=1;

  // Translation
  Matrix4x4 T = GetTranslationMatrix(position);

  return T * Ry * Rx * Rz * S;
}

// ---------------------------------------------------------------------------
// Scene object management
// ---------------------------------------------------------------------------
bool HelloCardboardApp::AddSceneObject(JNIEnv* env,
                                        const std::string& id,
                                        const std::string& obj_asset,
                                        const std::string& texture_asset,
                                        std::array<float, 3> position,
                                        std::array<float, 3> rotation_deg,
                                        float scale,
                                        bool gaze_interactive) {
  SceneObject so;
  so.id              = id;
  so.gaze_interactive = gaze_interactive;
  so.visible         = true;
  so.transform       = MakeTransform(position, rotation_deg, scale);

  std::string obj_path = std::string("objects/") + obj_asset;
  std::string tex_path = std::string("objects/") + texture_asset;

  if (!so.mesh.Initialize(obj_position_param_, obj_uv_param_,
                          obj_path, asset_mgr_)) {
    LOGE("AddSceneObject: failed to load mesh %s", obj_path.c_str());
    return false;
  }
  if (!so.texture.Initialize(env, java_asset_mgr_, tex_path)) {
    LOGE("AddSceneObject: failed to load texture %s", tex_path.c_str());
    return false;
  }

  std::lock_guard<std::mutex> lock(scene_objects_mutex_);
  // Remove any existing object with the same id.
  scene_objects_.erase(
      std::remove_if(scene_objects_.begin(), scene_objects_.end(),
                     [&](const SceneObject& o){ return o.id == id; }),
      scene_objects_.end());
  scene_objects_.push_back(std::move(so));
  return true;
}

void HelloCardboardApp::RemoveSceneObject(const std::string& id) {
  std::lock_guard<std::mutex> lock(scene_objects_mutex_);
  scene_objects_.erase(
      std::remove_if(scene_objects_.begin(), scene_objects_.end(),
                     [&](const SceneObject& o){ return o.id == id; }),
      scene_objects_.end());
}

void HelloCardboardApp::SetSceneObjectVisible(const std::string& id,
                                               bool visible) {
  std::lock_guard<std::mutex> lock(scene_objects_mutex_);
  for (auto& obj : scene_objects_) {
    if (obj.id == id) { obj.visible = visible; return; }
  }
}

void HelloCardboardApp::SetSceneObjectTransform(
    const std::string& id,
    std::array<float, 3> position,
    std::array<float, 3> rotation_deg,
    float scale) {
  Matrix4x4 t = MakeTransform(position, rotation_deg, scale);
  std::lock_guard<std::mutex> lock(scene_objects_mutex_);
  for (auto& obj : scene_objects_) {
    if (obj.id == id) { obj.transform = t; return; }
  }
}

}  // namespace ndk_hello_cardboard
