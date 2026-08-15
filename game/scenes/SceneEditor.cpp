#include "game/scenes/SceneEditor.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/VisualForms.hpp" // StaticForm, LightForm
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/FlyCamera.hpp"
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height, TerrainParams
#include "game/LevelEditor.hpp"
#include "engine/render/MeshCache.hpp"
#include "world/scene/Components.hpp"        // Transform, MeshRender, RefId
#include "world/scene/Spawner.hpp"           // Spawner, SpawnContext
#include "world/streaming/CellLoader.hpp"    // CellLoader
#include "world/streaming/CellStreamer.hpp"  // CellStreamer (adopt)
#include "world/worldspace/FormCategory.hpp" // FormCategoryRegistry
#include "world/worldspace/WorldForms.hpp"   // WorldspaceForm, ReferenceForm, PrefabForm
#include "world/worldspace/WorldModel.hpp"   // WorldModel

namespace game {

void SceneEditor::deselect() {
    editSelection = ecs::Entity {};
    placementBase = core::Guid {};
}

Vec3 SceneEditor::mouseRayDirection(const EditorContext& ctx,
                                    const Vec2& mousePx) const {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const f32 aspect = display.y > 0.0f ? display.x / display.y : 1.0f;
    const Vec2 ndc { 2.0f * mousePx.x / display.x - 1.0f,
                     1.0f - 2.0f * mousePx.y / display.y };
    const Mat4 inv = glm::inverse(ctx.camera.camera.viewProj(aspect));
    // Reversed-Z, 0..1 clip: near sits at ndc z = 1, far at 0.
    Vec4 nearPoint = inv * Vec4 { ndc.x, ndc.y, 1.0f, 1.0f };
    Vec4 farPoint = inv * Vec4 { ndc.x, ndc.y, 0.0f, 1.0f };
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    return glm::normalize(Vec3 { farPoint } - Vec3 { nearPoint });
}

bool SceneEditor::pickEntity(const EditorContext& ctx, const Vec2& mousePx,
                             ecs::Entity& out) const {
    const Vec3 origin = ctx.camera.camera.position;
    const Vec3 dir = mouseRayDirection(ctx, mousePx);
    f32 bestT = 1e9f;
    ecs::Entity best {};
    ctx.world.handle()
        .query<const world::Transform, const world::MeshRender,
               const world::RefId>()
        .each([&](flecs::entity e, const world::Transform& transform,
                  const world::MeshRender& mesh, const world::RefId&) {
            Vec3 lo { -0.5f }, hi { 0.5f };
            if (const render::MeshCache::CpuMesh* cpu =
                    ctx.meshCache.cpuMesh(mesh.model)) {
                lo = cpu->boundsMin;
                hi = cpu->boundsMax;
            }
            // World AABB of the transformed local box (8 corners).
            const Mat4 model =
                glm::translate(Mat4 { 1.0f }, transform.position) *
                glm::mat4_cast(transform.rotation) *
                glm::scale(Mat4 { 1.0f }, transform.scale);
            Vec3 wlo { 1e9f }, whi { -1e9f };
            for (u32 i = 0; i < 8; ++i) {
                const Vec3 corner { (i & 1) ? hi.x : lo.x,
                                    (i & 2) ? hi.y : lo.y,
                                    (i & 4) ? hi.z : lo.z };
                const Vec3 world3 = Vec3 { model * Vec4 { corner, 1.0f } };
                wlo = glm::min(wlo, world3);
                whi = glm::max(whi, world3);
            }
            // Slab test.
            f32 t0 = 0.0f, t1 = 1e9f;
            for (u32 axis = 0; axis < 3; ++axis) {
                const f32 d = dir[static_cast<i32>(axis)];
                const f32 o = origin[static_cast<i32>(axis)];
                if (std::abs(d) < 1e-6f) {
                    if (o < wlo[static_cast<i32>(axis)] ||
                        o > whi[static_cast<i32>(axis)]) {
                        return;
                    }
                    continue;
                }
                f32 tNear = (wlo[static_cast<i32>(axis)] - o) / d;
                f32 tFar = (whi[static_cast<i32>(axis)] - o) / d;
                if (tNear > tFar) {
                    std::swap(tNear, tFar);
                }
                t0 = glm::max(t0, tNear);
                t1 = glm::min(t1, tFar);
                if (t0 > t1) {
                    return;
                }
            }
            if (t0 < bestT) {
                bestT = t0;
                best = ecs::Entity { e };
            }
        });
    out = best;
    return best.is_alive();
}

bool SceneEditor::groundUnderMouse(const EditorContext& ctx,
                                   const Vec2& mousePx, Vec3& out) const {
    const Vec3 origin = ctx.camera.camera.position;
    const Vec3 dir = mouseRayDirection(ctx, mousePx);
    if (ctx.interiorMode) { // interiors: intersect the y = 0 floor plane
        if (std::abs(dir.y) < 1e-4f) {
            return false;
        }
        const f32 t = -origin.y / dir.y;
        if (t <= 0.0f || t > 200.0f) {
            return false;
        }
        out = origin + dir * t;
        return true;
    }
    // Raymarch the height function: coarse steps, then a refinement.
    f32 t = 0.0f;
    f32 previous = t;
    for (u32 i = 0; i < 400; ++i) {
        t += 1.5f;
        const Vec3 p = origin + dir * t;
        if (p.y <= render::terrain::height(ctx.terrainParams, p.x, p.z)) {
            for (u32 r = 0; r < 12; ++r) { // bisect
                const f32 mid = (previous + t) * 0.5f;
                const Vec3 m = origin + dir * mid;
                if (m.y <=
                    render::terrain::height(ctx.terrainParams, m.x, m.z)) {
                    t = mid;
                } else {
                    previous = mid;
                }
            }
            const Vec3 hit = origin + dir * t;
            out = { hit.x,
                    render::terrain::height(ctx.terrainParams, hit.x, hit.z),
                    hit.z };
            return true;
        }
        previous = t;
    }
    return false;
}

void SceneEditor::draw(const EditorContext& ctx) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    const f32 aspect = display.y > 0.0f ? display.x / display.y : 1.0f;
    ImGuizmo::BeginFrame();
    ImGuizmo::SetRect(0.0f, 0.0f, display.x, display.y);

