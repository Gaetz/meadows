# Phase 4 — Scripting, abilities, conditions, quests, dialogue (DONE 2026-06-14)

> Journal of the Phase 4 implementation, same role as `docs/PHASE-1/2/3.md`.
> Read before touching `script/`, the condition evaluator, or `meadows-narrative`.

Phase 4 adds the **dynamic/authoring layer** on the systems built so far: Lua
scripting, event dispatch, the full ability framework (Lua + tasks/`wait`), a
shared **condition evaluator**, **quests**, and **dialogue**.

## Architecture decided with the dev (incl. NarrativePro study)

We studied Unreal's **NarrativePro** plugin. Its key reusable idea: a **unified
node model** — every quest AND dialogue node derives from a base carrying
`Conditions[]` (gate) + `Events[]` (side effects on enter/exit). We adopt that
pattern, but **decomposed into individually-patchable Form records** (not one
monolithic UObject asset), to fit §5.

- **Lua = sol2, ONE shared sandboxed VM** (§2.8). Scripts = **stateless shared
  modules**; `self` = entity handle (a proxy over the entity's components).
  Persistent per-entity state → reflected **`ScriptVars`** (the §5 serialization
  currency: reflect::Values, not loose Lua tables). Transient → a Lua scratch
  table. **Attributes are read-only from Lua** (§2.9 — mutate only via effects);
  gameplay mutations are explicit (`self:addTag`, later `self:applyEffect`).
  **Determinism (§8):** no `os`/`io`/wall-clock/`math.random`; randomness via the
  engine-seeded `rng()`.
- **Latent execution = Lua coroutines + a central scheduler keyed by entity**
  (§2.8): `wait(t)` yields; the scheduler resumes after `t` game-seconds.
- **Conditions = structured data clauses + a Lua escape hatch** (clauses:
  HasTag, AttributeAtLeast, QuestStage, HasItem, FactionStanding…; patchable §5,
  deterministic, tag-centric §6). **The condition evaluator is THE shared engine**
  (abilities, quests, dialogue, AI). **Events/actions = a structured action list**
  (addTag, applyEffect, advanceQuest, giveItem, startDialogue, completeTask,
  runLua) — the GAS↔narrative bridge.
- **Quests/dialogue = decomposed records linked by id.** Each quest
  state/branch/task and each dialogue node/response is a Form with an id +
  parent/next/target id-fields + its conditions/events. **Many records per file**
  (a quest = one `.toml`, N records); enriching = a mod adds records + patches
  links (no list-merge conflict). The Phase-2 child→parent pattern (parent +
  order) assembles a node's children. The visual graph editor (Phase 12) is the
  human layer over these flat records.
- **Reused from NarrativePro:** quest state-machine (States → Branches → Tasks
  with progress), data-task history, dialogue chunk model, speakers, tagged
  dialogue sets, stable-GUID binding (= our `RefId`) for aliases. **Dropped:**
  network replication, party, UObject/Blueprint, cinematic shots/montages.

## Lib layering

The condition evaluator must be usable by GAS abilities → it lives **low**
(`meadows-gameplay`, which already has tags/attributes/factions). Higher:
`meadows-script` (VM, ScriptVars, event bus, coroutine scheduler — deps
`meadows-gameplay`; sol2/Lua **PRIVATE**, never in headers) and
`meadows-narrative` (quests/dialogue — deps script + gameplay + data).

## Bricks

### (4a) Lua foundation — DONE 2026-06-14
- Deps: **lua 5.4.8** (built as a static lib from sources, à la imgui/stb) +
  **sol2 3.5.0** (header-only INTERFACE target, `SOL_ALL_SAFETIES_ON`), pinned
  in root `CMakeLists.txt`. New lib **`meadows-script`** (`script/`).
- `script/Vm.hpp` + `.cpp`: `script::Vm` — sol2 `sol::state` **behind a pimpl**
  (sol2/Lua never leak into headers). Sandbox: removes `os`/`io`/`load*`/
  `require`/`package`/`collectgarbage`/`math.random`. Deterministic `rng()`
  (seedable xorshift64*, `seedRng`). `run(code, ScriptContext&)` returns a
  `RunResult{ok, error}` (errors returned, never thrown, §8).
- `ScriptContext`: pointers to the acting entity's `AttributeSet` /
  `AbilitySystem` / `ScriptVars` / `GameplayTagRegistry`. The `self` proxy
  (sol2 usertype with index/new_index): `self.x` reads/writes ScriptVars,
  `self.<attr>` reads the **current** attribute value (writing throws — §2.9),
  `self:addTag/removeTag/hasTag`. reflect::Value ↔ Lua conversion for
  bool/int/float/string (+ Guid→string).
- `script/ScriptVars.hpp` + `.cpp`: `ScriptVars` runtime component
  (`map<str, reflect::Value>`) + `registerScriptComponents`.
- Tests `tests/ScriptTest.cpp`: ScriptVars round-trip via `self.x`, attributes
  read-only (write fails, value unchanged), addTag/hasTag, deterministic rng +
  `os` removed. Suite green (98 cases / 732 assertions); build clean.

### (4b) Event dispatch — DONE 2026-06-14
- `gameplay/event/EventBus.hpp` + `.cpp`: `Event` (kind = fnv1a of the name +
  generic payload: source/target entities, tag, value, name — covers
  hit/death/activate/trigger/task-progress without a typed struct per event).
  `EventBus::subscribe/unsubscribe/dispatch`; **Lua-agnostic** (`std::function`
  handlers); **deterministic** dispatch in subscription order; re-entrant
  (snapshots matching handlers before calling).
- Lua bridge in `script::Vm`: `bindEvents(bus)` exposes `events.on(name, fn)` —
  a Lua handler subscribes and receives a `{source, target, value, name, tag}`
  table on dispatch. `getNumber(global)` added for inspection/tests.
- Tests: `tests/EventBusTest.cpp` (order, kind filtering, payload, unsubscribe)
  + a Lua-subscription case in `tests/ScriptTest.cpp`. Suite green (101 cases /
  739 assertions).
### (4c) Condition evaluator — DONE 2026-06-14
- `gameplay/condition/Condition.hpp` + `.cpp`: `ConditionForm` (a Form — one
  clause, linked to its node by `parent`; ANDed per node; `negate` flips it).
  Clause kinds: `HasTag`, `AttributeAtLeast`, `AttributeAtMost`, `HasItem`,
  `Lua`. `EvalContext` = the entity's `AbilitySystem`/`Inventory`/tag registry +
  a **`luaPredicate` callback** (the Lua escape — gameplay must not depend on the
  VM, so the script layer supplies the callback; no cycle). `evaluateClause` +
  `conditionsPass(forms, node, ctx)` (AND; true if no clauses).
- Wired into abilities: `AbilityContext` gained an optional `eval`; `tryActivate`
  checks the ability's conditions when it is set (Phase-3 callers pass null →
  unchanged). `ConditionForm` registered in `registerGameplayFormTypes`.
