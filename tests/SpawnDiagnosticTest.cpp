#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"

// Hidden diagnostic (run explicitly): reconstructs the seed-1337 sandbox
// spawn exactly like setSandboxMode does, bakes its tile, and reports
// what actually sits there — the headless twin of "start the game and
// look around the spawn".
//   meadows-tests '-tc=spawn diagnostic*' -ns

using namespace render::terraingen;

TEST_CASE("spawn diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };

    // The setSandboxMode probe, verbatim.
    Vec3 start { 2600.0f, 0.0f, 0.0f };
    bool found = false;
    const f32 seaLevel = params.macro.seaLevel;
    for (f32 radius = 2600.0f; radius <= 24000.0f && !found;
         radius += 700.0f) {
        for (u32 step = 0; step < 16 && !found; ++step) {
            const f32 angle =
                radius * 0.0137f + static_cast<f32>(step) * 0.3927f;
            const f32 x = std::cos(angle) * radius;
            const f32 z = std::sin(angle) * radius;
            const f32 h =
                macroHeightAnalytic(controls, params.macro, x, z);
            if (h > seaLevel + 8.0f && h < 95.0f) {
                start = { x, h, z };
                found = true;
            }
        }
    }
    MESSAGE("probe: found=", found, " at (", start.x, ", ", start.z,
            "), analytic h=", start.y, ", sea=", seaLevel);

    const i32 tx = static_cast<i32>(std::floor(start.x / params.tileSize));
    const i32 tz = static_cast<i32>(std::floor(start.z / params.tileSize));
    MESSAGE("spawn tile: (", tx, ", ", tz, ")");
    const TileBakeResult baked = bakeTile(params, tx, tz);

    // Baked ground at the spawn.
    render::TerrainParams tp;
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(baked.region);
    tp.base = base;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = controlParams;
    sandbox->macro = params.macro;
    tp.sandbox = sandbox;
    const f32 bakedH = render::terrain::height(tp, start.x, start.z);
    MESSAGE("baked h at spawn = ", bakedH, " (analytic said ", start.y,
            ", delta ", bakedH - start.y, ")");
    if (bakedH < seaLevel) {
        MESSAGE("  -> UNDER THE SEA on the baked terrain");
    }

    // Water bodies over/near the spawn.
    u32 covering = 0;
    for (const Lake& lake : baked.lakes) {
        const bool inBbox = start.x >= lake.minX && start.x <= lake.maxX &&
                            start.z >= lake.minZ && start.z <= lake.maxZ;
        const f32 cx = (lake.minX + lake.maxX) * 0.5f;
        const f32 cz = (lake.minZ + lake.maxZ) * 0.5f;
        const f32 dist = std::hypot(cx - start.x, cz - start.z);
        if (inBbox || dist < 600.0f) {
            ++covering;
            MESSAGE("lake near spawn: level=", lake.level,
                    " bbox=(", lake.minX, ",", lake.minZ, ")-(",
                    lake.maxX, ",", lake.maxZ, ") dug=",
                    static_cast<int>(lake.dug), " cells=", lake.cells,
                    inBbox ? "  [SPAWN IN BBOX]" : "");
        }
    }
    MESSAGE("lakes on tile: ", baked.lakes.size(), " (", covering,
            " near spawn), rivers: ", baked.rivers.size());

    // The wetAt verdict the warmup uses, and where the spiral would go.
    render::WaterBodies bodies;
    bodies.seaLevel = seaLevel;
    for (const Lake& lake : baked.lakes) {
        render::LakeSurface surface;
        surface.level = lake.level;
        surface.minX = lake.minX;
        surface.minZ = lake.minZ;
        surface.maxX = lake.maxX;
        surface.maxZ = lake.maxZ;
        surface.maskWidth = lake.maskWidth;
        surface.maskHeight = lake.maskHeight;
        surface.maskTexel = lake.maskTexel;
        surface.mask = lake.mask;
        bodies.lakes.push_back(std::move(surface));
    }
    const auto wetAt = [&](f32 x, f32 z) {
        const f32 h = render::terrain::height(tp, x, z);
        if (h < seaLevel + 2.0f) {
            return true;
        }
        return render::terrain::waterSurfaceAt(bodies, x, z, h + 1.0f)
            .has_value();
    };
    const bool spawnWet = wetAt(start.x, start.z);
    MESSAGE("wetAt(spawn) = ", spawnWet);
    if (spawnWet) {
        for (f32 radius = 60.0f; radius <= 1500.0f; radius += 60.0f) {
            bool dry = false;
            for (u32 k = 0; k < 12 && !dry; ++k) {
                const f32 angle =
                    static_cast<f32>(k) * (6.2831853f / 12.0f);
                const f32 x = start.x + std::cos(angle) * radius;
                const f32 z = start.z + std::sin(angle) * radius;
                if (!wetAt(x, z)) {
                    MESSAGE("relocation would land at (", x, ", ", z,
                            "), h=", render::terrain::height(tp, x, z),
                            " (radius ", radius, ")");
                    dry = true;
                }
            }
            if (dry) {
                break;
            }
        }
    }
    CHECK(found);
}

