# Quests, dialogue events & the journal

Quests are **pure data** — decomposed records linked by GUIDs, progressed
by **events** fired from dialogue or the world. No code is needed to add
one; a quest mod is an ordinary plugin ([How plugins work](plugins.md)).

## The record family

| Type | Fields | Role |
|---|---|---|
| `QuestForm` | `editorId`, `displayName` | The quest itself (the journal title). |
| `QuestStateForm` | `parent` (quest), `displayName`, `kind` | One stage. `kind` = `Regular`, `Success` or `Failure` — reaching a Success/Failure state finishes the quest. |
| `QuestBranchForm` | `state`, `destination` | A transition: when every task of the branch completes, the quest moves from `state` to `destination`. |
| `QuestTaskForm` | `branch`, `displayName`, `event`, `filterTag`, `required` | One objective. It completes when `event` fires `required` times (default 1), optionally filtered by `filterTag` (the event's tag must match). |

The first `QuestStateForm` child of the quest is the starting state when
the quest begins.

## Events: how quests advance

Everything is driven by named events on the game's event bus:

- **Dialogue** fires an event when a node carrying `event = "..."` is
  entered or picked ([data model](data-model.md), `DialogueNodeForm`).
  This is how a quest is offered (`OnAcceptEasternMenace` in the base
  game) and turned in (`OnReportBandit`).
- **The world** fires events too: `OnDeath` fires whenever an actor dies,
  carrying the actor's first `Faction.*` tag — a kill task filters it
  with `filterTag = "Faction.Bandits"`.

A task like "kill 3 bandits" is just:

```toml
[[records]]
form = "YOUR-TASK-GUID"
type = "QuestTaskForm"
new = true
[records.fields]
branch = "YOUR-BRANCH-GUID"
displayName = "Chasser les bandits"
event = "OnDeath"
filterTag = "Faction.Bandits"
required = 3
```

## Gating dialogue on quest state

Quest progress is mirrored onto the player as **gameplay tags**
(`Quest.<EditorId>.Active`, `.Ready`, `.Done` in the base quest), so
dialogue options gate on it with ordinary `ConditionForm` records
(`HasTag`, `negate = true` for "not yet taken"). This is the same
condition evaluator used everywhere ([Effects & abilities](effects-and-abilities.md)).

## The journal

Press **J** in game. It lists every quest in the log with its current
objective and progress (n/m). The journal screen is a moddable RmlUi
document like every other screen ([UI modding](ui-modding.md)):
`ui/journal.rml`.

## Saving

Quest progress rides the save plugin like everything else: one
`SavedQuestForm` per quest (current state + status) and one
`SavedQuestTaskForm` per task with progress ([Save games](saving.md)).
You never have to do anything for your quest mod to be save-safe.

## Vendors & crime (economy v1)

Two small data hooks land with the same update:

- **Vendors** — an actor's `ActorForm` can carry `buyMult` / `sellMult`
  (0 = use the global tuning values). A vendor's stock re-rolls from its
  `LoadoutEntryForm` children when more than 24 in-game hours have
  passed since the last barter.
- **Crime** — assaulting a peaceful NPC in front of a witness puts a
  bounty on the player and the `Crime.Wanted` tag. Guards (actors
  tagged `Faction.VillageGuard`) turn hostile while it is set; a
  dialogue option gated `HasTag Crime.Wanted` + `HasItem` (gold) clears
  it. Your mod can gate anything on `Crime.Wanted` — it is an ordinary
  gameplay tag.
