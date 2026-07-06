[← Back to the hub](README.md)

# World, levels & prefabs

## Spaces and cells

The world is organized as **worldspaces** (an exterior region, or a single
interior) divided into **cells** on a grid. Cells stream in and out around
the player.

```toml
[[records]]
form = "GUID-CELL"
type = "CellForm"
new = true
[records.fields]
worldspace = "GUID-WORLDSPACE"
gridX = 3
gridY = -1
```

## Placing things: references

A `ReferenceForm` places any base form (a static, an actor, a light, a
piece of furniture...) into a cell:

```toml
[[records]]
form = "GUID-MY-BARREL"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "GUID-BARREL-STATIC"
cell = "GUID-CELL"
position = [12.0, 0.0, -4.5]
```

Because references are records, mods can **move** (`position`),
**disable** (`enabled = false`) or **re-home** (`cell`) anything the base
game placed — with one-field patches.

Placeable base-form families: statics (`StaticForm`), actors
(`ActorForm`), lights (`LightForm`), invisible **markers**
(`MarkerForm` — spawn points, patrol stops, schedule locations), gameplay
**triggers** (`TriggerForm` — volumes that fire events or scripts), and
furniture ([Schedules & furniture](schedules-and-furniture.md)).

## Prefabs

A **prefab** is a reusable group — a campfire with its logs, light and
cooking pot; a whole house with its furniture. The group is a `PrefabForm`
plus template references pointing at it:

```toml
[[records]]
form = "GUID-CAMPFIRE"
type = "PrefabForm"
new = true
[records.fields]
displayName = "Campfire"

# A template child: `prefab` set, transform RELATIVE to the prefab pivot
[[records]]
form = "GUID-CAMPFIRE-LIGHT"
type = "ReferenceForm"
new = true
[records.fields]
prefab = "GUID-CAMPFIRE"
baseForm = "GUID-FIRE-LIGHT"
position = [0.0, 0.5, 0.0]
```

Placing the prefab = one ordinary reference whose `baseForm` is the
`PrefabForm`; the game expands the group on load. The in-game level editor
will offer *create prefab from selection*.

## 3D props: models and materials

A `StaticForm` gives a placed prop its 3D look: a glTF model asset plus a
`MaterialForm` (flat albedo texture × tint, stylized ramp lighting):

```toml
[assets]
"GUID-BARREL-MODEL" = "models/barrel.gltf"

[[records]]
form = "GUID-BARREL-MATERIAL"
type = "MaterialForm"
new = true
[records.fields]
editorId = "BarrelWood"
tint = [0.45, 0.32, 0.20, 1.0]   # keep tints well below 1.0: the sun is
                                 # HDR — near-white tints blow out to white

[[records]]
form = "GUID-BARREL-STATIC"
type = "StaticForm"
new = true
[records.fields]
editorId = "Barrel"
model = "GUID-BARREL-MODEL"
material = "GUID-BARREL-MATERIAL"
```

Model conventions: meters, the engine recenters the footprint and drops
the base to the ground (references place the base on the terrain);
per-instance size comes from the reference's `scale`. While a model
streams in, a placeholder box renders — nothing ever blocks.

## Characters: appearance and animation

An actor's look and motion are data too. `ActorForm.appearance` points at
an `AppearanceForm` (the rig's glTF asset + a body mesh per slot + skin
tint) and `ActorForm.animGraph` at an `AnimGraphForm` — a locomotion
state machine whose states and transitions are child records (a mod adds
an animation or retunes a blend threshold with one record). Clips
reference animations *inside* a glTF by name; they must be authored
**in-place** (the engine drives movement, and syncs playback to the
entity's real speed so feet don't slide). Timeline events (`AnimEventForm`
children of a clip: footsteps, hit frames) are the animation→gameplay
bridge.

## Doors and interiors

An interior is its own worldspace (`WorldspaceForm` with
`interior = true`) with its own cells — no terrain or sky, ambient plus
local lights. A `DoorForm` links spaces: its `targetMarker` is the GUID of
a placed marker *reference* — position, facing AND destination cell (thus
worldspace) all come from that one record:

```toml
[[records]]
form = "GUID-HOUSE-DOOR"
type = "DoorForm"
new = true
[records.fields]
displayName = "House door"
model = "GUID-DOOR-MODEL"
targetMarker = "GUID-ARRIVAL-MARKER-REF"
```

## Authored terrain

Terrain height = the procedural base + **authored delta grids**, one per
64 m chunk, stored as `.ter` assets referenced by `TerrainPatchForm`
records — so terrain edits are moddable like everything else (override
the asset by guid, or patch the record). Sculpt in the in-game level
editor (F6 → Sculpt terrain), or generate a leveled pad offline with
`cooker terrain-pad`. Building modules use `snapToGround = false` in
their `StaticForm` (absolute heights on a leveled pad); loose props keep
the default (their `y` is an offset above the ground).

## Hand-made world, procedural helpers

The game world is authored by hand. Procedural tools exist to *assist*
authoring (terrain starting points, scatter brushes, dungeon-base
generators) — their output is always ordinary records you can edit.

Related: [The data model](data-model.md) · [In-game tools](tools.md)
