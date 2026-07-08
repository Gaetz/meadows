#pragma once

namespace data {
class FormTypeRegistry;
}

namespace game {

// Registers EVERY Form family the game knows (chantier 4 B1) — THE single
// registration site. The game exe links it; the cooker compiles this same
// translation unit (see tools/CMakeLists.txt), so the two can never drift.
// Add a family here and it is known to both loading and cooking for free.
// A family missing here = its records silently skipped at load (types unknown
// to the registry are skipped with a log).
void registerAllFormTypes(data::FormTypeRegistry& types);

} // namespace game
