# Chantier ESPRITS — un monde manipulé par les neuf esprits

> Journal du chantier (ouvert le 2026-09-27, après la clôture de CARTES —
> `docs/TERRAIN-MAPS.md`). Plan approuvé :
> `~/.claude/plans/ce-sont-de-spistes-stateful-truffle.md` (cadre, briques
> E1-E5, décisions). Doc utilisateur/moddeur : `userdoc/spirits.md` (à la
> livraison de la brique 1).

## 1. Pourquoi

Phase 1 du plan de développement : **une boucle de gameplay intéressante** sur le
monde procédural borné. Décision dev : avant de repasser la génération de
terrain (chantier PAYSAGE, backlog), construire le gameplay de manipulation du
monde par les 9 esprits de l'univers du jeu.

### Les neuf esprits — trois triades en shifumi
Chaque triade est un cycle calé sur les canaux de résonance de STATS :
**Onyx (matière) > Ambre (temporalité/énergie) > Grenat (esprit) > Onyx**.

| Triade | Matière (Onyx) | Énergie (Ambre) | Esprit (Grenat) |
|---|---|---|---|
| 1 | Végétation / esprit Arbre | Lumière / esprit Lune | Ténèbres / esprit Noir |
| 2 | Terre + contondant / esprit Marteau | Foudre + tranchant / esprit Lame | Psy (conscience) + perçant / esprit Miroir |
| 3 | Eau **et froid** (un seul esprit) | Flamme | Vent / souffle (cosmos) |

Le Psy est « un pouvoir qui active des choses » — ses interactions se
découvriront au playtest. Correspondance avec les endurances déjà codées :
Flamme→Ignition, Eau→Glaciation, Foudre→Électrocution, Psy→Mental,
Ténèbres→Curse, Lame→Bleed.

## 2. Ce que la recherche a fixé

- **BotW « chemistry engine »** : éléments (états non permanents) vs matériaux ;
  un élément change un matériau ; un élément change un élément ; **un matériau
  ne change jamais un matériau**. → la forme de notre table de règles, imposée
  par construction (`world/spirit/SpiritRules`).
- **Far Cry 2 fire** (J.-F. Levesque) : grille de cellules à points de vie,
  propagation modulée par `dot(vent, dir)`, humidité/matériau, **budget de
  points de propagation**, événements groupés, émetteurs en LOD. → E3.
- **From Dust** : règles simples uniformes, zéro cas spécial, réglage au
  playtest. → doctrine du cadre.
- **Eau en heightfield** (Müller, Kellomäki) : notre sim EST ce modèle, le
  kernel ne change pas ; les sources sont sa primitive native.
- **Portal 2 / Splatoon** : l'état « peint » d'une surface, « la dernière
  couche gagne » ; la difficulté = la relecture CPU. → vérité CPU = grille de
  couverture 2 m par chunk, qui est aussi l'entrée du rendu.
- **TotK** : engagement physique total ; matériaux « épaissis » pour la
  lisibilité. **Noita** : damier multithread — levier ultérieur.

## 3. Le cadre en une page

- Un esprit = un **champ simulé** (5 natures : volume qui coule, couverture
  de surface, vectoriel, radiatif, conduction ; + activation pour le Psy, +
  deltas de terrain pour la Terre). Le type unificateur est la **source
  placée** (`engine/terrain/SpiritField.hpp`, `SpiritSource`) : ce que
  l'ability a posé. Le champ est re-dérivé chaque tick ; **seules les sources
  persistent** (`SpiritSourceForm`, `world/worldspace/WorldForms.hpp`).
- Le shifumi des triades est une **fonction** de l'ordre de l'enum
  (`spiritDominates`), jamais des données ; les `SpiritRuleForm` précisent ou
  surchargent ; un acteur non-esprit est rejeté à la compilation.
- Les champs n'atteignent les acteurs que par `contactEffect` (un
  `EffectForm` à buildup) — §2.9.
- Seam ability → monde : `AbilityForm.script` (déclaré, jamais exécuté avant
  ce chantier) → `AbilityContext.scriptRunner` → `Vm::startCoroutine` ;
  bindings Lua qui ne font que **queuer** (idiome `bindMapTravel` → point sûr).

