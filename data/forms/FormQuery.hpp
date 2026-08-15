#pragma once

#include "data/forms/FormDatabase.hpp"

// Typed iteration helpers over a resolved FormDatabase. They generalize
// the manual handle-scan pattern (resolveWeatherForms) and are the query
// side of the CHILD-RECORD convention:
//
//   Reflection is flat (no containers), so any variable-cardinality data is
//   expressed as CHILD Forms carrying a `core::Guid parent` field pointing
//   at their owner — the pattern conditions/quests/dialogue already use.
//   A mod ADDS an entry (schedule slot, anim event, sound variant...) by
//   adding a child record; it never touches the parent record.
//
// Iteration order is handle order = creation order = plugin load order —
// deterministic for a given load order (§8). When authored order matters
// beyond that, the child Form carries its own sort key (e.g. startHour).

namespace data {

// Calls `fn(const T&)` for every form of type T (or derived). Backed by
// the database's type index — same handle order as a full scan, without
// touching the other N-1 types.
template<typename T, typename Fn>
void forEach(const FormDatabase& database, Fn&& fn) {
    for (const FormHandle handle :
         database.handlesByType(T::staticTypeInfo().id)) {
        fn(*static_cast<const T*>(database.get(handle)));
    }
}

// Calls `fn(const T&)` for every T whose `parent` field equals `parent`.
// T must expose a public `core::Guid parent` member (the child convention).
// Backed by the parent index: one bucket walk instead of a full scan.
template<typename T, typename Fn>
void childrenOf(const FormDatabase& database, const core::Guid& parent,
                Fn&& fn) {
    const u32 typeId = T::staticTypeInfo().id;
    for (const FormHandle handle : database.childHandles(parent)) {
        const reflect::TypeInfo* type = database.typeOf(handle);
        if (type && type->isA(typeId)) {
            fn(*static_cast<const T*>(database.get(handle)));
        }
    }
}

// Convenience: collect instead of visit.
template<typename T>
vector<const T*> collectChildren(const FormDatabase& database,
                                 const core::Guid& parent) {
    vector<const T*> children;
    childrenOf<T>(database, parent,
                  [&](const T& form) { children.push_back(&form); });
    return children;
}

// Reverse lookup (the GameDB "used by" tool): every form
// carrying a Guid field equal to `target` — parents found from a child,
// wielders found from a weapon... Reflection-driven, so new Form types
// participate for free. O(forms × fields): tooling only, never per-frame.
struct FormReferenceHit {
    core::Guid from; // the referencing form
    str typeName;    // its type
    str fieldName;   // the Guid field that points at target
};

inline vector<FormReferenceHit> referencesTo(const FormDatabase& database,
                                             const core::Guid& target) {
    vector<FormReferenceHit> hits;
    for (u32 i = 1; i <= database.count(); ++i) {
        const FormHandle handle { i };
        const reflect::TypeInfo* type = database.typeOf(handle);
        if (!type) {
            continue;
        }
        const Form* form = database.get(handle);
        reflect::forEachField(*type, [&](const reflect::FieldInfo& field) {
            if (field.kind != reflect::FieldKind::Guid) {
                return;
            }
            if (std::get<core::Guid>(field.get(form)) == target) {
                hits.push_back({ form->id, type->name, field.name });
            }
        });
    }
    return hits;
}

// First form of type T with the given editorId (tool/console lookups; the
// runtime resolves by guid, editorIds are an authoring convenience).
template<typename T>
const T* findByEditorId(const FormDatabase& database,
                        std::string_view editorId) {
    const T* found = nullptr;
    forEach<T>(database, [&](const T& form) {
        if (!found && form.editorId == editorId) {
            found = &form;
        }
    });
    return found;
}

} // namespace data
