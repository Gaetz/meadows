#pragma once

#include <functional>
#include <unordered_map>

#include <glm/glm.hpp>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/VisualForms.hpp"
#include "engine/core/Defines.hpp"
#include "engine/fx/Particles.hpp"

// GameplayCues (horizontal pass H7) — THE sim/presentation bridge for
// feedback. The SIM emits cues by tag name at world positions; it neither
// knows nor cares whether anything listens (headless = zero handlers =
// zero work, the §2.10 invariant holds by construction). The RUNTIME
// registers handlers that resolve the tag through CueForms (data,
// moddable) and fire particles/sound/shake.
//
//   sim:      cues.emit({ "Cue.Hit.Slash", hitPos, damage });
//   runtime:  cues.addHandler([&](const CueEvent& e) {
//                 if (const auto* cue = table.find(e.tag)) { ...fx... }
//             });
//
// A mod ships a spell's LOOK as one CueForm — no C++ (the decided cue
// contract, MEADOWS-PLAN §F).
//
// HOW TO FILL (post-7/07, "vivant" vertical): standard handler set in the
// runtime (particles via fx::ParticleSim, sound via audio::AudioSystem,
// camera shake), emission points in combat (hits, blocks, deaths),
// effects (applyEffect gains an optional CueRegistry* for status cues),
// and footstep events from the anim graph.

namespace gameplay {

struct CueEvent {
    str tag;               // hierarchical name, e.g. "Cue.Hit.Slash"
    Vec3 position {};      // world position of the feedback
    f32 magnitude { 0.0f }; // damage amount, intensity...
    // Sneak & co: the emitter can soften its own feedback (sound volume
    // and pitch multipliers over the SoundForm's authored values).
    f32 volumeScale { 1.0f };
    f32 pitchScale { 1.0f };
};

class CueRegistry {
public:
    using Handler = std::function<void(const CueEvent&)>;

    u32 addHandler(Handler handler) {
        const u32 id = nextId++;
        handlers.emplace(id, std::move(handler));
        return id;
    }
    void removeHandler(u32 id) { handlers.erase(id); }

    void emit(const CueEvent& event) const {
        for (const auto& [id, handler] : handlers) {
            handler(event);
        }
    }
    bool empty() const { return handlers.empty(); }

private:
    std::unordered_map<u32, Handler> handlers;
    u32 nextId { 1 };
};

// Tag -> CueForm lookup with HIERARCHICAL fallback: "Cue.Hit.Slash" falls
// back to "Cue.Hit" then "Cue" — mods override the specific, the base
// game covers the generic. Build once after resolve.
class CueTable {
public:
    void build(const data::FormDatabase& forms);
    const data::CueForm* find(std::string_view tag) const;

private:
    std::unordered_map<str, const data::CueForm*> byTag;
};

// The ParticleForm -> sim mapping (chantier 8.10, the H7 seam filled):
// engine/fx never sees data:: (rule n°2) — the runtime layer maps here.
// Shape/rate/duration/texture/blend are emitter-loop and render-site
// concerns, deliberately NOT part of EmitterParams (Particles.hpp
// HOW TO FILL) — callers drive them (the FxPanel preview does).
fx::EmitterParams toEmitterParams(const data::ParticleForm& form);

} // namespace gameplay
