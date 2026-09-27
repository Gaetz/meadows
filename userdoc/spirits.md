[← Back to the hub](README.md)

# Spirits: shaping the world

The nine **spirits** of the game's universe are the powers that act on
the *world itself* — pour water, raise earth, light a fire, raise a
wind — rather than on a character's stats. They are pure data, like
everything else: a spirit is a record, its rules are records, the spring
a player poured is a record in the save.

> Status of this chapter: **Water is playable** (the Q ability). Earth,
> fire and wind follow, one at a time. Everything below that names another
> spirit is the framework waiting for its kernel.

## The nine spirits — three triads

| Triad | Matter (Onyx) | Energy (Amber) | Mind (Garnet) |
|---|---|---|---|
| 1 | `Vegetation` (Tree) | `Light` (Moon) | `Darkness` (Black) |
| 2 | `Earth` + blunt (Hammer) | `Lightning` + slashing (Blade) | `Psy` + piercing (Mirror) |
| 3 | `Water` / cold (Water) | `Fire` (Flame) | `Wind` / breath (Breath) |

Inside each triad, **matter beats energy, energy beats mind, mind beats
matter** — the same cycle as the resonance channels of the stats system.
The game derives that dominance itself; a `SpiritRuleForm` only *refines*
what "beats" means (water *extinguishes* fire, wind *pushes* water…).
Pairs outside a triad interact only where a rule says so.

Water and ice are **one** spirit (one resistance): ice is the cold state
of water.

## The records

- **`SpiritForm`** — one per spirit: its triad and channel, the kind of
  field it produces, decay/spread rates, the `contactEffect` (an
  `EffectForm` with a `buildupType` — the only road into a character's
  statuses, see [Effects & abilities](effects-and-abilities.md)), the
  physical damage type for triad 2, and its cues.
- **`SpiritRuleForm`** — one per (actor, target, verb): `actor` **must be
  a spirit** (a material can never change another material — every change
  goes through a spirit; the loader rejects the record otherwise),
  `target` is a spirit or a material class, `verb` is one of
  `extinguish ignite conduct block grow evaporate wet freeze push erode
  activate suppress`, plus `rate` and `threshold`.
- **`SurfaceMaterialForm`** — a material class (`grass`, `rock`, `snow`,
  `sand`, `cliff`, `wood`, `bush`…) with the properties the world lacks
  otherwise: `flammability`, `fuel`, `moisture`, `conductivity`,
  `hardness`.
- **`SpiritSourceForm`** — a *placed* source: `worldspace`, `spirit`,
  `x`/`z`, an optional direction, `rate`, `radius`, `remainingSeconds`
  (`-1` = permanent). A spring authored in a mod and a spring the player
  poured (captured by the save) are the **same record** — a mod can
  therefore add a permanent spring to a hillside with one record.

All of these live in `game/data/base/spirits.toml`; a mod patches or adds
them like any other form ([How plugins work](plugins.md)).

## Casting a spirit: the ability's script

A spirit power is an ordinary `AbilityForm` — cost and cooldown are
effects — whose `script` places the source. The script sees two world
actions:

```toml
[[records]]
form = "5b1e1700-0000-4000-8000-000000000063"
type = "AbilityForm"
new = true
[records.fields]
editorId = "SpiritWater"
cost = "5b1e1700-0000-4000-8000-000000000061"     # essence -15, strict
cooldown = "5b1e1700-0000-4000-8000-000000000062" # 8 s, Cooldown.SpiritWater
costPolicy = "strict"
blockedTag = "State.Exhausted"
script = """
local p = aim()                      -- the aimed ground point {x, y, z}, or nil
if p then spirit.spawn("Water", p.x, p.z, 3.0, 4.0, 10.0) end
"""
```

- `aim()` — where the player looks, on the terrain (a rock or a wall is
  not ground). `nil` when nothing is aimed.
- `spirit.spawn(kind, x, z, rate, radius, seconds)` — places a source.
  `rate` is in the spirit's own unit (m³/s for water), `seconds` runs in
  **simulation** time (a 10-second spring pours the same volume whatever
  the time scale), `-1` makes it permanent.
- `wait(t)` — the script is a coroutine: two jets half a second apart is
  `spirit.spawn(...) wait(0.5) spirit.spawn(...)`.
- `spirit.push_terrain(x, z, radius, amount, brush)` — reserved for the
  earth spirit.

The script runs **only after the activation commits** (cost paid, cooldown
up). The game refuses the cast *before* paying when nothing is aimed or
the ground is already under water (a spring poured into a lake would be
swallowed).

At most **16** sources exist at once; the oldest is evicted. The spring's
water is the live water simulation: it flows downhill, pools, can be swum,
and dries when the source expires.

Default binding: **Q** on keyboard, **RB** on a pad (remappable in the
options screen, `spiritCast` in `settings.toml`).

## Console

```
spirit spawn Water <x> <z> [rate] [radius] [seconds]
spirit list
spirit clear
```

The console path needs no ability and no script — handy to test a spot,
then `Dump sim state` + `cooker water-replay` to replay what the
simulation did.

## Cues

When a source lands the game emits `Cue.Spirit.<Kind>.Spawn` at the spot
(magnitude = the rate). A `CueForm` with that tag — or the shorter
`Cue.Spirit` as a fallback — decides the particles, sound and shake.
