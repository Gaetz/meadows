#include "world/spirit/SpiritRules.hpp"

#include "data/forms/FormQuery.hpp"
#include "data/forms/SpiritForms.hpp"

namespace world {

using render::terrain::SpiritKind;

SpiritVerb spiritVerbFromName(std::string_view name) {
    struct Entry {
        std::string_view name;
        SpiritVerb verb;
    };
    static constexpr Entry kVerbs[] = {
        { "suppress", SpiritVerb::Suppress },
        { "extinguish", SpiritVerb::Extinguish },
        { "ignite", SpiritVerb::Ignite },
        { "conduct", SpiritVerb::Conduct },
        { "block", SpiritVerb::Block },
        { "grow", SpiritVerb::Grow },
        { "evaporate", SpiritVerb::Evaporate },
        { "wet", SpiritVerb::Wet },
        { "freeze", SpiritVerb::Freeze },
        { "push", SpiritVerb::Push },
        { "erode", SpiritVerb::Erode },
        { "activate", SpiritVerb::Activate },
    };
    for (const Entry& e : kVerbs) {
        if (e.name == name) {
            return e.verb;
        }
    }
    return SpiritVerb::None;
}

i32 SpiritRuleTable::classIndex(std::string_view materialClass) const {
    for (size_t i = 0; i < classes.size(); ++i) {
        if (classes[i] == materialClass) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

SpiritRuleTable compileSpiritRules(const data::FormDatabase& forms) {
    SpiritRuleTable table;
    // 1. Material classes and their properties.
    data::forEach<data::SurfaceMaterialForm>(
        forms, [&](const data::SurfaceMaterialForm& m) {
            if (m.materialClass.empty() ||
                table.classIndex(m.materialClass) >= 0) {
                return;
            }
            table.classes.push_back(m.materialClass);
            table.materials.push_back({});
            table.props.push_back({ m.flammability, m.fuel, m.moisture,
                                    m.conductivity, m.hardness });
        });
    // 2. The shifumi default: every dominating pair of a triad
    // suppresses its target unless a record says otherwise.
    for (u32 a = 0; a < SpiritRuleTable::kSpirits; ++a) {
        for (u32 b = 0; b < SpiritRuleTable::kSpirits; ++b) {
            if (render::terrain::spiritDominates(
                    static_cast<SpiritKind>(a),
                    static_cast<SpiritKind>(b))) {
                table.spirits[a][b] = { SpiritVerb::Suppress, 1.0f, 0.0f };
            }
        }
    }
    // 3. The records: refine/override; reject non-spirit actors.
    data::forEach<data::SpiritRuleForm>(
        forms, [&](const data::SpiritRuleForm& r) {
            const SpiritKind actor =
                render::terrain::spiritFromName(r.actor);
            if (actor == SpiritKind::kCount) {
                table.errors.push_back(
                    "spirit rule '" + r.editorId +
                    "': actor '" + r.actor +
                    "' is not a spirit (a material never changes a "
                    "material)");
                return;
            }
            const SpiritVerb verb = spiritVerbFromName(r.verb);
            if (verb == SpiritVerb::None) {
                table.errors.push_back("spirit rule '" + r.editorId +
                                       "': unknown verb '" + r.verb + "'");
                return;
            }
            const SpiritRule rule { verb, r.rate, r.threshold };
            const SpiritKind target =
                render::terrain::spiritFromName(r.target);
            if (target != SpiritKind::kCount) {
                table.spirits[static_cast<u32>(actor)]
                             [static_cast<u32>(target)] = rule;
                return;
            }
            const i32 cls = table.classIndex(r.target);
            if (cls < 0) {
                table.errors.push_back(
                    "spirit rule '" + r.editorId + "': target '" +
                    r.target + "' is neither a spirit nor a material");
                return;
            }
            table.materials[static_cast<size_t>(cls)]
                           [static_cast<u32>(actor)] = rule;
        });
    return table;
}

} // namespace world
