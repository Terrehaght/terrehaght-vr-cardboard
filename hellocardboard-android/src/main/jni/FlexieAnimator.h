//
// FlexieAnimator.h
// Flexie Travel Robot — GLES 2.0 segmented animation controller
//
// Integrates with Google Cardboard NDK HelloCardboard project.
// Replaces the single robot_mesh_ / robot_tex_blue_ / robot_tex_pink_ calls
// inside DrawRobotGuide() with:
//
//     flexie_animator_.Update(delta_sec);
//     flexie_animator_.Draw(mvp, img_program_, img_mvp_param_,
//                           img_position_param_, img_uv_param_);
//
// Euler rotation order: X then Y then Z (column-major OpenGL convention).
// No third-party libs; JSON parsed with a hand-rolled minimal parser.
//
// Copyright (c) 2024 Flexie Travel. All rights reserved.
//

#pragma once

#include <jni.h>
#include <GLES2/gl2.h>
#include <android/asset_manager.h>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

// FIX: Forward-declare TexturedMesh and Texture in the correct namespace.
// The full definitions live in util.h (ndk_hello_cardboard namespace).
// Include util.h before this header (or include it here if preferred).
namespace ndk_hello_cardboard { class TexturedMesh; class Texture; }
using ndk_hello_cardboard::TexturedMesh;
using ndk_hello_cardboard::Texture;

namespace flexie {

// ─────────────────────────────────────────────────────────────────────────────
// Segment names — must match group names in QuadSphere_Flexie.obj
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr int kSegmentCount = 17;

inline constexpr const char* kSegmentNames[kSegmentCount] = {
    "head",
    "torso",
    "chest_badge",
    "arm_upper_L",
    "arm_lower_L",
    "hand_L",
    "arm_upper_R",
    "arm_lower_R",
    "hand_R",
    "leg_upper_L",
    "leg_lower_L",
    "foot_L",
    "leg_upper_R",
    "leg_lower_R",
    "foot_R",
    "backpack",
    "antenna",
};

// ─────────────────────────────────────────────────────────────────────────────
// Segment pivot translations (world-space, rest position)
// Order matches kSegmentNames above.
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr float kSegmentPivots[kSegmentCount][3] = {
    {  0.000f,  0.720f,  0.000f },  // head
    {  0.000f,  0.300f,  0.000f },  // torso
    {  0.000f,  0.500f,  0.131f },  // chest_badge
    {  0.200f,  0.620f,  0.000f },  // arm_upper_L
    {  0.200f,  0.480f,  0.000f },  // arm_lower_L
    {  0.200f,  0.300f,  0.000f },  // hand_L
    { -0.200f,  0.620f,  0.000f },  // arm_upper_R
    { -0.200f,  0.480f,  0.000f },  // arm_lower_R
    { -0.200f,  0.300f,  0.000f },  // hand_R
    {  0.120f,  0.300f,  0.000f },  // leg_upper_L
    {  0.120f,  0.130f,  0.000f },  // leg_lower_L
    {  0.120f,  0.040f,  0.000f },  // foot_L
    { -0.120f,  0.300f,  0.000f },  // leg_upper_R
    { -0.120f,  0.130f,  0.000f },  // leg_lower_R
    { -0.120f,  0.040f,  0.000f },  // foot_R
    {  0.000f,  0.500f, -0.130f },  // backpack
    {  0.000f,  0.900f,  0.000f },  // antenna
};

// Parent index for each segment (-1 = root/world).
// Draw order must be parent-before-child (torso before arms, etc.).
inline constexpr int kParentIndex[kSegmentCount] = {
     1,  // head        → torso
    -1,  // torso       → world
     1,  // chest_badge → torso
     1,  // arm_upper_L → torso
     3,  // arm_lower_L → arm_upper_L
     4,  // hand_L      → arm_lower_L
     1,  // arm_upper_R → torso
     6,  // arm_lower_R → arm_upper_R
     7,  // hand_R      → arm_lower_R
     1,  // leg_upper_L → torso
     9,  // leg_lower_L → leg_upper_L
    10,  // foot_L      → leg_lower_L
     1,  // leg_upper_R → torso
    12,  // leg_lower_R → leg_upper_R
    13,  // foot_R      → leg_lower_R
     1,  // backpack    → torso
     0,  // antenna     → head
};

// ─────────────────────────────────────────────────────────────────────────────
// Euler rotation (degrees) for one segment in one pose
// ─────────────────────────────────────────────────────────────────────────────
struct SegmentRotation {
    float rx = 0.f, ry = 0.f, rz = 0.f;
};

// A full pose = rotation for all 17 segments
using Pose = std::array<SegmentRotation, kSegmentCount>;

// ─────────────────────────────────────────────────────────────────────────────
// FlexieAnimator
// ─────────────────────────────────────────────────────────────────────────────
class FlexieAnimator {
 public:
    FlexieAnimator() = default;
    ~FlexieAnimator();

