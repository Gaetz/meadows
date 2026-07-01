# Phase 7 — Permanent-Status & Resonance Mechanics

> Brick journal. Read before touching stats, buildup, injuries, drugs, or afflictions.
> Design reference: `docs/STATS.md`. Modder reference: `docs/MODDING-EFFECTS.md`.

**Scope (decided, "value-first"):** foundations that Phase 6 left hard-coded, plus the novel
mechanics most at risk of design failure. The full secondary stat list, combat state machine
(shaken / critical-weakness / dismember), temperature / clothing, social reputation,
encumbrance, and the erudition curve were explicitly deferred to Phase 9.

**Final state:** 176 tests / 5087 assertions green after all post-phase additions.

---

## Brick 1 — `core::Rng` (seeded engine RNG)

**Commit:** `ab53eb7` — 2026-06-15

**File:** `engine/core/Rng.hpp`

All gameplay randomness must flow through a `core::Rng` instance passed by reference —
never a global, never wall-clock entropy — so saves and replays can reproduce (§8).

Generator: **xorshift64\*** (fast, good enough for gameplay; not crypto). State = one `u64`;
seed 0 is remapped to 1 (fixed point of xorshift). Public API:

```cpp
core::Rng rng(seed);
f64  x  = rng.unit();           // uniform [0, 1)
i32  n  = rng.range(lo, hi);    // uniform [lo, hi] inclusive
bool b  = rng.chance(p);        // true with probability p
u64  s  = rng.rawState();       // for save/replay serialization (Phase 8)
rng.setRawState(s);
```

**Non-obvious:** `0` is a fixed point — `seed(0)` silently becomes `seed(1)` to avoid
the degenerate zero stream. `rawState()` / `setRawState()` are the hooks for Phase 8
save serialization; they are no-ops until then.

**Test:** `tests/RngTest.cpp` — distribution, determinism, chance edge cases.

---

## Brick 2 — `StatsTuningForm` (moddable constants)

**Commit:** `1f0a198` — 2026-06-15

**Files:** `gameplay/stats/StatsTuning.hpp/.cpp`

All Phase 6 derived-stat formulas used hard-coded floats inside the lambda captures
of `registerCoreDerivedStats`. Phase 7 extracts every constant into a reflected Form
so mods can retune them through the §5 patch layer.

`StatsTuningForm` is a singleton Form with a canonical GUID, resolved at startup via
`resolveStatsTuning(forms)`. If the form is absent from the database the defaults
apply unchanged. The struct is then captured (by value) in the derived-stat lambdas
at `rebuild()` time — meaning a mod that changes a tuning constant takes effect on the
next `DerivedStatRegistry::rebuild()` call, not mid-frame.

**Key constants (defaults):**

| Group | Constant | Default |
|---|---|---|
| Primary maxima | `attributeToMax` | 5.0 — max = Σ(3 attrs) × 5 |
| Defense | `mitigationPerAttribute` | 0.5 |
| Posture | `basePosture`, `posturePerAlacrity` | 50, 1.0 |
| Regen | `energyRegenBase`, `healthRegenPerGrace`, `essenceRegenBase` | 35, 0.0002, 0.01 |
| Buildup | `enduranceBase`, `endurancePerAttribute` | 100, 0.5 |
| Status dmg | `poisonBaseDamagePerSecond`, `ignitionDamagePercent`, etc. | see file |
| Rest | `comfortableSleepHours` | 8.0 |
| Damage | `flatMitigationCapBase`, `staggerSeconds` | 25.0, 1.5 |
| Survival | `survivalThreshold`, `survivalResonanceAtEmpty` | 75, −50 |
| Survival decay | `hungerHoursPerPoint`, `thirstHoursPerPoint`, `sleepHoursPerPoint` | 0.96, 0.32, 0.72 |

**Test:** `tests/StatsTuningTest.cpp` — verify default values and that a patched form
changes derived output.

---

## Brick 3 — Equipment (WeaponForm / ArmorForm / ConsumableForm)

**Commit:** `6024b98` — 2026-06-15

**Files:** `data/forms/CoreForms.hpp/.cpp`, `gameplay/stats/EquipmentStats.hpp/.cpp`

