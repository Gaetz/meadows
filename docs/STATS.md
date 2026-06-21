# Character statistics — design reference

> Canonical reference for the game's character-stats system. Distilled from the
> dev's design doc + decisions taken with the dev. **Read this before touching
> `gameplay/stats/`.** This is the *design* (what the stats are); the
> implementation journal lives in `docs/PHASE-6.md` / `PHASE-7.md`.
>
> **Scope tags:** `[6]` = vertical-slice core (built first, tested in 2D).
> `[7]` = full system (bolts onto the 6 machinery, still 2D). Everything is
> validated in 2D before streaming (Phase 8) and 3D (Phase 9).

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
| Ecchymose / Plaie / Fracture | Injury.Bruise / Cut / Fracture | permanent injuries (7) |

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
character has maxHealth 90, maxEnergy 90, maxEssence 60. `[6]`

Growth: each level the player may add +5 to one of health/energy/essence max;
attributes also rise when associated skills pass thresholds (skills = separate
doc, later), which raises the associated primary. `[6]` for the derivation;
leveling UI later.

> **Derivation:** maxHealth = (strength+constitution+grace)·5, maxEnergy =
> (dexterity+alacrity+perception)·5, maxEssence = (charisma+ego+insight)·5.
> The maxima derive from the attributes' **base** (starting / leveled) value, not
> their current value: a *temporary* attribute change (Resonance, buffs) must
> **not** move the max — only Resonance's % does (§2); a *permanent* change
> (leveling) does. (Decided with the dev to avoid Resonance hitting the max
> twice.) The Resonance attribute offset still feeds the **secondary** stats,
> which read the current value.

## 2. Resonance (hidden stat) & Harmony

**Resonance** is a float per channel (onyx=health, amber=energy, garnet=essence),
range **-100..1500**, base **0**. Two effects `[6]`: (1) a **percentage modifier
on the maximum** of its primary stat — `max × (1 + r/100)` — the *only* direct
effect on the max; (2) **every 15 points, ±1 to all attributes linked to that
primary** (`trunc(r/15)`), which lowers/raises the attributes' *current* value and
thereby the **secondary** stats derived from them. Because the maxima derive from
*base* attributes (§1), effect (2) does **not** touch the max — avoiding a double
hit.

It is only *visible* through its effects: bars that no longer refill to max (or
overfill), and the attribute bonus/malus. Temporary negative resonance is tagged
**Status.Wound** (health), **Status.Exhaustion** (energy), **Status.Stress**
(essence); positive is **Status.Stimulation**.

Lore: synchronization/desynchronization with the three spheres — Onyx (bodily
health/form/structure), Amber (insertion in time/causality), Garnet (essence /
persistence-in-being).

**Acquisition** `[6 minimal — one source; 7 full]`:
- Neglecting needs: hunger/thirst → energy (amber) resonance; sleep → essence
  (garnet) resonance; cold/heat → health (onyx) resonance. `[6: hunger,
  thirst, sleep; temperature → 7]`
- Using stats: losing health/energy/essence accrues a matching negative
  resonance; bars won't refill to max until an 8h rest. `[7]`
- Fighting negative resonance: enduring builds resistance — +0.001 positive
  resonance per negative point removed during an 8h rest. This shifts the
  character's *default* phase, so it does **not** affect attributes. `[7]`
- Drugs: sharp positive/negative resonance, time-limited, with an aftershock of
  negative resonance + side effects (8h/day rest removes 10 negative points). `[7]`

**Harmony** `[6]`: resonance channels are coupled. From the most-displaced
channel, a displacement cascades **half** to the next channel and **quarter**
(truncated) to the third, **once**, in order Amber→Garnet→Onyx (energy→essence→
health→…). Example: -10 health resonance ⇒ -5 energy ⇒ -2 essence. It is a
*minimum*, applied once from the most-displaced stat.

