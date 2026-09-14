#pragma once

namespace cooker {

// cooker border-report <gameDir> <tx> <tz> [x|z] [out.csv]
// Bakes tile (tx,tz) and its +x (or +z) neighbour through the shared
// terrain cache, then measures the tile-border continuity the runtime
// blend has to swallow: band height-divergence profile, lake-mask
// truncation at the owner rect, and bed-carve asymmetry. The numbers
// baseline the FRONTIÈRES bricks (docs/CPU-PERF.md pattern: measure
// first).
int borderReport(char** argv, int argc);

} // namespace cooker