Stat-bearing items are Forms (§2.2), so they layer and mod cleanly through §5.

### WeaponForm

Carries the attack side of the damage pipeline:
- Per-type attack channels: `slashAttack`, `pierceAttack`, `bluntAttack`, `fireAttack`,
  `lightningAttack`
- Scaling: `scalingAttribute` (name, e.g. `"strength"`) + `scalingK` (multiplier applied
  to the attribute's current value)
- `postureDamage` (how much posture it removes on hit)
- `weight`, `twoHanded`

### ArmorForm

Carries the defense side per equipment slot:
- Slot: `"head"`, `"torso"`, `"arms"`, `"legs"`
- Per-type mitigations: `armorSlash`, `armorBlunt`, `armorPierce`, `resistFire`,
  `resistLightning` (added to the actor's derived stats)
- `coldExposure`, `heatExposure` (climate hooks, used in Phase 9 temperature pass)
- `weight`

### ConsumableForm

- `category`: `"food"`, `"drug"`, `"treatment"`
- `effectGuid`: GUID of an `EffectForm` applied on use

### EquipmentStats

`gameplay/stats/EquipmentStats.hpp/.cpp` — translates equipped items into a
`StatModifiers` struct (add / multiply maps) that feeds Phase B of `recomputeStats`.
Multiple pieces of armor stack additively; weapon scaling adds once.

**Test:** `tests/EquipmentStatsTest.cpp`

---

## Brick 4 — Rest / sleep recovery

**Commit:** `9040dc4` — 2026-06-16

**Files:** `gameplay/stats/Rest.hpp/.cpp`

Rest is "time since last hit" — a precondition for injury and resonance recovery.
`applyDamage()` resets `CombatState.restSeconds` to 0; `accrueRest()` increments it
each game-time tick.

`sleep(clock, survival, combat, hours, tuning)` — advances the clock by `hours` in-game
hours, decays `hunger` and `thirst` over that window, restores the `sleep` need
(full at 8h via `comfortableSleepHours`, +`sleepPerHour` per hour below), and calls
`accrueRest` for the full slept window. Returns the in-game seconds elapsed (for callers
that need to tick other systems, e.g. injury recovery).

**Non-obvious:** `sleep()` deliberately does NOT call injury/resonance recovery directly
— those systems call `recoverInjuries()` / `tickResonanceDecays()` from `tickGameTime()`,
which receives the game-time delta from `sleep()`'s return value. Keeping sleep "dumb"
avoids circular includes and lets callers chain systems.

**Test:** `tests/RestTest.cpp`

---

## Brick 5 — Status buildup (9 types)

**Commits:** `9833ffc` (initial), `4e75305` (fixes including death / glaciation / electrocution)
— 2026-06-16 to 2026-06-17

**Files:** `gameplay/stats/StatusBuildup.hpp/.cpp`

Nine status types accumulate as float counters in `StatusBuildup` (reflected component,
serializes via §5):

```
Poison  Bleed   Mental    Disease  Curse
Death   Ignition  Glaciation  Electrocution
```

**Acquisition model:**
1. Weapons/effects call `tryAddBuildup(buildup, type, points, system, tags)`.
   This is a no-op if the `Status.*` tag is already present (can't re-acquire while active).
2. Each real-time tick, `tickBuildup()` decays flat `statusBuildupDecayFlat` pts/s before
   acquisition; once the status is active it decays `statusBuildupDecayPercent` %/s.
3. When `buildup >= endurance` the status triggers: its `Status.*` tag is granted;
   `bleed` and `death` also reset to 0 immediately.

**Per-type triggered behavior** (returned in `BuildupTickResult`, applied by caller):

| Type | Behavior on trigger |
|---|---|
| Poison | HP/s DoT = `poisonBaseDamagePerSecond × (1 − vitality/100)`; tag until buildup=0 |
| Bleed | One burst = `bleedBurstDamage` slash damage; resets to 0 immediately |
| Mental / Disease / Curse | Tag + passive decay; no automatic DoT |
| Death | `vitals.health → 0` immediately; `State.Dead` |
| Ignition | HP/s DoT = `ignitionDamagePercent × maxHealth`; reduced by `will` |
| Glaciation | Paralysis `glaciationParalysisDuration` seconds + energy regen ×`glaciationEnergyRegenMult` |
| Electrocution | Essence DoT + posture collapse to 0 + stagger on trigger |

**`buildupStatusModifiers()`** — injects regen multipliers into `StatModifiers` before
`recomputeStats` so that `currentValueOf("essenceRegen")` already reflects the glaciation
malus. Call in Phase B alongside resonance / equipment mods.

**Endurance formula:** `enduranceBase + endurancePerAttribute × linked_attribute`
(three groups: dex/ala for poison/bleed/mental/disease; per/ala for curse/death;
cha/ego/ins for elemental).

**Test:** `tests/StatusBuildupTest.cpp`

---

## Brick 6 — Body-part injuries

**Commit:** `8f2025d` — 2026-06-16

**Files:** `gameplay/stats/Injuries.hpp/.cpp`

Three injury types × four body parts × up to 3 severity levels (0 = light, 2 = severe).
Each injury inflicts a GAS effect (onyx resonance penalty + attribute malus + movement
speed penalty) with `gameTime = true, durationHours = recoveryHoursRemaining`.

**Types and severity:**

| Type | Max severity | onyx penalty (sev 0/1/2) | Attribute malus |
|---|---|---|---|
| Bruise | 1 | −1 / −2 | grace (head), str (torso), dex (arms), none (legs) |
| Cut | 2 | −1 / −2 / −4 | ala (head), con (torso), dex (arms), str (legs) |
| Fracture | 2 | −10 / −20 / −30 | ala (head), ego (torso), dex (arms), str (legs) + speed −10%/−25%/−40% |

**Infliction gate:** `rollInjury()` returns false (immune) if `onyx >= 0`. Otherwise rolls
`baseChance × |onyx| / 100` through `core::Rng`. Negative onyx = resonance dissonance
in the health channel = physical vulnerability.

**Aggravation:** `addInjury()` bumps `severity` if an injury of the same type and body
part already exists (capped at max), and resets the recovery timer.

**Sync pattern:** After any `addInjury()` or `recoverInjuries()` call, the caller must
call `syncInjuryEffects()`. This removes all `"Injury.Active"`-tagged GAS effects, then
re-applies effects for all current injuries. Full re-sync is cheap (a few iterations over
a short list) and avoids stale-effect bugs from partial updates.

**Recovery:** `recoverInjuries(injuries, restHours)` decrements each
`recoveryHoursRemaining`. When it reaches 0, severity drops by 1; the injury is removed
when severity goes below 0.

**Non-obvious:** `registerInjuryTags()` must be called before any scene that uses
injuries — it pre-registers the `"Injury.Active"` tag in the `GameplayTagRegistry`.

**Test:** `tests/InjuriesTest.cpp`

---

## Brick 7 — Afflictions / diseases (via EffectForm + gate)

**Commit:** `6eae98d` — 2026-06-16

**Files:** `gameplay/stats/Afflictions.hpp/.cpp`

Diseases and psychoses are expressed as `EffectForm` records in plugin data, not as
a separate C++ type. What `Afflictions.cpp` provides is the **infliction gate**:

```cpp
bool inflictEffect(AttributeSet& vitals, AbilitySystem& system,
                   const EffectForm& effect, f32 channelResonance,
                   f64 baseChance, core::Rng& rng,
                   const GameplayTagRegistry& tags);
```

- If `channelResonance >= 0` → immune (positive resonance is protective).
- Otherwise: `chance = baseChance × |channelResonance| / 100`. On success:
  - If the `effect.grantedTag` is already active → remove the existing effect first
    (re-infliction refreshes the duration rather than stacking).
  - Apply the effect via `applyEffect()`.

The `EffectForm` for a disease typically targets a resonance channel (`amber`) with
a negative magnitude and a second attribute malus (`attribute2`, e.g. `constitution`),
and uses `durationHours` for game-time recovery.

**`AfflictionForm` is gone.** There is no longer a separate form type for diseases.
Any `EffectForm` with `durationHours > 0` and a `grantedTag` can serve as a disease/
affliction — fully data-driven and moddable via §5.

---

## Brick 8 — Drugs + harmony break

**Commit:** `a251c86` (initial), `2508d56` (API cleanup + tests) — 2026-06-16 to 2026-06-21

**Files:** `gameplay/stats/Drugs.hpp/.cpp`

Drugs are `EffectForm` records:
- `attribute = "amber"` (or another channel) with a large positive `magnitude`
- `durationHours > 0` for game-time duration
- `grantedTag = "Status.HarmonyBroken"` to disable the Harmony cascade
- `expiryMode = "decay"`, `expiryMagnitude` = negative aftershock start value,
  `decayPerHour` = recovery speed

When the effect expires, a `ResonanceDecay` entry is registered (from the pre-scan in
`tickCharacter` / `tickGameTime`): the aftershock fades gradually toward 0 over subsequent
game-time ticks, rather than snapping to 0.

**`DrugForm` is gone.** `Drugs.hpp` retains only:
```cpp
bool harmonyBroken(const AbilitySystem& system, const GameplayTagRegistry& tags);
```
This query is called by `buildCharacterMods()` to decide whether to run the Harmony
cascade or treat channels as independent.

**Non-obvious:** Harmony break is checked **per-frame in Phase B** (`buildCharacterMods`).
The drug doesn't set a field on `Resonance` — it sets a tag. The tag is what gates the
cascade. This means no special-casing in the resonance pipeline: the cascade path
simply skips when `harmonyBroken()` is true.

---

## Brick 9 — Bug fixes + GameTime (game-clock tick integration)

**Commits:** `4e75305`, `563ccc6` — 2026-06-17

**Files:** `gameplay/stats/GameTime.hpp/.cpp`, `gameplay/stats/Damage.hpp`, `gameplay/stats/StatusBuildup.hpp/.cpp`

Several issues surfaced when testing the full tick loop end-to-end:

1. **StatusBuildup: death, glaciation, electrocution** not triggering — missing branches
   in `tickBuildup`. Added `BuildupTickResult.deathTriggered`, `glaciationTriggered`,
   `electrocutionTriggered` as distinct flags.

2. **`GameTime.cpp` — the `tickGameTime` / `advanceGameTime` architecture:**
   - `tickGameTime(args, gameDt, mods)` handles one chunk of game-time: health/essence
     regen, survival decay, survival effect sync, pre-scan of expiring resonance effects,
     `tickGameTimeEffects` (game-time GAS effects), resonance decay tick, injury recovery
     + sync, rest accumulation, final `recomputeStats`.
   - `advanceGameTime(args, gameDt, timescale, equipmentMods)` splits the total
     game-time into 10-second real-time chunks to avoid one massive tick missing buildup
     triggers. Each chunk runs the 3-phase recompute, ticks buildup, then calls
     `tickGameTime`.

3. **`State.Dead` / `updateLifeState`:** `applyDamage` sets health to 0 but didn't
   always set `State.Dead`. `updateLifeState(system, tags)` was added in `Combat.hpp`
   and called at the end of `applyDamage` and at the end of `initializeActorStats`
   (to clear the tag on Heal Full).

4. **`durationHours` implicit override bug (fixed later, see post-phase):**
   `applyEffect()` had an early-return for `DurationPolicy::Instant` that fired *before*
   the `isGameTime = (durationHours > 0)` check. Effects with `durationHours > 0` but
   default `duration = "instant"` were applied as instant base-value changes. Fixed by
   moving the `isGameTime` detection before the instant-path guard.

---

## Post-phase additions (2026-06-21 to 2026-07-01)

These were identified during StatsScene testing after the Phase 7 test baseline
(172 / 5071) was set. They are considered part of Phase 7's feature set.

### Survival → energy canal (c2181a0)

The original survival model drove `amber` resonance directly. Corrected to match
`docs/STATS.md`: hunger and thirst drain the **energy** (`amber`) channel resonance;
sleep drains the **essence** (`garnet`) channel.

### Harmony fallback correction (2508d56)

When all three resonance channels are at 0, the original fallback selected channel 0
(onyx) as the "dominant". Corrected: if no channel has a non-zero value there is no
dominant channel and the cascade result is all-zeros (no penalty, no bonus). Prevents
a spurious cascade from a clean-slate actor.

### Better regen modifiers — `buildupStatusModifiers` (8ec38eb)

Glaciation energy regen malus and electrocution essence regen malus were not flowing
through `buildupStatusModifiers` into `StatModifiers.mul`. Fixed: the function now
injects the appropriate multipliers for active statuses before Phase C `recomputeStats`.

### CharacterTick extraction (d6f1492)

All per-frame character logic (`tickCharacter`, `initializeActorStats`) was extracted
from `game/scenes/DemoScenes.cpp` into:
- `gameplay/actors/CharacterTick.hpp/.cpp` — the tick + initialization pipeline
- `game/ui/CharacterStatsPanel.hpp/.cpp` — the ImGui stats display

This was a prerequisite for Phase 8 (the scene needs to instantiate multiple actors
without copy-pasting the tick logic).

### GAS unification — ResonanceDecays + full EffectForm routing (a719d52 + session fixes)

- **`ResonanceDecays.hpp/.cpp`** added: transient resonance fade entries populated
  when a duration GAS effect with `expiryMode = "decay"` expires. `addResonanceDecayToResonance()`
  folds them into the resonance read in Phase B; `tickResonanceDecays()` advances
  the fade each game-time tick.
- **`AfflictionForm` and `DrugForm` types removed.** All afflictions and drugs are now
  `EffectForm` records routed through `applyEffect`. The gate (`inflictEffect` for
  afflictions; the `"Status.HarmonyBroken"` tag + `expiryMode="decay"` for drugs) is
  still C++ but the data is fully moddable.
- **`applyEffect()` bug fix** (`durationHours` implicit override): moved the
  `isGameTime = (effect.durationHours > 0.0f)` detection before the `Instant`
  early-return so effects with `durationHours > 0` always land in `activeEffects`
  as game-time duration effects.
- **`tickGameTime` end-of-function `recomputeStats`** added: `tickGameTimeEffects` and
  `syncInjuryEffects` internally call the 2-arg `recomputeCurrent` (which lacks
  `CoreAttributes` and so overwrites derived fields like `maxHealth` with struct
  defaults). The final `recomputeStats` call at the end of `tickGameTime` re-syncs all
  derived stats correctly.

---

## Decisions to remember

**`syncInjuryEffects` is a full rebuild, not incremental.**
Attempting an incremental "only update changed injuries" was considered but rejected.
The list is always short (< 10 entries), and incremental sync would require tracking
which `effectId` belongs to which `Injury` rank. A full rebuild from scratch on any
change is simpler, cheaper to reason about, and correct by construction.

**Buildup triggers use the tag as the "already active" sentinel.**
`tryAddBuildup` checks `system.tags.has(statusTag)` rather than `buildup.value > threshold`.
This means: during the decay-back-to-zero phase after a status is acquired, the tag
prevents re-accumulation. The tag is removed when buildup reaches 0, re-opening the
acquisition window.

**`inflictEffect` removes-and-reapplies, not stacks.**
On a second infliction while the status is active, the existing effect (identified by
`grantedTag`) is removed first, then the fresh effect is applied. This resets the timer
rather than stacking a second penalty. This matches Skyrim disease behavior and prevents
exploit-stacking.

**`StatsTuningForm` is captured at `rebuild()` time, not per-frame.**
The derived-stat lambdas capture a copy of `StatsTuningForm` when `DerivedStatRegistry::rebuild()`
is called. Changing a tuning constant mid-run requires calling `rebuild()` again. This
is intentional: derived-stat computation must be cheap per-frame (no form lookup).

**`durationHours > 0` implicitly makes an effect game-time.**
Even if `duration = "instant"` is left as the default, setting `durationHours > 0` on
an `EffectForm` routes the effect into `activeEffects` as a game-time duration effect.
This simplifies the data interface for modders who only need to think about "this wears
off after N in-game hours."
