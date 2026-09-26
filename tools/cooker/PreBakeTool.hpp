#pragma once

namespace cooker {

// `cooker pre-bake` — bakes the bounded maps overlapping a rect into
// the game's terrain cache offline (game::bakeMap, same cache keys),
// so a demo route streams with zero in-session bakes and the far-water
// sees every lake. See preBake() in PreBakeTool.cpp for the forms.
int preBake(char** argv, int argc);

} // namespace cooker
