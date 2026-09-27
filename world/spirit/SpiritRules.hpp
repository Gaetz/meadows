#pragma once

#include <array>

#include "data/forms/FormDatabase.hpp"
#include "engine/terrain/SpiritField.hpp"

// Chantier ESPRITS: the SpiritForm / SpiritRuleForm / SurfaceMaterialForm
// records compiled into the POD table the field kernels tick with
// (engine/ never includes data/ — the Forms -> params seam). The triad
// shifumi is generated first (matter > energy > spirit > matter,
// "suppress"), then the records refine or override it; a rule whose
// actor is not a spirit is REJECTED — a material never changes a
// material, every interaction routes through a spirit (the BotW rule,
// enforced by the table's shape, not by discipline).

namespace world {

enum class SpiritVerb : u8 {
    None = 0,
    Suppress, // the shifumi default
    Extinguish,
    Ignite,
    Conduct,
    Block,
    Grow,
    Evaporate,
    Wet,
    Freeze,
    Push,
    Erode,
    Activate,
};

SpiritVerb spiritVerbFromName(std::string_view name);

struct SpiritRule {
    SpiritVerb verb { SpiritVerb::None };
    f32 rate { 0.0f };
    f32 threshold { 0.0f };
};

struct MaterialProps {
    f32 flammability { 0.0f };
    f32 fuel { 0.0f };
    f32 moisture { 0.0f };
    f32 conductivity { 0.0f };
    f32 hardness { 1.0f };
};

struct SpiritRuleTable {
    static constexpr u32 kSpirits =
        static_cast<u32>(render::terrain::SpiritKind::kCount);
    // [actor][target] between spirits.
    std::array<std::array<SpiritRule, kSpirits>, kSpirits> spirits {};
    // [actor][material class index] — classes in `classes` order.
    vector<str> classes;
    vector<std::array<SpiritRule, kSpirits>> materials; // per class
    vector<MaterialProps> props;                        // per class
    vector<str> errors; // rejected records (logged by the caller)

    const SpiritRule& between(render::terrain::SpiritKind actor,
                              render::terrain::SpiritKind target) const {
        return spirits[static_cast<u32>(actor)][static_cast<u32>(target)];
    }
    i32 classIndex(std::string_view materialClass) const;
};

SpiritRuleTable compileSpiritRules(const data::FormDatabase& forms);

} // namespace world
