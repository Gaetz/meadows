#pragma once

#include "data/forms/Form.hpp"

// Visual/presentation Forms (horizontal pass H1). Pure data — the engine
// runtimes consume flat param structs; the world/runtime layer maps these
// Forms onto them (LandscapeTuningForm precedent). All moddable (§5).
//
// These types are complete for the first verticals; extend them with
// new fields as needs appear.

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
    // true = the reference's
    // authored y is an offset above the terrain (props); false = authored
    // y is ABSOLUTE (building modules on a leveled pad).
    bool snapToGround { true };

    REFLECT_BEGIN(StaticForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(model)
        REFLECT_FIELD(material)
        REFLECT_FIELD(sprite)
        REFLECT_FIELD(collides)
        REFLECT_FIELD(snapToGround)
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
    // Dust light shaft (docs/3D-RENDERER.md):
    // a procedural additive prism along the light's
    // direction (the Skyrim FXShaft model). `sunLinked` re-derives the
    // direction/color from the (quantized) sun every frame — window
    // shafts follow the time of day.
    bool shaft { false };
    f32 shaftLength { 5.0f };   // meters along the direction
    f32 shaftSoftness { 0.5f }; // axial fade exponent blend
    f32 dustDensity { 0.6f };   // scrolling dust/motes visibility
    bool sunLinked { false };
    // Shadow policy per light (docs/LIGHTING.md §3): "" or "none" =
    // direct, unshadowed (legacy castsShadow=true still means "key");
    // "key" = key-shadow candidate; "rcOnly" = routed ENTIRELY through
    // the GI field (soft free penumbras — candles, mood lights).
    str shadowMode {};
    // Window projector (docs/LIGHTING.md §3): > 0 turns the light into
    // the WINDOW's rectangle extruded along the live sun — the frame
    // clips beam, pool and dust without any shadow map. The reference's
    // rotation is the window's into-room normal.
    f32 windowHalfWidth { 0.0f };
    f32 windowHalfHeight { 0.0f };

    REFLECT_BEGIN(LightForm, Form)
        REFLECT_FIELD(kind)
        REFLECT_FIELD(color)
        REFLECT_FIELD(intensity)
        REFLECT_FIELD(radius)
        REFLECT_FIELD(spotAngle)
        REFLECT_FIELD(flicker)
        REFLECT_FIELD(castsShadow)
        REFLECT_FIELD(shaft)
        REFLECT_FIELD(shaftLength)
        REFLECT_FIELD(shaftSoftness)
        REFLECT_FIELD(dustDensity)
        REFLECT_FIELD(sunLinked)
        REFLECT_FIELD(shadowMode)
        REFLECT_FIELD(windowHalfWidth)
        REFLECT_FIELD(windowHalfHeight)
    REFLECT_END()
};

// A placeable water volume (docs/3D-RENDERER.md): the box's
// TOP face is the water surface, the box itself is the "in water" test —
// mountain lakes above sea level, flooded interior rooms. The global sea
// stays the implicit case (and keeps the planar mirror; volumes render a
// stylized non-mirrored surface).
struct WaterVolumeForm : Form {
    str displayName;
    Vec3 halfExtents { 4.0f, 1.0f, 4.0f };
    Vec3 tint { 0.10f, 0.30f, 0.34f }; // linear water color
    f32 chop { 0.5f };                 // wave busyness on the surface
    bool swimmable { true };           // reserved (swim = gameplay brick)

    REFLECT_BEGIN(WaterVolumeForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(halfExtents)
        REFLECT_FIELD(tint)
        REFLECT_FIELD(chop)
        REFLECT_FIELD(swimmable)
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
    // The camera-shake feel, moddable per cue.
    // cameraShake is scaled by magnitude / shakeScale, clamped to
    // [shakeScaleMin, shakeScaleMax]; the impulse wobbles at
    // shakeAmplitude m per strength unit and decays exp(-shakeDecay·t).
    f32 shakeScale { 10.0f };    // magnitude at which shake = authored strength
    f32 shakeScaleMin { 0.5f };  // floor of the magnitude scaling
    f32 shakeScaleMax { 2.0f };  // ceiling (a crit doesn't nauseate)
    f32 shakeAmplitude { 0.05f }; // meters of offset per strength unit
    f32 shakeDecay { 9.0f };     // 1/s exponential decay (~110 ms half-life)

    REFLECT_BEGIN(CueForm, Form)
        REFLECT_FIELD(tag)
        REFLECT_FIELD(particles)
        REFLECT_FIELD(sound)
        REFLECT_FIELD(cameraShake)
        REFLECT_FIELD(attachToTarget)
        REFLECT_FIELD(shakeScale)
        REFLECT_FIELD(shakeScaleMin)
        REFLECT_FIELD(shakeScaleMax)
        REFLECT_FIELD(shakeAmplitude)
        REFLECT_FIELD(shakeDecay)
    REFLECT_END()
};

void registerVisualFormTypes(FormTypeRegistry& registry);

} // namespace data
