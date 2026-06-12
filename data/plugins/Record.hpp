#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/reflect/Reflect.hpp"

namespace data {

// One entry of a plugin (§5): either creates a new form (new = true) or
// patches an existing one. Field-level by design: it carries ONLY the
// fields it touches — that is why two mods editing different fields of the
// same form do not conflict.
struct Record {
    core::Guid formId;
    u32 typeId { 0 };
    bool creates { false };
    std::unordered_map<u32 /*fieldId*/, reflect::Value> fields;
};

// An ordered set of records: base game, mod, or (later) the save layer —
// all the same thing to the resolver (§2.4).
struct Plugin {
    core::Guid id;
    str name;
    vector<core::Guid> dependencies; // declared requirements, for load-order
                                     // validation (not enforced yet)
    vector<Record> records;
};

} // namespace data
