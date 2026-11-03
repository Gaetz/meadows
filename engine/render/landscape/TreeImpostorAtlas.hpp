#pragma once

#include <utility>

#include "engine/core/Defines.hpp"
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/render/landscape/VegetationSystem.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace render {

// The far-tree impostor atlas (docs/RENDERING.md, docs/PAYSAGE.md
// §7.7): every tree variant rendered with ITS OWN pipeline and
// materials — leaf cards, species tints, bark — from kViews directions
// (kAzimuths around the trunk × kElevations rings) into one atlas,
// under a bake frame: orthographic camera, white ambient, no sun, no
// fog, no season, no wind, no fire. far_tree.frag then reads the tiles
// nearest the view direction (two azimuths blended) instead of drawing
// its analytic discs, so the far woods carry the shape and colour of
// the near trees. Re-baked whenever the variant meshes land
// (VegetationSystem::treeMeshGeneration): ~80 small draws, one frame.
//
// Tile space: the tree is scaled so that max(height, 2 × radial
// extent) — its "fit" — is one unit, stood at the origin, and the
// orthographic box spans ±kBoxHalf around (0, 0.5): the whole tree,
// from every ring, inside the tile. far_tree.vert maps the box back to
// the world through the instance's height and the variant's fit/height
// ratio (TreeSilhouette::variants). Rows = variants, columns = views
// (ring-major), origin bottom-left like every offscreen target.
class TreeImpostorAtlas {
public:
    static constexpr u32 kAzimuths = 8;
    static constexpr u32 kElevations = 2;
    static constexpr u32 kViews = kAzimuths * kElevations;
    static constexpr u32 kTile = 128; // px per tile side
    static constexpr f32 kBoxHalf = 0.75f;
    static constexpr f32 kElevationStep = 0.6f; // rad between rings (34°)
    static constexpr u32 kAtlasUnit = 6;        // far_tree.frag's sampler

    // `hdr`: the scene colour format the tree pipeline renders to
    // (RGBA16F, else RGBA8) — the atlas must match it.
    void create(rhi::Device& device, bool hdr);
    void destroy(rhi::Device& device);

    bool ready() const { return bakedGeneration != 0; }
    bool stale(u64 generation) const {
        return generation != 0 && generation != bakedGeneration;
    }

    // Records the bake into `cmd`: one render pass over the atlas,
    // OUTSIDE any other pass. `base` = this frame's uniforms (the bake
    // overrides the camera and the lighting); `passGroups` = the
    // pass-level bind groups (set, group) the tree pipeline's layout
    // expects, as the main pass binds them.
    void bake(rhi::CommandBuffer& cmd, rhi::Device& device,
              VegetationSystem& vegetation, const FrameUniforms& base,
              rhi::BindGroupHandle shadowGroup,
              const vector<std::pair<u32, rhi::BindGroupHandle>>& passGroups,
              u64 generation);

    rhi::BindGroupHandle bindGroup() const { return atlasGroup.get(); }

private:
    rhi::UniqueTexture atlas;
    rhi::UniqueTexture depth;
    rhi::UniqueFramebuffer framebuffer;
    rhi::UniqueSampler sampler;
    rhi::UniqueBindGroup atlasGroup;
    // One frame UBO per view: recorded draws of one pass all read a
    // buffer's final contents (CommandBuffer.hpp), so each view owns
    // its camera.
    array<rhi::UniqueBuffer, kViews> viewUbos;
    array<rhi::UniqueBindGroup, kViews> viewGroups;
    rhi::UniqueBuffer instances; // one Instance per tree variant
    u64 bakedGeneration { 0 };
};

} // namespace render
