# Phase 4 — Scripting, abilities, conditions, quests, dialogue (IN PROGRESS)

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
  order) assembles a node's children. The visual graph editor (Phase 9) is the
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

### (4b) Event dispatch — TODO
### (4c) Condition evaluator — TODO (structured clauses + Lua escape; the shared engine)
### (4d) Full GameplayAbility — TODO (Lua hook + coroutine scheduler `wait()` + activation conditions)
### (4e) Quests — TODO (records-by-id state machine; advance via events + conditions; aliases; data-task history)
### (4f) Dialogue — TODO (NPC/Player node graph; chunk runtime; condition-gated options; speakers; ImGui demo)

## Out of scope (Phase 5+ / deferred)
Save serialization of runtime state (ScriptVars/QuestLog/active effects) → Phase
5. Visual graph editors → Phase 9. Cinematics/audio, networking, party,
AngelScript/WASM.
