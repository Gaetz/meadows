# Phase 6 — Character stats: vertical slice (DONE 2026-06-15)

> Journal of the Phase 6 implementation, same role as `docs/PHASE-1..5.md`. The
> **design** (what the stats are) lives in `docs/STATS.md` — read that first. This
> is the build journal: what each brick delivered and the non-obvious decisions.
> Read before touching `gameplay/stats/`.

Phase 6 is the load-bearing core of the character-stats design (`docs/STATS.md`),
validated in 2D before streaming (Phase 8) and 3D (Phase 9). It builds the new
machinery the Phase-3 GAS deferred (§6: "custom execution calculations only when
a concrete case needs them") and the signature **Resonance/Harmony** mechanic.
The long tail (injuries, full secondary list, social/reputation, drugs, climate,
permanent statuses, erudition) is **Phase 7**.

## Architecture decided with the dev

- **Periodic decisions** (from `docs/STATS.md`): vertical slice (core) not the
  full sheet; derived stats via **C++ calculators + data constants** (§2.7/§6),
  not Lua/data; a minimal **game clock** introduced now; the hidden stat named
  **Resonance** (signed; negative = dissonance).
- **Multiple AttributeSets.** The Phase-3 `recomputeCurrent` iterated one set and
  clamped three vitals; generalized to aggregate the f32 fields of **several**
  reflected sets into the `AbilitySystem::current` overlay (keyed by `fnv1a`(field
  name); names kept globally unique → no collision).
- **Derived pass = C++ calculators, opt-in.** A `DerivedStatRegistry` of
  `{ target, sourceSet, formula(StatView) }`. `recomputeCurrent` runs two passes:
  non-derived fields (base + modifiers), then derived fields (formula +
  modifiers), then clamp. A calculator runs **only when its source set is
  present**, so the Phase-3 combat (TrainingDummy, no CoreAttributes) is unchanged.
- **Override for non-humanoids = an infinite Override effect** (§2.9), not a new
  field: the effect modifier wins the aggregation, so a monster/legendary pins a
  derived stat past the humanoid formula. (Raised by the dev: formulas assume
  attributes 1–20.)
- **Primary maxima read BASE attributes; secondary stats read CURRENT.** Decided
  mid-flight (S2): Resonance offsets the attributes' *current*, so if the maxima
  read current they'd be hit twice (the % **and** the lowered attributes). Maxima
  derive from the *base* (starting/leveled) value → only Resonance's % moves them;
  leveling (a base change) still does; secondary stats read current so Resonance
  weakens them.

## Bricks (dependency order; each landed green; STOP for validation between each)

### S1 — Multi-AttributeSets + derived pass + primary stats — DONE 2026-06-15
- `gameplay/stats/CoreAttributes.hpp` (the nine attributes), `gameplay/ability/
  DerivedStats.hpp` (generic: `StatView`, `DerivedStat`, `DerivedStatRegistry`,
  `AttrSetRef`), `gameplay/stats/CharacterStats` (`registerStatsComponents`,
  `registerCoreDerivedStats` — the ×5 maxima). `recomputeCurrent` generalized in
  `GameplayEffects` (multi-set + derived pass + backward-compatible single-set
  overload). Tests `tests/DerivedStatsTest.cpp`. Machinery in `ability/`, content
  in `stats/`.

### S2 — Resonance + Harmony — DONE 2026-06-15
- `gameplay/stats/Resonance` — reflected `Resonance { onyx, amber, garnet }`,
  `harmonyEffective` (cascade: half then quarter, truncated, one-shot from the
  most-displaced channel, cycle amber→garnet→onyx), `buildResonanceModifiers`
  (per channel: `×(1+r/100)` on the max, `trunc(r/15)` offset on the linked
  attributes). Fed into `recomputeCurrent` via a generic `StatModifiers` (add/mul
  by id) — the core stays Resonance-agnostic. Tests `tests/ResonanceTest.cpp`.
- The **base/current** decision (above) landed here to kill the double-hit.

### S3 — Game clock — DONE 2026-06-15
- `gameplay/stats/GameClock.hpp` (header-only): `gameSeconds` (f64) + `timescale`
  (×10), `advance(realDt) → f64` (the per-tick game-seconds, for survival),
  `gameHours`/`gameDays`. Tests `tests/GameClockTest.cpp`.
- **Reflection gained `f64`** (it was absent only because unneeded): `FieldKind::
  F64` + the variant alternative **appended last** (binary kind ordinals stay
  stable), `KindOf<f64>`, and the TOML / binary (`f64_` via `u64_`/`bit_cast`) /
  Lua paths extended. Covered by `ReflectTest` + the `CookerTest` "every kind"
  binary round-trip (a 1e300 value, beyond f32).

### S4 — Survival → Resonance — DONE 2026-06-15
- `gameplay/stats/Survival` — reflected `Survival { hunger, thirst, sleep }`,
  `tickSurvival` (hunger 1/3h, thirst & sleep 1/h, by game-time delta),
  `effectiveResonance(persistent, survival)` folding the **transient** survival
  contribution (a function of current value, restored by eating/drinking/sleeping
  — not accumulated): hunger + thirst → onyx, sleep → garnet. Tests
  `tests/SurvivalTest.cpp`. (Temperature → amber is Phase 7.)

### S5 — Typed damage + posture/stagger — DONE 2026-06-15
- `gameplay/stats/Damage` — `DamageType` (slash/pierce/blunt + fire/lightning),
  `DamageEvent`, `applyDamage` (the §6 execution calculation): per channel a flat
  reduction (defense/will, capped at 25+attr % of the hit) then a percentage
  reduction (armor/resistance), summed onto health; posture is a **runtime combat
  resource** (`CombatState`, not a GAS attribute), depleting it grants
  `State.Staggered` (timed, `updateStagger`). Defensive derived calculators added
  to `registerCoreDerivedStats` (defense, armorSlash/Blunt/Pierce, resistFire/
  Lightning, will, maxPosture [reads base], postureRegen, criticalSensitivity).
  `recomputeStats` orchestration helper. Tests `tests/TypedDamageTest.cpp`.

### S6 — StatsScene harness + wiring — DONE 2026-06-15
- `game/scenes/StatsScene` (in `DemoScenes`): a single actor with the full slice
  sheet, driven each frame (clock advance → survival tick → stagger tick →
  recompute with Resonance+survival modifiers → posture regen). An ImGui panel
  edits the nine attributes and the survival needs (sliders), shows the vitals +
  posture bars, the effective Resonance (after survival + harmony), the derived
  readouts, and buttons for typed damage / wound / heal / eat / advance time — so
  the cascade, shifting maxima and posture break are visible. Selector button in
  `main.cpp`. (Validated visually by the dev.) The actor is scene-owned; wiring
  stats into the ECS actor spawner is deferred until data-spawned NPCs need it.

## Mid-flight notes
- **Demo observability fix** (post-S6): survival decays slowly (×10 timescale →
  hours of real play to cross the threshold), so the survival→resonance link was
  hard to see. Made hunger/thirst/sleep **editable sliders** and wired thirst→onyx
  + sleep→garnet (not just hunger), so the loop is visible immediately.
- **Build hygiene**: changing a shared type's layout needs a clean rebuild (the
  Phase-5 stale-obj lesson); the `f64` variant change was clean-rebuilt.

---

**Phase 6 complete (2026-06-15).** 131 test cases / 871 assertions green; full
build clean; `true-adventurer.exe` runs the `StatsScene`. New machinery: the
derived-attribute layer (`gameplay/ability/DerivedStats`), `gameplay/stats/*`
(CoreAttributes, Resonance, Survival, GameClock, Damage, CharacterStats), and
`f64` in the reflection system.

## Out of scope (Phase 7 / deferred)
The full `docs/STATS.md` tail: body-part injuries, the rest of the secondary
list (endurances, all resistances, social, utility, encumbrance), reputation by
faction/location, drugs + harmony break, climate/exposure/clothing, permanent
statuses, full critical-weakness/shaken/dismemberment, the erudition curve. A
`StatsTuningForm` to move the slice's hard-coded constants into moddable data.
Wiring stats onto data-spawned ECS actors.
