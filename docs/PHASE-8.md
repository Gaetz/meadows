# Phase 8 — Combat 2D dynamique (journal)

> Brick journal for Phase 8. Design references: `docs/STATS.md`,
> `docs/SIMULATION-AND-PRESENTATION.md`. Modding: `docs/MODDING-EFFECTS.md`.
> **Status: IN PROGRESS.** Update the per-step boxes as work lands.

---

## Goal

A single playable **"village + arena"** 2D scene (Zelda-like top-down) that
exercises the whole stats stack (Phases 6-7) in a **real-time combat loop**,
not through ImGui buttons. The player fights active enemies, loots gold, visits
an innkeeper to recover and a merchant to trade, then returns to the fight.

This is the first scene where the full `tickCharacter` pipeline runs **every
frame on every combatant** — the move from "ImGui-driven single actor" (StatsScene)
to a live multi-combatant simulation.

Still 2D. Still the OpenGL prototype renderer (the Godot port is Phase 8.5).

---

## Locked design decisions

- **One combined scene** (`CombatArenaScene`), not separate demos. Best matches
  "exercise all systems in a real dynamic situation."
- **FPS-like control scheme** (decided with the dev):
  - **Movement** = WASD (independent of facing).
  - **Aim / facing** = toward the **mouse cursor** in world space (decoupled from
    movement, like an FPS). The player's attack direction and sprite facing follow
    the cursor, not the walk direction.
  - **Attack** = **left mouse button**.
  - **Dodge** = **short press on Shift** (roll/dash with brief i-frames).
  - **Interact** (innkeeper / merchant / pickups) = **E**.
- **Consequence:** the platform input layer must gain **mouse support** (button
  state + cursor position projected to world space). It is keyboard-only today
  (`engine/platform/Input.hpp`). Keep it platform-clean (§3.1): logical mouse
  state in the header, SDL mapping in the `.cpp`; the world-space projection uses
  the existing `render::Camera2D`.

---

## Existing bricks to reuse (do NOT rebuild)

| Brick | Location | Use in Phase 8 |
|---|---|---|
| `WorldDemoScene` | `game/scenes/WorldDemoScene.*` | Scene base: plugins, cells, texture cache, camera, render seam |
| `platform::Input` (`isDown`/`wasPressed`) | `engine/platform/Input.hpp` | Keyboard; **extend with mouse** |
| `applyMovement` / `resolveCollisions` / `forEachTriggerOverlap` | `world/scene/` | Movement, AABB push-out, triggers/pickups |
| `ai::AiAgent` + `updateChaseAi` + `seek`/`withinRange` | `world/ai/` | Chase package (no attack yet — extend) |
| Grid A* | `world/ai/Pathfinding.*` | Navigation if needed |
| `performAttack` / `updateLifeState` | `gameplay/combat/Combat.*` | Ability→effect attack, `State.Dead` |
| `tickCharacter` | `gameplay/actors/CharacterTick.*` | The full 3-phase stats tick, per combatant per frame |
| `applyDamage` + `CombatState` (posture/stagger/paralysis) | `gameplay/stats/Damage.*` | Typed-damage pipeline, posture break |
| `Inventory` / `Equipment` / `EquipmentStats` | `gameplay/inventory/`, `gameplay/stats/` | Items, live equipment mods |
| `CharacterStatsPanel` | `game/ui/` | Debug inspection during bring-up |
| `Spawner` | `world/scene/Spawner.*` | Wires the 7 stat components onto actors |

---

## Step plan (bottom-up, each step independently demonstrable)

### Step 1 — Scene + live combat loop (foundation)  ✅ DONE
`CombatArenaScene : WorldDemoScene` in its own files
(`game/scenes/CombatArenaScene.{hpp,cpp}` — Phase 8 will grow it a lot). Runs the
full `tickCharacter` every frame on every spawned combatant. Spawns a player + two
training dummies with the full stat sheet at full vitals. A compact ImGui table
shows each combatant's live health / energy / essence / posture / state.
**Delivered:** actors tick & regen live; the readout proves it. Wired into the
`main.cpp` demo selector ("Combat arena"). Build green, 181 tests unchanged.

