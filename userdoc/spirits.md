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
| `channeled` + `costPeriod` + `upkeepScale` | bool, seconds, fraction | hold the key to sustain; every `costPeriod` seconds `upkeepScale` × the activation cost is paid again (default 0.25); release ends it |

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
refilled by the lake at its weir rate), `control` as `point`
(channeled: the aimed water is drawn into a floating volume that follows
the aim; release drops it where you look), and `understand` as `point`
(a reading of the aimed water — body, depth, current, volume — or, on
dry ground, where the nearest water lies; `duration` is how long the
block stays on the HUD). For **Earth**: `create` as `point` raises an
instant mound of `intensity` metres (divided by the ground's hardness, a
`SurfaceMaterialForm` field) that throws whoever stands on it, and
`destroy` as `point` (channeled) digs `intensity` metres per second under
the aim while the key is held — water reacts live, so a mound in front of
a spring dams it and a trench drains a pool. `create` channeled is the
stone brush (a ridge follows the aim), `create` as `line` a straight wall
from the press spot to the release spot, `understand` reads the ground,
and `control` (channeled) seizes the nearest rock prop no bigger than
`intensity` metres — a `StaticForm` whose `surfaceMaterial` is `"rock"` —
carries it over the aim and lets it fly and roll on release (a real
physics body from then on). For **Fire**: `create` as `point` is the
spark — `intensity` heat dealt to every fire-field cell within
`areaRadius` of the aimed spot; a cell ignites once its heat reaches the
spirit's `ignitionPoints`, then burns its ground's `fuel` seconds (a
`SurfaceMaterialForm` field, blended over the splat under it) while
dealing `spreadRate` heat per second to its eight neighbours, damped by
their `moisture`; bare rock, sand and snow carry no fuel, standing water
puts a burning cell out and keeps it out. Rain counts as moisture (the
wetter of the two) and, from half strength up, soaks burning cells out
in a few seconds. Burnt ground stays charred and its grass is gone; the
field lives in a 512 m window around the camera and **is saved**: a
`FireStateForm` record in the save carries the burning and burnt cells,
and the next visit relights them (a mod could ship one too — a
smouldering battlefield). The fire is dangerous: whoever stands in the flames
takes the spirit's `contactEffect` every `contactPeriod` seconds — an
ignition buildup that ends in `Status.Ignited` (damage over time, armour
resistances apply). Wooden props (a `StaticForm` whose `surfaceMaterial`
is `"wood"`: the village's crates, wagons and fences) heat up in the
flames, burn for their material's `fuel` seconds, set the ground around
them alight and are gone for good (disabled in the save, like a picked-up
item). Burnt ground is not forever: a burnt cell regrows over the
spirit's `regrowSeconds` (dry ground; wet ground up to four times
faster), its char fading back to green and its grass returning, and it
can burn again. Trees are never removed by the fire: a tree whose
surroundings burn heats up (`treeIgnitionSeconds` in full fire), then
burns for `treeBurnSeconds` with flames at its trunk, shedding embers
around it, its canopy falling progressively like in winter and its wood
charring; it then stands bare and grows its foliage back over
`treeRegrowSeconds`. Villagers run from burning ground (any peaceful
actor within 6 m of flames drops what he was doing and runs the other
way until the ground is safe). Fire the world carries: a `StaticForm`
may declare a `light` (a `LightForm`, at `lightOffset`) and
`flameParticles` / `smokeParticles` (`ParticleForm`s, at
`particlesOffset`), `hazeParticles` (a `"haze"`-blend `ParticleForm`: the
air shimmers above it), a looped `sound` (a hearth's crackle, 3D) and
`igniteRadius` / `igniteHeat` — a prop whose flame sets alight the wooden
props and trees within that radius, and the ground too when the flame
stands within a metre of it. The spawner attaches them and the scene
keeps them alight within 90 m; the base data ships a `Campfire` (safe in
its stones) and a `Torch` (its flame, 1.6 m up, lights wood and trees
within a metre but spares the grass) on the village square. The fire
reading also names the wind at the aimed spot (still air, or its speed
and bearing). The other fire spells: `destroy` as
`point` puts the flames out around the aim (the cells keep their fuel),
`destroy` as `self` held is the fire ward (nothing burns within
`areaRadius` of you and you feel no flame while you hold it), `control`
as `point` held is the firebrand (a flame at the aim that lights the
ground under it, never on water), `understand` reads the aimed cell, its
fuel and the nearest fire front; `create` as `stream` held is the flame
jet — a short cone of flames ahead of you (`range`), the ground under it
catches (so do the props and trees standing there) and whoever stands in
it burns. For **Wind**: `create` as `point` places a gust at the aimed
spot (a spirit source like a spring: `intensity` m/s, `areaRadius`,
`duration`, blowing where you faced when you cast), `create` as `stream`
held is the breath (a gust carried ahead of you), `destroy` on `self`
held calms the weather's wind while you concentrate, and `control` on
`self` held steers it where you face. Gusts push you and the villagers,
carry particles and bend the fire, and floating props drift with the
wind; the weather's ambient wind never pushes anyone. A spell outside that set is
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
