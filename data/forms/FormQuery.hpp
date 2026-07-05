#pragma once

#include "data/forms/FormDatabase.hpp"

// Typed iteration helpers over a resolved FormDatabase (horizontal pass H1).
// They generalize the manual handle-scan pattern (resolveWeatherForms) and
// are the query side of the CHILD-RECORD convention:
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

// Calls `fn(const T&)` for every form of type T (or derived).
template<typename T, typename Fn>
void forEach(const FormDatabase& database, Fn&& fn) {
    const u32 typeId = T::staticTypeInfo().id;
    for (u32 i = 1; i <= database.count(); ++i) {
        const FormHandle handle { i };
        const reflect::TypeInfo* type = database.typeOf(handle);
        if (!type || !type->isA(typeId)) {
            continue;
        }
        fn(*static_cast<const T*>(database.get(handle)));
    }
}

// Calls `fn(const T&)` for every T whose `parent` field equals `parent`.
// T must expose a public `core::Guid parent` member (the child convention).
template<typename T, typename Fn>
void childrenOf(const FormDatabase& database, const core::Guid& parent,
                Fn&& fn) {
    forEach<T>(database, [&](const T& form) {
        if (form.parent == parent) {
            fn(form);
        }
    });
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
