#include "world/spirit/Spells.hpp"

#include <cmath>

namespace world {

// The names are data::vocab (bound to SpellForm's fields); the asserts
// pin the enum order they encode.
static_assert(data::vocab::kSpellForms.size() ==
              static_cast<size_t>(SpellVerb::Understand) + 1);
static_assert(data::vocab::kSpellTrajectories.size() ==
              static_cast<size_t>(SpellTrajectory::Line) + 1);
static_assert(data::vocab::kSpellAreas.size() ==
              static_cast<size_t>(SpellArea::Ring) + 1);

std::optional<SpellVerb> parseSpellVerb(std::string_view name) {
    return reflect::parseEnum<SpellVerb>(data::vocab::kSpellForms, name);
}

std::optional<SpellTrajectory> parseSpellTrajectory(std::string_view name) {
    return reflect::parseEnum<SpellTrajectory>(data::vocab::kSpellTrajectories,
                                               name);
}

std::optional<SpellArea> parseSpellArea(std::string_view name) {
    return reflect::parseEnum<SpellArea>(data::vocab::kSpellAreas, name);
}

std::optional<SpellSpec> compileSpell(const data::SpellForm& form,
                                      str* error) {
    auto fail = [&](const str& what) -> std::optional<SpellSpec> {
        if (error) {
            *error = "spell '" + form.editorId + "': " + what;
        }
        return std::nullopt;
    };
    SpellSpec spec;
    const auto verb = parseSpellVerb(form.form);
    if (!verb) {
        return fail("unknown form '" + form.form + "'");
    }
    spec.verb = *verb;
    spec.element = render::terrain::spiritFromName(form.element);
    if (spec.element == render::terrain::SpiritKind::kCount) {
        return fail("unknown element '" + form.element + "'");
    }
    const auto trajectory = parseSpellTrajectory(form.trajectory);
    if (!trajectory) {
        return fail("unknown trajectory '" + form.trajectory + "'");
    }
    spec.trajectory = *trajectory;
    const auto area = parseSpellArea(form.areaShape);
    if (!area) {
        return fail("unknown areaShape '" + form.areaShape + "'");
    }
    spec.area = *area;
    if (form.range <= 0.0f) {
        return fail("range must be positive");
    }
    if (form.intensity < 0.0f) {
        return fail("intensity must not be negative");
    }
    if (form.duration == 0.0f || form.duration < -1.0f) {
        return fail("duration must be positive or -1 (permanent)");
    }
    if (form.areaRadius <= 0.0f) {
        return fail("areaRadius must be positive");
    }
    if (form.channeled && form.costPeriod <= 0.0f) {
        return fail("costPeriod must be positive on a channeled spell");
    }
    if (form.upkeepScale < 0.0f) {
        return fail("upkeepScale must not be negative");
    }
    spec.range = form.range;
    spec.intensity = form.intensity;
    spec.duration = form.duration;
    spec.areaRadius = form.areaRadius;
    spec.channeled = form.channeled;
    spec.costPeriod = form.costPeriod;
    spec.upkeepScale = form.upkeepScale;
    return spec;
}

f32 launchSpeedForRange(f32 range, f32 gravity) {
    return std::sqrt(glm::max(range, 0.0f) * gravity);
}

const SpellCell* spellCell(const SpellSpec& spec) {
    for (const SpellCell& cell : kSpellMatrix) {
        if (cell.element != spec.element || cell.verb != spec.verb ||
            cell.trajectory != spec.trajectory) {
            continue;
        }
        const bool channelFits = cell.channel == Channel::Any ||
                                 (cell.channel == Channel::Held) == spec.channeled;
        if (channelFits) {
            return &cell;
        }
    }
    return nullptr;
}

bool spellSupported(const SpellSpec& spec) {
    return spellCell(spec) != nullptr;
}

} // namespace world
