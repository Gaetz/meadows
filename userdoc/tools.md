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

Related: [How plugins work](plugins.md) ·
[Load order & conflicts](load-order.md)
