[← Back to the hub](README.md)

# How plugins work

A **plugin** is a `.toml` text file containing a header and a list of
**records**. The base game, every mod, and (eventually) your save file are
all plugins — the same format, the same rules.

## The header

```toml
[plugin]
id = "0a3f9c1e-5b71-4d2a-9c64-8e21d7b3f0a2"  # the plugin's own GUID
name = "sharper-swords"
# dependencies = ["guid-of-another-plugin"]  # optional
```

Every plugin (and every record) is identified by a **GUID** — a random
128-bit id like `1b4e28ba-2fa1-41d2-883f-0016d3cca427`. GUIDs never depend
on load order, so your mod's ids stay valid forever no matter what other
mods are installed. Mint fresh GUIDs for everything NEW you create (any
UUIDv4 generator works; the in-game editor mints them for you).

## Records: create or patch

A record either **creates** a new form or **patches** an existing one:

```toml
# CREATE: new = true, and you own this GUID from now on
[[records]]
form = "11112222-3333-4444-8555-666677778888"
type = "WeaponForm"
new = true
[records.fields]
editorId = "FlameSword"
displayName = "Flame Sword"
damage = 22.0
fireAttack = 10.0

# PATCH: no `new`, target someone else's GUID, list ONLY what you change
[[records]]
form = "GUID-OF-THE-IRON-SWORD"
type = "WeaponForm"
[records.fields]
damage = 30.0
```

**A patch carries only the fields it changes.** This is the heart of the
system: if your mod changes `damage` and another mod changes
`displayName` on the same sword, both apply — no conflict, no compatibility
patch needed. See [Load order & conflicts](load-order.md) for what happens
when two mods touch the *same* field.

## Field values

| Field type | TOML syntax |
|---|---|
| number | `damage = 22.0`, `count = 3` |
| bool | `twoHanded = true` |
| text | `displayName = "Flame Sword"` |
| GUID reference | `effect = "aaaa..."` (points at another record) |
| vector | `position = [12.0, 0.0, -4.5]` |
| color | `tint = [1.0, 0.8, 0.5, 1.0]` |
| rotation | `rotation = [0.0, 0.0, 0.0, 1.0]` (quaternion x,y,z,w) |

Unknown fields are reported and skipped — a typo can't crash the game.

## `editorId`

Every record has an optional `editorId`: a human-readable name
(`"IronSword"`, `"quest.intro.title"`). It's for you and the tools — the
game links records by GUID only. Keep them unique within your plugin.

## Assets

Plugins can ship files (textures, models, sounds, UI documents) next to
the `.toml`. Assets are layered like fields: a later plugin providing the
same asset GUID overrides the earlier one.

Next: [Load order & conflicts](load-order.md) ·
[The data model](data-model.md)
