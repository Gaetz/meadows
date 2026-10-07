#pragma once

namespace cooker {

// cooker landscape-report <gameDir> [mapX mapZ]
// The brick's ONE measurement (docs/PAYSAGE.md §7.7): bakes the map
// when its cache is stale for the game's params (pupitre included),
// writes plan.png next to it and prints the landscape census.
int landscapeReport(char** argv, int argc);

} // namespace cooker
