#pragma once

#include <functional>
#include <optional>

#include "engine/core/Defines.hpp"

namespace core {

// Stable 128-bit identity for plugins, forms, and assets (§2.5). GUIDs only
// live at the boundaries (files, manifests, mod references); runtime code
// resolves them once into compact handles at load.
struct Guid {
    u64 hi { 0 };
    u64 lo { 0 };

    // Random (v4). Generation is an authoring-time act: the editor or the
    // cooker mints GUIDs; the engine only reads them back.
    static Guid generate();

    // Canonical lowercase 8-4-4-4-12 form, e.g.
    // "1b4e28ba-2fa1-41d2-883f-0016d3cca427".
    str toString() const;
    static std::optional<Guid> fromString(std::string_view text);

    // Deterministic derived identity: mixes two guids into a third,
    // stable across runs, platforms and load orders. THE prefab contract:
    // child = combine(placedInstanceId, templateChildId), so saves and
    // patches can target one child of one placed prefab forever. Keeps
    // the v4 version/variant bits so derived guids stay well-formed.
    static Guid combine(const Guid& a, const Guid& b);

    bool isValid() const { return hi != 0 || lo != 0; }

    bool operator==(const Guid&) const = default;
    auto operator<=>(const Guid&) const = default;
};

} // namespace core

template<>
struct std::hash<core::Guid> {
    size_t operator()(const core::Guid& guid) const noexcept {
        // hi and lo are already uniformly random; mixing is enough.
        return guid.hi ^ (guid.lo * 0x9E3779B97F4A7C15ull);
    }
};
