#pragma once

#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

// Chantier ESPRITS: the nine spirits of the world as simulated FIELDS the
// player creates and steers. The one runtime type every spirit shares is
// the placed SOURCE — what an ability put in the world; the field is what
// the kernels derive from sources + rules + materials each tick. Only
// sources persist (as SpiritSourceForm records, world/worldspace).
//
// The nine spirits form three TRIADS, each a cycle keyed to the STATS
// resonance channels: Onyx (matter) > Amber (energy/time) > Garnet
// (spirit) > Onyx. Within a triad the kind order IS the channel order, so
// the cycle is a function of the enum, never data.

namespace render::terrain {

enum class SpiritKind : u8 {
    Vegetation = 0, // triad 1 — spirit Tree   (Onyx)
    Light,          //           spirit Moon   (Amber)
    Darkness,       //           spirit Black  (Garnet)
    Earth,          // triad 2 — spirit Hammer (Onyx)  + blunt
    Lightning,      //           spirit Blade  (Amber) + slashing
    Psy,            //           spirit Mirror (Garnet)+ piercing
    Water,          // triad 3 — water AND cold (Onyx)
    Fire,           //           flame          (Amber)
    Wind,           //           breath/cosmos  (Garnet)
    kCount
};

enum class SpiritChannel : u8 { Onyx = 0, Amber = 1, Garnet = 2 };

constexpr u32 spiritTriad(SpiritKind kind) {
    return static_cast<u32>(kind) / 3u;
}

constexpr SpiritChannel spiritChannel(SpiritKind kind) {
    return static_cast<SpiritChannel>(static_cast<u32>(kind) % 3u);
}

// The shifumi: within one triad, matter > energy > spirit > matter.
constexpr bool spiritDominates(SpiritKind a, SpiritKind b) {
    return spiritTriad(a) == spiritTriad(b) &&
           (static_cast<u32>(spiritChannel(a)) + 1u) % 3u ==
               static_cast<u32>(spiritChannel(b));
}

constexpr std::string_view spiritName(SpiritKind kind) {
    switch (kind) {
    case SpiritKind::Vegetation: return "Vegetation";
    case SpiritKind::Light: return "Light";
    case SpiritKind::Darkness: return "Darkness";
    case SpiritKind::Earth: return "Earth";
    case SpiritKind::Lightning: return "Lightning";
    case SpiritKind::Psy: return "Psy";
    case SpiritKind::Water: return "Water";
    case SpiritKind::Fire: return "Fire";
    case SpiritKind::Wind: return "Wind";
    case SpiritKind::kCount: break;
    }
    return "";
}

// kCount when the name matches no spirit (data validation).
constexpr SpiritKind spiritFromName(std::string_view name) {
    for (u32 i = 0; i < static_cast<u32>(SpiritKind::kCount); ++i) {
        const auto kind = static_cast<SpiritKind>(i);
        if (spiritName(kind) == name) {
            return kind;
        }
    }
    return SpiritKind::kCount;
}

struct SpiritSource {
    core::Guid id;   // minted at spawn, reused on every re-save
    SpiritKind kind { SpiritKind::Water };
    f32 x { 0.0f };  // world XZ (y re-derived from ground/water)
    f32 z { 0.0f };
    f32 dirX { 0.0f }; // wind / lightning: the aim forward at cast
    f32 dirZ { 0.0f };
    f32 rate { 0.0f };   // kind-specific: m³/s (water), heat/s (fire)...
    f32 radius { 0.0f }; // meters
    // SIM-seconds left (real dt x the sim time scale, so the volume a
    // spring pours is rate x duration at any dev time scale); < 0 =
    // permanent (an authored world spring).
    f32 remaining { -1.0f };
    // Mint order within its list — persisted so a reload keeps minting
    // fresh ids after the survivors.
    i32 sequence { 0 };
};

} // namespace render::terrain
