#include "game/scenes/MapController.hpp"

#include <algorithm>
#include <cstdio>

#include "data/forms/FormQuery.hpp"
#include "engine/core/Clock.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/ui/UiSystem.hpp"
#include "game/ScreenStack.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

namespace {

// [cpp-tuning] Raster resolution (square). The world extent is padded to
// a square too, so one texel = the same meters on both axes and markers
// keep their aspect.
constexpr u32 kMapRasterSize = 512;

str pct(f32 value01) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.2f", value01 * 100.0f);
    return text;
}

} // namespace

const world::WorldspaceForm* MapController::exteriorWorldspace(
    const MapContext& ctx) const {
    // The map always shows the exterior overworld: indoors, fall back to
    // it (the raster stays useful, the marker hides).
    const data::FormHandle handle =
        ctx.interior ? ctx.overworld : ctx.activeWorldspace;
    const data::Form* form = ctx.forms.get(handle);
    if (!form) {
        return nullptr;
    }
    return ctx.forms.find<world::WorldspaceForm>(form->id);
}

void MapController::open(const MapContext& ctx) {
    const world::WorldspaceForm* space = exteriorWorldspace(ctx);
    if (!space) {
        LOG_WARN("C9.6: no exterior worldspace to map");
        return;
    }

    // Extent = bbox of the worldspace's authored cells (the world has no
    // stored bounds) + one cell of margin, padded to a square.
    i32 minGX = 0, maxGX = 0, minGY = 0, maxGY = 0;
    bool any = false;
    data::forEach<world::CellForm>(
        ctx.forms, [&](const world::CellForm& cell) {
            if (cell.worldspace != space->id) {
                return;
            }
            if (!any) {
                minGX = maxGX = cell.gridX;
                minGY = maxGY = cell.gridY;
                any = true;
                return;
            }
            minGX = std::min(minGX, cell.gridX);
            maxGX = std::max(maxGX, cell.gridX);
            minGY = std::min(minGY, cell.gridY);
            maxGY = std::max(maxGY, cell.gridY);
        });
    if (!any) {
        LOG_WARN("C9.6: worldspace '{}' has no cells — no map",
                 space->editorId);
        return;
    }
    const f32 cellSize = space->cellSize;
    // Cell (gx, gy) spans [gx*cs, (gx+1)*cs) (CellStreamer convention);
    // one margin cell on every side.
    f32 minX = static_cast<f32>(minGX - 1) * cellSize;
    f32 maxX = static_cast<f32>(maxGX + 2) * cellSize;
    f32 minZ = static_cast<f32>(minGY - 1) * cellSize;
    f32 maxZ = static_cast<f32>(maxGY + 2) * cellSize;
    const f32 spanX = maxX - minX;
    const f32 spanZ = maxZ - minZ;
    if (spanX > spanZ) {
        minZ -= (spanX - spanZ) * 0.5f;
        maxZ += (spanX - spanZ) * 0.5f;
    } else if (spanZ > spanX) {
        minX -= (spanZ - spanX) * 0.5f;
        maxX += (spanZ - spanX) * 0.5f;
    }

    // The extent alone places every marker — valid right now, while the
    // pixels may still be baking. terrain stays null in desc_: mapUv
    // never dereferences it, and the worker uses its OWN params copy.
    desc_ = { .terrain = nullptr,
              .minX = minX,
              .minZ = minZ,
              .maxX = maxX,
              .maxZ = maxZ,
              .size = kMapRasterSize };
    extentValid_ = true;

    // Kick the raster to a worker when this exterior isn't the one on
    // screen (first open, worldspace changed) and it isn't already
    // baking. updateOpen() publishes it once done.
    const bool baking = pending_ && pending_->worldspace == space->id;
    if ((!hasRaster_ || rasterWorldspace_ != space->id) && !baking) {
        auto job = std::make_shared<AsyncRaster>();
        job->params = ctx.terrain; // deep enough: patches ride an sptr
        job->desc = desc_;
        job->desc.terrain = &job->params;
        job->worldspace = space->id;
        pending_ = job;
        const str spaceName = space->editorId;
        ctx.jobs.enqueue([job, spaceName] {
            const core::TimePoint start = core::clockNow();
            job->pixels = generateMapRaster(job->desc);
            LOG_INFO("C9.6: map raster {}x{} of '{}' ({} x {} m) "
                     "generated in {:.1f} ms",
                     job->desc.size, job->desc.size, spaceName,
                     job->desc.maxX - job->desc.minX,
                     job->desc.maxZ - job->desc.minZ,
                     core::secondsSince(start) * 1000.0);
            job->done.store(true, std::memory_order_release);
        });
    }

    // Door POIs: authored placed references whose base Form is a door and
    // whose cell lives in the mapped worldspace — straight from the
    // resolved records (positions are authored; live entities only exist
    // for RESIDENT cells).
    vector<::ui::UiRow> rows;
    data::forEach<world::ReferenceForm>(
        ctx.forms, [&](const world::ReferenceForm& ref) {
            if (!ref.enabled || ref.prefab.isValid()) {
                return; // disabled, or a prefab TEMPLATE child
            }
            const auto* door =
                ctx.forms.find<world::DoorForm>(ref.baseForm);
            if (!door) {
                return;
            }
            const auto* cell = ctx.forms.find<world::CellForm>(ref.cell);
            if (!cell || cell->worldspace != space->id) {
                return;
            }
            const Vec2 uv = mapUv(desc_, ref.position.x, ref.position.z);
            ::ui::UiRow row;
            row.id = ref.editorId;
            row.c0 = door->displayName;
            row.c1 = pct(uv.x); // CSS left %
            row.c2 = pct(uv.y); // CSS top %
            rows.push_back(std::move(row));
        });
    ctx.ui.setRows("map", std::move(rows));

    ctx.ui.setBool("map", "interiorNote", ctx.interior);
    pushMarker(ctx);
    ctx.screenStack.show("map");
}

void MapController::pushMarker(const MapContext& ctx) {
    // Indoors the overworld position is meaningless — hide the marker.
    const bool visible = extentValid_ && !ctx.interior;
    ctx.ui.setBool("map", "playerVisible", visible);
    if (visible) {
        const Vec2 uv = mapUv(desc_, ctx.playerPos.x, ctx.playerPos.z);
        ctx.ui.setNumber("map", "playerX", uv.x * 100.0f);
        ctx.ui.setNumber("map", "playerY", uv.y * 100.0f);
    }
}

void MapController::updateOpen(const MapContext& ctx) {
    // Publish a finished bake — MAIN thread only (setRuntimeTexture
    // releases the old GL texture through RmlUi).
    if (pending_ && pending_->done.load(std::memory_order_acquire)) {
        ctx.ui.setRuntimeTexture("map", pending_->pixels.data(),
                                 pending_->desc.size, pending_->desc.size);
        rasterWorldspace_ = pending_->worldspace;
        hasRaster_ = true;
        pending_.reset();
    }
    pushMarker(ctx);
}

void MapController::handleEvent(const MapContext& ctx, const str& event) {
    if (event == "mapBack") {
        ctx.screenStack.closeTop();
    }
}

} // namespace game
