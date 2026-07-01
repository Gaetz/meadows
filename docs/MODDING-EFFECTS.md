# Modding Guide — GameplayEffects & Abilities

This guide covers everything you need to create spells, weapons, potions, buffs, debuffs,
afflictions, and abilities as data — **no C++ required**.

All stat modifications in this engine go through `EffectForm`. Whether you are nerfing
armor, adding a poison blade, creating a disease, or designing an ultimate ability,
one record type handles it all.

---

## Table of Contents

1. [How plugins work](#1-how-plugins-work)
2. [Attributes — what you can target](#2-attributes--what-you-can-target)
3. [EffectForm — the building block](#3-effectform--the-building-block)
4. [AbilityForm — activatable actions](#4-abilityform--activatable-actions)
5. [Tags — state, conditions, immunity](#5-tags--state-conditions-immunity)
6. [Recipes — common use cases](#6-recipes--common-use-cases)
7. [Reference tables](#7-reference-tables)

---

## 1. How plugins work

A plugin is a `.toml` file with a required header and any number of **records**.
Each record either creates a new form (`new = true`) or patches fields of an existing
one (just reference its GUID without `new`).

```toml
[plugin]
id   = "aaaaaaaa-0000-4000-8000-000000000001"   # Your unique plugin GUID
name = "my-cool-mod"

[[records]]
form = "bbbbbbbb-0000-4000-8000-000000000001"   # This record's unique GUID
type = "EffectForm"
new  = true
[records.fields]
editorId  = "MyPotion_Heal"    # Human-readable ID (unique within your plugin)
attribute = "health"
op        = "add"
magnitude = 30.0
duration  = "instant"
```

**Key rules:**
- Every GUID must be unique across all loaded plugins — generate one with `uuidgen` or
  any online UUID4 generator.
- Omitting `new = true` means the record is a **patch**: it only overrides the listed
  fields of an existing form, leaving everything else untouched.
- Unknown fields are warned and skipped — the plugin still loads.
- Load order: later plugins override earlier ones, **field by field** (no whole-record
  replacement). Two mods touching different fields of the same effect never conflict.

---

## 2. Attributes — what you can target

These are all the attribute names you can use in `attribute` / `attribute2`.

### Vitals (primary resources)
| Name | Description |
|---|---|
| `health` | Current health points |
| `energy` | Current energy points |
| `essence` | Current essence (mana) points |
| `damage` | **Meta-attribute** — write here for damage effects; engine routes it to health via armor reduction |

### Primary maxima (derived from CoreAttributes)
| Name | Formula |
|---|---|
| `maxHealth` | (strength + constitution + grace) × 5 |
| `maxEnergy` | (dexterity + alacrity + perception) × 5 |
| `maxEssence` | (charisma + ego + insight) × 5 |

### The nine base attributes
These feed all derived stats. Resonance channel bonuses also add to them.

| Name | Default | Linked vital |
|---|---|---|
| `strength`     | 6 | health |
| `constitution` | 6 | health |
| `grace`        | 6 | health |
| `dexterity`    | 6 | energy |
| `alacrity`     | 6 | energy |
| `perception`   | 6 | energy |
| `charisma`     | 6 | essence |
| `ego`          | 6 | essence |
| `insight`      | 0 | essence |

### Defensive stats (derived)
| Name | Formula |
|---|---|
| `defense`      | 0.5 × constitution |
| `armorSlash`   | 0.5 × strength |
| `armorBlunt`   | 0.5 × constitution |
| `armorPierce`  | 0.5 × grace |
| `resistFire`   | 0.5 × charisma |
| `resistCold`   | 0.5 × ego |
| `resistLightning` | 0.5 × insight |
| `will`         | 1 + ego × 0.25 |
| `criticalSensitivity` | 25 − constitution × 0.1 |

### Regeneration & posture
| Name | Formula |
|---|---|
| `maxPosture`   | 50 + alacrity × 1 |
| `postureRegen` | 2 + alacrity × 0.333 |
| `energyRegen`  | 35 + alacrity × 1 (pts/s, real-time) |
| `healthRegen`  | grace × 0.0002 (pts/s, game-time) |
| `essenceRegen` | 0.01 + insight × 0.0025 (pts/s, game-time) |
| `movementSpeed`| 90 + alacrity + strength |

### Resonance channels
These are the hidden synchronization values driving cascade modifiers.
Negative = dissonance (bad); positive = resonance (good).

| Name | Linked vital | Effect per point |
|---|---|---|
| `onyx`   | health  | Attribute offset: onyx÷15 added to str/con/grace; maxHealth scaled by (1+onyx÷100) |
| `amber`  | energy  | Same for dex/ala/per and maxEnergy |
| `garnet` | essence | Same for cha/ego/ins and maxEssence |

### Status endurance thresholds
These are the accumulation caps for status effects. A mob with high dexterity is harder
to poison. You rarely need to target these directly.

`endurancePoison`, `enduranceBleed`, `enduranceMental`, `enduranceDisease`,
`enduranceCurse`, `enduranceDeath`, `enduranceIgnition`, `enduranceGlaciation`,
`enduranceElectrocution`

---

## 3. EffectForm — the building block

Every stat modification is an `EffectForm`. Here is the complete field reference.

### Core fields

| Field | Type | Default | Description |
|---|---|---|---|
| `editorId` | string | — | Human-readable name for your effect (unique in your plugin) |
| `attribute` | string | `""` | Target attribute name (see §2). Required for stat effects. |
| `op` | string | `"add"` | How to combine: `"add"`, `"multiply"`, `"override"` |
| `magnitude` | float | `0.0` | Value applied by `op`. Negative for penalties. |
| `attribute2` | string | `""` | Optional second attribute (e.g. affliction attribute malus) |
| `magnitude2` | float | `0.0` | Magnitude for `attribute2` |

**How `op` works:**
```
add       → currentValue = (base + Σ all additive magnitudes) × Π multipliers
multiply  → multiplied together: two ×0.9 effects give ×0.81 total
override  → ignores base and all other modifiers (last override wins)
```

### Duration fields

| Field | Type | Default | Description |
|---|---|---|---|
| `duration` | string | `"instant"` | `"instant"`, `"duration"`, `"infinite"`, `"periodic"` |
| `durationSeconds` | float | `0.0` | Real-time seconds (for `"duration"`) |
| `durationHours` | float | `0.0` | **Game-time hours** (for `"duration"`). Overrides `durationSeconds` if > 0. |
| `period` | float | `0.0` | For `"periodic"`: interval in real-time seconds between base-value hits |

**Duration modes:**

| Mode | What it does |
|---|---|
| `"instant"` | Changes the BaseValue immediately and permanently. Use for potions, level-ups, damage. |
| `"duration"` | Adds a temporary modifier for `durationSeconds` seconds (real-time) or `durationHours` game-hours. |
| `"infinite"` | Permanent modifier while the effect is active (until removed explicitly). Use for equipment, diseases. |
| `"periodic"` | Ticks every `period` seconds, each time applying `magnitude` to BaseValue. Use for DoT/HoT. |

> **Tip:** `durationHours > 0` automatically makes the effect game-time (afflictions wear off
> while the player rests or advances time, not while they stand still in real time).

### Tag fields

| Field | Type | Default | Description |
|---|---|---|---|
| `grantedTag` | string | `""` | Tag added to the target while a duration/infinite effect lasts. Removed on expiry. |
| `requiredTag` | string | `""` | Effect only applies if the target **has** this tag (ancestor-aware). |
| `blockedTag` | string | `""` | Effect is blocked (immunity) if the target **has** this tag. |

### Resonance decay fields
*Only relevant when `attribute` is `"onyx"`, `"amber"`, or `"garnet"`.*

| Field | Type | Default | Description |
|---|---|---|---|
| `expiryMode` | string | `"immediate"` | `"immediate"` or `"decay"`. With `"decay"`, the channel value fades toward 0 after expiry instead of snapping. |
| `decayPerHour` | float | `1.0` | Speed of the fade in points per game-hour. |
| `expiryMagnitude` | float | `0.0` | Starting value of the decay (if different from `magnitude`). Useful for drugs: the aftershock starts at a different value than the high. |

### Status buildup routing

| Field | Type | Default | Description |
|---|---|---|---|
| `buildupType` | string | `""` | If non-empty, routes this effect to the status accumulator instead of applying it as a stat modifier. |

Valid values: `"poison"`, `"bleed"`, `"mental"`, `"disease"`, `"curse"`,
`"death"`, `"ignition"`, `"glaciation"`, `"electrocution"`.

When `buildupType` is set, `attribute`, `op`, `magnitude2`, and duration fields are
**ignored** — the engine adds `magnitude` to the accumulator. When it reaches the
target's endurance threshold, the corresponding status is triggered.

---

## 4. AbilityForm — activatable actions

An ability is a data record that references up to three `EffectForm` GUIDs.

| Field | Type | Default | Description |
|---|---|---|---|
| `editorId` | string | — | Human-readable name |
| `requiredTag` | string | `""` | Caster must have this tag to activate |
| `blockedTag` | string | `""` | Caster must NOT have this tag (stun, freeze, etc.) |
| `cost` | GUID | — | `EffectForm` applied to **self** on activation (energy cost, etc.) |
| `cooldown` | GUID | — | `EffectForm` applied to **self**; its `grantedTag` is the cooldown gate |
| `effect` | GUID | — | `EffectForm` applied to the **target** |

**Cooldown pattern:** The cooldown effect grants a tag while active. The ability checks
that the caster does NOT have that tag before activating. When the duration expires,
the tag is removed and the ability is usable again.

```toml
# Cooldown effect: 2s, grants a gate tag while active
[[records]]
form = "cc000001-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId  = "Fireball_Cooldown"
duration  = "duration"
durationSeconds = 2.0
grantedTag = "Cooldown.Fireball"

# Ability: blocked while the cooldown tag is present
[[records]]
form = "ab000001-0000-4000-8000-000000000001"
type = "AbilityForm"
new = true
[records.fields]
editorId    = "Fireball"
blockedTag  = "Cooldown.Fireball"
cooldown    = "cc000001-0000-4000-8000-000000000001"
effect      = "<your-fireball-damage-effect-guid>"
```

---

## 5. Tags — state, conditions, immunity

Tags are hierarchical string labels (`Parent.Child.Leaf`) shared across all systems:
ability gates, dialogue conditions, immunity checks, and quest logic.

### Built-in engine tags
These are registered by the engine and used internally. You can check them
in `grantedTag`, `requiredTag`, and `blockedTag` conditions.

| Tag | Meaning |
|---|---|
| `State.Dead` | Actor is dead (health = 0) |
| `State.Staggered` | Posture broken; briefly vulnerable |
| `State.Paralyzed` | Glaciation freeze; cannot act |
| `Status.Poisoned` | Poison accumulator triggered |
| `Status.Bleeding` | Bleed accumulator triggered |
| `Status.Mental` | Mental accumulator triggered |
| `Status.Diseased` | Disease accumulator triggered |
| `Status.Cursed` | Curse accumulator triggered |
| `Status.Dying` | Death accumulator triggered |
| `Status.Ignited` | Ignition accumulator triggered |
| `Status.Glaciated` | Glaciation freeze triggered |
| `Status.Electrocuted` | Electrocution triggered |
| `Status.HarmonyBroken` | Drug harmony break active |

### Creating your own tags

Tags used in your effects (via `grantedTag`, `requiredTag`, `blockedTag`) must be
**registered** before use. Right now this is done in C++ at scene startup. Future
versions will auto-register from plugin data.

By convention, namespace your tags with your mod name to avoid conflicts:
`MyMod.Buff.Haste`, `MyMod.Status.Cursed.Shadow`, etc.

---

## 6. Recipes — common use cases

### Instant heal (potion)

```toml
[[records]]
form = "aa000001-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId  = "PotionHealing_Small"
attribute = "health"
op        = "add"
magnitude = 30.0
duration  = "instant"
```

`"instant"` adds directly to the BaseValue of health. Use `"damage"` instead if you
want armor to apply (the damage meta-attribute route applies mitigation).

---

### Instant damage (bypasses armor)

```toml
[[records]]
form = "aa000002-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId  = "TrueDamage_20"
attribute = "health"
op        = "add"
magnitude = -20.0
duration  = "instant"
```

To make damage go through armor mitigation, target `"damage"` instead:

```toml
[records.fields]
editorId  = "PhysicalDamage_20"
attribute = "damage"     # routes through armor/defense
op        = "add"
magnitude = 20.0
duration  = "instant"
```

---

### Timed real-time buff (e.g. speed potion, 30 s)

```toml
[[records]]
form = "aa000003-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId        = "SpeedPotion"
attribute       = "movementSpeed"
op              = "multiply"
magnitude       = 1.30       # +30% movement speed
duration        = "duration"
durationSeconds = 30.0
grantedTag      = "Status.Hasted"   # shows in UI while active
```

---

### Disease / affliction (game-time hours)

Afflictions reduce a resonance channel AND an attribute, lasting many in-game hours.
The effect wears off while resting or advancing time, not while standing still.

```toml
[[records]]
form = "aa000004-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId      = "Disease_RottingLung"
attribute     = "amber"       # amber = energy channel resonance
op            = "add"
magnitude     = -20.0         # energy channel penalty
attribute2    = "constitution"
magnitude2    = -3.0          # constitution malus
durationHours = 72.0          # 72 game-hours to recover
grantedTag    = "Status.Diseased.RottingLung"
```

> Gate on `blockedTag = "Status.Diseased.RottingLung"` on the infliction ability so the
> player cannot stack the same disease.

---

### Drug / stimulant (harmony break + aftershock)

A drug boosts a resonance channel while breaking the Harmony cascade (channels become
independent). When it wears off, the aftershock decays slowly.

```toml
[[records]]
form = "aa000005-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId         = "Drug_CrimsonDust"
attribute        = "amber"        # amber channel boost
op               = "add"
magnitude        = 80.0           # large energy resonance spike
durationHours    = 3.0            # 3 game-hours of high
grantedTag       = "Status.HarmonyBroken"   # breaks the cascade
expiryMode       = "decay"
expiryMagnitude  = -40.0          # aftershock starts at -40 amber
decayPerHour     = 2.0            # recovers 2 pts/game-hour
```

---

### Equipment: permanent armor buff (infinite, removed when unequipped)

```toml
[[records]]
form = "aa000006-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId  = "Armor_PlateChest_Slash"
attribute = "armorSlash"
op        = "add"
magnitude = 25.0
duration  = "infinite"
```

Infinite effects are added when equipping the item and removed when unequipping.
Reference this GUID from your `ArmorForm`.

---

### Status buildup (poison weapon, poison trap)

```toml
[[records]]
form = "aa000007-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId    = "PoisonCoating_30"
buildupType = "poison"
magnitude   = 30.0         # 30 points of poison accumulation per hit
duration    = "instant"
```

When the target's accumulated poison exceeds their `endurancePoison`, the
`Status.Poisoned` tag is applied and DoT begins.

---

### Passive regeneration tick (heal-over-time, 5 pts every 2 s)

```toml
[[records]]
form = "aa000008-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId  = "HealOverTime_5pt_2s"
attribute = "health"
op        = "add"
magnitude = 5.0
duration  = "periodic"
period    = 2.0            # heal 5 hp every 2 real seconds
```

Periodic effects add to the BaseValue on each tick. They are NOT temporary modifiers
(they stack permanently if not removed). Use a `grantedTag` and a duration to bound them.

---

### Resistance override (boss immunity to fire)

```toml
[[records]]
form = "aa000009-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId  = "BossFireImmunity"
attribute = "resistFire"
op        = "override"
magnitude = 100.0          # 100% fire resistance
duration  = "infinite"
```

Override wins over all other modifiers. Use it for hard immunity or special cases.

---

### Ability with energy cost + cooldown + damage

```toml
# 1. Energy cost: -25 energy (instant, applied to self)
[[records]]
form = "ab100001-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId  = "Slam_Cost"
attribute = "energy"
op        = "add"
magnitude = -25.0
duration  = "instant"

# 2. Cooldown: 3-second gate (applied to self)
[[records]]
form = "ab100002-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId        = "Slam_Cooldown"
duration        = "duration"
durationSeconds = 3.0
grantedTag      = "Cooldown.Slam"

# 3. Damage effect: 35 physical damage to target
[[records]]
form = "ab100003-0000-4000-8000-000000000001"
type = "EffectForm"
new = true
[records.fields]
editorId  = "Slam_Damage"
attribute = "damage"
op        = "add"
magnitude = 35.0
duration  = "instant"

# 4. Ability: wires them together
[[records]]
form = "ab100010-0000-4000-8000-000000000001"
type = "AbilityForm"
new = true
[records.fields]
editorId   = "Slam"
blockedTag = "Cooldown.Slam"   # can't use while on cooldown
cost       = "ab100001-0000-4000-8000-000000000001"
cooldown   = "ab100002-0000-4000-8000-000000000001"
effect     = "ab100003-0000-4000-8000-000000000001"
```

---

### Patching an existing effect (nerfing base-game damage)

You don't need `new = true` — just list the GUID of the existing effect and the
fields you want to change:

```toml
[[records]]
form = "e0000000-0000-4000-8000-000000000001"   # StrikeDamage from base game
type = "EffectForm"
[records.fields]
magnitude = 7.0    # reduced from 10 to 7; all other fields unchanged
```

---

## 7. Reference tables

### `op` values

| Value | Formula | Use for |
|---|---|---|
| `"add"` | `base + Σ(magnitudes)` | Damage, healing, flat bonuses/penalties |
| `"multiply"` | `value × Π(magnitudes)` | % speed changes, resistance scaling, stacking multipliers |
| `"override"` | Fixed value, ignores everything else | Hard immunity (100% resist), locked stats |

### `duration` values

| Value | Ticked by | `durationSeconds` / `durationHours` |
|---|---|---|
| `"instant"` | — (applies once) | Ignored |
| `"duration"` | `tickEffects` (real-time) or `tickGameTimeEffects` (if `durationHours > 0`) | Required |
| `"infinite"` | Never expires | Ignored |
| `"periodic"` | `tickEffects` every `period` seconds | Ignored; use `period` instead |

### `buildupType` values and their thresholds

| Value | Threshold stat | Status granted | Effect when triggered |
|---|---|---|---|
| `"poison"` | `endurancePoison` | `Status.Poisoned` | Ongoing health DoT (reduced by vitality %) |
| `"bleed"` | `enduranceBleed` | `Status.Bleeding` | One-shot burst (30 slash damage, tunable) |
| `"mental"` | `enduranceMental` | `Status.Mental` | Persistent status with gradual decay |
| `"disease"` | `enduranceDisease` | `Status.Diseased` | Persistent status with gradual decay |
| `"curse"` | `enduranceCurse` | `Status.Cursed` | Persistent status with gradual decay |
| `"death"` | `enduranceDeath` | — | Instant death (health → 0) |
| `"ignition"` | `enduranceIgnition` | `Status.Ignited` | Ongoing health DoT (reduced by will %) |
| `"glaciation"` | `enduranceGlaciation` | `Status.Glaciated` | Paralysis 3 s + energy regen penalty |
| `"electrocution"` | `enduranceElectrocution` | `Status.Electrocuted` | Essence DoT + posture drain + stagger |

### Resonance channels at a glance

| Channel | Stat | Attributes affected | Max vital scaled |
|---|---|---|---|
| `onyx` | health | strength, constitution, grace | maxHealth |
| `amber` | energy | dexterity, alacrity, perception | maxEnergy |
| `garnet` | essence | charisma, ego, insight | maxEssence |

Each point of resonance = +1/15 attribute bonus ≈ +0.067 per point.
Each point of resonance = ±1% vital maximum.

### Common mistake checklist

| Mistake | Fix |
|---|---|
| Effect applies once then disappears | Change `"instant"` to `"infinite"` or `"duration"` |
| Duration effect never wears off | Make sure `durationSeconds > 0` or `durationHours > 0` |
| Drug effect doesn't break Harmony | Set `grantedTag = "Status.HarmonyBroken"` |
| Affliction wears off in real time, not game time | Use `durationHours` instead of `durationSeconds` |
| Two copies of same affliction stack | Add `blockedTag = "<your-effect-tag>"` on the ability / affliction |
| Armor not applying to damage | Target `"damage"` (meta-attribute route) not `"health"` |
| Multiply doesn't feel right | Remember all multipliers chain: ×1.5 × ×1.5 = ×2.25 |
| Effect applies to wrong actor | Cost/cooldown apply to **self**; `effect` applies to **target** |
