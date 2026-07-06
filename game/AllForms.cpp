#include "game/AllForms.hpp"

#include "data/forms/AnimForms.hpp"
#include "data/forms/AudioForms.hpp"
#include "data/forms/CoreForms.hpp"
#include "data/forms/LandscapeForms.hpp"
#include "data/forms/LocForms.hpp"
#include "data/forms/UiForms.hpp"
#include "data/forms/VisualForms.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/ai/AiForms.hpp"
#include "gameplay/faction/Factions.hpp"
#include "gameplay/interaction/FurnitureForms.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "quest/Dialogue.hpp"
#include "quest/Quest.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

void registerAllFormTypes(data::FormTypeRegistry& types) {
    data::registerCoreFormTypes(types);
    data::registerVisualFormTypes(types);
    data::registerAnimFormTypes(types);
    data::registerAudioFormTypes(types);
    data::registerUiFormTypes(types);
    data::registerLocFormTypes(types);
    data::registerLandscapeFormTypes(types);
    world::registerWorldFormTypes(types);
    gameplay::registerGameplayFormTypes(types);
    gameplay::registerStatsFormTypes(types);
    gameplay::registerFactionFormTypes(types);
    gameplay::registerCharacterFormTypes(types);
    gameplay::registerAiFormTypes(types);
    gameplay::registerFurnitureFormTypes(types);
    quest::registerQuestFormTypes(types);
    quest::registerDialogueFormTypes(types);
}

} // namespace game
