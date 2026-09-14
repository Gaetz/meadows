#pragma once

namespace cooker {

// cooker map-proto <gameDir> <mapTx> <mapTz> [tilesPerSide=2]
// TERRAIN-RECUL prototype: erode ONE map-sized window globally (stage-1
// with tileSize = map size), then finalize each 4096 m tile inside it
// against that single shared surface (the map stage-1 stands in for all
// nine 3x3 neighbours). Measures what the bounded-map architecture
// buys: the band divergence between adjacent produced tiles (expected
// ~0 vs 200-440 m for windowed stage-1s) and the global-bake cost.
int mapProto(char** argv, int argc);

} // namespace cooker
