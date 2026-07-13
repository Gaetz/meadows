[← Back to the hub](README.md)

# In-game tools

## The Game DB editor

Open **Game DB (editor)** from the menu. It shows the ENTIRE resolved game
database — every record of every type, from every enabled plugin.

- **Browse**: filter by type, search by `editorId`.
- **Edit**: select a record — every field appears in the property panel
  (the panel is generated from the data model itself, so new fields and
  types show up automatically). Full undo/redo.
- **Create**: pick a type, hit *New* — a fresh GUID is minted for you.
- **Export plugin**: your pending edits become a standard `.toml` mod
  file, added to the load order, containing ONLY what you changed. This
  is the fastest way to make a tweak mod: edit, export, done. Edits apply
  after *Reload data* (or a relaunch).
- **Anim Preview** (Windows menu): select an anim clip or an anim graph —
  it plays on its skinned mesh in 3D (drag to orbit, wheel to zoom). A
  clip scrubs with its timeline events highlighted; a graph runs LIVE
  with sliders for its parameters and checkboxes for its tag gates, so
  you can trigger transitions and watch the cross-fades. Your unsaved
  edits preview immediately.

## The level editor (in the 3D scene)

Press **F6** in the Landscape scene. Everything you do writes RECORDS,
and *Export* saves them as an ordinary mod (`data/mods/level-edits.toml`)
loaded on the next run — your level edits ARE a plugin.

- **Pick**: left-click an object. **Gizmo**: move/rotate/scale with the
  handles; `1`/`2`/`3` switch the operation. The record is patched when
  you release. Tick **Snap** to move on a metric grid (step configurable)
  and rotate on a 15° lattice.
- **Duplicate**: with an object selected, the *Duplicate* button (or
  **Ctrl+D**) clones it one meter aside — one Ctrl+Z removes the copy.
- **Place**: arm a palette entry (Statics / Lights / Prefabs), click the
  ground. `Esc` cancels.
- **Group into a prefab**: Ctrl+click several objects, name it, *Create
  prefab from group* — the originals collapse into one reusable prefab
  you can place again from the palette.
- **Sculpt terrain**: check *Sculpt terrain*, choose Raise/Lower/Flatten/
  Smooth, paint with the left button. *Save terrain to mod* stages the
  grids; *Export* writes them with the rest.

### First steps, in order

1. Open the **Landscape (3D)** scene and press **F6** — the *Level
   editor* window appears (if not: F10 un-hides the panels).
2. **Move an object**: left-click a rock. Colored gizmo handles appear on
   it — drag them. Press `2` to rotate, `3` to scale, `1` back to move.
   Release the mouse: the change is recorded.
3. **Sculpt**: in the Level editor window, tick **Sculpt terrain**. Now
   the left mouse button is a brush — hold it on the ground and the
   terrain rises (pick *Lower*, *Flatten* or *Smooth* in the *Brush*
   combo; *Radius*/*Strength* sliders tune it). Grass, props and
   collision follow when you release. Untick *Sculpt terrain* to go back
   to selecting objects.
4. **Keep your work**: *Save terrain to mod* (if you sculpted), then
   **Export mod** — everything lands in `data/mods/level-edits.toml` and
   is loaded automatically on the next launch.
5. To fly while editing: hold the **left button on the sky/ground far
   away won't select** — use LMB-hold **+ WASD** like the normal fly
   camera (the camera only captures the mouse while you hold and move).

## The plugin manager

The **Plugins** window shows the load order: reorder with the arrows,
enable/disable with the checkboxes, *Save order* writes `plugins.toml`.
Below it, the **conflict report** lists every field written by more than
one plugin, with the writers in order — the last one is what the game
uses ([why](load-order.md)).

## The developer console

The **Console** window understands:

| Command | Effect |
|---|---|
| `help` | list commands |
| `find <text>` | list records whose editorId matches |
| `get <EditorId>.<field>` | read any field of any record |
| `set <EditorId>.<field> <value>` | edit it (becomes a pending edit — export to keep it) |
| `undo` / `redo` | edit history |
| anything else | runs as **Lua** on the game's script VM |

`set` goes through the same edit session as the Game DB editor: console
tweaks export to a plugin exactly the same way.

The IN-GAME console (F8, Landscape scene) adds `spawn`, `tp`, `tgm`,
`settime`, `startquest`, and `setstage <quest> <state>` (jump a quest to
any of its states by editorId).

## Checking a mod: `cooker validate`

The command-line cooker can lint a load order before you ship it:

```
cooker validate base.toml my-mod.toml
```

It resolves the plugins IN THAT ORDER and reports:

- **errors** (exit code 1 — fix before shipping): patches to records
  nothing creates, missing/misordered dependencies, and **dangling
  references** — any GUID field pointing at a record or asset that no
  listed plugin provides;
- **information**: per-field conflicts. Two plugins writing the same
  field is normal layering (the last one wins) — the report just shows
  who wrote what, in order.

Validate your mod TOGETHER with the plugins it builds on, or references
into them will (correctly) show up as dangling.

Related: [How plugins work](plugins.md) ·
[Load order & conflicts](load-order.md)
