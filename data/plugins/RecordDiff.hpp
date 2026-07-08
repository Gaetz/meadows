#pragma once

#include "data/plugins/Record.hpp"
#include "engine/reflect/Reflect.hpp"

namespace data {

// The §5 rule "a record carries only what it changes", in ONE place. Emits
// into `record.fields` every non-Transient reflected field of `object` (an
// instance of `type`) whose value differs from `reference`. A null `reference`
// emits every non-Transient field (a from-scratch record).
//
// `includeInherited` picks the traversal, and the two callers genuinely differ:
//  - true  → parents first too (reflect::forEachField): the editor's export,
//            which persists an inherited editorId that a mod changed.
//  - false → own fields only (type.fields): the save's child records, which
//            deliberately keep inherited id/editorId OUT — a saved row's
//            identity is its deterministic record guid, not a stored field.
void diffToRecord(const reflect::TypeInfo& type, const void* object,
                  const void* reference, Record& record, bool includeInherited);

} // namespace data
