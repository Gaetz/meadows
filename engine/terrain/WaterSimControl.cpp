#include "engine/terrain/WaterSimControl.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Clock.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"

namespace render::terrain {

void WaterSimControl::create(core::JobSystem& jobSystem) {
    simJobs.create(jobSystem, "waterSim");
}

void WaterSimControl::destroy() {
    // Identity survives the reset: the lane (its generation keeps
    // moving ACROSS the reset so every in-flight job dies on arrival —
    // a fresh zero would let it land as if current), `simEpoch`
    // likewise, the config and the source provider. Everything else
    // returns to the default-constructed state.
    core::BakeMailbox<SimResult> savedSim = std::move(simJobs);
    savedSim.reset();
    simTransition(SimPhase::Off, "destroy"); // bumps the epoch
    const u32 savedSimEpoch = simEpoch;
    SimConfig savedSimCfg = std::move(simCfg);
    SimSourcesFn savedSimSourcesFn = std::move(simSourcesFn);
    *this = WaterSimControl {};
    simJobs = std::move(savedSim);
    simEpoch = savedSimEpoch;
    simCfg = std::move(savedSimCfg);
    simSourcesFn = std::move(savedSimSourcesFn);
}

Vec4 WaterSimControl::simMapInfo() const {
    // Until the window is Live (settle gate released) the shaders see
    // "no sim" (w = 0): the baked water stays on screen everywhere and
    // the window keeps simulating behind the curtain.
    if (simPhase != SimPhase::Live || !simSnap || simSnap->spec.n < 2) {
        return { 0.0f, 0.0f, 0.0f, 0.0f };
    }
    const auto& spec = simSnap->spec;
    const f32 span = static_cast<f32>(spec.n - 1) * spec.texelSize;
    return { spec.originX, spec.originZ, 1.0f / span, 1.0f };
}

void WaterSimControl::simCachePush(sptr<WaterSimState> state,
                                   f32 replaceRadius, bool pushIfNoMatch) {
    if (!state || !state->valid()) {
        return;
    }
    // One entry per window footprint: an entry within replaceRadius
    // is superseded (same zone, staler water).
    bool matched = false;
    for (size_t i = 0; i < simCache.size(); ++i) {
        const auto& spec = simCache[i]->spec;
        if (spec.n == state->spec.n &&
            std::abs(spec.texelSize - state->spec.texelSize) < 1.0e-3f &&
            std::abs(spec.originX - state->spec.originX) <
                replaceRadius &&
            std::abs(spec.originZ - state->spec.originZ) <
                replaceRadius) {
            simCache.erase(simCache.begin() + static_cast<i32>(i));
            matched = true;
            break;
        }
    }
    if (!matched && !pushIfNoMatch) {
        return; // refresh-only call: never grow, never evict the trail
    }
    if (simCache.size() >= kSimCacheCap) {
        simCache.erase(simCache.begin()); // front = least recent
    }
    simCache.push_back(std::move(state));
}

void WaterSimControl::simFreeze(const sptr<const WaterSimSnapshot>& snap,
                                f32 replaceRadius, bool pushIfNoMatch) {
    gpuRequests.push_back({ .kind = GpuRequest::Kind::FreezeMesh,
                            .snap = snap,
                            .replaceRadius = replaceRadius,
                            .pushIfNoMatch = pushIfNoMatch });
}

const char* WaterSimControl::simPhaseName(SimPhase phase) {
    switch (phase) {
    case SimPhase::Off:
        return "off";
    case SimPhase::AwaitingGround:
        return "awaiting-ground";
    case SimPhase::PreRolling:
        return "pre-rolling";
    case SimPhase::Settling:
        return "settling";
    case SimPhase::Live:
        return "live";
    }
    return "?";
}

void WaterSimControl::simTransition(SimPhase next,
                                    [[maybe_unused]] const char* why) {
    if (next == simPhase) {
        return;
    }
    if (next == SimPhase::Off) {
        ++simEpoch; // results of the window left behind die on arrival
    }
    LOG_DEBUG("Water sim: {} -> {} ({})", simPhaseName(simPhase),
              simPhaseName(next), why);
    simPhase = next;
}

void WaterSimControl::simInvalidate(const char* reason) {
    simState.reset();
    simSnap.reset();
    simAccum = 0.0f;
    simHasLastCrumb = false;
    simTransition(SimPhase::Off, reason);
}

terraingen::GridSpec WaterSimControl::simWindowSpecAt(
    const Vec3& cameraPos) const {
    const f32 texel = simWindowTexel();
    terraingen::GridSpec spec;
    spec.texelSize = texel;
    spec.n = static_cast<u32>(simCfg.span / texel) + 1;
    spec.originX =
        std::floor((cameraPos.x - simCfg.span * 0.5f) / texel) * texel;
    spec.originZ =
        std::floor((cameraPos.z - simCfg.span * 0.5f) / texel) * texel;
    return spec;
}

void WaterSimControl::simArmSettleGate() {
    simCalmTicks = 0;
    simLastVolume = -1.0;
    simSettleTimer = 0.0f;
}

const vector<WaterSimControl::GpuRequest>& WaterSimControl::update(
    const Inputs& in) {
    gpuRequests.clear();
    if (simFrozenClearPending) {
        gpuRequests.push_back(
            { .kind = GpuRequest::Kind::ClearFrozenMeshes });
        simFrozenClearPending = false;
    }
    simDrainResults();
    if (!simApplyPolicy(in)) {
        return gpuRequests;
    }
    if (!simState && !simMaybeEnter(in)) {
        return gpuRequests;
    }
    simTick(in);
    return gpuRequests;
}

void WaterSimControl::simDrainResults() {
    // Apply finished jobs (newest wins; at most one is ever in flight).
    simJobs.drain([&](SimResult& res) {
        if (res.epoch != simEpoch) {
            // Invalidated while the job ran (teleport): the state is
            // still a perfectly settled window — cache it for the
            // return trip instead of dropping it, and keep its mesh
            // frozen on screen.
            if (res.snap) {
                simFreeze(res.snap, simCfg.span * 0.5f, true);
            }
            simCachePush(std::move(res.state), simCfg.span * 0.5f,
                         true);
            return;
        }
        simState = res.state;
        simLastMs = res.millis;
        if (res.sourcesFresh) {
            simSrcCache = std::move(res.sources);
        }
        if (!res.snap) {
            return;
        }
        u32 wet = 0;
        for (const f32 d : res.snap->depth) {
            wet += d > 0.0f ? 1u : 0u;
        }
        simWetCells = wet;
        gpuRequests.push_back({ .kind = GpuRequest::Kind::UploadWindow,
                                .snap = res.snap });
        simSnap = res.snap;
        switch (simPhase) {
        case SimPhase::Settling:
        case SimPhase::Live:
            break;
        case SimPhase::PreRolling:
        case SimPhase::Off:
        case SimPhase::AwaitingGround:
            // The pre-roll landed: the window stays behind the settle
            // gate unless the gate is off or its clock (running since
            // the kick) already passed the cap.
            if (simCfg.settleGated && simSettleTimer > kSimSettleCap) {
                LOG_INFO("Water sim: revealed at CAP ({:.1f} s, window "
                         "not calm — {} calm tick(s), last volume "
                         "{:.0f} m3)",
                         kSimSettleCap, simCalmTicks, simLastVolume);
                simTransition(SimPhase::Live,
                              "pre-roll landed past the settle cap");
            } else {
                simTransition(simCfg.settleGated ? SimPhase::Settling
                                                 : SimPhase::Live,
                              "pre-roll landed");
            }
            break;
        }
        if (simPhase == SimPhase::Settling) {
            // Settle-gate calm metric: consecutive published volumes
            // within a relative epsilon = the window is calm, reveal.
            const f64 ref = glm::max(std::abs(simLastVolume), 1.0);
            if (simLastVolume >= 0.0 &&
                std::abs(res.volume - simLastVolume) / ref <
                    static_cast<f64>(kSimCalmEps)) {
                ++simCalmTicks;
            } else {
                simCalmTicks = 0;
            }
            simLastVolume = res.volume;
            if (simCalmTicks >= kSimCalmTicks) {
                LOG_INFO("Water sim: revealed CALM after {:.1f} s "
                         "(volume {:.0f} m3, {} wet cells)",
                         simSettleTimer, res.volume, wet);
                simTransition(SimPhase::Live, "calm");
            }
        }
    });
}

bool WaterSimControl::simApplyPolicy(const Inputs& in) {
    const TerrainParams& params = *in.params;
    const Vec3& cameraPos = in.cameraPos;
    const f32 dt = in.dt;
    // The settle clock runs from the (re)entry, pre-roll included. The
    // reveal never blocks: a window that keeps sloshing (heavy inflow)
    // shows after the cap regardless. Toggling the gate off reveals
    // immediately.
    switch (simPhase) {
    case SimPhase::PreRolling:
        simSettleTimer += dt;
        break;
    case SimPhase::Settling:
        simSettleTimer += dt;
        if (!simCfg.settleGated || simSettleTimer > kSimSettleCap) {
            if (simCfg.settleGated) {
                // A cap-path reveal shows a window still moving — the
                // shore dip a hard swap can flash (diagnosis lane for
                // the reveal-gap report, docs/CPU-PERF.md).
                LOG_INFO("Water sim: revealed at CAP ({:.1f} s, window "
                         "not calm — {} calm tick(s), last volume "
                         "{:.0f} m3)",
                         kSimSettleCap, simCalmTicks, simLastVolume);
            }
            simTransition(SimPhase::Live,
                          simCfg.settleGated ? "settle cap" : "gate off");
        }
        break;
    case SimPhase::Off:
    case SimPhase::AwaitingGround:
    case SimPhase::Live:
        break;
    }
    if (!simCfg.enabled || !params.base) {
        const char* why = simCfg.enabled ? "no terrain base" : "disabled";
        switch (simPhase) {
        case SimPhase::Settling:
        case SimPhase::Live:
            simInvalidate(why);
            break;
        case SimPhase::AwaitingGround:
            simTransition(SimPhase::Off, why);
            break;
        case SimPhase::PreRolling: // the solve lands first, then drops
        case SimPhase::Off:
            break;
        }
        return false;
    }
    // Fast travel does NOT invalidate: the window SCROLLS at any
    // speed (interior preserved bit-exactly, only entering strips
    // re-init) — dropping the state made every spectator move replay
    // the waterfall from scratch. Only a true TELEPORT (a jump past
    // half a window in one frame) re-enters through the pre-roll.
    const Vec2 camXz { cameraPos.x, cameraPos.z };
    Vec2 camDelta { 0.0f, 0.0f };
    if (simHasLastCam) {
        camDelta = camXz - simLastCam;
    }
    simLastCam = camXz;
    simHasLastCam = true;
    if (simState &&
        (std::abs(camDelta.x) > simCfg.span * 0.5f ||
         std::abs(camDelta.y) > simCfg.span * 0.5f)) {
        // Teleport: keep the settled window for the return trip, and
        // its mesh frozen on screen.
        if (simSnap) {
            simFreeze(simSnap, simCfg.span * 0.5f, true);
        }
        simCachePush(std::move(simState), simCfg.span * 0.5f, true);
        simInvalidate("teleport");
    }
    if (simJobs.busy()) {
        return false;
    }
    // Main owns the state here: honor a pending dump request.
    if (!simDumpPath.empty() && simState) {
        // Boundary + runtime sources: the replay reproduces a cast
        // spring exactly as the step job saw it.
        vector<terraingen::WaterSource> dumpSources = simSrcCache;
        dumpSources.insert(dumpSources.end(), simRuntimeSources.begin(),
                           simRuntimeSources.end());
        if (dumpSimState(*simState, simCfg.params, dumpSources,
                         simDumpPath.c_str())) {
            LOG_INFO("Water sim: state dumped to {}", simDumpPath);
        }
        simDumpPath.clear();
    }
    // Live span/texel knob change: rebuild the window from scratch.
    const f32 texel = simWindowTexel();
    if (simState &&
        (std::abs(simState->spec.texelSize - texel) > 1.0e-3f ||
         std::abs(static_cast<f32>(simState->spec.n - 1) *
                      simState->spec.texelSize -
                  simCfg.span) > texel * 2.0f)) {
        simInvalidate("window knob changed");
    }
    return true;
}

bool WaterSimControl::simMaybeEnter(const Inputs& in) {
    const TerrainParams& params = *in.params;
    const Vec3& cameraPos = in.cameraPos;
    // Wait for the ground truth: pre-rolling before the camera's tile
    // is published would settle water on the analytic fallback terrain,
    // meters off the baked one. The authored (story) world has no
    // streamed tiles: its base is final, the window may start anywhere.
    if (!params.groundFinalAt(cameraPos.x, cameraPos.z)) {
        simTransition(SimPhase::AwaitingGround,
                      "camera tile not published");
        return false;
    }
    // Teleport / first entry: async pre-roll to equilibrium.
    const terraingen::GridSpec spec = simWindowSpecAt(cameraPos);
    // Session cache first: a window evicted here earlier RESUMES
    // (scrolled by the regular tick) instead of re-running the
    // pre-roll solver — returning water is already flowing.
    {
        vector<terraingen::GridSpec> specs;
        specs.reserve(simCache.size());
        for (const auto& s : simCache) {
            specs.push_back(s->spec);
        }
        const CachedWindowPick pick = chooseCachedWindow(specs, spec);
        if (pick.index >= 0) {
            simState = std::move(
                simCache[static_cast<size_t>(pick.index)]);
            simCache.erase(simCache.begin() + pick.index);
            // The ground may have re-baked while evicted: refresh
            // it on the next job; the regular tick scrolls the
            // window to the new origin and re-pins the lakes.
            simGroundDirty = true;
            simArmSettleGate();
            simTransition(simCfg.settleGated ? SimPhase::Settling
                                             : SimPhase::Live,
                          "resumed from the session cache");
            return true;
        }
    }
    simArmSettleGate();
    simTransition(SimPhase::PreRolling, "pre-roll");
    const f32 maxX =
        spec.originX + static_cast<f32>(spec.n - 1) * spec.texelSize;
    const f32 maxZ =
        spec.originZ + static_cast<f32>(spec.n - 1) * spec.texelSize;
    simJobs.kick([params, spec, simParams = simCfg.params,
                  fn = simSourcesFn, bodiesRef = in.bodies,
                  epoch = simEpoch, maxX, maxZ,
                  pinHw = simCfg.pinRiverHalfWidth](
                     SimResult& out, const std::atomic<bool>&) {
        out.epoch = epoch;
        const core::TimePoint start = core::clockNow();
        if (fn) {
            out.sources = fn(spec.originX, spec.originZ, maxX, maxZ);
        }
        if (bodiesRef) {
            const auto baked = entrySourcesFromBodies(
                *bodiesRef, spec, out.sources, simParams, pinHw);
            out.sources.insert(out.sources.end(), baked.begin(),
                               baked.end());
        }
        out.sourcesFresh = true;
        auto state = std::make_shared<WaterSimState>(preRollWindow(
            spec,
            [&params](f32 x, f32 z) { return height(params, x, z); },
            simParams, out.sources));
        if (bodiesRef) {
            // Baked lakes = pinned reservoirs, then a settling
            // burst (~15 s of sim, well under a second of worker):
            // the offline pre-roll knows nothing of the pins, so
            // without this every (re)entry showed falls RESTARTING
            // from a dry cliff instead of already flowing.
            pinLakes(*state, *bodiesRef);
            pinRivers(*state, *bodiesRef, pinHw);
            stepWindow(*state, simParams, out.sources, 450);
        }
        auto snap = std::make_shared<WaterSimSnapshot>();
        extractSnapshot(*state, simParams, *snap);
        f64 volume = 0.0;
        for (const f32 d : snap->depth) {
            volume += d;
        }
        out.volume = volume * spec.texelSize * spec.texelSize;
        out.state = std::move(state);
        out.snap = std::move(snap);
        out.millis =
            static_cast<f32>(core::secondsSince(start) * 1000.0);
    });
    return false;
}

void WaterSimControl::simTick(const Inputs& in) {
    const TerrainParams& params = *in.params;
    const Vec3& cameraPos = in.cameraPos;
    const f32 dt = in.dt;
    // Regular tick: fixed-dt accumulator (hitch-clamped), re-anchor by
    // hysteresis, ground refresh on terraforming. The time scale
    // multiplies accumulated sim-time only — per-substep dt is fixed
    // (CFL intact), and the hitch clamp scales with it so a fast sim
    // is not silently throttled back to 1x.
    const f32 timeScale = glm::clamp(simCfg.timeScale, 0.25f, 8.0f);
    simAccum += glm::min(dt, 0.25f) * timeScale;
    const f32 tickDt = glm::max(simCfg.params.dt, 1.0f / 240.0f);
    u32 substeps = static_cast<u32>(simAccum / tickDt);
    substeps = glm::min(
        substeps,
        static_cast<u32>(glm::ceil(
            static_cast<f32>(simCfg.maxSubsteps) *
            glm::max(timeScale, 1.0f))));
    simAccum = glm::min(simAccum - static_cast<f32>(substeps) * tickDt,
                        tickDt);
    const auto& spec = simState->spec;
    const f32 span = static_cast<f32>(spec.n - 1) * spec.texelSize;
    const f32 centerX = spec.originX + span * 0.5f;
    const f32 centerZ = spec.originZ + span * 0.5f;
    i32 dCol = 0;
    i32 dRow = 0;
    if (std::abs(cameraPos.x - centerX) > simCfg.anchorHysteresis) {
        dCol = static_cast<i32>(
            std::lround((cameraPos.x - centerX) / spec.texelSize));
    }
    if (std::abs(cameraPos.z - centerZ) > simCfg.anchorHysteresis) {
        dRow = static_cast<i32>(
            std::lround((cameraPos.z - centerZ) / spec.texelSize));
    }
    if (substeps == 0 && dCol == 0 && dRow == 0 && !simGroundDirty) {
        return;
    }
    // Breadcrumb drop: every half-window of TRAVEL (scroll never
    // evicts, so without this the cache only ever saw teleports), a
    // copy of the current state joins the cache (~2.4 MB, sub-ms).
    if (!simHasLastCrumb) {
        simLastCrumb = { spec.originX, spec.originZ };
        simHasLastCrumb = true;
    } else if (std::abs(spec.originX - simLastCrumb.x) >
                   simCfg.span * 0.5f ||
               std::abs(spec.originZ - simLastCrumb.y) >
                   simCfg.span * 0.5f) {
        simCachePush(std::make_shared<WaterSimState>(*simState),
                     simCfg.span * 0.5f, true);
        if (simSnap) {
            // Trail stays visible behind.
            simFreeze(simSnap, simCfg.span * 0.5f, true);
        }
        simLastCrumb = { spec.originX, spec.originZ };
        simFreshTimer = 0.0f;
    } else {
        // Timed refresh of the CURRENT footprint (see the member
        // comment: dynamic-only water formed since the last drop must
        // reach the crumb and the frozen mesh before it is evicted;
        // faster sim = faster cadence). REPLACE-ONLY, tight radius:
        // with the travel-wide radius it superseded the previous
        // trail entry every few seconds of walking — ONE entry slid
        // along with the player and the waterfall behind vanished
        // past ~half a window. While moving, the jalons own the trail
        // (a jalon's window still covers the 256 m behind it, so water
        // formed on the move is captured).
        simFreshTimer += dt * glm::max(timeScale, 1.0f);
        if (simFreshTimer >= kSimFreshSeconds) {
            simFreshTimer = 0.0f;
            simCachePush(std::make_shared<WaterSimState>(*simState),
                         kSimFreshRadius, false);
            if (simSnap) {
                simFreeze(simSnap, kSimFreshRadius, false);
            }
        }
    }
    // Crumbs for the entering strips (immutable while the job reads
    // them: one job in flight, and a cache pop can only happen after
    // it lands).
    vector<sptr<const WaterSimState>> crumbs;
    if (dCol != 0 || dRow != 0) {
        crumbs.reserve(simCache.size());
        for (const auto& c : simCache) {
            crumbs.push_back(c);
        }
    }
    const LiveStepRequest request {
        .dCol = dCol,
        .dRow = dRow,
        .substeps = substeps,
        .refreshGround = simGroundDirty,
        .pinRiverHalfWidth = simCfg.pinRiverHalfWidth,
        .rainIntensity = simRainIntensity,
        .rainPuddles = simCfg.rainPuddles,
    };
    simGroundDirty = false;
    simJobs.kick([params, state = simState, simParams = simCfg.params,
                  fn = simSourcesFn, bodiesRef = in.bodies,
                  sources = simSrcCache, runtime = simRuntimeSources,
                  epoch = simEpoch, request, crumbs = std::move(crumbs)](
                     SimResult& out, const std::atomic<bool>&) mutable {
        out.epoch = epoch;
        const core::TimePoint start = core::clockNow();
        const auto heightFn = [&params](f32 x, f32 z) {
            return height(params, x, z);
        };
        LiveStepResult stepped = stepLiveWindow(
            *state, simParams, heightFn, bodiesRef.get(), fn,
            std::move(sources), runtime, crumbs, request);
        out.volume = stepped.volume;
        out.state = std::move(state);
        out.snap = std::move(stepped.snap);
        out.sources = std::move(stepped.sources);
        out.sourcesFresh = stepped.sourcesFresh;
        out.millis =
            static_cast<f32>(core::secondsSince(start) * 1000.0);
    });
}

} // namespace render::terrain
