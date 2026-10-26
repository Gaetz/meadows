#include "gameplay/save/SaveState.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <variant>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Hash.hpp" // deterministic SavedBountyForm / SavedScriptVarForm row guids
#include "engine/reflect/Fields.hpp"
#include "data/forms/FormQuery.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp" // grantAbility
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/ability/Tags.hpp"
#include "gameplay/actors/ActorKit.hpp"
#include "gameplay/actors/ScriptVars.hpp"
#include "gameplay/combat/Combat.hpp"

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
constexpr core::Guid kSavedScriptVarNs { 0x5344534356415221ull, 0x0000000000000008ull };

// A ScriptVars value -> the row's kind + slot. False for a kind the row
// does not carry (not script-authored state; see gameplay::ScriptVars).
bool writeScriptVarValue(const reflect::Value& value, SavedScriptVarForm& row) {
    const auto kindName = [](ScriptVarKind kind) {
        return str { reflect::enumName(kScriptVarKinds, kind) };
    };
    if (const f32* number = std::get_if<f32>(&value)) {
        row.kind = kindName(ScriptVarKind::Number);
        row.number = *number;
        return true;
    }
    if (const str* text = std::get_if<str>(&value)) {
        row.kind = kindName(ScriptVarKind::Text);
        row.text = *text;
        return true;
    }
    if (const bool* flag = std::get_if<bool>(&value)) {
        row.kind = kindName(ScriptVarKind::Flag);
        row.flag = *flag;
        return true;
    }
    return false;
}

// The inverse: the row's selected slot as a Value; nullopt for an unknown
// kind (a row from a newer build — skipped, never fatal, §5).
std::optional<reflect::Value> readScriptVarValue(const SavedScriptVarForm& row) {
    const auto kind = reflect::parseEnum<ScriptVarKind>(kScriptVarKinds, row.kind);
    if (!kind) {
        return std::nullopt;
    }
    switch (*kind) {
    case ScriptVarKind::Number:
        return reflect::Value { row.number };
    case ScriptVarKind::Text:
        return reflect::Value { row.text };
    case ScriptVarKind::Flag:
        return reflect::Value { row.flag };
    }
    return std::nullopt;
}

// One savedStatsComponents() entry: the component's TypeInfo and the
// default instance whose values seed the generated form's slots.
template<typename T>
SavedStatsComponent persistedComponent() {
    static const T kDefaults {};
    return { &T::staticTypeInfo(), &kDefaults };
}

// Visits the persisted components the entity carries, in
// savedStatsComponents() order: fn(const reflect::TypeInfo&, void*
// storage). Type-erased through the ECS reflection bridge — no per-type
// code here; the kit is bridged first so an actor assembled by hand (a
// bench) persists exactly like a spawned one. A reflected component the
// kit does not list is skipped (see SavedStatsComponent).
template<typename Fn>
void forEachPersistedComponent(ecs::Entity entity, Fn&& fn) {
    registerActorKit(entity.world());
    const std::span<const SavedStatsComponent> kit = savedStatsComponents();
    vector<void*> storage(kit.size(), nullptr);
    ecs::forEachReflectedComponent(
        entity, [&](const reflect::TypeInfo& type, void* component) {
            for (size_t i = 0; i < kit.size(); ++i) {
                if (kit[i].type->id == type.id) {
                    storage[i] = component;
                }
            }
        });
    for (size_t i = 0; i < kit.size(); ++i) {
        if (storage[i]) {
            fn(*kit[i].type, storage[i]);
        }
    }
}

constexpr SavedStatsExclusion kSavedStatsExclusions[] = {
    { "AttributeSet", "damage",
      "transient meta-attribute: a write is routed into health by the "
      "PostExecute hook and reset, never holds state across a frame" },
    { "AttributeSet", "maxHealthOverride",
      "re-seeded at spawn by the Spawner from ActorForm.maxHealth "
      "(the `override ?? formula` balance pin), before the saved state "
      "applies" },
};

} // namespace

std::span<const SavedStatsComponent> savedStatsComponents() {
    // Function-local so the TypeInfos and default instances are built on
    // first use; the reflected kit members, in kit order.
    static const vector<SavedStatsComponent> kComponents = [] {
        vector<SavedStatsComponent> components;
        forEachActorKitType([&](auto tag) {
            using T = typename decltype(tag)::type;
            if constexpr (ReflectedKitComponent<T>) {
                components.push_back(persistedComponent<T>());
            }
        });
        return components;
    }();
    return kComponents;
}

std::span<const reflect::TypeInfo* const> savedStatsComponentTypes() {
    static const vector<const reflect::TypeInfo*> kTypes = [] {
        vector<const reflect::TypeInfo*> types;
        for (const SavedStatsComponent& component : savedStatsComponents()) {
            types.push_back(component.type);
        }
        return types;
    }();
    return kTypes;
}

std::span<const SavedStatsExclusion> savedStatsExclusions() {
    return kSavedStatsExclusions;
}

