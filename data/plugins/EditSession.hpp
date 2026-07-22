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

    // A brand-new form (guid minted here — authoring-time act). A valid
    // `imposedId` skips the mint: DERIVED identities (implicit cells,
    // §2.5 — cellGuidFor) must create under their deterministic guid so
    // every session/mod talks about the same record. Refused ({}) when a
    // draft already holds that guid; SHADOWING a base-visible form is
    // allowed — a live-materialized cell sits in the resolved database
    // but not in any plugin, and the export must still CREATE it.
    core::Guid createForm(u32 typeId, const str& editorId,
                          const core::Guid& imposedId = {});

    // A copy of an existing form (base or draft) under a new guid — the
    // GameDB "duplicate" tool. Every reflected field is
    // cloned; CHILD records are NOT duplicated (v1 — recursive clones are
    // the quest editor's job). Null guid when the source is unknown.
    core::Guid duplicateForm(const core::Guid& source, const str& editorId);

    // Removes a form CREATED this session (the editors' "delete node").
    // False on base records: a plugin cannot delete a record (§5), so the
    // UI only offers delete on session drafts. Undoable — the op snapshots
    // the draft so undo restores its edited field values, not defaults.
    bool removeCreated(const core::Guid& id);

    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }
    // Undo/redo work in GESTURES: every op recorded inside a live
    // Gesture shares one group, and undo/redo pop the WHOLE group —
    // '+ State' = create + setField, and a lone Ctrl+Z would un-parent
    // the node instead of removing it (a half-created orphan).
    // Ops recorded outside a gesture group alone.
    void undo();
    void redo();

    class Gesture {
    public:
        explicit Gesture(EditSession& session) : session { session } {
            session.activeGroup = session.nextGroup++;
        }
        ~Gesture() { session.activeGroup = 0; }
        Gesture(const Gesture&) = delete;
        Gesture& operator=(const Gesture&) = delete;

    private:
        EditSession& session;
    };

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
    // next resolve. Order: base handle order, then created
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
        u32 group { 0 };         // gesture id: undo/redo pop whole groups
    };

    // The gesture group for the op being recorded NOW.
    u32 groupForNewOp() { return activeGroup != 0 ? activeGroup : nextGroup++; }

    Draft* draftFor(const core::Guid& id); // clone-on-demand
    void apply(const EditOp& op, bool forward);

    const FormDatabase& base;
    const FormTypeRegistry& types;
    std::unordered_map<core::Guid, Draft> drafts;
    vector<EditOp> undoStack;
    vector<EditOp> redoStack;
    u32 nextGroup { 1 };
    u32 activeGroup { 0 }; // non-zero while a Gesture is alive
};

} // namespace data
