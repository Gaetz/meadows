#pragma once

#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Record.hpp"

namespace data {

// Plugin -> human-readable TOML (the uncook direction). Needs the registry
// to turn type/field ids back into names; records with unknown type ids and
// fields with unknown ids are skipped with a warning (stale cooked data).
// Output is deterministic: fields sorted by name, so uncooked files diff
// cleanly under version control.
str writePluginToml(const Plugin& plugin, const FormTypeRegistry& types);

} // namespace data