Implementation notes:
- **Own file, not DemoScenes.cpp** — Phase 8's scene grows across 8 steps.
- **`spawnCombatant(name, pos, tint)`** mirrors StatsScene's player setup: sets the
  full component set (`CoreAttributes`, `AttributeSet`, `AbilitySystem`, `Resonance`,
  `Survival`, `StatusBuildup`, `CombatState`, `Injuries`, `ResonanceDecays`) + a
  `Velocity`/`Collider` (ready for Step 2) + `world::ActorMarker`, then
  `initializeCurrent` + `initializeActorStats` (full vitals).
- **`update` does NOT call `WorldDemoScene::update`** — the base loop calls
  `tickEffects` on all actors, which `tickCharacter` already does; calling both
  would double-tick durations/cooldowns. (Note: `StatsScene` has this latent
  double-tick; harmless there with one actor, avoided here.)
- **Clock advanced once per frame**, same `gameDt` passed to every combatant.
- **Sprite**: reuses the iron-sword texture (already resident from base.toml), so
  no extra loading / no placeholder pop-in for combatants spawned after the gate.
- **Compact multi-combatant table** instead of reusing the full `CharacterStatsPanel`
  (whose demo-gear scaffolding — sample weapon/armor/drug/disease — belongs to the
  teaching StatsScene, not the arena). The full panel can be wired back as a
  per-combatant deep-inspect toggle later if needed.
- Equipment mods = `{}` for now (no equipment until Step 6).

#### Placeholder art — 8-direction character sheets  ✅ DONE (Step 2 prep)
Two debug sprite-sheets stand in for real (and, later, Godot) character sprites:
`placeholder_player_8dir.png` (blue) and `placeholder_enemy_8dir.png` (red).
Each is a **512×64 horizontal strip = 8 frames of 64×64**, one per facing, with a
bright "beak" showing orientation. Generated (no deps) by
`tools/gen_placeholder_sprites.py`; declared as assets in `base.toml`
(GUIDs `b1a7c0de-…-0001` / `…-0002`).

Plumbing added to make them usable:
- **`SpriteRender.uvRect`** (Vec4, default full texture) — a texture sub-region,
  reflected + appended last (binary ordinals stable). Passed through `spriteFor`
  into `render::Sprite.uvRect` (the renderer already supported it).
- **`uvRectForFacing8(facing)`** helper selects the frame:
  `frame = round(atan2(dir.y, dir.x) / 45°) mod 8`, i.e. frame 0 = East, CCW,
  world +Y up. **The generator and this helper share the exact same convention** —
  keep them in sync if either changes.

The arena has the player (facing south) + two dummies facing the player. Dynamic
facing (the player frame follows mouse aim) is wired in Step 2; dummies face their
target once enemy AI lands in Step 4. (All eight frames were visually validated
against the generated PNGs; no need for a permanent 8-dummy showcase.)

### UI approach for Phase 8 (decided)  ✅
**No UI system / framework is built for Phase 8.** UI is disposable presentation
that Godot replaces (Phase 8.5/11); building RmlUi or a custom widget layer now
would be premature gold-plating (CLAUDE.md §3 "defer game UI", §10; SIMULATION-AND-
PRESENTATION.md "UI = disposable shell"). Reuse what is already integrated:
- **Player HUD** (health / energy / posture) = **in-world sprite bars** via the
  existing `SpriteRenderer` (`renderer.draw({.position,.size,.tint})`, exactly like
  `WorldDemoScene::drawLoadingScreen`). Chosen for the "real situation" feel and to
  exercise the render snapshot.
