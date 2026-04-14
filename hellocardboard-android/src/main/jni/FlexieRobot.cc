/*
 * FlexieRobot.cc
 * Implements the Flexie Travel robot tour-guide for the Cardboard VR viewer.
 */

#include "FlexieRobot.h"

#include <android/log.h>
#include <cmath>
#include <cstring>

#define FLEXIE_TAG "FlexieRobot"
#define FLEXI_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, FLEXIE_TAG, __VA_ARGS__)
#define FLEXI_LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, FLEXIE_TAG, __VA_ARGS__)

namespace ndk_hello_cardboard {

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
FlexieRobot::~FlexieRobot() {
    if (program_) { glDeleteProgram(program_); program_ = 0; }
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool FlexieRobot::Initialize(JNIEnv* env, jobject java_asset_mgr,
                              AAssetManager* asset_mgr) {
    // ---- Compile shader program ----------------------------------------
    GLuint vert = LoadGLShader(GL_VERTEX_SHADER,   kVertSrc);
    GLuint frag = LoadGLShader(GL_FRAGMENT_SHADER, kFragSrc);
    if (!vert || !frag) {
        FLEXI_LOGE("Failed to compile Flexie shaders");
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vert);
    glAttachShader(program_, frag);
    glLinkProgram(program_);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        FLEXI_LOGE("Flexie shader program link failed");
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    pos_attrib_  = glGetAttribLocation (program_, "a_Position");
    uv_attrib_   = glGetAttribLocation (program_, "a_UV");
    mvp_uniform_ = glGetUniformLocation(program_, "u_MVP");
    CHECKGLERROR("Flexie shader setup");

    // ---- Load OBJ mesh -----------------------------------------------------
    if (!mesh_.Initialize(pos_attrib_, uv_attrib_,
                          "models/QuadSphere_Flexie.obj", asset_mgr)) {
        FLEXI_LOGE("Failed to load Flexie OBJ");
        return false;
    }

    // ---- Load diffuse textures ---------------------------------------------
    if (!tex_blue_.Initialize(env, java_asset_mgr,
                               "textures/flexie_atlas_diffuse_blue.png")) {
        FLEXI_LOGE("Failed to load blue atlas texture");
        return false;
    }
    if (!tex_pink_.Initialize(env, java_asset_mgr,
                               "textures/flexie_atlas_diffuse_pink.png")) {
        FLEXI_LOGE("Failed to load pink atlas texture");
        return false;
    }

    initialized_ = true;
    FLEXI_LOGD("FlexieRobot initialized OK");
    return true;
}

// ---------------------------------------------------------------------------
// UpdatePosition
// ---------------------------------------------------------------------------
void FlexieRobot::UpdatePosition(const std::array<float,3>& head_dir,
                                  float dt) {
    switch (mode_) {
        case FlexieMode::ANCHORED:
            // Already placed; nothing to do.
            robot_pos_ = target_pos_;
            break;

        case FlexieMode::FOLLOW: {
            // Desired position = head_dir * follow_distance_ (horizontal only)
            std::array<float,3> desired = {
                head_dir[0] * follow_distance_,
                robot_pos_[1],                     // keep current height
                head_dir[2] * follow_distance_
            };
            // Smooth toward desired
            float max_step = follow_speed_ * dt;
            for (int i = 0; i < 3; ++i) {
                float diff = desired[i] - robot_pos_[i];
                float step = diff;
                if (step >  max_step) step =  max_step;
                if (step < -max_step) step = -max_step;
                robot_pos_[i] += step;
            }
            break;
        }

        case FlexieMode::MOVE_TO: {
            // Glide toward target_pos_
            float dx = target_pos_[0] - robot_pos_[0];
            float dy = target_pos_[1] - robot_pos_[1];
            float dz = target_pos_[2] - robot_pos_[2];
            float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist > 0.01f) {
                float step = move_speed_ * dt;
                if (step > dist) step = dist;
                float inv = step / dist;
                robot_pos_[0] += dx * inv;
                robot_pos_[1] += dy * inv;
                robot_pos_[2] += dz * inv;
            }
            break;
        }

        case FlexieMode::HIDDEN:
            break;
    }

    // Compute yaw so the robot always faces the camera (origin)
    float dx = -robot_pos_[0];
    float dz = -robot_pos_[2];
    yaw_deg_ = std::atan2(dx, dz) * (180.0f / static_cast<float>(M_PI));
}

// ---------------------------------------------------------------------------
// BuildModelMatrix
// ---------------------------------------------------------------------------
Matrix4x4 FlexieRobot::BuildModelMatrix() const {
    // Translation
    Matrix4x4 T = GetTranslationMatrix({robot_pos_[0],
                                        robot_pos_[1],
                                        robot_pos_[2]});

    // Yaw rotation (Y-axis) – face toward origin
    float rad = yaw_deg_ * static_cast<float>(M_PI) / 180.0f;
    float c = std::cos(rad);
    float s = std::sin(rad);
    Matrix4x4 R;
    memset(R.m, 0, sizeof(R.m));
    R.m[0][0] =  c;   R.m[0][2] = s;
    R.m[1][1] =  1.f;
    R.m[2][0] = -s;   R.m[2][2] = c;
    R.m[3][3] =  1.f;

    // Uniform scale
    Matrix4x4 S;
    memset(S.m, 0, sizeof(S.m));
    S.m[0][0] = scale_;
    S.m[1][1] = scale_;
    S.m[2][2] = scale_;
    S.m[3][3] = 1.f;

    return T * R * S;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
void FlexieRobot::Draw(const Matrix4x4& view_proj,
                        const std::array<float,3>& head_direction,
                        float delta_seconds) {
    if (!initialized_ || mode_ == FlexieMode::HIDDEN) return;

    UpdatePosition(head_direction, delta_seconds);

    Matrix4x4 model  = BuildModelMatrix();
    Matrix4x4 mvp    = view_proj * model;
    auto      mvp_gl = mvp.ToGlArray();

    glUseProgram(program_);
    glUniformMatrix4fv(mvp_uniform_, 1, GL_FALSE, mvp_gl.data());

    glActiveTexture(GL_TEXTURE0);
    if (skin_ == FlexieSkin::PINK) {
        tex_pink_.Bind();
    } else {
        tex_blue_.Bind();
    }
    glUniform1i(glGetUniformLocation(program_, "u_Texture"), 0);

    // Enable depth testing so the robot occludes correctly with the sphere.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    // Enable back-face culling for the solid robot model.
    glEnable(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);
 
    mesh_.Draw();

    glDisable(GL_CULL_FACE);
    CHECKGLERROR("FlexieRobot::Draw");
}

}  // namespace ndk_hello_cardboard
