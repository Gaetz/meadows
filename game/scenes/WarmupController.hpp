#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "game/scenes/StreamingController.hpp" // WorldPolicy

namespace render {
class WorldRenderer;
class MeshCache;
class TextureCache;
}
namespace ui {
class UiSystem;
}

namespace game {

class ScreenStack;
class TerrainBakeStreamer;

// World warmup — the ONE state machine behind every loading veil
// (boot, sandbox entry, travel, the spectator catch-up). Phases run
// strictly in order so consumers never race the generation:
//   BakeRing    bake/publish every tile of the ring at `target`
//   PlaceSpawn  validate/relocate the spawn ONCE, on the FINAL
//               baked+water world (sandbox boot only)
//   BuildScene  let meshes/scatter/caches converge on that world
//   Reveal      fade the veil
// Soft mode (spectator catch-up) shows a light veil instead of the
// black shroud and cancels itself if the camera speeds off.
enum class WarmupPhase : u8 {
    Idle,
    BakeRing,
    PlaceSpawn,
    BuildScene,
    Reveal,
};

// The scene systems the warmup polls and drives, bundled so the phase
// machine and the veil bookkeeping are decoupled from LandscapeScene.
// The scene rebuilds it each call from its own members — references, a
// few scalars, and the actions that stay scene territory as closures
// (the InteractionContext pattern). Mirrors StreamingContext.
struct WarmupContext {
    TerrainBakeStreamer* bakeStreamer;   // ring completion at the target
    render::WorldRenderer& renderer;     // pending work counters, streaming boost
    render::MeshCache* meshCache;        // pending residency (BuildScene)
    render::TextureCache* materialTextures;
    ScreenStack& screenStack;            // the "loading" veil document
    ::ui::UiSystem& uiSystem;            // the veil's data model
    bool uiCreated;
    Vec3 cameraPosition;  // spectator speed estimate + catch-up probe
    bool spectatorMode;   // mode == Spectator (the catch-up only arms there)
    WorldPolicy policy;   // streamsBakes gates the catch-up probe
    // The single-shot spawn validation (sandbox): relocates the spawn
    // within `spawnSearchRadius`, re-seats a traveler's capsule on the
    // baked ground, and returns the FINAL spawn (the next ring target).
    std::function<Vec3(f32 spawnSearchRadius)> onSpawnPlaced;
    // A deferred "Play" entry fires the moment the spawn is final (the
    // veil still covers the scene convergence) — restoreMode(Play).
    std::function<void()> enterPlay;
    // A full (non-soft) veil has fully faded: the boot override's
    // scripted "Enter the world" click lives behind this.
    std::function<void()> onRevealed;
    // The screen set changed (the veil document closed).
    std::function<void()> syncScreens;
};

// Owns the warmup phase machine and the veil state (progress, eased
// bar, alpha, soft-veil cooldown). The scene arms it from every entry
// path, runs it at the end of update(), and reads phase() to gate the
// map-crossing check and the re-arm on (re-)enter. State deliberately
// survives onExit/onEnter: a sandbox boot arms before the re-enter's
// own arm, which must not clobber the PlaceSpawn step.
class WarmupController {
public:
    // `spawnSearchRadius`: how far the wet-spawn validation may
    // relocate. Mode entries scout up to 6 km; a TRAVEL arrival is an
    // authored point — it may only snap locally, never be re-scouted
    // kilometers away. `minSeconds`: minimum veil time (map travel asks
    // ~1 s so the crossing READS as a transition even with both maps
    // warm; 0 everywhere else).
    void arm(const WarmupContext& ctx, const Vec3& target, bool placeSpawn,
             bool soft, f32 minSeconds = 0.0f,
             f32 spawnSearchRadius = 6000.0f);
    // End of the scene's update(): phase machine + veil state. Runs on
    // the SIM dt, paused or not (the boot completes behind the main
    // menu), after the camera moved (the catch-up reads this frame's
    // speed).
    void update(const WarmupContext& ctx, f32 dt);

    WarmupPhase phase() const { return phase_; }
    // The spawn is not FINAL yet (sandbox): a Play entry must wait for
    // PlaceSpawn — see deferPlayEntry.
    bool spawnPending() const {
        return phase_ == WarmupPhase::BakeRing ||
               phase_ == WarmupPhase::PlaceSpawn;
    }
    // "Play" clicked mid-warmup: fires `enterPlay` once the spawn is
    // placed (dropped if a menu re-opened meanwhile).
    void deferPlayEntry() { pendingPlayEntry_ = true; }
    // Skip to the fade (a warmup that can never complete — the interior
    // frame publishes no overworld tile).
    void reveal() { phase_ = WarmupPhase::Reveal; }

private:
    WarmupPhase phase_ { WarmupPhase::Idle };
    bool pendingPlayEntry_ { false };
    Vec3 target_ { 0.0f };
    bool placeSpawn_ { false };
    bool soft_ { false };
    u32 frames_ { 0 };
    u32 peakPending_ { 0 };  // BuildScene high-water mark
    f32 progress_ { 0.0f };  // monotone within one warmup
    f32 elapsed_ { 0.0f };   // wall time under the veil
    f32 spawnRadius_ { 6000.0f }; // wet-relocation bound (arm)
    f32 minSeconds_ { 0.0f };
    // Spectator soft veil re-arm cooldown: a hover near the map rim
    // completes its clamped ring instantly — without the cooldown the
    // veil arms and reveals every few frames (border flicker).
    f32 softVeilCooldown_ { 0.0f };
    f32 veilAlpha_ { 0.0f };
    f32 shownProgress_ { 0.0f }; // eased display value (bar/label)
    Vec3 lastCamPos_ { 0.0f };   // spectator speed estimate
};

} // namespace game
