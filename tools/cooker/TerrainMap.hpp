#pragma once

// `cooker terrain-map` — top-down PNG overview of the analytic sandbox
// macro (engine/terrain/generation/MapExport). Args:
//   cooker terrain-map <seed> <centerX> <centerZ> <spanMeters> <out.png>
//                      [sizePx=1000]

namespace cooker {

int terrainMap(char** argv, int argc);

} // namespace cooker
