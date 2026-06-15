# Character statistics — design reference

> Canonical reference for the game's character-stats system. Distilled from the
> dev's design doc + decisions taken with the dev. **Read this before touching
> `gameplay/stats/`.** This is the *design* (what the stats are); the
> implementation journal lives in `docs/PHASE-4.6.md` / `PHASE-4.7.md`.
>
> **Scope tags:** `[4.6]` = vertical-slice core (built first, tested in 2D).
> `[4.7]` = full system (bolts onto the 4.6 machinery, still 2D). Everything is
> validated in 2D before streaming (Phase 5) and 3D (Phase 6).

The system is a richer, data-driven extension of the simplified GAS (CLAUDE.md
§6, `docs/PHASE-3.md`): attributes are reflected C++ AttributeSets, the only
mutator is a GameplayEffect (§2.9), state is expressed with GameplayTags. What is
**new** vs the Phase-3 GAS: multiple AttributeSets, **derived stats via formula**,
and **typed damage**.

## Naming (FR design doc → EN code)

Identifiers stay English (§8). New concept names were chosen with the dev.

| Design (FR)        | Code (EN)        | Notes |
|--------------------|------------------|-------|
| Santé / Énergie / Essence | health / energy / essence | the 3 primary (visible) stats |
| Force              | strength         | attribute |
| Constitution       | constitution     | attribute |
| Grâce              | grace            | attribute |
| Dextérité          | dexterity        | attribute |
| Alacrité           | alacrity         | attribute |
| Perception         | perception       | attribute (kept; not "acuity") |
| Charisme           | charisma         | attribute |
| Ego                | ego              | attribute |
| Clairvoyance/Mystique | insight       | attribute (starts at 0) |
| Déphasage          | **resonance**    | hidden signed stat; negative = dissonance |
| Sphère Onyx / Ambre / Grenat | onyx / amber / garnet | resonance channel for health / energy / essence |
| Harmonie           | harmony          | cross-channel resonance cascade |
| Blessure/Épuisement/Stress/Stimulation | Status.Wound / Exhaustion / Stress / Stimulation | temporary resonance status tags |
| Ecchymose / Plaie / Fracture | Injury.Bruise / Cut / Fracture | permanent injuries (4.7) |

## 1. Primary stats & attributes

Three **visible** primary stats: **health** (survival), **energy** (fatiguing
actions — every action but move/parry costs energy), **essence** (supernatural
actions / spells).

Nine **attributes**, grouped three-per-primary:

- **health:** strength, constitution, grace
- **energy:** dexterity, alacrity, perception
- **essence:** charisma, ego, insight

Attributes start at **6** on average (origin can modify), except **insight = 0**.
**Each attribute point grants +5 to its primary stat's maximum.** So a starting
character has maxHealth 90, maxEnergy 90, maxEssence 60. `[4.6]`

Growth: each level the player may add +5 to one of health/energy/essence max;
attributes also rise when associated skills pass thresholds (skills = separate
doc, later), which raises the associated primary. `[4.6]` for the derivation;
leveling UI later.

> **Derivation:** maxHealth = (strength+constitution+grace)·5, maxEnergy =
> (dexterity+alacrity+perception)·5, maxEssence = (charisma+ego+insight)·5.

## 2. Resonance (hidden stat) & Harmony

**Resonance** is a float per channel (onyx=health, amber=energy, garnet=essence),
range **-100..1500**, base **0**. It is a percentage modifier on the **maximum**
of its primary stat, and **every 15 points adds/subtracts 1 to all attributes
linked to that primary** (which cascades into the derived stats). `[4.6]`

It is only *visible* through its effects: bars that no longer refill to max (or
overfill), and the attribute bonus/malus. Temporary negative resonance is tagged
**Status.Wound** (health), **Status.Exhaustion** (energy), **Status.Stress**
(essence); positive is **Status.Stimulation**.

Lore: synchronization/desynchronization with the three spheres — Onyx (bodily
health/form/structure), Amber (insertion in time/causality), Garnet (essence /
persistence-in-being).

**Acquisition** `[4.6 minimal — one source; 4.7 full]`:
- Neglecting needs: hunger/thirst → health resonance; cold/heat → energy
  resonance; sleep → essence resonance. `[4.6 hunger only]`
- Using stats: losing health/energy/essence accrues a matching negative
  resonance; bars won't refill to max until an 8h rest. `[4.7]`
- Fighting negative resonance: enduring builds resistance — +0.001 positive
  resonance per negative point removed during an 8h rest. This shifts the
  character's *default* phase, so it does **not** affect attributes. `[4.7]`
- Drugs: sharp positive/negative resonance, time-limited, with an aftershock of
  negative resonance + side effects (8h/day rest removes 10 negative points). `[4.7]`

**Harmony** `[4.6]`: resonance channels are coupled. From the most-displaced
channel, a displacement cascades **half** to the next channel and **quarter**
(truncated) to the third, **once**, in order Amber→Garnet→Onyx (energy→essence→
health→…). Example: -10 health resonance ⇒ -5 energy ⇒ -2 essence. It is a
*minimum*, applied once from the most-displaced stat.

