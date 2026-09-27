#include <doctest/doctest.h>

#include <cmath>

#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/forms/SpiritForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "world/spirit/SpiritRules.hpp"
#include "world/spirit/SpiritSources.hpp"

// Chantier ESPRITS E1.b: the placed sources are ordinary records (a save
// emits them, a mod ships them), the triad shifumi is a function of the
// kind order, and the rule compiler enforces "a material never changes a
// material".

using namespace world;
using render::terrain::SpiritKind;

namespace {

const core::Guid kMap =
    *core::Guid::fromString("cccc0001-0000-4000-8000-000000000001");
const core::Guid kOtherMap =
    *core::Guid::fromString("cccc0002-0000-4000-8000-000000000001");

data::FormTypeRegistry makeTypes() {
    data::FormTypeRegistry types;
    registerWorldFormTypes(types);
    data::registerSpiritFormTypes(types);
    return types;
}

render::terrain::SpiritSource spring(f32 x, f32 z, f32 rate, f32 radius,
                                     f32 seconds) {
    render::terrain::SpiritSource s;
    s.kind = SpiritKind::Water;
    s.x = x;
    s.z = z;
    s.rate = rate;
    s.radius = radius;
    s.remaining = seconds;
    return s;
}

} // namespace

TEST_CASE("spirits: the triad shifumi is a function of the kind order") {
    using render::terrain::spiritDominates;
    // Triad 3: Water > Fire > Wind > Water; nothing across triads.
    CHECK(spiritDominates(SpiritKind::Water, SpiritKind::Fire));
    CHECK(spiritDominates(SpiritKind::Fire, SpiritKind::Wind));
    CHECK(spiritDominates(SpiritKind::Wind, SpiritKind::Water));
    CHECK_FALSE(spiritDominates(SpiritKind::Fire, SpiritKind::Water));
    const bool mutual = spiritDominates(SpiritKind::Water, SpiritKind::Fire) &&
                        spiritDominates(SpiritKind::Fire, SpiritKind::Water);
    CHECK_FALSE(mutual);
    CHECK_FALSE(spiritDominates(SpiritKind::Water, SpiritKind::Lightning));
    // Triad 2: Earth > Lightning > Psy > Earth.
    CHECK(spiritDominates(SpiritKind::Earth, SpiritKind::Lightning));
    CHECK(spiritDominates(SpiritKind::Psy, SpiritKind::Earth));
    // Names round-trip.
    for (u32 i = 0; i < static_cast<u32>(SpiritKind::kCount); ++i) {
        const auto kind = static_cast<SpiritKind>(i);
        CHECK(render::terrain::spiritFromName(
                  render::terrain::spiritName(kind)) == kind);
    }
    CHECK(render::terrain::spiritFromName("Frost") == SpiritKind::kCount);
}

TEST_CASE("spirit sources: a spring round-trips through the save plugin") {
    const data::FormTypeRegistry types = makeTypes();
    SpiritSourceList list;
    const core::Guid id = list.add(spring(120.0f, 340.0f, 3.0f, 2.0f, 6.5f),
                                   kMap);
    list.add(spring(10.0f, 10.0f, 1.0f, 8.0f, -1.0f), kOtherMap);
    REQUIRE(list.entries().size() == 2);

    data::Plugin save;
    save.name = "slot";
    save.records = list.capture();
    REQUIRE(save.records.size() == 2);
    const str toml = data::writePluginToml(save, types);
    const auto reparsed = data::parsePluginToml(toml, types, "slot");
    REQUIRE(reparsed.has_value());
    data::FormDatabase db;
    data::resolve({ &*reparsed }, types, db);

    SpiritSourceList restored;
    restored.apply(db);
    REQUIRE(restored.entries().size() == 2);
    const auto& a = restored.entries()[0];
    CHECK(a.source.id == id);
    CHECK(a.worldspace == kMap);
    CHECK(a.source.kind == SpiritKind::Water);
    CHECK(a.source.x == doctest::Approx(120.0f));
    CHECK(a.source.z == doctest::Approx(340.0f));
    CHECK(a.source.rate == doctest::Approx(3.0f));
    CHECK(a.source.remaining == doctest::Approx(6.5f));
    CHECK(restored.entries()[1].source.remaining < 0.0f); // permanent
    // Re-saving after a reload keeps the SAME ids (idempotent records)
    // and mints fresh ones after the survivors.
    const core::Guid fresh =
        restored.add(spring(1.0f, 1.0f, 1.0f, 1.0f, 1.0f), kMap);
    CHECK(fresh != id);
    CHECK(restored.capture()[0].formId == id);
}

