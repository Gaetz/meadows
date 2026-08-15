#include "gameplay/save/SaveState.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Hash.hpp" // deterministic SavedBountyForm row guids
#include "data/forms/FormQuery.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp" // grantAbility
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/Skills.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

namespace gameplay {

namespace {

// Fixed namespaces for the deterministic save-record guids (§8: re-saving
// produces the same identities, so save TOMLs diff cleanly).
constexpr core::Guid kSavedEffectNs { 0x5344454646454354ull, 0x0000000000000001ull };
constexpr core::Guid kSavedStatsNs  { 0x5344535441545321ull, 0x0000000000000002ull };
constexpr core::Guid kSavedItemNs   { 0x53444954454d2121ull, 0x0000000000000003ull };
constexpr core::Guid kSavedInjuryNs { 0x5344494e4a555259ull, 0x0000000000000004ull };
constexpr core::Guid kSavedAbilityNs { 0x534441424c545921ull, 0x0000000000000005ull };
constexpr core::Guid kSavedSkillNs  { 0x5344534b494c4c21ull, 0x0000000000000006ull };
constexpr core::Guid kSavedBountyNs { 0x5344424f554e5459ull, 0x0000000000000007ull };

// Copies a component's fields into the SavedStatsForm (capture) or back
// (apply) when the entity carries it.
template<typename T>
void componentToSaved(ecs::Entity entity, SavedStatsForm& saved) {
    if (entity.has<T>()) {
        copyMatchingFields(T::staticTypeInfo(), &entity.get<T>(),
                           SavedStatsForm::staticTypeInfo(), &saved);
    }
}

template<typename T>
void savedToComponent(const SavedStatsForm& saved, ecs::Entity entity) {
    if (entity.has<T>()) {
        copyMatchingFields(SavedStatsForm::staticTypeInfo(), &saved,
                           T::staticTypeInfo(), &entity.get_mut<T>());
    }
}

} // namespace

void copyMatchingFields(const reflect::TypeInfo& srcType, const void* src,
                        const reflect::TypeInfo& dstType, void* dst) {
    for (const reflect::FieldInfo& field : srcType.fields) {
        const reflect::FieldInfo* match = dstType.findField(field.id);
        if (!match || match->kind != field.kind) {
            continue;
        }
        match->set(dst, field.get(src));
    }
}

void restoreActiveEffect(AbilitySystem& system, const SavedEffectForm& row,
                         const GameplayTagRegistry& registry) {
    ActiveEffect effect;
    effect.attribute = row.attribute;
    effect.op = static_cast<ModifierOp>(row.op);
    effect.magnitude = row.magnitude;
    effect.infinite = row.infinite;
    effect.remaining = row.remaining;
    effect.period = row.period;
    effect.sinceLastTick = row.sinceLastTick;
    effect.decayOnExpiry = row.decayOnExpiry;
    effect.decayPerHour = row.decayPerHour;
    effect.expiryMagnitude = row.expiryMagnitude;
    effect.gameTime = row.gameTime;
    effect.effectId = system.nextEffectId++;
    if (!row.grantedTag.empty()) {
        // A tag from an unloaded mod fails the lookup: the row still
        // restores, just untagged (§5: never fatal).
        if (const auto tag = registry.find(row.grantedTag)) {
            effect.grantedTag = *tag;
            system.tags.add(*tag, registry);
        }
    }
    system.activeEffects.push_back(effect);
}

vector<data::Record> captureActiveEffects(const AbilitySystem& system,
                                          const core::Guid& parent,
                                          const GameplayTagRegistry& registry) {
    vector<data::Record> records;
    records.reserve(system.activeEffects.size());
    u64 index = 0;
    for (const ActiveEffect& effect : system.activeEffects) {
        SavedEffectForm row;
        row.parent = parent;
        row.attribute = effect.attribute;
        row.op = static_cast<i32>(effect.op);
        row.magnitude = effect.magnitude;
        row.infinite = effect.infinite;
        row.remaining = effect.remaining;
        row.period = effect.period;
        row.sinceLastTick = effect.sinceLastTick;
        row.decayOnExpiry = effect.decayOnExpiry;
        row.decayPerHour = effect.decayPerHour;
        row.expiryMagnitude = effect.expiryMagnitude;
        row.gameTime = effect.gameTime;
        if (effect.grantedTag.isValid()) {
            if (const str* name = registry.nameOf(effect.grantedTag)) {
                row.grantedTag = *name;
            }
        }
        const core::Guid rowId = core::Guid::combine(
            core::Guid::combine(kSavedEffectNs, parent),
            core::Guid { 0x524f57u, ++index }); // per-row ordinal
        records.push_back(createRecord(row, rowId));
    }
    return records;
}

vector<data::Record> captureActor(ecs::Entity entity,
                                  const core::Guid& refGuid,
                                  const GameplayTagRegistry& registry) {
    vector<data::Record> records;

    // The stat sheet — one flat record, fields mirrored by name.
    SavedStatsForm stats;
    stats.parent = refGuid;
    componentToSaved<CoreAttributes>(entity, stats);
    componentToSaved<AttributeSet>(entity, stats);
    componentToSaved<Resonance>(entity, stats);
    componentToSaved<Survival>(entity, stats);
    componentToSaved<StatusBuildup>(entity, stats);
    componentToSaved<CombatState>(entity, stats);
    componentToSaved<Equipment>(entity, stats);
    componentToSaved<VendorState>(entity, stats);
    componentToSaved<Bounty>(entity, stats);
    componentToSaved<FollowerState>(entity, stats);
    records.push_back(
        createRecord(stats, core::Guid::combine(kSavedStatsNs, refGuid)));
    // The sentinel must survive resolution even for a pristine actor: an
    // empty record would carry no field at all — keep `parent` always.
    records.back().fields.emplace(
        SavedStatsForm::staticTypeInfo().findField("parent")->id,
        reflect::Value { refGuid });

    if (entity.has<AbilitySystem>()) {
        const auto effectRecords = captureActiveEffects(
            entity.get<AbilitySystem>(), refGuid, registry);
        records.insert(records.end(), effectRecords.begin(),
                       effectRecords.end());
        // The granted abilities. Sorted by guid: deterministic identities
        // and diffs (§8 — the SavedItemForm idiom); grant ORDER is not
        // load-bearing (pickPower reads "first non-attack", and the class
        // perk sync re-derives the set anyway).
        vector<core::Guid> granted = entity.get<AbilitySystem>().grantedAbilities;
        std::sort(granted.begin(), granted.end());
        for (const core::Guid& ability : granted) {
            SavedAbilityForm row;
            row.parent = refGuid;
            row.ability = ability;
            records.push_back(createRecord(
                row, core::Guid::combine(
                         core::Guid::combine(kSavedAbilityNs, refGuid),
                         ability)));
        }
    }

    if (entity.has<Inventory>()) {
        // Sorted by item guid: deterministic identities and diffs (§8).
        vector<ItemStack> stacks = entity.get<Inventory>().items;
        std::sort(stacks.begin(), stacks.end(),
                  [](const ItemStack& a, const ItemStack& b) {
                      return a.item < b.item;
                  });
        for (const ItemStack& stack : stacks) {
            if (stack.count <= 0) {
                continue;
            }
            SavedItemForm item;
            item.parent = refGuid;
            item.item = stack.item;
            item.count = stack.count;
            records.push_back(createRecord(
                item, core::Guid::combine(
                          core::Guid::combine(kSavedItemNs, refGuid),
                          stack.item)));
        }
    }

    if (entity.has<SkillProgress>()) {
        // Sorted by skill guid: deterministic identities and diffs (§8).
        vector<std::pair<core::Guid, SkillEntry>> entries {
            entity.get<SkillProgress>().skills.begin(),
            entity.get<SkillProgress>().skills.end()
        };
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) {
                      return a.first < b.first;
                  });
        for (const auto& [skill, entry] : entries) {
            if (entry.xp <= 0.0f && entry.granted == 0) {
                continue;
            }
            SavedSkillForm row;
            row.parent = refGuid;
            row.skill = skill;
            row.xp = entry.xp;
            row.granted = entry.granted;
            records.push_back(createRecord(
                row, core::Guid::combine(
                         core::Guid::combine(kSavedSkillNs, refGuid),
                         skill)));
        }
    }

    if (entity.has<Bounty>()) {
        // Per-faction slices by dotted NAME, sorted for determinism (§8);
        // the total already rides SavedStatsForm.bounty (name-matched).
        vector<std::pair<str, f32>> rows;
        for (const FactionBounty& slice : entity.get<Bounty>().perFaction) {
            if (slice.amount <= 0.0f) {
                continue;
            }
            if (const str* name = registry.nameOf(slice.faction)) {
                rows.emplace_back(*name, slice.amount);
            }
        }
        std::sort(rows.begin(), rows.end());
        for (const auto& [faction, amount] : rows) {
            SavedBountyForm row;
            row.parent = refGuid;
            row.faction = faction;
            row.amount = amount;
            records.push_back(createRecord(
                row, core::Guid::combine(
                         core::Guid::combine(kSavedBountyNs, refGuid),
                         core::Guid { core::fnv1a(faction), 0x424e5459u })));
        }
    }

    if (entity.has<Injuries>()) {
        u64 index = 0;
        for (const Injury& injury : entity.get<Injuries>().list) {
            SavedInjuryForm row;
            row.parent = refGuid;
            row.type = static_cast<i32>(injury.type);
            row.part = static_cast<i32>(injury.part);
            row.severity = injury.severity;
            row.recoveryHoursRemaining = injury.recoveryHoursRemaining;
            records.push_back(createRecord(
                row, core::Guid::combine(
                         core::Guid::combine(kSavedInjuryNs, refGuid),
                         core::Guid { 0x524f57u, ++index })));
        }
    }

    return records;
}

