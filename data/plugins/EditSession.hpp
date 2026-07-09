#pragma once

#include <unordered_map>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Record.hpp"

// The editor-writes-plugins loop (horizontal pass H2). An EditSession is a
// mutable overlay above a RESOLVED FormDatabase: tools edit drafts, never
// the resolved forms (§2.2 stays intact — the runtime keeps reading the
// database). Exporting diffs the drafts against the base and emits an
// ORDINARY plugin (§5): the editor is just another plugin author. Changes
// apply on the next resolve (relaunch/reload); live re-resolve is a
// vertical (post-7/07).
//
// Undo/redo: every field edit stores its before/after Value; undo replays
// the inverse. Creations undo by dropping the draft.
//
// HOW TO FILL (post-7/07): reference/level editing goes through the SAME
// session (ReferenceForm drafts: move = setField position); "create prefab
// from selection" = createForm(PrefabForm) + setField(prefab) on copies.

namespace data {

class EditSession {
public:
    EditSession(const FormDatabase& base, const FormTypeRegistry& types)
        : base { base }, types { types } {}

    // The edited draft when one exists, else the base form. Null when the
    // guid is unknown to both.
    const Form* view(const core::Guid& id) const;
    const reflect::TypeInfo* viewType(const core::Guid& id) const;

    // Clones the base form into a draft on first edit. False when the guid
    // or field is unknown, or the value kind mismatches.
    bool setField(const core::Guid& id, u32 fieldId,
                  const reflect::Value& value);

    // A brand-new form (guid minted here — authoring-time act).
    core::Guid createForm(u32 typeId, const str& editorId);

    // A copy of an existing form (base or draft) under a new guid — the
    // GameDB "duplicate" tool (chantier 8.1). Every reflected field is
    // cloned; CHILD records are NOT duplicated (v1 — recursive clones are
    // the quest editor's job). Null guid when the source is unknown.
    core::Guid duplicateForm(const core::Guid& source, const str& editorId);

    // Removes a form CREATED this session (chantier 8.6 — "delete node").
    // False on base records: a plugin cannot delete a record (§5), so the
    // UI only offers delete on session drafts. Undoable — the op snapshots
    // the draft so undo restores its edited field values, not defaults.
    bool removeCreated(const core::Guid& id);

    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }
    void undo();
    void redo();

    bool isDirty(const core::Guid& id) const { return drafts.contains(id); }
    // Created THIS session (vs an edited base form) — what the graph
    // editors may delete (§5: a plugin cannot remove a base record).
    bool isCreated(const core::Guid& id) const {
        const auto it = drafts.find(id);
        return it != drafts.end() && it->second.created;
    }
    u32 dirtyCount() const { return static_cast<u32>(drafts.size()); }
    void discardAll();

    // Visits every form the session can see: the resolved base (a dirty
    // form is visited through its draft) plus the session-CREATED forms —
    // what tool lists must iterate so fresh records show up before the
    // next resolve (chantier 8.2). Order: base handle order, then created
    // drafts (unordered — tools sort by their own keys).
    template<typename Fn> // Fn(const Guid&, const Form&, const TypeInfo&)
    void forEachVisible(Fn&& fn) const {
        for (u32 i = 1; i <= base.count(); ++i) {
            const FormHandle handle { i };
            const Form* form = base.get(handle);
            const reflect::TypeInfo* type = base.typeOf(handle);
            if (!form || !type) {
                continue;
            }
            if (const auto it = drafts.find(form->id); it != drafts.end()) {
                fn(form->id, *it->second.form, *it->second.type);
            } else {
                fn(form->id, *form, *type);
            }
        }
        for (const auto& [id, draft] : drafts) {
            if (draft.created) {
                fn(id, *draft.form, *draft.type);
            }
        }
    }

    // The diff as an ordinary plugin: patch records carry only the fields
    // that differ from the base; created forms carry every field that
    // differs from the C++ defaults. Records sorted by guid (determinism).
    Plugin exportPlugin(const core::Guid& pluginId, const str& name) const;

private:
    struct Draft {
        uptr<Form> form;
        const reflect::TypeInfo* type { nullptr };
        bool created { false };
    };
    struct EditOp {
        core::Guid id;
        u32 fieldId { 0 };       // 0 = creation/removal op
        reflect::Value before;
        reflect::Value after;
        u32 typeId { 0 };        // creation/removal only
        str editorId;            // creation only
        core::Guid sourceId;     // duplication only: redo re-copies from it
        sptr<Form> snapshot;     // removal only: undo restores from it
    };

    Draft* draftFor(const core::Guid& id); // clone-on-demand
    void apply(const EditOp& op, bool forward);

    const FormDatabase& base;
    const FormTypeRegistry& types;
    std::unordered_map<core::Guid, Draft> drafts;
    vector<EditOp> undoStack;
    vector<EditOp> redoStack;
};

} // namespace data
