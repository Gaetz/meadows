#include "game/ui/RenderTuningPanels.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <type_traits>

#include "engine/core/Assert.hpp"
#include "engine/core/FrameProbe.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/AtmosphereParams.hpp"
#include "engine/render/WorldRenderer.hpp"
#include "game/scenes/RenderTuningIo.hpp"
#include "game/ui/PropertyGrid.hpp"

#include <imgui.h>

namespace game {

namespace {

using namespace tuning_lanes;

template <typename M>
struct MemberOf;
template <typename C, typename T>
struct MemberOf<T C::*> {
    using Class = C;
};

// The reflected range of a Form field named by member pointer, resolved
// once per field (the lookup walks a probe instance of the Form).
template <auto Field>
const reflect::FieldRange& fieldRange() {
    using Form = typename MemberOf<decltype(Field)>::Class;
    static const reflect::FieldRange* const range = [] {
        const reflect::FieldInfo* info =
            Form::staticTypeInfo().findFieldAt(reflect::memberOffset(Field));
        ENGINE_ASSERT_MSG(info && info->range, "{}: field without a range",
                          Form::staticTypeInfo().name);
        static constexpr reflect::FieldRange kUnbounded { -3.4e38f, 3.4e38f };
        return info && info->range ? info->range : &kUnbounded;
    }();
    return *range;
}

ImGuiSliderFlags sliderFlags(const reflect::FieldRange& range) {
    return range.logarithmic ? ImGuiSliderFlags_Logarithmic : 0;
}

// A hand-placed slider over a Form field's reflected range, for the knobs
// whose live value is not a lane (a Vec4 component, a clamped scalar).
// `format` overrides the field's declared precision when given.
template <auto Field>
bool rangedSlider(const char* label, f32& value,
                  const char* format = nullptr) {
    const reflect::FieldRange& range = fieldRange<Field>();
    return ImGui::SliderFloat(label, &value, range.lo, range.hi,
                              format ? format : rangeFormat(range, "%.3f"),
                              sliderFlags(range));
}
template <auto Field>
bool rangedSliderInt(const char* label, i32& value) {
    const reflect::FieldRange& range = fieldRange<Field>();
    return ImGui::SliderInt(label, &value, static_cast<int>(range.lo),
                            static_cast<int>(range.hi));
}

// Draws one widget per lane of a table, in table order: a checkbox for a
// bool lane, a slider for a numeric one bounded by the Form field's
// reflected range (REFLECT_FIELD_RANGE) and labelled by the lane. The
// table is a template argument so its field lookup resolves once.
// Returns true when a widget finished an edit this frame (slider release,
// checkbox click): the callers' regen/rescatter trigger.
template <const auto& Lanes, typename Params>
bool drawLanes(Params& params) {
    using Lane = std::remove_cvref_t<decltype(Lanes[0])>;
    using Form = typename Lane::FormType;
    using T = typename Lane::ValueType;
    static_assert(std::is_same_v<typename Lane::ParamsType, Params>,
                  "drawLanes: the params struct is not the table's");
    static_assert(std::is_same_v<T, bool> || std::is_same_v<T, f32> ||
                      std::is_same_v<T, i32>,
                  "drawLanes draws bool, f32 and i32 lanes");
    constexpr std::size_t count = std::size(Lanes);
    static const std::array<const reflect::FieldInfo*, count> fields = [] {
        std::array<const reflect::FieldInfo*, count> out {};
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = Form::staticTypeInfo().findFieldAt(
                reflect::memberOffset(Lanes[i].form));
            ENGINE_ASSERT_MSG(out[i] != nullptr,
                              "{}: lane without a reflected field",
                              Form::staticTypeInfo().name);
        }
        return out;
    }();

    bool edited = false;
    for (std::size_t i = 0; i < count; ++i) {
        const Lane& lane = Lanes[i];
        const reflect::FieldInfo* field = fields[i];
        const char* label =
            lane.label ? lane.label : (field ? field->name.c_str() : "?");
        T& value = params.*lane.param;
        if constexpr (std::is_same_v<T, bool>) {
            edited |= ImGui::Checkbox(label, &value);
        } else {
            const reflect::FieldRange* range = field ? field->range : nullptr;
            ENGINE_ASSERT_MSG(range != nullptr, "{}: lane without a range",
                              label);
            if constexpr (std::is_same_v<T, f32>) {
                if (range) {
                    ImGui::SliderFloat(label, &value, range->lo, range->hi,
                                       rangeFormat(*range, "%.3f"),
                                       sliderFlags(*range));
                } else {
                    ImGui::DragFloat(label, &value, 0.01f);
                }
            } else {
                if (range) {
                    ImGui::SliderInt(label, &value,
                                     static_cast<int>(range->lo),
                                     static_cast<int>(range->hi));
                } else {
                    ImGui::InputInt(label, &value);
                }
            }
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        }
    }
    return edited;
}

} // namespace