- **All menus / interactions** (innkeeper, merchant, debug inspector) = **Dear
  ImGui** (already wired at engine level; immediate-mode, throwaway).
- Hard rule: the UI is a **reader of sim state + emitter of intentions**, never
  game logic (`Game.hpp`: draw must not mutate game state). This is what makes the
  ImGui→Godot swap mechanical in Phase 8.5.

### Step 2 — Real-time player: move + aim + dodge  ✅ DONE
- WASD → `Velocity` (reuse `applyMovement` + `resolveCollisions`).
- **Mouse support** added to `platform::Input`; facing = normalized vector from
  player to cursor world position.
- **Dodge** on short Shift press: a brief velocity burst (in movement direction,
  or backward from aim if stationary) + a transient `State.Dodging` i-frame tag +
  an energy cost (GameplayEffect) + a cooldown.
**Delivered:** player strafes, aims at the cursor (8-dir frame follows the mouse),
rolls with i-frames. Build green, tests 181 → 183.

Implementation notes:
- **Mouse in `platform::Input`** (platform-clean, §3.1): logical `MouseButton`
  enum + `mousePosition()`/`mouseDown`/`mousePressed` in the header; SDL3 mapping
  (`SDL_GetMouseState` → float pos + button bitmask via `SDL_BUTTON_MASK`) in the
  `.cpp`. Added `Key::Shift` (→ `SDL_SCANCODE_LSHIFT`). `Input.hpp` now pulls
  `<glm/vec2.hpp>` because `Defines.hpp` only forward-declares glm.
- **`render::screenToWorld`** (free inline in `Camera2D.hpp`): inverse of
  `viewProj` for a point, flips screen-Y → world +Y. Reusable by later steps.
- **Dodge = a real `AbilityForm`** (`base.toml`: `Dodge` + `DodgeCost`/`DodgeIFrames`/
  `DodgeCooldown` effects), activated by `tryActivate`. The **AbilitySystem** owns
  the gating — cooldown (`Cooldown.Dodge`), cost affordability, **and**
  `blockedTag=State.Exhausted`. The scene does **zero** manual cooldown/cost
  checks; on success it only drives the movement burst + `dodgeTimer`. Facing
  updated each frame via the existing `uvRectForFacing8`.
- **Energy-exhaustion gate** (decided with the dev): energy-costed actions
  (dodge now; attack Step 3; sprint later) are blocked when out of energy via a
  shared `State.Exhausted` tag with **hysteresis** (set at energy 0, cleared above
  20% max). Lives in the shared tick (`updateExhaustion`, called from
  `tickCharacter`) so **enemies gate too** (Step 4). Extracted as a free helper in
  `GameplayEffects` → unit-tested headless (`tests/EnergyExhaustionTest.cpp`).
  Threshold is a constant for now → `StatsTuningForm` later.
- **Sprint (course) deliberately out of Step 2 scope** (per this doc); the gate is
  designed to cover it when it lands.

Post-review fixes (dev feedback):
- **Facing was vertically inverted.** Textures upload un-flipped (row 0 = top) but
  the sprite quad maps world +Y → v=1, so the renderer samples V downward — a
  vertical mirror. `uvRectForFacing8` now negates `facing.y` to cancel it, so the
  on-screen facing matches the world-space aim. (Horizontal was already correct;
  the static Step-1 spawns render identically since they were authored in world
  facing.)
- **Movement now accelerates** (was instant velocity). Target speed = the
  `movementSpeed` derived stat × world scale; velocity ramps toward it at a new
  **`acceleration`** derived stat (`90 + alacrity·2`, provisional → Phase 9
  utility pass) × scale, with sharper braking inertia when idle. So stats/injuries
  that touch movementSpeed/acceleration change the feel live.
- **Dodge burst eases out** (ease-out quad on the remaining fraction) instead of a
  constant speed + hard stop, so it decelerates into the roll's end.
