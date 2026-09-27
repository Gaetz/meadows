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

## Spells: a form applied to an element

A spirit power is a **spell**: one *form* (verb) applied to one *element*
(spirit), with its characteristics. Two records make a spell:

- an **`AbilityForm`** — the *activation*: cost and cooldown (effects),
  required/blocked tags, conditions, the skill it trains;
- a **`SpellForm`** — the *effect on the world*, a **child** of the
  ability (`parent` = the ability's guid).

| Field | Values | Meaning |
|---|---|---|
| `form` | `create` `destroy` `transform` `control` `understand` | the verb |
| `element` | a spirit name (`Water`, `Fire`…) | the subject |
| `trajectory` | `self` `point` `stream` `projectile` | on the caster / at the aimed ground / a continuous arc from the hand that follows the aim / one arc, the effect where it lands |
| `range` | metres | aim reach (`point`); ballistic reach at 45° for `stream`/`projectile` (20 m ⇒ 14 m/s) |
| `intensity` | element units per second (water: m³/s) | how strong |
| `duration` | seconds (`-1` = permanent) | how long the effect persists (simulation seconds) |
| `areaShape` + `areaRadius` | `disc` \| `ring`, metres | the footprint at the effect point |
| `channeled` + `costPeriod` | bool, seconds | hold the key to sustain; the cost is paid again every `costPeriod`; release ends it |

```toml
[[records]]                                   # CREATE + WATER at the aimed spot
form = "5b1e1700-0000-4000-8000-000000000081"
type = "SpellForm"
new = true
[records.fields]
editorId = "SpellWaterSpring"
parent = "5b1e1700-0000-4000-8000-000000000063"   # the SpiritWater ability
form = "create"
element = "Water"
trajectory = "point"
range = 24.0
intensity = 3.0
duration = 10.0
areaShape = "disc"
areaRadius = 4.0
```

Only some (form, element, trajectory) cells are implemented so far —
today, for **Water**: `create` as `point` (a spring) and `stream` (a jet
from the hand), `destroy` as `point` (a drain — on a lake the hole is
refilled by the lake at its weir rate), and `control` as `point`
(channeled: the aimed water is drawn into a floating volume that follows
the aim; release drops it where you look). A spell outside that set is
refused before any cost is paid. The base game ships `SpellWaterSpring`,
`SpellWaterStream` (the default: hold Q to keep streaming), `SpellWaterDrain`
and `SpellWaterHold`. The **mouse wheel** cycles the spell book (every
ability that carries a `SpellForm`); the current spell's `name` (a
LocString key) shows above the status bars.

The game refuses a `point` cast *before* paying when nothing is aimed,
the spot is beyond `range`, or the ground is already under water (a
spring poured into a lake would be swallowed).

### The script escape hatch

An ability **without** a `SpellForm` child runs its Lua `script` on
activation instead — for anything the spell matrix does not cover yet:

- `aim()` — the aimed ground point `{x, y, z}`, or `nil`;
- `spirit.spawn(kind, x, z, rate, radius, seconds)` — places a source
  (`seconds` in simulation time, `-1` permanent);
- `spirit.jet(kind, speed, rate, radius, seconds)` — a stream from the
  hand along the aim, simulated where it lands;
- `wait(t)` — the script is a coroutine.

At most **16** sources exist at once; the oldest is evicted. The spring's
water is the live water simulation: it flows downhill, pools, can be swum,
and dries when the source expires.

Default binding: **Q** on keyboard, **RB** on a pad (remappable in the
options screen, `spiritCast` in `settings.toml`).

## Console

```
spirit spawn Water <x> <z> [rate] [radius] [seconds]
spirit cast <AbilityEditorId>     # what Q casts: SpiritWaterJet (default) or SpiritWater
spirit list
spirit clear                      # sources and jets
```

The console path needs no ability and no script — handy to test a spot,
then `Dump sim state` + `cooker water-replay` to replay what the
simulation did.

## Cues

When a source lands the game emits `Cue.Spirit.<Kind>.Spawn` at the spot
(magnitude = the rate). A `CueForm` with that tag — or the shorter
`Cue.Spirit` as a fallback — decides the particles, sound and shake.