SavedActorRecords savedRecordsFor(const data::FormDatabase& forms,
                                  const core::Guid& refGuid) {
    SavedActorRecords saved;
    data::childrenOf<SavedStatsForm>(
        forms, refGuid,
        [&](const SavedStatsForm& form) { saved.stats = &form; });
    data::childrenOf<SavedEffectForm>(
        forms, refGuid,
        [&](const SavedEffectForm& form) { saved.effects.push_back(&form); });
    data::childrenOf<SavedItemForm>(
        forms, refGuid,
        [&](const SavedItemForm& form) { saved.items.push_back(&form); });
    data::childrenOf<SavedInjuryForm>(
        forms, refGuid,
        [&](const SavedInjuryForm& form) { saved.injuries.push_back(&form); });
    data::childrenOf<SavedAbilityForm>(
        forms, refGuid,
        [&](const SavedAbilityForm& form) { saved.abilities.push_back(&form); });
    data::childrenOf<SavedSkillForm>( // skills-by-use
        forms, refGuid,
        [&](const SavedSkillForm& form) { saved.skills.push_back(&form); });
    data::childrenOf<SavedBountyForm>( // per-faction crime
        forms, refGuid,
        [&](const SavedBountyForm& form) { saved.bounties.push_back(&form); });
    return saved;
}

