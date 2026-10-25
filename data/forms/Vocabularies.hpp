#pragma once

#include <string_view>

#include "engine/reflect/Reflect.hpp"

// The closed vocabularies of the string fields declared in data/forms,
// as reflect::EnumTable (REFLECT_FIELD_ENUM): the loader rejects a value
// outside the table, the validator reports it, the property grid offers
// a dropdown. Each table is in the order of the C++ enum that consumes it
// — the consumer pins that with a static_assert next to its enum and
// parses through reflect::parseEnum, so the vocabulary is written ONCE.
// Vocabularies owned by gameplay/quest/world Forms live next to their
// enums; this file holds the ones data/ cannot see the enum of.

namespace data::vocab {

// gameplay::StatusType order; "" = no buildup.
inline constexpr std::string_view kStatusTypeNames[] = {
    "poison", "bleed",    "mental",     "disease",      "curse",
    "death",  "ignition", "glaciation", "electrocution",
};
inline constexpr reflect::EnumTable kStatusTypes { kStatusTypeNames, true };

inline constexpr std::string_view kArmorSlotNames[] = { "head", "torso",
                                                        "arms", "legs" };
inline constexpr reflect::EnumTable kArmorSlots { kArmorSlotNames };

inline constexpr std::string_view kConsumableCategoryNames[] = {
    "food", "drug", "treatment" };
inline constexpr reflect::EnumTable kConsumableCategories {
    kConsumableCategoryNames };

inline constexpr std::string_view kLightKindNames[] = { "point", "spot" };
inline constexpr reflect::EnumTable kLightKinds { kLightKindNames };

// "" = direct, unshadowed (docs/RENDERING.md §3).
inline constexpr std::string_view kShadowModeNames[] = { "none", "key",
                                                         "rcOnly" };
inline constexpr reflect::EnumTable kShadowModes { kShadowModeNames, true };

// fx::EmitterShape order.
inline constexpr std::string_view kEmitterShapeNames[] = {
    "point", "sphere", "disc", "cone", "box" };
inline constexpr reflect::EnumTable kEmitterShapes { kEmitterShapeNames };

inline constexpr std::string_view kParticleBlendNames[] = {
    "alpha", "additive", "flame", "haze" };
inline constexpr reflect::EnumTable kParticleBlends { kParticleBlendNames };

inline constexpr std::string_view kAnimCompareNames[] = { "greater", "less" };
inline constexpr reflect::EnumTable kAnimCompares { kAnimCompareNames };

// world::SpellVerb / SpellTrajectory / SpellArea order.
inline constexpr std::string_view kSpellFormNames[] = {
    "create", "destroy", "transform", "control", "understand" };
inline constexpr reflect::EnumTable kSpellForms { kSpellFormNames };
inline constexpr std::string_view kSpellTrajectoryNames[] = {
    "self", "point", "stream", "projectile", "line" };
inline constexpr reflect::EnumTable kSpellTrajectories {
    kSpellTrajectoryNames };
inline constexpr std::string_view kSpellAreaNames[] = { "disc", "ring" };
inline constexpr reflect::EnumTable kSpellAreas { kSpellAreaNames };

} // namespace data::vocab