**Harmony break** `[4.7]`: most drugs break harmony (channels become independent)
during the positive effect; the aftershock usually restores harmony, so it hits
all three channels.

**Resonance as resistance to permanent statuses** `[4.7]`: permanent negative
statuses (injuries/diseases/psychoses) have a low inflict chance (~10%). Phase
acts as resistance: with non-negative resonance the status **cannot** apply; with
negative resonance of value `d`, the chance to actually take it is `d%`. Net
chance ≈ `inflict% · d%` (e.g. 10% · 10% = 1%).

## 3. Secondary (derived) stats

Derived from attributes (and equipment). All computed by **C++ calculators with
data-tuned constants** (`StatsTuningForm`), never stored as base — except the
**override/offset** mechanism (see §6) that lets non-humanoids / legendary gear
bypass the humanoid formula. Defaults below assume humanoid attributes 1–20.

### Defensive
- **health regen** — per second. 0.0002 · grace (very slow). Food increases it.
- **defense** `[4.6]` — flat physical reduction before %. constitution·0.5, capped
  at 25+constitution % of incoming damage.
- **critical sensitivity** — % health removed by a crit (on top of the weapon's
  crit damage). 25 − constitution·0.1.
- **armor/slash · blunt · pierce** `[4.6]` — % reduction of that physical type.
  0.5·strength / 0.5·constitution / 0.5·grace.
- **energy regen** — 35 + alacrity per second; pauses during an action, resumes
  after `0.9 − alacrity·0.1` s idle (min 0.5). Food: small %.
- **dodge** — % chance an incoming hit is nullified. Default 0 (rare/strong).
- **posture** `[4.6]` — poise points. 50 + alacrity.
- **posture regen** `[4.6]` — 2 + alacrity/3 per second. Food: small %.
- **vitality** — reduces a status's damage by % once its buildup is met. 1 +
  alacrity/4, capped at 25+alacrity %.
- **endurance/poison · bleed** — buildup duration. 100 + dexterity·0.5.
- **endurance/mental · disease · curse · death** — 100 + alacrity·0.5.
- **essence regen** — 0.005 · insight per second (slow).
- **will** — flat non-physical reduction before %. 1 + ego/4, capped at 25+ego %.
- **resistance/fire · cold · lightning · sonic · chemical · psychic · holy · dark
  · ether** `[4.6: fire+lightning only]` — % reduction; also raises the matching
  buildup by the same %. 0.5 · {charisma|ego|insight} per the table below.

Attribute → defense mapping:

| Primary | Attribute    | Physical armor | Endurance         | Elemental resistance |
|---------|--------------|----------------|-------------------|----------------------|
| health  | strength     | slash          | —                 | —                    |
| health  | constitution | blunt          | —                 | —                    |
| health  | grace        | pierce         | —                 | —                    |
| energy  | dexterity    | —              | bleed, poison     | —                    |
| energy  | alacrity     | —              | mental, disease   | —                    |
| energy  | perception   | —              | curse, death      | —                    |
| essence | charisma     | —              | —                 | fire, sonic, holy    |
| essence | ego          | —              | —                 | cold, chemical, dark |
| essence | insight      | —              | —                 | lightning, psychic, ether |

### Offensive
- **attack** `[4.6]` — flat damage; also unarmed damage. 5 + strength.
- **bonus attack** — flat damage of the weapon's type from its attribute(s).
- **slash / pierce / blunt attack** `[4.6 slash]` — flat physical of that type.
  A weapon's physical type is exclusive per attack animation (a short sword's
  side strikes deal slash, its thrust deals pierce).
- **fire / lightning / ice / holy / dark attack** `[4.6 fire]` (+ chemical / sonic
  / psychic / ether — NPC-only) — flat elemental, added to physical each hit.
- Each attack channel scales by `k · attribute %` (k usually 0.5–2.0, total per
  classic weapon = 2.0 ⇒ +40% at 20 attribute points; rare/legendary push beyond).
- **posture damage** `[4.6]` — per hit. base + base·(strength−5)%.
- **critical damage** — crit multiplier. 1.5 + (dexterity−0.5). Weapons can raise
  the base.
- **attack speed** — animation speed mult. 95% + 1%/alacrity.
- **armor penetration** — subtract from armor %, floor 0. (alacrity−5)·0.5%.
- **resistance penetration** — subtract from resistance %, floor 0. (insight−5)·0.5%.
- **status damage** — flat buildup points. value + value·(attribute−10)%.
- **range** — projectile range before drop.

Each attack animation carries **two motion values**: one multiplies weapon
damage, one multiplies posture damage (usually equal; lunges can be high-damage,
low-posture). A light-attack combo averages motion value 1.

