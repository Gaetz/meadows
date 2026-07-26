#include "game/scenes/TreeCreationScene.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <imgui.h>

#include "data/forms/FormQuery.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/Engine.hpp"
#include "engine/FrameContext.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "game/AllForms.hpp"
#include "game/SceneStack.hpp"
#include "game/scenes/LandscapeScene.hpp"
#include "game/scenes/RenderTuningIo.hpp"
#include "game/ui/RenderTuningPanels.hpp"

namespace game {

namespace {

// The tree-types plugin layer (mods/tree-types.toml) — a sibling of the
// render-tuning overlay (f2) and the level edits.
const core::Guid& treeTypesPluginGuid() {
    static const core::Guid guid =
        *core::Guid::fromString("aaaaaaaa-0000-4000-8000-0000000000f4");
    return guid;
}

} // namespace

void TreeCreationScene::onEnter() {
    // Own plugin stack (the scene is independent of LandscapeScene —
    // it REPLACED it). No settings/save layer: data only.
    formTypes = data::FormTypeRegistry {};
    registerAllFormTypes(formTypes);
    const auto dataDir = platform::executableDir() / "data";
    data::PluginConfig config;
    if (const auto loaded =
            data::loadPluginConfigFile(dataDir / "plugins.toml")) {
        config = *loaded;
    } else {
        config = data::defaultConfigFromDirectory(dataDir / "base");
        for (auto& entry : config.entries) {
            entry.file = "base/" + entry.file;
        }
    }
    pluginStack = data::loadPluginStack(dataDir, config, formTypes);
    forms = data::FormDatabase {};
    data::resolve(data::pointersOf(pluginStack), formTypes, forms);
    loadLibrary();

    // Flat ground: zero relief, the sea/sand band pushed far below —
    // the "plane" is ordinary terrain, so splat + shadows just work.
    render::TerrainParams& params = renderer.terrainParams();
    params.hillAmplitude = 0.0f;
    params.mountainAmplitude = 0.0f;
    params.seaLevel = -100.0f;
    applySelected();

    // The R3 opt-in for real: no water, no GI, no froxels, no occlusion.
    // postFx stays ON — the postFx-less blit fallback is unproven on
    // Vulkan (unbound tonemap samplers); hardening it is a follow-up.
    rhi::Device& device = engine->getDevice();
    renderer.create(device, engine->getJobSystem(),
                    render::RendererConfig { .terrain = true,
                                             .water = false,
                                             .sky = true,
                                             .vegetation = true,
                                             .grass = true,
                                             .gi = false,
                                             .froxels = false,
                                             .occlusion = false,
                                             .postFx = true });

    // The showcased specimen: variant 0 at the origin, no distance fade.
    const f32 ground = render::terrain::height(params, 0.0f, 0.0f);
    renderer.vegetationSystem().setShowcase(
        device, { { .positionScale = { 0.0f, ground, 0.0f, 1.0f },
                    .params = { 0.0f, 0.5f, 0.0f, 1.0e6f } } });

    flyCamera.camera.position = { 7.0f, ground + 4.0f, 11.0f };
    flyCamera.camera.yaw = glm::radians(-31.5f); // face the specimen
    flyCamera.camera.pitch = glm::radians(-12.0f);
    renderReady = true;
}

void TreeCreationScene::onExit() {
    if (renderReady) {
        renderer.destroy(engine->getDevice());
        renderReady = false;
    }
}

void TreeCreationScene::update(f32 dt) {
    frameProbe.beginFrame(); // ends in render() — one probe per frame
    timeSeconds += dt;
    windTime += dt * atmos.windStrength;
    flyCamera.update(engine->getInput(), engine->getWindow(), dt,
                     !ImGui::GetIO().WantCaptureMouse,
                     render::FlyCamera::LookTrigger::RightOrAltLeft);
}

void TreeCreationScene::render(engine::FrameContext& frame) {
    const render::RenderSnapshot snapshot; // no world: the showcase only
    const render::RenderView view {
        .camera = flyCamera.camera,
        .atmos = atmos,
        .timeSeconds = timeSeconds,
        .windTime = windTime,
        .snowLine = 1000.0f, // flat ground stays below every band
        .probe = &frameProbe,
    };
    renderer.render(frame, snapshot, view);
    frameProbe.endFrame();
}

void TreeCreationScene::loadLibrary() {
    library.clear();
    data::forEach<data::ColonizedTreeTuningForm>(
        forms, [&](const data::ColonizedTreeTuningForm& form) {
            if (form.id == data::colonizedTreeTuningGuid()) {
                return; // the forest singleton is not a library entry
            }
            library.push_back({ form.id, form.editorId, true,
                                data::LobeTreeTuningForm {}, form });
        });
    data::forEach<data::LobeTreeTuningForm>(
        forms, [&](const data::LobeTreeTuningForm& form) {
            if (form.id == data::lobeTreeTuningGuid()) {
                return;
            }
            library.push_back({ form.id, form.editorId, false, form,
                                data::ColonizedTreeTuningForm {} });
        });
    std::sort(library.begin(), library.end(),
              [](const TreeType& a, const TreeType& b) {
                  return a.name < b.name;
              });
    if (library.empty()) {
        // First run: the current forest tree becomes the first type.
        TreeType seed;
        seed.id = core::Guid::generate();
        seed.name = "default";
        seed.colonized = true;
        seed.lobes = data::resolveLobeTreeTuning(forms);
        seed.colonizedParams = data::resolveColonizedTreeTuning(forms);
        library.push_back(std::move(seed));
        saveLibrary();
    }
    selected = 0;
}

void TreeCreationScene::saveLibrary() {
    // The saveRenderTuning pattern: one ordinary plugin, rewritten whole
    // (deleting a type = the record simply no longer exists in OUR layer;
    // base records are never touched, §5).
    data::Plugin plugin;
    plugin.id = treeTypesPluginGuid();
    plugin.name = "tree-types";
    const auto createRecord = [&](const core::Guid& guid, data::Form& form,
                                  const str& name,
                                  const reflect::TypeInfo& type) {
        form.editorId = name;
        data::Record record;
        record.formId = guid;
        record.typeId = type.id;
        record.creates = true;
        reflect::forEachField(type, [&](const reflect::FieldInfo& field) {
            if ((field.flags & reflect::Transient) != 0) {
                return;
            }
            record.fields[field.id] = field.get(&form);
        });
        plugin.records.push_back(std::move(record));
    };
    for (TreeType& type : library) {
        if (type.colonized) {
            createRecord(type.id, type.colonizedParams, type.name,
                         data::ColonizedTreeTuningForm::staticTypeInfo());
        } else {
            createRecord(type.id, type.lobes, type.name,
                         data::LobeTreeTuningForm::staticTypeInfo());
        }
    }
    const auto path =
        platform::executableDir() / "data" / "mods" / "tree-types.toml";
    std::error_code errc;
    std::filesystem::create_directories(path.parent_path(), errc);
    std::ofstream file { path, std::ios::trunc };
    if (!file) {
        LOG_ERROR("Tree types: cannot write {}", path.string());
        return;
    }
    file << data::writePluginToml(plugin, formTypes);
    LOG_INFO("Tree types saved: {} type(s) -> {}", library.size(),
             path.string());
}

void TreeCreationScene::applySelected() {
    const TreeType& type = library[static_cast<size_t>(selected)];
    RenderTuningIo::applyTreeTuning(renderer, type.lobes,
                                    type.colonizedParams);
    renderer.vegetationSystem().colonizationTrees = type.colonized;
    renderer.requestReseedVegetation();
}

void TreeCreationScene::captureIntoSelected() {
    TreeType& type = library[static_cast<size_t>(selected)];
    RenderTuningIo::captureTreeTuning(renderer, type.lobes,
                                      type.colonizedParams);
    type.colonized = renderer.vegetationSystem().colonizationTrees;
}

void TreeCreationScene::drawUi() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420.0f, display.y),
                             ImGuiCond_FirstUseEver);
    ImGui::Begin("Tree creation");

    // Leaving reloads the world from scratch (the strip REPLACED it —
    // no warm overlay, by design: the tool frees the world's memory).
    if (ImGui::Button("Back: world (reloads)")) {
        host()->replace(std::make_unique<LandscapeScene>(*engine));
    }
    ImGui::Separator();

    // --- Library ---------------------------------------------------------
    ImGui::SeparatorText("Tree types");
    for (i32 i = 0; i < static_cast<i32>(library.size()); ++i) {
        const TreeType& type = library[static_cast<size_t>(i)];
        const str label = type.name +
                          (type.colonized ? "  (colonization)" : "  (lobes)") +
                          "##type" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), selected == i) &&
            selected != i) {
            selected = i;
            applySelected();
        }
    }
    ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer));
    if (ImGui::Button("New from current")) {
        TreeType type;
        type.id = core::Guid::generate();
        type.name = nameBuffer[0] != '\0'
                        ? str { nameBuffer }
                        : "tree-" + std::to_string(library.size() + 1);
        library.push_back(std::move(type));
        selected = static_cast<i32>(library.size()) - 1;
        captureIntoSelected(); // current live params become the new type
        saveLibrary();
        nameBuffer[0] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button("Save type")) {
        captureIntoSelected();
        saveLibrary();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(library.size() <= 1); // at least one type stays
    if (ImGui::Button("Delete")) {
        library.erase(library.begin() + selected);
        selected = glm::min(selected,
                            static_cast<i32>(library.size()) - 1);
        applySelected();
        saveLibrary();
    }
    ImGui::EndDisabled();

    // --- Generation ------------------------------------------------------
    ImGui::SeparatorText("Generation");
    render::VegetationSystem& vegetation = renderer.vegetationSystem();
    int algorithm = vegetation.colonizationTrees ? 0 : 1;
    if (ImGui::Combo("Algorithm", &algorithm,
                     "Space colonization\0Lobe trees\0")) {
        vegetation.colonizationTrees = algorithm == 0;
        renderer.requestReseedVegetation();
    }
    // Specimen seed: full regenerate (the terrain-panel flow) — the flat
    // ground is seed-independent, only the tree changes.
    ImGui::InputScalar("Specimen seed", ImGuiDataType_U32,
                       &renderer.terrainParams().seed);
    ImGui::SameLine();
    if (ImGui::Button("Reroll")) {
        renderer.requestRegenerate();
    }
    if (RenderTuningPanels::drawTreeKnobs(renderer)) {
        renderer.requestReseedVegetation();
    }

    ImGui::End();
}

} // namespace game
