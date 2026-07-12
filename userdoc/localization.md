[← Back to the hub](README.md)

# Localization

Every player-facing string is a `LocStringForm` record whose **key is its
`editorId`** (like `quest.new` or `ui.pause.title`) and whose payload is
`text`.

## The model: English base + language packs

**English is the base language**: `base/text-en.toml` *creates* one record
per key. **A language pack is an ordinary plugin** that *patches* `text`
on those same records (field-level, last writer wins — the standard patch
system). The shipped French pack (`base/text-fr.toml`) is exactly that:

```toml
[plugin]
dependencies = [ '18a05b5b-eda3-4267-bbac-318a1d3989e7' ] # text-en
id = 'b754251d-39a5-40b3-9c1e-39d15a8125e8'

[[records]]
form = 'GUID-OF-THE-ENGLISH-RECORD'
type = 'LocStringForm'
[records.fields]
text = "La Longue Route"
```

Because it's the standard patch system:

- **switching language = enabling/disabling a plugin.** The game does it
  for you: any plugin file named `text-<code>.toml` is a language pack,
  enabled exactly when `<code>` matches `language` in `settings.toml`
  (the Options screen's *Language* toggle flips it live);
- a pack can cover the base game AND any mod (patch their string records;
  declare them as `dependencies`);
- **mods localize themselves** by shipping their own `LocStringForm`
  records — and other people can ship packs for your mod;
- an untranslated key falls back to the last writer — usually the English
  base; a *missing* key displays the key itself (visible and greppable,
  never a blank).

## Localizing screens: `data-loc`

UI documents (`ui/*.rml`) mark every static label with a `data-loc`
attribute; the inner text is the English authoring fallback:

```html
<button data-event-click="menuAction('resume')"
        data-loc="ui.pause.resume">Resume</button>
```

On document load (and again on a language switch) the engine replaces the
element's content with the `LocStringForm` text for that key. Strings the
game formats itself (toasts, prompts, the inventory footer…) use the same
keys with `{}` placeholders filled left to right (e.g.
`quest.new = "New quest: {} (journal: J)."`).

**Data content needs no keys**: `displayName`, dialogue node `text`,
quest names… are patched DIRECTLY on their records by a language pack —
one more `[[records]]` patch per form, same mechanism.

## Authoring pipeline: CSV → plugin

Strings are authored as a spreadsheet (`editorId,text`) and imported by
the cooker:

```sh
# The base (creates the records; keep the SAME plugin guid on re-imports):
cooker import-csv text-en.csv text-en.toml LocStringForm <base-guid>

# A pack (patches the base plugin's records; --patch names the TARGET):
cooker import-csv text-fr.csv text-fr.toml LocStringForm <pack-guid> \
    --patch <base-guid>
```

In `--patch` mode each row derives its identity from the *target* plugin
and `editorId`, emits a field patch (no `new = true`) carrying only the
value columns, and the generated plugin declares the target in its
`dependencies`. Re-running an import always yields the same guids, so
packs never break on a re-import.

Related: [How plugins work](plugins.md) ·
[Load order & conflicts](load-order.md) ·
[UI modding](ui-modding.md)
