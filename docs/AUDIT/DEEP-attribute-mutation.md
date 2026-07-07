# DEEP DIVE — Invariant §2.9 "attributes mutate ONLY through GameplayEffects"

**Scope:** rule on every direct attribute-write site flagged by U6-F1, U6-F8, U7,
and cross-checked against U9. Verdict only — no code changed.

**Date:** 2026-07-07 · **Subject:** `gameplay/ability`, `gameplay/stats`,
`gameplay/actors`, `gameplay/combat`, `world/scene`.

---

## 0. The key distinction the audits blurred

`setBaseValue()` is **not** a bypass — it is the effect pipeline's *own*
terminal write. `applyEffect` → `applyModifierToBase` → `setBaseValue`;
`routeDamageMeta` → `setBaseValue(health)`; `clampBasePair` → `setBaseValue`.
So "code calls `setBaseValue`" is necessary-but-not-sufficient evidence of a
§2.9 violation. The real question per site is:

> Is this write **(a)** the terminal primitive of an effect/execution
> calculation, **(b)** init/load-time seeding (explicitly sanctioned by the
> `initializeCurrent` contract and the §2.9 "derive CurrentValues on load"
> note), or **(c)** a runtime `Attribute += rate` that skips the pipeline
> entirely and *should* have been an effect (§6 lists regen/DoT/healing as
> effects)?

Only category (c) is a genuine §2.9 deviation.

There are two write shapes in play:
- `setBaseValue(set, attr("health"), …)` — reflected write, the pipeline primitive.
- `vitals.health = …` — a **direct poke of the same reflected field**
  (`AttributeSet::health` is `REFLECT_FIELD(health)`, i.e. the BaseValue).
  Same storage, but hand-written outside any effect. This is where the real
  deviations live.

---

## 1. Per-site verdict table

| # | Site (file:line) | What it writes | Verdict | Reasoning |
|---|---|---|---|---|
| 1 | `GameplayEffects.cpp:47` `applyModifierToBase` | BaseValue via `setBaseValue` | **LEGITIMATE (pipeline core)** | This *is* the instant/periodic effect primitive. §2.9 itself. |
| 2 | `GameplayEffects.cpp:52-60` `routeDamageMeta` | `health -= damage`, resets `damage` | **LEGITIMATE (execution-calc)** | The PHASE-3 damage meta-attribute → PostExecute → Health. Canonical, test-locked (GameplayEffectsTest case 1). |
| 3 | `GameplayEffects.cpp:63-74` `clampBasePair`/`clampBaseVitals` | clamps health/energy/essence base | **LEGITIMATE (pipeline post-step)** | Post-execute clamp, part of the effect pipeline. |
| 4 | `Damage.cpp:81` `setBaseValue(health, base-totalHealth)` | health BaseValue after mitigation | **LEGITIMATE (execution-calc)** | Typed-damage → flat → armor/resist → crit is a *custom execution calculation* — exactly what GAS ExecCalcs write attributes for; add/mul/override modifiers cannot express mitigation. Recomputes stats after. U6 miscounted this as a bypass; it is the combat form of the §6 "damage → health" pipeline. |
| 5 | `Spawner.cpp:103-106` `setBaseValue(maxHealth/health)` | seeds base from the form | **LEGITIMATE (init seeding)** | Construction-time seeding through reflection (§2.7), before the entity ticks. The `initializeCurrent` contract explicitly blesses "after spawn / on load (§2.9)". Not a runtime mutation. |
| 6 | `CharacterTick.cpp:165-169` `vitals.* = cur("max*")` | restore-to-full at init | **LEGITIMATE (init seeding)** | `initializeActorStats`, once, before combat. Same category as #5. |
| 7 | `CharacterTick.cpp:119` `vitals.energy += regen*dt` | energy BaseValue regen | **VIOLATION (soft, cat-c)** | §6 says regen "is an effect". Direct gated `+= rate` poke, no EffectForm. Driven by a dynamic `cur("energyRegen")` + `energyRegenDelay`/exhaustion gate. |
| 8 | `CharacterTick.cpp:109` `combat.posture += regen*dt` | posture regen | **N/A to §2.9** | `posture` lives on `CombatState`, **not** the AttributeSet — not a GAS attribute. Out of §2.9's scope (it is plain component state). Note for consistency only. |
| 9 | `CharacterTick.cpp:74-78` `vitals.health/essence -= br.*Damage` | poison/ignition/electrocution DoT | **VIOLATION (cat-c)** | Buildup DoT poked into base directly, while the sibling `bleedBurst` on line 81-83 correctly routes through `applyDamage`. Inconsistent; should route through the damage meta / `applyDamage`. |
| 10 | `GameTime.cpp:89-90` `vitals.health/essence += regen*gdt` | game-time health/essence regen | **VIOLATION (soft, cat-c)** | Same as #7, game-time path. §6 "regen is an effect". |
| 11 | `GameTime.cpp:22-27` `vitals.health/essence -= br.*Damage` | game-time buildup DoT | **VIOLATION (cat-c)** | Same as #9 (game-time path); `bleedBurst` here also correctly uses `applyDamage` (line 30-34). |
| 12 | `GameTime.cpp:49` `vitals.health = 0.0f` | death zeroing (buildup death) | **VIOLATION (soft, cat-c)** | A lethal `override→0` should be an instant override effect or a lethal `applyDamage`. Direct poke. (CharacterTick.cpp:99-101 does death *correctly* — it only adds `State.Dead`, no base write.) |
| 13 | `AbilitySystem.cpp:49` `setCurrentValue` (unused) | CurrentValue overlay directly | **LEGITIMATE-but-footgun (U6-F8)** | Dead code. If ever called it would let a caller set CurrentValue outside `recomputeCurrent`, breaking the "Current = base + Σmods" identity. Not an active violation; remove or comment. |