## 4. Journal des briques

### E1.a — sources runtime dans la sim d'eau (livrée, 2026-09-27)
- `WaterSim.cpp` : clamp ≥ 0 sur l'injection des sources (une décharge
  négative = un drain, jamais d'eau négative). Rien d'autre dans le kernel.
- `WaterSystem` : `setSimRuntimeSources()` — liste possédée par main, copiée
  par valeur dans le **job de step seulement** (jamais le pré-roll : le
  solveur stationnaire éterniserait une source de 10 s) ; `simSrcCache` reste
  frontière-seule (sinon doublage au scroll) ; le dump WSD2 emporte frontière
  + runtime → `cooker water-replay` reproduit une source castée.
- Tests : mare au-dessus du seuil de publication (cuvette : sur un plan
  mathématiquement plat le film s'étale sous 3 cm — cas pathologique de
  test, pas de jeu), décharge négative bornée à zéro, source hors fenêtre
  ignorée (le contrat du kernel), rigole connexe sur pente, requête de nage
  sur la mare. 33/33 tests eau.

### E1.b — données, directeur, persistance, console (livrée, 2026-09-27)
- Data : `data/forms/SpiritForms` (`SpiritForm` avec triade/canal/nature de
  champ/`contactEffect`/`damageType`, `SpiritRuleForm`, `SurfaceMaterialForm`),
  `game/data/base/spirits.toml` (9 esprits, 7 classes de matériau, premières
  règles — inertes tant que les noyaux n'existent pas).
- `world/spirit/SpiritSources` : la liste headless (cap 16, éviction du plus
  ancien, durées en secondes-sim, `capture()`/`apply()` = records §5, anneau
  de 7 taps pour un rayon > 4 m sans bump du format WSD).
- `world/spirit/SpiritRules` : compilation Forms → POD, shifumi par défaut,
  veto matériau-acteur.
- `game/scenes/SpiritDirector` (main thread ; la voie job arrive avec le feu),
  `SaveContext.extraRecords` (le créneau du journal de quêtes), `build(forms)`
  à l'entrée de scène (une source autorée et une source sauvée = le même
  record), push vers la sim à la fin d'`applyMapWorld`.
- Console : `spirit spawn Water <x> <z> [rate] [radius] [s]` / `spirit list` /
  `spirit clear`.
- Reporté à E3 : `StaticForm.surfaceMaterial` (changement de layout d'un Form
  partagé — inutile avant le feu).

