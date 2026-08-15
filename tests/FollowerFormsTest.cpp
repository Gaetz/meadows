#include <doctest/doctest.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/actors/FollowerForms.hpp"
#include "gameplay/actors/Followers.hpp" // followerStance round-trip
#include "gameplay/save/SaveForms.hpp"
#include "gameplay/save/SaveState.hpp"

// The follower data socle (docs/CHANTIER-FOLLOWERS.md). Class curves
// resolve into CoreAttributes, perks are ordinary childrenOf records,
// FollowerState round-trips through the SavedStatsForm name-match bridge,
// and the minimal `level` attribute rides the overlay.

namespace {

core::Guid guid(const char* text) {
    return *core::Guid::fromString(text);
}

const core::Guid kClass = guid("40000000-0000-4000-8000-000000000010");
const core::Guid kOtherClass = guid("40000000-0000-4000-8000-000000000020");

constexpr const char* kFollowerToml = R"([plugin]
id = "40000000-0000-4000-8000-000000000001"
name = "follower-data"

[[records]]
form = "40000000-0000-4000-8000-000000000010"
type = "FollowerClassForm"
new = true
[records.fields]
editorId = "WarCryWarrior"
displayName = "War-cry warrior"
combatStyle = "berserk"
strengthBase = 8.0
strengthPerLevel = 0.5
insightBase = 1.0
insightPerLevel = 0.25

[[records]]
form = "40000000-0000-4000-8000-000000000011"
type = "ClassPerkForm"
new = true
[records.fields]
parent = "40000000-0000-4000-8000-000000000010"
level = 3.0
ability = "40000000-0000-4000-8000-0000000000aa"

[[records]]
form = "40000000-0000-4000-8000-000000000012"
type = "ClassPerkForm"
new = true
[records.fields]
parent = "40000000-0000-4000-8000-000000000010"
level = 5.0
effect = "40000000-0000-4000-8000-0000000000bb"

[[records]]
form = "40000000-0000-4000-8000-000000000020"
type = "FollowerClassForm"
new = true
[records.fields]
editorId = "Healer"

[[records]]
form = "40000000-0000-4000-8000-000000000021"
type = "ClassPerkForm"
new = true
[records.fields]
parent = "40000000-0000-4000-8000-000000000020"
level = 2.0
)";

} // namespace

TEST_CASE("follower forms: class curves resolve linearly into attributes") {
    gameplay::FollowerClassForm cls;
    cls.strengthBase = 8.0f;
    cls.strengthPerLevel = 0.5f;
    cls.insightBase = 1.0f;
    cls.insightPerLevel = 0.25f;

    // Level 1 = the bases, untouched.
    const gameplay::CoreAttributes atLevelOne =
        gameplay::classAttributesAt(cls, 1.0f);
    CHECK(atLevelOne.strength == doctest::Approx(8.0f));
    CHECK(atLevelOne.constitution == doctest::Approx(6.0f)); // default
    CHECK(atLevelOne.insight == doctest::Approx(1.0f));

    // Level N = base + perLevel × (N - 1), linear v1.
    const gameplay::CoreAttributes atLevelFive =
        gameplay::classAttributesAt(cls, 5.0f);
    CHECK(atLevelFive.strength == doctest::Approx(8.0f + 0.5f * 4.0f));
    CHECK(atLevelFive.insight == doctest::Approx(1.0f + 0.25f * 4.0f));
    CHECK(atLevelFive.constitution == doctest::Approx(6.0f)); // flat curve

    // Below 1 clamps to the bases (no negative growth).
    const gameplay::CoreAttributes atLevelZero =
        gameplay::classAttributesAt(cls, 0.0f);
    CHECK(atLevelZero.strength == doctest::Approx(8.0f));
}

