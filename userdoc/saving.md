[← Back to the hub](README.md)

# Save games

**A save is an ordinary plugin.** No custom binary blob, no parallel
format: when you save, the engine diffs the live world against the
resolved data and writes the differences as **field-level patch records**
— the exact mechanism mods use. Loading = resolving your mod stack with
the save as the **last layer**.

## Where and what

Saves live in `saves/<slot>.toml` next to the executable. **F5**
quicksaves, **F9** quickloads; the pause menu saves timestamped slots and
the main menu's *Load game* lists them. The console (`F8`) accepts
`save [name]` / `load [name]`.

Open one in a text editor — it reads like any plugin:

- **`ReferenceForm` patches** — a moved actor patches `position`; a
  picked-up item patches `enabled = false`; a re-homed actor patches
  `cell`. Only the changed fields appear.
- **Per-actor child records**, keyed by `parent` = the actor's reference
  guid: one `SavedStatsForm` (attributes, vitals BASE values, resonance,
  survival, buildup, posture, equipment slots), `SavedItemForm` per
  inventory stack, `SavedEffectForm` per ACTIVE durational effect,
  `SavedInjuryForm` per injury. Current values are never stored — they
  recompute from the bases on load (instant effects are already baked in).
- One **`WorldStateForm`** — game clock, active worldspace, camera.
- Prefab-derived children (a crate inside a placed prefab) materialize
  as full records under their deterministic derived guid.

Record guids are deterministic: saving twice produces the same
identities, so two saves **diff cleanly** under version control.

## Consequences worth knowing

- **Mods layer under saves.** Retuning a weapon in a mod affects a
  saved game immediately (the save only stores what CHANGED at runtime).
  Removing a mod mid-playthrough leaves the save's references to it
  dangling — they are skipped with a log, never fatal.
- **Unloaded cells remember without saving**: loot a crate, walk away,
  come back — the runtime keeps a pending patch layer in memory; saving
  to disk just flushes it.
- A mod (or a curious player) can hand-edit a save: it is data like
  everything else.

Related: [How plugins work](plugins.md) · [The data model](data-model.md)