### E1.c — le seam ability (livrée, 2026-09-27)
- `AbilityContext.scriptRunner` : `tryActivate` l'appelle **après le commit**
  (coût payé, cooldown posé, effet appliqué) et avant `OnAbilityUsed`,
  seulement si `AbilityForm.script` est non vide. `gameplay/` ne voit pas
  `script/` : l'hôte qui possède le Vm fournit le runner. Runner nul = le
  script reste inerte (comportement d'avant).
- `Vm::bindWorldActions({aim, spawnSource, pushTerrain})` → Lua `aim()`
  (`{x,y,z}` ou nil), `spirit.spawn(kind, x, z, rate, radius, seconds)`,
  `spirit.push_terrain(...)` (no-op loggé tant que la brique terre n'est pas
  là). Les callbacks **queuent** (`pendingSpiritActions`), appliqués au point
  sûr de `pendingMapTravel` ; `tickCoroutines(dt)` après
  `playerController.update` — le scheduler n'était tické nulle part.
- Visée : rayon œil → physique (64 m), rejet si le hit est à > 1 m du sol du
  terrain (rocher, tronc). Pré-checks **avant** de payer : viser le sol, sol
  sec (`waterSurfaceQuery` — verser dans un lac épinglé serait avalé par le
  pin) ; toasts `spirit.noGround` / `spirit.wetGround` / `spirit.refused`.
- Entrée : `InputAction::SpiritCast` = Q / RB, `PlayerContext.castSpirit`.
- Données (`spirits.toml`) : `SpiritWater` (essence −15 strict, cooldown 8 s
  `Cooldown.SpiritWater`, `blockedTag = State.Exhausted`, script = aim +
  spawn 3 m³/s, r 4 m, 10 s-sim), `SpringBurst` + `Cue.Spirit.Water.Spawn`
  (émis par la scène quand la source atterrit : `Cue.Spirit.<Kind>.Spawn`).
- Tests : `GameplayAbilityTest` (runner une fois par activation validée,
  jamais sur cooldown/tag bloquant, jamais sans script) ; `ScriptTest`
  (`spirit.spawn` depuis une coroutine, `aim()` nil → rien, `wait(1)` puis
  second jet au tick).
- Décision §4.9 du plan à garder en tête : un script sans `wait` qui boucle
  bloque la frame (comme tout `run`) — garde de resume plus tard.

### E1.d — le jet d'eau (demande dev, 2026-09-27)
Retour dev après validation de la source : « tirer un jet d'eau depuis le
personnage, simulé quand il tombe sur le sol ».
- `world/spirit/SpiritJets` (headless) : `jetLanding(origin, velocity, g,
  heightFn)` = l'arc balistique pas à pas (1/30 s) + une bissection au
  franchissement du sol → point d'atterrissage + temps de vol ; nullopt au-delà
  de 6 s (gouffre, rebord marin). `SpiritJetList` (cap 4, éviction du plus
  ancien) : `aim()` re-vise les jets « suiveurs » à chaque frame (même vitesse,
  buse et forward courants), `resolveLandings()`, `appendWaterSources()` = un
  disque par jet d'eau atterri. Un jet est **transitoire** (le geste du
  lanceur, comme une flèche en vol) : rien à sauver — l'eau de la sim ne l'est
  jamais, seules les sources le sont.
- `SpiritDirector` : `jetList()`, `tick()` rend les émetteurs des jets expirés,
  `waterSources()` = sources placées + jets atterris ; `SpiritForm.jetParticles`
  (APPEND, patché sur l'esprit Eau) = le ParticleForm du flux.
- Présentation : UN émetteur continu par jet (`WaterJetStream`, 240 p/s),
  `fx::ParticleSim::steerEmitter(id, velocity, lifetime)` (nouveau, engine/fx)
  re-vise le flux chaque frame et borne la vie des gouttes au **temps de vol**
  → le flux meurt au sol au lieu de le traverser ; `Cue.Spirit.Water.Jet`
  (éclaboussure `JetSplash`) toutes les 0,25 s au point d'impact.
- Scène : `PendingSpiritAction.jet/speed`, buse = œil + 0,45 m devant − 0,35 m
  (une main, pas l'objectif) ; `updateSpiritJets(dt)` dans la branche Play
  (aim → landings → steer → cue → push des sources chaque frame : une poignée
  de disques copiés par valeur) ; les jets meurent au swap de carte.
- Lua : `spirit.jet(kind, speed, rate, radius, seconds)`. Ability
  `SpiritWaterJet` (essence −10 strict, cooldown 4 s) = `spirit.jet("Water",
  14, 2, 2, 3)` : 3 s de flux à 2 m³/s qui suit la visée. **Q = le jet par
  défaut** ; console `spirit cast SpiritWater` rend la source, `spirit cast
  <EditorId>` choisit n'importe quelle ability tant qu'il n'y a pas de barre
  d'esprits.
- Tests : `SpiritJetsTest` (portée balistique exacte à 1 %, mur qui attrape
  l'arc, gouffre = nullopt, buse enterrée, suiveur re-visé, disque d'eau,
  expiration rendant les émetteurs, cap) ; `ScriptTest` `spirit.jet`.
- Limites v1 : tap = 3 s de jet (pas de maintien/drain), le jet atterrit sur le
  terrain seul (au-dessus d'un lac épinglé l'eau est avalée par le pin, comme
  la source), pas de collision des gouttes avec les props.

**Validation dev attendue (la phrase de la brique 1)** : en Play, Q vers une
pente → éclaboussure, l'eau jaillit au point visé, coule, s'accumule ; nage ;
l'essence baisse, le cooldown bloque 8 s ; save mi-source puis load → le
record revient et la mare se re-remplit. Console : `spirit list`.