// Second stage: reproduce the GAME's post-reconcile state around the
// relocated spawn and inspect the water column there.
//   meadows-tests '-tc=spawn diagnostic 2' -ns
TEST_CASE("spawn diagnostic 2" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    // The game's relocated spawn from the verified run log.
    const f32 px = 2583.0f;
    const f32 pz = 60.0f;

    const TileBakeResult a = bakeTile(params, 0, -1);
    const TileBakeResult b = bakeTile(params, 0, 0);
    render::TerrainParams tp;
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(a.region);
    base->regions.push_back(b.region);
    tp.base = base;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = params.controls;
    sandbox->controls.seed = params.worldSeed;
    sandbox->macro = params.macro;
    tp.sandbox = sandbox;

    vector<Lake> lakes = a.lakes;
    lakes.insert(lakes.end(), b.lakes.begin(), b.lakes.end());
    // The scene's reconcile, verbatim: clear mask cells whose REAL
    // ground pokes above the lake level.
    for (Lake& lake : lakes) {
        if (lake.mask.empty()) {
            continue;
        }
        u32 kept = 0;
        for (u32 row = 0; row < lake.maskHeight; ++row) {
            for (u32 col = 0; col < lake.maskWidth; ++col) {
                u8& cell =
                    lake.mask[static_cast<size_t>(row) * lake.maskWidth +
                              col];
                if (!cell) {
                    continue;
                }
                const f32 wx =
                    lake.minX + static_cast<f32>(col) * lake.maskTexel;
                const f32 wz =
                    lake.minZ + static_cast<f32>(row) * lake.maskTexel;
                if (render::terrain::height(tp, wx, wz) >
                    lake.level - 0.15f) {
                    cell = 0;
                    continue;
                }
                ++kept;
            }
        }
        lake.cells = kept;
    }

    const f32 ground = render::terrain::height(tp, px, pz);
    MESSAGE("relocated spawn (", px, ", ", pz, "): baked ground = ",
            ground, ", sea = ", params.macro.seaLevel);
    for (const Lake& lake : lakes) {
        if (px < lake.minX || px > lake.maxX || pz < lake.minZ ||
            pz > lake.maxZ) {
            continue;
        }
        // Mask cell under the point + wet cells nearby (the ribbon
        // quads grow runs by 0.75 texel — a wet neighbour renders a
        // sheet overhead even when THIS cell is dry).
        const u32 col = static_cast<u32>(
            glm::clamp((px - lake.minX) / lake.maskTexel + 0.5f, 0.0f,
                       static_cast<f32>(lake.maskWidth - 1)));
        const u32 row = static_cast<u32>(
            glm::clamp((pz - lake.minZ) / lake.maskTexel + 0.5f, 0.0f,
                       static_cast<f32>(lake.maskHeight - 1)));
        const u8 cell = lake.mask.empty()
                            ? 1
                            : lake.mask[static_cast<size_t>(row) *
                                            lake.maskWidth +
                                        col];
        u32 wetNear = 0;
        for (i32 dz = -2; dz <= 2; ++dz) {
            for (i32 dx = -2; dx <= 2; ++dx) {
                const i32 cx = static_cast<i32>(col) + dx;
                const i32 cz = static_cast<i32>(row) + dz;
                if (cx < 0 || cz < 0 ||
                    cx >= static_cast<i32>(lake.maskWidth) ||
                    cz >= static_cast<i32>(lake.maskHeight)) {
                    continue;
                }
                if (lake.mask.empty() ||
                    lake.mask[static_cast<size_t>(cz) * lake.maskWidth +
                              cx]) {
                    ++wetNear;
                }
            }
        }
        MESSAGE("lake level=", lake.level, " cells=", lake.cells,
                ": spawn IN BBOX, mask cell=", static_cast<int>(cell),
                ", wet cells within 2 texels=", wetNear,
                ", ground vs level: ", ground, " vs ", lake.level,
                (ground < lake.level ? "  [GROUND UNDER LEVEL]" : ""));
    }
    CHECK(true);
}

// Regime map: where the seed-1337 world puts its OLD MASSIFS (plateau
// wearing hills) and HILL CHAINS — teleport targets for visual review.
//   meadows-tests '-tc=regime diagnostic' -ns
TEST_CASE("regime diagnostic" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };

    struct Spot {
        f32 x, z, score, h;
    };
    vector<Spot> massifs;
    vector<Spot> chains;
    for (f32 z = -30000.0f; z <= 30000.0f; z += 500.0f) {
        for (f32 x = -30000.0f; x <= 30000.0f; x += 500.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                continue;
            }
            const f32 h =
                macroHeightAnalytic(controls, params.macro, x, z);
            if (s.plateau > 120.0f && s.hillRelief > 40.0f) {
                massifs.push_back({ x, z, s.plateau + s.hillRelief, h });
            } else if (s.hillRelief > 35.0f && s.plateau < 20.0f &&
                       s.uplift < 0.05f) {
                chains.push_back({ x, z, s.hillRelief, h });
            }
        }
    }
    const auto report = [](const char* label, vector<Spot>& spots) {
        std::sort(spots.begin(), spots.end(),
                  [](const Spot& a, const Spot& b) {
                      return a.score > b.score;
                  });
        u32 shown = 0;
        vector<Spot> kept;
        for (const Spot& spot : spots) {
            bool near = false;
            for (const Spot& other : kept) {
                if (std::hypot(spot.x - other.x, spot.z - other.z) <
                    4000.0f) {
                    near = true;
                    break;
                }
            }
            if (near) {
                continue;
            }
            kept.push_back(spot);
            MESSAGE(label, ": (", spot.x, ", ", spot.z, ")  score=",
                    spot.score, "  altitude~", spot.h);
            if (++shown >= 4) {
                break;
            }
        }
        MESSAGE(label, " samples total: ", spots.size());
    };
    report("OLD MASSIF", massifs);
    report("HILL CHAIN", chains);
    CHECK(true);
}
