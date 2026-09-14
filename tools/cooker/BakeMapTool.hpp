#pragma once

namespace cooker {

// cooker bake-map <gameDir> <mapX> <mapZ> [tilesPerSide=6]
// Bakes one bounded map (chantier CARTES M1.1): ONE global erosion,
// slices written in the streamer cache format under
// terrain-cache/<seed>/map_<mx>_<mz>/ with a manifest, then an
// interior-border divergence report read back from the written slices
// (acceptance: <= 8 m everywhere until M1.2 moves hydrology per-map).
int bakeMapCmd(char** argv, int argc);

} // namespace cooker
