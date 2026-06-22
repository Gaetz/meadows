#include "gameplay/stats/ResonanceDecays.hpp"

#include <algorithm>
#include <cmath>

#include "gameplay/ability/AbilitySystem.hpp" // attr()

namespace gameplay {

void addResonanceDecayToResonance(const ResonanceDecays& decays, Resonance& res) {
    for (const ResonanceDecay& d : decays.list) {
        if (d.attrId == attr("onyx"))        res.onyx   += d.remaining;
        else if (d.attrId == attr("amber"))  res.amber  += d.remaining;
        else if (d.attrId == attr("garnet")) res.garnet += d.remaining;
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