**Net genuine §2.9 deviations:** #7, #9, #10, #11, #12 — all in the periodic
character-tick sites (regen, buildup DoT, buildup death). Everything the audits
also flagged (#4 Damage.cpp:81, #5 Spawner.cpp:103, #13 setCurrentValue) is
**not** an active bypass.

---

## 2. Reconciling U6 (FAIL) vs U9 (pass)

Both are correct because they describe **different code regions**:

- **U9 is right about the pipeline.** `GameplayEffectsTest.cpp` locks
  `applyEffect`: instant damage → meta → health (case 1), `[0,maxHealth]` clamp
  (case 2), infinite raises Current not Base (case 3), duration reverts + drops
  tag (case 4), tag gating (case 5), periodic ticks base then expires (case 6).
  The effect *pipeline itself* is test-locked and faithful to §2.9. `CombatTest`
  further locks the Damage execution-calc.
- **U6-F1 is right about the tick sites.** `CharacterTick`/`GameTime` apply
  regen, buildup DoT, and buildup-death by writing `vitals.*` directly, outside
  `applyEffect`. No test asserts those go through the pipeline — *because they
  do not.* So there is no contradiction: the pipeline is locked; the periodic
  stat-tick sites live beside it.
- **Where U6 over-reached:** it lists `Damage.cpp:81` `setBaseValue` as a
  bypass. That write is the terminal primitive of the combat *execution
  calculation* (mitigation math that modifiers cannot express) and is exactly
  the sanctioned §6 damage→health path — reclassified LEGITIMATE here.

One-line reconciliation: **the effect pipeline is correct and tested (U9); the
periodic regen/DoT/death writes never enter it (U6). Both true.**

---

## 3. Remediation per real violation

Two viable directions — pick per taste; they are not exclusive.

**Option A (cheapest, honest): amend the invariant.** GAS itself sanctions
*Execution Calculations* that write attributes directly. Add one sentence to
CLAUDE.md §2.9 naming (i) combat/DoT damage and (ii) rate-driven regen as
"execution calculations — the effect pipeline's own terminal writes, not
bypasses," and require them to call `recompute*` after. This converts #7-#12
from "violation" to "sanctioned seam" with a documented contract. **Effort S.**

**Option B (purist): route them through effects.**

- **#9 / #11 — buildup DoT (poison/ignition/electrocution).** Route through
  `applyDamage` (as `bleedBurst` already does) or an instant `damage`
  meta-effect. This also unifies mitigation/clamp behaviour. Removes the
  inconsistency where bleed goes through the pipeline and poison does not.
  **Effort S** — the plumbing (`applyDamage`, damage meta) already exists.
- **#12 — buildup death.** Replace `vitals.health = 0` with a lethal
  `applyDamage` (huge amount) or an instant `override→0` effect on `health`,
  then `updateLifeState`. **Effort S.**
- **#7 / #10 — regen.** Hardest, because the magnitude is a *dynamic captured
  attribute* (`cur("healthRegen")` etc.) and is **gated** (energy pauses via
  `energyRegenDelay`/`State.Exhausted`; posture pauses during stagger/crit
  window). A static periodic `EffectForm` cannot express a captured, gated
  magnitude today. Proper fix needs periodic effects with **attribute-capture
  magnitude** + a **blocked-tag gate** (`blockedTag=State.Exhausted` for energy).
  That is a real pipeline feature, not a rewrite of these call sites. **Effort M-L.**
  Until that feature exists, Option A is the pragmatic call for regen.

Recommended sequence: **B for #9/#11/#12 (S, quick consistency win) + A for
regen #7/#10 (document as execution calc until capture-magnitude effects land)**;
delete or comment `setCurrentValue` (#13, S).

---

## 4. Final severity call for the §2.9 item

**MEDIUM — not HIGH — once execution-calc and init sites are excluded.**

Rationale:
- The three "high-scary" sites the audits cite (Damage.cpp:81, Spawner.cpp:103,
  setCurrentValue) are **not** active bypasses (execution-calc, init seeding,
  dead code). Removing them from the count deflates the finding.
- The genuine deviations (regen, buildup DoT, buildup death) are all **base**
  writes, so the §2.9 save contract is intact — saves still persist BaseValues +
  active durational effects and recompute Current on load; nothing breaks
  persistence, determinism, or the layering invariant (§2.4).
- The practical harm is **internal inconsistency** (bleed routes through
  `applyDamage`; its DoT siblings do not) and **invariant purity** (§6 says
  regen is an effect; it is coded imperatively for a real reason — gated dynamic
  rates). No correctness, save, or moddability regression.
- It is not trivial ("low") either: DoT/death *should* unify through the damage
  meta with an S-effort change, and the regen story exposes a genuine missing
  pipeline feature (attribute-capture periodic effects).

**Verdict: MEDIUM.** Downgrade U6-F1 from FAIL/high to a MEDIUM consistency +
documentation finding; fix DoT/death routing (S), and either build
capture-magnitude periodic effects (M-L) or formally sanction rate regen as an
execution calculation in §2.9 (S).