TEST_CASE("follower forms: class perks are ordinary childrenOf records") {
    data::FormTypeRegistry types;
    gameplay::registerFollowerFormTypes(types);
    const auto plugin =
        data::parsePluginToml(kFollowerToml, types, "follower-data");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    const auto* cls = data::findByEditorId<gameplay::FollowerClassForm>(
        db, "WarCryWarrior");
    REQUIRE(cls != nullptr);
    CHECK(cls->displayName == "War-cry warrior");
    CHECK(cls->combatStyle == "berserk");
    CHECK(cls->strengthBase == doctest::Approx(8.0f));

    // Only the class's own perks come back, in plugin/creation order (§8).
    const auto perks =
        data::collectChildren<gameplay::ClassPerkForm>(db, kClass);
    REQUIRE(perks.size() == 2);
    CHECK(perks[0]->level == doctest::Approx(3.0f));
    CHECK(perks[0]->ability ==
          guid("40000000-0000-4000-8000-0000000000aa"));
    CHECK(!perks[0]->effect.isValid());
    CHECK(perks[1]->level == doctest::Approx(5.0f));
    CHECK(perks[1]->effect ==
          guid("40000000-0000-4000-8000-0000000000bb"));
    CHECK(!perks[1]->ability.isValid());

    const auto otherPerks =
        data::collectChildren<gameplay::ClassPerkForm>(db, kOtherClass);
    REQUIRE(otherPerks.size() == 1);
    CHECK(otherPerks[0]->level == doctest::Approx(2.0f));
}

TEST_CASE("follower forms: FollowerState round-trips the save bridge") {
    // The Pattern A idiom (SaveFormsTest "CombatState is reflected"):
    // component -> SavedStatsForm by fnv1a name match, then back into a
    // fresh component. The follower* prefix keeps the names unique.
    gameplay::FollowerState state;
    state.followerActive = true;
    state.followerLevel = 7.0f;
    state.followerAffinity = 12.5f;
    state.followerHoursTogether = 40.0f;
    state.followerContractExpiryHours = 168.0f;
    state.followerLastLevelSyncedFrom = 6.0f;
    state.followerLastHomeUpgradeHours = 90.0f;
    state.followerDownedRecoveryHours = 8.0f;
    gameplay::setFollowerStance(state, gameplay::FollowerStance::Stay);

    gameplay::SavedStatsForm saved;
    gameplay::copyMatchingFields(
        gameplay::FollowerState::staticTypeInfo(), &state,
        gameplay::SavedStatsForm::staticTypeInfo(), &saved);
    CHECK(saved.followerActive);
    CHECK(saved.followerLevel == doctest::Approx(7.0f));
    CHECK(saved.followerAffinity == doctest::Approx(12.5f));

    gameplay::FollowerState restored;
    gameplay::copyMatchingFields(
        gameplay::SavedStatsForm::staticTypeInfo(), &saved,
        gameplay::FollowerState::staticTypeInfo(), &restored);
    CHECK(restored.followerActive == state.followerActive);
    CHECK(restored.followerLevel == doctest::Approx(state.followerLevel));
    CHECK(restored.followerAffinity ==
          doctest::Approx(state.followerAffinity));
    CHECK(restored.followerHoursTogether ==
          doctest::Approx(state.followerHoursTogether));
    CHECK(restored.followerContractExpiryHours ==
          doctest::Approx(state.followerContractExpiryHours));
    CHECK(restored.followerLastLevelSyncedFrom ==
          doctest::Approx(state.followerLastLevelSyncedFrom));
    CHECK(restored.followerLastHomeUpgradeHours ==
          doctest::Approx(state.followerLastHomeUpgradeHours));
    CHECK(restored.followerDownedRecoveryHours ==
          doctest::Approx(state.followerDownedRecoveryHours));
    // A Stay order survives the save (followerStance rides
    // the same name-match bridge).
    CHECK(gameplay::followerStance(restored) ==
          gameplay::FollowerStance::Stay);
}

TEST_CASE("follower forms: the minimal level attribute rides the overlay") {
    // AttributeSet.level is copied into the current-value
    // overlay by initializeCurrent like every f32 field — so an
    // `AttributeAtLeast level` condition works with no extra plumbing.
    const gameplay::AttributeSet set;
    gameplay::AbilitySystem system;
    gameplay::initializeCurrent(system, set);
    CHECK(gameplay::currentValueOf(system, gameplay::attr("level")) ==
          doctest::Approx(1.0f));
}
