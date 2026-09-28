#include <doctest/doctest.h>

#include <cmath>
#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/SpiritForms.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "world/spirit/Spells.hpp"

using core::Guid;
using namespace world;
using render::terrain::SpiritKind;

namespace {

data::SpellForm waterSpring() {
    data::SpellForm form;
    form.editorId = "SpellWaterSpring";
    form.form = "create";
    form.element = "Water";
    form.trajectory = "point";
    form.range = 24.0f;
    form.intensity = 3.0f;
    form.duration = 10.0f;
    form.areaShape = "disc";
    form.areaRadius = 4.0f;
    return form;
}

} // namespace

TEST_CASE("spells: a valid record compiles into its spec") {
    str error;
    const auto spec = compileSpell(waterSpring(), &error);
    REQUIRE(spec.has_value());
    CHECK(error.empty());
    CHECK(spec->verb == SpellVerb::Create);
    CHECK(spec->element == SpiritKind::Water);
    CHECK(spec->trajectory == SpellTrajectory::Point);
    CHECK(spec->area == SpellArea::Disc);
    CHECK(spec->range == 24.0f);
    CHECK(spec->intensity == 3.0f);
    CHECK(spec->duration == 10.0f);
    CHECK(spec->areaRadius == 4.0f);
    CHECK_FALSE(spec->channeled);
    CHECK(spec->upkeepScale == 0.25f);
    CHECK(spellSupported(*spec));

    data::SpellForm stream = waterSpring();
    stream.trajectory = "stream";
    stream.channeled = true;
    stream.duration = -1.0f;
    const auto jet = compileSpell(stream, &error);
    REQUIRE(jet.has_value());
    CHECK(jet->channeled);
    CHECK(jet->duration == -1.0f);
    CHECK(spellSupported(*jet));
}

TEST_CASE("spells: every enumerated field is validated by name") {
    str error;
    data::SpellForm bad = waterSpring();
    bad.form = "summon";
    CHECK_FALSE(compileSpell(bad, &error).has_value());
    CHECK(error.find("unknown form 'summon'") != str::npos);

    bad = waterSpring();
    bad.element = "Plasma";
    CHECK_FALSE(compileSpell(bad, &error).has_value());
    CHECK(error.find("unknown element") != str::npos);

    bad = waterSpring();
    bad.trajectory = "line";
    CHECK(compileSpell(bad, &error)->trajectory == SpellTrajectory::Line);
    bad.trajectory = "beam";
    CHECK_FALSE(compileSpell(bad, &error).has_value());
    CHECK(error.find("unknown trajectory") != str::npos);

    bad = waterSpring();
    bad.areaShape = "cube";
    CHECK_FALSE(compileSpell(bad, &error).has_value());

    bad = waterSpring();
    bad.duration = 0.0f;
    CHECK_FALSE(compileSpell(bad, &error).has_value());

    bad = waterSpring();
    bad.channeled = true;
    bad.costPeriod = 0.0f;
    CHECK_FALSE(compileSpell(bad, &error).has_value());
    CHECK(error.find("costPeriod") != str::npos);
}

TEST_CASE("spells: the matrix names what the generic caster implements") {
    auto spec = *compileSpell(waterSpring());
    CHECK(spellSupported(spec));
    spec.trajectory = SpellTrajectory::Projectile;
    CHECK_FALSE(spellSupported(spec)); // one-shot arcs wait for their brick
    spec.trajectory = SpellTrajectory::Point;
    spec.element = SpiritKind::Fire;
    CHECK(spellSupported(spec)); // the spark
    spec.trajectory = SpellTrajectory::Stream;
    CHECK_FALSE(spellSupported(spec)); // the flame jet is held by nature
    spec.channeled = true;
    CHECK(spellSupported(spec)); // the flame jet
    spec.channeled = false;
    spec.trajectory = SpellTrajectory::Point;
    spec.verb = SpellVerb::Destroy;
    CHECK(spellSupported(spec)); // put out at the aim
    spec.trajectory = SpellTrajectory::Self;
    CHECK_FALSE(spellSupported(spec)); // the ward is held by nature
    spec.channeled = true;
    CHECK(spellSupported(spec)); // the fire ward
    spec.trajectory = SpellTrajectory::Point;
    spec.verb = SpellVerb::Control;
    CHECK(spellSupported(spec)); // the firebrand, held
    spec.channeled = false;
    CHECK_FALSE(spellSupported(spec));
    spec.verb = SpellVerb::Understand;
    CHECK(spellSupported(spec)); // the fire reading
    spec.verb = SpellVerb::Create;
    spec.element = SpiritKind::Wind;
    CHECK_FALSE(spellSupported(spec)); // wind waits for E4
    spec.element = SpiritKind::Earth;
    CHECK(spellSupported(spec)); // the bump
    spec.trajectory = SpellTrajectory::Line;
    CHECK(spellSupported(spec)); // the wall
    spec.trajectory = SpellTrajectory::Point;
    spec.verb = SpellVerb::Understand;
    CHECK(spellSupported(spec)); // the ground reading
    spec.verb = SpellVerb::Control;
    CHECK_FALSE(spellSupported(spec)); // seizing a rock is channeled
    spec.channeled = true;
    CHECK(spellSupported(spec));
    spec.channeled = false;
    spec.verb = SpellVerb::Create;
    spec.verb = SpellVerb::Destroy;
    CHECK_FALSE(spellSupported(spec)); // the dig is channeled by nature
    spec.channeled = true;
    CHECK(spellSupported(spec));
    spec.channeled = false;
    spec.verb = SpellVerb::Create;
    spec.element = SpiritKind::Water;
    spec.verb = SpellVerb::Destroy;
    CHECK(spellSupported(spec)); // a draining source
    spec.verb = SpellVerb::Control;
    CHECK_FALSE(spellSupported(spec)); // control is channeled by nature
    spec.channeled = true;
    CHECK(spellSupported(spec));
    spec.verb = SpellVerb::Understand;
    CHECK(spellSupported(spec)); // the reading
    spec.verb = SpellVerb::Transform;
    CHECK_FALSE(spellSupported(spec));
}

TEST_CASE("spells: range converts to the 45-degree launch speed") {
    // v^2 / g = range on level ground at 45 degrees.
    const f32 v = launchSpeedForRange(20.0f);
    CHECK(v == doctest::Approx(std::sqrt(20.0f * 9.81f)));
    CHECK(v * v / 9.81f == doctest::Approx(20.0f));
    CHECK(launchSpeedForRange(0.0f) == 0.0f);
}

TEST_CASE("spells: a spell is the child record of its ability") {
    const Guid abilityId = *Guid::fromString("ab000000-0000-4000-8000-000000000077");
    const Guid spellId = *Guid::fromString("5b1e1700-0000-4000-8000-000000000099");
    data::FormDatabase db;
    auto ability = std::make_unique<gameplay::AbilityForm>();
    ability->id = abilityId;
    ability->editorId = "SpiritWater";
    db.add(std::move(ability), gameplay::AbilityForm::staticTypeInfo());
    auto spell = std::make_unique<data::SpellForm>(waterSpring());
    spell->id = spellId;
    spell->parent = abilityId;
    db.add(std::move(spell), data::SpellForm::staticTypeInfo());

    const auto children = data::collectChildren<data::SpellForm>(db, abilityId);
    REQUIRE(children.size() == 1);
    CHECK(children[0]->editorId == "SpellWaterSpring");
    CHECK(data::collectChildren<data::SpellForm>(db, spellId).empty());
}
