#include "data/plugins/EditSession.hpp"

#include <algorithm>

#include "data/plugins/RecordDiff.hpp"
#include "engine/core/Log.hpp"

namespace data {

namespace {

// Clones every reflected field (parents first) from src into dst.
void copyFields(const reflect::TypeInfo& type, const Form& src, Form& dst) {
    reflect::forEachField(type, [&](const reflect::FieldInfo& field) {
        field.set(&dst, field.get(&src));
    });
}

} // namespace

const Form* EditSession::view(const core::Guid& id) const {
    const auto it = drafts.find(id);
    if (it != drafts.end()) {
        return it->second.form.get();
    }
    return base.find(id);
}

const reflect::TypeInfo* EditSession::viewType(const core::Guid& id) const {
    const auto it = drafts.find(id);
    if (it != drafts.end()) {
        return it->second.type;
    }
    const FormHandle handle = base.handleOf(id);
    return handle.isValid() ? base.typeOf(handle) : nullptr;
}

EditSession::Draft* EditSession::draftFor(const core::Guid& id) {
    if (const auto it = drafts.find(id); it != drafts.end()) {
        return &it->second;
    }
    const FormHandle handle = base.handleOf(id);
    if (!handle.isValid()) {
        return nullptr;
    }
    const reflect::TypeInfo* type = base.typeOf(handle);
    const Form* source = base.get(handle);
    uptr<Form> clone = types.instantiate(type->id);
    if (!clone) {
        LOG_ERROR("EditSession: no factory for type '{}'", type->name);
        return nullptr;
    }
    clone->id = source->id;
    copyFields(*type, *source, *clone);
    Draft draft { std::move(clone), type, /*created=*/false };
    return &drafts.emplace(id, std::move(draft)).first->second;
}

bool EditSession::setField(const core::Guid& id, u32 fieldId,
                           const reflect::Value& value) {
    Draft* draft = draftFor(id);
    if (!draft) {
        return false;
    }
    const reflect::FieldInfo* field = draft->type->findField(fieldId);
    if (!field) {
        return false;
    }
    const reflect::Value before = field->get(draft->form.get());
    if (!field->set(draft->form.get(), value)) {
        return false; // kind mismatch — untrusted input, caller's log
    }
    EditOp op { id, fieldId, before, value, 0, {} };
    op.group = groupForNewOp();
    undoStack.push_back(std::move(op));
    redoStack.clear();
    return true;
}

core::Guid EditSession::createForm(u32 typeId, const str& editorId) {
    uptr<Form> form = types.instantiate(typeId);
    if (!form) {
        return {};
    }
    const core::Guid id = core::Guid::generate();
    form->id = id;
    form->editorId = editorId;
    drafts.emplace(id, Draft { std::move(form), types.findType(typeId),
                               /*created=*/true });
    EditOp op { id, 0, {}, {}, typeId, editorId };
    op.group = groupForNewOp();
    undoStack.push_back(std::move(op));
    redoStack.clear();
    return id;
}

core::Guid EditSession::duplicateForm(const core::Guid& source,
                                      const str& editorId) {
    const Form* src = view(source);
    const reflect::TypeInfo* type = viewType(source);
    if (!src || !type) {
        return {};
    }
    uptr<Form> form = types.instantiate(type->id);
    if (!form) {
        LOG_ERROR("EditSession: no factory for type '{}'", type->name);
        return {};
    }
    copyFields(*type, *src, *form);
    const core::Guid id = core::Guid::generate();
    form->id = id;
    form->editorId = editorId; // after copyFields: editorId is reflected
    drafts.emplace(id, Draft { std::move(form), type, /*created=*/true });
    EditOp op { id, 0, {}, {}, type->id, editorId, source };
    op.group = groupForNewOp();
    undoStack.push_back(std::move(op));
    redoStack.clear();
    return id;
}

bool EditSession::removeCreated(const core::Guid& id) {
    const auto it = drafts.find(id);
    if (it == drafts.end() || !it->second.created) {
        return false; // base records are immutable-visible (§5)
    }
    EditOp op;
    op.id = id;
    op.typeId = it->second.type->id;
    // Full snapshot: undo must restore edited field values, not defaults.
    sptr<Form> snapshot = types.instantiate(op.typeId);
    snapshot->id = id;
    copyFields(*it->second.type, *it->second.form, *snapshot);
    op.snapshot = std::move(snapshot);
    op.group = groupForNewOp();
    drafts.erase(it);
    undoStack.push_back(std::move(op));
    redoStack.clear();
    return true;
}

void EditSession::apply(const EditOp& op, bool forward) {
    if (op.snapshot) { // removal of a session-created draft
        if (forward) {
            drafts.erase(op.id);
        } else {
            const reflect::TypeInfo* type = types.findType(op.typeId);
            uptr<Form> form = types.instantiate(op.typeId);
            form->id = op.id;
            copyFields(*type, *op.snapshot, *form);
            drafts.emplace(op.id,
                           Draft { std::move(form), type, /*created=*/true });
        }
        return;
    }
    if (op.fieldId == 0) { // creation (blank or duplicate)
        if (forward) {
            uptr<Form> form = types.instantiate(op.typeId);
            if (op.sourceId.isValid()) {
                if (const Form* source = view(op.sourceId)) {
                    copyFields(*types.findType(op.typeId), *source, *form);
                }
            }
            form->id = op.id;
            form->editorId = op.editorId;
            drafts.emplace(op.id, Draft { std::move(form),
                                          types.findType(op.typeId), true });
        } else {
            drafts.erase(op.id);
        }
        return;
    }
    // Field edit: re-applying `before` may need to re-clone (undo after a
    // redo cleared the draft is impossible here since drafts persist, but
    // draftFor keeps this robust anyway).
    Draft* draft = draftFor(op.id);
    if (!draft) {
        return;
    }
    if (const reflect::FieldInfo* field = draft->type->findField(op.fieldId)) {
        field->set(draft->form.get(), forward ? op.after : op.before);
    }
}

void EditSession::undo() {
    if (undoStack.empty()) {
        return;
    }
    // Pop the whole gesture: reversed op order is correct by
    // construction (field edits revert before their creation drops).
    const u32 group = undoStack.back().group;
    while (!undoStack.empty() && undoStack.back().group == group) {
        const EditOp op = undoStack.back();
        undoStack.pop_back();
        apply(op, /*forward=*/false);
        redoStack.push_back(op);
    }
}

void EditSession::redo() {
    if (redoStack.empty()) {
        return;
    }
    // The redo stack holds the group reversed, so popping the back
    // replays in the ORIGINAL order (creation before its field edits).
    const u32 group = redoStack.back().group;
    while (!redoStack.empty() && redoStack.back().group == group) {
        const EditOp op = redoStack.back();
        redoStack.pop_back();
        apply(op, /*forward=*/true);
        undoStack.push_back(op);
    }
}

void EditSession::discardAll() {
    drafts.clear();
    undoStack.clear();
    redoStack.clear();
}

Plugin EditSession::exportPlugin(const core::Guid& pluginId,
                                 const str& name) const {
    Plugin plugin;
    plugin.id = pluginId;
    plugin.name = name;

    for (const auto& [id, draft] : drafts) {
        Record record;
        record.formId = id;
        record.typeId = draft.type->id;
        record.creates = draft.created;

        // Reference values: the base form for edits, C++ defaults for
        // creations — either way, only differing fields are emitted (§5:
        // a record carries only what it changes; the shared rule lives in
        // diffToRecord). Inherited fields included so a changed editorId
        // persists.
        const Form* reference = nullptr;
        uptr<Form> defaults;
        if (draft.created) {
            defaults = types.instantiate(draft.type->id);
            reference = defaults.get();
        } else {
            reference = base.find(id);
        }
        diffToRecord(*draft.type, draft.form.get(), reference, record,
                     /*includeInherited=*/true);
        if (!record.fields.empty() || record.creates) {
            plugin.records.push_back(std::move(record));
        }
    }
    std::sort(plugin.records.begin(), plugin.records.end(),
              [](const Record& a, const Record& b) {
                  return a.formId < b.formId;
              });
    return plugin;
}

} // namespace data
