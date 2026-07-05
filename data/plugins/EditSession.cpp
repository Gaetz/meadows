#include "data/plugins/EditSession.hpp"

#include <algorithm>

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
    undoStack.push_back({ id, fieldId, before, value, 0, {} });
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
    undoStack.push_back({ id, 0, {}, {}, typeId, editorId });
    redoStack.clear();
    return id;
}

void EditSession::apply(const EditOp& op, bool forward) {
    if (op.fieldId == 0) { // creation
        if (forward) {
            uptr<Form> form = types.instantiate(op.typeId);
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
    const EditOp op = undoStack.back();
    undoStack.pop_back();
    apply(op, /*forward=*/false);
    redoStack.push_back(op);
}

void EditSession::redo() {
    if (redoStack.empty()) {
        return;
    }
    const EditOp op = redoStack.back();
    redoStack.pop_back();
    apply(op, /*forward=*/true);
    undoStack.push_back(op);
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
        // a record carries only what it changes).
        const Form* reference = nullptr;
        uptr<Form> defaults;
        if (draft.created) {
            defaults = types.instantiate(draft.type->id);
            reference = defaults.get();
        } else {
            reference = base.find(id);
        }
        reflect::forEachField(*draft.type, [&](const reflect::FieldInfo&
                                                   field) {
            if (field.flags & reflect::Transient) {
                return;
            }
            const reflect::Value value = field.get(draft.form.get());
            if (!reference || field.get(reference) != value) {
                record.fields.emplace(field.id, value);
            }
        });
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
