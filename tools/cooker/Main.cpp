// cooker — text <-> binary plugin tool (§3 on-disk formats).
//
//   cooker cook       <in.toml> <out.bin>   text  -> cooked binary
//   cooker uncook     <in.bin>  <out.toml>  cooked binary -> text
//   cooker new-guid   [count]               mint authoring guids
//   cooker import-csv <in.csv> <out.toml> <FormType> [pluginGuid]
//                     [--patch <targetPluginGuid>]
//                     spreadsheet -> ordinary plugin; with --patch,
//                     rows become §5 patches on the target plugin's records
//                     (language packs)
//   cooker validate   <plugin.toml...>       mod lint: loads
//                     the plugins IN ARGUMENT ORDER, resolves, and reports
//                     orphan patches, dependency violations, dangling guid
//                     references (reflection sweep) and field conflicts
//                     (informational — §5 layering). Exit 1 on errors.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "TextureCook.hpp"
#include "data/plugins/BinaryFormat.hpp"
#include "data/plugins/CsvImport.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "ErosionBench.hpp"
#include "TerrainMap.hpp"
#include "PreBakeTool.hpp"
#include "WaterReplayTool.hpp"
#include "WaterSolveTool.hpp"
#include "data/plugins/Validate.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "game/AllForms.hpp" // registerAllFormTypes — the single registration site
#include "game/WeaponMeshes.hpp" // runtimeMeshGuids (validate whitelist)
#include "world/terrain/TerrainPatches.hpp"