### Social (0..100) `[4.7]`
- **beauty** — (strength+constitution)/2 + grace + charisma.
- **prestige** (apparat) — status/wealth impression. Default 0.
- **menace** — danger impression. Default 0.
- **suspicion** — dishonesty impression (draws guards). Default 0.
- **stealth** (discrétion) — (dexterity+alacrity)/2.
- **erudition** — (grace+insight+alacrity)/3 + 80·(1 − e^(−x/k)), x = books read.
- **faction reputation** — per-faction score −100..100, geographically located;
  one location's faction reputation partially influences another's.

### Utility `[4.7]`
- **encumbrance / max** — max = 50 + strength·10 + constitution·2.
- **encumbrance category** — light (<40% max, no effect) / medium (40–70%, −25%
  speeds, −44% accel) / heavy (70–100%, −50%, −75% accel) / overencumbered
  (>100%, −75% speed+accel, no jump/sprint/mount).
- **movement speed** — m/s at light jog (walk divides, sprint multiplies).
  90 + (alacrity + strength).
- **acceleration** — speed ramp; heavier encumbrance ⇒ slower; also braking inertia.
- **stealth speed** — 80 + alacrity·2 + dexterity + grace.
- **jump power** — 80 + (grace + dexterity + strength·2).
- **climb grip / swim speed** — % modifiers.
- **breath** (apnea) — seconds; at 0, lose health; −2× during sprint.

## 4. Combat model `[4.6 subset]`

- Attacks/actions (except move & parry) cost energy; at 0 energy, no action.
  Energy regen halves while parrying.
- A hit while parrying with no energy **staggers** the character (loses posture %
  = critical sensitivity, plus normal posture damage).
- **States:** normal, **staggered** (can't act/parry/dodge, very slow), **shaken**
  (like stagger but fast recovery, dodge-cancellable), **critical weakness**
  (posture at 0 → prostrate 5 s; open to a charged heavy = critical attack:
  removes % health = critical sensitivity + heavy·crit-damage mult, ignores armor;
  reduced damage from non-crit hits while down, encouraging the crit).
- **shaken** triggers when posture damage exceeds 15+constitution % of total posture.
- **death:** health 0 → unconscious; dies after `constitution` hours unless
  finished (instant) or stabilized (regains 1 health after max(1, 10−constitution/2)
  minutes). Charged heavy as the killing blow → chance of **dismemberment** (death
  in constitution/2 minutes; head = instant). Player + essential companions are
  immune to dismemberment.

> **4.6 slice:** posture + posture damage + posture→0 ⇒ `State.Staggered` (timed),
> reusing the `Combat.cpp::updateLifeState` pattern. Full critical-weakness /
> shaken / dismemberment / bleed-out timers are `[4.7]`.

## 5. Injuries & permanent statuses `[4.7]`

Permanent negative statuses (don't fade over time; need an action/sleep to remove)
gated by the ~10% inflict chance × resonance-resistance (§2). Health injuries:
**bruise** (light/hematoma, by body part: head/torso/arms/legs), **cut**
(light/major/severe; can be open → blood loss, or infected → cumulative energy
resonance), **fracture** (light/major/severe; longest to heal, max two at once).
Each carries a resonance penalty + per-body-part attribute/speed maluses, recovery
times (rest = time without combat/health-loss; starts after 4h sleep, +4h bonus
for 8h comfortable), aggravation chances, and treatment items (compress/herbs,
clean/dirty bandage, medical alcohol, needle & thread, splint). Diseases (energy)
and psychoses (essence) follow the same shape. Full tables in the design doc.

## 6. Implementation architecture

- **Multiple reflected AttributeSets** (CoreAttributes = the 9; Vitals = the 3
  primary + maxima; Defenses/Offense/Survival as built). The `AbilitySystem`
  current-value overlay aggregates fields across all present sets (key =
  `fnv1a(field name)`, names globally unique). `recomputeCurrent` is generalized
  to multiple sets.
- **Derived pass = C++ calculators + data constants** (§2.7/§6). A
  `DerivedStatRegistry` of `{ target, sourceSetMask, fn(StatView) }`. Order:
  base → derived value (`override ?? formula(attributes)`) → effect modifiers →
  clamp. **Opt-in**: a calculator runs only when its source sets are present, so
  the Phase-3 combat (TrainingDummy with authored maxHealth) stays unchanged.
- **Override / offset for non-humanoids** (§6 concern raised by the dev): humanoid
  formulas assume attributes 1–20; a monster (e.g. 90% armor) or legendary item
  authors a per-stat override/offset in data → the formula is bypassed. Keeps
  recomputation (never stale) while covering out-of-range stats.
- **Resonance** integrates into the derived pass: scales maxima by `(1 +
  resonance%/100)`, offsets linked attributes by `trunc(resonance/15)`, applies
  the Harmony cascade.
- **Game clock** (`GameClock`, timescale ×10) backs time-based stats (regen,
  survival, rest, injury recovery).
- **§2.9**: derived stats are *recomputed*, never set directly; survival/resonance
  mutate through the resonance/effect path. **§8**: any probability roll
  (injuries, dismemberment) goes through the engine RNG. **§5**: constants,
  overrides, and sets are moddable data.

See `docs/PHASE-4.6.md` for the brick-by-brick build of the 4.6 slice.
