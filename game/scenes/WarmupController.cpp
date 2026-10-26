#include "game/scenes/WarmupController.hpp"

#include <cstdio>

#include <glm/glm.hpp>

#include "engine/render/MeshCache.hpp"
#include "engine/render/TextureCache.hpp"
#include "engine/render/WorldRenderer.hpp"
#include "engine/ui/UiSystem.hpp"
#include "game/ScreenStack.hpp"
#include "game/TerrainBakeStreamer.hpp"

namespace game {

void WarmupController::arm(const WarmupContext& ctx, const Vec3& target,
                           bool placeSpawn, bool soft, f32 minSeconds,
                           f32 spawnSearchRadius) {
    phase_ = WarmupPhase::BakeRing;
    target_ = target;
    placeSpawn_ = placeSpawn;
    soft_ = soft;
    frames_ = 0;
    peakPending_ = 0;
    progress_ = 0.0f;
    elapsed_ = 0.0f;
    minSeconds_ = minSeconds;
    spawnRadius_ = spawnSearchRadius;
    shownProgress_ = 0.0f;
    if (ctx.uiCreated && ctx.screenStack.find("loading")) {
        ctx.screenStack.show("loading");
    }
}

void WarmupController::update(const WarmupContext& ctx, f32 rawDt) {
    const f32 dt = glm::max(rawDt, 1.0e-4f);
    // Spectator catch-up: flying faster than the bake wavefront leaves
    // analytic macro around; when the camera STOPS over an incomplete
    // ring, a light veil shows the ring completing.
    const Vec3 camPos = ctx.cameraPosition;
    const f32 camSpeed = glm::length(camPos - lastCamPos_) / dt;
    lastCamPos_ = camPos;
    if (phase_ == WarmupPhase::Idle) {
        softVeilCooldown_ = glm::max(0.0f, softVeilCooldown_ - dt);
        const bool menuOpen = ctx.uiCreated && ctx.screenStack.modalOpen();
        // No terrain exists in interiors: the camera sits in the interior
        // frame, so the ring probe would ask for overworld tiles around a
        // meaningless position and the veil would never complete.
        if (ctx.policy.streamsBakes && ctx.bakeStreamer &&
            ctx.spectatorMode && !menuOpen && camSpeed < 6.0f &&
            softVeilCooldown_ <= 0.0f) {
            const auto ring = ctx.bakeStreamer->ringStatus(camPos);
            // >= 2 missing: near the map rim the clamped ring drops to
            // a couple of tiles and completes within frames — arming
            // there strobes the veil (the border flicker); a real hole
            // in the world is several tiles deep.
            if (ring.needed > ring.published &&
                ring.needed - ring.published >= 2) {
                arm(ctx, camPos, false, true);
            }
        }
        if (phase_ == WarmupPhase::Idle) {
            ctx.renderer.setStreamingBoost(false);
            return;
        }
    }
    ++frames_;
    elapsed_ += dt;
    // A soft veil cancels itself when the camera speeds off again.
    if (soft_ && phase_ != WarmupPhase::Reveal && camSpeed > 20.0f) {
        phase_ = WarmupPhase::Reveal;
    }

    f32 progress = progress_;
    switch (phase_) {
    case WarmupPhase::BakeRing: {
        // Weight 0.7 of the bar: published slices over needed (the map
        // bake's own slice progress rides bakeMap's log; here the ring
        // completes slice by slice off the cache).
        bool complete = true;
        f32 ringFrac = 1.0f;
        if (ctx.bakeStreamer) {
            const auto ring = ctx.bakeStreamer->ringStatus(target_);
            complete = ring.published >= ring.needed;
            if (!complete) {
                ringFrac = glm::min(
                    static_cast<f32>(ring.published) /
                        static_cast<f32>(ring.needed),
                    0.97f);
            }
        }
        progress = ringFrac * 0.7f;
        if (complete) {
            phase_ = WarmupPhase::PlaceSpawn;
        }
        break;
    }
    case WarmupPhase::PlaceSpawn: {
        if (placeSpawn_) {
            // The scene validates/relocates the spawn ONCE on the final
            // baked ground and re-seats a traveler's capsule there.
            const Vec3 spawn = ctx.onSpawnPlaced(spawnRadius_);
            placeSpawn_ = false;
            target_ = spawn;
            // The relocation may have crossed toward lesser-baked
            // ground: bake THAT ring before building the scene.
            if (ctx.bakeStreamer) {
                const auto ring = ctx.bakeStreamer->ringStatus(target_);
                if (ring.published < ring.needed) {
                    phase_ = WarmupPhase::BakeRing;
                    break;
                }
            }
        }
        peakPending_ = 0;
        phase_ = WarmupPhase::BuildScene;
        // A deferred "Play" click (main menu) fires now — the spawn is
        // final; the veil still covers the scene convergence. Dropped
        // if the player re-opened a menu meanwhile.
        if (pendingPlayEntry_) {
            pendingPlayEntry_ = false;
            if (!(ctx.uiCreated && ctx.screenStack.modalOpen())) {
                ctx.enterPlay();
            }
        }
        break;
    }
    case WarmupPhase::BuildScene: {
        // Weight 0.3: meshes/scatter/caches converge on the FINAL
        // world — nothing left to invalidate afterwards.
        const u32 pending =
            ctx.renderer.terrainSystem().pendingCount() +
            (ctx.meshCache ? ctx.meshCache->pendingCount() : 0u) +
            (ctx.materialTextures ? ctx.materialTextures->pendingCount()
                                  : 0u) +
            ctx.renderer.pendingChunkRebuilds();
        peakPending_ = glm::max(peakPending_, pending);
        const f32 buildFrac =
            peakPending_ == 0u
                ? 1.0f
                : 1.0f - static_cast<f32>(pending) /
                             static_cast<f32>(peakPending_);
        progress = 0.7f + 0.3f * buildFrac;
        // Grace: the first frames are still announcing work — and the
        // veil holds its caller's minimum (map travel asks ~1 s so a
        // warm-cache crossing still READS as a transition).
        if (frames_ > 30 && pending == 0 && elapsed_ >= minSeconds_) {
            progress = 1.0f;
            phase_ = WarmupPhase::Reveal;
        }
        break;
    }
    case WarmupPhase::Reveal:
    case WarmupPhase::Idle:
        progress = 1.0f;
        break;
    }
    // Failsafe: nothing (missing asset, dead worker) may lock the
    // player out — after two minutes the veil opens on what is there.
    if (frames_ > 7200) {
        phase_ = WarmupPhase::Reveal;
        progress = 1.0f;
    }
    progress_ = glm::max(progress_, progress);
    // Behind the opaque boot veil, stream flat-out: the anti-stutter
    // budgets protect a frame nobody sees. The soft (spectator) veil
    // keeps them — the scene stays visible there.
    ctx.renderer.setStreamingBoost(!soft_ && phase_ != WarmupPhase::Reveal &&
                                   phase_ != WarmupPhase::Idle);
    // The DISPLAYED bar chases the truth at a capped rate: discrete
    // completions become a glide, and it never overtakes reality.
    shownProgress_ = glm::min(progress_, shownProgress_ + dt * 0.35f);

    // Veil: black shroud for full warmups, a light one for the
    // spectator catch-up. The bar visually finishes before the fade.
    if (phase_ == WarmupPhase::Reveal) {
        if (shownProgress_ >= 0.999f || soft_) {
            veilAlpha_ = glm::max(0.0f, veilAlpha_ - dt / 0.8f);
            if (veilAlpha_ <= 0.0f) {
                phase_ = WarmupPhase::Idle;
                if (soft_) {
                    softVeilCooldown_ = 3.0f; // no border strobing
                }
                if (!soft_) {
                    ctx.onRevealed();
                }
            }
        }
    } else {
        veilAlpha_ = glm::min(soft_ ? 0.55f : 1.0f, veilAlpha_ + dt * 3.0f);
    }
    // Feed the RmlUi loading screen (loading.rml).
    if (ctx.uiCreated && ctx.screenStack.find("loading")) {
        char label[32];
        std::snprintf(label, sizeof(label), "%.0f%%",
                      static_cast<f64>(shownProgress_) * 100.0);
        ctx.uiSystem.setNumber("loading", "loadingAlpha", veilAlpha_);
        ctx.uiSystem.setNumber(
            "loading", "loadingBarAlpha",
            glm::clamp((veilAlpha_ - (soft_ ? 0.1f : 0.6f)) / 0.4f, 0.0f,
                       1.0f));
        ctx.uiSystem.setNumber("loading", "loadingPct",
                               static_cast<f64>(shownProgress_) * 100.0);
        ctx.uiSystem.setString("loading", "loadingText", label);
        if (phase_ == WarmupPhase::Idle && veilAlpha_ <= 0.0f) {
            ctx.screenStack.close("loading");
            ctx.syncScreens();
        }
    }
}

} // namespace game
