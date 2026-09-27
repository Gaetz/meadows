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

### E1.e — les sortilèges systématisés (cadre dev, 2026-09-27)
Cadre posé par le dev après le jet : **cinq formes** (créer, détruire,
transformer, contrôler, comprendre) × **un élément**, plus des
caractéristiques (portée, trajectoire, intensité, durée, zone, maintien).
Référence : **`docs/SPELLS.md`**.
- `SpellForm` (data, `SpiritForms.hpp`) = **enfant** de l'`AbilityForm`
  (pattern `parent`, comme ConditionForm) : l'ability reste l'activation
  (coût/cooldown/tags/skill, §6), le sort est l'effet sur le monde. Sans
  enfant, l'ability reste scriptée (Lua = soupape).
- `world/spirit/Spells` : `compileSpell` (validation par nom + bornes),
  `spellSupported` = **la matrice** (v1 : Créer × Eau en `point` et
  `stream`), `launchSpeedForRange` (portée à 45° → vitesse).
- Scène : `castSpirit` résout l'enfant, refuse hors matrice AVANT de payer,
  pré-checks (`point` : sol visé, ≤ portée — toast `spirit.tooFar` —, sol
  sec), `tryActivate`, `executeSpell` → file `pendingSpiritActions`.
  **Maintien** : un jet `channeled` vit tant que Q est tenu, repaye le coût
  toutes les `costPeriod` s via `gameplay::payAbilityCost` (nouveau, le
  même canAfford/applyEffect que tryActivate), s'arrête au relâché ou faute
  d'essence.
- Données : les deux sorts réécrits en `SpellForm` (`SpellWaterSpring`,
  `SpellWaterStream` maintenu 6 s max, −10/s) ; les scripts Lua des deux
  abilities retirés.
