[← Back to the hub](README.md)

# Effects & abilities

Everything that changes a character's stats goes through **one record
type**: `EffectForm`. Damage, healing, buffs, poisons, diseases, drug
highs, enchantments — all the same building block, all pure data.
Activatable actions (attacks, spells, shouts) are `AbilityForm`s that
bundle a cost, a cooldown and an effect.

```toml
# A poison: 2 damage/second for 10 seconds
[[records]]
form = "GUID-POISON"
type = "EffectForm"
new = true
[records.fields]
editorId = "WeakVenom"
attribute = "health"
op = "add"
magnitude = -2.0
duration = "periodic"
period = 1.0
durationSeconds = 10.0
grantedTag = "Status.Poisoned"
```

Key ideas:

- **Attributes** (`health`, `energy`, `posture`, `armorRating`...) are only
  ever modified through effects — that's why buffs stack cleanly and
  saves stay small.
- **Tags** (`Status.Burning`, `State.InCombat`) are the shared vocabulary:
  effects grant them, abilities require or block on them, conditions test
  them.
- **Costs and cooldowns are themselves effects**, so anything can tweak
  them.

## Skills (progression by use)

Skills are data too. A `SkillForm` names a skill and how much XP one use
grants; `SkillThresholdForm` child records are its ranks — each carries
an XP threshold and the `EffectForm` applied (once, permanently) when it
is crossed. Point an ability at its skill with the `skill` field:

```toml
[[records]]
form = "GUID-SKILL"
type = "SkillForm"
new = true
[records.fields]
editorId = "OneHanded"
xpPerUse = 1.0

[[records]]
form = "GUID-RANK1"
type = "SkillThresholdForm"
new = true
[records.fields]
parent = "GUID-SKILL"        # the skill
xp = 25.0
effect = "GUID-PERK-EFFECT"  # any EffectForm — perks ARE passive effects

# on the ability that trains it:
#   skill = "GUID-SKILL"
```

Every successful activation of that ability grants the XP (and fires an
`OnAbilityUsed` event quests can listen to). A mod adds a rank by adding
one threshold record — the skill itself is never touched.

➡ **The complete reference lives in
[MODDING-EFFECTS.md](../docs/MODDING-EFFECTS.md)** — every field of
`EffectForm`/`AbilityForm`, the full attribute list, and recipes for
damage, DoTs, drugs, afflictions, buildup statuses and abilities.

Related: [The data model](data-model.md) ·
[Schedules & furniture](schedules-and-furniture.md)
