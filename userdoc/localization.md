[← Back to the hub](README.md)

# Localization

Every player-facing string is a `LocStringForm` record whose **key is its
`editorId`** (like `quest.intro.title`) and whose payload is `text`.

**A language pack is an ordinary plugin** that patches `text` on existing
records:

```toml
[plugin]
id = "YOUR-PACK-GUID"
name = "french-pack"

[[records]]
form = "GUID-OF-THE-STRING"
type = "LocStringForm"
[records.fields]
text = "La Longue Route"
```

Because it's the standard patch system:

- switching language = enabling/disabling a plugin in the load order;
- a language pack can cover the base game AND any mod (patch their string
  records; declare them as dependencies);
- **mods localize themselves** by shipping their own `LocStringForm`
  records — and other people can ship packs for your mod.

Untranslated strings simply fall back to whatever the last writer left —
usually the original language.

Related: [How plugins work](plugins.md) ·
[Load order & conflicts](load-order.md)
