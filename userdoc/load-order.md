[← Back to the hub](README.md)

# Load order & conflicts

## `plugins.toml`

The file `plugins.toml`, in the game's data folder next to the plugin
files, lists what loads and in which order:

```toml
[[plugins]]
file = "base.toml"

[[plugins]]
file = "landscape.toml"

[[plugins]]
file = "my-mod.toml"
enabled = true        # optional, defaults to true
```

Top loads first. If the file is missing, the game loads every `.toml` in
the folder in alphabetical order.

You normally don't edit this by hand: the **in-game plugin manager**
([In-game tools](tools.md)) reorders, enables/disables and saves it.

## How conflicts resolve: last writer wins, per field

All plugins stack in order, and for **each individual field** the last
plugin that wrote it wins. Consequences:

- Two mods editing **different fields** of the same record: both apply.
  This is the normal, conflict-free case — most "conflicts" from other
  games simply don't exist here.
- Two mods editing the **same field**: the one lower in the load order
  wins. The plugin manager lists every such case (record, field, and the
  writers in order) so you can see exactly what overrode what.
- A mod can even patch another mod's records — just reference their GUIDs
  and load after it (declare it in `dependencies` to make the requirement
  explicit).

## Dependencies

```toml
[plugin]
id = "..."
name = "flame-swords-expansion"
dependencies = ["0a3f9c1e-5b71-4d2a-9c64-8e21d7b3f0a2"]
```

Dependencies declare "I patch or reference records from that plugin". The
manager uses them to warn about missing or badly-ordered requirements.

Next: [The data model](data-model.md) · [In-game tools](tools.md)