void applySavedState(ecs::Entity entity, const SavedActorRecords& saved,
                     const GameplayTagRegistry& registry) {
    if (!saved.stats) {
        return;
    }
    savedToComponent<CoreAttributes>(*saved.stats, entity);
    savedToComponent<AttributeSet>(*saved.stats, entity);
    savedToComponent<Resonance>(*saved.stats, entity);
    savedToComponent<Survival>(*saved.stats, entity);
    savedToComponent<StatusBuildup>(*saved.stats, entity);
    savedToComponent<CombatState>(*saved.stats, entity);
    savedToComponent<Equipment>(*saved.stats, entity);
    savedToComponent<VendorState>(*saved.stats, entity);
    savedToComponent<Bounty>(*saved.stats, entity);
    savedToComponent<FollowerState>(*saved.stats, entity);

    if (entity.has<Inventory>()) {
        auto& bag = entity.get_mut<Inventory>();
        bag.items.clear();
        for (const SavedItemForm* item : saved.items) {
            addItem(bag, item->item, item->count);
        }
    }
    if (entity.has<SkillProgress>()) {
        auto& progress = entity.get_mut<SkillProgress>();
        progress.skills.clear();
        for (const SavedSkillForm* row : saved.skills) {
            progress.skills[row->skill] = { row->xp, row->granted };
        }
    }
    if (entity.has<Bounty>()) {
        // The total was restored by the name-match sweep above; rows
        // re-attribute it. A faction from an unloaded mod folds back into
        // the unattributed remainder (§5: never fatal).
        auto& bounty = entity.get_mut<Bounty>();
        bounty.perFaction.clear();
        for (const SavedBountyForm* row : saved.bounties) {
            if (const auto faction = registry.find(row->faction)) {
                bounty.perFaction.push_back({ *faction, row->amount });
            }
        }
    }
    if (entity.has<Injuries>()) {
        auto& injuries = entity.get_mut<Injuries>();
        injuries.list.clear();
        for (const SavedInjuryForm* row : saved.injuries) {
            injuries.list.push_back(
                { static_cast<InjuryType>(row->type),
                  static_cast<BodyPart>(row->part), row->severity,
                  row->recoveryHoursRemaining });
        }
    }

    if (entity.has<AbilitySystem>() && entity.has<AttributeSet>()) {
        auto& system = entity.get_mut<AbilitySystem>();
        // Re-grant the saved abilities (grantAbility is
        // idempotent — a class-perk sync running before or after this
        // never doubles an entry).
        for (const SavedAbilityForm* row : saved.abilities) {
            grantAbility(system, row->ability);
        }
        for (const SavedEffectForm* row : saved.effects) {
            restoreActiveEffect(system, *row, registry);
        }
        // §6: currents are DERIVED — seed from the restored bases, fold
        // the restored modifiers back in, then re-derive the life state
        // (a dead actor must load dead; initializeActorStats cleared it).
        // Re-mirror Follower.Protected from the restored
        // FollowerState BEFORE the life-state derive — a follower saved
        // DOWNED (0 HP under protection) must reload downed, not dead.
        // Owned tags are not captured state; this is their re-derivation.
        if (entity.has<FollowerState>()) {
            syncStateTag(system, registry, "Follower.Protected",
                         entity.get<FollowerState>().followerActive);
        }
        const auto& set = entity.get<AttributeSet>();
        // PARTIAL recompute only: it seeds every non-derived current from
        // the restored bases and deliberately SKIPS the derived targets
        // (system.derivedTargetIds, cached by the spawn-time
        // initializeActorStats) — their formula values survive the load,
        // so the HUD's maxima are right on the very first frame; the next
        // tickCharacter re-derives them exactly. Seeding the overlay from
        // the raw AttributeSet here would stomp maxHealth/... with the
        // authored seeds until that tick.
        recomputeCurrent(set, system);
        updateLifeState(system, registry);
    }
}

} // namespace gameplay
