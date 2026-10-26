#pragma once

// The script-side name of the persistent per-entity variables. The struct
// lives in gameplay (gameplay/actors/ScriptVars.hpp) so the save layer can
// persist it without depending on the script layer; the VM only needs the
// alias.
#include "gameplay/actors/ScriptVars.hpp"

namespace script {

using ScriptVars = gameplay::ScriptVars;
using gameplay::registerScriptComponents;

} // namespace script
