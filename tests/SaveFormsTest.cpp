#include <doctest/doctest.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Hash.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/save/SaveForms.hpp"
#include "gameplay/save/SaveState.hpp"
#include "gameplay/stats/Damage.hpp"

// The save record types are ORDINARY forms — they resolve
// through §5 (create/patch/childrenOf), write through the TomlWriter, and
// an active-effect row survives its capture->restore mirror.

namespace {

core::Guid guid(const char* text) {
    return *core::Guid::fromString(text);
}

constexpr const char* kSaveToml = R"([plugin]
id = "30000000-0000-4000-8000-000000000001"
name = "save-slot"

[[records]]
form = "30000000-0000-4000-8000-000000000010"
type = "SavedStatsForm"
new = true
[records.fields]
parent = "30000000-0000-4000-8000-0000000000aa"
health = 42.5
hunger = 63.0
posture = 12.0
weapon = "30000000-0000-4000-8000-0000000000bb"

[[records]]
form = "30000000-0000-4000-8000-000000000011"
type = "SavedItemForm"
new = true
[records.fields]
parent = "30000000-0000-4000-8000-0000000000aa"
item = "30000000-0000-4000-8000-0000000000bb"
count = 3

[[records]]
form = "30000000-0000-4000-8000-000000000012"
type = "WorldStateForm"
new = true
[records.fields]
gameSeconds = 123456.789
activeWorldspace = "30000000-0000-4000-8000-0000000000cc"
playMode = true
)";

} // namespace

TEST_CASE("save forms: resolve through the ordinary §5 pipeline") {
    data::FormTypeRegistry types;
    gameplay::registerSaveFormTypes(types);
    const auto plugin = data::parsePluginToml(kSaveToml, types, "save");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    const core::Guid actor = guid("30000000-0000-4000-8000-0000000000aa");
    const auto stats =
        data::collectChildren<gameplay::SavedStatsForm>(db, actor);
    REQUIRE(stats.size() == 1);
    CHECK(stats[0]->health == doctest::Approx(42.5f));
    CHECK(stats[0]->hunger == doctest::Approx(63.0f));
    CHECK(stats[0]->posture == doctest::Approx(12.0f));
    CHECK(stats[0]->maxHealth == doctest::Approx(100.0f)); // default kept
    CHECK(stats[0]->weapon ==
          guid("30000000-0000-4000-8000-0000000000bb"));

    const auto items =
        data::collectChildren<gameplay::SavedItemForm>(db, actor);
    REQUIRE(items.size() == 1);
    CHECK(items[0]->count == 3);

    const auto* worldState = db.find<gameplay::WorldStateForm>(
        guid("30000000-0000-4000-8000-000000000012"));
    REQUIRE(worldState != nullptr);
    CHECK(worldState->gameSeconds == doctest::Approx(123456.789));
    CHECK(worldState->playMode);
}

TEST_CASE("save forms: an active effect survives capture -> TOML -> restore") {
    gameplay::GameplayTagRegistry tags;
    tags.registerTag("Status.Poisoned");

    gameplay::AbilitySystem source;
    gameplay::ActiveEffect original;
    original.attribute = core::fnv1a("health");
    original.op = gameplay::ModifierOp::Add;
    original.magnitude = -2.0f;
    original.remaining = 7.5f;
    original.period = 1.0f;
    original.sinceLastTick = 0.25f;
    original.gameTime = true;
    original.grantedTag = *tags.find("Status.Poisoned");
    original.effectId = 41;
    source.tags.add(original.grantedTag, tags);
    source.activeEffects.push_back(original);

    // Capture -> plugin -> TOML -> reparse -> resolve (the full save path).
    const core::Guid actor = guid("30000000-0000-4000-8000-0000000000aa");
    data::Plugin plugin;
    plugin.id = guid("30000000-0000-4000-8000-000000000001");
    plugin.name = "save";
    plugin.records = gameplay::captureActiveEffects(source, actor, tags);
    REQUIRE(plugin.records.size() == 1);

    data::FormTypeRegistry types;
    gameplay::registerSaveFormTypes(types);
    const str toml = data::writePluginToml(plugin, types);
    const auto reparsed = data::parsePluginToml(toml, types, "save");
    REQUIRE(reparsed.has_value());
    data::FormDatabase db;
    data::resolve({ &*reparsed }, types, db);

    const auto rows =
        data::collectChildren<gameplay::SavedEffectForm>(db, actor);
    REQUIRE(rows.size() == 1);

    gameplay::AbilitySystem restored;
    restored.nextEffectId = 1;
    gameplay::restoreActiveEffect(restored, *rows[0], tags);
    REQUIRE(restored.activeEffects.size() == 1);
    const gameplay::ActiveEffect& effect = restored.activeEffects[0];
    CHECK(effect.attribute == original.attribute);
    CHECK(effect.op == original.op);
    CHECK(effect.magnitude == doctest::Approx(original.magnitude));
    CHECK(effect.remaining == doctest::Approx(original.remaining));
    CHECK(effect.period == doctest::Approx(original.period));
    CHECK(effect.sinceLastTick == doctest::Approx(original.sinceLastTick));
    CHECK(effect.gameTime == original.gameTime);
    CHECK(effect.grantedTag == original.grantedTag);
    CHECK(effect.effectId == 1);            // reallocated, never persisted
    CHECK(restored.nextEffectId == 2);
    CHECK(restored.tags.has(original.grantedTag)); // refcount re-added

    // Determinism (§8): capturing twice yields the same record identity.
    const auto again = gameplay::captureActiveEffects(source, actor, tags);
    CHECK(again[0].formId == plugin.records[0].formId);
}

TEST_CASE("save forms: CombatState is reflected (field-name bridge)") {
    const reflect::TypeInfo& type = gameplay::CombatState::staticTypeInfo();
    CHECK(type.findField("posture") != nullptr);
    CHECK(type.findField("restSeconds") != nullptr);

    gameplay::CombatState combat;
    combat.posture = 33.0f;
    combat.restSeconds = 5.0f;
    gameplay::SavedStatsForm saved;
    gameplay::copyMatchingFields(type, &combat,
                                 gameplay::SavedStatsForm::staticTypeInfo(),
                                 &saved);
    CHECK(saved.posture == doctest::Approx(33.0f));
    CHECK(saved.restSeconds == doctest::Approx(5.0f));

    // And back.
    gameplay::CombatState back;
    gameplay::copyMatchingFields(gameplay::SavedStatsForm::staticTypeInfo(),
                                 &saved, type, &back);
    CHECK(back.posture == doctest::Approx(33.0f));
}
