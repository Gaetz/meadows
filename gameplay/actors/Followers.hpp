#pragma once

#include <glm/glm.hpp> // Vec3 by value (Defines only forward-declares glm)

#include "engine/core/Defines.hpp"

// The follow decision (FOLLOWERS É1 — docs/CHANTIER-FOLLOWERS.md). Pure and
// headless (§2.10): position in, intent out — the game-side AI package
// (NpcScheduleController::followPlayer) executes the intent through the
// existing goTo/moveNpcAlongPath idiom. All feel knobs live in
// StatsTuningForm (§5 moddable), pulled through followTuning below.

namespace gameplay {

struct StatsTuningForm;

// The follow feel knobs, mirrored from StatsTuningForm (follow* fields).
struct FollowTuning {
    f32 nearRadius { 3.5f };     // closer than this: stand, face the player
    f32 catchupRadius { 8.0f };  // beyond this: hurry (catchupSpeed)
    f32 catchupSpeed { 1.25f };  // speedScale while catching up
    f32 teleportRadius { 40.0f };// beyond this: reposition next to the player
};

FollowTuning followTuning(const StatsTuningForm& tuning);

// What the follower should do this frame.
struct FollowIntent {
    bool move { false };      // walk toward `target`
    Vec3 target { 0.0f };     // the player's position (the path goal)
    f32 speedScale { 1.0f };  // moveNpcAlongPath scale (catchupSpeed when far)
    bool teleport { false };  // lost him: reposition near the player
};

// Distance is HORIZONTAL (both actors are terrain-grounded; the Y gap is
// presentation, not separation). Bands, inclusive at the low edge:
//   d <= near                : idle (move = false)
//   near < d <= catchup      : walk (speedScale 1)
//   catchup < d <= teleport  : walk fast (speedScale = catchupSpeed)
//   d > teleport             : teleport
FollowIntent decideFollow(const Vec3& followerPos, const Vec3& playerPos,
                          const FollowTuning& tuning);

} // namespace gameplay
