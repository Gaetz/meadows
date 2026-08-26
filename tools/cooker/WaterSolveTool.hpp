#pragma once

namespace cooker {

// `cooker water-solve <seed> <tx> <tz> <out.png> [texel] [rain]` —
// the option-D prototype (docs/WATER-RESEARCH.md): bakes the tile,
// runs the steady-state virtual-pipes solve on its heights and writes
// a judgment map (hillshade + water depth blues + rapids whitening),
// with convergence/cost stats on stdout.
int waterSolve(char** argv, int argc);

} // namespace cooker
