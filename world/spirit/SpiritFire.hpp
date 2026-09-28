#pragma once

#include <array>

#include "engine/core/Defines.hpp"
#include "engine/terrain/FireField.hpp"
#include "world/spirit/SpiritRules.hpp"

// Chantier ESPRITS E3 — the fire lane, headless: the camera window the
// fire field lives in (the water sim's footprint and scroll discipline),
// the material -> fuel blend the kernel's FuelFn is built from, and the
// ONE job body a worker runs (init/scroll, ignitions, N steps, the render
// mask and the emitter centers). The scene owns the grid between jobs
// and moves it in and out (one job in flight, value copies of everything
// else — the Phase-5 rule: a worker touches neither the World nor the
// GPU). Deterministic for a given input, like the kernel.

namespace world {

struct FireWindow {
    static constexpr f32 kSpan = 512.0f; // the water sim's footprint
    static constexpr f32 kTexel = 2.0f;
    // The focus straying past this from the window's center scrolls it
    // (a cast reaches ~180 m: the window must still cover the aim).
    static constexpr f32 kRecenter = kSpan * 0.125f;
    // The window centered (cell-snapped) on the focus.
    static render::terraingen::GridSpec specFor(f32 focusX, f32 focusZ);
    // The whole-cell scroll bringing `spec` back over the focus — zero
    // while the focus is within kRecenter of the window's center.
    static void scrollFor(const render::terraingen::GridSpec& spec,
                          f32 focusX, f32 focusZ, i32* dCol, i32* dRow);
};

// The terrain splat classes the fuel is blended over (the same names
// SurfaceMaterialForm.materialClass carries).
enum class GroundClass : u8 { Grass = 0, Rock, Cliff, Snow, Sand, kCount };
constexpr size_t kGroundClasses = static_cast<size_t>(GroundClass::kCount);
using GroundProps = std::array<MaterialProps, kGroundClasses>;
using GroundWeights = std::array<f32, kGroundClasses>;
// The compiled rule table's props by ground class; a class without a
// record is inert (no fuel).
GroundProps groundPropsFrom(const SpiritRuleTable& rules);
// One cell's fuel from the splat weights (normalized here) and the
// baked region wetness (a marsh damps like a wet material).
render::terrain::FireCellFuel fuelFromWeights(const GroundProps& props,
                                              const GroundWeights& weights,
                                              f32 wetness);

struct FireIgnition {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 radius { 1.0f };
    f32 heat { 1.0f };
};

struct FireDouse {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 radius { 1.0f };
};

// One job's input, moved in. An invalid grid (no window yet) is
// initialised at `spec`; a valid one is scrolled to `spec` when the
// origins differ (whole cells, bit-exact interior).
struct FireJobInput {
    render::terrain::FireGrid grid;
    render::terraingen::GridSpec spec;
    render::terrain::FireParams params;
    vector<FireIgnition> ignitions;
    vector<FireDouse> douses; // applied before the ignitions
    u32 steps { 1 };
    render::terrain::FuelFn fuel;
    render::terrain::WetFn wet;
    u32 epoch { 0 };
    u32 maxCenters { 1024 };
};

struct FireJobOutput {
    render::terrain::FireGrid grid;
    render::terrain::FireStats stats;
    vector<u8> scorch;    // the render mask (fireScorch)
    vector<u8> glow;      // the ember mask (fireGlow)
    vector<u8> state;     // FireState per cell (the gameplay's "is it burning")
    vector<Vec2> burning; // burning cell centers, hottest first
    u32 epoch { 0 };
    f32 millis { 0.0f };
    // False once nothing burns: the lane idles (no job) until the next
    // ignition; the scorch stays as it is.
    bool active { false };
};

FireJobOutput runFireJob(FireJobInput&& in);

} // namespace world