**Harmony break** `[7]`: most drugs break harmony (channels become independent)
during the positive effect. When the drug wears off, harmony is restored and the
aftershock kicks in — but not as an instant hit to persistent resonance. Instead,
each affected channel accumulates a **progressive penalty** (initially equal to
`aftershockResonance`, e.g. −30) that fades back to 0 at `aftershockRecoveryPerHour`
pts/game-hour (default 1). Multiple drug aftershocks accumulate independently.
Because this penalty is **transient** (not baked into persistent resonance), it
stacks correctly across repeated drug use and does not interact with the 8h rest
recovery (which operates on `persistent` only). With harmony restored, the harmony
cascade applies to the aftereffect, so it spreads to the other channels.

**Resonance as resistance to permanent statuses** `[7]`: permanent negative
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
- **defense** `[6]` — flat physical reduction before %. constitution·0.5, capped
  at 25+constitution % of incoming damage.
- **critical sensitivity** — % health removed by a crit (on top of the weapon's
  crit damage). 25 − constitution·0.1.
- **armor/slash · blunt · pierce** `[6]` — % reduction of that physical type.
  0.5·strength / 0.5·constitution / 0.5·grace.
- **energy regen** — 35 + alacrity per second; pauses during an action, resumes
  after `0.9 − alacrity·0.1` s idle (min 0.5). Food: small %.
- **dodge** — % chance an incoming hit is nullified. Default 0 (rare/strong).
- **posture** `[6]` — poise points. 50 + alacrity.
- **posture regen** `[6]` — 2 + alacrity/3 per second. Food: small %.
- **vitality** — reduces a status's damage by % once its buildup is met. 1 +
  alacrity/4, capped at 25+alacrity %.
- **endurance/poison · bleed** — buildup threshold. 100 + dexterity·0.5.
- **endurance/mental · disease · curse · death** — 100 + alacrity·0.5.
- **endurance/ignition** — 100 + resistFire (= 100 + charisma·0.5).
- **endurance/glaciation** — 100 + resistCold (= 100 + ego·0.5).
- **endurance/electrocution** — 100 + resistLightning (= 100 + insight·0.5).
- **essence regen** — 0.005 · insight per second (slow).
- **will** — flat non-physical reduction before %. 1 + ego/4, capped at 25+ego %.
- **resistance/fire · cold · lightning · sonic · chemical · psychic · holy · dark
  · ether** `[6: fire+lightning; 7: cold]` — % reduction; also raises the matching
  elemental buildup threshold by the same value. 0.5 · {charisma|ego|insight} per
  the table below. (`resistCold = 0.5·ego`; `resistFire = 0.5·charisma`; `resistLightning = 0.5·insight`.)

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

### Elemental buildup statuses `[7]`

The three elemental buildups work like poison/bleed but trigger different effects.
Buildup decays at 3/s before the status is acquired; at 1% of threshold/s once
acquired (flat rate — status lasts ~100 s from threshold). Cannot re-acquire
while active. Status expires when buildup reaches 0.

**Ignition** (`Status.Ignited`). Triggered by fire buildup reaching `enduranceIgnition`.
- **Ongoing** while buildup decays: loses **0.2% of maxHealth per second**.
  Reduced by vitality. Unlike poison (which is a flat 1 HP/s), ignition scales
  with the target's max HP so it is a % drain.

**Glaciation** (`Status.Glaciated`). Triggered by cold buildup reaching `enduranceGlaciation`.
- **On trigger**: paralyzes for 3 seconds (reuses `staggerSeconds`).
- **Ongoing** while buildup decays: slows energy regeneration to **0.7× normal**.

**Electrocution** (`Status.Electrocuted`). Triggered by lightning buildup reaching
`enduranceElectrocution`.
- **On trigger**: collapses posture to 0 instantly (opening a stagger/critical window).
- **Ongoing** while buildup decays: drains **0.2% of maxEssence per second**.

**Resistance duality.** Each elemental resistance serves two roles simultaneously:
it reduces incoming elemental damage *and* it raises the matching buildup threshold
by the same amount. High-charisma characters are harder to ignite *and* take less
fire damage; this is the same stat, not two separate systems.

### Offensive
- **attack** `[6]` — flat damage; also unarmed damage. 5 + strength.
- **bonus attack** — flat damage of the weapon's type from its attribute(s).
- **slash / pierce / blunt attack** `[6 slash]` — flat physical of that type.
  A weapon's physical type is exclusive per attack animation (a short sword's
  side strikes deal slash, its thrust deals pierce).
