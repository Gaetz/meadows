#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/ecs/World.hpp"
#include "game/SaveGame.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/actors/Followers.hpp"
#include "gameplay/save/SaveForms.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Spawner.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldForms.hpp"
#include "world/worldspace/WorldModel.hpp"

// FOLLOWERS É1 (docs/CHANTIER-FOLLOWERS.md): the pure follow decision and
// the recruit/dismiss persistence contract at the pending-layer level —
// recruit = ReferenceForm.cell -> 0 as a field-level patch (chantier 5),
// dismiss = cell -> home; the flush is the disk save's input.

using core::Guid;

TEST_CASE("followers: decideFollow bands and boundaries") {
    const gameplay::FollowTuning tuning; // 3.5 / 8 / 1.25 / 40
    const Vec3 player { 0.0f, 0.0f, 0.0f };
    const auto at = [&](f32 distance) {
        return gameplay::decideFollow(Vec3 { distance, 0.0f, 0.0f }, player,
                                      tuning);
    };

    // Near: idle (inclusive at the boundary).
    CHECK_FALSE(at(0.0f).move);
    CHECK_FALSE(at(3.5f).move);
    CHECK_FALSE(at(3.5f).teleport);

    // Walk band: just past near up to catchup, speedScale 1.
    CHECK(at(3.6f).move);
    CHECK(at(3.6f).speedScale == doctest::Approx(1.0f));
    CHECK(at(8.0f).move);
    CHECK(at(8.0f).speedScale == doctest::Approx(1.0f));

    // Catchup band: past catchup up to teleport, catchupSpeed.
    CHECK(at(8.1f).move);
    CHECK(at(8.1f).speedScale == doctest::Approx(1.25f));
    CHECK(at(40.0f).move);
    CHECK(at(40.0f).speedScale == doctest::Approx(1.25f));
    CHECK_FALSE(at(40.0f).teleport);

    // Lost: beyond teleport.
    CHECK(at(40.1f).teleport);
    CHECK_FALSE(at(40.1f).move);

    // The target is always the player; the Y gap never counts (both
    // actors are terrain-grounded).
    const auto uphill = gameplay::decideFollow(
        Vec3 { 2.0f, 30.0f, 0.0f }, player, tuning);
    CHECK_FALSE(uphill.move);
    CHECK(uphill.target.x == doctest::Approx(player.x));
}

TEST_CASE("followers: followTuning mirrors the StatsTuningForm knobs") {
    gameplay::StatsTuningForm form;
    form.followNearRadius = 2.0f;
    form.followCatchupRadius = 5.0f;
    form.followCatchupSpeed = 1.5f;
    form.followTeleportRadius = 25.0f;
    const gameplay::FollowTuning tuning = gameplay::followTuning(form);
    CHECK(tuning.nearRadius == doctest::Approx(2.0f));
    CHECK(tuning.catchupRadius == doctest::Approx(5.0f));
    CHECK(tuning.catchupSpeed == doctest::Approx(1.5f));
    CHECK(tuning.teleportRadius == doctest::Approx(25.0f));
}

namespace {

const Guid kCell = *Guid::fromString("22220000-0000-4000-8000-0000000000f0");
const Guid kFollowerForm =
    *Guid::fromString("6a1dc0de-0000-4000-8000-0000000000f1");
const Guid kFollowerRef =
    *Guid::fromString("6a1dc0de-0000-4000-8000-0000000000f2");

constexpr const char* kFollowerPlugin = R"toml(
[plugin]
id = "11111111-1111-4111-8111-1111111111f0"
name = "base"

[[records]]
form = "11110000-0000-4000-8000-0000000000f0"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "Overworld"

[[records]]
form = "22220000-0000-4000-8000-0000000000f0"
type = "CellForm"
new = true
[records.fields]
worldspace = "11110000-0000-4000-8000-0000000000f0"

[[records]]
form = "6a1dc0de-0000-4000-8000-0000000000f1"
type = "ActorForm"
new = true
[records.fields]
editorId = "Aldric"
followerCategory = "major"

[[records]]
form = "6a1dc0de-0000-4000-8000-0000000000f2"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "6a1dc0de-0000-4000-8000-0000000000f1"
cell = "22220000-0000-4000-8000-0000000000f0"
position = [4.0, 0.0, 4.0]
)toml";

ecs::Entity findByRef(ecs::World& world, const Guid& refGuid) {
    ecs::Entity found {};
    world.handle().query<const world::RefId>().each(
        [&](flecs::entity e, const world::RefId& ref) {
            if (ref.referenceId == refGuid) {
                found = ecs::Entity { e };
            }
        });
    return found;
}

