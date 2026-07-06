#include "game/LevelEditor.hpp"

#include <fstream>

#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

namespace {

u32 fieldId(const reflect::TypeInfo& type, const char* name) {
    const reflect::FieldInfo* field = type.findField(name);
    return field ? field->id : 0;
}

} // namespace

bool LevelEditor::commitTransform(const core::Guid& reference,
                                  const Vec3& position, const Quat& rotation,
                                  const Vec3& scale) {
    const reflect::TypeInfo* type = session.viewType(reference);
    if (!type || !type->isA(world::ReferenceForm::staticTypeInfo().id)) {
        return false;
    }
    bool ok = true;
    ok &= session.setField(reference, fieldId(*type, "position"),
                           reflect::Value { position });
    ok &= session.setField(reference, fieldId(*type, "rotation"),
                           reflect::Value { rotation });
    ok &= session.setField(reference, fieldId(*type, "scale"),
                           reflect::Value { scale });
    return ok;
}

core::Guid LevelEditor::placeReference(const core::Guid& baseForm,
                                       const core::Guid& cell,
                                       const Vec3& position) {
    const core::Guid id = session.createForm(
        world::ReferenceForm::staticTypeInfo().id, "placed");
    if (!id.isValid()) {
        return {};
    }
    const reflect::TypeInfo& type = world::ReferenceForm::staticTypeInfo();
    session.setField(id, fieldId(type, "baseForm"),
                     reflect::Value { baseForm });
    session.setField(id, fieldId(type, "cell"), reflect::Value { cell });
    session.setField(id, fieldId(type, "position"),
                     reflect::Value { position });
    return id;
}

bool LevelEditor::disableReference(const core::Guid& reference) {
    const reflect::TypeInfo* type = session.viewType(reference);
    if (!type || !type->isA(world::ReferenceForm::staticTypeInfo().id)) {
        return false;
    }
    return session.setField(reference, fieldId(*type, "enabled"),
                            reflect::Value { false });
}

core::Guid LevelEditor::createPrefabFromSelection(
    const vector<core::Guid>& references, const str& name) {
    // Collect the source reference views (drafts included).
    struct Source {
        core::Guid id;
        const world::ReferenceForm* form;
    };
    vector<Source> sources;
    Vec3 centroid { 0.0f };
    core::Guid firstCell {};
    for (const core::Guid& id : references) {
        const reflect::TypeInfo* type = session.viewType(id);
        if (!type ||
            !type->isA(world::ReferenceForm::staticTypeInfo().id)) {
            continue;
        }
        const auto* form =
            static_cast<const world::ReferenceForm*>(session.view(id));
        if (!form) {
            continue;
        }
        sources.push_back({ id, form });
        centroid += form->position;
        if (!firstCell.isValid()) {
            firstCell = form->cell;
        }
    }
    if (sources.size() < 2) {
        LOG_WARN("LevelEditor: prefab needs at least 2 references");
        return {};
    }
    centroid /= static_cast<f32>(sources.size());

    const core::Guid prefab = session.createForm(
        world::PrefabForm::staticTypeInfo().id, name);
    const reflect::TypeInfo& prefabType = world::PrefabForm::staticTypeInfo();
    session.setField(prefab, fieldId(prefabType, "displayName"),
                     reflect::Value { name });

    const reflect::TypeInfo& refType = world::ReferenceForm::staticTypeInfo();
    for (const Source& source : sources) {
        // Template child: transform RELATIVE to the centroid, no cell.
        const core::Guid child =
            session.createForm(refType.id, name + "_part");
        session.setField(child, fieldId(refType, "baseForm"),
                         reflect::Value { source.form->baseForm });
        session.setField(child, fieldId(refType, "prefab"),
                         reflect::Value { prefab });
        session.setField(child, fieldId(refType, "position"),
                         reflect::Value { source.form->position - centroid });
        session.setField(child, fieldId(refType, "rotation"),
                         reflect::Value { source.form->rotation });
        session.setField(child, fieldId(refType, "scale"),
                         reflect::Value { source.form->scale });
        // The original leaves the world (the prefab instance replaces it).
        disableReference(source.id);
    }

    // One placed instance of the group where the originals stood.
    return placeReference(prefab, firstCell, centroid);
}

void LevelEditor::addExportAsset(const core::Guid& id,
                                 const str& relativePath) {
    for (data::AssetEntry& entry : exportAssets) {
        if (entry.id == id) {
            entry.path = relativePath;
            return;
        }
    }
    exportAssets.push_back({ id, relativePath });
}

bool LevelEditor::exportTo(const std::filesystem::path& path,
                           const core::Guid& pluginId, const str& name) {
    data::Plugin plugin = session.exportPlugin(pluginId, name);
    plugin.assets = exportAssets;
    std::error_code errc;
    std::filesystem::create_directories(path.parent_path(), errc);
    std::ofstream file { path, std::ios::trunc };
    if (!file) {
        LOG_ERROR("LevelEditor: cannot write {}", path.string());
        return false;
    }
    file << data::writePluginToml(plugin, types);
    LOG_INFO("LevelEditor: exported {} record(s) to {}",
             plugin.records.size(), path.string());
    return static_cast<bool>(file);
}

} // namespace game