- **Dodge retuned**: cooldown 1.2s → **0.5s**, energy cost −15 → **−25** (base.toml).
- **Energy regen delay** (new): spending energy pauses regen for a beat (constant
  1.0s → StatsTuning later), so rapid use has a recharge cost. Timer lives in
  `AbilitySystem.energyRegenDelay` (runtime), armed in `applyEffect` on any negative
  energy `add`, counted down in `CharacterTick` (gates the regen line). A new
  `EffectForm.bypassEnergyRegenDelay` TOML flag lets specific actions spend energy
  without arming the pause. Modder ref updated in `docs/MODDING-EFFECTS.md`; tested
  in `tests/EnergyExhaustionTest.cpp`.

### Step 3 — Player melee attack  ✅ DONE
Attack state machine windup→active→recovery on left-click. During *active*, a
transient melee hitbox in front of the aim direction; overlap query → `applyDamage`
(typed) + posture damage on enemies hit. Energy cost + cooldown (GameplayEffects).
**Delivered:** left-click swings the iron sword; dummies take typed slash + posture
damage, Staggered fires on posture break, and they die (State.Dead). Build green,
187 tests.

Implementation notes:
- **Attack = a GAS `AbilityForm`** (`PlayerAttack` in base.toml) for gating only —
  energy cost (`AttackCost`, permissive like all energy costs), cooldown
  (`AttackCooldown`, `grantedTag=Cooldown.Attack`, 0.5s), `blockedTag=State.Exhausted`.
  It has **no `effect`**: the damage is NOT a generic effect. Left-click →
  `tryActivate`; on success the scene starts the swing.
- **Damage is weapon-driven + typed**, per the doc default: the scene builds
  `weaponDamageEvent(playerWeapon, playerSystem)` and feeds `applyDamage` (the
  typed mitigation + posture/stagger pipeline). `playerWeapon` = the `IronSword`
  WeaponForm resolved from base.toml (given typed `slashAttack`/`postureDamage`
  fields, which it lacked). Live equip/unequip is Step 6; here the weapon is a
  resolved scene pointer (equipment "matters from Step 3" without the inventory UI).
- **Swing state machine is scene-side** (`updatePlayerAttack`): `None → Windup
  (0.12s) → Active (0.10s) → Recovery (0.18s)`. Only the **Active** window deals
  damage. Aim is **locked** at swing start (`attackDir`), so the hit lands where you
  committed, not where the cursor drifted.
- **Hitbox = a front arc**: within `kAttackRange` (1.4u) and `dot(dirToTarget,
  attackDir) ≥ cos60°`. Each enemy is hit **at most once per swing**
  (`hitThisSwing` set). Can't start a swing mid-dodge.
- **No new headless test for the swing**: the timing/arc is presentation; the
  damage path (`weaponDamageEvent` + `applyDamage`) is already covered by the
  typed-damage / equipment tests. Enemies still just tick (real AI is Step 4).

Post-step (dev request) — **full damage system on the dummies**:
- **All 9 elemental resistances implemented** (was fire/lightning + cold only):
  `DamageType` gained Sonic/Chemical/Psychic/Holy/Dark/Ether; `CharacterStats` got
  the six matching `resist*` derives (charisma → fire/sonic/holy, ego →
  cold/chemical/dark, insight → lightning/psychic/ether, 0.5·attr); `ArmorForm`
  gained `resistCold` + the six (appended last, ordinals stable); `applyDamage`
  maps every channel; `armorModifiers` folds them all.
- **Negative resistance now amplifies** (vulnerability): the mitigation clamp is
  `[-100%, 100%]`, so a negative resist takes up to ×2. Makes iron's conductive
  lightning weakness real. Tested (`TypedDamageTest`).
- **Two armors in base.toml**: `LeatherArmor` (elemental + blunt focus) and
  `IronArmor` (slash/pierce focus, −20 lightning). Three arena dummies now spawn
  unarmored / leather / iron (equipped via a torso `ArmorForm`; each combatant's
  armor is folded into **its** `tickCharacter` via `equipmentModsFor`, so mitigation
  is live). Static equip only — live equip/unequip UI is still Step 6.