namespace {

int usage() {
    std::printf(
        "usage:\n"
        "  cooker cook   <in.toml> <out.bin>\n"
        "  cooker uncook <in.bin>  <out.toml>\n"
        "  cooker new-guid [count]\n"
        "  cooker terrain-pad <cx> <cz> <x0> <z0> <x1> <z1> <height> "
        "<out.ter>\n"
        "     flattens the rect [x0..x1]x[z0..z1] of terrain chunk "
        "(cx,cz)\n"
        "     to <height> m (3 m smooth blend ring), demo seed/params\n"
        "  cooker validate <plugin.toml...>\n"
        "     mod lint: resolve the plugins in argument order and report\n"
        "     orphan patches, dependency violations, dangling guid refs\n"
        "     and field conflicts (informational). Exit 1 on errors\n"
        "  cooker cook-terrain-materials <manifest.toml> <outDir>\n"
        "     PNG material maps -> the four terrain .mtex arrays\n"
        "     (albedo BC7, normal BC5, ORM BC7, height R16), 512^2,\n"
        "     offline mips. Source paths are manifest-relative\n"
        "  cooker terrain-map <seed> <centerX> <centerZ> <spanMeters> "
        "<out.png> [sizePx=1000] [carrierWavelength] [carrierAmp] "
        "[layout 0|1]\n"
        "     top-down PNG of the analytic sandbox macro (hypsometric\n"
        "     tints, hillshade, master rivers, center cross); carrier*\n"
        "     overrides the continental-carrier experiment knobs\n"
        "  cooker pre-bake <gameDir> <x0> <z0> <x1> <z1>\n"
        "  cooker pre-bake <gameDir> <centerX> <centerZ> <radius>\n"
        "     bakes the sandbox tiles overlapping the rect (or the\n"
        "     centered square) into <gameDir>/terrain-cache/<seed>,\n"
        "     with the bake params resolved from <gameDir>/data — the\n"
        "     game then streams the area with zero in-session bakes\n"
        "     and the far-water sees every lake in it\n"
        "  cooker import-csv <in.csv> <out.toml> <FormType> [pluginGuid]\n"
        "                    [--patch <targetPluginGuid>]\n"
        "     header row = reflected field names; row identity = the\n"
        "     'form' guid column, else derived from (pluginGuid,\n"
        "     editorId) — pass the SAME pluginGuid on re-imports (without\n"
        "     one it derives from <out.toml>'s filename). With --patch,\n"
        "     rows derive their identity from the TARGET plugin instead\n"
        "     and become field patches (language packs)\n");
    return 2;
}

// Authored-terrain helper: computes the delta grid that
// levels a rectangle of the DEMO terrain (default TerrainParams — matches
// landscape.toml) to a target height, feathered over 3 m. The output .ter
// ships as a plugin asset referenced by a TerrainPatchForm.
int terrainPad(char** argv) {
    const i32 cx = std::atoi(argv[2]);
    const i32 cz = std::atoi(argv[3]);
    const f32 x0 = static_cast<f32>(std::atof(argv[4]));
    const f32 z0 = static_cast<f32>(std::atof(argv[5]));
    const f32 x1 = static_cast<f32>(std::atof(argv[6]));
    const f32 z1 = static_cast<f32>(std::atof(argv[7]));
    const f32 target = static_cast<f32>(std::atof(argv[8]));
    constexpr f32 kChunk = 64.0f;
    constexpr u32 kSamples = 65; // 1 m grid, shared edges
    constexpr f32 kBlend = 3.0f;

    render::TerrainParams params; // demo defaults (seed 1337)
    render::HeightPatch patch;
    patch.samples = kSamples;
    patch.deltas.resize(static_cast<size_t>(kSamples) * kSamples, 0.0f);
    for (u32 row = 0; row < kSamples; ++row) {
        for (u32 col = 0; col < kSamples; ++col) {
            const f32 x = static_cast<f32>(cx) * kChunk +
                          static_cast<f32>(col);
            const f32 z = static_cast<f32>(cz) * kChunk +
                          static_cast<f32>(row);
            // Distance outside the rect (0 inside), feathered to kBlend.
            const f32 dx = std::max({ x0 - x, 0.0f, x - x1 });
            const f32 dz = std::max({ z0 - z, 0.0f, z - z1 });
            const f32 outside = std::sqrt(dx * dx + dz * dz);
            if (outside >= kBlend) {
                continue;
            }
            const f32 t = 1.0f - outside / kBlend;
            const f32 weight = t * t * (3.0f - 2.0f * t); // smoothstep
            const f32 base = render::terrain::height(params, x, z);
            patch.deltas[row * kSamples + col] = (target - base) * weight;
        }
    }
    if (!world::writeTerFile(argv[9], patch)) {
        return 1;
    }
    LOG_INFO("terrain-pad: chunk ({}, {}) rect [{},{}]x[{},{}] -> {} m, "
             "wrote {}",
             cx, cz, x0, z0, x1, z1, target, argv[9]);
    return 0;
}

std::optional<vector<u8>> readFileBytes(const std::filesystem::path& path) {
    std::ifstream file { path, std::ios::binary | std::ios::ate };
    if (!file) {
        return std::nullopt;
    }
    const auto size = file.tellg();
    vector<u8> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return file ? std::optional { std::move(bytes) } : std::nullopt;
}

bool writeFileBytes(const std::filesystem::path& path,
                    std::span<const u8> bytes) {
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

int cook(const char* inPath, const char* outPath,
         const data::FormTypeRegistry& types) {
    const auto plugin = data::loadPluginFile(inPath, types);
    if (!plugin) {
        LOG_ERROR("cook failed: {}", plugin.error()); // the reason, not just "failed"
        return 1;
    }
    const vector<u8> bytes = data::writePluginBinary(*plugin);
    if (!writeFileBytes(outPath, bytes)) {
        LOG_ERROR("Cannot write {}", outPath);
        return 1;
    }
    LOG_INFO("Cooked {} -> {} ({} records, {} bytes)", inPath, outPath,
             plugin->records.size(), bytes.size());
    return 0;
}

// Spreadsheet -> ordinary plugin. The default plugin guid derives
// from the output FILENAME so re-running the same import command yields
// the same row identities; an explicit guid overrides (recommended once
// the plugin ships — renaming the file then can't shift identities).
int importCsv(const char* inPath, const char* outPath, const char* typeName,
              const char* pluginGuidText, const char* patchTargetText,
              const data::FormTypeRegistry& types) {
    const reflect::TypeInfo* type = types.findType(typeName);
    if (!type) {
        LOG_ERROR("import-csv: unknown form type '{}'", typeName);
        return 1;
    }
    core::Guid pluginId;
    if (pluginGuidText != nullptr) {
        const auto parsed = core::Guid::fromString(pluginGuidText);
        if (!parsed) {
            LOG_ERROR("import-csv: malformed plugin guid '{}'",
                      pluginGuidText);
            return 1;
        }
        pluginId = *parsed;
    } else {
        const str stem = std::filesystem::path { outPath }.stem().string();
        pluginId = data::csvRowGuid(core::Guid { 0x6d656164u, 0x6f777321u },
                                    "csv-plugin:" + stem);
        LOG_INFO("import-csv: plugin guid {} (derived from '{}' — pass it "
                 "explicitly to survive a rename)",
                 pluginId.toString(), stem);
    }
    core::Guid patchTarget;
    if (patchTargetText != nullptr) {
        const auto parsed = core::Guid::fromString(patchTargetText);
        if (!parsed) {
            LOG_ERROR("import-csv: malformed --patch target guid '{}'",
                      patchTargetText);
            return 1;
        }
        patchTarget = *parsed;
    }
    std::ifstream file { inPath, std::ios::binary };
    if (!file) {
        LOG_ERROR("Cannot read {}", inPath);
        return 1;
    }
    std::ostringstream content;
    content << file.rdbuf();
    const auto plugin =
        data::importCsv(content.str(), *type, pluginId,
                        std::filesystem::path { inPath }.filename().string(),
                        patchTarget);
    if (!plugin) {
        LOG_ERROR("import-csv failed: {}", plugin.error());
        return 1;
    }
    const str toml = data::writePluginToml(*plugin, types);
    std::ofstream out { outPath, std::ios::trunc };
    out << toml;
    if (!out) {
        LOG_ERROR("Cannot write {}", outPath);
        return 1;
    }
    LOG_INFO("Imported {} -> {} ({} records of {})", inPath, outPath,
             plugin->records.size(), typeName);
    return 0;
}

// Mod lint: the reusable data::validatePlugins
// pass behind a CLI a mod CI can gate on.
int validate(int argc, char** argv, const data::FormTypeRegistry& types) {
    vector<data::Plugin> plugins;
    plugins.reserve(static_cast<size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) {
        auto plugin = data::loadPluginFile(argv[i], types);
        if (!plugin) {
            LOG_ERROR("validate: cannot load {}: {}", argv[i],
                      plugin.error());
            return 1;
        }
        plugins.push_back(std::move(*plugin));
    }
    vector<const data::Plugin*> loadOrder;
    loadOrder.reserve(plugins.size());
    for (const data::Plugin& plugin : plugins) {
        loadOrder.push_back(&plugin);
    }
    // The engine injects procedural meshes under fixed guids at runtime
    // (sword/club/bow/arrow/pony): data legitimately references them
    // though no plugin declares them as assets.
    const data::ValidationReport report =
        data::validatePlugins(loadOrder, types, game::runtimeMeshGuids());

    LOG_INFO("validate: {} plugins, {} forms created, {} records applied",
             plugins.size(), report.resolve.formsCreated,
             report.resolve.recordsApplied);
    if (report.resolve.recordsSkipped > 0) {
        LOG_WARN("  {} records SKIPPED (unknown type / type mismatch)",
                 report.resolve.recordsSkipped);
    }
    if (report.resolve.orphanPatches > 0) {
        LOG_ERROR("  {} ORPHAN patches (target guid never created)",
                  report.resolve.orphanPatches);
    }
    if (report.resolve.dependencyViolations > 0) {
        LOG_ERROR("  {} DEPENDENCY violations (missing or loaded after "
                  "their dependent)",
                  report.resolve.dependencyViolations);
    }
    for (const data::DanglingReference& ref : report.danglingRefs) {
        LOG_ERROR("  dangling ref: {} ({}) field '{}' -> {}",
                  ref.form.toString(), ref.typeName, ref.fieldName,
                  ref.target.toString());
    }
    // Conflicts are §5 layering facts, not errors — the synthesis tool
    // (§5.1) arbitrates them; here they are surfaced for information.
    for (const data::FieldConflict& conflict : report.resolve.conflicts) {
        str writers;
        for (const data::FieldWrite& write : conflict.writers) {
            writers += writers.empty() ? write.plugin : " -> " + write.plugin;
        }
        LOG_INFO("  conflict: {} ({}) field '{}': {}",
                 conflict.formId.toString(), conflict.typeName,
                 conflict.fieldName, writers);
    }
    if (report.hasErrors()) {
        LOG_ERROR("validate: FAILED ({} dangling refs)",
                  report.danglingRefs.size());
        return 1;
    }
    LOG_INFO("validate: OK ({} field conflicts, informational)",
             report.resolve.conflicts.size());
    return 0;
}

int uncook(const char* inPath, const char* outPath,
           const data::FormTypeRegistry& types) {
    const auto bytes = readFileBytes(inPath);
    if (!bytes) {
        LOG_ERROR("Cannot read {}", inPath);
        return 1;
    }
    const auto plugin = data::readPluginBinary(*bytes, inPath);
    if (!plugin) {
        return 1;
    }
    const str toml = data::writePluginToml(*plugin, types);
    std::ofstream file { outPath, std::ios::trunc };
    file << toml;
    if (!file) {
        LOG_ERROR("Cannot write {}", outPath);
        return 1;
    }
    LOG_INFO("Uncooked {} -> {} ({} records)", inPath, outPath,
             plugin->records.size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    core::Log::init();

    if (argc < 2) {
        return usage();
    }
    const std::string_view command = argv[1];

    if (command == "new-guid") {
        int count = 1;
        if (argc >= 3) {
            // atoi returns 0 on garbage and anything on overflow — bound it
            // instead of looping on whatever the shell passed.
            count = std::atoi(argv[2]);
            if (count < 1 || count > 1000) {
                std::fprintf(stderr,
                             "new-guid: count must be 1..1000 (got '%s')\n",
                             argv[2]);
                return 1;
            }
        }
        for (int i = 0; i < count; ++i) {
            std::printf("%s\n", core::Guid::generate().toString().c_str());
        }
        return 0;
    }

    // EVERY form family, or the cooker silently drops records (unknown types
    // skip with a warning). Single source of truth: game::registerAllFormTypes
    // — the same aggregator the game exe uses, compiled into the cooker (see
    // tools/CMakeLists.txt). A family added there is cooked here for free; the
    // two can no longer drift.
    data::FormTypeRegistry types;
    game::registerAllFormTypes(types);

    if (command == "cook" && argc == 4) {
        return cook(argv[2], argv[3], types);
    }
    if (command == "uncook" && argc == 4) {
        return uncook(argv[2], argv[3], types);
    }
    if (command == "validate" && argc >= 3) {
        return validate(argc, argv, types);
    }
    if (command == "import-csv" && (argc == 5 || argc == 6)) {
        return importCsv(argv[2], argv[3], argv[4],
                         argc == 6 ? argv[5] : nullptr, nullptr, types);
    }
    if (command == "import-csv" && argc == 8 &&
        std::string_view { argv[6] } == "--patch") {
        return importCsv(argv[2], argv[3], argv[4], argv[5], argv[7], types);
    }
    if (command == "terrain-pad" && argc == 10) {
        return terrainPad(argv);
    }
    if (command == "cook-terrain-materials" && argc == 4) {
        return cooker::cookTerrainMaterials(argv[2], argv[3]);
    }
    if (command == "terrain-map" && argc >= 7 && argc <= 11) {
        return cooker::terrainMap(argv, argc);
    }
    if (command == "erosion-bench" && (argc == 6 || argc == 7)) {
        return cooker::erosionBench(argv, argc);
    }
    if (command == "water-solve" && argc >= 6 && argc <= 8) {
        return cooker::waterSolve(argv, argc);
    }
    if (command == "water-replay" && (argc == 4 || argc == 5)) {
        return cooker::waterReplay(argv, argc);
    }
    if (command == "pre-bake" && (argc == 6 || argc == 7)) {
        return cooker::preBake(argv, argc);
    }
    return usage();
}