- **fire / lightning / ice / holy / dark attack** `[6 fire]` (+ chemical / sonic
  / psychic / ether — NPC-only) — flat elemental, added to physical each hit.
- Each attack channel scales by `k · attribute %` (k usually 0.5–2.0, total per
  classic weapon = 2.0 ⇒ +40% at 20 attribute points; rare/legendary push beyond).
- **posture damage** `[6]` — per hit. base + base·(strength−5)%.
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

### Social (0..100) `[7]`
- **beauty** — (strength+constitution)/2 + grace + charisma.
- **prestige** (apparat) — status/wealth impression. Default 0.
- **menace** — danger impression. Default 0.
- **suspicion** — dishonesty impression (draws guards). Default 0.
- **stealth** (discrétion) — (dexterity+alacrity)/2.
- **erudition** — (grace+insight+alacrity)/3 + 80·(1 − e^(−x/k)), x = books read.
- **faction reputation** — per-faction score −100..100, geographically located;
  one location's faction reputation partially influences another's.

### Utility `[7]`
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

## 4. Combat model `[6 subset]`

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

> **6 slice:** posture + posture damage + posture→0 ⇒ `State.Staggered` (timed),
> reusing the `Combat.cpp::updateLifeState` pattern. Full critical-weakness /
> shaken / dismemberment / bleed-out timers are `[7]`.

## 5. Injuries & permanent statuses

Permanent negative statuses don't fade with time — they recover only over **Rest**
(in-game time without taking a hit; §4/F4) and with treatment. Inflict is gated:
a low base chance (from the hit) **× resonance-resistance** (§2 — with non-negative
resonance the injury **cannot** apply; with negative onyx `d`, the chance is scaled
by `|d|/100`), rolled via the engine RNG (§8). Health injuries are **bruise**,
**cut**, **fracture**, by body part (head/torso/arms/legs). Diseases (energy) and
psychoses (essence) follow the same shape (N3).

Each injury carries an **onyx resonance penalty**, a **per-body-part attribute
malus** (and a leg **speed** malus), and a **recovery time** (in rest-hours; when
it elapses the severity drops one rank, then the injury clears). `N2` implements
this core; **open/infected** cut sub-states, **aggravation**, **treatment items**
(compress/herbs, clean/dirty bandage, alcohol, needle & thread, splint) and the
exact source-probability tables are `[7+]` (a follow-up).

**Bruise** (`Injury.Bruise`, severity light/hematoma; one per body part). Resonance
−1/−2. Recovery 24h/rank.

| Body part | Malus (light / hematoma) |
|-----------|--------------------------|
| head  | grace 0 / −1 |
| torso | strength 0 / −1 |
| arms  | dexterity 0 / −1 |
| legs  | movement speed 0% / −5% |

**Cut** (`Injury.Cut`, light/major/severe). Resonance −1/−2/−4. Recovery 48h/rank.

| Body part | Malus (light / major / severe) |
|-----------|--------------------------------|
| head  | alacrity −1 / −2 / −3 |
| torso | constitution −1 / −2 / −3 |
| arms  | dexterity 0 / −1 / −2 |
| legs  | strength −1/−2/−3 + speed 0% / −5% / −10% |

**Fracture** (`Injury.Fracture`, light/major/severe; longest, max two at once).
Resonance −10/−20/−30. Recovery 72h/rank.

| Body part | Malus (light / major / severe) |
|-----------|--------------------------------|
| head  | alacrity −1 / −3 / −4 |
| torso | ego −1 / −3 / −4 |
| arms  | dexterity −1 / −2 / −3 |
| legs  | strength −1/−2/−3 + speed −10% / −25% / −40% |

A second injury of the same type on the same part **aggravates** it (severity +1,
capped). Maluses feed the stat system through `StatModifiers`; the resonance
penalty feeds the onyx channel (so it also resists further injuries, §2). Leg speed
maluses need a `movementSpeed` derived stat (added with N2; full utility later).

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

See `docs/PHASE-6.md` for the brick-by-brick build of the 6 slice.
