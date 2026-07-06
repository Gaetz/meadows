[← Back to the hub](README.md)

# UI modding

> **Status: LIVE.** The game runs on these screens (HUD, inventory,
> container/loot, dialogue, barter, pause/main/wait menus, workstation)
> — every one of them a moddable document.

Game screens are **RmlUi documents** — HTML/CSS-like files (`.rml`
layout + `.rcss` styles) living in each plugin's `ui/` folder. The base
game's live in `data/base/ui/`; `theme.rcss` holds the shared look
(override that one file to reskin every screen at once).

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

## Data bindings a document can use

Screens read game state through named **data models** (RmlUi data
bindings). The slots each model exposes are fixed by the engine; the
document decides what to show and how. The live models:

| Model | Feeds | Notable slots |
|---|---|---|
| `hud` | the overlay | `healthPct/energyPct/essencePct/posturePct`, `healthText...`, `clock`, `prompt`, `talk`, `rows` (nameplates: `c0` name, `c1` health %, `c2/c3` screen px) |
| `inventory` | the player's item table | `rows` (`c0` name, `c1` weight, `c2` value/price, `c3` power, `tag` "equipped"), `search` (two-way), detail slots, events `tab/sortCol/pick/equipAction/useAction` |
| `container` | the loot side | `rows`, `title`, events `pickLoot/takeAll` |
| `barter` | the vendor side | `rows` (`c2` = buy price), `title`, `vendorGold`, event `pickBuy` |
| `dialogue` | the conversation | `npcName`, `npcLine`, `rows` (options), event `choose` |
| `menu` | pause/main/wait/workshop | `clockLine`, event `menuAction('...')` |

Two hard rules learned the painful way: put `data-model` on a wrapper
`div`, **never on `<body>`** (structural bindings like `data-for` are
silently skipped there), and never nest one `data-model` inside another.
Every screen is loaded once at startup, so a broken modded document
shows its errors in the log immediately.

Text in documents references [localization](localization.md) keys, so a
modded screen stays translatable (discipline pending — the base screens
still hardcode English).

Related: [How plugins work](plugins.md) · [The data model](data-model.md)
