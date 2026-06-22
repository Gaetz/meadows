#pragma once

#include "engine/core/Defines.hpp"
#include "gameplay/stats/Resonance.hpp"

// ResonanceDecays: transient resonance contributions that fade toward 0
// after a duration effect with expiryMode="decay" expires. One per actor,
// invisible to designers — populated automatically by tickCharacter().

namespace gameplay {

struct ResonanceDecay {
    u32 attrId       { 0 };      // attr("onyx"), attr("amber"), or attr("garnet")
    f32 remaining    { 0.0f };   // approaches 0; same sign as original magnitude
    f32 decayPerHour { 1.0f };   // pts/game-hour toward 0
    f32 initial      { 0.0f };   // snapshot for UI progress bars
};

struct ResonanceDecays {
    vector<ResonanceDecay> list;
};

// Adds all active decay contributions to `res` (before the harmony cascade).
void addResonanceDecayToResonance(const ResonanceDecays& decays, Resonance& res);

// Advances all decays toward 0 by `gameHours`; removes fully-decayed entries.
void tickResonanceDecays(ResonanceDecays& decays, f32 gameHours);

} // namespace gameplay
