#pragma once

#include "data/forms/Form.hpp"

// Moddable tuning constants for the stats system (§5). A single Form, resolved
// from the FormDatabase by a canonical guid; the defaults below match the
// original hard-coded values, so behaviour is unchanged when no tuning Form is
// present. New stats content (Phase 7) reads its knobs from here too. Threaded
// into the derived calculators (captured), survival, and damage.

namespace data {
class FormDatabase;
class FormTypeRegistry;
} // namespace data

namespace gameplay {

struct StatsTuningForm : data::Form {
    // Derived stats (CharacterStats).
    f32 attributeToMax { 5.0f };          // primary max = Σ(linked attr) × this
    f32 mitigationPerAttribute { 0.5f };  // defense / armor / resistance per attr
    f32 willBase { 1.0f };
    f32 willPerEgo { 0.25f };
    f32 basePosture { 50.0f };
    f32 posturePerAlacrity { 1.0f };
    f32 postureRegenBase { 2.0f };
    f32 postureRegenPerAlacrity { 0.333333f };
    f32 critSensBase { 25.0f };
    f32 critSensPerConstitution { 0.1f }; // subtracted
    // Status buildup / endurance (StatusBuildup, N1).
    f32 enduranceBase { 100.0f };         // buildup threshold to trigger a status
    f32 endurancePerAttribute { 0.5f };
    f32 statusBuildupDecay { 5.0f };      // buildup points lost per second
    // Damage (Damage).
    f32 flatMitigationCapBase { 25.0f };  // flat reduction cap = (this + attr) %
    f32 staggerSeconds { 1.5f };
    // Survival (Survival).
    f32 survivalThreshold { 75.0f };      // below this, a need drives resonance
    f32 survivalResonanceAtEmpty { -50.0f };
    f32 hungerHoursPerPoint { 3.0f };
    f32 thirstHoursPerPoint { 1.0f };
    f32 sleepHoursPerPoint { 1.0f };

    REFLECT_BEGIN(StatsTuningForm, data::Form)
        REFLECT_FIELD(attributeToMax)
        REFLECT_FIELD(mitigationPerAttribute)
        REFLECT_FIELD(willBase)
        REFLECT_FIELD(willPerEgo)
        REFLECT_FIELD(basePosture)
        REFLECT_FIELD(posturePerAlacrity)
        REFLECT_FIELD(postureRegenBase)
        REFLECT_FIELD(postureRegenPerAlacrity)
        REFLECT_FIELD(critSensBase)
        REFLECT_FIELD(critSensPerConstitution)
        REFLECT_FIELD(enduranceBase)
        REFLECT_FIELD(endurancePerAttribute)
        REFLECT_FIELD(statusBuildupDecay)
        REFLECT_FIELD(flatMitigationCapBase)
        REFLECT_FIELD(staggerSeconds)
        REFLECT_FIELD(survivalThreshold)
        REFLECT_FIELD(survivalResonanceAtEmpty)
        REFLECT_FIELD(hungerHoursPerPoint)
        REFLECT_FIELD(thirstHoursPerPoint)
        REFLECT_FIELD(sleepHoursPerPoint)
    REFLECT_END()
};

// Registers the stats Form types (StatsTuningForm; later items/injuries). Call at
// startup before loading plugins.
void registerStatsFormTypes(data::FormTypeRegistry& registry);

// Resolves the tuning from the database (canonical guid), or defaults if absent.
StatsTuningForm resolveStatsTuning(const data::FormDatabase& forms);

} // namespace gameplay
