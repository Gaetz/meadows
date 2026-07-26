#pragma once

namespace core {
class FrameProbe;
}
namespace render {
class WorldRenderer;
struct AtmosphereParams;
}

namespace game {

// The world renderer's ImGui dev panels (tuning, tree builder, GPU
// perf), kept out of render::WorldRenderer so the renderer itself carries no
// dev-UI code (docs/RENDERING.md §7). Stateless: every panel edits the
// renderer's live knobs in place, through friendship — this class is the
// renderer's debug UI, not a subsystem. The "Save render tuning" buttons
// only raise the renderer's save request; the scene owns the plugin write
// (consumeSaveTuningRequest).
class RenderTuningPanels {
public:
    // Terrain & streaming: stats, seed, vegetation radii, culling A/Bs.
    static void drawTerrainPanel(render::WorldRenderer& r);
    // Rendering & post-FX: every live render knob (grass, GI, lighting,
    // sun FX, fog, water, post) + the atmosphere sliders.
    static void drawRenderPanel(render::WorldRenderer& r,
                                render::AtmosphereParams& atmos);
    // Tree builder: every generation knob of both tree types, live —
    // regen on slider release; "Log TOML" prints paste-ready records
    // (the CLAUDE.md §5 round trip until the editor's EditSession takes
    // over).
    static void drawTreeBuilderPanel(render::WorldRenderer& r);
    // The generation knobs alone (both tree types' headers) — shared by
    // drawTreeBuilderPanel and the TreeCreation scene. Returns true on a
    // knob RELEASE (the regen trigger); the caller owns what regen means.
    static bool drawTreeKnobs(render::WorldRenderer& r);
    // Per-pass GPU/CPU budget table ("GPU Perf" window).
    // `cpuProbe` = the scene's FrameProbe for the CPU column (nullable).
    static void drawPerfPanel(render::WorldRenderer& r,
                              const core::FrameProbe* cpuProbe);
};

} // namespace game
