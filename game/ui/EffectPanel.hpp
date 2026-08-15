#pragma once

#include "data/plugins/EditSession.hpp"
#include "game/ui/TestActor.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayTags.hpp"

namespace game {

// The dedicated EffectForm editor: the 17 flat fields
// organized into the sections that matter (Modifier / Duration / Tags /
// Expiry / Buildup), each showing ONLY what its kind reads, with the
// gameplay::effectWarnings lint on top — plus "Test apply": a throwaway
// AttributeSet/AbilitySystem the effect is applied to through the REAL
// applyEffect (the session's view, drafts included), attributes shown
// before/after. Zero new mechanism — the GAS API called directly.
class EffectPanel {
public:
    explicit EffectPanel(data::EditSession& session) : session { session } {}

    void drawEditor(const core::Guid& effect);

private:

    data::EditSession& session;
    // The test actor (throwaway, editor-only).
    TestActor testActor; // the "Try it" bench
};

} // namespace game
