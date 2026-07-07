# U6 — GAS + stats (`gameplay/`) — audit report

Scope: `gameplay/{ability,stats,combat,inventory,faction,condition,event,cue,interaction,actors,ai,save}`.
Reference design: CLAUDE.md §2.9/§6, docs/STATS.md, docs/MODDING-EFFECTS.md.

## Hard-check results (explicit pass/fail)

- **§2.10 headless purity — PASS.** Grep for `rhi|render|backends|glad|GL/|SDL|imgui`
  includes across `gameplay/` returns zero matches. No renderer/backend coupling.
- **§8 RNG routing — PASS.** Every stochastic status system routes through
  `core::Rng` (`Afflictions.cpp:7,13`, `Injuries.cpp:176,181`, `CharacterForms.cpp:17,23`).
  No `std::rand`/`mt19937`/ad-hoc RNG. Drugs/StatusBuildup are deterministic (no roll) — fine.
- **§8 alias discipline — PASS.** No raw `std::uint32_t`/`glm::vec3`; uses `u32/f32/Vec3`.
- **§2.3 reflection — PASS.** 28 reflected types via `REFLECT_BEGIN/END`; no ad-hoc per-type serialization.
- **Attribute overlay — PASS (no inline re-derivation).** Systems read current values through
  `currentValueOf()` (Damage.cpp, GameTime.cpp `cur()` lambda, Combat.cpp); none re-derive the
  overlay inline. The `AbilitySystem.current` map is a deliberate, documented current-value cache.
- **§2.9 attribute mutation — FAIL (see F1).** Multiple direct writes to attribute state bypass
  the GameplayEffect pipeline.

## Findings

| id | sev | axis | file:line | description | action | effort | inter-unit |
|----|-----|------|-----------|-------------|--------|--------|-----------|
| F1 | high | archi | CharacterTick.cpp:74-78,119,165-167; GameTime.cpp:22-26,48-49,89-90; Damage.cpp:81 | §2.9 says nothing sets an attribute directly — all changes flow through the effect pipeline; §6 explicitly lists regen/healing/DoT as effects. Yet health/energy/essence regen, buildup DoT, and death are raw writes to `vitals.health/energy/essence` (and `setBaseValue` in the damage execution calc). | Route regen/DoT through periodic/instant GameplayEffects, or formally carve out a documented "execution-calculation" seam in §6 so these are sanctioned rather than silent drift. At minimum funnel all base writes through one `depleteVital()` helper. | L | true |
| F2 | med | factor | CharacterTick.cpp:73-102 vs GameTime.cpp:19-55 (`applyBuildupResult`) | The BuildupTickResult→state application is duplicated across the real-time and game-time paths, and **diverges**: electrocution adds `staggerSeconds`+`State.Staggered` in CharacterTick but not in GameTime; death sets `State.Dead` tag in one, `health=0` in the other. Latent bug. | Extract one `applyBuildupResult()` used by both tick paths; make the divergence intentional or delete it. | M | true (H-e) |
| F3 | med | factor | GameplayEffects.cpp:302-335 (`tickEffects`) vs 352-376 (`tickGameTimeEffects`) | Two near-identical loops over the same `activeEffects` vector, each `continue`-ing on the other's `gameTime` bool. The "dual clock GameClock vs GameTime" framing is a misnomer — `GameClock` is a clock struct, `GameTime.cpp` is the tick module; the real duplication is these two tick paths + F2. | Parameterize one tick over a dt-source predicate; single decrement/expire/erase pass. | M | true (H-e) |
| F4 | med | reutil | event/EventBus.hpp:36-53; cue/GameplayCues.hpp:41-62 | EventBus and CueRegistry are two parallel dispatch systems with duplicated subscribe/unsubscribe/id-counter/emit shape (UiSystem is the 3rd, outside this unit). | Promote a shared `core::Signal<Payload>` (id alloc + ordered dispatch + re-entrancy) and build both on it. | M | true (H-c) |
| F5 | med | factor | save/SaveState.hpp:33-68 (`createRecord`/`formFromRecord`/`copyMatchingFields`) | Reflection copy/diff-vs-defaults logic duplicates the resolver/EditSession/Synthesis machinery in `data/plugins/` (the comment even names EditSession export semantics). | Share one reflect-based diff/copy helper between `data/plugins` and `gameplay/save`. | M | true (H-b) |
| F6 | low | reutil | event/EventBus.hpp:34,52; cue/GameplayCues.hpp:46,61; ability/AbilitySystem.hpp:37,51 | Three ad-hoc `u32 nextId/nextEffectId {1}` handle-allocation schemes (SubscriptionId, cue handle, effectId) with no shared `Handle<Tag>`/id-allocator. | Fold into a shared typed-id/allocator primitive (Tier-3 H-d). | S | true (H-d) |
| F7 | med | qualité | Combat.cpp:5-17,26; CharacterTick.cpp:99-102; GameTime.cpp:47-53 | Death detection is inconsistent: `updateLifeState` keys off `currentValueOf("health")`, but the tick paths set death by writing `vitals.health` (base) and CharacterTick adds `State.Dead` directly instead of calling `updateLifeState`. Base vs current can diverge if a health modifier is ever active. | Make `updateLifeState` the single death-transition authority; have all paths write base then call it. | M | false |
| F8 | low | qualité | ability/AbilitySystem.hpp:74; .cpp:49-51 | `setCurrentValue` is public API but only referenced by tests; it writes the overlay directly, bypassing `recomputeCurrent` — a footgun that can desync current from base+effects. | Remove, or make it internal/test-only. | S | false |
| F9 | low | propreté | AbilitySystem.hpp:43-44; GameClock.hpp:8; Attributes.hpp:11; Inventory.hpp:21; GameplayEffects.hpp:15 | Comments say serialization is "deferred to Phase 8", but save/capture now exists (chantier 5/8, `save/SaveState`). Stale roadmap references mislead readers about what is implemented. | Refresh comments to point at the shipped save seam. | S | false |
| F10 | low | archi | GameTime.cpp:131-135 (comment) + recompute call sites | `tickGameTimeEffects`/`syncInjuryEffects` call the 2-arg `recomputeCurrent` which overwrites derived targets with raw AttributeSet defaults, forcing a corrective `recomputeStats` afterward. Fragile ordering documented only in a comment. | Fold the derived-aware recompute into the game-time tick so no path leaves derived stats transiently wrong. | M | false |

## Notes for Tier-3

- F1 is the load-bearing §2.9 question and needs a design ruling (sanction the execution-calc
  seam vs. force everything through effects). It is the single highest-value item in this unit.
- F2/F3 (H-e), F4 (H-c), F5 (H-b), F6 (H-d) all converge on shared primitives that belong in
  `engine/core`/`engine/reflect`: a `Signal<Payload>`, a typed id/handle allocator, and a
  reflect diff/copy helper. These recur across units — good Tier-3 mutualization candidates.