- Tests : `SpellsTest` (compilation, validation champ par champ, matrice,
  portée → vitesse, enfant de l'ability) + `payAbilityCost`.

### E1.f — livre de sorts, Détruire × Eau, Contrôler × Eau (demande dev, 2026-09-27)
- **UI brouillon** : `SpellForm.name` (clé LocString) affiché au-dessus des
  barres de statut (`#spell` dans `hud.rml`/`hud.rcss`, modèle `spellName`/
  `spellVisible`, `HudContext.spellName`) ; la **molette** cycle le livre
  (`buildSpellBook` : abilities portant un SpellForm, triées par editorId ;
  hors modale). Toujours pas de barre : c'est un brouillon.
- **Détruire × Eau** (`SpellWaterDrain`, sphère 2 m, 3 s, 40 m³/s) = la même
  source à débit négatif. **Règle noyau (WaterSim)** : une source négative
  relâche le pin sur son disque pendant le step (`applyPins(..., released)`),
  un débit d'entrée jamais — le trou dans un lac se voit et se re-remplit au
  déversoir. Test « a draining source releases the pin on its disc, an
  inflow is swallowed ». Aucun changement de format WSD.
- **Contrôler × Eau** (`SpellWaterHold`, maintenu, 6 m³/s, 10 s de capacité) =
  `world::SpiritHold` (aspire à la visée si l'eau est là, blob flottant
  `holdParticles`, relâché = source d'une seconde à la visée) ;
  `SpiritDirector::setHoldSource` (la source drainante de l'emprise dans la
  vue noyau). Le pré-check « sol sec » ne vaut que pour Créer.
- Doc : `docs/SPELLS.md` §4 (matrice, règle du pin, l'emprise).
- À valider : le trou dans un lac (40 m³/s contre ~24 rendus par la
  couronne : à régler en data), la lisibilité du blob, le relâché sur sol
  sec = une mare de `volume` m³.

### E1.g — l'eau des gestes en vrais volumes (demande dev, 2026-09-27)
« Créer de réels volumes d'eau quand on les déplace ou qu'on les projette. »
- `RenderSnapshot::WaterMeshInstance` : des soupes de triangles monde
  construites CPU chaque frame ; `WorldRenderer::drawWaterVolumes` les
  dessine avec **le pipeline des volumes d'eau placés** (même shader
  `watervolume`, alpha, depth test sans write) via un vertex buffer
  dynamique par id, mark/swept comme les quads. UBO élargi à 2 vec4
  (append) : `uWaterMeshInfo.x = 1` → le fragment prend la **normale
  géométrique** (`dFdx/dFdy`, facettes low-poly) au lieu de la nappe plate,
  alpha 0,8-0,95.
- `world/spirit/SpiritWaterMesh` (headless, testé) : `appendJetTube` (tube à
  6 faces le long de l'arc, rayon = √(débit/vitesse/π) → 2 m³/s à 14 m/s
  ≈ 21 cm, évasé ×1,4 vers la chute) ; `appendBlob` (sphère UV aplatie
  0,85, rayon = ∛(3V/4π) → 60 m³ ≈ 2,4 m). `LandscapeScene::
  extractSpiritWater` à l'extract (Phase 5 : le renderer ne lit que le
  snapshot).
- Les particules restent en **embruns** (jet 70/s au lieu de 240, blob
  40/s au lieu de 160) + l'éclaboussure d'impact.
- Ce que « réel » ne veut PAS dire ici : ni collision ni nage dans le blob
  ou le jet (l'eau simulée reste celle du sol) ; pas de réfraction
  (le shader des volumes n'en a pas) ; le tube ne se casse pas en gouttes.
- **Rendu = validation visuelle dev avant commit.**

### Réglage dev après validation d'E1.g (2026-09-27)
« Pas assez impressionnant » : **intensités × 8** (source 24 m³/s, jet
16, drain 320, emprise 48 — capacité 480 m³ ; plafonds de rayon des
maillages relevés : tube 1,5 m, blob 8 m) et **coût de maintien ÷ 4** :
`SpellForm.upkeepScale` (défaut 0,25, la fraction du coût d'activation
payée à chaque `costPeriod`), `payAbilityCost(..., scale)` applique le
même EffectForm de coût à magnitude mise à l'échelle (§2.9 intact).

### Réglage dev 2 après E1.g (2026-09-27) : le jet en sphères, portée × 3
- Le tube est retiré : le jet est un **chapelet de sphères d'eau** lancées
  toutes les 0,12 s (`JetSphere`, `SpiritJetList::advanceSpheres`), chacune
  avec la vitesse de la buse à SON lancement (balayer la visée laisse une
  traînée, pas un saut), rayon = ∛(3·débit·intervalle/4π) (16 m³/s →
  ~0,8 m), chacune **éclabousse là où elle tombe** (`Cue.Spirit.Water.Jet`
  par atterrissage, plus de cadence de 0,25 s). Un jet expiré ne lance plus
  rien et ne nourrit plus le noyau, mais reste jusqu'à ce que sa dernière
  sphère touche le sol. Les particules restent en embruns.
- `appendJetTube` reste disponible (testé) mais n'est plus utilisé.
- **Portée × 3** : sorts `point` 72 m (rayon de visée physique 160 m), jet
  60 m ⇒ 24 m/s à 45°.

### E1.h — Comprendre × Eau, l'intelligo (2026-09-27)
- `world/spirit/WaterReading` (headless, testé) : `readWater(inputs, x, z,
  sol)` lit **les sources que le gameplay croit déjà** — `WaterQuery`
  (surface, profondeur, courant), les corps cuits (lac par masque + niveau,
  rivière par distance à la polyligne et demi-largeur interpolée, mer sous
  le niveau), les sources d'esprit (disque couvrant la visée →
  « source d'esprit, N s restantes »). **Volume** : flood 4-connexe des
  cellules mouillées du snapshot depuis la visée, × 4 m² — *exact* si le
  flood n'a pas touché la marge de la fenêtre, sinon *estimation* = niveau
  du lac − sol sur le masque cuit (sous-échantillonné à ≤ 4096 prises) ; la
  mer n'a pas de volume. **Visée sèche** : l'eau la plus proche (cellules
  mouillées de la sim, bornes des lacs, nœuds des rivières) dans
  `searchRadius` (512 m), direction + distance ; `compassCode` (x = est,
  −z = nord = le yaw 0 de la caméra).
- Scène : `executeSpell` → `castWaterReading(at, duration, live)` (aucune
  action monde, pas de file), lignes formatées depuis les clés
  `reading.*`/`dir.*` (EN/FR) dans un panneau HUD `#reading` (440 dp, une
  ligne par slot `reading0..5` — RmlUi ne rend pas les `
` d'une chaîne
  liée ; retour dev : « fenêtre toute fine, illisible »), cue
  `Cue.Spirit.Water.Read` à l'ouverture. **Maintenu** (retour dev) : le sort
  est `channeled` (upkeep 0) — tant que Q est tenu la lecture suit la visée
  et se rafraîchit chaque frame (courant, volume vivants), relâcher la
  ferme ; un sort non maintenu resterait `duration` s.
- Données : `SpiritWaterUnderstand` (essence −3, cooldown 1 s) +
  `SpellWaterUnderstand` (understand × Water, point, 72 m, 8 s d'affichage)
  ; matrice : Comprendre × Eau × point.
- Tests `WaterReadingTest` : mare simulée (Pool, volume exact = Σ profondeur
  × 4), visée sèche → eau la plus proche à l'est, lac cuit (volume estimé
  = niveau − sol × aire), rivière (largeur, débit), source d'esprit, mer,
  rose des vents.
- v2 (quand la carte aura ses overlays) : gués, tracé du cours ; plus tard
  froid/pureté quand ces esprits existeront.

### E2.a — la Terre : pinceau extrait, bosse, creuser (2026-09-27)
Cadre dev pour la Terre (2026-09-27 soir) : **Créer** = une bosse (ce qui
est dessus est propulsé) ou, dans une autre version, tracer un mur ;
**Détruire** = creuser en maintenant ; **Contrôler** = déplacer des objets
liés à la terre (rochers ; l'intensité borne la taille contrôlable) ;
**Comprendre** = les caractéristiques du terrain ; **Transformer** =
changer la nature de la pierre (peindre de la pierre dure). Décision dev :
la save emporte des assets (`.ter`) → E2.b.
- `world/terrain/TerrainBrush` (headless, testé : raise+lower = zéro
  bit-exact, falloff symétrique, arêtes de chunk partagées, publish
  n'altère pas l'overlay publié, flatten contre la hauteur vive) :
  `applyTerrainBrush(grids, published, chunkSize, params, centre, dt,
  liveHeight)` + `publishBrushGrids`. `TerrainSculptTool` délègue
  (comportement identique).
- Scène : `Créer × Terre` → `applyEarthBump` (bosse instantanée de
  `intensity` m ÷ dureté de la classe dominante du sol —
  `SurfaceMaterialForm.hardness` via la table compilée —, publish commit,
  cue `Cue.Spirit.Earth.Spawn` + shake ; **propulsion** : le joueur par
  `CharacterBody::jump(√(2gh) × 1,5)`, les PNJ par un état aérien
  cinématique nouveau `Npc.airHeight/airVelocity` — ils n'ont pas de corps
  physique, ils sont collés au sol chaque frame — retombant sous gravité).
  `Détruire × Terre` → `SpiritEarth` (creuser sous la visée tant que Q est
  tenu : brush Lower `intensity` m/s ÷ dureté, preview à 20 Hz commit=false,
  **commit une fois au relâché** — collisions, scatter, snap des cellules ;
  upkeep 0,25 × coût/s ; cue `Cue.Spirit.Earth.Dig` toutes les 0,3 s).
  L'eau réagit pendant la fouille (`notifySimGroundChanged` est dans la
  closure de republish) : **creuser un canal devant une mare la draine, une
  bosse devant une source fait un barrage** — la boucle From Dust.
- Données : `SpellEarthBump` (create, 3 m, r 5, essence −8, cd 3 s),
  `SpellEarthDig` (destroy, maintenu, 2 m/s, r 4, 12 s max, essence −6 puis
  −1,5/s) ; matrice : Terre × Créer/Détruire × point.
- Non fait ici (briques suivantes, questions au dev) : le mur (Créer v2),
  Contrôler × Terre (rochers : il faut un corps mobile — les statiques Jolt
  ne bougent pas — et `StaticForm.surfaceMaterial`), Comprendre × Terre,
  Transformer × Terre (peindre la pierre : il n'existe pas de couche de
  patches de matériaux, seulement de hauteurs), E2.b persistance.

**Validation dev attendue (la phrase de la brique 1)** : en Play, Q vers une
pente → éclaboussure, l'eau jaillit au point visé, coule, s'accumule ; nage ;
l'essence baisse, le cooldown bloque 8 s ; save mi-source puis load → le
record revient et la mare se re-remplit. Console : `spirit list`.