    // Call once after GL context is ready.
    //
    // FIX: Added JNIEnv* env and jobject java_asset_mgr so that textures can
    // be loaded via Texture::Initialize(JNIEnv*, jobject, path), which is the
    // only Initialize overload provided by util.h.
    //
    // obj_asset_path : path inside APK assets, e.g. "flexie/QuadSphere_Flexie.obj"
    // pose_asset_path: path inside APK assets, e.g. "flexie/FlexiePose.json"
    // tex_blue_path  : "flexie/flexie_atlas_diffuse_blue.png"
    // tex_pink_path  : "flexie/flexie_atlas_diffuse_pink.png"
    bool Initialize(JNIEnv*            env,
                    jobject            java_asset_mgr,
                    AAssetManager*     asset_mgr,
                    GLuint             position_attrib,
                    GLuint             uv_attrib,
                    const std::string& obj_asset_path,
                    const std::string& pose_asset_path,
                    const std::string& tex_blue_path,
                    const std::string& tex_pink_path);

    // Switch instantly to named pose (no blend).
    void SetPose(const std::string& pose_name);

    // Smoothly blend from current rotations to named pose over durationSec.
    void BlendToPose(const std::string& pose_name, float duration_sec);

    // Advance blend timer.
    void Update(float delta_sec);

    // Draw all segments.
    // mvp            : column-major 4×4 view-projection matrix from the app
    // img_program_   : GLES program with position + uv attributes
    // img_mvp_param_ : uniform location for MVP matrix
    // img_position_param_: attribute location for positions
    // img_uv_param_  : attribute location for UVs
    // use_pink_tex   : true during "talking" pose (speaking state)
    void Draw(const float     mvp[16],
              GLuint           img_program_,
              GLuint           img_mvp_param_,
              GLuint           img_position_param_,
              GLuint           img_uv_param_,
              bool             use_pink_tex = false) const;

    // Convenience: returns true while a blend is in progress.
    bool IsBlending() const { return blend_time_ < blend_duration_; }

 private:
    // ── helpers ──────────────────────────────────────────────────────────────

    // Load all OBJ groups from a single asset into separate TexturedMesh objects.
    bool LoadSegmentedOBJ(AAssetManager*     mgr,
                          const std::string& path,
                          GLuint             pos_attrib,
                          GLuint             uv_attrib);

    // Minimal hand-rolled JSON parser for FlexiePose.json.
    bool LoadPoseJSON(AAssetManager* mgr, const std::string& path);

    // Build the per-segment world-space model matrix (parent-chain composition).
    void ComputeWorldMatrix(int seg_idx, float out_mat[16]) const;

    // 4×4 matrix multiply: out = a * b  (column-major)
    static void MatMul4(const float a[16], const float b[16], float out[16]);

    // Rotation matrices
    static void MakeRotX(float deg, float out[16]);
    static void MakeRotY(float deg, float out[16]);
    static void MakeRotZ(float deg, float out[16]);
    static void MakeTranslate(float tx, float ty, float tz, float out[16]);
    static void Identity4(float out[16]);

    // Lerp between two poses
    static SegmentRotation LerpRot(const SegmentRotation& a,
                                   const SegmentRotation& b,
                                   float t);

    // ── state ─────────────────────────────────────────────────────────────────

    // Meshes and textures — one per segment
    std::vector<TexturedMesh*> meshes_;   // size kSegmentCount (may be nullptr if absent)
    Texture* tex_blue_ = nullptr;
    Texture* tex_pink_ = nullptr;

    // Named poses loaded from JSON
    std::unordered_map<std::string, Pose> poses_;

    // Animation state
    Pose current_pose_;           // what we're interpolating FROM
    Pose target_pose_;            // what we're interpolating TO
    float blend_time_     = 0.f;
    float blend_duration_ = 0.f;

    bool initialized_ = false;
};

}  // namespace flexie