- **Debug typed-hit buttons** in the arena panel (Slash/Fire/Lightning/Holy 40 +
  reset) apply a channel to every dummy, so the mitigation differences are
  directly comparable (the melee sword only exercises slash).

Second armor pass (dev request) — **buildup endurances + Elden Ring-flavoured values**:
- **Armor now raises status-buildup endurances.** `ArmorForm` gained six fields
  (`endurancePoison/Bleed/Mental/Disease/Curse/Death`), folded by `armorModifiers`.
  The three elemental endurances follow the armor's `resist*` instead: the
  `enduranceIgnition/Glaciation/Electrocution` derives now read `resistFire/Cold/
  Lightning` (was the raw attribute) — a no-op bare, but armor propagates, and
  **iron's −22 lightning makes electrocution easier** (threshold drops).
- **Leather / iron reworked from the ER Leather/Iron sets**: leather = modest
  physical (best vs blunt), good elemental insulation, very high poison/disease/
  death/mental endurance; iron = strong slash/pierce, poor elemental (−22
  lightning), high bleed/frost robustness, low poison/focus/vitality. Values in
  base.toml.
- **Arena**: a Poison/Bleed/Ignition buildup readout makes the endurance
  difference visible — leather soaks far more poison than iron before
  `Status.Poisoned`. Tests in `EquipmentStatsTest` (endurance fold + negative-resist
  electrocution drop). 191 tests green.

Swappable test weapons (dev request) — **one full profile per weapon, keys 1-5**:
- `WeaponForm` gained native `buildupType` + `buildupAmount` (appended last, ordinals
  stable): a weapon applies status buildup on hit, gated by the target's endurance
  (so armor resists it). `docs/MODDING-EFFECTS.md` updated.
- Five weapons in base.toml, each a complete profile: **1 Iron Sword** (slash),
  **2 Mace** (blunt, high posture → stagger), **3 Poison Dagger** (pierce + poison
  buildup), **4 Spiked Mace** (blunt + bleed buildup), **5 Flame Scimitar**
  (slash + fire + ignition buildup). Keys `1-5` swap the equipped weapon
  (`platform::Key::Num1..5` added to the input layer).
- The arena panel shows the current weapon's full stat line; the combatant table
  gained Poison/Bleed/Ignition columns (value / endurance) so each weapon's effect
  is observable and its armor resistance comparable across the three dummies. The
  old per-channel debug buttons were pruned to just Lightning (no weapon covers it)
  + Reset.

### Step 4 — Enemy combat AI  ▢
Extend `AiAgent` into a small FSM: idle → chase (perceive player) → attack (in
range: telegraph + strike applying damage to the player) → recover; stagger on
posture break; death on `State.Dead`. Enemies run the **same** `tickCharacter` +
attack path as the player.
**Deliverable:** enemies chase, telegraph, hit the player, can be staggered/killed.

### Step 5 — Combat feedback + death  ▢
Hit flash, knockback, posture/stagger cues, enemy death removal, player death →
game-over / respawn. In-world HUD: player health / energy / posture bars (drawn as
sprites, not just ImGui).
**Deliverable:** readable combat; losing/winning is clear without the debug panel.

### Step 6 — Currency + live equipment  ▢
Add a gold resource; loot gold (and items) from dead enemies. Verify equip/unequip
changes combat stats **live** (`EquipmentStats` already feeds `buildCharacterMods`).
**Deliverable:** picking up / equipping a better weapon changes damage mid-fight.

### Step 7 — Innkeeper NPC (rest)  ▢
A static NPC; approach + **E** → restore health / energy / essence via
GameplayEffects, plus rest recovery (heals injuries / afflictions over advanced
game time). Reuse `Rest::sleep` / the game clock.
**Deliverable:** a wounded player heals up at the inn and the injury/affliction
timers advance.

