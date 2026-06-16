#include <memory>

#include <doctest/doctest.h>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Rng.hpp"
#include "gameplay/stats/Afflictions.hpp"

using namespace gameplay;

namespace {
core::Guid addAffliction(data::FormDatabase& forms, const char* guid,
                         const char* channel, f32 resonance, const char* attribute,
                         f32 attributeValue, f32 recovery) {
    auto form = std::make_unique<AfflictionForm>();
    form->id = *core::Guid::fromString(guid);
    form->channel = channel;
    form->resonancePenalty = resonance;
    form->attributeMalus = attribute ? attribute : "";
    form->attributeMalusValue = attributeValue;
    form->recoveryHours = recovery;
    const core::Guid id = form->id;
    forms.add(std::move(form), AfflictionForm::staticTypeInfo());
    return id;
}
} // namespace

TEST_CASE("afflictions: a disease adds amber resonance and an attribute malus") {
    data::FormDatabase forms;
    const core::Guid fever = addAffliction(
        forms, "d1000000-0000-4000-8000-000000000001", "amber", -15.0f,
        "constitution", -2.0f, 48.0f);
    Afflictions afflictions;
    afflictions.list.push_back({ fever, 48.0f });

    const Resonance res = afflictionResonance(afflictions, forms);
    CHECK(res.amber == doctest::Approx(-15.0f));
    CHECK(res.garnet == doctest::Approx(0.0f));
    CHECK(res.onyx == doctest::Approx(0.0f));

    StatModifiers mods;
    afflictionStatModifiers(afflictions, forms, mods);
    CHECK(mods.add[attr("constitution")] == doctest::Approx(-2.0f));
}

TEST_CASE("afflictions: a psychosis hits the garnet (essence) channel") {
    data::FormDatabase forms;
    const core::Guid phobia = addAffliction(
        forms, "d2000000-0000-4000-8000-000000000001", "garnet", -20.0f, "", 0.0f,
        72.0f);
    Afflictions a;
    a.list.push_back({ phobia, 72.0f });
    CHECK(afflictionResonance(a, forms).garnet == doctest::Approx(-20.0f));
}

TEST_CASE("afflictions: inflict is gated by channel resonance-resistance (§2)") {
    data::FormDatabase forms;
    const core::Guid fever = addAffliction(
        forms, "d3000000-0000-4000-8000-000000000001", "amber", -15.0f, "", 0.0f,
        48.0f);
    const AfflictionForm* def = forms.find<AfflictionForm>(fever);
    REQUIRE(def != nullptr);

    Afflictions a;
    core::Rng rng(1);
    CHECK_FALSE(inflictAffliction(a, fever, *def, 0.0f, 1.0, rng)); // amber ≥ 0 → immune
    CHECK(a.list.empty());
    CHECK(inflictAffliction(a, fever, *def, -100.0f, 1.0, rng));    // chance 1.0
    CHECK(a.list.size() == 1);
}

TEST_CASE("afflictions: rest clears them when the timer elapses") {
    data::FormDatabase forms;
    const core::Guid fever = addAffliction(
        forms, "d4000000-0000-4000-8000-000000000001", "amber", -15.0f, "", 0.0f,
        48.0f);
    Afflictions a;
    a.list.push_back({ fever, 48.0f });
    recoverAfflictions(a, 24.0f);
    CHECK(a.list.size() == 1);
    recoverAfflictions(a, 24.0f);
    CHECK(a.list.empty());
}
