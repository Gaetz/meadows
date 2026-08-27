#pragma once

namespace cooker {

// `cooker water-replay <dump.wsd> <out-prefix> [substeps]` — load an
// in-game sim dump (Water panel > "Dump sim state"), print its stats,
// render a judgment map, optionally step it and render again. The
// offline reproduction bench for the live water sim.
int waterReplay(char** argv, int argc);

} // namespace cooker
