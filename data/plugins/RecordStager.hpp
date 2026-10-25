#pragma once

#include <type_traits>

#include "data/forms/FormDatabase.hpp"
#include "data/plugins/EditSession.hpp"
#include "engine/core/Log.hpp"
#include "engine/reflect/Fields.hpp"

// The generator-side writer of records (map bake, dungeon bake): a
// deterministic guid per record, fields named in code, staged as SESSION
// drafts so Export ships them — and, when the generator wants the running
// session to play the result right away, mirrored LIVE into the resolved
// database (the materializeCell precedent). Same guid on both sides; the
// next full resolve unifies them.
//
// Every field write goes through reflect::fieldId: a misspelled field is
// a programming error caught in Debug and a logged skip in Release,
// never a null dereference.

namespace data {

class RecordStager {
public:
    RecordStager(EditSession& session, const FormDatabase& forms)
        : session { session }, forms { forms } {}

    // Creates the draft unless a plugin already ships the record or this
    // session already created it (then Export PATCHES it: the re-Accept
    // rule — same guids, no duplicates). Call it BEFORE live(): once the
    // live copy exists, forms.find() can no longer tell "shipped by a
    // plugin" from "live-added by this pass".
    void ensure(const reflect::TypeInfo& type, const core::Guid& guid,
                const str& editorId) {
        if (!forms.find(guid) && !session.isCreated(guid)) {
            session.createForm(type.id, editorId, guid);
        }
    }

    // One field by name. Unknown name: asserted (Debug), skipped (Release).
    template <typename T>
    void set(const reflect::TypeInfo& type, const core::Guid& guid,
             const char* field, const T& value) {
        const u32 id = reflect::fieldId(type, field);
        if (id == 0) {
            LOG_ERROR("stage: {} has no field '{}' (write skipped)",
                      type.name, field);
            return;
        }
        session.setField(guid, id, reflect::Value { value });
    }

    // Every reflected field of a fully built form (parents first), except
    // the identity the draft already carries (`id`, `editorId`) and
    // Transient state. Export then keeps what differs from the defaults.
    template <typename T>
    void setAll(const T& form) {
        static const u32 kId = core::fnv1a("id");
        static const u32 kEditorId = core::fnv1a("editorId");
        reflect::forEachField(
            T::staticTypeInfo(), [&](const reflect::FieldInfo& field) {
                if ((field.flags & reflect::Transient) != 0 ||
                    field.id == kId || field.id == kEditorId) {
                    return;
                }
                session.setField(form.id, field.id, field.get(&form));
            });
    }

    // ensure() + setAll(): the whole record of a built form in one call.
    template <typename T>
    void stage(const T& form, const str& editorId) {
        ensure(T::staticTypeInfo(), form.id, editorId);
        setAll(form);
    }

    // Mirrors the form LIVE into `live` (the mutable view of the same
    // database): added when unknown, else every field overwritten in
    // place — the generator owns its records, hand retouches belong in a
    // later plugin layer. `onLive(handle, before)` lets the caller keep
    // its own indexes in step: `before` is null for an added form, else
    // the field values the form carried before the overwrite.
    template <typename T, typename OnLive>
    void live(FormDatabase& live, const T& form, OnLive&& onLive) {
        const FormHandle handle = live.handleOf(form.id);
        if (!handle.isValid()) {
            const FormHandle added =
                live.add(std::make_unique<T>(form), T::staticTypeInfo());
            onLive(added, static_cast<const T*>(nullptr));
            return;
        }
        Form* target = live.getMutable(handle);
        if (!target) {
            return;
        }
        const T before = *static_cast<const T*>(target);
        reflect::copyFields(T::staticTypeInfo(), &form, target);
        onLive(handle, &before);
    }
    template <typename T>
    void live(FormDatabase& liveForms, const T& form) {
        live(liveForms, form, [](FormHandle, const T*) {});
    }

private:
    EditSession& session;
    const FormDatabase& forms;
};

} // namespace data