TEST_CASE("spirit sources: expiry, the cap, and the kernel view") {
    SpiritSourceList list;
    list.add(spring(0.0f, 0.0f, 3.0f, 2.0f, 2.0f), kMap);
    list.add(spring(50.0f, 50.0f, 3.0f, 2.0f, -1.0f), kMap); // permanent
    CHECK_FALSE(list.tick(1.0f));
    CHECK(list.tick(1.5f)); // the 2 s spring expired
    REQUIRE(list.entries().size() == 1);
    CHECK(list.capture().size() == 1); // expired never captured
    // Filter by map: the other map's springs are invisible.
    list.add(spring(9.0f, 9.0f, 2.0f, 2.0f, -1.0f), kOtherMap);
    CHECK(list.waterSourcesFor(WorldspaceFilter { kMap }).size() == 1);
    CHECK(list.waterSourcesFor(WorldspaceFilter { kOtherMap }).size() == 1);
    // A wide spring becomes a ring whose discharges sum to the rate.
    list.add(spring(200.0f, 200.0f, 7.0f, 10.0f, -1.0f), kMap);
    const auto ring = list.waterSourcesFor(WorldspaceFilter { kMap });
    CHECK(ring.size() == 1 + 7);
    f64 sum = 0.0;
    for (const auto& s : ring) {
        sum += s.discharge;
    }
    CHECK(sum == doctest::Approx(3.0 + 7.0));
    // The cap evicts the OLDEST deterministically.
    for (u32 i = 0; i < 40; ++i) {
        list.add(spring(static_cast<f32>(i), 0.0f, 1.0f, 1.0f, -1.0f),
                 kMap);
    }
    CHECK(list.entries().size() == SpiritSourceList::kMaxSources);
    CHECK(list.entries().front().source.x ==
          doctest::Approx(40.0f - SpiritSourceList::kMaxSources));
}

TEST_CASE("spirit rules: shifumi defaults, refinements, and the material "
          "veto") {
    const data::FormTypeRegistry types = makeTypes();
    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "cccc00ff-0000-4000-8000-000000000001"
name = "rules"

[[records]]
form = "cccc0010-0000-4000-8000-000000000001"
type = "SurfaceMaterialForm"
new = true
[records.fields]
materialClass = "grass"
flammability = 1.0

[[records]]
form = "cccc0011-0000-4000-8000-000000000001"
type = "SpiritRuleForm"
new = true
[records.fields]
editorId = "WaterFire"
actor = "Water"
target = "Fire"
verb = "extinguish"
rate = 2.0

[[records]]
form = "cccc0011-0000-4000-8000-000000000002"
type = "SpiritRuleForm"
new = true
[records.fields]
editorId = "FireGrass"
actor = "Fire"
target = "grass"
verb = "ignite"

[[records]]
form = "cccc0011-0000-4000-8000-000000000003"
type = "SpiritRuleForm"
new = true
[records.fields]
editorId = "GrassWood"
actor = "grass"
target = "wood"
verb = "ignite"
)toml",
                                              types, "rules");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);
    const SpiritRuleTable table = compileSpiritRules(db);
    // The refinement wins over the shifumi default...
    CHECK(table.between(SpiritKind::Water, SpiritKind::Fire).verb ==
          SpiritVerb::Extinguish);
    CHECK(table.between(SpiritKind::Water, SpiritKind::Fire).rate ==
          doctest::Approx(2.0f));
    // ...the default stands where no record speaks...
    CHECK(table.between(SpiritKind::Fire, SpiritKind::Wind).verb ==
          SpiritVerb::Suppress);
    // ...and nothing exists across triads without a record.
    CHECK(table.between(SpiritKind::Fire, SpiritKind::Earth).verb ==
          SpiritVerb::None);
    // Material rules land in the class table.
    const i32 grass = table.classIndex("grass");
    REQUIRE(grass >= 0);
    CHECK(table.materials[static_cast<size_t>(grass)]
                         [static_cast<u32>(SpiritKind::Fire)]
              .verb == SpiritVerb::Ignite);
    CHECK(table.props[static_cast<size_t>(grass)].flammability ==
          doctest::Approx(1.0f));
    // A material as ACTOR is rejected: materials never change materials.
    REQUIRE(table.errors.size() == 1);
    CHECK(table.errors[0].find("GrassWood") != str::npos);
}
