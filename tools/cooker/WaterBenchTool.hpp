#pragma once

namespace cooker {

// `cooker water-bench <gameDir> <mapX> <mapZ> <x> <z> [substeps]
//   [--boost x] [--pin hw] [--probe file] [--png prefix]`
// — open the live water sim's window at (x, z) on the BAKED map, the
// way the game does on entry (master boundary sources + baked entry
// sources, pre-roll, lake and river pins, settling burst), then step
// it and print the stats, the brook film census and the probe lines.
// The headless bench for "what does the sim hold at this spot" without
// an in-game dump; the window and tuning are the game's (data stack).
int waterBench(char** argv, int argc);

} // namespace cooker
