#pragma once

// Subsystem map: docs/AUDIT/U7-data-world.md

#include <unordered_map>

#include "data/forms/Form.hpp"
#include "engine/reflect/Reflect.hpp"

namespace data {

// The resolved view of the world's definitions: output of the §5 plugin
// resolution, read-only for gameplay. Owns the forms.
class FormDatabase {
public:
    // Takes ownership. Returns an invalid handle (and logs) if the guid is
    // missing or already present.
    FormHandle add(uptr<Form> form, const reflect::TypeInfo& type);

    const Form* find(const core::Guid& id) const;
    const Form* get(FormHandle handle) const;
    FormHandle handleOf(const core::Guid& id) const;
    const reflect::TypeInfo* typeOf(FormHandle handle) const;

    // TOOL-ONLY in-place mutation (the hatch the index comment below
    // anticipates): generators re-Accepting over records a plugin already
    // shipped update the live copy so the running session matches. Gameplay
    // never calls this (§2.2). Callers touching an indexed field (`parent`,
    // a reference's cell) own the re-index.
    Form* getMutable(FormHandle handle);

    // Typed lookup; nullptr if absent or not a T (checked via isA).
    template<typename T>
    const T* find(const core::Guid& id) const {
        const FormHandle handle = handleOf(id);
        if (!handle.isValid()) {
            return nullptr;
        }
        if (!typeOf(handle)->isA(T::staticTypeInfo().id)) {
            return nullptr;
        }
        return static_cast<const T*>(get(handle));
    }

    u32 count() const { return static_cast<u32>(entries.size()); }

    // --- Secondary indexes (the
    // resolved FormDatabase IS the base — scalability comes from in-memory
    // indexes, not SQL). Both return handles in HANDLE ORDER (= creation
    // order = plugin load order — the forEach/childrenOf contract).
    // add() maintains them: the resolver materializes every field WRITE
    // (patches included) BEFORE adding, so the indexed `parent` is final.
    // Nothing mutates resolved forms in place today (§2.2 — the editor
    // edits EditSession drafts); rebuildIndexes() is the safety hatch if
    // a tool ever does.

    // Every form whose type isA(typeId) — the whole inheritance chain is
    // bucketed at add, so base-type queries are one lookup.
    const vector<FormHandle>& handlesByType(u32 typeId) const;
    // Every form whose reflected `parent` guid field equals `parent` (the
    // child-record convention).
    const vector<FormHandle>& childHandles(const core::Guid& parent) const;
    // Re-derives both indexes from the entries.
    void rebuildIndexes();

private:
    struct Entry {
        uptr<Form> form;
        const reflect::TypeInfo* type { nullptr };
    };

    void indexForm(FormHandle handle, const Form& form,
                   const reflect::TypeInfo& type);

    vector<Entry> entries; // FormHandle::value = index + 1
    std::unordered_map<core::Guid, u32> indexByGuid;
    std::unordered_map<u32, vector<FormHandle>> byType;
    std::unordered_map<core::Guid, vector<FormHandle>> byParent;
};

} // namespace data
