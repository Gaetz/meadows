#pragma once

namespace data {
class FormTypeRegistry;
}

namespace game {

// Registers EVERY Form family the game knows (chantier 4 B1) — the single
// registration site for the executable's scenes. Keep in sync with
// tools/cooker/Main.cpp (the only other complete site; the cooker cannot
// link game code). A family missing here = its records silently skipped at
// load (types unknown to the registry are skipped with a log).
void registerAllFormTypes(data::FormTypeRegistry& types);

} // namespace game
