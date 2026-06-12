#pragma once

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/reflect/Reflect.hpp"

namespace data {

// A Form is a *definition* — what an "iron sword" IS (§2.2). Forms are
// immutable at runtime: gameplay reads them and creates References, it
// never mutates them. All mutable per-instance state lives on References.
//
// `id` is deliberately NOT reflected: it is the record's address, not its
// payload — patches target a form BY id, they never patch the id itself.
struct Form {
    core::Guid id;

    // Human-readable authoring name (like Skyrim's EditorID), unique per
    // plugin by convention; reflected, so a patch may fix it.
    str editorId;

    virtual ~Form() = default;

    REFLECT_BEGIN(Form, void)
        REFLECT_FIELD(editorId)
    REFLECT_END()
};

// Compact runtime identity for a resolved form (§2.5): GUIDs live at the
// boundaries (files, mod references), handles live in runtime code.
// 0 = invalid. Stable for the lifetime of one FormDatabase.
struct FormHandle {
    u32 value { 0 };

    bool isValid() const { return value != 0; }
    bool operator==(const FormHandle&) const = default;
};

} // namespace data
