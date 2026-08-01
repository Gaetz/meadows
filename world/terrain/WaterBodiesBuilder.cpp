#include "world/terrain/WaterBodiesBuilder.hpp"

#include <algorithm>
#include <unordered_map>

#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

sptr<const render::WaterBodies> buildWaterBodies(
    const data::FormDatabase& forms, f32 seaLevel) {
    auto bodies = std::make_shared<render::WaterBodies>();
    bodies->seaLevel = seaLevel;

    // Materials: index 0 = default water; referenced presets resolve to
    // stable indices (dedup by GUID). A dangling reference falls back to
    // the default.
    bodies->materials.push_back({});
    std::unordered_map<u64, u32> materialIndices;
    const auto materialKey = [](const core::Guid& guid) {
        return guid.hi ^ (guid.lo * 0x9e3779b97f4a7c15ull);
    };
    const auto resolveMaterial = [&](const core::Guid& guid) -> u32 {
        if (!guid.isValid()) {
            return 0;
        }
        const auto found = materialIndices.find(materialKey(guid));
        if (found != materialIndices.end()) {
            return found->second;
        }
        const data::FormHandle handle = forms.handleOf(guid);
        const data::Form* raw = forms.get(handle);
        const reflect::TypeInfo* type = forms.typeOf(handle);
        if (!raw || !type ||
            !type->isA(WaterMaterialForm::staticTypeInfo().id)) {
            LOG_WARN("Water material {} not found: default water",
                     guid.toString());
            return 0;
        }
        const auto* form = static_cast<const WaterMaterialForm*>(raw);
        render::WaterMaterialParams params;
        params.tint = form->tint;
        params.tintStrength = form->tintStrength;
        params.deepColor = form->deepColor;
        params.absorption = form->absorption;
        params.foamColor = form->foamColor;
        params.foamGain = form->foamGain;
        params.emissiveColor = form->emissiveColor;
        params.emissiveStrength = form->emissiveStrength;
        params.flowSpeedScale = form->flowSpeedScale;
        params.viscosity = form->viscosity;
        params.waveScale = form->waveScale;
        const u32 index = static_cast<u32>(bodies->materials.size());
        bodies->materials.push_back(params);
        materialIndices.emplace(materialKey(guid), index);
        return index;
    };

    data::forEach<WaterBodyForm>(forms, [&](const WaterBodyForm& form) {
        render::LakeSurface lake;
        lake.level = form.surfaceLevel;
        lake.minX = form.minX;
        lake.minZ = form.minZ;
        lake.maxX = form.maxX;
        lake.maxZ = form.maxZ;
        lake.tint = form.tint;
        lake.chop = form.chop;
        lake.materialIndex = resolveMaterial(form.material);
        bodies->lakes.push_back(lake);
    });

    struct Course {
        const RiverForm* river { nullptr };
        vector<const RiverPointForm*> points;
    };
    std::unordered_map<u64, Course> courses; // key = parent guid low bits
    const auto keyOf = [](const core::Guid& guid) {
        return guid.hi ^ (guid.lo * 0x9e3779b97f4a7c15ull);
    };
    data::forEach<RiverForm>(forms, [&](const RiverForm& form) {
        courses[keyOf(form.id)].river = &form;
    });
    data::forEach<RiverPointForm>(forms,
                                  [&](const RiverPointForm& form) {
                                      courses[keyOf(form.parent)]
                                          .points.push_back(&form);
                                  });
    for (auto& [key, course] : courses) {
        if (!course.river || course.points.size() < 2) {
            if (!course.river && !course.points.empty()) {
                LOG_WARN("River points with no RiverForm parent ({})",
                         course.points.size());
            }
            continue;
        }
        std::sort(course.points.begin(), course.points.end(),
                  [](const RiverPointForm* a, const RiverPointForm* b) {
                      return a->index < b->index;
                  });
        render::RiverSurface river;
        river.tint = course.river->tint;
        river.flowSpeed = course.river->flowSpeed;
        river.materialIndex = resolveMaterial(course.river->material);
        river.minX = river.minZ = 1.0e30f;
        river.maxX = river.maxZ = -1.0e30f;
        for (const RiverPointForm* pt : course.points) {
            render::RiverNode node;
            node.x = pt->position.x;
            node.z = pt->position.z;
            node.surface = pt->position.y;
            node.halfWidth = pt->halfWidth;
            river.nodes.push_back(node);
            river.minX = glm::min(river.minX, node.x - node.halfWidth);
            river.maxX = glm::max(river.maxX, node.x + node.halfWidth);
            river.minZ = glm::min(river.minZ, node.z - node.halfWidth);
            river.maxZ = glm::max(river.maxZ, node.z + node.halfWidth);
        }
        bodies->rivers.push_back(std::move(river));
    }
    return bodies;
}

} // namespace world
