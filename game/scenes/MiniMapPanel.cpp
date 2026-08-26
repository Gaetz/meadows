#include "game/scenes/MiniMapPanel.hpp"

#include <atomic>

#include <imgui.h>

#include "engine/core/Jobs.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Device.hpp"
#include "game/MapRaster.hpp"

namespace game {

namespace {

constexpr u32 kSize = 256;
constexpr f32 kRadii[] = { 1000.0f, 5000.0f, 10000.0f, 100000.0f };
const char* kRadiusLabels[] = { "1 km", "5 km", "10 km", "100 km" };
// New-tile publishes only refresh the CLOSE rings: at 100 km a tile is
// ~2 px and the raster is mostly analytic anyway — re-baking ~1 s of
// worker per publish while roaming would starve the bake queue.
constexpr f32 kStampRefreshMaxRadius = 20000.0f;

} // namespace

struct MiniMapPanel::Job {
    render::TerrainParams params; // own copy (patches ride an sptr)
    MapRasterDesc desc {};
    vector<u8> pixels;
    std::atomic<bool> done { false };
    f32 centerX { 0.0f };
    f32 centerZ { 0.0f };
    u32 radius { 0 };
    u64 stamp { 0 };
};

void MiniMapPanel::draw(rhi::Device& device, core::JobSystem& jobs,
                        const render::TerrainParams& terrain,
                        const Vec3& cameraPos, u64 contentStamp,
                        bool* open) {
    // Publish a finished bake (main thread owns the GPU).
    if (pending && pending->done.load(std::memory_order_acquire)) {
        if (hasTexture) {
            device.destroyTexture(texture);
        }
        texture = device.createTexture(
            { .width = kSize,
              .height = kSize,
              .format = rhi::TextureFormat::RGBA8,
              .usage = rhi::TextureUsage_Sampled },
            pending->pixels.data());
        hasTexture = true;
        bakedCenterX = pending->centerX;
        bakedCenterZ = pending->centerZ;
        bakedSpan = pending->desc.maxX - pending->desc.minX;
        bakedRadius = pending->radius;
        bakedStamp = pending->stamp;
        pending.reset();
    }

    ImGui::SetNextWindowSize(ImVec2(300.0f, 372.0f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Map", open)) {
        ImGui::End();
        return;
    }
    for (u32 i = 0; i < 4; ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        if (ImGui::RadioButton(kRadiusLabels[i], radiusIndex == i)) {
            radiusIndex = i;
        }
    }

    // Kick a bake when stale: no raster, radius switch, camera strayed,
    // or (close rings) the terrain content changed under it.
    bool stale = !hasTexture || bakedRadius != radiusIndex;
    if (!stale) {
        const f32 moved =
            glm::max(std::abs(cameraPos.x - bakedCenterX),
                     std::abs(cameraPos.z - bakedCenterZ));
        stale = moved > bakedSpan * 0.12f ||
                (contentStamp != bakedStamp &&
                 kRadii[radiusIndex] <= kStampRefreshMaxRadius);
    }
    if (stale && !pending) {
        auto job = std::make_shared<Job>();
        job->params = terrain;
        job->centerX = cameraPos.x;
        job->centerZ = cameraPos.z;
        job->radius = radiusIndex;
        job->stamp = contentStamp;
        const f32 r = kRadii[radiusIndex];
        job->desc = { .terrain = &job->params,
                      .minX = cameraPos.x - r,
                      .minZ = cameraPos.z - r,
                      .maxX = cameraPos.x + r,
                      .maxZ = cameraPos.z + r,
                      .size = kSize };
        pending = job;
        jobs.enqueue([job] {
            job->pixels = generateMapRaster(job->desc);
            job->done.store(true, std::memory_order_release);
        });
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 side =
        glm::max(glm::min(avail.x, avail.y - 18.0f), 64.0f);
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    if (hasTexture) {
        ImGui::Image(static_cast<ImTextureID>(texture.id),
                     ImVec2(side, side));
        // Camera marker: offset from the BAKED center (the map only
        // re-centers when it re-bakes).
        const f32 u = glm::clamp(
            0.5f + (cameraPos.x - bakedCenterX) / bakedSpan, 0.02f,
            0.98f);
        const f32 v = glm::clamp(
            0.5f + (cameraPos.z - bakedCenterZ) / bakedSpan, 0.02f,
            0.98f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 at { corner.x + u * side, corner.y + v * side };
        draw->AddCircleFilled(at, 4.0f, IM_COL32(255, 255, 255, 255));
        draw->AddCircle(at, 5.0f, IM_COL32(20, 20, 20, 255), 0, 2.0f);
    } else {
        ImGui::Dummy(ImVec2(side, side));
    }
    ImGui::TextDisabled("%.0f m/px%s",
                        kRadii[radiusIndex] * 2.0f /
                            static_cast<f32>(kSize),
                        pending ? "  (bake...)" : "");
    ImGui::End();
}

void MiniMapPanel::shutdown(rhi::Device& device) {
    if (hasTexture) {
        device.destroyTexture(texture);
        texture = {};
        hasTexture = false;
    }
    pending.reset();
    bakedRadius = ~0u;
}

} // namespace game
