#pragma once

#include <filesystem>
#include <unordered_set>

#include "data/plugins/EditSession.hpp"
#include "engine/core/Defines.hpp"

namespace world {
class WorldModel;
}

// The level editor's OPERATIONS (chantier 2 B3/B4) — pure record edits
// through an EditSession (§5: the editor is just another plugin author),
// no ImGui/gizmo code here (the scene owns interaction and rendering).
// Replaces the pre-EditSession WorldEditor embryo.
//
// Live entities are the CALLER's concern: after an op, the scene updates
// or respawns the affected entities (records are the truth; the world is
// a projection).

namespace game {

class LevelEditor {
public:
    LevelEditor(const data::FormDatabase& forms,
                const data::FormTypeRegistry& types)
        : session { forms, types }, forms { forms }, types { types } {}

    data::EditSession& editSession() { return session; }

    // Selection (single + a multi-set for prefab grouping).
    const core::Guid& selected() const { return selectedRef; }
    void select(const core::Guid& reference) { selectedRef = reference; }
    vector<core::Guid>& groupSelection() { return group; }

    // Writes a gizmo-committed transform into the ReferenceForm draft
    // (three field edits — one undo step each, v1).
    bool commitTransform(const core::Guid& reference, const Vec3& position,
                         const Quat& rotation, const Vec3& scale);

    // Palette placement: a new ReferenceForm draft in `cell` (0 = none).
    core::Guid placeReference(const core::Guid& baseForm,
                              const core::Guid& cell, const Vec3& position);

    // Duplicates a placed reference (chantier 2 leftover, 2026-07-13):
    // session.duplicateForm clones every reflected field (the 8.1 tool),
    // the offset nudges the copy so it never hides its original. ONE undo
    // gesture; the copy becomes the selection. Returns 0 on a non-ref.
    core::Guid duplicateReference(const core::Guid& reference,
                                  const Vec3& offset);

    // Implicit cells (IMPLICIT-CELLS brick 2): "place anywhere". Ensures
    // grid square (gx, gy) of `worldspace` exists BOTH live
    // (WorldModel::materializeCell — the streamer starts resolving it)
    // and in the EditSession (a created CellForm under the deterministic
    // guid, §2.5 — the export must SHIP the cell its references point
    // to, since the live form exists only in this session's memory).
    // Authored cells pass through untouched; re-records after an undo
    // dropped the draft. `db` is the SAME database the session reads —
    // mutable here because materialization is a live act. Returns the
    // cell guid (invalid when `worldspace` is not one).
    core::Guid ensureCell(world::WorldModel& model, data::FormDatabase& db,
                          data::FormHandle worldspace, i32 gx, i32 gy);

    // Disable (works for base AND session-created references; exports as
    // an enabled=false patch either way).
    bool disableReference(const core::Guid& reference);

    // "Create prefab from selection": mints a PrefabForm, copies every
    // selected reference as a TEMPLATE child (transform relative to the
    // selection centroid), disables the originals, and places ONE instance
    // of the prefab at the centroid (first reference's cell). Returns the
    // guid of the PLACED INSTANCE reference (0 on failure).
    core::Guid createPrefabFromSelection(const vector<core::Guid>& references,
                                         const str& name);

    // Registers an asset file for the export (sculpted .ter grids...);
    // same-guid entries are replaced. Paths are relative to the mod file.
    void addExportAsset(const core::Guid& id, const str& relativePath);

    // Exports the session diff (+ registered assets) as an ordinary mod,
    // loaded by the scene on the next run.
    bool exportTo(const std::filesystem::path& path,
                  const core::Guid& pluginId, const str& name);

private:
    data::EditSession session;
    const data::FormDatabase& forms;
    const data::FormTypeRegistry& types;
    core::Guid selectedRef {};
    vector<core::Guid> group;
    vector<data::AssetEntry> exportAssets;
    // Cells ensureCell materialized THIS session: once live, the database
    // can no longer tell them from authored cells, and the session record
    // may be undone away — this set is what keeps re-placement re-recording.
    std::unordered_set<core::Guid> materializedCells;
};

} // namespace game
