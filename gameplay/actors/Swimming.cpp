#include "gameplay/actors/Swimming.hpp"

namespace gameplay {

MoveMode decideMoveMode(MoveMode current, std::optional<f32> surfaceY,
                        f32 feetY, f32 headHeight, bool onGround,
                        f32 submergeDepth, f32 wadeOutRatio) {
    if (!surfaceY) {
        return MoveMode::Ground;
    }
    const f32 headY = feetY + headHeight;
    switch (current) {
    case MoveMode::Ground:
        return headY < *surfaceY - submergeDepth ? MoveMode::Swim
                                                 : MoveMode::Ground;
    case MoveMode::Swim:
        if (headY > *surfaceY) {
            return MoveMode::Ground; // surfaced
        }
        if (onGround && *surfaceY - feetY < headHeight * wadeOutRatio) {
            return MoveMode::Ground; // wading out through the shallows
        }
        return MoveMode::Swim;
    }
    return MoveMode::Ground;
}

} // namespace gameplay