- Script side: `Vm::evalPredicate(expr, ctx)` evaluates a boolean Lua expression
  with `self` bound (the real backing for the `Lua` clause's callback).
- Tests `tests/ConditionTest.cpp` (each clause, negate, AND, ability gating) +
  an `evalPredicate` case in `tests/ScriptTest.cpp`. Suite green (105 cases /
  756 assertions).
### (4d) Full GameplayAbility — DONE 2026-06-14
- `AbilityForm` gained a `script` field (optional Lua coroutine for latent/custom
  logic). The "full ability" = `tryActivate` (gates + 4c conditions + cost +
  cooldown + fixed effect) **plus** the script coroutine for `wait()`-driven
  logic. Activation conditions already wired in 4c.
- **Coroutine scheduler in `script::Vm`** (§2.8: Lua coroutines + a central
  scheduler): `startCoroutine(code, self, target)` runs the script on a
  `sol::thread`; `wait(t)` (global = `coroutine.yield(t)`) suspends; the entry is
  stored in a `std::list` (stable addresses for the bound `ScriptContext`s).
  `tickCoroutines(dt)` resumes due coroutines; `pendingCoroutines()`. **Needed
  `sol::lib::coroutine` in the sandbox.** Note: contexts hold component pointers
  for the coroutine's lifetime (safe while no structural ECS change / entity
  alive during the wait).