void RenderTuningPanels::drawPerfPanel(render::WorldRenderer& r,
                                       const core::FrameProbe* cpuProbe) {
    if (!r.gpuProbe.active()) {
        ImGui::TextDisabled("(no GPU timer queries on this device)");
        return;
    }
    ImGui::Text("GPU frame: %.2f ms avg  %.2f ms max",
                r.gpuProbe.frameAverageMs(), r.gpuProbe.frameMaxMs());
    ImGui::SameLine();
    if (ImGui::SmallButton("reset window")) {
        r.gpuProbe.resetWindow();
    }
    if (!ImGui::BeginTable("gpuperf", 4,
                           ImGuiTableFlags_SizingStretchProp |
                               ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("pass");
    ImGui::TableSetupColumn("GPU avg (ms)");
    ImGui::TableSetupColumn("GPU max");
    ImGui::TableSetupColumn("CPU (ms)");
    ImGui::TableHeadersRow();
    for (const render::GpuProbe::PassRow& row : r.gpuProbe.rows()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.name);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", row.stats.averageMs);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", row.stats.maxMs);
        ImGui::TableNextColumn();
        // The CPU column: the FrameProbe scope of the SAME name when one
        // exists (the CPU probes also cover streaming blocks the GPU
        // never sees — those rows are simply absent here).
        f64 cpuMs = -1.0;
        if (cpuProbe) {
            for (const core::FrameProbe::Entry& entry :
                 cpuProbe->currentEntries()) {
                if (std::strcmp(entry.name, row.name) == 0) {
                    cpuMs = entry.ms;
                    break;
                }
            }
        }
        if (cpuMs >= 0.0) {
            ImGui::Text("%.2f", cpuMs);
        } else {
            ImGui::TextDisabled("-");
        }
    }
    ImGui::EndTable();
    if (r.gpuProbe.rows().empty()) {
        ImGui::TextDisabled("(warming up — first frames resolving)");
    }

    // Worker-job costs (JobProbe): what the bakes burn off-frame. Total
    // accumulates since the last reset — the chantier-économie baseline
    // number (docs/CPU-PERF.md).
    if (r.jobSystem) {
        core::JobProbe& jobProbe = r.jobSystem->probe();
        ImGui::SeparatorText("Worker jobs (since reset)");
        ImGui::Text("CPU bakes: %.1f s total", jobProbe.windowTotalMs() / 1000.0);
        ImGui::SameLine();
        if (ImGui::SmallButton("reset##jobs")) {
            jobProbe.resetWindow();
        }
        if (ImGui::BeginTable("jobperf", 5,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("job");
            ImGui::TableSetupColumn("runs");
            ImGui::TableSetupColumn("avg (ms)");
            ImGui::TableSetupColumn("max");
            ImGui::TableSetupColumn("total");
            ImGui::TableHeadersRow();
            for (const core::JobProbe::Row& row : jobProbe.rows()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.name);
                ImGui::TableNextColumn();
                ImGui::Text("%u", row.runs);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", row.averageMs());
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", row.maxMs);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", row.totalMs);
            }
            ImGui::EndTable();
        }
        if (jobProbe.rows().empty()) {
            ImGui::TextDisabled("(no named worker jobs landed yet)");
        }
    }

    // CPU-side geometry counters, ALL passes summed (casters,
    // reflection, main). This is the mainPass dissection on Vulkan, where
    // mid-pass GPU timestamps cannot measure (Metal runs a pass as one
    // tiled unit) — and the input to the impostor decision.
    ImGui::SeparatorText("Geometry this frame (all passes)");
    const f32 terrainMTri =
        static_cast<f32>(r.terrain.indicesThisFrame()) / 3.0e6f;
    const f32 vegMTri =
        static_cast<f32>(r.vegetation.indicesThisFrame()) / 3.0e6f;
    const f32 grassMTri =
        static_cast<f32>(r.grass.indicesThisFrame()) / 3.0e6f;
    ImGui::Text("terrain: %.2f Mtri", terrainMTri);
    ImGui::Text("trees: %.2f Mtri (%u high + %u low + %u ultra instances)",
                vegMTri, r.vegetation.highDetailInstancesThisFrame(),
                r.vegetation.lowDetailInstancesThisFrame(),
                r.vegetation.ultraDetailInstancesThisFrame());
    ImGui::Text("grass: %.2f Mtri (%u blades)", grassMTri,
                r.grass.bladesThisFrame());
    ImGui::Text("total: %.2f Mtri", terrainMTri + vegMTri + grassMTri);
}