u32 countByRef(ecs::World& world, const Guid& refGuid) {
    u32 count = 0;
    world.handle().query<const world::RefId>().each(
        [&](flecs::entity, const world::RefId& ref) {
            if (ref.referenceId == refGuid) {
                ++count;
            }
        });
    return count;
}

// The flush record patching `ref`'s cell field, if any.
std::optional<Guid> flushedCell(const game::PendingSaveLayer& pending,
                                const Guid& ref) {
    const u32 cellFieldId =
        world::ReferenceForm::staticTypeInfo().findField("cell")->id;
    for (const data::Record& record : pending.flush()) {
        if (record.formId != ref) {
            continue;
        }
        if (const auto field = record.fields.find(cellFieldId);
            field != record.fields.end()) {
            return std::get<Guid>(field->second);
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("followers: recruit/dismiss ride the pending layer's cell patch") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    world::registerWorldFormTypes(types);
    gameplay::registerSaveFormTypes(types);
    const auto plugin = data::parsePluginToml(kFollowerPlugin, types, "base");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    ecs::World world;
    world::registerSceneComponents(world);
    gameplay::registerGameplayComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);
    const world::WorldModel model = world::WorldModel::build(db);
    world::CellLoader loader { world, db, model, spawner, categories };

    gameplay::GameplayTagRegistry tags;
    tags.registerTag("State.Dead");
    game::PendingSaveLayer pending;
    loader.beforeUnload = [&](data::FormHandle, ecs::Entity cellEntity) {
        pending.captureCell(world, db, cellEntity, tags);
    };
    // The scene's É1 filter: disabled OR re-homed references don't respawn.
    loader.spawnFilter = [&](const Guid& referenceId) {
        return pending.isEnabled(referenceId) &&
               !pending.isRehomed(referenceId);
    };

    const ecs::Entity cellEntity = loader.loadCell(db.handleOf(kCell));
    ecs::Entity follower = findByRef(world, kFollowerRef);
    REQUIRE(follower.is_alive());
    follower.set<gameplay::AttributeSet>({});
    follower.set<gameplay::AbilitySystem>({});
    follower.set<gameplay::FollowerState>({});

    // --- Recruit: the chantier-5 contract, as FollowerController applies
    // it — active state, live cell -> null, InCell dropped, captured.
    follower.get_mut<gameplay::FollowerState>().followerActive = true;
    follower.get_mut<world::RefId>().cell = data::FormHandle {};
    follower.remove<ecs::InCell>(flecs::Wildcard);
    pending.captureEntity(follower, db, tags);

    CHECK(pending.isRehomed(kFollowerRef));
    // The flush (= the disk save's records) carries cell = 0: persistent,
    // exactly like the player.
    const auto recruited = flushedCell(pending, kFollowerRef);
    REQUIRE(recruited.has_value());
    CHECK(*recruited == Guid {});

    // His origin cell unloads (the player walks away): no InCell, so the
    // delete_with sweep misses him — he keeps following.
    loader.unloadCell(db.handleOf(kCell));
    CHECK(findByRef(world, kFollowerRef).is_alive());

    // The cell reloads (the player walks back): the re-home veto keeps
    // the authored record from spawning a DOUBLE of him.
    loader.loadCell(db.handleOf(kCell));
    CHECK(countByRef(world, kFollowerRef) == 1);

    // --- Dismiss (home cell resident): cell -> home (= the authored
    // cell), InCell re-added; the capture drops the cell diff.
    const ecs::Entity homeCell = loader.cellEntity(db.handleOf(kCell));
    REQUIRE(homeCell.is_alive());
    follower.get_mut<gameplay::FollowerState>().followerActive = false;
    follower.get_mut<world::RefId>().cell = db.handleOf(kCell);
    follower.add<ecs::InCell>(homeCell);
    pending.captureEntity(follower, db, tags);

    CHECK_FALSE(pending.isRehomed(kFollowerRef));
    CHECK_FALSE(flushedCell(pending, kFollowerRef).has_value());
    // The saved actor state remembers he is off duty.
    REQUIRE(pending.hasActorState(kFollowerRef));
    CHECK_FALSE(pending.actorState(kFollowerRef).stats->followerActive);

    // Back home, the normal unload/reload cycle owns him again.
    loader.unloadCell(db.handleOf(kCell));
    CHECK_FALSE(findByRef(world, kFollowerRef).is_alive());
    loader.loadCell(db.handleOf(kCell));
    CHECK(countByRef(world, kFollowerRef) == 1);
    (void)cellEntity;
}

// --- É2: the aggro table (gameplay::adoptOnHit — pure, headless) ------------
// Entity ids stand in for the cast: 1 = player, 2/3 = active followers,
// 10/11 = hostiles, 20 = a villager (no roles).

namespace {

constexpr u64 kPlayer = 1;
constexpr u64 kFollower = 2;
constexpr u64 kFollower2 = 3;
constexpr u64 kHostile = 10;
constexpr u64 kHostile2 = 11;

// Roles for `self`, given the cast above.
gameplay::AggroRoles rolesFor(u64 self, u64 source, u64 target,
                              bool selfHasLiveTarget = false,
                              bool friendlyTrial = false) {
    const auto isFollower = [](u64 id) {
        return id == kFollower || id == kFollower2;
    };
    const auto isHostile = [](u64 id) {
        return id == kHostile || id == kHostile2;
    };
    gameplay::AggroRoles roles;
    roles.self = self;
    roles.selfFollower = isFollower(self);
    roles.selfHostile = isHostile(self);
    roles.selfHasLiveTarget = selfHasLiveTarget;
    roles.sourcePlayer = source == kPlayer;
    roles.sourceFollower = isFollower(source);
    roles.sourceHostile = isHostile(source);
    roles.targetPlayer = target == kPlayer;
    roles.targetFollower = isFollower(target);
    roles.targetHostile = isHostile(target);
    roles.friendlyTrial = friendlyTrial;
    return roles;
}

u64 adopt(u64 self, u64 source, u64 target, bool selfHasLiveTarget = false,
          bool friendlyTrial = false) {
    return gameplay::adoptOnHit(
        source, target,
        rolesFor(self, source, target, selfHasLiveTarget, friendlyTrial));
}

} // namespace

TEST_CASE("followers É2: a follower defends the party") {
    // A hostile hits the player -> the idle follower adopts the hostile.
    CHECK(adopt(kFollower, kHostile, kPlayer) == kHostile);
    // A hostile hits a FELLOW follower -> same adoption.
    CHECK(adopt(kFollower, kHostile, kFollower2) == kHostile);
    // Already committed to a live target: no target hopping.
    CHECK(adopt(kFollower, kHostile2, kPlayer, true) == 0);
    // ...unless HE is the one being hit: the victim re-aims.
    CHECK(adopt(kFollower, kHostile2, kFollower, true) == kHostile2);
    // The player's own hits on the party never get adopted (brawl,
    // friendly fire): the source must be a hostile.
    CHECK(adopt(kFollower, kPlayer, kFollower2) == 0);
    // A villager (no follower/hostile role) reacts to nothing.
    CHECK(adopt(20, kHostile, kPlayer) == 0);
}

TEST_CASE("followers É2: the player's initiative is followed") {
    // The player strikes a hostile first -> followers adopt it.
    CHECK(adopt(kFollower, kPlayer, kHostile) == kHostile);
    // But not when committed elsewhere...
    CHECK(adopt(kFollower, kPlayer, kHostile, true) == 0);
    // ...and never a NON-hostile victim (a crime is not an order).
    CHECK(adopt(kFollower, kPlayer, 20) == 0);
}

TEST_CASE("followers É2: hostiles fight followers back") {
    // A follower hits a hostile -> the hostile re-aims at the follower.
    CHECK(adopt(kHostile, kFollower, kHostile) == kFollower);
    // Hit by the PLAYER: keep the default player targeting (no
    // combatTarget — the exact pre-É2 behavior).
    CHECK(adopt(kHostile, kPlayer, kHostile) == 0);
    // Another hostile's brawl is not his problem.
    CHECK(adopt(kHostile, kFollower, kHostile2) == 0);
    // Self-targeting guard: a hostile "hitting itself" adopts nothing.
    CHECK(adopt(kHostile, kHostile, kHostile) == 0);
}

TEST_CASE("followers É2: Combat.FriendlyTrial gates follower adoption") {
    // Every follower adoption path is suppressed...
    CHECK(adopt(kFollower, kHostile, kPlayer, false, true) == 0);
    CHECK(adopt(kFollower, kPlayer, kHostile, false, true) == 0);
    CHECK(adopt(kFollower, kHostile, kFollower, false, true) == 0);
    // ...but hostile retaliation is not (a real enemy stays real).
    CHECK(adopt(kHostile, kFollower, kHostile, false, true) == kFollower);
}

TEST_CASE("followers É2: death disengages whoever targeted the dead") {
    CHECK(gameplay::disengageOnDeath(kHostile, kHostile));
    CHECK_FALSE(gameplay::disengageOnDeath(kHostile, kHostile2));
    CHECK_FALSE(gameplay::disengageOnDeath(kHostile, 0)); // no target: no-op
}