### Step 8 — Merchant NPC + integration  ▢
Merchant NPC with an inventory + prices; **E** → buy/sell ImGui panel moving items
between merchant and player inventories against gold, with equip. Final integration
pass: one scene exercising **all** stat systems — poisoned enemy weapons (status
buildup), afflictions/injuries from combat (resonance-gated), survival over playtime.
Balance pass. Finalize this journal.
**Deliverable:** full playable loop: fight → loot → heal/trade → fight.

---

## Open design points (decide as steps land)

- ~~**Attack model**~~ — DECIDED (Step 3): one melee driven by the equipped
  `WeaponForm`'s typed attack + scaling (`weaponDamageEvent` → `applyDamage`). Light/
  heavy variants deferred; the weapon is a resolved scene pointer until Step 6 wires
  live equipment.
- **Dodge i-frames:** exact window + whether it also cancels attack recovery.
- **Enemy variety:** start with one melee archetype; add a ranged/caster only if
  time allows.
- **Respawn vs permadeath** for the slice: default respawn at the inn (a prototype
  convenience, not a design statement).
- ~~**HUD tech**~~ — DECIDED: in-world sprite bars for the player HUD; ImGui for
  menus/debug (see "UI approach for Phase 8" above).

---

## Decisions to remember (fill in as we go)

- **No UI framework in Phase 8.** ImGui (already integrated) for menus/interactions;
  in-world sprite bars for the player HUD. UI stays a pure reader of sim state +
  emitter of intentions (never logic) so the Godot swap in Phase 8.5 is mechanical.
  Do NOT add RmlUi or a custom widget system — that's disposable presentation Godot
  will own.

- **A scene that runs `tickCharacter` must not also run the `WorldDemoScene`
  effect loop.** `tickCharacter` already ticks effects; layering the base loop on
  top double-ticks durations/cooldowns. Combat-arena-style scenes own their tick
  loop entirely.
- **All combatants share one game clock, advanced once per frame.** The same
  `gameDt` goes to every `tickCharacter` call — never advance the clock per actor.
- **Player/enemy actions with a cost or cooldown are GameplayAbilities**
  (`AbilityForm` + `tryActivate`), never hand-rolled `applyEffect` in the scene.
  The AbilitySystem owns the gating (cooldown tag, cost affordability, blocked
  tags); the scene only reacts to a `true` return (e.g. the dodge movement burst).
  This is the pattern to reuse for the Step 3 attack and any enemy ability.

- **Cost gating has two policies, set per-ability in TOML (`AbilityForm.costPolicy`).**
  `"permissive"` (stamina): activate with any reserve > 0, overdraw clamps to 0 (roll
  at 12 energy for a 25 cost; the empty-reserve block is `State.Exhausted`).
  `"strict"` (magic): require the full cost. Default (`""`) resolves by resource —
  **energy → permissive, essence/other → strict**. `canAfford` in
  `GameplayAbility.cpp` implements it; reuse for the Step 3 attack and any spell.

- **Energy-costed actions are gated by a shared `State.Exhausted` tag**, not
  per-action checks. `updateExhaustion` (in `GameplayEffects`, called from
  `tickCharacter`) sets it at energy 0 and clears it above a fraction of max
  (hysteresis, no flicker); every energy-costed `AbilityForm` carries
  `blockedTag=State.Exhausted`. Living in the shared tick means enemies are gated
  by the same rule (Step 4). Threshold is a constant → migrate to `StatsTuningForm`.

- **Combatant spawning is code-side for now** (`spawnCombatant`), not data-driven.
  The full stat sheet matches the Spawner's `spawnActor` wiring, so moving to
  data-driven arena population later is a mechanical change. Per-actor stat variety
  (a tougher enemy) will need `CoreAttributes` seeded from the actor's Form.
