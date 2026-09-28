#include "world/spirit/Spells.hpp"

#include <cmath>

namespace world {

std::optional<SpellVerb> parseSpellVerb(std::string_view name) {
    if (name == "create") return SpellVerb::Create;
    if (name == "destroy") return SpellVerb::Destroy;
    if (name == "transform") return SpellVerb::Transform;
    if (name == "control") return SpellVerb::Control;
    if (name == "understand") return SpellVerb::Understand;
    return std::nullopt;
}

std::optional<SpellTrajectory> parseSpellTrajectory(std::string_view name) {
    if (name == "self") return SpellTrajectory::Self;
    if (name == "point") return SpellTrajectory::Point;
    if (name == "stream") return SpellTrajectory::Stream;
    if (name == "projectile") return SpellTrajectory::Projectile;
    if (name == "line") return SpellTrajectory::Line;
    return std::nullopt;
}

std::optional<SpellArea> parseSpellArea(std::string_view name) {
    if (name == "disc") return SpellArea::Disc;
    if (name == "ring") return SpellArea::Ring;
    return std::nullopt;
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

bool spellSupported(const SpellSpec& spec) {
    using render::terrain::SpiritKind;
    // The matrix grows one brick at a time (docs/SPELLS.md §4).
    if (spec.element == SpiritKind::Earth) {
        switch (spec.verb) {
        case SpellVerb::Create:
            // The bump (point, instant), the stone brush (point, channeled)
            // and the wall (line, press to release).
            return spec.trajectory == SpellTrajectory::Point ||
                   spec.trajectory == SpellTrajectory::Line;
        case SpellVerb::Destroy: // the dig, channeled by nature
            return spec.trajectory == SpellTrajectory::Point && spec.channeled;
        case SpellVerb::Understand: // the ground reading
            return spec.trajectory == SpellTrajectory::Point;
        case SpellVerb::Control: // a rock seized and carried, channeled
            return spec.trajectory == SpellTrajectory::Point && spec.channeled;
        case SpellVerb::Transform:
            break;
        }
        return false;
    }
    if (spec.element == SpiritKind::Fire) {
        switch (spec.verb) {
        case SpellVerb::Create: // the spark: heat under the aim, the field spreads it
            return spec.trajectory == SpellTrajectory::Point;
        case SpellVerb::Destroy: // put out at the aim; held on self = the fire ward
            return spec.trajectory == SpellTrajectory::Point ||
                   (spec.trajectory == SpellTrajectory::Self && spec.channeled);
        case SpellVerb::Understand: // the fire reading
            return spec.trajectory == SpellTrajectory::Point;
        case SpellVerb::Control: // the firebrand: a flame carried at the aim, held
            return spec.trajectory == SpellTrajectory::Point && spec.channeled;
        case SpellVerb::Transform:
            break;
        }
        return false;
    }
    if (spec.element != SpiritKind::Water) {
        return false;
    }
    switch (spec.verb) {
    case SpellVerb::Create:
        return spec.trajectory == SpellTrajectory::Point ||
               spec.trajectory == SpellTrajectory::Stream;
    case SpellVerb::Destroy: // a draining source (the kernel releases the pin)
        return spec.trajectory == SpellTrajectory::Point;
    case SpellVerb::Control: // the held volume — channeled by nature
        return spec.trajectory == SpellTrajectory::Point && spec.channeled;
    case SpellVerb::Understand: // a reading at the aim, no world action
        return spec.trajectory == SpellTrajectory::Point;
    case SpellVerb::Transform:
        break;
    }
    return false;
}

} // namespace world
