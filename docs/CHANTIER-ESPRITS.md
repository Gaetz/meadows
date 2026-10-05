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

### E2.c / E2.d — Intelligo de la terre, Pinceau de pierre, Mur de pierre (2026-09-27)
Décisions dev : (1) Contrôler × Terre = rochers avec **vraie physique**
(intensité = taille max, le rocher roule et pèse) ; (2) **deux** sorts de
mur : Pinceau de pierre (crête qui suit la visée en maintenant) et Mur de
pierre (droit, de l'appui au relâché) ; (3) les **transformations**
(Transformer × tout élément : eau → glace, peindre la pierre…) = un
**chantier futur TRANSFORMATIONS**, à ouvrir quand les éléments auront leurs
noyaux ; (4) Comprendre × Terre tout de suite.
- **Trajectoire `line`** (`SpellTrajectory::Line`) : le sol visé à l'appui →
  le sol visé au relâché (`SpiritLine`, `updateSpiritLine`).
  `world::applyTerrainWall` (headless, testé : hauteur pleine le long de
  l'axe sans accumulation, symétrie, extrémités rondes, segment dégénéré =
  bosse) : falloff sur la distance perpendiculaire, une seule application.
- **Pinceau de pierre** (`SpellEarthPaint`, create, point, maintenu) = le
  même `SpiritEarth` que Creuser avec `raise = true` (2 m/s ÷ dureté, r 3).
- **Mur de pierre** (`SpellEarthWall`, create, line, 4 m de haut ÷ dureté au
  milieu, demi-largeur 1,5 m, essence −12, cd 4 s).
- **Intelligo de la terre** (`SpellEarthUnderstand`, maintenu, upkeep 0) :
  classe du sol (splat dominant) + dureté, pente + altitude, humidité de
  biome + climat (`RegionFields`), inflammabilité + moiteur
  (`SurfaceMaterialForm`) — même panneau HUD (`ReadingKind`), clés
  `ground.*`/`material.*`.
- Matrice Terre : Créer × point/line, Détruire × point maintenu,
  Comprendre × point ; Contrôler → E2.e (physique dynamique).

### E2.e — Contrôler × Terre : les rochers à physique dynamique (2026-09-27)
Première **physique dynamique** du moteur (le plan §4.1 la réservait à un
chantier à part ; décision dev : « le rocher obéit à la physique, roule et
pèse »).
- Façade Jolt (`engine/physics`, pimpl intact) : `addDynamicConvex`
  (enveloppe convexe des sommets du modèle, échelle cuite, layer MOVING,
  masse ≈ volume × 2000 kg/m³, friction 0,7, sommeil), `setKinematic`
  (Kinematic ↔ Dynamic), `moveKinematic` (le corps tenu est *conduit* :
  il traverse le monde et pousse ce qu'il rencontre), `setLinearVelocity`,
  `bodyPose`. Test headless : un cube convexe tombe et se pose, est porté
  cinématiquement à une cible, relâché avec une vitesse il vole et se repose.
- `StaticForm.surfaceMaterial` (APPEND ; « rock » sur MossyRock/PaintedRock
  par patch dans `spirits.toml`, qui déclare sa dépendance à adventure) :
  ce que l'esprit peut saisir (et plus tard brûler).
- Scène : `seizeRock` = le rocher `rock` le plus proche à `areaRadius` de la
  visée dont le **rayon englobant ≤ `intensity`** (« l'intensité borne la
  taille »), son corps statique rendu par `StreamingController::
  takeStaticCollider` (jamais re-cuit : ensemble `seized`), re-créé en
  enveloppe convexe dynamique puis cinématique tant que Q est tenu — porté
  2,5 m + rayon au-dessus de la visée (ou 6 m devant l'œil sans sol visé),
  upkeep 0,25 × coût/s ; relâché = dynamique avec la vitesse du portage
  (il vole, retombe, roule). Après le tick physique, chaque rocher saisi
  copie la pose de son corps dans son `Transform` (rendu inchangé) ; à la
  mort de l'entité (cellule déchargée) le corps part ; au swap de carte
  tous partent.
- Sort `SpellEarthControl` (control, point, maintenu, 3 m max, portée
  216 m, reach 8 m, essence −8 puis −2/s).
- Limites : le rocher déplacé n'est **pas encore persisté** (E2.b :
  marqueur « déplacé » + position sauvée comme offset au sol pour rester
  compatible avec le snap) ; un rocher qui roule sur le joueur ne le
  pousse que par la résolution du `CharacterVirtual` (hors broadphase).

### E2.b — la save emporte le terrain reshapé et les rochers déplacés (2026-09-27)
Décision dev : oui à la save avec assets.
- `world::stageTerrainPatchRecords` (headless, testé en aller-retour
  plugin TOML → resolve → `buildHeightPatches`) : pour chaque chunk touché,
  un `.ter` sous `saves/<slot>/terrain/patch_x_z.ter`, une `AssetEntry`
  (guid déterministe par chunk dans l'espace `7e88a112-…`) et un record
  `TerrainPatchForm` — **patch** de `asset` si le chunk est déjà autoré
  (une base/un mod), `creates` sous `7e88a111-…` sinon. §5 tel quel : la save
  est un plugin de plus, ses assets layerent par guid.
- `SaveContext.stageAssets` : appelé sur la frame avant que la sérialisation
  ne la quitte ; la scène y verse `spellTouchedChunks` (tout commit de
  `republishTerrain` : sorts ET strokes de sculpt), **re-semé au chargement**
  depuis les `TerrainPatchForm` dont l'asset est dans l'espace save — une
  re-save re-porte les chunks d'avant (le plugin save est réécrit entier).
- **Rochers déplacés** : composant réfléchi `world::Displaced { groundOffsetY }`
  posé à la saisie et tenu à jour après chaque tick physique ; `captureReference`
  écrit position (y = l'offset au sol, ce que le snap rajoute) et rotation.
  Au rechargement le rocher est un statique à sa nouvelle pose.
- Limites : les `.ter` d'un slot ne sont pas nettoyés quand un chunk redevient
  vierge (il reste dans la liste : delta nul) ; taille ~17 Ko/chunk (question
  §4.2 du plan, plafond/compression à décider quand ça pèsera).

### Retour dev E2 : « le sol qui monte nous traverse » → la poussée du sol (2026-09-27)
Pendant un stroke seul le visuel monte (la collision ne se reconstruit
qu'au relâché) : la capsule restait sur l'ancien sol, dans la terre. Deux
premières versions par frame (pénétration, puis montée au point mémorisé)
ont été jetées : la bosse — appliquée au point sûr, collision reconstruite,
un tick physique passé — n'était jamais vue, et un cas particulier pour elle
aurait cassé la cohérence (retour dev : « un système cohérent et
améliorable, pas un truc spécifique pour la bosse »). La version retenue :
**`throwActorsOnGroundRise` dans l'unique entonnoir `republishTerrain`** —
tout changement de sol (bosse, pinceau, mur, creuser, preview ET outil de
sculpt) y échantillonne le sol sous chaque acteur des chunks modifiés avant
le swap de l'overlay, swappe, ré-échantillonne : la montée passe par la
courbe de l'esprit Terre, `v = liftQuadratic × montée²` bornée à
[`liftMin`, `liftMax`] (SpiritForm Terre : 2,0 / 1,5 / 25 m/s). Le joueur
est sorti du sol (`CharacterBody::setPosition`, nouveau) puis `jump(v)` ;
les PNJ prennent `airVelocity`. Un stroke de pinceau (quelques cm par
publish à 20 Hz) fait sautiller au minimum, une bosse de 3 m lance à 18 m/s,
un mur est plafonné. Indépendant de l'ordre dans la frame et de la
collision ; améliorable au seul endroit qui compte.

### E3.a — le noyau feu, headless (2026-09-27)
`engine/terrain/FireField` (à côté de WaterSim, même fenêtre 2 m
`GridSpec`) : par cellule chaleur, combustible restant / initial,
inflammabilité, moiteur, état {dormant, brûle, brûlé, mouillé} ;
**combustible paresseux** (`FuelFn` appelée la première fois qu'une cellule
compte — une fenêtre ne coûte rien tant que le feu ne la touche pas).
`fireStep` en trois passes déterministes : (1) l'eau d'abord (`WetFn` :
une cellule sous l'eau perd sa chaleur, une brûlante est éteinte, un
mouillé redevient dormant quand l'eau part — le shifumi Eau > Flamme),
(2) chaque cellule brûlante déverse `spreadRate × ((1 − 0,75·w) +
0,75·w·max(0, vent·dir)) × inflammabilité × (1 − moiteur) × dt` sur ses
8 voisines (w = force du vent 0..1 ; dans un scratch : l'ordre de visite
est sans effet) — le front sous le vent est un **cône** (les chaînes
diagonales l'élargissent), pas une ellipse : le test vérifie la portée
sous le vent (> 2× contre le vent), pas un rapport d'axes, (3) ignition à
`ignitionPoints` sous `spreadBudgetPerTick` (le surplus garde sa chaleur et
s'allume au tick suivant — jamais « toute la carte brûle », Far Cry 2),
brûler consomme `burnRate` × dt de combustible → brûlé, la chaleur d'une
cellule non alimentée décroît. `fireScorch` = masque u8 (brûlé = 255,
brûlant = part consommée), `fireBurningCenters` = les N plus riches en
combustible (le budget d'émetteurs, le « hair transplant »),
`fireScrollWindow` bit-exact. Tests : disque sans vent, ellipse sous le
vent (ratio > 1,5, plus loin sous le vent), bande de roche infranchissable
+ sol saturé qui ne prend jamais, mare qui éteint en un tick et garde
éteint puis rend dormant en séchant, budget d'ignition plafonné, brûlé de
part en part + scorch + deux runs bit-exacts + scroll.

### E3.b — la voie job du feu, le masque de scorch, l'étincelle (2026-09-27)
**Headless (`world/spirit/SpiritFire`)** : `FireWindow` (fenêtre 512 m @
2 m centrée caméra, comme la sim d'eau ; scroll par cellules entières dès
que la caméra s'écarte de 64 m du centre — la portée d'un cast (180 m)
reste couverte), `groundPropsFrom(rules)` + `fuelFromWeights` (le
combustible d'une cellule = la moyenne pondérée des `SurfaceMaterialForm`
des cinq classes de splat herbe/roche/falaise/neige/sable, la moiteur
relevée par la `wetness` bakée de la région), et `runFireJob` — LE corps
du job : init ou scroll de la grille, étincelles, N pas, `fireScorch`,
`fireBurningCenters`, `active` = quelque chose brûle encore. Tests
`SpiritFireTest` (fenêtre + scroll, mélange, job de bout en bout + scroll
qui garde la cellule brûlante + fenêtre inactive).
**Le directeur (`SpiritDirector::updateFire`)** : la deuxième voie « un
job en vol », distincte de l'eau, même discipline Phase 5 — la grille est
DÉPLACÉE dans le job et revient avec le résultat ; le worker reçoit des
copies : un `TerrainParams` partagé par les deux échantillonneurs, les
props des matériaux, le snapshot d'eau (`sptr` immuable) + les corps
bakés, `seaLevel`. `FuelFn` = `height` + `normal` + `regionFieldsAt` +
`materialWeightsAt` → `fuelFromWeights` ; `WetFn` = `waterSurfaceQuery`
(sim si affichée, sinon bakée) > 3 cm au-dessus du sol. Cadence : le feu
doit 0,1 s de sim par pas, jusqu'à 5 pas par job ; **la voie est oisive
(aucun job) tant que rien ne brûle et qu'aucune étincelle n'attend**, et
le redevient quand le dernier foyer s'éteint (le scorch reste). Époque
incrémentée au changement de carte : un job en vol atterrit périmé et
tombe. Réglages du `SpiritForm` Feu : `spreadRate`, `ignitionPoints`,
`spreadBudgetPerTick`, `decayPerSecond` (→ `heatDecay`), et le nouveau
champ `fieldParticles` (le ParticleForm d'une cellule ACTIVE).
**Le rendu (`engine/render/landscape/FireScorchMap`)** : l'idiome de la
pool map — une texture R16F n×n par job atterri (1×1 à zéro sinon),
bind group au **slot 8** (le replay Vulkan passe de 8 à 12 slots ; GL
ignore l'index), unité 10 (`firescorch.glsl`, libre dans les includes de
terrain.frag et grass.frag), `uFireScorchInfo` ajouté EN FIN de
`FrameUniforms` (2160 → 2176, static_asserts + miroir common.glsl) :
{origine, 1/texel, n}. terrain.frag charbonne l'albédo (mix 0,92 vers
un brun-noir) avant l'éclairage ; grass.frag brunit les brins puis les
`discard` au-delà de 0,55 de scorch — l'herbe disparaît derrière le
front. Purge des .obj hors `_deps` + rebuild des deux configs (le type
partagé a changé de taille — leçon Phase 5).
**La scène** : `updateSpiritFire` après le tick des sources (secondes de
sim) → upload du masque quand un job atterrit, puis les **flammes** :
parmi les cellules brûlantes, les 24 plus proches de la caméra à moins
de 160 m gardent/reçoivent un émetteur `FlameTongue` (durée infinie,
coupé quand la cellule s'éteint ou sort du budget) — le « hair
transplant » de Far Cry 2. `resetSpiritFire` au changement de carte et
au `spirit clear` de la console.
**Le sort** : `SpellFireIgnite` (Créer × Feu, `point`, portée 180 m,
`intensity` 2 = la chaleur déversée, disque 2,5 m, essence −8, cooldown
2 s) → `PendingSpiritAction::Mode::FireIgnite` → `SpiritDirector::ignite`
+ cue `Cue.Spirit.Fire.Spawn` (gerbe d'étincelles additive). Matrice :
Feu → Créer × point. La pré-vérification « sol mouillé » d'un Créer
refuse déjà l'étincelle sur l'eau. Données : herbe `fuel` 0,4 → 8 s de
flammes par cellule (le noyau brûle 1 fuel/s) ; `spreadRate` 1 ×
(1 − moiteur 0,2) = 0,8 chaleur/s → une voisine prend en ~1,25 s : un
front de ~1,6 m/s sur l'herbe, lisible.
**Restes** : le vent du champ (`FireParams.wind`) attend E4 ; la pluie
n'humidifie pas encore le combustible (seule la `wetness` bakée) ;
persistance du scorch = E3.d optionnelle. E3.c (suite) : contact →
`buildupType = "ignition"` (Status.Ignited) joueur/PNJ, props en bois
(`surfaceMaterial` → `disableReference`), scatter gaté par le masque.

### Demandes dev après E3.b (2026-09-27) — à planifier
- **Le rendu du feu et son éclairage** : « il faudrait travailler sur la
  représentation du feu, un très beau rendu plus l'éclairage qui va
  avec ». Recherche lancée (techniques stylisées : quads de flamme à bruit
  + rampe + érosion alpha, flipbooks, liseré de braises entre brûlé et
  intact, agrégation des cellules brûlantes en quelques lumières
  clusterisées avec scintillement, distorsion de chaleur, fumée) → rapport
  dans `docs/FIRE-RENDER.md`, briques à découper avec le dev (validation
  visuelle, bascule A/B).
- **Les arbres et les éléments** : aujourd'hui les arbres sont totalement
  épargnés — les sphères d'un jet et une crue traversent les troncs (pas de
  collision eau/arbre), et le feu les ignore (le scatter n'est pas une
  entité). Cible : (a) l'eau rencontre les troncs (les sphères de jet
  éclaboussent sur un tronc, la sim voit les gros troncs comme des
  obstacles — Kellomäki, les corps bloquent l'eau) ; (b) **un arbre brûle
  quand assez de cellules brûlantes l'entourent** : chaleur intégrée sur le
  tronc depuis les cellules voisines (le `fuel`/`flammability` d'un
  `SurfaceMaterialForm` bois), puis flammes sur le tronc et la canopée,
  arbre charbonné/retiré au re-scatter (gaté par le masque, persistant via
  la couverture E3.d). S'inscrit dans E3.c avec les props en bois.

### F1 — le lit de braises + les étincelles (2026-09-28, NON COMMITÉE : validation visuelle dev)
Première version = un liseré émissif sur le front érodé par un bruit de
valeur procédural (transposition des *burn shaders* d'objets DeepSpaceBanana /
Zhu). Retour dev : bande invisible, bruit « très géométrique, pas beau du
tout », il attendait « un système de braises visibles ». Choix dev : **lit
de braises + particules**. La v2 :
**Noyau** : `FireGrid.ember` — une cellule qui finit de brûler part à 1 et
refroidit sur `FireParams.emberSeconds` (`SpiritForm.emberSeconds`, 45 s) ;
l'eau l'éteint aussi. `fireGlow` = flammes (0,55 + 0,45 × combustible
restant) → braises (0,55 × ember) → chauffe avant ignition (0,5 ×
chaleur/ignition) ; canal G du masque RGBA8 de `FireScorchMap`.
**Shader** (`firescorch.glsl`, terrain.frag, grass.vert/frag) : le volume
Perlin-Worley tileable du moteur (NoiseVolume, nouveau groupe de
binding 12 lié au slot 9 des passes terrain ; repli sur un hash sans
compute) — canal r (lisse) pour éroder le front (`charred =
smoothstep(0,42 ; 0,56)`, plus de bande), canal b (Worley haute
fréquence) pour découper le charbon en **corps de charbons** (`body`)
séparés de **fissures** (`crack`) ; `fireCharAlbedo` : charbon sombre, les
corps virent à la cendre grise à mesure que la braise meurt (`ash × body ×
(1 − glow)`) ; `fireEmber` : la lumière des fissures = rampe froid→chaud
× intensité × glow² × (fissures + plein feu sur les cellules fraîches) ×
respiration à phase par charbon (`sin(2,5t + 25·r + 9·g)`). L'herbe garde
le rabattement, le charbon et le cull, reçoit 0,4 × la lumière du lit.
**Particules** : `SpiritForm.emberParticles` → `EmberSparks` (additif,
0,06 m, 7/s, montée 1,6 m/s avec poussée, 2,2 s ± 0,8) spawné à côté de
chaque émetteur de flamme (même budget de 24 cellules, `FlameEmitter.sparks`).
**Réglages** (`FireLook`, panneau « Fire ») : intensité du lit (0 = A/B
simple scorch), pouls, érosion, échelle des charbons (m / tuile, charbon ≈
1/16), cendre, couleurs chaud/froid/charbon, seuil de cull. UBO : les 4
lanes `uFireEmber*` réinterprétées (w : volume prêt, échelle, cendre).
**À valider par le dev** : le lit de braises de jour et de nuit, la taille
des charbons, le refroidissement en cendre, les étincelles.

### Retour dev F1 v2 + F2 — les flammes (2026-09-28, NON COMMITÉES : validation visuelle dev)
Retour dev : « l'effet braises est intéressant seulement sur le premier
mètre du front, revenir ensuite à la couleur du sol brûlé ; le front
n'affiche pas de vraies flammes, il en faut aussi pour les torches et les
feux de camp ». Deux réponses :
**F1 confinée** : `fireFront` borne la braise à une bande `1 −
smoothstep(0,58 ; 0,78)` du scorch érodé — le premier mètre derrière les
flammes ; au-delà, charbon uni (les charbons/cendre ne se dessinent que
là où la braise luit) ; `emberSeconds` 45 → 8 s en data.
**F2 — les flammes** (`docs/FIRE-RENDER.md` §3, générique : tout
`ParticleForm` avec `blend = "flame"` — le front, une torche, un feu de
camp, une cue) : `EmitterParams/Particle.flame` + `seed` par particule,
`ParticleSim::forEachRaw`, `FxInstance` passe à trois vec4 (position +
taille ; couleur cœur + âge ; couleur bord + graine — les sprites ordinaires
ignorent la troisième), `RenderSnapshot.fxFlames` trié loin→près,
`FxRenderer` gagne le pipeline `fxflame` (alpha, test de profondeur sans
écriture) dessiné entre l'alpha et l'additif. `fxflame.vert` : quad
DEBOUT ancré à la position de la particule, billboard cylindrique autour
de l'axe monde (une flamme ne se couche jamais), goutte effilée vers la
pointe, `size` = hauteur. `fxflame.frag` : deux octaves de bruit qui
montent (le volume Perlin-Worley à l'unité 12 quand il est cuit, sinon un
hash), distorsion croissant vers la pointe, silhouette `(1 − y)(1 −
across²)`, trois bandes posterisées à bord dur (bord = colorEnd, milieu,
cœur = colorStart × 2,5 HDR), l'âge ÉRODE la flamme au lieu de la fondre,
base fondue dans le sol. Nouvelle forme d'émission `disc` (disque
horizontal : les flammes naissent AU sol, pas dans une sphère) ;
`FlameTongue` réécrit (disc 0,9 m, 9/s, 1 s ± 0,35, hauteur 1,1 → 0,7 m,
montée lente). Bascule A/B : « Flames as plain sprites » (panneau
« Fire »). Manque encore (F2 suite) : le fondu de profondeur (soft
particles, la profondeur de scène n'est pas liée à la passe fx), le LOD
(3-5 langues par cellule près, 1 par 2×2 loin), et F3 les lumières —
une torche sans lumière n'est pas une torche.

### Retour dev F2 v1 → F2 v2, les flammes en flipbook (2026-09-28, NON COMMITÉE)
Retour dev sur la v1 procédurale : « franchement ce sont des sprites super
basiques, cherche des sprites de flammes ». Recherche → **Unity Labs, « Free
VFX image sequences and flipbooks » (2016), CC0** : de vraies simulations de
fluide rendues en planches 16×4 (64 images, 128×256 px l'image). Deux
planches importées en PNG sous `game/data/base/textures/fx/` (README avec la
licence) : `smallflame01` (flamme basse dense — le sol) et `flame02` (langue
avec traîne de fumée — torches, braseros), déclarées dans `[assets]` de
spirits.toml (`…00d0`, `…00d1`). **Rendu** : `ParticleForm.texture` +
`flipbookColumns/Rows/Fps/Aspect` (repris par `EmitterParams`/`Particle`,
`RenderSnapshot::FlameSheet` = LA planche de la frame, une seule pour
l'instant) ; `FxRenderer` lie la planche (résolue par `view.materialTextures`,
le cache résident : placeholder tant que le PNG décode) au binding 3, slot 2
de la passe fx ; `fxflame.frag` joue le flipbook depuis une image de départ
tirée de la graine, à `fps`, **deux images fondues** (le mouvement vient de la
sim, pas besoin de vecteurs de mouvement), linéarise la planche sRGB, teinte
par `colorStart`, `flameBoost` HDR, `flamePosterize` optionnel (bandes de
luminance = le look stylisé) ; fondu d'entrée rapide, érosion sur le dernier
tiers de vie ; le quad debout prend l'aspect de l'image (`uFireFlameInfo`,
lanes UBO 2240 → 2272 avec `uFireFlameLook`). Sans texture, la langue
procédurale v1 reste. `FlameTongue` : 5 flammes/s de 1,6 → 1,2 m sur un
disque de 0,9 m, 1,6 s ± 0,5. **Reste** : plusieurs planches par frame
(torche + sol), soft depth, LOD, F3 lumières.

### Retour dev F2 v2 : « les flammes font un décalage sur la droite » (2026-09-28)
Mesuré sur la planche : le cœur de `SmallFlame01` balance de ±45 px (un
tiers de la case) sur un cycle de ~16 images — la sim d'origine ondule dans
le vent, ce qui se lit comme la flamme entière qui glisse. `Flame02` est
stable (±6 px). Correction hors ligne : la planche du sol est re-posée sur
des cases élargies 192×256 (aspect 0,75, `flipbookAspect`), chaque image
décalée pour que son cœur lumineux tombe sur l'axe de la case (rien n'est
coupé), l'alpha fondu sur 22 px aux bords de la case source (la sim
débordait de ses cases : sans le fondu, recentrer expose des coupes
franches ; une trace subtile reste sur 4-5 images). Repli si ça gêne :
`Flame02` au sol aussi (une ligne de données). Discipline rappelée par le
dev : suite complète UNIQUEMENT avant le commit, tests ciblés sinon.

### F2 v3 — la flamme stylisée BotW et la combo des styles (2026-09-28, NON COMMITÉE)
Retour dev sur le flipbook : « vraiment pas beau », « comment sont gérées
les flammes dans Breath of the Wild ? ». Documenté (reconstructions Silva /
80.lv, observations bgolus ; rien d'officiel) : un SHADER sur une forme
peinte — silhouette douce, bruit peint qui défile vers le haut et déplace
les UV, `smoothstep` à seuil = bord dur + 2-3 bandes plates, érosion par
l'âge (née pleine, consumée), quelques quads superposés dont des étirés,
bloom fort ; fumée et braises = sprites érodés non ronds. Prototype Python
validé sur image (`.claude/tmp/fire/botw_flame_proto2.png`) puis porté :
`fxflame.frag` réécrit en **quatre styles à switch plat**
(`FxRenderer::FlameStyle`, `uFireFlameLook.z`) : 0 sprite rond (pipeline
alpha), 1 langue procédurale, 2 flipbook (repli sur 1 sans planche), 3
**stylisée** — bruit du volume à deux octaves défilant, `xd = x + (n −
0,5)·0,9·y²`, largeur `(1 − 0,85·y^1,6)`, silhouette `(1 − across²)·(1 −
y)^0,6·√(8y)`, champ `v = shape − n·(0,35 + 0,5y) − max(age − 0,5, 0) −
grow·0,4`, bandes à 0,16 / 0,34 (bord = `colorEnd`, milieu, cœur =
`colorStart` × boost). Vertex : un tiers des flammes (graine < 0,35) en
variante étirée (×1,35 de haut, ×0,26 de large) — jamais une rangée de
découpes identiques. Panneau « Fire » : **combo « Flame style »** à la place
de la case A/B (demande dev), défaut = stylisée. Reste : la fumée et les
braises non rondes, le fondu de profondeur, le LOD, F3.

### Retours dev F2 v3 (2026-09-28) : violet, braises sous les flammes
- « Les couleurs stylisées sont violettes » : la durée de vie était rangée
  dans le canal bleu de la couleur de bord (`extra.b` = 1,6). `FxInstance`
  passe à QUATRE vec4 (`life.x` = durée de vie ; SSBO à stride 4).
- « La partie braises du sol doit être SOUS le front de flammes » : le lit
  de braises était gaté par « sol déjà charbonné », donc absent sous les
  flammes. `fireBurn(charred, glow) = max(charred, smoothstep(0,15 ; 0,6 ;
  glow))` : le sol noircit et luit dès que sa cellule brûle (la lueur =
  l'intensité des flammes), puis reste charbon uni derrière (les braises
  refroidissent en 8 s) ; l'herbe se rabat et se cull sur `fireBurn` aussi.
- « Le front de braises reste allumé trop longtemps, 1-2 m comme les
  flammes » : la lueur d'une cellule brûlante ne suit plus son combustible
  restant (8 s d'herbe = 13 m de bande) mais le temps écoulé depuis son
  ignition : `glow = 1 − brûlé/emberSeconds` (`fireGlow(grid, params)`),
  et `fireBurningCenters(…, burnRate, maxBurnedSeconds = emberSeconds)`
  ne rend que les cellules fraîches — les flammes et le lit de braises
  couvrent la même bande. `emberSeconds` = LA largeur du front en temps :
  1,2 s en data ≈ 2 m à 1,6 m/s ; derrière, charbon uni pendant que la
  cellule finit de brûler sans lumière.
- « Une ligne de braises arrive jusqu'au front quelques secondes plus
  tard » : reste du lit de braises — à l'extinction d'une cellule (8 s
  après ignition) `ember` repartait à 1, une seconde ligne s'allumait sur
  le bord arrière de la bande. Continuité : `ember` reprend la lueur là
  où elle en était (`1 − fuel0/burnRate/emberSeconds`, donc 0 pour de
  l'herbe). Largeur du front doublée sur demande : `emberSeconds` 2,4 s.
- Décision dev (2026-09-28) : **le flipbook seul**, les trois autres
  styles retirés (`FlameStyle`, la combo, la langue procédurale et la
  stylisée BotW — la recette reste documentée dans FIRE-RENDER §3 et le
  prototype Python) ; une flamme sans planche se dessine en sprite rond.
  HDR des flammes à 3. « Une fois la braise disparue le terrain doit être
  brûlé, pas revenir vert » : le scorch d'une cellule brûlante suivait sa
  part de combustible consommée (0,3 après le passage du front, sous le
  seuil de 0,42) — il suit maintenant le passage du front
  (`fireScorch(grid, params)` : `brûlé/emberSeconds`, 1 dès que le front
  est passé). Bande de braises +1 m : `emberSeconds` 3,0 s.
- Retour dev (2026-09-28) : « flammes en retrait du front, pas sur tout
  le front, un son 3D près du front ». (1) `fireBurningCenters` inclut les
  cellules dormantes chauffées à plus de la moitié de leur ignition (une
  flamme avant même qu'elles prennent : le front visible mène) et
  `FlameTongue` naît avec `burst = 2` (des flammes dès la pose de
  l'émetteur, sans attendre le premier tick de débit). (2) Budget : 96
  émetteurs à moins de 120 m (au lieu de 24 / 160), `maxCenters` 1024. (3)
  `SpiritForm.fieldSound` → `SoundForm` « SpiritFireCrackle » (bus
  ambient, 3D, 4-45 m, boucle) sur `sounds/fire/fire-loop.wav` (PagDev,
  OpenGameArt, CC0, remixé mono 12 s bouclable) ; UNE source qui suit la
  cellule brûlante la plus proche de la caméra (`AudioSystem::setPosition`,
  nouveau), coupée au-delà de 60 m ou quand le feu meurt.
- Idée dev (2026-09-28) : « faire apparaître les sprites relativement au
  centre de la caméra pour les économiser — coûteux ? » Non : la sélection
  est CPU à 10 Hz sur ≤ 1024 candidats ; un produit scalaire par cellule.
  Fait : les cellules derrière la caméra (facing < −0,2) ne reçoivent pas
  d'émetteur au-delà de 12 m, et le score de tri = distance × (1,6 −
  0,6·facing) met les cellules dans l'axe de vue devant à distance égale.
  Le coût GPU (overdraw des quads visibles) ne change pas, le budget
  d'émetteurs sert là où on regarde.

### F1 + F2 COMMITÉES (`43bd2d6`, 2026-09-28) après validation dev ; F3 — les lumières du feu (NON COMMITÉE)
`LandscapeScene::extractFireLights`, appelée juste après `extractLights` à
l'extract : les cellules du front (`fireBurning`, pré-ignition comprise)
agrégées par **tuiles de 8 m** → une `SceneLight` au barycentre de chaque
tuile (y = sol + 0,8 m), `intensité = min(lightIntensity × cellules,
lightMaxIntensity)`, `rayon = lightRadius + 2·√cellules`, `flicker` (le
scintillement CPU existant, deux sinus déphasés par index), couleur
`lightColor` ; les `lightCount` tuiles les plus proches de la caméra sont
INSÉRÉES EN TÊTE de `snapshot.lights` (le feu est la chose la plus
lumineuse alentour ; la queue du budget de 64 tombe). Le chemin clusterisé
les éclaire comme toute lumière locale ; les cascades de radiance prennent
les 24 plus proches → le rebond orangé sur la prairie et les personnages
vient gratuitement. Réglages `FireLook` (panneau « Fire », section
Lights) : intensité par cellule (0 = pas de lumière), plafond par tuile,
rayon de base, scintillement, nombre, couleur. Pas de lumière par flamme
héros ni de torche : les torches viendront avec leurs props (flamme
`flame02` + `LightSource`). À valider : nuit sur le front, le rebond GI,
le nombre de lumières au F6.

### F3 COMMITÉE (`c8fa108`) ; E3.c — le feu devient dangereux (2026-09-28, NON COMMITÉE)
**Contact → statut** : `SpiritForm.contactEffect` (Feu : `SpiritFireContact`,
EffectForm `buildupType = "ignition"`, 30 points) appliqué tous les
`contactPeriod` (0,5 s) au joueur et aux PNJ dont les pieds sont sur une
cellule dont la braise ≥ 0,25 (`SpiritDirector::fireGlowAt`, lu dans le
masque de la dernière job — jamais dans la grille, qui vit dans le job) ;
`applyEffect(…, &StatusBuildup)` route vers `tryAddBuildup` : Status.Ignited
après ~2 s dans les flammes contre une endurance de 100, la DoT, les
résistances d'armure et la règle « pas de ré-acquisition » viennent du
buildup existant (§2.9 : l'UNIQUE voie vers les attributs). Horloges par
acteur (`playerFireClock`, `npcFireClocks`).
**Props en bois** : `village.toml` en dépendance ; `Crate`, `Wagon`,
`WoodenFence` patchés `surfaceMaterial = "wood"` (bois `fuel` 12 s). À
chaque job atterrie (10 Hz) les props statiques en bois posés sur une
cellule qui luit chauffent (`kPropHeatRate` 0,6/s × braise) ; à 1 ils
prennent (`BurningProp` : flammes + étincelles pendant `fuel`, cue), ils
enflamment le sol autour d'eux une fois (`ignite` 2 m / 1,5 — le feu saute
d'une palissade à l'herbe), puis : collider retiré, référence désactivée
dans la couche de save (`disableReference`, comme un objet ramassé — absent
après save/load), entité détruite.
**Le scatter ne repousse pas** : `FireBurntMask` (spec + scorch) publié
par le directeur à chaque job, porté par `TerrainParams.burnt` (sptr : les
workers du scatter gardent leur copie, comme `patches`) ; `scatterProps`
refuse arbres, troncs, plantes et buissons sur une cellule brûlée (les
cailloux restent) ; les chunks nouvellement brûlés sont poussés dans
`sculptScatterQueue` → l'herbe et la végétation se ré-instancient. Test :
un masque brûlé sur tout un chunk → 0 arbre, 0 buisson, autant de rochers.
Limite connue : la couverture est transitoire (fenêtre 512 m) — hors
fenêtre ou après un scroll, un re-scatter fait revenir les arbres (E3.d
persisterait le masque).
**Reste** : les arbres du scatter ne brûlent pas encore (ils ne sont pas
des entités : il faudra intégrer la chaleur au tronc depuis les cellules
voisines et une flamme sur la canopée — demande dev notée plus haut) ;
les PNJ en feu ne fuient pas (IA).

### Retour dev E3.c (2026-09-28) : « ça ne va pas — les plantes repoussent, un arbre ne disparaît pas comme ça »
RÈGLE DE DESIGN (dev) : **l'état brûlé est transitoire** — l'herbe
correctement irriguée revient au bout d'un moment ; **un arbre ne disparaît
jamais par un masque** ; un arbre ne brûlera que par un mécanisme
progressif dédié (chantier à part), et au pire il perd son feuillage
(absent comme en hiver). Fait : le gate du scatter retiré entièrement
(`TerrainParams.burnt`, `FireBurntMask`, la file de re-scatter, le test).
À la place, la **repousse dans le noyau** : `FireGrid.regrow` ; une cellule
brûlée regagne `dt × (1 + 3 × moiteur) / regrowSeconds` (`SpiritForm.
regrowSeconds`, 360 s en data après retour dev « double » : 6 min sur sol sec, ~1,5 min en marais) ; à 1
elle redevient dormante avec son combustible restauré (elle peut brûler à
nouveau) et le scorch s'efface avec elle (`1 − regrow` : le charbon refond
au vert, l'herbe revient par le même masque qui l'avait culée). Test :
deux cellules sèche/marais, la marais revient 3× plus vite, le sec refond
puis rebrûle. Les props en bois et le contact restent.

### E3.c COMMITÉE (`2ada6f2`, repousse doublée à 360 s) ; E3.d — les sorts du feu (2026-09-28, NON COMMITÉE)
Demande dev : les sorts proposés + « un Globe anti-feu maintenu qui éteint
autour du personnage, lui-même insensible à la brûlure ». Noyau :
`fireDouse(grid, x, z, r)` — les cellules brûlantes redeviennent dormantes
EN GARDANT leur combustible (elles reprennent si le feu revient), chaleur
et braises à zéro, le brûlé reste brûlé ; `FireJobInput.douses` appliquées
avant les étincelles ; `SpiritDirector::douse` (une fois) et `setWard`
(une extinction répétée par CHAQUE job tant qu'elle est posée). Matrice
Feu : Créer × point ; **Détruire × point** (`SpellFireDouse`, disque
5 m : extinction, cue `Cue.Spirit.Fire.Douse` = bouffée grise) ;
**Détruire × self maintenu** (`SpellFireWard`, le globe : `setWard` autour
du corps du joueur à chaque frame, rayon 6 m, voile de particules
`FireWardVeil` qui suit le joueur, `applyFireContact` ignore le joueur tant
que le globe tient, upkeep ÷4 chaque seconde) ; **Comprendre × point**
(`SpellFireUnderstand`, maintenu, `castFireReading` : état de la cellule
depuis les masques — brûle/braises %, brûlé/repoussé %, froid —,
combustible/inflammabilité/moiteur par le même mélange que le noyau,
front de feu le plus proche en m + point cardinal, « air immobile » en
attendant E4) ; **Contrôler × point maintenu** (`SpellFireBrand`, le
brandon : une flamme portée à la visée qui allume 1 m autour toutes les
0,25 s, jamais sur l'eau, upkeep ÷4 toutes les 0,5 s). Loc en/fr
`spell.fire*`, `fire.*`. Tests : douse (éteint, garde le combustible,
reprend), matrice Feu. À valider en jeu : les quatre sorts à la molette.
- Retour dev (2026-09-28) : « le personnage ne prend pas de dégâts dans le
  feu ». Deux causes : le contact lisait la lueur des braises (nulle au-delà
  de la bande de 3 s du front — au milieu d'une zone qui brûle, rien) et le
  seul effet était le buildup, dont la DoT Ignited vaut 0,2 %/s. Fait :
  `FireJobOutput.state` voyage avec les masques, `fireBurningAt` = l'état
  BRÛLE de la cellule (toute la durée du combustible) pour le contact ET
  les props ; `SpiritForm.contactDamage` (Feu : 6 par contact, ~12/s) =
  dégâts typés Feu par `applyDamage` (StatBlock, résistances, l'écriture
  terminale sanctionnée §2.9), en plus du buildup.
- Retour dev : « SpellFireWard: range must be positive » — `compileSpell`
  exige une portée > 0 même pour un sort sur soi ; le globe avait `range =
  0` et ne compilait donc jamais. Portée mise à 6 (ignorée pour `self`).

### E3.d COMMITÉE (`5a3612b`) ; E3.e — les arbres qui brûlent, progressivement (2026-09-28, NON COMMITÉE)
Règle dev : jamais de disparition ; au pire le feuillage absent comme en
hiver, et seulement après avoir brûlé progressivement. **Headless**
(`world/spirit/SpiritFire` : `TreeFire`, `advanceTreeFire`, `TreeFireParams`
= `SpiritForm.treeIgnitionSeconds` 6 / `treeBurnSeconds` 25 /
`treeRegrowSeconds` 900) : Cold — la chaleur monte avec l'exposition (part
des 3×3 cellules autour du tronc qui brûlent) et retombe seule ; ≥ 1 →
Burning — `burn` (feuillage parti, 0..1) monte linéairement sur
`burnSeconds` ; → Burnt (`burn` = 1) : nu, ne reprend pas tant qu'il n'a pas
regagné un quart de sa repousse ; → Cold, `burn` redescend sur
`regrowSeconds` (le feuillage revient). Test : demi-exposé prend en 8 s,
canopée à 50 % à mi-combustion, nu, repousse, refroidissement d'un arbre
laissé seul. **Scène** (`updateSpiritFireTrees`, par job atterrie, avec le
temps de sim écoulé depuis la précédente) : les arbres du scatter à moins
de 160 m viennent de la copie CPU de la végétation (`WorldRenderer::
collectProps`, `GiProp.kind == 0`), un état par arbre clé par sa base
quantifiée au quart de mètre ; à la prise : flammes et étincelles au tronc
(taille × échelle) pendant `burnSeconds`, cue ; un arbre qui brûle sème
des braises autour de lui toutes les 3 s (`ignite` 3 m × échelle) ; les
états oisifs sont effacés. **Rendu** : le masque de feu gagne un canal B =
canopée brûlée à la cellule de l'arbre (`fireCanopy`, composé par la scène,
`FireScorchMap::upload` à trois canaux) ; `firemask.glsl` (sampler 10 +
`fireMaskAt` vec3) séparé de `firescorch.glsl` (qui garde le bruit à
l'unité 12, occupée par les normales des props dans le pipeline d'arbres) ;
`tree.vert` : `fall = max(chute hivernale, canopée brûlée)` — la MÊME
règle de chute par carte que l'hiver, progressive et réversible — et
`vBurn` vers `tree.frag` qui charbonne feuilles restantes et bois. Limite :
le caster d'ombre garde les feuilles (shadow_prop.vert sans le masque) ;
l'état des arbres est transitoire comme le champ (fenêtre 512 m).

### E3.f — le jet de flammes (demande dev 2026-09-28, NON COMMITÉ)
« Comme le sort qui jette de l'eau, un sort qui jette une traînée de feu
devant nous, brûlant l'ennemi ou l'objet sur une portée limitée ; ça peut
servir à mettre le feu à un arbre. » Créer × Feu × `stream` maintenu
(`SpellFireStream`, portée 10 m, chaleur 1,2, rayon 1,2 m, upkeep ÷4 toutes
les 0,5 s) : contrairement au jet d'eau balistique, un cône droit et court
(`kFlameJetHalfAngle` 0,28 rad, flammes flipbook `FlameJet` émises de la
main à 14 m/s, durée de vie = portée/vitesse, steer suit la visée) ; toutes
les 0,25 s le sol sous le cône prend tous les 1,5 m jusqu'à la portée ou
jusqu'au premier versant touché (jamais sur l'eau) — les props et les
arbres prennent par le champ ; les PNJ dans le cône reçoivent le contact
direct (`fireTouch`, désormais un membre partagé avec le contact du champ :
buildup + dégâts typés). Matrice Feu : Créer × stream (maintenu).
- Retour dev E3.e : « les feuilles ont pris feu mais n'ont pas disparu ».
  Le journal montre le CPU correct (burn 1,00 → masque 255) : la faute était
  au **fetch à lod implicite dans un vertex shader** (`texture()` sans
  dérivées : Vulkan rend zéro) — `fireMaskAt` et `fireNoiseAt` passent en
  `textureLod(…, 0)` (ils servent tree.vert et grass.vert). Leçon : tout
  échantillonnage partagé vertex/fragment se fait en lod explicite.
- Retour dev : « les feuilles grisent mais ne tombent pas ». Le canal
  était bon mais DILUÉ : la valeur est estampée sur UN texel (la cellule de
  la base) et lue en bilinéaire à une base décentrée → 25-60 % de la valeur.
  `fireCanopyAt` (firemask.glsl) lit le canal B par `texelFetch` sur la
  cellule même (le `lround` du C++ et le `floor(+0,5)` du shader
  coïncident) ; tree.vert l'utilise pour la chute.
- Validé dev (chute des feuilles OK) ; traces retirées. Retouches
  demandées : les flammes AUSSI dans la CANOPÉE (deuxième émetteur, sphère de 40 % de la hauteur
  autour du centre du houppier à 70 % de la hauteur, hauteur ≈ 2,12 ×
  échelle d'après la silhouette moyenne du scatter), celles du tronc et des
  branches conservées ;
  les feuilles ROUSSISSENT comme à l'automne pendant les premiers 40 % de la
  combustion (teinte de saison du slot, persistants compris) avant de
  noircir et de tomber.

### E3.e/f COMMITÉES (`fc5220c`) ; E4.a — la direction du vent (2026-09-28, NON COMMITÉE)
Avant : une force (`windStrength`), une horloge accumulée, et un cap codé
en dur dans chaque shader (herbe 0,34 rad, arbres et caster (0,9 ; 0,35),
nuages et brume (1 ; 0,35), pluie fixe). Fait : `WeatherForm.
windDirectionDeg` (degrés boussole vers lesquels le vent SOUFFLE : 0 =
est, 90 = nord, sens trigonométrique vu de dessus — la boussole des
intelligos) → `AtmosphereParams.windDirectionDeg`, crossfade de météo sur
l'ARC COURT (350 → 10 = +20, jamais −340 par le sud) ; lane UBO
`windDirInfo` ajoutée EN FIN (2272 → 2288, assignée après le bloc
d'initialiseurs désignés — l'ordre de déclaration l'impose) ; les six
shaders lisent `uWindDirInfo.xy` (herbe : cap ± bruit, arbres et caster
d'ombre, nuages 2D et volumétriques, brume, pluie penchée). Sept météos
autorées (Clear 20°, Morning Mist 60°, Hazy 35°, Cloudy 300°, Overcast
280°, Storm 250°, Rain 240°). `engine/terrain/WindField` headless : vent
global + rafales analytiques (`windAt`, `tick`, `windDirectionFromDegrees`),
testé. Le feu propage sous le vent (`FireParams.wind` = direction ×
force × 0,6) ; les particules dérivent (`ParticleForm.windDrag` : braises
1,5, flammes 0,35, bouffée 1 ; `ParticleSim::setWind`, vent = 3 m/s à
force 1). E4.b (suite) : l'esprit Vent — rafales placées (`spirit.spawn
Wind`), sorts Créer × Vent (souffle en stream, rafale posée), poussée
cinématique des personnages et des floaters, fumée.

### E4.c — la fumée du feu (demande dev 2026-09-29, « assez discrète »)
`SpiritForm.smokeParticles` → `FireSmoke` (alpha, gris doux à 22 %,
2,5 bouffées/s, 4,5 s ± 1,5, montée 0,9 m/s, 0,9 → 3,2 m, `windDrag`
1,2) : une cellule brûlante sur quatre (parité de cellule) porte un
émetteur de fumée à 1,2 m au-dessus des flammes ; les props en bois et
les arbres qui brûlent en ont un (×2 et ×1,5 au-dessus de la canopée pour
les arbres). Coupée avec les flammes.

### E4.a + E4.c COMMITÉES (`5594941`) ; E4.b — l'esprit Vent (2026-09-29)
Demande dev : les sorts prévus + « Calmer le vent » (le vent tombe tant que
le joueur se concentre) et « Diriger le vent » (Contrôler : il souffle vers
où le joueur regarde). **Le champ** (`LandscapeScene::windField`, un
`WindField` par frame) : global = direction × force de la météo ×
`kWindSpeedPerStrength` (5 m/s à force 1) + les rafales = les
`SpiritSource` de type Vent de la carte (posées par `SpellWindGust`,
persistées comme toute source, `dirX/dirZ` = le cap du joueur au cast,
`rate` = m/s au centre, `radius`, `remaining`) + la rafale portée du
Souffle. **Consommateurs** : le job du feu reçoit une copie du champ
(`FireJobInput.wind`, `fireStep` prend un `WindFn` par cellule brûlante,
|vent| / 10 m/s = force 0..1 — test : une rafale locale courbe le front) ;
les particules échantillonnent le champ par particule
(`ParticleSim::setWindSampler`) ; le corps du joueur est poussé par les
seules RAFALES (`CharacterBody::setExternalVelocity`, 60 % de leur
vitesse — jamais par le vent ambiant). **Les sorts** (matrice Vent) :
Créer × point = `SpellWindGust` (12 m/s, 8 m, 10 s) ; Créer × stream
maintenu = `SpellWindBlow` le Souffle (14 m/s, portée 20 m, rafale de
10 m centrée à mi-portée qui suit la visée, traînées `WindStreak`) ;
Détruire × self maintenu = `SpellWindCalm` (la force de la météo tombe à
zéro en 1 s, rendue au relâché) ; Contrôler × self maintenu =
`SpellWindDirect` (le cap de la météo = celui du joueur : `atan2(−fz,
fx)` dans la boussole des lectures). Les deux derniers reposent sur une
BASE (`windBase*`) capturée au cast et rafraîchie pendant un crossfade de
météo (qui réécrit l'atmosphère chaque frame avant les surcharges) ; sans
crossfade en cours, le contrôleur de météo ne réécrit pas l'atmosphère,
d'où la base explicite. La pré-vérification « sol mouillé » d'un Créer
épargne le Vent. Leçon : `WindField.cpp` doit vivre dans la lib CŒUR
(la sim l'utilise) — le verrou §2.10 `meadows-simlink` l'a attrapé dans
la lib de rendu. Reste : la poussée des PNJ et des floaters, un cue
sonore, le vent dans la lecture du feu (« air immobile » → direction).

### E4.b COMMITÉE (`aa23507`) ; E5 — la liste de fin de chantier (2026-09-29)
Demande dev : « fais toute la liste » — PNJ qui fuient le feu, torches et
feux de camp, fondu de profondeur et LOD des flammes, rampe de
température des lumières, pluie qui mouille le combustible, persistance
du feu, ombres des feuilles brûlées, poussée PNJ/floaters par le vent ;
un seul commit à la fin (« sinon tu vas devoir tout tester pour chaque
feature »). Quatre volets :

**A — le rendu du feu.** Ombres : `shadow_prop.vert` inclut
`firemask.glsl` et lit `fireCanopyAt` comme `tree.vert` (chute = max(hiver,
brûlure de canopée)) ; le caster déclare `uFireScorch` (unité 10) et les
cascades comme le caster de pluie lient le slot 8 avant `drawDepth` (le
replay Vulkan d'une frame part vide : lier par passe). Rampe de
température : `extractFireLights` cumule `fireGlowAt` par tuile 8 m,
couleur = `mix(emberCold, lightColor, fraîcheur)` — une tuile de braises
mourantes éclaire rouge sombre, un front frais orange. Fondu de
profondeur : le `fxflame` reçoit le groupe de scène de l'eau (slot 3,
`sceneDepthCopy` en unité 1), `fxflame.frag` fond l'alpha sur 0,6 m avant
la surface derrière (reversed-Z : profondeur 0 = ciel = pas de fondu) —
la base d'une flamme qui coupe le sol n'a plus d'arête. LOD : au-delà de
`kFlameLodNear` 40 m, une flamme par bloc 2×2 de cellules (centre du
bloc, taille ×1,8, rayon ×2, cadence ×0,7) — le front lointain coûte un
quart d'émetteurs.

**B — la simulation.** Pluie : `FireParams.rain` (= `atmos.rainIntensity`
via `FireFrame.rain`) ; la propagation prend `max(moiteur, pluie)` ; à
partir de `rainDouseLevel` 0,5 une cellule brûlante compte la pluie dans
sa `heat` (libre pendant qu'elle brûle) et s'éteint après
`rainDouseSeconds` 6 s sous pluie pleine (brûlée, sans braise) ; une
bruine ne fait que ralentir le front (test noyau). Persistance :
`world::FireStateForm` (WorldForms, record de la couche save
`5a5e0000-…-00f1`, `worldspace` + `cells` texte « x z état fuel repousse
braise; » brûlantes d'abord, plafond 65 536) ; le job sort `out.cells`
(`fireCollectCells` : brûlantes + brûlées), `SpiritDirector::capture(ws)`
emballe le dernier atterri (`packFireCells`), `restoreFire` déballe dans
`fireRestores` que le job suivant écrit en premier (`fireRestoreCell` :
fuel échantillonné × fraction) — la voie se réveille pour ça. Restauré à
l'entrée de scène après le `WorldStateForm` (même worldspace ou sans).
La chaleur des voisins n'est pas sauvée : le front repart de ses cellules.

**C — le monde.** `StaticForm.light/lightOffset/flameParticles/
smokeParticles/particlesOffset` : le spawner attache un `LightSource`
(nouvel `offset` réfléchi, `SceneSubmit` le compose dans les deux
extractions) et un `world::FxSource` ; `LandscapeScene::updateFxSources`
tient un émetteur par entité à portée (`kFxSourceReach` 90 m), balaie les
disparues. Data (spirits.toml, qui dépend déjà du village) : `Campfire`
(lueur 4,5/12 m/scintillement 0,5, `CampfireFlame` disque 0,22 m,
`CampfireSmoke`) et `Torch` (3/8 m/0,4, `TorchFlame` 0,34 m à 1,62 m) ;
modèles générés en boîtes (`tools/scripts/gen_fire_props.py` →
`models/props/`), trois références sur la place du village (52 ; 373,5)
et (49 / 55 ; 370,5). Ni l'un ni l'autre n'allume le sol : le foyer reste
dans ses pierres (backlog : un crépitement 3D, une torche qui se prend).

**D — les acteurs.** `NpcContext.spirits/wind` ; `NpcMovement::
steerFromFire` (trois anneaux de 12 échantillons jusqu'à 6 m sur
`fireBurningAt`, répulsion 1/r, tout droit si encerclé, `steerBlocked`
puis ±70°, `moveNpcDirect` ×1,35) avant le planning (interruption
`fleeingFire` comme le combat, siège lâché) ; `pushNpcByWind` = la règle
du joueur (rafales seules, 60 %, seuil 0,5 m/s, `groundNpc`) ; les
floaters prennent 8 % du vent dans leur closure de courant
(`kFloaterWindDrift`). Le vent dans l'intelligo feu et un cue sonore de
rafale restent au backlog.

### E5 COMMITÉE (`9b373b2`) ; E6 — la garniture du feu et du vent (2026-09-29)
Demande dev : « crépitement 3D des feux de camp, torche qui peut allumer,
distorsion de chaleur, vent dans l'intelligo du feu, cue sonore de rafale »
(les trois derniers à tester par le dev ensuite).

- **Crépitement** : `StaticForm.sound` → `FxSource.sound` ; la scène joue
  la boucle 3D par source à portée (`soundResolver.resolve` + `play`,
  `stop` au balayage). Le feu de camp prend la boucle du champ
  (`SpiritFireCrackle`, d3), la torche un `TorchCrackle` (même asset,
  35 %, 14 m, pitch 1,15).
- **Torche qui allume** : `StaticForm.igniteRadius/igniteHeat` →
  `FxSource` → liste `fxIgniters` par frame (`applyFxIgniters`). Les props
  en bois dans le rayon chauffent (`propFireHeat`, `kPropHeatRate`) et
  prennent par `lightProp` (création du `BurningProp` extraite de
  `updateSpiritFireProps`) ; les arbres comptent une exposition 1,0
  (`nearFxIgniter` dans `updateSpiritFireTrees`) ; le sol seulement si la
  flamme est à moins d'1 m (`FxSource.offset.y`) — une torche sur son
  poteau épargne l'herbe, un foyer (`igniteRadius` 0) reste dans ses
  pierres. La voie feu s'endort sans feu : `SpiritDirector::wakeFire()`
  force un job (consommé) quand un prop vient de prendre, brûle, ou
  qu'un arbre est dans une flamme (vérifié à 1 Hz par `collectProps`).
  Data : Torch rayon 1 m, chaleur 1,5/s — les deux torches de la place
  sont à > 2,8 m de la clôture et de la caisse.
- **Distorsion de chaleur** : `ParticleForm.blend = "haze"` →
  `Particle.haze` → lot `fxHaze` du snapshot → pipeline `fxhaze`
  (fxhaze.vert = le billboard des particules ; fxhaze.frag = bruit de
  valeur défilant × masque doux × rampe alpha, lit `uSceneColor` unité 0
  du groupe de scène de l'eau au slot 3 — le même groupe que le fondu de
  profondeur — et écrit la scène réfractée en alpha). Amplitude =
  `FireLook.hazeStrength` (0,012 écran, panneau Fire « Heat haze »,
  lane `uFireFlameLook.z`). Émetteurs : `SpiritForm.hazeParticles`
  (`FireHaze`) sur les cellules à < 30 m, `StaticForm.hazeParticles` sur
  foyers/torches. Sans copie de scène (GL sans copies) : pas de haze.
- **Vent dans l'intelligo** : `castFireReading` lit `windField.windAt` à la
  visée : < 0,3 m/s « air immobile », sinon `fire.wind` (« Vent de {} m/s
  vers {} », boussole `compassCode` comme le front). Loc EN/FR
  régénérée par `cooker import-csv` (sans --patch).
- **Cue de rafale** : `Cue.Spirit.Wind.Spawn` était déjà émis au spawn
  d'une rafale posée ; il manquait sa `CueForm` — ajoutée avec un
  `SoundForm` `WindGustWhoosh` (3D, 45 m, jitter de pitch) sur un
  `gust.wav` généré (`tools/scripts/gen_gust_wav.py` : bruit filtré,
  passe-bas balayé) ; le Souffle l'émet aussi au démarrage, au nez.

### E6 COMMITÉE (`4567f91`) ; retour dev : « les sorts ne se lancent pas en story » (2026-09-30)
Cause : tout le chemin des esprits était verrouillé sur `sandboxActive`
(`castSpirit`, `aimGround`, `applyPendingSpiritActions`, le tick du
directeur + `updateSpiritFire`, le lâcher de l'emprise, la console
`spirit spawn`) — la règle `canCastAt` d'E1, quand seule l'Eau existait
et versait dans la sim d'eau du monde sandbox. La sim d'eau n'a en fait
aucune dépendance au sandbox (rien dans WaterSystem). Fait : la porte
devient PAR ÉLÉMENT — l'Eau (sort d'élément Eau, ou script contenant
`"Water"`) exige `waterSystem().simIsValid()` (toast `spirit.refused`
avant tout coût ; une action Eau queue-ée par script est ignorée sans
sim) ; Feu, Terre, Vent ne demandent que le sol (physique + terrain),
dans tous les mondes. Les voyages de carte restent sandbox.

### Crash au lancement du mode histoire (2026-10-01) — le driver Vulkan, pas le feu
Rapport dev : `VkResult -4` (DEVICE_LOST) en rafale puis 0xC0000005 à
l'entrée du mode histoire, juste après le bake de brume au village. Pour
reproduire sans la main du dev : override de boot
`MEADOWS_BOOT=story|sandbox` (le clic « Entrer dans le monde » scripté
au moment où le warmup révèle : `setSandboxMode` + `enterPlayMode`) et
`MEADOWS_BOOT_SECONDS=N` (quitte N s après). Pièges rencontrés : le jeu
démarre en sandbox et c'est le clic Story qui bascule et téléporte au
village ; le build DEBUG bake les 17 AO de végétation (≥ 2 min chacun
ici) faute de cache `data/cache/ao` à côté de l'exe — le dev lance la
RELEASE (101 entrées de cache) ; recopier le cache suffit.
Diagnostic : 1 run release sur 4 plante dans `nvoglv64.dll` à l'offset
`e100f4` — le MÊME que quatre dumps des 14/09, 15/09 et 28/09 (journal
d'événements Windows), donc antérieur à tout le chantier ESPRITS ; le
crash du dev (offset `fa1025`, device lost) en est une variante. La
couche de validation (build debug) le disait depuis le début : sur la
file d'upload (famille transfer-only) les transitions d'image d'upload
portaient des stages FRAGMENT/COMPUTE et les images, CONCURRENT
graphics+compute seulement, y étaient écrites sans transfert de
propriété ; sur la file compute, des barrières tampon avec des stages
VERTEX/FRAGMENT. Correctif backend : `clampStagesToQueue` (les stages
que la famille ne sait pas exécuter tombent, avec leurs bits d'accès ;
vide → ALL_COMMANDS), `transitionLayout(..., queueCaps)` partout,
`VulkanCommandBuffer::queueCaps()` (compute vs graphics), images
partagées avec la famille d'upload, et au teardown un reset des pools
avant les frees (un cb resté en enregistrement à la fermeture). Après :
validation propre en jeu (reste un avertissement d'attribut de vertex
non consommé et deux messages de teardown dans vksmoke, préexistants).
Réf. durable : docs/RENDERING.md §1.2 (file d'upload) et leçon 17.

### Crash à la fermeture (2026-10-01) — un free une frame trop tôt
Les deux messages de teardown restants (« vkDestroyBuffer : buffer in
use by VkCommandBuffer », « vkFreeCommandBuffers : in use ») et le
segfault de sortie de vksmoke / des runs debug (dans la couche de
validation elle-même) avaient une seule cause : `createTexture` avec
pixels soumet sa copie en asynchrone SANS fence (`immediateSubmit(wait
= false)`) et parque son staging et son cb sous `frameCounter` ; quand
l'appel tombe ENTRE deux frames (chargement de scène, atlas de police du
smoke), la fence de ce compteur est déjà soumise et ne couvre pas la
copie — libérée au cycle suivant pendant qu'elle s'exécute encore.
`Impl::asyncParkFrame()` parque une frame plus tard hors frame (dans une
frame, la fence de la frame courante suit la copie sur la même file).
Avec le reset des pools au teardown : vksmoke passe de 18 PASS +
segfault à 40 PASS et « Vulkan validation: clean run (0 message) ».

### Retours dev de la session de test des sortilèges (2026-10-05)
- **« Les arbres prennent feu immédiatement »** → +50 % : `treeIgnitionSeconds`
  6 → 9 s (plein feu autour), `kPropHeatRate` 0,6 → 0,4/s (une caisse en
  plein feu prend en 2,5 s au lieu de 1,7).
- **« Le bandit ne brûle pas sous le jet de flammes »** : le jet appelait
  `fireTouch` et accumulait l'horloge de contact du PNJ, puis
  `applyFireContact` (par job atterri) la remettait à zéro parce que le
  sol sous lui ne brûlait pas. Fait : le jet ne fait que marquer les
  acteurs dans son cône (`flameJetTouched`, par frame) et la passe de
  contact lit « sol brûlant OU dans le jet ».
- **« Les sorts d'eau ne répondent pas en story »** : la fenêtre de sim
  attendait une RÉGION bakée sous la caméra (`params.base->regionAt`) —
  la garde du streaming sandbox (« ne pas pré-rouler sur le terrain
  analytique ») ; le monde autoré n'a pas de régions, donc jamais de
  sim. Fait : la garde ne s'applique que si `params.sandbox` ; en story
  la fenêtre démarre partout, sur les lacs autorés épinglés, la pluie et
  les sources du joueur ; `setSandboxMode(false)` retire la closure des
  entrées du réseau maître (`setSimSources({})`), qui répondrait pour un
  autre monde. Vérifié en boot story : « Water sim: revealed » au village.
- **« Diriger le vent change brutalement, et revient brutalement »** :
  l'emprise de vent (`SpiritWindHold`) suit désormais sa cible par un
  lissage exponentiel (`kWindHoldEase` 0,33 s ≈ 95 % en une seconde) sur
  l'arc court ; au relâché elle passe en `releasing` et revient à la base
  par le même lissage avant de se terminer (un recast annule le retour).
  Calmer le vent remonte de la même façon (il descendait déjà en 1 s mais
  remontait d'un coup).
- **« La map de bruit du pliage de l'herbe snappe, elle devrait tiler »** :
  le `hash21` de grass.vert faisait `fract(p × 435)` sur des entrées qui
  grimpent dans les milliers (coordonnée monde × 0,25 + horloge de vent
  accumulée, ou × 137 pour la variété par brin) — en float il ne reste
  que quelques bits, la grille de bruit devient en escalier. Fait : hash
  de maille sur un tore de 256 cellules (`mod(p, 256)` puis un hash à
  petits coefficients) — précision pleine à toute phase, et le champ tile
  toutes les 256 mailles (1024 m à l'échelle des rafales). Même correctif
  dans fxhaze.frag, dont le défilement roule sur l'horloge de frame.

### Retours dev, deuxième tour (2026-10-05)
- **« L'eau ne repousse pas les ennemis sous le jet »** : les sphères du
  jet (`JetSphere`, lancées toutes les 0,12 s) traversent désormais les
  acteurs : à moins de rayon + 0,6 m de la poitrine, le PNJ gagne une
  poussée le long du vol (`Npc::shove`, 14 m/s² plafonnée à 3,5 m/s),
  appliquée au sol et amortie (⅔ perdus en 0,35 s) par
  `NpcMovement::applyNpcShove` dans le tick du directeur, à côté de la
  poussée des rafales. Le `shove` est le canal générique de toute poussée
  cinématique d'un PNJ (la version PNJ du `setExternalVelocity` du joueur).
- **« Diriger le vent téléporte les nuages »** : nuages 2D, volume du
  ciel et brume calculaient leur dérive comme direction × horloge de
  vent — tourner la direction faisait pivoter tout le vecteur, donc
  sauter le motif. Fait : `windDrift` (Vec2) intégré dans la scène avec
  le même poids que `windTime` (dt × force), transporté par
  `uWindDirInfo.zw` ; `clouds.glsl`, `skyclouds.frag`, `mist.frag` lisent
  la dérive accumulée — un vent qui tourne courbe la trajectoire. L'herbe
  et les arbres gardent la phase scalaire (leurs domaines de bruit sont
  isotropes) et la direction instantanée, lissée par le sort.
- **« L'herbe penche perpendiculairement à mon regard »** : `grass.vert`
  prenait la direction du vent comme AXE de rotation ; or le brin tourne
  autour de l'axe, donc penchait à 90°. L'axe est désormais la
  perpendiculaire (`(-sin, 0, cos)` du cap) : le haut du brin bascule vers
  le cap lui-même. Le défaut était invisible tant que le cap était une
  constante codée en dur (avant E4.a).

### Le feu lent au calme, poussé par le vent (demande dev 2026-10-05)
Question dev : « le Souffle a-t-il un impact sur les flammes ? » — oui
depuis E4.b (sa rafale entre dans le champ que le job échantillonne),
mais la règle E3.a ne faisait que FREINER hors vent (`(1 − 0,75·w) +
0,75·w·max(0, cos)` : plein régime au calme, 0,25× de côté et contre le
vent, jamais plus vite sous le vent). Nouvelle règle, en fractions de
`spreadRate` : au calme `calmSpread` (0,4) partout ; à vent plein
`windSpread` (1,6) droit sous le vent, `upwindSpread` (0,1) contre,
rampe entre les deux à partir d'un peu en arrière du travers
(`clamp((cos + 0,3) / 1,3)`), le tout mélangé depuis la valeur calme par
la force du vent. Le vent de météo à force 1 vaut un demi-vent plein
(5 m/s sur 10) ; le Souffle et une rafale posée valent un vent plein.
Data : `SpiritForm.fireCalmSpread / fireWindSpread / fireUpwindSpread`.
Test : au calme le front rampe (> 2 cellules en 4 s), sous le vent il
atteint ≥ 2× plus loin sous le vent et retient le côté contre le vent ;
l'ancien test de forme (ratio d'axes > 1,1) devient > 0,8 — le cône sous
le vent s'élargit en avançant, la boîte englobante reste carrée.

### « Le Souffle n'empêche pas le feu d'avancer vers moi » (2026-10-05)
Le champ est additif (météo 5 m/s + rafale 14 m/s : la rafale domine
localement, la question de l'override ne se pose pas). Le défaut était
dans le noyau : contre le vent une cellule reçoit encore 0,1 × spreadRate
par voisin brûlant, et la décroissance de chaleur ne s'appliquait qu'aux
cellules qui ne reçoivent RIEN — un filet s'accumulait donc sans fin et
finissait par allumer ; le front remontait le vent, lentement mais
sûrement. Fait : une cellule dormante refroidit À CHAQUE tick
(`heat = max(0, heat + dealt − heatDecay·dt)`) ; l'allumage demande un
FLUX au-dessus de la décroissance. L'inégalité qui façonne le feu est
dès lors écrite dans FireParams : `calmSpread × spreadRate > heatDecay`
(un voisin seul propage au calme) et `3 × upwindSpread × spreadRate <
heatDecay` (trois voisins ne remontent pas un vent plein) ; data du jeu :
spreadRate 1, calm 0,4, upwind 0,1, `decayPerSecond` 0,5 → 0,35. Test
avec les chiffres livrés : en 15 s sous un vent plein, pas une cellule
gagnée contre le vent, > 4 sous le vent ; au calme le disque rampe.
