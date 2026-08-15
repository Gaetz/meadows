#include "gameplay/stats/ResonanceDecays.hpp"

#include <algorithm>
#include <cmath>

#include "gameplay/ability/AbilitySystem.hpp" // attr()

namespace gameplay {

void addResonanceDecayToResonance(const ResonanceDecays& decays, Resonance& res) {
    const u32 kOnyx = attr("onyx"), kAmber = attr("amber"),
              kGarnet = attr("garnet");
    for (const ResonanceDecay& d : decays.list) {
        if (d.attrId == kOnyx)        res.onyx   += d.remaining;
        else if (d.attrId == kAmber)  res.amber  += d.remaining;
        else if (d.attrId == kGarnet) res.garnet += d.remaining;
    }
}

void tickResonanceDecays(ResonanceDecays& decays, f32 gameHours) {
    for (ResonanceDecay& d : decays.list) {
        const f32 delta = d.decayPerHour * gameHours;
        if (d.remaining > 0.0f) {
            d.remaining = std::max(0.0f, d.remaining - delta);
        } else {
            d.remaining = std::min(0.0f, d.remaining + delta);
        }
    }
    std::erase_if(decays.list,
                  [](const ResonanceDecay& d) { return std::abs(d.remaining) < 0.001f; });
}

} // namespace gameplay
