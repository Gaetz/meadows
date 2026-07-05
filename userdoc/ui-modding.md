[← Back to the hub](README.md)

# UI modding

> **Status: the model is decided; the UI system is being wired in.** This
> page documents the contract so mods can plan for it.

Game screens (HUD, inventory, dialogue...) are **RmlUi documents** —
HTML/CSS-like files (`.rml` layout + `.rcss` styles) living in each
plugin's `ui/` folder.

## The registry

A `UiScreenForm` names each screen and points at its document:

```toml
[[records]]
form = "GUID-HUD-SCREEN"
type = "UiScreenForm"
new = true
[records.fields]
screen = "hud"
document = "hud.rml"
overlay = true
```

## Overriding a screen

Documents resolve through the plugin stack **by path, last plugin wins**:
ship `ui/inventory.rml` in your mod and, if you load after the base game,
players get YOUR inventory — the whole screen, restyled or rebuilt. This
is the model that made SkyUI possible, minus the decompiling.

Two levels of UI modding, then:

- **Reskin/rebuild a screen**: provide the same document path (no records
  needed at all).
- **Add new screens**: ship new documents + new `UiScreenForm` records,
  and open them from scripts or furniture (`FurnitureForm.screen`).

Text in documents references [localization](localization.md) keys, so a
modded screen stays translatable.

Related: [How plugins work](plugins.md) · [The data model](data-model.md)
