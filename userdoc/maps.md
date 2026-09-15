# Bounded maps — the world as a graph of 24 km lands

The overworld is not one endless plane: it is a **graph of bounded
maps**, each a 24.6 × 24.6 km land, connected at their borders. Each
map is eroded **globally in one solve** — rivers, lakes and valleys are
consistent across the whole map, with no seams inside it. What lies
beyond a border is another map: another worldspace, swapped in when you
travel there.

## What a border looks like

Every border line between two maps has a **style**, decided by the
world seed and resolved against the terrain it crosses:

- **Mountains** — a range rises progressively on both sides of the
  border, its crest varying along the line so that natural **cols**
  (saddles) emerge. Crossing happens at a col.
- **Sea** — a genuine sea arm separates the two maps, with coasts on
  both sides and occasional islets mid-channel. Sea borders only occur
  where the world is actually coastal; deep inland a sea proposal
  resolves to mountains instead.

Borders meander (no ruler-straight coastlines), compose naturally at
corners (a range diving into a sea arm makes coastal cliffs), and both
neighbouring maps agree on the shared border by construction.

## Traveling between maps

Crossing is a **travel** (a door-style fade): the whole world —
terrain, water, streaming, collisions — swaps to the destination map.
A crossing point is pure data: a pair of persistent `TriggerForm`
records whose script calls the Lua binding

```lua
travelToMap(mapX, mapZ, arrivalX, arrivalZ)
```

See `base/passes.toml` for a complete working col (two triggers, one
per direction, arrival points past the twin trigger). If the
destination map was never baked, it bakes in the background behind the
loading veil (about two minutes); approaching a col also pre-bakes the
neighbour map ahead of time.

The in-game map screen (M) shows the full extent of the current
bounded map. Saves made on any map reload on that map.

## A map is a worldspace (and a mod can add one)

A bounded map IS a `WorldspaceForm` with these fields:

```toml
[[records]]
form = "GUID-OF-THE-MAP"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "MyIsland"
bounded = true
mapX = 1          # grid coords: the rect is map(X,Z) * mapSize
mapZ = 0
mapSize = 24576.0
mapSeed = 0       # 0 = derived from the world seed + coords
```

Its terrain is the set of `TerrainRegionForm` records scoped to it
(field `worldspace`), each pointing at a `.trg` slice asset; its water
is `WaterBodyForm` / `RiverForm` records scoped the same way. Records
not tagged with a worldspace belong to the default overworld only —
a bounded map never inherits them by accident.

## Shipping a map as a mod

Two ways, both producing an ordinary plugin:

1. **Command line** — bake and export in one go:

   ```
   cooker bake-map <gameDir> <mapX> <mapZ> --export-plugin my-island
   ```

   This writes `data/mods/my-island.toml` (worldspace + slice records +
   water) plus the 36 terrain assets under `data/mods/terrain/`, and
   tells you the line to add to `plugins.toml`.

2. **In the editor** — the *Terrain generation* panel's **Bounded
   map** section: pick the map coords, **Bake map** (progress bar; a
   map already in the cache accepts instantly), then **Accept map →
   records** and the ordinary **Export** ships the mod.

Re-baking or re-exporting the same map **patches the same records**
(identities derive from the map's GUID), so hand retouches made on top
of a generated map survive a re-export, exactly like generated
dungeons.

Related: [World, levels & prefabs](world-and-levels.md) ·
[In-game tools](tools.md) · [How plugins work](plugins.md)
