#pragma once

#include <deque>
#include <set>
#include <unordered_map>

#include "data/plugins/EditSession.hpp"
#include "engine/anim/Anim.hpp"
#include "engine/assets/GltfMesh.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace assets {
class AssetDatabase;
}
namespace engine {
struct FrameContext;
}
namespace rhi {
class Device;
}

namespace game {

// 3D animation preview for the DB editor (chantier 8, follow-up 2026-07-13):
// the selected AnimClipForm scrubs/plays and the selected AnimGraphForm runs
// live (params, tag gates, transitions, fired events) on its skinned mesh,
// rendered offscreen through the RHI and shown via ImGui::Image.
//
// Same split as the game (Phase-5 spirit): drawEditor() runs the HEADLESS
// anim runtime (samplePose / GraphInstance -> skinMatrices) plus the ImGui
// controls; render() records the GPU pass, called from EditorScene::render
// (the editor owns the frame for exactly this). Edits preview LIVE like
// FxPanel: forms are re-read through the EditSession every frame, and the
// graph rebuilds when any of its records' fields change (fingerprint).
//
// The body mesh comes from the clip's own glTF when it carries a skin, else
// from the first ActorForm whose appearance rides the same skeleton asset.
class AnimPreviewPanel {
public:
    AnimPreviewPanel(rhi::Device& device, data::EditSession& session,
                     const data::FormDatabase& db,
                     const assets::AssetDatabase& assetDb)
        : device { device }, session { session }, db { db },
          assetDb { assetDb } {}

    // ImGui side: controls + the headless anim step + the stage widget.
    void drawEditor(const core::Guid& itemSelected);
    // GPU side: records the offscreen pass into the current frame.
    void render(engine::FrameContext& frame);

private:
    struct Rig {
        anim::Skeleton skeleton;
        vector<assets::GltfClip> clips;
        bool valid { false };
    };

    const Rig* loadRig(const core::Guid& asset);
    bool ensureMesh(const core::Guid& skeletonAsset);
    void drawClip(const core::Guid& clipId);
    void drawGraph(const core::Guid& graphId);
    bool rebuildGraph(const core::Guid& graphId);
    u64 graphFingerprint(const core::Guid& graphId) const;
    void drawStage();

    rhi::Device& device;
    data::EditSession& session;
    const data::FormDatabase& db;
    const assets::AssetDatabase& assetDb;

    core::Guid shown; // resets playback when the selection changes
    std::unordered_map<core::Guid, Rig> rigs;

    // Body mesh for the current skeleton asset (negative-cached until the
    // asset changes — a failed lookup doesn't retry every frame).
    core::Guid meshFor;
    bool meshTried { false };
    bool meshValid { false };
    rhi::UniqueBuffer vertices;
    rhi::UniqueBuffer indices;
    u32 indexCount { 0 };
    Vec3 boundsCenter { 0.0f };
    f32 boundsRadius { 1.0f };
    Vec4 tint { 1.0f };
    str meshSource;

    // Transport (shared) + clip scrub.
    bool playing { true };
    f32 timeScale { 1.0f };
    f32 time { 0.0f };

    // Graph runtime. graphDesc must outlive graphInstance (it holds a ref).
    uptr<anim::GraphDesc> graphDesc;
    uptr<anim::GraphInstance> graphInstance;
    u64 graphFp { 0 };
    core::Guid graphSkeleton; // asset of the first resolved clip
    vector<str> paramNames;   // float params found on transitions
    std::unordered_map<str, f32> paramValues;
    vector<str> tagNames;     // tag gates found on transitions
    std::set<str> activeTags;
    f32 entitySpeed { 0.0f }; // feeds referenceSpeed playback sync
    std::deque<str> eventLog; // last fired AnimEvents, newest first

    // Evaluated pose -> palette, consumed by render() next frame.
    const anim::Skeleton* skeleton { nullptr };
    anim::Pose pose;
    vector<Mat4> palette;
    bool wantRender { false };
    bool unsupported { false }; // no offscreenTargets on this backend

    // Orbit camera around the mesh bounds.
    f32 yaw { 0.8f };
    f32 pitch { 0.35f };
    f32 zoom { 2.4f }; // eye distance in units of bounds radius

    // GPU pass state, lazy. Fixed-size square target (editor tool).
    rhi::UniqueTexture colorTex;
    rhi::UniqueTexture depthTex;
    rhi::UniqueFramebuffer framebuffer;
    rhi::Unique<rhi::ShaderHandle> shader;
    rhi::UniquePipeline pipeline;
    rhi::UniqueBuffer ubo;
    rhi::UniqueBuffer paletteSsbo;
    rhi::UniqueBindGroup bindGroup;
    u32 paletteCapacity { 0 }; // joints the SSBO can hold
};

} // namespace game
