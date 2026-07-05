#pragma once

#include "data/forms/Form.hpp"

// Visual/presentation Forms (horizontal pass H1). Pure data — the engine
// runtimes consume flat param structs; the world/runtime layer maps these
// Forms onto them (LandscapeTuningForm precedent). All moddable (§5).
//
// HOW TO FILL (post-7/07): these types are complete for the first
// verticals; extend by APPENDING fields only (binary ordinals must stay
// stable — CoreForms convention).

namespace data {

class FormTypeRegistry;

// A stylized material: flat albedo texture × tint, lit by the shared ramp
// (stylized.glsl). No PBR — emissive is the only extra term (bloom does
// the rest). Referenced by StaticForm, appearance slots, particles...
struct MaterialForm : Form {
    core::Guid albedoTexture;             // asset guid (VFS); 0 = white
    Vec4 tint { 1.0f, 1.0f, 1.0f, 1.0f }; // multiplies albedo
    f32 emissive { 0.0f };                // HDR emissive strength
    bool doubleSided { false };
    bool alphaCutout { false };           // fill-rate: keep the exception

    REFLECT_BEGIN(MaterialForm, Form)
        REFLECT_FIELD(albedoTexture)
        REFLECT_FIELD(tint)
        REFLECT_FIELD(emissive)
        REFLECT_FIELD(doubleSided)
        REFLECT_FIELD(alphaCutout)
    REFLECT_END()
};

// A placeable static prop: mesh + material. The Static spawn category
// gains its 3D visual through this (2D keeps using sprites).
struct StaticForm : Form {
    str displayName;
    core::Guid model;    // glTF mesh asset guid
    core::Guid material; // MaterialForm
    core::Guid sprite;   // 2D fallback/minimap icon
    bool collides { true };

    REFLECT_BEGIN(StaticForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(model)
        REFLECT_FIELD(material)
        REFLECT_FIELD(sprite)
        REFLECT_FIELD(collides)
    REFLECT_END()
};

// A local light, placed as a reference (interiors, torches). The landscape
// sun stays its own system; these are the point/spot lights of cells.
struct LightForm : Form {
    str kind { "point" }; // "point" | "spot"
    Vec3 color { 1.0f, 0.9f, 0.7f };
    f32 intensity { 1.0f };  // linear HDR
    f32 radius { 8.0f };     // meters (falloff reach)
    f32 spotAngle { 45.0f }; // degrees, spot only
    f32 flicker { 0.0f };    // 0 = steady; else amplitude of flame flicker
    bool castsShadow { false }; // budget: few key lights per interior

    REFLECT_BEGIN(LightForm, Form)
        REFLECT_FIELD(kind)
        REFLECT_FIELD(color)
        REFLECT_FIELD(intensity)
        REFLECT_FIELD(radius)
        REFLECT_FIELD(spotAngle)
        REFLECT_FIELD(flicker)
        REFLECT_FIELD(castsShadow)
    REFLECT_END()
};

// A CPU particle emitter description. Curves are start/end pairs (linear
// over lifetime) in v1 — append richer keys later if a vertical needs them.
struct ParticleForm : Form {
    str shape { "point" };   // "point" | "sphere" | "cone" | "box"
    f32 shapeRadius { 0.1f };
    f32 rate { 20.0f };      // particles/second (0 = burst only)
    i32 burst { 0 };         // particles on spawn
    f32 lifetime { 1.0f };   // seconds per particle
    f32 lifetimeJitter { 0.2f };
    Vec3 velocity { 0.0f, 1.0f, 0.0f }; // initial, meters/second
    f32 velocityJitter { 0.5f };
    Vec3 gravity { 0.0f, -3.0f, 0.0f };
    f32 sizeStart { 0.2f };  // meters
    f32 sizeEnd { 0.05f };
    Vec4 colorStart { 1.0f, 1.0f, 1.0f, 1.0f };
    Vec4 colorEnd { 1.0f, 1.0f, 1.0f, 0.0f };
    core::Guid texture;      // 0 = soft round default
    str blend { "alpha" };   // "alpha" | "additive"
    f32 duration { 0.0f };   // emitter seconds; 0 = one burst + drain

    REFLECT_BEGIN(ParticleForm, Form)
        REFLECT_FIELD(shape)
        REFLECT_FIELD(shapeRadius)
        REFLECT_FIELD(rate)
        REFLECT_FIELD(burst)
        REFLECT_FIELD(lifetime)
        REFLECT_FIELD(lifetimeJitter)
        REFLECT_FIELD(velocity)
        REFLECT_FIELD(velocityJitter)
        REFLECT_FIELD(gravity)
        REFLECT_FIELD(sizeStart)
        REFLECT_FIELD(sizeEnd)
        REFLECT_FIELD(colorStart)
        REFLECT_FIELD(colorEnd)
        REFLECT_FIELD(texture)
        REFLECT_FIELD(blend)
        REFLECT_FIELD(duration)
    REFLECT_END()
};

// A GameplayCue: the data mapping from a gameplay tag to its presentation
// (particles/sound/shake). The sim emits cues by tag (headless no-op); the
// frontend resolves them through these records. THE bridge that keeps the
// GAS presentation-agnostic AND moddable — a mod ships a spell's visuals
// as one CueForm, zero C++.
struct CueForm : Form {
    str tag;              // gameplay tag name, e.g. "Cue.Hit.Slash"
    core::Guid particles; // ParticleForm (optional)
    core::Guid sound;     // SoundForm (optional)
    f32 cameraShake { 0.0f };
    bool attachToTarget { false }; // follow the entity vs spawn at position

    REFLECT_BEGIN(CueForm, Form)
        REFLECT_FIELD(tag)
        REFLECT_FIELD(particles)
        REFLECT_FIELD(sound)
        REFLECT_FIELD(cameraShake)
        REFLECT_FIELD(attachToTarget)
    REFLECT_END()
};

void registerVisualFormTypes(FormTypeRegistry& registry);

} // namespace data
