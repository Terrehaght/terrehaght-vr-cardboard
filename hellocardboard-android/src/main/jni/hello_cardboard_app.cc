/*
 * hello_cardboard_app.cc  (modified – Flexie robot tour-guide integration)
 *
 * All original Cardboard pipeline code is PRESERVED UNCHANGED.
 * New code is marked with // ← FLEXIE comments.
 */

#include "hello_cardboard_app.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <GLES2/gl2ext.h>

#include <array>
#include <cmath>
#include <cstring>
#include <time.h>      // clock_gettime  ← FLEXIE

#include "cardboard.h"

namespace ndk_hello_cardboard {

namespace {

constexpr uint64_t kPredictionTimeWithoutVsyncNanos = 50000000;
constexpr int      kVelocityFilterCutoffFrequency   = 6;

// ---------------------------------------------------------------------------
// Image shader (sampler2D) – unchanged
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
      gl_FragColor = texture2D(u_Texture, vec2(v_UV.x, 1.0 - v_UV.y));
    })glsl";

// ---------------------------------------------------------------------------
// OES video shader – unchanged
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

// ← FLEXIE: helper to read monotonic clock in nanoseconds
inline int64_t NowNanos() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor / Destructor – add last_frame_ns_ init
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
      last_frame_ns_(0) {               // ← FLEXIE
    JNIEnv* env;
    vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    java_asset_mgr_ = env->NewGlobalRef(asset_mgr_obj);
    asset_mgr_      = AAssetManager_fromJava(env, asset_mgr_obj);

    Cardboard_initializeAndroid(vm, obj);
    head_tracker_ = CardboardHeadTracker_create();
    CardboardHeadTracker_setLowPassFilter(head_tracker_,
                                          kVelocityFilterCutoffFrequency);
}

