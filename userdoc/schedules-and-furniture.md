[← Back to the hub](README.md)

# NPC schedules & furniture

## Daily routines

An NPC's day is a `ScheduleForm` with **entries** (one record per slice of
the day) choosing an **AI package** (what to do) at a **location** (where):

```toml
[[records]]
form = "GUID-SCHEDULE"
type = "ScheduleForm"
new = true
[records.fields]
editorId = "BlacksmithDay"

[[records]]
form = "GUID-ENTRY-WORK"
type = "ScheduleEntryForm"
new = true
[records.fields]
parent = "GUID-SCHEDULE"
startHour = 8.0
endHour = 19.0
package = "GUID-PKG-WORK"        # kind = "work" at the forge
location = "GUID-FORGE-REF"      # a placed furniture/marker reference

[[records]]
form = "GUID-ENTRY-SLEEP"
type = "ScheduleEntryForm"
new = true
[records.fields]
parent = "GUID-SCHEDULE"
startHour = 22.0
endHour = 6.0                    # wraps past midnight
package = "GUID-PKG-SLEEP"
```

- **AI packages** (`AiPackageForm`) are the executable behaviors:
  `sleep`, `eat`, `work`, `wander`, `travel`, `useFurniture`, `guard` —
  with a location, a radius and a walk-speed multiplier.
- Overlapping entries: the last one in load order wins — so **a mod can
  reroute anyone's evening by adding one entry**.
- Entries can carry conditions (the same condition records used by
  dialogue and quests): "only if it's raining", "only if the player is a
  guild member"...
- Attach the schedule to an NPC through `ActorForm.schedule`.

## Furniture & workstations

Furniture is what makes routines visible — and it works for the player
too. A `FurnitureForm` (bed, chair, forge, alchemy table...) has **use
points** as child records:

```toml
[[records]]
form = "GUID-BED"
type = "FurnitureForm"
new = true
[records.fields]
editorId = "SimpleBed"
category = "bed"
effect = "GUID-REST-EFFECT"      # applied while sleeping

[[records]]
form = "GUID-BED-POINT"
type = "FurniturePointForm"
new = true
[records.fields]
parent = "GUID-BED"
offset = [0.0, 0.4, 0.0]
animTag = "Sleep"
```

- `effect` is a regular gameplay effect
  ([Effects & abilities](effects-and-abilities.md)) active while the
  furniture is used — sleeping restores through the same rest system the
  player uses.
- `screen` (workstations) opens a UI screen for the player: crafting
  tables are furniture + a screen.
- Points have an offset, a facing and an animation tag; multi-seat
  benches simply have several points.

## What runs in the current build

- Schedules are **executed** in the 3D world: NPCs re-evaluate their
  entry every 10 in-game minutes and walk to it (`wander`, `travel`,
  `useFurniture` — claim + sit animation —, `guard` are live).
- The player can use furniture with **E**: a `category = "bed"` sleeps
  8 hours, anything else rests 1 hour — both advance the game clock,
  restore the sleep need and accrue Rest (injury recovery).
- **Workstations open their screen**: set `screen` to a `UiScreenForm`
  name and [E] shows that document instead of resting (the village
  Workbench proves it). The per-use `effect` is still declared-only.

Related: [The data model](data-model.md) ·
[World & levels](world-and-levels.md)
