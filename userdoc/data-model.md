[← Back to the hub](README.md)

# The data model

## Forms and References

- A **Form** is a *definition*: what an "iron sword" IS — its damage,
  weight, name. Forms never change during play.
- A **Reference** is a *placed instance* of a form in the world: THIS
  sword, on THAT table, in THAT house. References carry position,
  rotation, scale, an enabled flag... and they are records too — a mod
  can move, disable or add placed objects with ordinary patches.

## The parent/child pattern

Records hold simple fields only — there are no lists inside a record. When
something needs a variable number of entries (a schedule's day slices, a
sound's variants, an animation's events), each entry is its **own record**
pointing back at its owner through a `parent` field:

```toml
[[records]]
form = "GUID-SCHEDULE"
type = "ScheduleForm"
new = true
[records.fields]
editorId = "InnkeeperDay"

[[records]]
form = "GUID-ENTRY"
type = "ScheduleEntryForm"
new = true
[records.fields]
parent = "GUID-SCHEDULE"
startHour = 8.0
endHour = 22.0
package = "GUID-WORK-PACKAGE"
```

This is a superpower for mods: **you can ADD an entry to anything without
touching the original record.** Want the innkeeper to visit the temple at
dawn? Add one `ScheduleEntryForm` with her schedule as `parent`. No
compatibility patch, no override of her whole day.

## Form families

| Family | Types | What they define |
|---|---|---|
| Items | `WeaponForm`, `ArmorForm`, `ConsumableForm` | Equipment stats, typed damage, resistances |
| Characters | `ActorForm`, `AppearanceForm` | NPCs: stats, modular looks (head/hair/torso/... meshes, tints) |
| Gameplay | `EffectForm`, `AbilityForm`, `ConditionForm` | The stat/combat/magic layer — see [Effects & abilities](effects-and-abilities.md) |
| Routines | `ScheduleForm` + entries, `AiPackageForm` | NPC daily life — see [Schedules & furniture](schedules-and-furniture.md) |
| Interaction | `FurnitureForm` + use points | Beds, chairs, workstations |
| World | `WorldspaceForm`, `CellForm`, `ReferenceForm`, `PrefabForm`, `MarkerForm`, `TriggerForm` | Spaces, cells, placements — see [World & levels](world-and-levels.md) |
| Visuals | `MaterialForm`, `StaticForm`, `LightForm`, `ParticleForm`, `CueForm` | Materials, props, lights, FX and the tag→FX/sound mapping |
| Animation | `AnimClipForm` + events, `AnimGraphForm` + states/transitions | Clips, timeline events (hit frames, footsteps), controllers |
| Audio | `SoundForm` + variants | Sound events, buses, 3D settings |
| UI | `UiScreenForm` | Screen registry — see [UI modding](ui-modding.md) |
| Text | `LocStringForm` | Localizable strings — see [Localization](localization.md) |
| Narrative | quest & dialogue forms | Quests, stages, dialogue trees |

Browse them all live in the **Game DB editor** ([In-game tools](tools.md))
— every field of every type, searchable, editable, exportable.
