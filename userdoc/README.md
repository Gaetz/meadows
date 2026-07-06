# Modding & User Guide

Welcome! This is the hub for everything about **using and modding the
game**. Every piece of game content — items, NPCs, spells, schedules,
lights, whole villages — is plain data in text files called **plugins**,
and everything a plugin can do, your mod can do too: the base game itself
is just the first plugin in the list.

> The engine promise: **mods patch fields, not files**. Two mods editing
> different fields of the same NPC never conflict. See
> [How plugins work](plugins.md) for the model.

## Guides

| Page | What it covers |
|---|---|
| [How plugins work](plugins.md) | The `.toml` plugin format: records, GUIDs, creating vs patching, dependencies |
| [Load order & conflicts](load-order.md) | `plugins.toml`, the in-game plugin manager, how conflicts resolve |
| [The data model](data-model.md) | Forms vs References, the parent/child record pattern, every form family |
| [Effects & abilities](effects-and-abilities.md) | Spells, buffs, poisons, diseases, abilities — pure data, no code |
| [World, levels & prefabs](world-and-levels.md) | Worldspaces, cells, placed references, markers, triggers, prefabs |
| [NPC schedules & furniture](schedules-and-furniture.md) | Daily routines, AI packages, beds/chairs/workstations |
| [Localization](localization.md) | Translating the game (or your mod) — a language pack is a plugin |
| [UI modding](ui-modding.md) | Replacing game screens (RmlUi documents) — the SkyUI model |
| [Save games](saving.md) | A save is an ordinary plugin: format, slots, what gets captured |
| [In-game tools](tools.md) | The Game DB editor, the developer console, exporting your edits as a mod |

## Quick start: your first mod in five minutes

1. Create `my-mod.toml` next to the game's other plugins (see
   [Load order](load-order.md) for where that is).
2. Paste this — it doubles the iron sword's damage:

```toml
[plugin]
id = "PUT-A-FRESH-GUID-HERE"      # generate one: the in-game console, or any UUIDv4 tool
name = "my-mod"

[[records]]
form = "GUID-OF-THE-IRON-SWORD"   # find it in the in-game Game DB editor
type = "WeaponForm"
[records.fields]
damage = 30.0
```

3. Add `my-mod.toml` to the load order ([how](load-order.md)) and launch.

Even easier: make your edits in the **in-game Game DB editor** and click
*Export plugin* — it writes a valid mod file for you
([In-game tools](tools.md)).
