#include "game/scenes/SpiritDirector.hpp"

#include "data/forms/FormQuery.hpp"
#include "data/forms/SpiritForms.hpp"
#include "engine/core/Log.hpp"

namespace game {

void SpiritDirector::build(const data::FormDatabase& forms) {
    table = world::compileSpiritRules(forms);
    for (const str& error : table.errors) {
        LOG_WARN("Spirits: {}", error);
    }
    jetFx.fill(core::Guid {});
    holdFx.fill(core::Guid {});
    data::forEach<data::SpiritForm>(forms, [&](const data::SpiritForm& spirit) {
        const auto kind = render::terrain::spiritFromName(spirit.name);
        if (kind != render::terrain::SpiritKind::kCount) {
            jetFx[static_cast<size_t>(kind)] = spirit.jetParticles;
            holdFx[static_cast<size_t>(kind)] = spirit.holdParticles;
        }
    });
    holdSource.reset();
    sources.apply(forms);
    if (!sources.entries().empty()) {
        LOG_INFO("Spirits: {} source(s) placed from the records",
                 sources.entries().size());
    }
}

core::Guid SpiritDirector::spawn(render::terrain::SpiritKind kind, f32 x,
                                 f32 z, f32 rate, f32 radius,
                                 f32 durationSimSeconds,
                                 const core::Guid& worldspace, f32 dirX,
                                 f32 dirZ) {
    render::terrain::SpiritSource source;
    source.kind = kind;
    source.x = x;
    source.z = z;
    source.dirX = dirX;
    source.dirZ = dirZ;
    source.rate = rate;
    source.radius = radius;
    source.remaining = durationSimSeconds;
    return sources.add(source, worldspace);
}

} // namespace game
