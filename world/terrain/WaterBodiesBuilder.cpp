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

    data::forEach<WaterBodyForm>(forms, [&](const WaterBodyForm& form) {
        render::LakeSurface lake;
        lake.level = form.surfaceLevel;
        lake.minX = form.minX;
        lake.minZ = form.minZ;
        lake.maxX = form.maxX;
        lake.maxZ = form.maxZ;
        lake.tint = form.tint;
        lake.chop = form.chop;
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
