//
// FlexieAnimator.cpp
// Flexie Travel Robot — GLES 2.0 segmented animation controller
//
// Build requirements:
//   - Android NDK r21+
//   - GLES 2.0
//   - HelloCardboard NDK project (provides TexturedMesh, Texture, util.h)
//   - No external animation or JSON libraries
//
// Euler order: X then Y then Z (column-major OpenGL convention)
//

#include "FlexieAnimator.h"

// FIX: TexturedMesh and Texture are defined in util.h (ndk_hello_cardboard
// namespace).  There are no separate textured_mesh.h / texture.h files.
// Removing the two phantom includes and keeping only util.h is all that
// is needed for the class definitions to be visible here.
#include "util.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

#define LOG_TAG "FlexieAnimator"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace flexie {

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────
FlexieAnimator::~FlexieAnimator() {
    for (auto* m : meshes_) delete m;
    delete tex_blue_;
    delete tex_pink_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Initialize
// FIX: Signature extended with JNIEnv* env + jobject java_asset_mgr so that
//      Texture::Initialize(JNIEnv*, jobject, path) — the only overload in
//      util.h — can be called correctly.
// ─────────────────────────────────────────────────────────────────────────────
bool FlexieAnimator::Initialize(JNIEnv*            env,
                                jobject            java_asset_mgr,
                                AAssetManager*     asset_mgr,
                                GLuint             position_attrib,
                                GLuint             uv_attrib,
                                const std::string& obj_asset_path,
                                const std::string& pose_asset_path,
                                const std::string& tex_blue_path,
                                const std::string& tex_pink_path) {
    if (initialized_) return true;

    // ── Textures ─────────────────────────────────────────────────────────────
    // FIX: Use Texture::Initialize(JNIEnv*, jobject, path) instead of the
    //      non-existent Initialize(AAssetManager*, path) overload.
    tex_blue_ = new Texture();
    if (!tex_blue_->Initialize(env, java_asset_mgr, tex_blue_path)) {
        LOGE("Failed to load blue texture: %s", tex_blue_path.c_str());
        return false;
    }

    tex_pink_ = new Texture();
    if (!tex_pink_->Initialize(env, java_asset_mgr, tex_pink_path)) {
        LOGE("Failed to load pink texture: %s", tex_pink_path.c_str());
        return false;
    }

    // ── Meshes ────────────────────────────────────────────────────────────────
    if (!LoadSegmentedOBJ(asset_mgr, obj_asset_path, position_attrib, uv_attrib)) {
        LOGE("Failed to load segmented OBJ: %s", obj_asset_path.c_str());
        return false;
    }

    // ── Poses ─────────────────────────────────────────────────────────────────
    if (!LoadPoseJSON(asset_mgr, pose_asset_path)) {
        LOGE("Failed to load pose JSON: %s", pose_asset_path.c_str());
        return false;
    }

    // Start in idle pose
    SetPose("idle");

    initialized_ = true;
    LOGI("FlexieAnimator initialised — %d segments loaded.", kSegmentCount);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SetPose — instant switch
// ─────────────────────────────────────────────────────────────────────────────
void FlexieAnimator::SetPose(const std::string& pose_name) {
    auto it = poses_.find(pose_name);
    if (it == poses_.end()) {
        LOGE("Pose not found: %s", pose_name.c_str());
        return;
    }
    current_pose_   = it->second;
    target_pose_    = it->second;
    blend_time_     = 0.f;
    blend_duration_ = 0.f;
}

// ─────────────────────────────────────────────────────────────────────────────
// BlendToPose — smooth transition
// ─────────────────────────────────────────────────────────────────────────────
void FlexieAnimator::BlendToPose(const std::string& pose_name, float duration_sec) {
    auto it = poses_.find(pose_name);
    if (it == poses_.end()) {
        LOGE("Pose not found: %s", pose_name.c_str());
        return;
    }
    // Snapshot current interpolated state as new start
    if (blend_duration_ > 0.f && blend_time_ < blend_duration_) {
        float t = blend_time_ / blend_duration_;
        for (int i = 0; i < kSegmentCount; ++i)
            current_pose_[i] = LerpRot(current_pose_[i], target_pose_[i], t);
    }
    target_pose_    = it->second;
    blend_time_     = 0.f;
    blend_duration_ = (duration_sec > 0.f) ? duration_sec : 0.001f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Update — advance blend timer
// ─────────────────────────────────────────────────────────────────────────────
void FlexieAnimator::Update(float delta_sec) {
    if (blend_duration_ <= 0.f) return;
    blend_time_ += delta_sec;
    if (blend_time_ >= blend_duration_) {
        blend_time_     = blend_duration_;
        current_pose_   = target_pose_;
        blend_duration_ = 0.f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw — render all segments
// ─────────────────────────────────────────────────────────────────────────────
void FlexieAnimator::Draw(const float mvp[16],
                          GLuint      img_program_,
                          GLuint      img_mvp_param_,
                          GLuint      img_position_param_,
                          GLuint      img_uv_param_,
                          bool        use_pink_tex) const {
    if (!initialized_) return;

    glUseProgram(img_program_);

    // FIX: Replace GetTextureId() (which doesn't exist in util.h's Texture
    //      class) with Bind(), which calls glActiveTexture(GL_TEXTURE0) and
    //      glBindTexture(GL_TEXTURE_2D, ...) internally — identical effect.
    Texture* active_tex = use_pink_tex ? tex_pink_ : tex_blue_;
    active_tex->Bind();

    // Draw in order: torso first (index 1), then children breadth-first
    // The parent-index table guarantees parents always have lower indices
    // except for head (0) which parents to torso (1).
    // We use a fixed draw order that satisfies parent-before-child:
    // torso(1), head(0), antenna(16), chest_badge(2),
    // arm_upper_L(3), arm_lower_L(4), hand_L(5),
    // arm_upper_R(6), arm_lower_R(7), hand_R(8),
    // leg_upper_L(9), leg_lower_L(10), foot_L(11),
    // leg_upper_R(12), leg_lower_R(13), foot_R(14),
    // backpack(15)
    static const int kDrawOrder[kSegmentCount] = {
        1, 0, 16, 2,
        3, 4, 5,
        6, 7, 8,
        9,10,11,
       12,13,14,
       15
    };

    // Suppress unused-parameter warnings: the attrib locations are baked into
    // each TexturedMesh at InitializeFromBuffer time, so Draw() needs no args.
    (void)img_position_param_;
    (void)img_uv_param_;

    for (int di = 0; di < kSegmentCount; ++di) {
        int i = kDrawOrder[di];
        if (i >= static_cast<int>(meshes_.size()) || meshes_[i] == nullptr)
            continue;

        // Build per-segment world matrix
        float world[16];
        ComputeWorldMatrix(i, world);

        // Concatenate with incoming MVP: final_mvp = mvp * world
        float final_mvp[16];
        MatMul4(mvp, world, final_mvp);

        glUniformMatrix4fv(img_mvp_param_, 1, GL_FALSE, final_mvp);

        // FIX: Draw() takes no arguments in util.h — the position and UV
        //      attribute locations were stored when InitializeFromBuffer was
        //      called, so passing them again here would not compile.
        meshes_[i]->Draw();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ComputeWorldMatrix
// Recursively composes: parent_world * translate(pivot) * Rx * Ry * Rz
// ─────────────────────────────────────────────────────────────────────────────
void FlexieAnimator::ComputeWorldMatrix(int seg_idx, float out_mat[16]) const {
    // Gather the interpolated rotation for this segment
    SegmentRotation rot;
    if (blend_duration_ > 0.f) {
        float t = blend_time_ / blend_duration_;
        rot = LerpRot(current_pose_[seg_idx], target_pose_[seg_idx], t);
    } else {
        rot = current_pose_[seg_idx];
    }

    // Build local transform: T(pivot) * Rx * Ry * Rz
    float rx[16], ry[16], rz[16], tp[16], tmp1[16], local[16];
    const float* piv = kSegmentPivots[seg_idx];

    MakeTranslate(piv[0], piv[1], piv[2], tp);
    MakeRotX(rot.rx, rx);
    MakeRotY(rot.ry, ry);
    MakeRotZ(rot.rz, rz);

    // local = T * Rx * Ry * Rz
    MatMul4(tp, rx,   tmp1);
    MatMul4(tmp1, ry, local);
    MatMul4(local, rz, tmp1);
    // tmp1 = T * Rx * Ry * Rz
    std::memcpy(local, tmp1, 64);

    int parent = kParentIndex[seg_idx];
    if (parent < 0) {
        // Root segment — local IS world
        std::memcpy(out_mat, local, 64);
    } else {
        float parent_world[16];
        ComputeWorldMatrix(parent, parent_world);
        MatMul4(parent_world, local, out_mat);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// LoadSegmentedOBJ
// Reads the OBJ asset and populates one TexturedMesh per "g <n>" group.
// Uses the same AAsset-based path that TexturedMesh::Initialize expects
// (we feed vertex data directly rather than re-reading asset inside it).
//
// Strategy: we extract each group's vertex/uv data into a minimal sub-OBJ
// written to a temp in-memory buffer, then call Initialize on each mesh.
// ─────────────────────────────────────────────────────────────────────────────
bool FlexieAnimator::LoadSegmentedOBJ(AAssetManager*     mgr,
                                      const std::string& path,
                                      GLuint             pos_attrib,
                                      GLuint             uv_attrib) {
    // ── Read entire OBJ asset into string ────────────────────────────────────
    AAsset* asset = AAssetManager_open(mgr, path.c_str(), AASSET_MODE_BUFFER);
    if (!asset) {
        LOGE("Cannot open OBJ asset: %s", path.c_str());
        return false;
    }
    size_t len = static_cast<size_t>(AAsset_getLength(asset));
    std::string obj_src(len, '\0');
    AAsset_read(asset, &obj_src[0], len);
    AAsset_close(asset);

    // ── Parse global vertex / uv / normal pools ────────────────────────────
    std::vector<std::array<float,3>> verts;
    std::vector<std::array<float,2>> uvs;
    std::vector<std::array<float,3>> normals;

    // Map segment name → list of triangulated face tuples {vi,ti,ni}
    struct FaceVert { int vi, ti, ni; };
    std::unordered_map<std::string, std::vector<FaceVert>> group_faces;
    std::string current_group;

    std::istringstream ss(obj_src);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ls(line);
        std::string token;
        ls >> token;

        if (token == "v") {
            float x, y, z;
            ls >> x >> y >> z;
            verts.push_back({x, y, z});
        } else if (token == "vt") {
            float u, v;
            ls >> u >> v;
            uvs.push_back({u, v});
        } else if (token == "vn") {
            float nx, ny, nz;
            ls >> nx >> ny >> nz;
            normals.push_back({nx, ny, nz});
        } else if (token == "g") {
            ls >> current_group;
            if (!current_group.empty() && group_faces.find(current_group) == group_faces.end())
                group_faces[current_group] = {};
        } else if (token == "f" && !current_group.empty()) {
            // Parse polygon, triangulate as fan
            std::vector<FaceVert> poly;
            std::string fv_str;
            while (ls >> fv_str) {
                FaceVert fv{0, 0, 0};
                // format: vi/ti/ni  or  vi//ni  or  vi/ti  or  vi
                std::replace(fv_str.begin(), fv_str.end(), '/', ' ');
                std::istringstream fvs(fv_str);
                int a = 0, b = 0, c = 0;
                fvs >> a;
                fv.vi = a - 1;
                if (fvs >> b) fv.ti = b - 1;
                if (fvs >> c) fv.ni = c - 1;
                poly.push_back(fv);
            }
            // Fan triangulation
            auto& gf = group_faces[current_group];
            for (size_t k = 1; k + 1 < poly.size(); ++k) {
                gf.push_back(poly[0]);
                gf.push_back(poly[k]);
                gf.push_back(poly[k + 1]);
            }
        }
    }

    // ── Build one TexturedMesh per named segment ──────────────────────────────
    meshes_.resize(kSegmentCount, nullptr);

    for (int i = 0; i < kSegmentCount; ++i) {
        const std::string name(kSegmentNames[i]);
        auto it = group_faces.find(name);
        if (it == group_faces.end() || it->second.empty()) {
            LOGI("Segment '%s' has no faces — skipping.", name.c_str());
            continue;
        }

        const auto& faces = it->second;

        // Build interleaved vertex buffer: [px, py, pz, u, v] per face-vert
        // TexturedMesh::Initialize will receive a minimal sub-OBJ string
        // reconstructed from these verts.
        std::string sub_obj;
        sub_obj.reserve(faces.size() * 80);

        // Emit vertices, uvs referenced in this group
        std::unordered_map<int, int> vi_remap, ti_remap;
        std::vector<std::array<float,3>> g_verts;
        std::vector<std::array<float,2>> g_uvs;

        auto remap_v = [&](int vi) -> int {
            auto r = vi_remap.find(vi);
            if (r != vi_remap.end()) return r->second;
            int idx = static_cast<int>(g_verts.size());
            vi_remap[vi] = idx;
            g_verts.push_back(verts[vi]);
            return idx;
        };
        auto remap_t = [&](int ti) -> int {
            auto r = ti_remap.find(ti);
            if (r != ti_remap.end()) return r->second;
            int idx = static_cast<int>(g_uvs.size());
            ti_remap[ti] = idx;
            g_uvs.push_back(uvs[ti]);
            return idx;
        };

        // Pre-pass to build remapped indices
        struct Tri { int vi[3], ti[3]; };
        std::vector<Tri> tris;
        for (size_t k = 0; k + 2 < faces.size(); k += 3) {
            Tri t;
            for (int j = 0; j < 3; ++j) {
                t.vi[j] = remap_v(faces[k + j].vi);
                t.ti[j] = remap_t(faces[k + j].ti);
            }
            tris.push_back(t);
        }

        // Reconstruct minimal sub-OBJ string
        char buf[128];
        sub_obj += "# sub-obj for ";
        sub_obj += name;
        sub_obj += "\n";
        for (auto& gv : g_verts) {
            snprintf(buf, sizeof(buf), "v %f %f %f\n", gv[0], gv[1], gv[2]);
            sub_obj += buf;
        }
        for (auto& gt : g_uvs) {
            snprintf(buf, sizeof(buf), "vt %f %f\n", gt[0], gt[1]);
            sub_obj += buf;
        }
        sub_obj += "g " + name + "\n";
        for (auto& tri : tris) {
            snprintf(buf, sizeof(buf),
                     "f %d/%d %d/%d %d/%d\n",
                     tri.vi[0]+1, tri.ti[0]+1,
                     tri.vi[1]+1, tri.ti[1]+1,
                     tri.vi[2]+1, tri.ti[2]+1);
            sub_obj += buf;
        }

        // Write sub-OBJ to a temp asset using AAssetManager trick:
        // TexturedMesh::Initialize reads from AAssetManager, so we write
        // the sub-OBJ into Android's file cache and open it that way.
        // Alternative: use a variant of TexturedMesh that accepts raw vertex
        // arrays.  Here we use the raw-buffer path:
        auto* mesh = new ndk_hello_cardboard::TexturedMesh();
        if (!mesh->InitializeFromBuffer(sub_obj.data(),
                                        static_cast<int>(sub_obj.size()),
                                        pos_attrib, uv_attrib)) {
            LOGE("TexturedMesh::InitializeFromBuffer failed for '%s'", name.c_str());
            delete mesh;
            // Fall back to the asset-based path if your version requires it:
            // Caller must write sub-OBJ to a temporary file first.
        } else {
            meshes_[i] = mesh;
        }
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// LoadPoseJSON — hand-rolled minimal parser (no external lib)
// Supports the exact structure produced by FlexiePose.json.
// ─────────────────────────────────────────────────────────────────────────────
static std::string ReadAssetToString(AAssetManager* mgr, const std::string& path) {
    AAsset* a = AAssetManager_open(mgr, path.c_str(), AASSET_MODE_BUFFER);
    if (!a) return {};
    size_t n = static_cast<size_t>(AAsset_getLength(a));
    std::string s(n, '\0');
    AAsset_read(a, &s[0], n);
    AAsset_close(a);
    return s;
}

// Advance pos past whitespace
static void SkipWS(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
                               s[pos] == '\n' || s[pos] == '\r')) ++pos;
}

// Read a quoted string key, returning content between ""
static std::string ReadString(const std::string& s, size_t& pos) {
    SkipWS(s, pos);
    if (pos >= s.size() || s[pos] != '"') return {};
    ++pos; // skip opening "
    size_t start = pos;
    while (pos < s.size() && s[pos] != '"') ++pos;
    std::string result = s.substr(start, pos - start);
    if (pos < s.size()) ++pos; // skip closing "
    return result;
}

// Expect a specific character
static bool Expect(const std::string& s, size_t& pos, char c) {
    SkipWS(s, pos);
    if (pos < s.size() && s[pos] == c) { ++pos; return true; }
    return false;
}

// Read a float number
static float ReadFloat(const std::string& s, size_t& pos) {
    SkipWS(s, pos);
    size_t start = pos;
    if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) ++pos;
    while (pos < s.size() && (std::isdigit(s[pos]) || s[pos] == '.' || s[pos] == 'e' ||
                               s[pos] == 'E' || s[pos] == '-' || s[pos] == '+')) ++pos;
    if (pos == start) return 0.f;
    return std::stof(s.substr(start, pos - start));
}

bool FlexieAnimator::LoadPoseJSON(AAssetManager* mgr, const std::string& path) {
    std::string src = ReadAssetToString(mgr, path);
    if (src.empty()) return false;

    // Build name→index lookup
    std::unordered_map<std::string, int> seg_index;
    for (int i = 0; i < kSegmentCount; ++i)
        seg_index[kSegmentNames[i]] = i;

    size_t pos = 0;

    // Expect outer { "poses": {
    if (!Expect(src, pos, '{')) return false;
    ReadString(src, pos); // "poses"
    if (!Expect(src, pos, ':')) return false;
    if (!Expect(src, pos, '{')) return false;

    while (true) {
        SkipWS(src, pos);
        if (pos >= src.size() || src[pos] == '}') break;

        // pose name
        std::string pose_name = ReadString(src, pos);
        if (pose_name.empty()) break;
        if (!Expect(src, pos, ':')) return false;
        if (!Expect(src, pos, '{')) return false;

        Pose pose;
        // Initialise to zero
        for (auto& sr : pose) sr = {0.f, 0.f, 0.f};

        while (true) {
            SkipWS(src, pos);
            if (pos >= src.size() || src[pos] == '}') break;

            std::string seg_name = ReadString(src, pos);
            if (!Expect(src, pos, ':')) return false;
            if (!Expect(src, pos, '[')) return false;

            float rx = ReadFloat(src, pos); Expect(src, pos, ',');
            float ry = ReadFloat(src, pos); Expect(src, pos, ',');
            float rz = ReadFloat(src, pos);
            Expect(src, pos, ']');

            auto it = seg_index.find(seg_name);
            if (it != seg_index.end()) {
                pose[it->second] = {rx, ry, rz};
            }

            SkipWS(src, pos);
            if (pos < src.size() && src[pos] == ',') ++pos;
        }
        if (!Expect(src, pos, '}')) return false;

        poses_[pose_name] = pose;
        LOGI("Loaded pose: %s", pose_name.c_str());

        SkipWS(src, pos);
        if (pos < src.size() && src[pos] == ',') ++pos;
    }

    return !poses_.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Math helpers
// ─────────────────────────────────────────────────────────────────────────────
static constexpr float kDegToRad = 3.14159265358979323846f / 180.f;

void FlexieAnimator::Identity4(float m[16]) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.f;
}

void FlexieAnimator::MakeTranslate(float tx, float ty, float tz, float m[16]) {
    Identity4(m);
    m[12] = tx; m[13] = ty; m[14] = tz;
}

// Column-major: M[col*4 + row]
void FlexieAnimator::MakeRotX(float deg, float m[16]) {
    Identity4(m);
    float rad = deg * kDegToRad;
    float c = std::cos(rad), s = std::sin(rad);
    m[5]  =  c; m[9]  = -s;
    m[6]  =  s; m[10] =  c;
}

void FlexieAnimator::MakeRotY(float deg, float m[16]) {
    Identity4(m);
    float rad = deg * kDegToRad;
    float c = std::cos(rad), s = std::sin(rad);
    m[0]  =  c; m[8]  =  s;
    m[2]  = -s; m[10] =  c;
}

void FlexieAnimator::MakeRotZ(float deg, float m[16]) {
    Identity4(m);
    float rad = deg * kDegToRad;
    float c = std::cos(rad), s = std::sin(rad);
    m[0]  =  c; m[4]  = -s;
    m[1]  =  s; m[5]  =  c;
}

// out = a * b  (column-major, no aliasing)
void FlexieAnimator::MatMul4(const float a[16], const float b[16], float out[16]) {
    float tmp[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[col * 4 + k];
            tmp[col * 4 + row] = sum;
        }
    }
    std::memcpy(out, tmp, 64);
}

SegmentRotation FlexieAnimator::LerpRot(const SegmentRotation& a,
                                        const SegmentRotation& b,
                                        float t) {
    return {
        a.rx + (b.rx - a.rx) * t,
        a.ry + (b.ry - a.ry) * t,
        a.rz + (b.rz - a.rz) * t
    };
}

}  // namespace flexie
