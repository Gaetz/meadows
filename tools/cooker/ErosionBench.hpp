#pragma once

// `cooker erosion-bench` — bakes one reference tile under the
// fine-erosion reintroduction variants (TileBakeParams fineCalmGate* /
// fineSlopeReturn / relaxGate* / keepCrestFade), renders each shaded
// relief and assembles a contact sheet. Args:
//   cooker erosion-bench <seed> <tileX> <tileZ> <outDir> [sizePx=1000]

namespace cooker {

int erosionBench(char** argv, int argc);

} // namespace cooker
