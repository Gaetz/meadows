#include <doctest/doctest.h>

#include "data/forms/LandscapeForms.hpp"
#include "engine/reflect/ValueText.hpp"
#include "engine/reflect/Visit.hpp"
#include "game/MapBaker.hpp"
#include "game/TerrainGenTuning.hpp"

// The generation pupitre (docs/PAYSAGE.md §7.7, Z0): the Form's
// defaults ARE the C++ defaults, the mapping round-trips, and the map
// cache key follows every knob.

using render::terraingen::TileBakeParams;

namespace {

bool sameValue(const reflect::Value& a, const reflect::Value& b) {
    return a == b;
}

} // namespace

TEST_CASE("terrain gen tuning: the form's defaults equal the C++ defaults") {
    const data::TerrainGenTuningForm defaults;
    const data::TerrainGenTuningForm captured =
        game::captureTerrainGenTuning(TileBakeParams {});
    u32 mismatches = 0;
    reflect::forEachField(
        data::TerrainGenTuningForm::staticTypeInfo(),
        [&](const reflect::FieldInfo& field) {
            if ((field.flags & reflect::Transient) != 0) {
                return;
            }
            if (!sameValue(field.get(&defaults), field.get(&captured))) {
                ++mismatches;
                MESSAGE("field differs from the C++ default: ", field.name,
                        " form=", reflect::valueToString(field.get(&defaults)),
                        " c++=", reflect::valueToString(field.get(&captured)));
            }
        });
    CHECK(mismatches == 0);
}

TEST_CASE("terrain gen tuning: apply and capture round-trip, the key follows") {
    data::TerrainGenTuningForm form;
    form.worldPlateauStep = 90.0f;
    form.poiVerticality = 0.25f;
    form.bakeFluvialIterations = 12;
    form.rhythmPlan = false;
    form.macroReliefAmplitudeEtage2 = 33.0f;
    TileBakeParams params;
    game::applyTerrainGenTuning(form, params);
    CHECK(params.controls.world.plateauStep == 90.0f);
    CHECK(params.controls.poi.verticality == 0.25f);
    CHECK(params.fluvial.iterations == 12);
    CHECK_FALSE(params.controls.rhythm.plan);
    CHECK(params.macro.tiers[2].reliefAmplitude == 33.0f);
    const data::TerrainGenTuningForm back =
        game::captureTerrainGenTuning(params);
    u32 mismatches = 0;
    reflect::forEachField(
        data::TerrainGenTuningForm::staticTypeInfo(),
        [&](const reflect::FieldInfo& field) {
            if (!sameValue(field.get(&form), field.get(&back))) {
                ++mismatches;
            }
        });
    CHECK(mismatches == 0);
    // The cache key: default vs edited differ; the edit alone decides.
    const u64 keyDefault = game::mapBakeKey(TileBakeParams {}, 2);
    const u64 keyEdited = game::mapBakeKey(params, 2);
    CHECK(keyDefault != keyEdited);
    TileBakeParams again;
    game::applyTerrainGenTuning(form, again);
    CHECK(game::mapBakeKey(again, 2) == keyEdited);
}