- `ScriptContext` extended: mutable `AttributeSet*` + a `FormDatabase*`, so
  `self:applyEffect(guid)` resolves an `EffectForm` and routes through
  `gameplay::applyEffect` (the §2.9-correct mutation path from Lua).
- Tests in `tests/ScriptTest.cpp`: a "wait 2s then apply a damage effect to the
  target" coroutine — pending while waiting, effect applied after the elapsed
  tick. Suite green (106 cases / 762 assertions).
### (4e) Quests — DONE 2026-06-14
- New lib **`meadows-narrative`** (`quest/`, deps gameplay + data). `quest/Quest`:
  `QuestForm`/`QuestStateForm`/`QuestBranchForm`/`QuestTaskForm` — NarrativePro's
  state machine decomposed into id-linked records (state → branches → tasks).
  Runtime `QuestLog` (current state + per-task progress + status). `beginQuest`;
  `onQuestEvent` progresses matching tasks (by event name + optional ancestor-
  aware tag filter) → takes a completed branch → enters its destination →
  Success/Failure finish. Queries `isActive`/`questState`/`taskProgress`/
  `questStatus`. `registerQuestFormTypes`.
- Tests `tests/QuestTest.cpp`: a "slay 2 bandits" quest progressing on tagged
  `OnDeath` events (wrong tag / wrong kind ignored) → Succeeded.

### (4f) Dialogue — DONE 2026-06-14
- `quest/Dialogue`: `DialogueForm` (rootNode) + `DialogueNodeForm` (parent,
  speaker, text, optional `event`, order) — a node tree by id. `DialogueRunner`:
  `start`/`currentLine`/`options(ctx)`/`select`/`end`. NPC lines + Player options;
  options are **gated by the 4c condition evaluator**; entering a node **dispatches
  its `event`** (4b) so quests/scripts react. Single-player (no replication/party).
- Tests `tests/DialogueTest.cpp`: chunk flow, a condition-gated option appearing
  when a tag is granted, `OnAccept` fired on select, advance to the NPC reply.

### Narrative demo — `game/scenes/NarrativeScene`
A two-step quest with a turn-in and reward, all in moddable records
(`base.toml`):
- The guard offers "Slay the Bandits"; the **Accept** option is gated to hide
  once the quest is active (a negated `HasTag Quest.Active`), **Brag** by
  `Status.Brave` (a checkbox), **Report** by `Quest.Ready`.
- Accepting fires `OnAcceptQuest` → `beginQuest`. The "bandit defeated" debug
  button dispatches `OnBanditDeath` (×2) → the quest reaches the intermediate
  **Report** state. Returning to the guard and picking **"The bandits are dealt
  with."** fires `OnReportToGuard` → the quest enters its `Success` state
  (`QuestStatus::Succeeded`) and the scene awards **200 copper** (an `Inventory`).
- The scene mirrors quest state into `Quest.Active`/`Quest.Ready` tags so the
  dialogue gates on it through the existing `HasTag` clause (no quest-aware
  condition clause needed — kept the gameplay→narrative edge out). Form types
  registered in `WorldDemoScene::onEnter`; selector button in `main.cpp`.

---

**Phase 4 complete (2026-06-14).** 108 test cases / 783 assertions green; full
build clean; `true-adventurer.exe` builds. New libs: `meadows-script`,
`meadows-narrative`. Visual run of the Narrative/Combat/Gameplay scenes owed to
the dev.

## Out of scope (Phase 8+ / deferred)
Save serialization of runtime state (ScriptVars/QuestLog/active effects) → Phase
5. Visual graph editors → Phase 12. Cinematics/audio, networking, party,
AngelScript/WASM.
