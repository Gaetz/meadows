#include "gameplay/cue/GameplayCues.hpp"

#include "data/forms/FormQuery.hpp"

namespace gameplay {

void CueTable::build(const data::FormDatabase& forms) {
    byTag.clear();
    data::forEach<data::CueForm>(forms, [&](const data::CueForm& cue) {
        // Load order wins on duplicate tags (insert_or_assign + handle
        // order = deterministic §5 semantics).
        byTag.insert_or_assign(cue.tag, &cue);
    });
}

const data::CueForm* CueTable::find(std::string_view tag) const {
    str current { tag };
    while (!current.empty()) {
        if (const auto it = byTag.find(current); it != byTag.end()) {
            return it->second;
        }
        const size_t dot = current.rfind('.');
        if (dot == str::npos) {
            return nullptr;
        }
        current.resize(dot); // "Cue.Hit.Slash" -> "Cue.Hit" -> "Cue"
    }
    return nullptr;
}

fx::EmitterParams toEmitterParams(const data::ParticleForm& form) {
    fx::EmitterParams params;
    // The full authoring surface maps — shape, continuous rate,
    // duration and the blend batch key (texture stays a render concern).
    params.shape = form.shape == "sphere" ? fx::EmitterShape::Sphere
                   : form.shape == "cone" ? fx::EmitterShape::Cone
                   : form.shape == "box"  ? fx::EmitterShape::Box
                                          : fx::EmitterShape::Point;
    // The form authors the cone in DEGREES of half-angle; the sim takes
    // radians (and meters for the volume shapes).
    params.shapeRadius = params.shape == fx::EmitterShape::Cone
                             ? glm::radians(form.shapeRadius)
                             : form.shapeRadius;
    params.rate = form.rate;
    params.duration = form.duration;
    params.burst = form.burst;
    params.lifetime = form.lifetime;
    params.lifetimeJitter = form.lifetimeJitter;
    params.velocity = form.velocity;
    params.velocityJitter = form.velocityJitter;
    params.gravity = form.gravity;
    params.sizeStart = form.sizeStart;
    params.sizeEnd = form.sizeEnd;
    params.colorStart = form.colorStart;
    params.colorEnd = form.colorEnd;
    params.additive = form.blend == "additive";
    return params;
}

} // namespace gameplay