HelloCardboardApp::~HelloCardboardApp() {
    CardboardHeadTracker_destroy(head_tracker_);
    CardboardLensDistortion_destroy(lens_distortion_);
    CardboardDistortionRenderer_destroy(distortion_renderer_);

    if (surface_texture_ref_) {
        JNIEnv* env = nullptr;
        jint result = java_vm_->GetEnv((void**)&env, JNI_VERSION_1_6);
        bool attached = false;
        if (result == JNI_EDETACHED) {
            java_vm_->AttachCurrentThread(&env, nullptr);
            attached = true;
        }
        if (env) env->DeleteGlobalRef(surface_texture_ref_);
        if (attached) java_vm_->DetachCurrentThread();
        surface_texture_ref_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Media setup
// ---------------------------------------------------------------------------

void HelloCardboardApp::SetMedia(const std::string& filename, bool is_video) {
    media_filename_    = filename;
    is_video_          = is_video;
    media_initialized_ = false;
}

GLuint HelloCardboardApp::GetVideoTextureId() {
    if (video_texture_id_ == 0) {
        glGenTextures(1, &video_texture_id_);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, video_texture_id_);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        CHECKGLERROR("GetVideoTextureId");
    }
    return video_texture_id_;
}

void HelloCardboardApp::SetSurfaceTexture(JNIEnv* env, jobject surface_texture) {
    if (surface_texture_ref_) env->DeleteGlobalRef(surface_texture_ref_);
    surface_texture_ref_ = surface_texture ? env->NewGlobalRef(surface_texture) : nullptr;
}

void HelloCardboardApp::UpdateVideoTexture(JNIEnv* env) {
    if (!surface_texture_ref_) return;
    jclass    cls = env->GetObjectClass(surface_texture_ref_);
    jmethodID mid = env->GetMethodID(cls, "updateTexImage", "()V");
    if (mid) env->CallVoidMethod(surface_texture_ref_, mid);
    else     LOGE("UpdateVideoTexture: updateTexImage not found");
    env->DeleteLocalRef(cls);
}

// ---------------------------------------------------------------------------
// Flexie control setters  ← FLEXIE
// ---------------------------------------------------------------------------

void HelloCardboardApp::SetFlexieMode(int mode) {
    switch (mode) {
        case 0: flexie_.SetMode(FlexieMode::ANCHORED); break;
        case 1: flexie_.SetMode(FlexieMode::FOLLOW);   break;
        case 2: flexie_.SetMode(FlexieMode::MOVE_TO);  break;
        default:flexie_.SetMode(FlexieMode::HIDDEN);   break;
    }
}

void HelloCardboardApp::SetFlexiePose(int pose) {
    switch (pose) {
        case 1: flexie_.SetPose(FlexiePose::WAVE);       break;
        case 2: flexie_.SetPose(FlexiePose::TALKING);    break;
        case 3: flexie_.SetPose(FlexiePose::POINT);      break;
        case 4: flexie_.SetPose(FlexiePose::TURN_LEFT);  break;
        case 5: flexie_.SetPose(FlexiePose::TURN_RIGHT); break;
        default:flexie_.SetPose(FlexiePose::IDLE);       break;
    }
}

void HelloCardboardApp::SetFlexieSkin(int skin) {
    flexie_.SetSkin(skin == 1 ? FlexieSkin::PINK : FlexieSkin::BLUE);
}

void HelloCardboardApp::SetFlexiePosition(float x, float y, float z) {
    flexie_.SetPosition(x, y, z);
}

void HelloCardboardApp::SetFlexieFollowDistance(float d) {
    flexie_.SetFollowDistance(d);
}

void HelloCardboardApp::SetFlexieScale(float s) {
    flexie_.SetScale(s);
}

// ---------------------------------------------------------------------------
// OnSurfaceCreated
// ---------------------------------------------------------------------------

void HelloCardboardApp::OnSurfaceCreated(JNIEnv* env) {
    // ---- Image shader -------------------------------------------------------
    GLuint img_vert = LoadGLShader(GL_VERTEX_SHADER,   kImgVertexShader);
    GLuint img_frag = LoadGLShader(GL_FRAGMENT_SHADER, kImgFragmentShader);
    img_program_        = glCreateProgram();
    glAttachShader(img_program_, img_vert);
    glAttachShader(img_program_, img_frag);
    glLinkProgram(img_program_);
    glDeleteShader(img_vert);
    glDeleteShader(img_frag);
    img_position_param_ = glGetAttribLocation (img_program_, "a_Position");
    img_uv_param_       = glGetAttribLocation (img_program_, "a_UV");
    img_mvp_param_      = glGetUniformLocation(img_program_, "u_MVP");
    CHECKGLERROR("img program");

    // ---- OES shader ---------------------------------------------------------
    GLuint oes_vert = LoadGLShader(GL_VERTEX_SHADER,   kOesVertexShader);
    GLuint oes_frag = LoadGLShader(GL_FRAGMENT_SHADER, kOesFragmentShader);
    oes_program_        = glCreateProgram();
    glAttachShader(oes_program_, oes_vert);
    glAttachShader(oes_program_, oes_frag);
    glLinkProgram(oes_program_);
    glDeleteShader(oes_vert);
    glDeleteShader(oes_frag);
    oes_position_param_ = glGetAttribLocation (oes_program_, "a_Position");
    oes_uv_param_       = glGetAttribLocation (oes_program_, "a_UV");
    oes_mvp_param_      = glGetUniformLocation(oes_program_, "u_MVP");
    CHECKGLERROR("oes program");

    // ---- Sphere geometry ----------------------------------------------------
    GenerateSphere(kSphereSectors, kSphereStacks);

    // ---- Image texture (static media only) ----------------------------------
    if (!is_video_ && !media_filename_.empty() && !media_initialized_) {
        if (sphere_image_tex_.Initialize(env, java_asset_mgr_, media_filename_)) {
            media_initialized_ = true;
        }
    }

    // ---- Flexie robot  ← FLEXIE --------------------------------------------
    flexie_.Initialize(env, java_asset_mgr_, asset_mgr_);
    last_frame_ns_ = NowNanos();

    // BUG 1 FIX: Mark screen params changed so UpdateDeviceParams() runs on
    // the very first frame, ensuring GlSetup() is called and the framebuffer
    // exists before any drawing occurs.
    screen_params_changed_ = true;

    CHECKGLERROR("OnSurfaceCreated");
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
// OnDrawFrame
// ---------------------------------------------------------------------------

void HelloCardboardApp::OnDrawFrame() {
    // ---- Delta time  ← FLEXIE ----------------------------------------------
    int64_t now_ns = NowNanos();
    float   dt     = static_cast<float>(now_ns - last_frame_ns_) * 1e-9f;
    if (dt > 0.1f) dt = 0.1f;   // clamp to avoid large jumps after pause
    last_frame_ns_  = now_ns;

    if (!UpdateDeviceParams()) return;

    // ---- Begin offscreen frame -----------------------------------------------
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // ---- Head pose -----------------------------------------------------------
    head_view_ = GetPose();

    // ---- Extract head-forward direction for Flexie  ← FLEXIE ---------------
    // head_view_ is the inverse-view matrix column-major: col 2 = look direction
    // We want world-space forward: negate the Z column of head_view_ inverse.
    // Since head_view_ = rotation only (no translation), forward = -col2 of M^-T
    // Simpler: forward = head_view_ * (0,0,-1,0)
    std::array<float,4> fwd4 = head_view_ * std::array<float,4>{0.f, 0.f, -1.f, 0.f};
    std::array<float,3> head_dir = {fwd4[0], fwd4[1], fwd4[2]};
    float hlen = std::sqrt(head_dir[0]*head_dir[0] +
                           head_dir[1]*head_dir[1] +
                           head_dir[2]*head_dir[2]);
    if (hlen > 0.f) { head_dir[0]/=hlen; head_dir[1]/=hlen; head_dir[2]/=hlen; }

    // ---- Render each eye -----------------------------------------------------
    for (int eye = kLeft; eye <= kRight; ++eye) {
        glViewport(eye == kLeft ? 0 : screen_width_ / 2,
                   0,
                   screen_width_ / 2,
                   screen_height_);

        Matrix4x4 eye_matrix  = GetMatrixFromGlArray(eye_matrices_[eye]);
        Matrix4x4 projection  = GetMatrixFromGlArray(projection_matrices_[eye]);

        // Sphere: view = eye_matrix * head_view_ (no translation used; sphere is huge)
        Matrix4x4 eye_view    = eye_matrix * head_view_;
        modelview_projection_sphere_ = projection * eye_view;
        DrawSphere();

        // Robot: same view-projection but robot has its own model matrix  ← FLEXIE
        Matrix4x4 view_proj = projection * eye_view;
        flexie_.Draw(view_proj, head_dir, dt);
    }

    // ---- Distortion ----------------------------------------------------------
    CardboardDistortionRenderer_renderEyeToDisplay(
        distortion_renderer_, 0,
        0, 0, screen_width_, screen_height_,
        &left_eye_texture_description_,
        &right_eye_texture_description_);

    CHECKGLERROR("OnDrawFrame");
}

// ---------------------------------------------------------------------------
// DrawSphere – unchanged from original
// ---------------------------------------------------------------------------

void HelloCardboardApp::DrawSphere() {
    const GLuint program   = is_video_ ? oes_program_        : img_program_;
    const GLuint pos_param = is_video_ ? oes_position_param_ : img_position_param_;
    const GLuint uv_param  = is_video_ ? oes_uv_param_       : img_uv_param_;
    const GLuint mvp_param = is_video_ ? oes_mvp_param_      : img_mvp_param_;

    glUseProgram(program);

    auto mvp = modelview_projection_sphere_.ToGlArray();
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
    glDrawElements(GL_TRIANGLES, sphere_index_count_, GL_UNSIGNED_SHORT, nullptr);

    glEnable(GL_CULL_FACE);

    glDisableVertexAttribArray(pos_param);
    glDisableVertexAttribArray(uv_param);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    CHECKGLERROR("DrawSphere");
}

// ---------------------------------------------------------------------------
// GenerateSphere – unchanged from original
// ---------------------------------------------------------------------------

void HelloCardboardApp::GenerateSphere(int sectors, int stacks) {
    std::vector<float>    positions;
    std::vector<float>    uvs;
    std::vector<uint16_t> indices;

    float sector_step = 2.f * static_cast<float>(M_PI) / sectors;
    float stack_step  =       static_cast<float>(M_PI) / stacks;

    for (int i = 0; i <= stacks; ++i) {
        float phi = static_cast<float>(M_PI) / 2.f - i * stack_step;
        float xz  = kSphereRadius * std::cos(phi);
        float y   = kSphereRadius * std::sin(phi);
        float v   = static_cast<float>(i) / stacks;

        for (int j = 0; j <= sectors; ++j) {
            float theta = j * sector_step;
            float x = xz * std::cos(theta);
            float z = xz * std::sin(theta);
            float u = static_cast<float>(j) / sectors;
            positions.push_back(x);
            positions.push_back(y);
            positions.push_back(z);
            uvs.push_back(u);
            uvs.push_back(v);
        }
    }

    for (int i = 0; i < stacks; ++i) {
        int k1 = i * (sectors + 1);
        int k2 = k1 + sectors + 1;
        for (int j = 0; j < sectors; ++j, ++k1, ++k2) {
            if (i != 0) {
                indices.push_back(static_cast<uint16_t>(k1));
                indices.push_back(static_cast<uint16_t>(k2));
                indices.push_back(static_cast<uint16_t>(k1 + 1));
            }
            if (i != stacks - 1) {
                indices.push_back(static_cast<uint16_t>(k1 + 1));
                indices.push_back(static_cast<uint16_t>(k2));
                indices.push_back(static_cast<uint16_t>(k2 + 1));
            }
        }
    }

    sphere_index_count_ = static_cast<int>(indices.size());

    glGenBuffers(1, &sphere_vbo_pos_);
    glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_pos_);
    glBufferData(GL_ARRAY_BUFFER,
                 positions.size() * sizeof(float),
                 positions.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &sphere_vbo_uv_);
    glBindBuffer(GL_ARRAY_BUFFER, sphere_vbo_uv_);
    glBufferData(GL_ARRAY_BUFFER,
                 uvs.size() * sizeof(float),
                 uvs.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &sphere_ibo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphere_ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 indices.size() * sizeof(uint16_t),
                 indices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    CHECKGLERROR("GenerateSphere");
}

// ---------------------------------------------------------------------------
// Trigger event – unchanged
// ---------------------------------------------------------------------------

void HelloCardboardApp::OnTriggerEvent() {}

// ---------------------------------------------------------------------------
// Lifecycle – unchanged
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
    if (size == 0) SwitchViewer();
    CardboardQrCode_destroy(buffer);
}

void HelloCardboardApp::SwitchViewer() {
    CardboardQrCode_scanQrCodeAndSaveDeviceParams();
}

// ---------------------------------------------------------------------------
// UpdateDeviceParams – unchanged
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
    CardboardLensDistortion_getProjectionMatrix(lens_distortion_, kLeft,  kZNear, kZFar, projection_matrices_[0]);
    CardboardLensDistortion_getProjectionMatrix(lens_distortion_, kRight, kZNear, kZFar, projection_matrices_[1]);

    screen_params_changed_ = false;
    device_params_changed_ = false;
    CHECKGLERROR("UpdateDeviceParams");
    return true;
}

// ---------------------------------------------------------------------------
// GlSetup / GlTeardown – unchanged
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
                 GL_RGB, GL_UNSIGNED_BYTE, nullptr);

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
    if (framebuffer_ == 0) return;
    glDeleteRenderbuffers(1, &depthRenderBuffer_); depthRenderBuffer_ = 0;
    glDeleteFramebuffers (1, &framebuffer_);        framebuffer_       = 0;
    glDeleteTextures     (1, &texture_);            texture_           = 0;
    CHECKGLERROR("GlTeardown");
}

// ---------------------------------------------------------------------------
// GetPose – unchanged
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
