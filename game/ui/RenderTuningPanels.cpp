#include "game/ui/RenderTuningPanels.hpp"

#include <cmath>
#include <cstring>

#include "engine/core/FrameProbe.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/AtmosphereParams.hpp"
#include "engine/render/WorldRenderer.hpp"

#include <imgui.h>

namespace game {

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
    // (content-keyed disk cache).
    bool dirty = false;
    const auto knob = [&](const char* label, f32& value, f32 lo, f32 hi) {
        ImGui::SliderFloat(label, &value, lo, hi, "%.3f");
        dirty |= ImGui::IsItemDeactivatedAfterEdit();
    };
    const auto knobInt = [&](const char* label, i32& value, i32 lo,
                             i32 hi) {
        ImGui::SliderInt(label, &value, lo, hi);
        dirty |= ImGui::IsItemDeactivatedAfterEdit();
    };

    if (ImGui::CollapsingHeader("Lobe trees (classic)")) {
        render::LobeTreeParams& p = r.vegetation.lobeTreeParams;
        knob("Trunk height min", p.trunkHeightMin, 1.0f, 12.0f);
        knob("Trunk height max", p.trunkHeightMax, 1.0f, 14.0f);
        knob("Trunk radius min", p.trunkRadiusMin, 0.05f, 0.6f);
        knob("Trunk radius max", p.trunkRadiusMax, 0.05f, 0.8f);
        knob("Trunk taper", p.trunkTaper, 0.1f, 1.0f);
        knob("Lean", p.lean, 0.0f, 0.5f);
        knobInt("Branches min", p.branchCountMin, 1, 6);
        knobInt("Branches max", p.branchCountMax, 1, 6);
        knob("Branch length min", p.branchLengthMin, 0.3f, 3.0f);
        knob("Branch length max", p.branchLengthMax, 0.3f, 4.0f);
        knob("Crown lobe min", p.crownLobeRadiusMin, 0.3f, 2.5f);
        knob("Crown lobe max", p.crownLobeRadiusMax, 0.3f, 3.0f);
        knob("Branch lobe min", p.branchLobeRadiusMin, 0.2f, 2.0f);
        knob("Branch lobe max", p.branchLobeRadiusMax, 0.2f, 2.5f);
        knob("Lobe flatten", p.lobeFlatten, 0.5f, 1.0f);
        knob("Normal spherize", p.normalSpherize, 0.0f, 1.0f);
    }
    if (ImGui::CollapsingHeader("Space colonization",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        render::ColonizedTreeParams& p = r.vegetation.colonizedTreeParams;
        ImGui::SeparatorText("Skeleton (Runions)");
        knob("Growth step D (m)", p.segment, 0.04f, 0.8f);
        knob("Kill distance (m)", p.killDistance, 0.08f, 2.0f);
        knobInt("Attractors", p.attractorCount, 50, 2000);
        knob("Pipe exponent", p.pipeExponent, 2.0f, 3.0f);
        knob("Tropism (up bias)", p.tropism, 0.0f, 0.6f);
        ImGui::SeparatorText("Wood");
        knobInt("Tube sides (max)", p.tubeSides, 3, 12);
        knob("Side taper floor", p.sideMinFraction, 0.25f, 1.0f);
        knob("Curve preserve", p.curvePreserve, 0.0f, 1.0f);
        knobInt("Curve subdivision", p.curveSubdiv, 0, 3);
        knob("Path noise (kinks)", p.pathJitter, 0.0f, 1.0f);
        knob("Ring irregularity", p.ringIrregularity, 0.0f, 1.0f);
        ImGui::SeparatorText("Crown envelope");
        knob("Bare trunk min (m)", p.trunkBaseMin, 0.5f, 5.0f);
        knob("Bare trunk max (m)", p.trunkBaseMax, 0.5f, 6.0f);
        knob("Crown height min", p.crownHeightMin, 1.0f, 7.0f);
        knob("Crown height max", p.crownHeightMax, 1.0f, 8.0f);
        knob("Crown radius min", p.crownRadiusMin, 0.8f, 5.0f);
        knob("Crown radius max", p.crownRadiusMax, 0.8f, 6.0f);
        ImGui::SeparatorText("Conifer habit");
        knob("Crown taper (cone)", p.crownTaper, 0.0f, 1.0f);
        knob("Leader bias", p.leaderBias, 0.0f, 0.8f);
        knob("Lateral flatten", p.lateralFlatten, 0.0f, 1.0f);
        knob("Spray foliage", p.sprayFoliage, 0.0f, 1.0f);
        ImGui::SeparatorText("Leaf style (atlas slot + season)");
        knobInt("Atlas slot", p.leafStyle, 0, 7);
        const char* kShapes =
            "Pointed ellipse\0Needles\0Rounded\0Lobed\0Serrated\0";
        dirty |= ImGui::Combo("Leaf shape", &p.leafShape, kShapes);
        dirty |= ImGui::ColorEdit3("Autumn tint", &p.autumnTint.x,
                                   ImGuiColorEditFlags_Float);
        knob("Seasonality", p.seasonality, 0.0f, 1.0f);
        ImGui::SeparatorText("Foliage SDF + cards");
        knob("Tip ball radius", p.tipBallRadius, 0.05f, 2.0f);
        knob("Tip ball floor", p.tipBallMin, 0.03f, 0.5f);
        knob("Tip order falloff", p.tipOrderFalloff, 0.5f, 1.0f);
        knob("Smooth-min k", p.smoothK, 0.1f, 2.0f);
        knob("Card size min", p.cardHalfSizeMin, 0.01f, 0.25f);
        knob("Card size max", p.cardHalfSizeMax, 0.01f, 0.35f);
        knob("Density gradient G", p.densityGradient, 1.0f, 6.0f);
        knob("Card density x", p.foliageDensity, 0.25f, 8.0f);
        ImGui::SeparatorText("Leaf mask (card texture)");
        knobInt("Leaf count", p.leafCount, 10, 200);
        knob("Leaf size min", p.leafSizeMin, 0.03f, 0.4f);
        knob("Leaf size max", p.leafSizeMax, 0.03f, 0.5f);
        // Live shader window (uLeafLodInfo) — no rebuild, plain sliders.
        ImGui::SliderFloat("Leaf solid start (mip)",
                           &p.leafSolidStart, 0.0f, 8.0f);
        ImGui::SliderFloat("Leaf solid end (mip)",
                           &p.leafSolidEnd, 0.0f, 8.0f);
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

    // The CLAUDE.md §5 round trip, v1: paste-ready records for
    // landscape.toml (the editor's EditSession can take over later —
    // same fields, same GUIDs).
    if (ImGui::Button("Log TOML records")) {
        const render::LobeTreeParams& l = r.vegetation.lobeTreeParams;
        const render::ColonizedTreeParams& c =
            r.vegetation.colonizedTreeParams;
        LOG_INFO("[records.fields]  # LobeTreeTuningForm\n"
                 "trunkHeightMin = {}\ntrunkHeightMax = {}\n"
                 "trunkRadiusMin = {}\ntrunkRadiusMax = {}\n"
                 "trunkTaper = {}\nlean = {}\n"
                 "branchCountMin = {}\nbranchCountMax = {}\n"
                 "branchLengthMin = {}\nbranchLengthMax = {}\n"
                 "crownLobeRadiusMin = {}\ncrownLobeRadiusMax = {}\n"
                 "branchLobeRadiusMin = {}\nbranchLobeRadiusMax = {}\n"
                 "lobeFlatten = {}\nnormalSpherize = {}",
                 l.trunkHeightMin, l.trunkHeightMax, l.trunkRadiusMin,
                 l.trunkRadiusMax, l.trunkTaper, l.lean, l.branchCountMin,
                 l.branchCountMax, l.branchLengthMin, l.branchLengthMax,
                 l.crownLobeRadiusMin, l.crownLobeRadiusMax,
                 l.branchLobeRadiusMin, l.branchLobeRadiusMax,
                 l.lobeFlatten, l.normalSpherize);
        LOG_INFO("[records.fields]  # ColonizedTreeTuningForm\n"
                 "tubeSides = {}\ncurvePreserve = {}\ncurveSubdiv = {}\n"
                 "pathJitter = {}\nringIrregularity = {}\n"
                 "sideMinFraction = {}\n"
                 "segment = {}\nkillDistance = {}\nattractorCount = {}\n"
                 "pipeExponent = {}\ntropism = {}\n"
                 "trunkBaseMin = {}\ntrunkBaseMax = {}\n"
                 "crownHeightMin = {}\ncrownHeightMax = {}\n"
                 "crownRadiusMin = {}\ncrownRadiusMax = {}\n"
                 "crownTaper = {}\nleaderBias = {}\n"
                 "lateralFlatten = {}\nsprayFoliage = {}\n"
                 "tipBallRadius = {}\ntipOrderFalloff = {}\n"
                 "tipBallMin = {}\nleafStyle = {}\nleafShape = {}\n"
                 "autumnTint = [{}, {}, {}]\nseasonality = {}\n"
                 "smoothK = {}\n"
                 "cardHalfSizeMin = {}\ncardHalfSizeMax = {}\n"
                 "densityGradient = {}\nfoliageDensity = {}\n"
                 "leafCount = {}\nleafSizeMin = {}\nleafSizeMax = {}\n"
                 "leafSolidStart = {}\nleafSolidEnd = {}",
                 c.tubeSides, c.curvePreserve, c.curveSubdiv,
                 c.pathJitter, c.ringIrregularity, c.sideMinFraction,
                 c.segment, c.killDistance, c.attractorCount,
                 c.pipeExponent, c.tropism, c.trunkBaseMin, c.trunkBaseMax,
                 c.crownHeightMin, c.crownHeightMax, c.crownRadiusMin,
                 c.crownRadiusMax, c.crownTaper, c.leaderBias,
                 c.lateralFlatten, c.sprayFoliage,
                 c.tipBallRadius, c.tipOrderFalloff, c.tipBallMin,
                 c.leafStyle, c.leafShape, c.autumnTint.x, c.autumnTint.y,
                 c.autumnTint.z, c.seasonality, c.smoothK, c.cardHalfSizeMin, c.cardHalfSizeMax,
                 c.densityGradient, c.foliageDensity, c.leafCount,
                 c.leafSizeMin, c.leafSizeMax, c.leafSolidStart,
                 c.leafSolidEnd);
    }

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
        // re-align it.
        ImGui::SliderFloat("Sea level (m)", &r.terrain.params.seaLevel,
                           0.0f, 60.0f,
                           "%.0f"); // range x1.5 with the amplitudes
        // Streaming ring = the draw distance (64 m chunks; the horizon
        // closure tracks it). Chunk count grows as (2r+1)^2 — watch F6.
        ImGui::SliderInt("View radius (chunks)", &r.terrain.viewRadius, 8,
                         render::TerrainSystem::kMaxViewRadius);
        // Coarse 12 km silhouette mesh past the ring (terrain + forest
        // fringe dissolving into the sky).
        ImGui::Checkbox("Far terrain (silhouettes)", &r.farTerrainUi);
        if (r.terrain.cookedAvailable()) {
            ImGui::Checkbox("Cooked materials (A/B vs procedural)",
                            &r.terrainCookedUi);
        }
    }
    if (ImGui::CollapsingHeader("Vegetation")) {
        ImGui::SliderFloat("Season: autumn", &r.seasonAutumnUi, 0.0f,
                           1.0f, "%.2f");
        ImGui::SliderFloat("Season: leaf fall", &r.seasonLeafFallUi,
                           0.0f, 1.0f, "%.2f");
        // The vegetation draw budget, live
        // (docs/RENDERING.md). Shrinking the ring pops at the edge
        // (the tree fade tops out at 880 m) — a budget-hunting knob.
        ImGui::SliderInt("Veg view radius (chunks)",
                         &r.vegetation.viewRadius, 4, 15);
        ImGui::SliderInt("Veg high-detail radius",
                         &r.vegetation.highDetailRadius, 0, 8);
        // 80-face twins within; 20-face ultra beyond.
        ImGui::SliderInt("Veg low-detail radius",
                         &r.vegetation.lowDetailRadius, 2, 12);
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
        ImGui::Checkbox("Occlusion culling (A/B)", &r.occlusionUi);
        ImGui::SameLine();
        ImGui::Checkbox("GPU Hi-Z", &r.gpuOcclusionUi);
        ImGui::SameLine();
        ImGui::Checkbox("Indirect draw", &r.gpuIndirectUi);
        ImGui::Checkbox("Wireframe (LOD debug)", &r.wireframeUi);
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
    if (ImGui::CollapsingHeader("Grass")) {
        render::GrassRenderTuning& gt = r.grass.renderTuning;
        ImGui::SeparatorText("Blade");
        ImGui::SliderFloat("Height (m)", &gt.bladeHeight, 0.2f, 2.0f,
                           "%.2f");
        ImGui::SliderFloat("Half width (m)", &gt.bladeHalfWidth, 0.01f,
                           0.12f, "%.3f");
        ImGui::ColorEdit3("Base tint (x ground)", &gt.baseTint.x,
                          ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3("Tip tint (x ground)", &gt.tipTint.x,
                          ImGuiColorEditFlags_Float);
        ImGui::SeparatorText("Shading");
        ImGui::SliderFloat("Root AO", &gt.rootAo, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Blade normals", &gt.bladeNormals, 0.0f, 1.0f,
                           "%.2f");
        ImGui::DragFloatRange2("Blade brightness", &gt.brightMin,
                               &gt.brightMax, 0.01f, 0.5f, 2.0f,
                               "min %.2f", "max %.2f");
        ImGui::SliderFloat("Middle darken", &gt.middleDarken, 0.0f, 0.5f,
                           "%.2f");
        ImGui::SliderFloat("Backscatter", &gt.backscatter, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Tip sheen", &gt.sheen, 0.0f, 1.0f, "%.2f");
        ImGui::SeparatorText("Detail / distance");
        ImGui::SliderFloat("Detail near (m)", &gt.detailNear, 2.0f, 60.0f,
                           "%.0f");
        ImGui::SliderFloat("Detail far (m)", &gt.detailFar, 5.0f, 120.0f,
                           "%.0f");
        ImGui::SliderFloat("Thin start (m)", &gt.thinStart, 2.0f, 100.0f,
                           "%.0f");
        ImGui::SliderFloat("Thin end (m)", &gt.thinEnd, 20.0f, 200.0f,
                           "%.0f");
        ImGui::SliderFloat("Far density", &gt.farDensity, 0.05f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Far width comp", &gt.widthCompensation, 0.0f,
                           3.0f, "%.1f");
        ImGui::SliderFloat("Fade start (m)", &gt.fadeStart, 40.0f, 300.0f,
                           "%.0f");
        ImGui::SliderFloat("Fade end (m)", &gt.fadeEnd, 60.0f, 350.0f,
                           "%.0f");
        ImGui::SeparatorText("Scatter (re-bakes on release)");
        render::GrassScatterTuning& st = r.grass.scatterTuning;
        bool scatterEdited = false;
        ImGui::SliderFloat("Blade spacing (m)", &st.spacing, 0.08f, 0.5f,
                           "%.2f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Patch scale (m)", &st.patchBroadScale, 4.0f,
                           60.0f, "%.0f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Clump detail (m)", &st.patchDetailScale, 1.0f,
                           20.0f, "%.0f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::DragFloatRange2("Patch threshold", &st.patchThresholdLo,
                               &st.patchThresholdHi, 0.005f, 0.0f, 1.0f,
                               "lo %.2f", "hi %.2f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::DragFloatRange2("Presence window", &st.presenceLo,
                               &st.presenceHi, 0.005f, 0.0f, 1.0f,
                               "rim %.2f", "solid %.2f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Material cutoff", &st.materialCutoff, 0.0f,
                           1.0f, "%.2f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
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
        ImGui::SliderInt("Resolution (voxels)", &rc.resolution, 32, 96);
        ImGui::SliderFloat("Fine voxel (m)", &rc.fineVoxel, 0.25f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Coarse voxel (m)", &rc.coarseVoxel, 1.0f, 4.0f,
                           "%.1f");
        ImGui::SliderInt("Update every N frames", &rc.updateInterval, 1, 4);
        ImGui::SeparatorText("Cascades");
        ImGui::SliderInt("Cascade count", &rc.cascadeCount, 2, 6);
        ImGui::SliderFloat("Interval 0 (m)", &rc.interval0, 0.25f, 4.0f,
                           "%.2f");
        ImGui::TextDisabled("reach: %.0f m",
                            rc.interval0 *
                                (std::pow(2.0f, static_cast<f32>(
                                                    rc.cascadeCount)) -
                                 1.0f));
        ImGui::SeparatorText("Injection");
        ImGui::SliderFloat("Sky factor", &rc.skyFactor, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Light emitter boost", &rc.emitterBoost, 0.0f,
                           4.0f, "%.2f");
        // Splat re-contract: with clustered direct on every surface, a
        // normal light's splat carries only its BOUNCE share.
        ImGui::SliderFloat("Light splat bounce (clustered)",
                           &rc.lightSplatBounce, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Bounce feedback", &rc.bounceFeedback, 0.0f,
                           0.9f, "%.2f");
        // A/B: chain at end of frame, consumers read frame N-1.
        ImGui::Checkbox("Pipelined GI (frame N-1)", &rc.pipelined);
        // A/B: the chain on the second queue (needs pipelined).
        ImGui::Checkbox("Async compute GI (2nd queue)", &rc.asyncCompute);
        ImGui::Checkbox("Lights via RC only (penumbra experiment)",
                        &rc.rcOnlyLights);
        ImGui::SeparatorText("Apply");
        ImGui::SliderFloat("Intensity", &rc.intensity, 0.0f, 2.0f, "%.2f");
        // RC's lower bound as a fraction of classic ambient — the
        // grid-border seam killer (0 = raw RC darkness).
        ImGui::SliderFloat("Ambient floor (x classic)", &rc.giFloor, 0.0f,
                           1.0f, "%.2f");
        ImGui::SliderFloat("Edge fade (m)", &rc.edgeFade, 1.0f, 16.0f,
                           "%.0f");
        // Fixed log-step ramp: predictable absolute exposure bands.
        ImGui::SliderFloat("Band count (0 = smooth)", &rc.bandCount,
                           0.0f, 8.0f, "%.0f");
        ImGui::SliderFloat("Band AA", &rc.bandAa, 0.02f, 0.45f, "%.2f");
        // x4 reach per marched step on long levels (A/B on rcBuild).
        ImGui::Checkbox("Interval extension (x4 march reach)",
                        &rc.intervalExtension);
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
        ImGui::Checkbox("Stylized lighting (BotW A/B)", &r.stylizedUi);
        if (r.stylizedUi && ImGui::TreeNode("Stylized ramp")) {
            ImGui::TextDisabled("Diffuse: shade -> half-tone -> full light");
            ImGui::SliderFloat("Terminator start", &r.stylizedDiffuseUi.x,
                               -0.2f, 0.5f, "%.3f");
            ImGui::SliderFloat("Terminator end", &r.stylizedDiffuseUi.y,
                               -0.2f, 0.5f, "%.3f");
            ImGui::SliderFloat("Full-light start", &r.stylizedDiffuseUi.z,
                               0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Full-light end", &r.stylizedDiffuseUi.w,
                               0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Half-tone level", &r.stylizedShadowUi.w,
                               0.0f, 1.0f, "%.2f");
            ImGui::TextDisabled("Cast shadows (CSM snap)");
            ImGui::SliderFloat("Snap window start", &r.stylizedShadowUi.x,
                               0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Snap window end", &r.stylizedShadowUi.y,
                               0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Shadow floor", &r.stylizedShadowUi.z, 0.0f,
                               0.8f, "%.2f");
            ImGui::TextDisabled("Specular band (characters/props)");
            ImGui::SliderFloat("Spec strength", &r.stylizedSpecUi.x, 0.0f,
                               1.0f, "%.2f");
            ImGui::SliderFloat("Spec threshold", &r.stylizedSpecUi.y,
                               0.05f, 1.0f, "%.2f");
            ImGui::SliderFloat("Spec exponent", &r.stylizedSpecUi.z, 4.0f,
                               64.0f, "%.0f");
            ImGui::TreePop();
        }
        // A/B: per-cluster light lists vs the legacy 24-light loop
        // (docs/RENDERING.md §5). Needs compute; the checkbox is inert
        // (and the budget stays 24) when the culling pass is absent.
        ImGui::Checkbox("Clustered lights (64-light budget)",
                        &r.clusteredLightsUi);
        ImGui::Checkbox("Shadows", &r.shadowsUi);
        ImGui::SameLine();
        ImGui::Checkbox("Cascade debug tint", &r.cascadeDebugUi);
        // A/B: far cascades on alternate frames —
        // off = every cascade every frame.
        ImGui::Checkbox("CSM round-robin (far cascades 1/2 rate)",
                        &r.shadowRoundRobinUi);
        // Sharpness: texels per cascade side (4096 = 2x definition
        // everywhere, ~150 MB more; the far cascade profits most).
        int shadowRes = r.shadowResolutionUi >= 4096 ? 2
                        : r.shadowResolutionUi >= 2048 ? 1 : 0;
        if (ImGui::Combo("Shadow map resolution", &shadowRes,
                         "1024\0002048\0004096\000")) {
            r.shadowResolutionUi = shadowRes == 2 ? 4096
                                   : shadowRes == 1 ? 2048 : 1024;
        }
        // A/B: houses/crates/NPCs casting into the sun cascades.
        ImGui::Checkbox("Mesh shadow casters", &r.meshShadowCastersUi);
        ImGui::Checkbox("Contact shadows", &r.contactShadowsUi);
        ImGui::Checkbox("SSAO", &r.ssaoUi);
        if (r.ssaoUi) {
            ImGui::SliderFloat("SSAO strength", &r.ssaoStrengthUi, 0.0f,
                               2.0f);
            ImGui::SliderFloat("SSAO radius (m)", &r.ssaoRadiusUi, 0.2f,
                               2.0f);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Terrain light map", &r.terrainLightUi);
        ImGui::Checkbox("Key light shadow", &r.keyShadowUi); // interiors
        // The interior ambient follows the outside (hour + weather).
        ImGui::SliderFloat("Interior daylight coupling",
                           &r.interiorDaylightWeightUi, 0.0f, 1.0f, "%.2f");
    }
    if (ImGui::CollapsingHeader("Sun FX")) {
        ImGui::SliderFloat("God rays intensity", &atmos.godRayIntensity,
                           0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Volumetric shafts", &atmos.volumetric, 0.0f,
                           3.0f, "%.2f");
        ImGui::Checkbox("Froxel fog (V4/H4 — off = 2D march)",
                        &r.postFx.froxelFog);
        ImGui::SliderFloat("Froxel temporal blend",
                           &r.postFx.froxelTemporalBlend, 0.02f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Dust wisps (sparse)", &r.postFx.froxelDustNoise,
                           0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Interior dust", &r.interiorDustDensityUi, 0.0f,
                           0.12f, "%.3f");
    }
    if (ImGui::CollapsingHeader("Fog & clouds")) {
        ImGui::SliderFloat("Fog density", &atmos.fogDensity, 0.0f, 0.004f,
                           "%.4f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Fog height falloff", &atmos.fogHeightFalloff,
                           0.001f, 0.08f, "%.3f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Fog low-altitude boost", &atmos.fogLowBoost,
                           0.0f, 5.0f, "%.1f");
        ImGui::SliderFloat("Fog start (m)", &atmos.fogStart, 0.0f, 500.0f,
                           "%.0f");
        ImGui::SliderFloat("Fog sun scatter", &atmos.fogSunScatter, 0.0f,
                           2.0f, "%.2f");
        ImGui::SliderFloat("Fog sun phase exp", &atmos.fogSunPhase, 1.0f,
                           32.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        // How fast the fog layer thins with altitude: high = clear sky
        // above the fog band, low = grey dome.
        ImGui::SliderFloat("Fog ceiling falloff", &atmos.fogCeiling,
                           0.0005f, 0.03f, "%.4f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Cloud coverage", &atmos.cloudCoverage, 0.0f,
                           1.0f, "%.2f");
        ImGui::SliderFloat("Cloud shadow strength", &atmos.cloudShadow,
                           0.0f, 1.0f, "%.2f");
        ImGui::SeparatorText("Sky clouds (volumetric)");
        // A/B vs the 2D dome layer; coverage/height/scale above drive
        // BOTH implementations (and the ground shadows).
        ImGui::Checkbox("Volumetric clouds (A/B)", &r.skyCloudsUi);
        ImGui::SliderFloat("Cloud thickness (m)", &r.skyCloudShapeUi.x,
                           80.0f, 800.0f, "%.0f");
        ImGui::SliderFloat("Cloud density", &r.skyCloudShapeUi.y, 0.002f,
                           0.12f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Cloud erosion", &r.skyCloudShapeUi.z, 0.0f,
                           0.95f, "%.2f");
        // Thickness follows the weather's coverage: full skies tower
        // (at 4, a full sky multiplies the thickness by 5).
        ImGui::SliderFloat("Cloud thickness<->coverage",
                           &r.skyCloudShapeUi.w, 0.0f, 4.0f, "%.2f");
        // Body = multi-octave scattering (luminous cores); lining = the
        // direct transmission exp(-tau)*HG — the silver lining. The
        // drama lives in lining gain + lining lobe + a LOW ambient.
        ImGui::SliderFloat("Cloud body gain", &r.skyCloudLightUi.x, 0.0f,
                           40.0f, "%.1f");
        ImGui::SliderFloat("Cloud body lobe g", &r.skyCloudLightUi.y, 0.0f,
                           0.95f, "%.2f");
        ImGui::SliderFloat("Cloud lining gain", &r.skyCloudLightUi.w, 0.0f,
                           60.0f, "%.1f");
        ImGui::SliderFloat("Cloud lining lobe g", &r.skyCloudLiningLobeUi,
                           0.5f, 0.97f, "%.2f");
        ImGui::SliderFloat("Cloud powder", &r.skyCloudPowderUi, 0.0f, 1.5f,
                           "%.2f");
        // Fractal edge erosion — the cauliflower florets.
        ImGui::SliderFloat("Cloud puffiness", &r.skyCloudPuffinessUi, 0.0f,
                           1.0f, "%.2f");
        // Silhouette glow where the cloud is thin along the VIEW ray.
        ImGui::SliderFloat("Cloud rim gain", &r.skyCloudRimGainUi, 0.0f,
                           30.0f, "%.1f");
        ImGui::SliderFloat("Cloud rim lobe g", &r.skyCloudRimLobeUi, 0.5f,
                           0.97f, "%.2f");
        // Storm dimming: the whole cloud's ambient falls with coverage
        // (like the ground's), bases hardest. Sun terms stay — dark
        // slabs rimmed with fire.
        ImGui::SliderFloat("Cloud storm darkening", &r.skyCloudBaseDarkUi,
                           0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Cloud ambient gain", &r.skyCloudLightUi.z,
                           0.0f, 2.0f, "%.2f");
        ImGui::SeparatorText("Ground mist");
        ImGui::Checkbox("Mist (A/B)", &r.mistUi);
        ImGui::SliderFloat("Mist density", &atmos.mistDensity, 0.0f, 2.0f,
                           "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Mist coverage", &atmos.mistCoverage, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Mist cover softness", &r.mistCoverageSoftnessUi,
                           0.02f, 1.0f, "%.2f");
        ImGui::SliderFloat("Mist reach (m)", &r.mistReachUi, 200.0f,
                           4000.0f, "%.0f");
        ImGui::SliderFloat("Mist lift (m)", &r.mistShapeUi.w, 0.0f, 60.0f,
                           "%.1f");
        ImGui::SliderFloat("Mist cover scale", &r.mistShapeUi.x, 0.0005f,
                           0.04f, "%.4f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Mist erosion scale", &r.mistShapeUi.y, 0.01f,
                           1.6f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Mist erosion strength", &r.mistShapeUi.z, 0.0f,
                           0.95f, "%.2f");
        ImGui::SeparatorText("Mist lighting");
        ImGui::SliderFloat("Mist sun boost", &r.mistSunBoostUi, 0.0f, 16.0f,
                           "%.1f");
        // Higher g = tighter, brighter halo hugging the sun direction.
        ImGui::SliderFloat("Mist sun lobe g", &r.mistLightUi.x, 0.0f,
                           0.95f, "%.2f");
        ImGui::SliderFloat("Mist backscatter", &r.mistLightUi.y, 0.0f,
                           1.0f, "%.2f");
        // Lower ambient makes the sun beam pop (the silver-lining
        // contrast is ambient-vs-sun, not sun alone).
        ImGui::SliderFloat("Mist ambient gain", &r.mistLightUi.z, 0.0f,
                           2.0f, "%.2f");
        ImGui::SliderFloat("Mist shadow floor", &r.mistLightUi.w, 0.0f,
                           1.0f, "%.2f");
        // A/B: baked Perlin-Worley volume vs analytic in-shader fbm3.
        ImGui::Checkbox("Mist noise texture (A/B)", &r.mistNoiseTexUi);
        ImGui::SliderFloat("Mist detail dropout (m)",
                           &r.mistDetailDropoutUi, 100.0f, 1200.0f, "%.0f");
        // Fractal edge florets on the patch borders (cloud recipe).
        ImGui::SliderFloat("Mist puffiness", &r.mistPuffinessUi, 0.0f,
                           1.0f, "%.2f");
        ImGui::SliderInt("Mist steps", &r.mistStepsUi, 8, 32);
        // 1 = accumulation off (A/B); lower = more history.
        ImGui::SliderFloat("Mist temporal blend",
                           &r.postFx.mistTemporalBlend, 0.05f, 1.0f,
                           "%.2f");
    }
    if (ImGui::CollapsingHeader("Water")) {
        ImGui::Checkbox("Reflections", &r.reflectionsUi);
        // Skip the mirror render when no resident water is in
        // view (A/B — the horizon-sea edge case), and trade its resolution.
        ImGui::SameLine();
        ImGui::Checkbox("auto-skip", &r.reflectionAutoSkipUi);
        ImGui::SliderFloat("Reflection scale", &r.reflectionScaleUi, 0.25f,
                           0.5f, "%.2f");
        ImGui::Combo("Debug view", &r.waterDebugUi,
                     "Off\0Flow\0Torrent\0River UV\0Info: surface\0"
                     "Info: depth\0Info: flow\0");
    }
    if (ImGui::CollapsingHeader("Post-processing")) {
        ImGui::Checkbox("Filmic tonemap (A/B)", &r.tonemapUi);
        ImGui::SliderFloat("Exposure", &r.exposureUi, 0.25f, 3.0f, "%.2f");
        // A/B: eye adaptation; Exposure becomes the bias.
        ImGui::Checkbox("Auto exposure", &r.autoExposureUi);
        if (r.autoExposureUi) {
            ImGui::SliderFloat("Auto-expo min", &r.autoExposureMinUi, 0.1f,
                               1.0f, "%.2f");
            ImGui::SliderFloat("Auto-expo max", &r.autoExposureMaxUi, 1.0f,
                               6.0f, "%.2f");
        }
        ImGui::SliderFloat("Bloom intensity", &atmos.bloomIntensity, 0.0f,
                           1.5f, "%.2f");
        // A/B: the analytical grade, off by default.
        ImGui::Checkbox("Grading", &r.gradingUi);
        if (r.gradingUi) {
            ImGui::SliderFloat("Vibrance", &r.gradeVibranceUi, 0.0f, 1.0f,
                               "%.2f");
            ImGui::SliderFloat("Split tone", &r.gradeSplitToneUi, 0.0f,
                               1.0f, "%.2f");
            ImGui::SliderFloat("Contrast", &r.gradeContrastUi, 0.8f, 1.4f,
                               "%.2f");
        }
        ImGui::Combo("Debug buffer", &r.debugBufferUi,
                     "Off\0Bloom\0God rays\0Volumetric\0Mist\0");
    }
}

} // namespace game
