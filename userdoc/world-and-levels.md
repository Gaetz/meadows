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

## Hand-made world, procedural helpers

The game world is authored by hand. Procedural tools exist to *assist*
authoring (terrain starting points, scatter brushes, dungeon-base
generators) — their output is always ordinary records you can edit.

Related: [The data model](data-model.md) · [In-game tools](tools.md)