bool isSavedStatsExcluded(std::string_view type, std::string_view field) {
    for (const SavedStatsExclusion& entry : kSavedStatsExclusions) {
        if (entry.type == type && entry.field == field) {
            return true;
        }
    }
    return false;
}

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
                         const GameplayTagRegistry& registry,
                         EffectIdRemap* remap) {
    ActiveEffect effect;
    // The reflected scalars land by name; the enum, the tag handle and the
    // identity are bridged below (ActiveEffect's header lists the three).
    copyMatchingFields(SavedEffectForm::staticTypeInfo(), &row,
                       ActiveEffect::staticTypeInfo(), &effect);
    effect.op = static_cast<ModifierOp>(row.op);
    // The saved id only PAIRS rows: a row whose saved id already got a
    // fresh id on this actor (the primary of an attribute2 pair) shares it.
    u32 id = 0;
    if (remap && row.effectId != 0) {
        if (const auto it = remap->find(row.effectId); it != remap->end()) {
            id = it->second;
        }
    }
    if (id == 0) {
        id = system.nextEffectId++;
        if (remap && row.effectId != 0) {
            (*remap)[row.effectId] = id;
        }
    }
    effect.effectId = id;
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
        copyMatchingFields(ActiveEffect::staticTypeInfo(), &effect,
                           SavedEffectForm::staticTypeInfo(), &row);
        row.op = static_cast<i32>(effect.op);
        row.effectId = effect.effectId;
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

    // The stat sheet — one flat record, fields matched by name.
    SavedStatsForm stats;
    stats.parent = refGuid;
    forEachPersistedComponent(
        entity, [&](const reflect::TypeInfo& type, void* component) {
            copyMatchingFields(type, component,
                               SavedStatsForm::staticTypeInfo(), &stats);
        });
    records.push_back(
        createRecord(stats, core::Guid::combine(kSavedStatsNs, refGuid)));
    // The sentinel must survive resolution even for a pristine actor: an
    // empty record would carry no field at all — keep `parent` always.
    records.back().fields.emplace(
        reflect::fieldId(SavedStatsForm::staticTypeInfo(), "parent"),
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

    if (entity.has<ScriptVars>()) {
        // Sorted by key: deterministic identities and diffs (§8). The row
        // guid derives from the key, so a variable keeps its record
        // identity across saves and a patch can target it.
        vector<std::pair<str, reflect::Value>> entries {
            entity.get<ScriptVars>().vars.begin(),
            entity.get<ScriptVars>().vars.end()
        };
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) {
                      return a.first < b.first;
                  });
        for (const auto& [key, value] : entries) {
            SavedScriptVarForm row;
            if (!writeScriptVarValue(value, row)) {
                continue;
            }
            row.parent = refGuid;
            row.key = key;
            records.push_back(createRecord(
                row, core::Guid::combine(
                         core::Guid::combine(kSavedScriptVarNs, refGuid),
                         core::Guid { core::fnv1a(key), 0x53564152u })));
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
    data::childrenOf<SavedScriptVarForm>( // §2.8 script state
        forms, refGuid,
        [&](const SavedScriptVarForm& form) { saved.scriptVars.push_back(&form); });
    return saved;
}

void applySavedState(ecs::Entity entity, const SavedActorRecords& saved,
                     const GameplayTagRegistry& registry) {
    if (!saved.stats) {
        return;
    }
    forEachPersistedComponent(
        entity, [&](const reflect::TypeInfo& type, void* component) {
            copyMatchingFields(SavedStatsForm::staticTypeInfo(), saved.stats,
                               type, component);
        });

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
    if (!saved.scriptVars.empty() || entity.has<ScriptVars>()) {
        // The rows exist because the captured entity carried the
        // component: re-create it on an entity spawned without one. A
        // present component with no rows was empty at capture.
        if (!entity.has<ScriptVars>()) {
            entity.set<ScriptVars>({});
        }
        auto& vars = entity.get_mut<ScriptVars>();
        vars.vars.clear();
        for (const SavedScriptVarForm* row : saved.scriptVars) {
            if (const auto value = readScriptVarValue(*row)) {
                vars.vars[row->key] = *value;
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
        EffectIdRemap effectIds; // this actor's saved id -> fresh id
        for (const SavedEffectForm* row : saved.effects) {
            restoreActiveEffect(system, *row, registry, &effectIds);
        }
        // §6: currents are DERIVED — seed from the restored bases, fold
        // the restored modifiers back in, then re-derive the life state
        // (a dead actor must load dead; initializeActorStats cleared it).
        // Re-mirror Follower.Protected from the restored
        // FollowerState BEFORE the life-state derive — a follower saved
        // DOWNED (0 HP under protection) must reload downed, not dead.
        // Owned tags are not captured state; this is their re-derivation.
        if (entity.has<FollowerState>()) {
            syncStateTag(system, registry, tags::FollowerProtected,
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
