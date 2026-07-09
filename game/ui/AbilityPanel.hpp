#pragma once

#include "data/forms/FormDatabase.hpp"
#include "data/plugins/EditSession.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayTags.hpp"

namespace game {

// The dedicated AbilityForm editor (chantier 8.11): what the grid can't
// say — the wiring health (dangling cost/cooldown/effect, a cooldown
// whose EffectForm grants no tag = no cooldown at all) and "Test
// activate": a throwaway caster runs the REAL gameplay::tryActivate
// (cost paid, cooldown tag granted, effect applied to self), attributes
// shown after. tryActivate resolves guids from the RESOLVED database —
// session-created effects need an export + reload to test (stated in
// the panel). Pickers and conditions live in the Inspector (the grid's
// typed pickers + the 8.9 builder).
class AbilityPanel {
public:
    AbilityPanel(data::EditSession& session, const data::FormDatabase& forms)
        : session { session }, forms { forms } {}

    void drawEditor(const core::Guid& ability);

private:
    void resetActor();

    data::EditSession& session;
    const data::FormDatabase& forms;
    gameplay::AttributeSet testSet;
    gameplay::AbilitySystem testSystem;
    gameplay::GameplayTagRegistry testTags;
    str lastResult;
};

} // namespace game
