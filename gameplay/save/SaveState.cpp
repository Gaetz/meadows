#include "gameplay/save/SaveState.hpp"

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"

namespace gameplay {

namespace {

// Fixed namespaces for the deterministic save-record guids (§8: re-saving
// produces the same identities, so save TOMLs diff cleanly).
constexpr core::Guid kSavedEffectNs { 0x5344454646454354ull, 0x0000000000000001ull };

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

} // namespace gameplay
