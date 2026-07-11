#include "gameplay/combat/CombatAi.hpp"

#include <glm/glm.hpp>

namespace gameplay {

CombatMove chooseCombatMove(const CombatSituation& s) {
    // Courage first: a broken fighter runs no matter the geometry
    // (0.75 courage = flees below 25% health).
    if (s.healthFraction < 1.0f - glm::clamp(s.courage, 0.0f, 1.0f)) {
        return CombatMove::Flee;
    }
    // A swing in flight plays out where it stands.
    if (s.swinging) {
        return CombatMove::Strike;
    }
    // No sight: go where the target was (the B2 investigation).
    if (!s.canSee) {
        return CombatMove::Approach;
    }
    if (s.distance <= s.attackRange && s.cooldownSeconds <= 0.0f) {
        return CombatMove::Strike;
    }
    // In the band but the attack is cooling: circle instead of standing
    // like a training dummy.
    if (s.distance <= s.preferredRange) {
        return CombatMove::Strafe;
    }
    return CombatMove::Approach;
}

std::optional<CombatMove> parseCombatMove(std::string_view name) {
    if (name == "approach") {
        return CombatMove::Approach;
    }
    if (name == "strike") {
        return CombatMove::Strike;
    }
    if (name == "strafe") {
        return CombatMove::Strafe;
    }
    if (name == "flee") {
        return CombatMove::Flee;
    }
    return std::nullopt;
}

} // namespace gameplay
