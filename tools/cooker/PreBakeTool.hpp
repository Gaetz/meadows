#pragma once

namespace cooker {

// `cooker pre-bake` — fills the game's terrain cache offline (the same
// TerrainBakeStreamer pipeline, same cache keys), so a demo route
// streams with zero in-session bakes and the far-water sees every lake
// in the area. See preBake() in PreBakeTool.cpp for the argument forms.
int preBake(char** argv, int argc);

} // namespace cooker
