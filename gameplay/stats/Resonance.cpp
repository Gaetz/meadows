#include "gameplay/stats/Resonance.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace gameplay {

Resonance harmonyEffective(const Resonance& res) {
    // Cycle order amber → garnet → onyx (energy → essence → health).
    f32 vals[3] = { res.amber, res.garnet, res.onyx };

    int m = 0; // index of the most-displaced channel
    for (int i = 1; i < 3; ++i) {
        if (std::fabs(vals[i]) > std::fabs(vals[m])) {
            m = i;
        }
    }

    const f32 source = vals[m];
    const f32 c1 = std::trunc(source / 2.0f); // half, to the next channel
    const f32 c2 = std::trunc(source / 4.0f); // quarter, to the one after
    const int n1 = (m + 1) % 3;
    const int n2 = (m + 2) % 3;

    // Minimum displacement in the source's direction (one-shot; a channel keeps
    // its own value if already more displaced).
    if (source < 0.0f) {
        vals[n1] = std::min(vals[n1], c1);
        vals[n2] = std::min(vals[n2], c2);
    } else if (source > 0.0f) {
        vals[n1] = std::max(vals[n1], c1);
        vals[n2] = std::max(vals[n2], c2);
    }

    return Resonance { /*onyx*/ vals[2], /*amber*/ vals[0], /*garnet*/ vals[1] };
}

void buildResonanceModifiers(const Resonance& res, StatModifiers& mods,
                             bool harmonyBroken) {
    // A drug (N4) breaks harmony: the channels act independently (no cascade).
    const Resonance eff = harmonyBroken ? res : harmonyEffective(res);

    const auto channel = [&](f32 r, std::initializer_list<const char*> attrs,
                             const char* maxField) {
        const f32 offset = std::trunc(r / 15.0f); // ±1 attribute per 15 points
        for (const char* a : attrs) {
            mods.add[attr(a)] += offset;
        }
        // mul defaults to the multiplicative identity, then scales by (1 + r%).
        auto [it, inserted] = mods.mul.try_emplace(attr(maxField), 1.0f);
        it->second *= (1.0f + r / 100.0f);
    };

    channel(eff.onyx, { "strength", "constitution", "grace" }, "maxHealth");
    channel(eff.amber, { "dexterity", "alacrity", "perception" }, "maxEnergy");
    channel(eff.garnet, { "charisma", "ego", "insight" }, "maxEssence");
}

} // namespace gameplay