bool RenderTuningPanels::drawTreeKnobs(render::WorldRenderer& r) {
    // Every knob reports dirty on RELEASE — regen meshes only,
    // scatter/instances stay. New content re-bakes AO once
    // (content-keyed disk cache). Labels and order ride the lane tables
    // (game/scenes/RenderTuningIo.hpp); the ranges are the tree Forms'
    // reflected metadata.
    bool dirty = false;

    if (ImGui::CollapsingHeader("Lobe trees (classic)")) {
        render::LobeTreeParams& p = r.vegetation.lobeTreeParams;
        dirty |= drawLanes<kLobeTrunk>(p);
        dirty |= drawLanes<kLobeBranchCount>(p);
        dirty |= drawLanes<kLobeCrown>(p);
    }
    if (ImGui::CollapsingHeader("Space colonization",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        render::ColonizedTreeParams& p = r.vegetation.colonizedTreeParams;
        ImGui::SeparatorText("Skeleton (Runions)");
        dirty |= drawLanes<kColonizedGrowth>(p);
        dirty |= drawLanes<kColonizedAttractors>(p);
        dirty |= drawLanes<kColonizedPipe>(p);
        ImGui::SeparatorText("Wood");
        dirty |= drawLanes<kColonizedTubeSides>(p);
        dirty |= drawLanes<kColonizedWood>(p);
        dirty |= drawLanes<kColonizedCurveSubdiv>(p);
        dirty |= drawLanes<kColonizedWoodNoise>(p);
        dirty |= drawLanes<kColonizedFlareLobes>(p);
        ImGui::SeparatorText("Bark material (live, no rebuild)");
        drawLanes<kColonizedBark>(p);
        ImGui::ColorEdit3("Bark tint", &p.barkTint.x,
                          ImGuiColorEditFlags_Float);
        ImGui::SeparatorText("Crown envelope");
        dirty |= drawLanes<kColonizedCrown>(p);
        ImGui::SeparatorText("Conifer habit");
        dirty |= drawLanes<kColonizedConifer>(p);
        ImGui::SeparatorText("Leaf style (atlas slot + season)");
        dirty |= drawLanes<kColonizedLeafStyle>(p);
        const char* kShapes =
            "Pointed ellipse\0Needles\0Rounded\0Lobed\0Serrated\0";
        dirty |= ImGui::Combo("Leaf shape", &p.leafShape, kShapes);
        dirty |= ImGui::ColorEdit3("Autumn tint", &p.autumnTint.x,
                                   ImGuiColorEditFlags_Float);
        dirty |= drawLanes<kColonizedSeason>(p);
        ImGui::SeparatorText("Foliage SDF + cards");
        dirty |= drawLanes<kColonizedFoliage>(p);
        ImGui::SeparatorText("Leaf mask (card texture)");
        dirty |= drawLanes<kColonizedLeafCount>(p);
        dirty |= drawLanes<kColonizedLeafSize>(p);
        // Live shader window (uLeafLodInfo) — no rebuild.
        drawLanes<kColonizedLeafLod>(p);
    }
    return dirty;
}

void RenderTuningPanels::drawTreeBuilderPanel(render::WorldRenderer& r) {
    bool dirty = false;
    if (ImGui::Checkbox("Space-colonization trees (A/B)",
                        &r.vegetation.colonizationTrees)) {
        dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Regenerate")) {
        dirty = true;
    }
    // The CLAUDE.md §5 round trip: the scene captures the live params
    // into the tuning records and writes the overlay plugin.
    if (ImGui::Button("Save render tuning (mods/render-tuning.toml)")) {
        r.saveTuningRequested = true;
    }

    // Bark pick per tree slot (oak / spruce) — group rebuild lands at
    // the next update() safe point.
    if (r.vegetation.barkLoaded() && ImGui::TreeNode("Bark")) {
        static const char* kBarkNames[] = { "Oak", "Spruce" };
        static const char* kSlotNames[] = {
            "Slot 0 (broadleaf)", "Slot 1 (broadleaf)",
            "Slot 2 (broadleaf)", "Slot 3 (conifer)", "Slot 4 (conifer)",
        };
        for (u32 i = 0; i < render::VegetationSystem::kTreeVariants;
             ++i) {
            int pick = r.vegetation.variantBark[i];
            if (ImGui::Combo(kSlotNames[i], &pick, kBarkNames, 2)) {
                r.vegetation.variantBark[i] = static_cast<u8>(pick);
                r.vegetation.barkGroupsDirty = true;
            }
        }
        ImGui::TreePop();
    }

    dirty |= drawTreeKnobs(r);

    if (dirty) {
        r.reseedVegetation = true;
    }
}

void RenderTuningPanels::drawTerrainPanel(render::WorldRenderer& r) {
    if (ImGui::Button("Save render tuning (mods/render-tuning.toml)")) {
        r.saveTuningRequested = true;
    }
    // Live stats stay on top, always visible; the knobs group below.
    ImGui::Text("Resident: %u | drawn: %u | pending: %u | uploads: %u",
                r.terrain.residentCount(), r.terrain.drawnLastFrame(),
                r.terrain.pendingCount(), r.terrain.uploadsLastFrame());
    // (The GPU Hi-Z verdict never crosses the CPU anymore — its culling
    // shows up in `drawn` when the indirect path is on.)
    ImGui::Text("Prop chunks drawn: %u | occluded CPU: %u",
                r.vegetation.drawnLastFrame(), r.occlusion.occludedCount());
    ImGui::Text("Grass blades: %u | props: %u", r.grass.instanceTotal(),
                r.vegetation.propTotal());
    if (ImGui::CollapsingHeader("Terrain",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputScalar("Seed", ImGuiDataType_U32,
                           &r.terrain.params.seed);
        ImGui::SameLine();
        if (ImGui::Button("Regenerate")) {
            r.regenerateRequested = true; // applied at the next render
        }
        // Water plane, sand band and material weights follow live; the
        // scatter (grass/trees/props) is baked per chunk — Regenerate to
        // re-align it. Terrain shape is apply-only, hence no lane.
        rangedSlider<&Tuning::seaLevel>("Sea level (m)",
                                        r.terrain.params.seaLevel);
        // Streaming ring = the draw distance (64 m chunks; the horizon
        // closure tracks it). Chunk count grows as (2r+1)^2 — watch F6.
        // The cap is the engine's constant, not Form metadata.
        ImGui::SliderInt("View radius (chunks)", &r.terrain.viewRadius, 8,
                         render::TerrainSystem::kMaxViewRadius);
        // Far silhouettes, the 24 km ring, impostor grounding from the
        // bake's half-grid vs the analytic function (re-bakes on toggle),
        // and the shared height pyramid master A/B (consumers off it fall
        // back to their exact pointwise paths).
        drawLanes<kTerrainFar>(r.tuning);
    }
    if (ImGui::CollapsingHeader("Vegetation")) {
        drawLanes<kSeasons>(r.tuning);
        // The vegetation draw budget, live
        // (docs/RENDERING.md). Shrinking the ring pops at the edge
        // (the tree fade tops out at 880 m) — a budget-hunting knob.
        // The slider stops at 15 while the load clamp (the field's
        // reflected range) accepts up to 24.
        ImGui::SliderInt("Veg view radius (chunks)",
                         &r.vegetation.viewRadius, 4, 15);
        rangedSliderInt<&Tuning::vegHighDetailRadius>(
            "Veg high-detail radius", r.vegetation.highDetailRadius);
        // 80-face twins within; 20-face ultra beyond.
        rangedSliderInt<&Tuning::vegLowDetailRadius>(
            "Veg low-detail radius", r.vegetation.lowDetailRadius);
        // EXPERIMENT (feature/space-colonization-trees): Runions skeleton
        // + SDF-normal cross-plane foliage vs the solid-lobe trees. The
        // swap re-bakes AO for the new meshes (disk-cached after once).
        if (ImGui::Checkbox("Space-colonization trees (A/B)",
                            &r.vegetation.colonizationTrees)) {
            // applied at the render()-top safe point
            r.reseedVegetation = true;
        }
        ImGui::TextDisabled("(generation knobs: Trees panel)");
    }
    if (ImGui::CollapsingHeader("Culling & debug")) {
        ImGui::Checkbox("Occlusion culling (A/B)", &r.tuning.occlusion);
        ImGui::SameLine();
        ImGui::Checkbox("GPU Hi-Z", &r.tuning.gpuOcclusion);
        ImGui::SameLine();
        ImGui::Checkbox("Indirect draw", &r.tuning.gpuIndirect);
        // Horizon rebake hysteresis: the verdicts stay conservative per
        // position; the drift only ages them.
        drawLanes<kOcclusion>(r.occlusion);
        ImGui::Checkbox("Wireframe (LOD debug)", &r.tuning.wireframe);
    }
}

void RenderTuningPanels::drawRenderPanel(render::WorldRenderer& r,
                                         render::AtmosphereParams& atmos) {
    if (ImGui::Button("Save render tuning (mods/render-tuning.toml)")) {
        r.saveTuningRequested = true;
    }
    // Every meadow constant, live. The render
    // half rides the FrameUbo; a scatter knob queues a grass-only
    // re-scatter on release (budgeted — the ring rebuilds over frames).
    if (ImGui::CollapsingHeader("Fire")) {
        render::FireLook& fl = r.fireScorchMap().look;
        ImGui::SliderFloat("Coal bed (0 = off)", &fl.bedIntensity, 0.0f,
                           10.0f, "%.2f");
        ImGui::SliderFloat("Pulse", &fl.pulse, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Front erosion", &fl.erode, 0.0f, 0.8f, "%.2f");
        ImGui::SliderFloat("Coal scale (m / tile)", &fl.coalScale, 2.0f,
                           16.0f, "%.1f");
        ImGui::SliderFloat("Ash", &fl.ash, 0.0f, 1.0f, "%.2f");
        ImGui::ColorEdit3("Ember hot", &fl.emberHot.x,
                          ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3("Ember cold", &fl.emberCold.x,
                          ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3("Char colour", &fl.charColor.x,
                          ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Grass cull (charred)", &fl.grassCull, 0.3f,
                           1.0f, "%.2f");
        ImGui::SliderFloat("Flame HDR boost", &fl.flameBoost, 0.2f, 6.0f,
                           "%.2f");
        ImGui::SliderFloat("Flame posterize (0 = off)", &fl.flamePosterize,
                           0.0f, 8.0f, "%.0f");
        ImGui::SliderFloat("Heat haze (0 = off)", &fl.hazeStrength, 0.0f,
                           0.05f, "%.3f");
        ImGui::SeparatorText("Lights");
        ImGui::SliderFloat("Light per cell (0 = off)", &fl.lightIntensity,
                           0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Light tile cap", &fl.lightMaxIntensity, 1.0f,
                           30.0f, "%.1f");
        ImGui::SliderFloat("Light radius base (m)", &fl.lightRadius, 1.0f,
                           16.0f, "%.1f");
        ImGui::SliderFloat("Light flicker", &fl.lightFlicker, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderInt("Light count", &fl.lightCount, 0, 48);
        ImGui::ColorEdit3("Light colour", &fl.lightColor.x,
                          ImGuiColorEditFlags_Float);
    }
    if (ImGui::CollapsingHeader("Grass")) {
        render::GrassRenderTuning& gt = r.grass.renderTuning;
        ImGui::SeparatorText("Blade");
        drawLanes<kGrassBlade>(gt);
        ImGui::ColorEdit3("Base tint (x ground)", &gt.baseTint.x,
                          ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3("Tip tint (x ground)", &gt.tipTint.x,
                          ImGuiColorEditFlags_Float);
        ImGui::SeparatorText("Shading");
        drawLanes<kGrassShading>(gt);
        {
            const reflect::FieldRange& bright =
                fieldRange<&Tuning::grassBrightMin>();
            ImGui::DragFloatRange2("Blade brightness", &gt.brightMin,
                                   &gt.brightMax, bright.step, bright.lo,
                                   bright.hi, "min %.2f", "max %.2f");
        }
        drawLanes<kGrassShadingTail>(gt);
        ImGui::SeparatorText("Detail / distance");
        drawLanes<kGrassDistance>(gt);
        ImGui::SeparatorText("Scatter (re-bakes on release)");
        render::GrassScatterTuning& st = r.grass.scatterTuning;
        bool scatterEdited = drawLanes<kGrassScatter>(st);
        {
            const reflect::FieldRange& threshold =
                fieldRange<&Tuning::grassPatchThresholdLo>();
            ImGui::DragFloatRange2("Patch threshold", &st.patchThresholdLo,
                                   &st.patchThresholdHi, threshold.step,
                                   threshold.lo, threshold.hi, "lo %.2f",
                                   "hi %.2f");
            scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
            const reflect::FieldRange& presence =
                fieldRange<&Tuning::grassPresenceLo>();
            ImGui::DragFloatRange2("Presence window", &st.presenceLo,
                                   &st.presenceHi, presence.step,
                                   presence.lo, presence.hi, "rim %.2f",
                                   "solid %.2f");
            scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        }
        scatterEdited |= drawLanes<kGrassScatterCutoff>(st);
        // A/B: coarse ~8 m tint lattice vs one macro-shading eval per
        // 0.6 m corner (docs/CPU-PERF.md).
        scatterEdited |= drawLanes<kGrassScatterTint>(st);
        if (scatterEdited || ImGui::Button("Rescatter now")) {
            r.grassRescatterRequested = true;
        }
    }
    // Every cost-affecting GI parameter is a live knob (workflow:
    // quality first, then the perf descent here, watching
    // the rcInject/rcBuild lines of the F6 table).
    if (ImGui::CollapsingHeader("Global illumination")) {
        render::RcTuning& rc = r.radianceCascades.tuning;
        int technique = rc.technique == render::GiTechnique::RadianceCascades
                            ? 1 : 0;
        if (ImGui::Combo("Technique", &technique,
                         "Classic (ambient x light map)\0"
                         "Radiance cascades (WIP)\0")) {
            rc.technique = technique == 1
                               ? render::GiTechnique::RadianceCascades
                               : render::GiTechnique::Classic;
        }
        ImGui::SeparatorText("Voxel clipmap");
        drawLanes<kRcResolution>(rc);
        drawLanes<kRcVoxels>(rc);
        drawLanes<kRcUpdateInterval>(rc);
        drawLanes<kRcTileDrift>(rc);
        // Wide-stencil grid normals in the tile bake (~÷4 on its cost);
        // OFF = the exact analytic reference path.
        drawLanes<kRcGridNormals>(rc);
        ImGui::TextDisabled("rebake after %.0f m of camera travel",
                            static_cast<f32>(rc.resolution) *
                                rc.coarseVoxel * 1.25f *
                                rc.tileRebakeDrift);
        ImGui::SeparatorText("Cascades");
        drawLanes<kRcCascadeCount>(rc);
        drawLanes<kRcInterval>(rc);
        ImGui::TextDisabled("reach: %.0f m",
                            rc.interval0 *
                                (std::pow(2.0f, static_cast<f32>(
                                                    rc.cascadeCount)) -
                                 1.0f));
        ImGui::SeparatorText("Injection");
        // Splat re-contract: with clustered direct on every surface, a
        // normal light's splat carries only its BOUNCE share.
        drawLanes<kRcInjection>(rc);
        // A/B: chain at end of frame, consumers read frame N-1; the chain
        // on the second queue (needs pipelined); lights via RC only.
        drawLanes<kRcScheduling>(rc);
        ImGui::SeparatorText("Apply");
        // Ambient floor = RC's lower bound as a fraction of classic
        // ambient — the grid-border seam killer (0 = raw RC darkness).
        // Band count = fixed log-step ramp: predictable exposure bands.
        drawLanes<kRcApply>(rc);
        // x4 reach per marched step on long levels (A/B on rcBuild).
        drawLanes<kRcIntervalExtension>(rc);
        ImGui::Combo("Debug view", &rc.debugView,
                     "Off\0Fine clip (raymarch)\0Coarse clip (raymarch)\0"
                     "Cascade 0 irradiance\0");
        // The GPU cost lines, in place (the full table stays on F6).
        for (const auto& row : r.gpuProbe.rows()) {
            if (row.name && str(row.name).rfind("rc", 0) == 0) {
                ImGui::TextDisabled("%s: %.2f ms (max %.2f)", row.name,
                                    row.stats.averageMs, row.stats.maxMs);
            }
        }
    }
    if (ImGui::CollapsingHeader("Lighting & shadows")) {
        ImGui::Checkbox("Stylized lighting (BotW A/B)", &r.tuning.stylized);
        if (r.tuning.stylized && ImGui::TreeNode("Stylized ramp")) {
            // The Form spells the Vec4 lanes as scalars: their ranges
            // drive these component sliders.
            ImGui::TextDisabled("Diffuse: shade -> half-tone -> full light");
            rangedSlider<&Tuning::stylizedDiffuseEdge0Start>(
                "Terminator start", r.tuning.stylizedDiffuse.x);
            rangedSlider<&Tuning::stylizedDiffuseEdge0End>(
                "Terminator end", r.tuning.stylizedDiffuse.y);
            rangedSlider<&Tuning::stylizedDiffuseEdge1Start>(
                "Full-light start", r.tuning.stylizedDiffuse.z);
            rangedSlider<&Tuning::stylizedDiffuseEdge1End>(
                "Full-light end", r.tuning.stylizedDiffuse.w);
            rangedSlider<&Tuning::stylizedHalfTone>(
                "Half-tone level", r.tuning.stylizedShadow.w);
            ImGui::TextDisabled("Cast shadows (CSM snap)");
            rangedSlider<&Tuning::stylizedShadowStart>(
                "Snap window start", r.tuning.stylizedShadow.x);
            rangedSlider<&Tuning::stylizedShadowEnd>(
                "Snap window end", r.tuning.stylizedShadow.y);
            rangedSlider<&Tuning::stylizedShadowFloor>(
                "Shadow floor", r.tuning.stylizedShadow.z);
            ImGui::TextDisabled("Specular band (characters/props)");
            rangedSlider<&Tuning::stylizedSpecStrength>(
                "Spec strength", r.tuning.stylizedSpec.x);
            rangedSlider<&Tuning::stylizedSpecThreshold>(
                "Spec threshold", r.tuning.stylizedSpec.y);
            rangedSlider<&Tuning::stylizedSpecExponent>(
                "Spec exponent", r.tuning.stylizedSpec.z);
            ImGui::TreePop();
        }
        // A/B: per-cluster light lists vs the legacy 24-light loop
        // (docs/RENDERING.md §5). Needs compute; the checkbox is inert
        // (and the budget stays 24) when the culling pass is absent.
        drawLanes<kClusteredLights>(r.tuning);
        ImGui::Checkbox("Shadows", &r.tuning.shadows);
        ImGui::SameLine();
        ImGui::Checkbox("Cascade debug tint", &r.tuning.cascadeDebug);
        // A/B: far cascades on alternate frames —
        // off = every cascade every frame.
        ImGui::Checkbox("CSM round-robin (far cascades 1/2 rate)",
                        &r.tuning.shadowRoundRobin);
        // Sharpness: texels per cascade side (4096 = 2x definition
        // everywhere, ~150 MB more; the far cascade profits most).
        int shadowRes = r.tuning.shadowResolution >= 4096 ? 2
                        : r.tuning.shadowResolution >= 2048 ? 1 : 0;
        if (ImGui::Combo("Shadow map resolution", &shadowRes,
                         "1024\0002048\0004096\000")) {
            r.tuning.shadowResolution = shadowRes == 2 ? 4096
                                   : shadowRes == 1 ? 2048 : 1024;
        }
        // A/B: houses/crates/NPCs casting into the sun cascades.
        ImGui::Checkbox("Mesh shadow casters", &r.tuning.meshShadowCasters);
        ImGui::Checkbox("Contact shadows", &r.tuning.contactShadows);
        ImGui::Checkbox("SSAO", &r.tuning.ssao);
        if (r.tuning.ssao) {
            ImGui::SliderFloat("SSAO strength", &r.tuning.ssaoStrength, 0.0f,
                               2.0f);
            ImGui::SliderFloat("SSAO radius (m)", &r.tuning.ssaoRadius, 0.2f,
                               2.0f);
        }
        const char* ssdmModes[] = { "Off", "Half res", "Full res" };
        ImGui::Combo("SSDM", &r.tuning.ssdmMode, ssdmModes, 3);
        if (r.tuning.ssdmMode != 0) {
            ImGui::SliderFloat("SSDM amplitude (m)", &r.tuning.ssdmAmp,
                               0.02f, 0.30f);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Terrain light map", &r.tuning.terrainLight);
        ImGui::Checkbox("Key light shadow", &r.tuning.keyShadow); // interiors
        // The interior ambient follows the outside (hour + weather).
        drawLanes<kInteriorDaylight>(r.tuning);
    }
    if (ImGui::CollapsingHeader("Sun FX")) {
        drawLanes<kSunFx>(atmos);
        drawLanes<kFroxelFog>(r.postFx);
        drawLanes<kFroxel>(r.postFx);
        drawLanes<kInteriorDust>(r.tuning);
    }
    if (ImGui::CollapsingHeader("Fog & clouds")) {
        drawLanes<kFog>(atmos);
        drawLanes<kFogTail>(r.tuning);
        drawLanes<kFogStart>(atmos);
        // Weather-only (WeatherForm::fogSunScatter): no tuning lane.
        ImGui::SliderFloat("Fog sun scatter", &atmos.fogSunScatter, 0.0f,
                           2.0f, "%.2f");
        // Fog ceiling falloff: how fast the fog layer thins with altitude
        // (high = clear sky above the fog band, low = grey dome).
        drawLanes<kFogSunAndClouds>(atmos);
        ImGui::SeparatorText("Sky clouds (volumetric)");
        // A/B vs the 2D dome layer; coverage/height/scale above drive
        // BOTH implementations (and the ground shadows).
        drawLanes<kSkyClouds>(r.tuning);
        rangedSlider<&Tuning::skyCloudThickness>("Cloud thickness (m)",
                                                 r.tuning.skyCloudShape.x);
        rangedSlider<&Tuning::skyCloudDensity>("Cloud density",
                                               r.tuning.skyCloudShape.y);
        rangedSlider<&Tuning::skyCloudErosion>("Cloud erosion",
                                               r.tuning.skyCloudShape.z);
        // Thickness follows the weather's coverage: full skies tower
        // (at 4, a full sky multiplies the thickness by 5).
        rangedSlider<&Tuning::skyCloudThicknessSpread>(
            "Cloud thickness<->coverage", r.tuning.skyCloudShape.w);
        // Body = multi-octave scattering (luminous cores); lining = the
        // direct transmission exp(-tau)*HG — the silver lining. The
        // drama lives in lining gain + lining lobe + a LOW ambient.
        rangedSlider<&Tuning::skyCloudSunGain>("Cloud body gain",
                                               r.tuning.skyCloudLight.x);
        rangedSlider<&Tuning::skyCloudSunLobe>("Cloud body lobe g",
                                               r.tuning.skyCloudLight.y);
        rangedSlider<&Tuning::skyCloudLiningGain>("Cloud lining gain",
                                                  r.tuning.skyCloudLight.w);
        // Lining lobe, powder, puffiness (fractal edge erosion — the
        // cauliflower florets), rim glow where the cloud is thin along
        // the VIEW ray, and storm darkening (the whole cloud's ambient
        // falls with coverage, bases hardest; sun terms stay — dark
        // slabs rimmed with fire).
        drawLanes<kSkyCloudLook>(r.tuning);
        rangedSlider<&Tuning::skyCloudAmbientGain>("Cloud ambient gain",
                                                   r.tuning.skyCloudLight.z);
        ImGui::SeparatorText("Ground mist");
        drawLanes<kMist>(r.tuning);
        // Weather-owned (WeatherForm): no tuning lane.
        ImGui::SliderFloat("Mist density", &atmos.mistDensity, 0.0f, 2.0f,
                           "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Mist coverage", &atmos.mistCoverage, 0.0f, 1.0f,
                           "%.2f");
        drawLanes<kMistCover>(r.tuning);
        rangedSlider<&Tuning::mistLift>("Mist lift (m)",
                                        r.tuning.mistShape.w);
        rangedSlider<&Tuning::mistCoverageScale>("Mist cover scale",
                                                 r.tuning.mistShape.x);
        rangedSlider<&Tuning::mistErosionScale>("Mist erosion scale",
                                                r.tuning.mistShape.y);
        rangedSlider<&Tuning::mistErosionStrength>("Mist erosion strength",
                                                   r.tuning.mistShape.z);
        ImGui::SeparatorText("Mist lighting");
        drawLanes<kMistSun>(r.tuning);
        // Higher g = tighter, brighter halo hugging the sun direction.
        rangedSlider<&Tuning::mistSunLobe>("Mist sun lobe g",
                                           r.tuning.mistLight.x);
        rangedSlider<&Tuning::mistBackscatter>("Mist backscatter",
                                               r.tuning.mistLight.y);
        // Lower ambient makes the sun beam pop (the silver-lining
        // contrast is ambient-vs-sun, not sun alone).
        rangedSlider<&Tuning::mistAmbientGain>("Mist ambient gain",
                                               r.tuning.mistLight.z);
        rangedSlider<&Tuning::mistShadowFloor>("Mist shadow floor",
                                               r.tuning.mistLight.w);
        // A/B: baked Perlin-Worley volume vs analytic in-shader fbm3;
        // detail dropout distance; fractal edge florets on the patch
        // borders (cloud recipe).
        drawLanes<kMistNoise>(r.tuning);
        drawLanes<kMistDetail>(r.tuning);
        // The slider stops at 8..32 while the load clamp (the field's
        // reflected range) accepts 4..64.
        ImGui::SliderInt("Mist steps", &r.tuning.mistSteps, 8, 32);
        // 1 = accumulation off (A/B); lower = more history.
        drawLanes<kMistBlend>(r.postFx);
    }
    if (ImGui::CollapsingHeader("Water")) {
        ImGui::Checkbox("Reflections", &r.tuning.reflections);
        // Skip the mirror render when no resident water is in
        // view (A/B — the horizon-sea edge case), and trade its resolution.
        ImGui::SameLine();
        ImGui::Checkbox("auto-skip", &r.tuning.reflectionAutoSkip);
        rangedSlider<&Tuning::reflectionScale>("Reflection scale",
                                               r.tuning.reflectionScale);
        ImGui::Combo("Debug view", &r.tuning.waterDebug,
                     "Off\0Flow\0Torrent\0River UV\0");
        ImGui::SeparatorText("Sim window");
        // The live windowed shallow-water sim (option C). Span/texel
        // apply on the next window re-init (toggle off/on to force).
        render::WaterSystem::SimConfig& sim =
            r.waterSystem().simConfig();
        ImGui::Checkbox("Sim enabled", &sim.enabled);
        ImGui::SameLine();
        ImGui::Text("%s | %u wet | job %.2f ms",
                    r.waterSystem().simIsSettling()
                        ? "settling..."
                        : (r.waterSystem().simIsValid()
                               ? "LIVE"
                               : (r.waterSystem().simIsPreRolling()
                                      ? "pre-roll..."
                                      : "OFF/waiting")),
                    r.waterSystem().simWetCellCount(),
                    r.waterSystem().simCostMs());
        ImGui::Checkbox("Settle-gated reveal", &sim.settleGated);
        ImGui::SameLine();
        ImGui::Checkbox("Rain puddles", &sim.rainPuddles);
        rangedSlider<&Tuning::waterSimSpan>("Sim span (m)", sim.span);
        rangedSlider<&Tuning::waterSimTexel>("Sim texel (m)", sim.texel);
        {
            const reflect::FieldRange& substeps =
                fieldRange<&Tuning::waterSimMaxSubsteps>();
            ImGui::SliderInt("Sim max substeps",
                             reinterpret_cast<int*>(&sim.maxSubsteps),
                             static_cast<int>(substeps.lo),
                             static_cast<int>(substeps.hi));
        }
        rangedSlider<&Tuning::waterSimTimeScale>("Sim time scale",
                                                 sim.timeScale, "%.2fx");
        // The Form stores m/s (its range is this one in m/s); the panel
        // reads mm/h.
        f32 rainMmh = sim.params.rainRate * 3.6e6f;
        if (ImGui::SliderFloat("Sim rain (mm/h)", &rainMmh, 0.0f, 60.0f,
                               "%.1f")) {
            sim.params.rainRate = rainMmh / 3.6e6f;
        }
        rangedSlider<&Tuning::waterSimFadeBand>("Sim fade band (m)",
                                                sim.fadeBand);
        rangedSlider<&Tuning::waterSimPinRiverHalfWidth>(
            "Pin rivers >= half-width (m)", sim.pinRiverHalfWidth);
        rangedSlider<&Tuning::waterSimReservoirOutflow>(
            "Reservoir outflow (m3/s/cell)", sim.params.reservoirOutflow);
        ImGui::Combo("Sim mode", &sim.debugMode,
                     "Sim + baked\0Force baked\0Seam overlay\0"
                     "Volumes debug\0");
        // On-site debugging: freeze the live window to disk;
        // `cooker water-replay` reproduces and steps it offline.
        if (ImGui::Button("Dump sim state")) {
            r.waterSystem().requestSimDump(
                (platform::executableDir() / "water-sim-dump.wsd")
                    .string());
        }
    }
    if (ImGui::CollapsingHeader("Post-processing")) {
        ImGui::Checkbox("Filmic tonemap (A/B)", &r.tuning.tonemap);
        drawLanes<kExposure>(r.tuning);
        // A/B: eye adaptation; Exposure becomes the bias.
        ImGui::Checkbox("Auto exposure", &r.tuning.autoExposure);
        if (r.tuning.autoExposure) {
            drawLanes<kAutoExposure>(r.tuning);
        }
        drawLanes<kBloom>(atmos);
        // A/B: the analytical grade, off by default.
        ImGui::Checkbox("Grading", &r.tuning.grading);
        if (r.tuning.grading) {
            drawLanes<kGrade>(r.tuning);
        }
        ImGui::Combo("Debug buffer", &r.tuning.debugBuffer,
                     "Off\0Bloom\0God rays\0Volumetric\0Mist\0");
    }
}

} // namespace game