    // Gizmo op hotkeys (1/2/3), like every DCC.
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
        gizmoOperation = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) {
        gizmoOperation = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
        gizmoOperation = 2;
    }

    // Gizmo on the live selection; commit to the RECORD on release.
    if (editSelection.is_alive()) {
        auto& transform = editSelection.get_mut<world::Transform>();
        Mat4 model = glm::translate(Mat4 { 1.0f }, transform.position) *
                     glm::mat4_cast(transform.rotation) *
                     glm::scale(Mat4 { 1.0f }, transform.scale);
        const Mat4 view = ctx.camera.camera.view();
        const Mat4 proj = ctx.camera.camera.proj(aspect);
        const ImGuizmo::OPERATION op =
            gizmoOperation == 0   ? ImGuizmo::TRANSLATE
            : gizmoOperation == 1 ? ImGuizmo::ROTATE
                                  : ImGuizmo::SCALE;
        // Grid snap: ImGuizmo's own snapping — meters on translate, a
        // 15° lattice on rotate (scale stays free).
        const f32 translateSnap[3] = { snapStep, snapStep, snapStep };
        const f32 rotateSnap[3] = { 15.0f, 15.0f, 15.0f };
        const f32* snap = nullptr;
        if (snapEnabled && op == ImGuizmo::TRANSLATE) {
            snap = translateSnap;
        } else if (snapEnabled && op == ImGuizmo::ROTATE) {
            snap = rotateSnap;
        }
        // Not while mouselooking: Alt+LMB arms the camera
        // and ImGuizmo only ever reacts to LMB, so without this guard a
        // Cmd-drag over the gizmo would turn the view AND drag the object.
        // Short-circuit, so IsUsing() falls to false and the release branch
        // below still commits a stroke that was in flight.
        if (!ctx.camera.capturing() &&
            ImGuizmo::Manipulate(&view[0][0], &proj[0][0], op,
                                 ImGuizmo::WORLD, &model[0][0], nullptr,
                                 snap)) {
            // Manual decompose (translation / per-column scale / rotation).
            transform.position = Vec3 { model[3] };
            Vec3 scale { glm::length(Vec3 { model[0] }),
                         glm::length(Vec3 { model[1] }),
                         glm::length(Vec3 { model[2] }) };
            scale = glm::max(scale, Vec3 { 1e-4f });
            transform.scale = scale;
            transform.rotation = glm::normalize(glm::quat_cast(
                Mat3 { Vec3 { model[0] } / scale.x,
                       Vec3 { model[1] } / scale.y,
                       Vec3 { model[2] } / scale.z }));
        }
        const bool usingNow = ImGuizmo::IsUsing();
        if (gizmoWasUsing && !usingNow) {
            ctx.levelEditor.commitTransform(
                editSelection.get<world::RefId>().referenceId,
                transform.position, transform.rotation, transform.scale);
        }
        gizmoWasUsing = usingNow;
    } else {
        gizmoWasUsing = false;
    }

    // Sculpt strokes take priority over pick/place while armed. The tool owns
    // the brush + stroke state; the scene supplies the ground hit under the
    // cursor and the publish effects (ctx.sculpt).
    if (sculptTool.active() && !io.WantCaptureMouse && !ctx.camera.capturing()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            Vec3 ground;
            if (groundUnderMouse(ctx, { io.MousePos.x, io.MousePos.y },
                                 ground)) {
                sculptTool.stroke(ctx.sculpt, ground, io.DeltaTime);
            }
        } else {
            sculptTool.endStroke(ctx.sculpt); // publishes once per stroke
        }
    }

    // Click: place (armed palette entry) or pick. Never while the mouse is
    // over a window/gizmo, while mouselooking, or while sculpting.
    if (!sculptTool.active() && !io.WantCaptureMouse && !ImGuizmo::IsOver() &&
        !ctx.camera.capturing() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const Vec2 mouse { io.MousePos.x, io.MousePos.y };
        if (placementBase.isValid()) {
            Vec3 ground;
            if (groundUnderMouse(ctx, mouse, ground)) {
                // The cell under the hit — implicit cells
                // (docs/IMPLICIT-CELLS.md): no more "authored cell or abort". A virgin
                // square is materialized live + recorded in the session
                // (ensureCell), then adopted by the streamer so it is
                // loaded exactly once. One gesture: Ctrl+Z removes the
                // cell record AND the reference together.
                data::EditSession::Gesture gesture {
                    ctx.levelEditor.editSession()
                };
                core::Guid cellGuid {};
                const reflect::TypeInfo* wsType =
                    ctx.forms.typeOf(ctx.activeWorldspace);
                const auto* space =
                    wsType &&
                            wsType->isA(world::WorldspaceForm::
                                            staticTypeInfo()
                                                .id)
                        ? static_cast<const world::WorldspaceForm*>(
                              ctx.forms.get(ctx.activeWorldspace))
                        : nullptr;
                if (space) {
                    const i32 gx = static_cast<i32>(
                        std::floor(ground.x / space->cellSize));
                    const i32 gy = static_cast<i32>(
                        std::floor(ground.z / space->cellSize));
                    cellGuid = ctx.levelEditor.ensureCell(
                        ctx.worldModel, ctx.forms, ctx.activeWorldspace,
                        gx, gy);
                }
                if (!cellGuid.isValid()) {
                    LOG_WARN("Editor: no worldspace under the hit — "
                             "placement aborted");
                } else {
                    // Load the (possibly fresh) cell before spawning into it.
                    ctx.streamer.adopt(ctx.forms.handleOf(cellGuid));
                    // Authored y follows the base's convention: snapping
                    // bases store an offset (0 = on the ground), pad-based
                    // ones (snapToGround = false) store the ABSOLUTE hit.
                    f32 storedY = 0.0f;
                    if (const data::Form* baseF = ctx.forms.find(placementBase)) {
                        const reflect::TypeInfo* baseT =
                            ctx.forms.typeOf(ctx.forms.handleOf(placementBase));
                        if (const reflect::FieldInfo* field =
                                baseT ? baseT->findField("snapToGround")
                                      : nullptr;
                            field &&
                            field->kind == reflect::FieldKind::Bool &&
                            !std::get<bool>(field->get(baseF))) {
                            storedY = ground.y;
                        }
                    }
                    const core::Guid placed = ctx.levelEditor.placeReference(
                        placementBase, cellGuid,
                        { ground.x, storedY, ground.z });
                    // Live spawn from the draft, into the loaded cell.
                    if (const auto* draft =
                            static_cast<const world::ReferenceForm*>(
                                ctx.levelEditor.editSession().view(placed))) {
                        world::SpawnContext spawnCtx { ctx.world, ctx.forms,
                                                       ctx.categories };
                        world::ReferenceForm live = *draft;
                        live.position = ground; // grounded live position
                        const ecs::Entity cellEntity =
                            ctx.cellLoader.cellEntity(
                                ctx.forms.handleOf(cellGuid));
                        const ecs::Entity entity =
                            ctx.spawner.spawn(spawnCtx, live, cellEntity);
                        if (entity.is_alive()) {
                            editSelection = entity;
                            ctx.levelEditor.select(placed);
                        }
                    }
                }
            }
        } else {
            ecs::Entity picked {};
            if (pickEntity(ctx, mouse, picked)) {
                editSelection = picked;
                ctx.levelEditor.select(picked.get<world::RefId>().referenceId);
                if (io.KeyCtrl) { // Ctrl+click: grow the prefab group
                    ctx.levelEditor.groupSelection().push_back(
                        picked.get<world::RefId>().referenceId);
                }
            } else {
                editSelection = ecs::Entity {};
                ctx.levelEditor.select(core::Guid {});
            }
        }
    }

    // The editor window.
    ImGui::Begin("Level editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted(
        "LMB: pick / place / sculpt | Ctrl+LMB: add to group | 1/2/3: gizmo "
        "op\nHold RMB (or Alt+LMB): look + WASD: fly | F3: back to Play");
    ImGui::Text("Session: %u dirty record(s)",
                ctx.levelEditor.editSession().dirtyCount());
    if (ImGui::Button("Undo") && ctx.levelEditor.editSession().canUndo()) {
        ctx.levelEditor.editSession().undo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Redo") && ctx.levelEditor.editSession().canRedo()) {
        ctx.levelEditor.editSession().redo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Export mod (data/mods/level-edits.toml)")) {
        ctx.levelEditor.exportTo(platform::executableDir() / "data" / "mods" /
                                     "level-edits.toml",
                                 *core::Guid::fromString(
                                     "aaaaaaaa-0000-4000-8000-0000000000ed"),
                                 "level-edits");
    }
    ImGui::Checkbox("Snap", &snapEnabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("##snapstep", &snapStep, 0.25f, 0.25f, 8.0f, "%.2f m");
    if (editSelection.is_alive()) {
        const auto& ref = editSelection.get<world::RefId>();
        ImGui::Separator();
        ImGui::Text("Selected: %s", ref.referenceId.toString().c_str());
        if (ImGui::Button("Disable (delete)")) {
            ctx.levelEditor.disableReference(ref.referenceId);
            editSelection.destruct();
            editSelection = ecs::Entity {};
        }
        ImGui::SameLine();
        // Duplicate: record copy through the
        // session (one undo gesture) + a live spawn beside the original.
        if (ImGui::Button("Duplicate") ||
            (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))) {
            const Vec3 offset { 1.0f, 0.0f, 1.0f };
            const core::Guid copy = ctx.levelEditor.duplicateReference(
                ref.referenceId, offset);
            const auto* draft = static_cast<const world::ReferenceForm*>(
                ctx.levelEditor.editSession().view(copy));
            if (draft) {
                world::ReferenceForm live = *draft;
                // Live position = the live original's spot + the offset
                // (the record may store a snapToGround offset, not the
                // grounded y).
                live.position =
                    editSelection.get<world::Transform>().position + offset;
                world::SpawnContext spawnCtx { ctx.world, ctx.forms,
                                               ctx.categories };
                const ecs::Entity entity = ctx.spawner.spawn(
                    spawnCtx, live,
                    ctx.cellLoader.cellEntity(
                        ctx.forms.handleOf(live.cell)));
                if (entity.is_alive()) {
                    editSelection = entity;
                }
            }
        }
    }
    ImGui::Separator();
    ImGui::Text("Prefab group: %u",
                static_cast<u32>(ctx.levelEditor.groupSelection().size()));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        ctx.levelEditor.groupSelection().clear();
    }
    static char prefabName[64] = "MyPrefab";
    ImGui::InputText("##prefabname", prefabName, sizeof(prefabName));
    ImGui::SameLine();
    if (ImGui::Button("Create prefab from group")) {
        const core::Guid instance = ctx.levelEditor.createPrefabFromSelection(
            ctx.levelEditor.groupSelection(), prefabName);
        if (instance.isValid()) {
            // Remove the originals' live entities; spawn the instance.
            for (const core::Guid& id : ctx.levelEditor.groupSelection()) {
                ctx.world.handle()
                    .query<const world::RefId>()
                    .each([&](flecs::entity e, const world::RefId& rid) {
                        if (rid.referenceId == id) {
                            ecs::Entity { e }.destruct();
                        }
                    });
            }
            ctx.levelEditor.groupSelection().clear();
            if (const auto* draft =
                    static_cast<const world::ReferenceForm*>(
                        ctx.levelEditor.editSession().view(instance))) {
                world::SpawnContext spawnCtx { ctx.world, ctx.forms,
                                               ctx.categories };
                ctx.spawner.spawn(spawnCtx, *draft, ecs::Entity {});
            }
        }
    }
    ImGui::Separator();
    // Terrain sculpt (TerrainSculptTool).
    sculptTool.drawPanel(ctx.sculpt);
    // Terrain generation (TerrainGenTool).
    genTool.drawPanel(ctx.gen);
    // Dungeon generation (DungeonGenTool).
    dungeonGenTool.drawPanel(ctx.dungeonGen);
    ImGui::Separator();
    ImGui::TextUnformatted(placementBase.isValid()
                               ? "Placing: click the ground (Esc: cancel)"
                               : "Palette — click to arm:");
    if (placementBase.isValid() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        placementBase = core::Guid {};
    }
    // Palette: every placeable base form, by category.
    const auto paletteEntry = [&](const data::Form& form) {
        const bool armed = placementBase == form.id;
        if (ImGui::Selectable(
                (form.editorId + (armed ? "  [armed]" : "")).c_str(), armed)) {
            placementBase = armed ? core::Guid {} : form.id;
        }
    };
    if (ImGui::CollapsingHeader("Statics", ImGuiTreeNodeFlags_DefaultOpen)) {
        data::forEach<data::StaticForm>(
            ctx.forms, [&](const data::StaticForm& form) { paletteEntry(form); });
    }
    if (ImGui::CollapsingHeader("Lights")) {
        data::forEach<data::LightForm>(
            ctx.forms, [&](const data::LightForm& form) { paletteEntry(form); });
    }
    if (ImGui::CollapsingHeader("Prefabs")) {
        data::forEach<world::PrefabForm>(
            ctx.forms,
            [&](const world::PrefabForm& form) { paletteEntry(form); });
    }
    ImGui::End();
}

} // namespace game
