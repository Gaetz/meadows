# DUNGEON-GEN — génération de donjons cyclique (v1 : les mines)

> Chantier démarré le 2026-08-07. Générateur « à tiroirs » fondé sur la
> cyclic dungeon generation (Dormans / Unexplored) : le même pipeline sert
> mines simples → antres → temples → donjons complexes en changeant des
> DONNÉES (patterns autorisés, récursion, vocabulaire lock/key, creusage),
> pas du code. Verticalité native : salles multi-étages, rampes, puits.
>
> Réalise l'outil « générateur de bases de donjons par règles » du
> MEADOWS-PLAN (§A, P1) : sortie = records/prefabs retouchables à la main,
> jamais un système runtime du monde canonique (décision actée 6).

---

## 1. Décisions structurantes (avec le dev, 2026-08-07)

1. **Portée v1 : mine jouable en jeu** — socle headless + outil éditeur +
   une mine explorable (entrée/sortie), nav intérieure comprise.
2. **Multi-étages : étages empilés en Y** dans UN worldspace intérieur
   (cellules XZ partagées, positions absolues). Salles traversantes, puits
   et balcons natifs. Pas de « un worldspace par étage ».
3. **Fonction pure, deux orchestrations** : `bakeDungeon(params) → données`
   est pur et déterministe (§8) ; v1 orchestré par l'outil éditeur
   uniquement. Le seam runtime-Sandbox est conçu, pas construit (le
   pattern terrain-gen : « un seul générateur, deux orchestrations »).
4. **Géométrie v1 : mesh de caverne généré** (champ de densité analytique
   creusé le long du graphe spatial → surface nets par cellule) — pas un
   assemblage de modules. Les modules/kits restent le second backend
   naturel (temples, parties bâties), tiroir ultérieur.
5. **Le donjon n'est PAS un silo** : entrées depuis la landscape scene OU
   depuis des intérieurs (portes ordinaires), même scène/renderer, sortie
   = cellules ordinaires retouchables avec TOUS les outils (LevelEditor,
   scatter, prefabs, végétation…).

## 2. Synthèse de recherche (sources et enseignements)

Sources : Dice Goblin (cyclic dungeon generation), Sersa Victory (les 12
cycle patterns), Boris the Brave (*Dungeon Generation in Unexplored*,
*Enter the Gungeon*, *Graph Rewriting*), thèse Dormans (*Engineering
Emergence*, mission/space grammars), blogs Ludomotion (Unexplored 2 :
génération en couches, *Theory of the Place*, *Tiles to Curves*), VAZGRIZ
(donjons 3D voxel : Delaunay + MST + cycles + escaliers A*).

Enseignements appliqués :
- **Mission / Space / Geometry séparés**, frontières sérialisables, dump
  visualisable à chaque étape (DOT, ASCII).
- **Le cycle est l'unité de design** : deux arcs différenciés entre entrée
  et objectif ; ~12 patterns encodant chacun une intention (lock-and-key,
  two-keys, hidden shortcut, dangerous route, foreshadowing, gambit,
  blocked retreat, altered return, monster patrol, false goal…) ;
  imbrication récursive aux nœuds Room.
- **Pas de moteur de réécriture de graphes généraliste** : la qualité
  d'Unexplored vient de ses ~5000 règles, pas du moteur. Les patterns
  codés en dur comme fonctions + insertion récursive suffisent (§2.11).
- **Grille de nœuds dès le départ** (pas de graphe libre) : la planarité
  par étage est garantie par construction, l'axe étage est libre.
- **Solvabilité = invariant de structure** : les liens lock↔clé sont des
  données du graphe, re-vérifiées après CHAQUE transformation (doctests),
  jamais réparées a posteriori.
- **La verticalité vit dans le graphe** : étage (ou intervalle d'étages)
  sur les nœuds, arêtes typées — la chute est le one-way naturel, le
  balcon un foreshadowing natif. Aucun pattern nouveau nécessaire.

## 3. Architecture

Lib headless `engine/dungeon/` (dans `meadows`, zéro include render —
même statut que `engine/terrain/generation/`). Pipeline D1→D7, chaque
étage une fonction pure testée :

```
D1 MissionGraph  engine/dungeon/MissionGraph   cycles, locks/keys, solvabilité, dump DOT
D2 SpaceGraph    engine/dungeon/SpaceGraph     embedding grille X×Z×étages, corridors
                                               routés (BFS), dump ASCII par étage
D3 CarveField    engine/dungeon/DensityField   SDF analytique : ellipsoïdes (salles),
                                               capsules (tunnels/rampes), bruit 3D parois,
                                               PLANCHERS PLATS par plan de sol
D4 MeshExtract   engine/dungeon/MeshExtract    surface nets sur lattice GLOBALE, chunks
                                               par cellule 64 m raccord au bit près
D5 NavBake       engine/dungeon/NavGrid        grille marchable MULTI-NIVEAUX (CSR),
                                               asset .nvg
D6 Populate      engine/dungeon/DungeonBake    torches en paroi, entrée, AO/teintes
                                               vertex-color échantillonnées dans le champ
D7 Emit          world/dungeon/DungeonRecords  bake → records (live + session)
Outil            game/scenes/DungeonGenTool    panel : bake (worker) → Accept → Export
```

### Décisions d'implémentation notables

- **Surface nets plutôt que marching cubes** (D4) : même contrat (lattice
  globale, coutures bit-identiques entre chunks — doctesté), sans les
  tables de cas ; sommet = centroïde des crossings, meilleure qualité sur
  l'organique. Un quad appartient au chunk qui possède le milieu de son
  arête de lattice.
- **Salles sur le sous-réseau pair** (D2) : x, z pairs — les rangées
  impaires restent des canaux de corridors, sinon le routage (cases
  libres uniquement) s'étouffe.
- **Corridors banals partageables, le reste exclusif** : deux tunnels
  Passage/Dangerous sans lock peuvent se croiser (naturel en mine, sans
  effet sur les locks) ; les corridors verrouillés/cachés/one-way restent
  exclusifs — la topologie D1 survit au creusage.
- **Un saut de rampe réserve son prisme vertical complet** (les cellules
  des deux étages aux deux positions) : sans cela, un tunnel plat routé
  au-dessus d'une rampe devient un balcon sur une goulotte — falaise de
  nav infranchissable (bug réel trouvé par le doctest end-to-end).
- **Planchers plats par plan de sol** (D3) : chaque primitive est coupée
  par `max(sd + bruit, solY − y)` — le bruit ne sculpte que parois et
  plafonds. Salles et tunnels d'un slot partagent le même plan (zéro
  lèvre aux jonctions), les rampes sont des plans inclinés réguliers
  (pente 11/14 ≈ 0,79, marchable), les puits restent des à-pics one-way.
  Sans cela, les lèvres des unions de surfaces courbes bruitées cassent
  la nav (> 0,55 m) à chaque embouchure. **Corollaire : toute primitive
  doit plonger sous le plan de plus que l'amplitude du bruit** (lift =
  0,45 pour tunnels ET salles) — sinon le bruit soulève le fond
  au-dessus du plan et le sol plat se brise en bosses courbes plus
  hautes que le pas de nav (raté à 1 cm près : 0,54 m pour 0,55 de
  limite). Et le **disque de sol plat vaut r·√(1−lift²)** : à 0,85 une
  salle n'offrait que 53 % de son rayon en sol utilisable — les props
  scatterés « spawnaient dans les murs » (seed 1340) et les salles
  paraissaient petites ; à 0,45 le disque fait ~90 % de r.
- **Embedding auto-grandissant** : tous les 8 essais échoués, la grille
  gagne +2 (déterministe) — l'embedding réussit par construction au lieu
  d'échouer seed par seed.
- **La capacité d'une salle est de 4 corridors** (4-voisinage des canaux)
  — trois contraintes en découlent, qu'aucune croissance de grille ne peut
  racheter : AUCUNE salle sur l'anneau extérieur de la grille (une salle
  de bord n'a que 2-3 canaux — les coins étaient les seeds inroutables),
  les sous-cycles ne se greffent que sur des salles de degré ≤ 2 (l'hôte
  monte à 4 pile), et le placement s'ÉTALE avec les tentatives (minRing
  jitté) — un cluster « plus proche slot libre » a la même forme à toute
  taille de grille, grossir ne le desserre pas. Trouvées par le sweep de
  seeds — les échecs d'embedding « impossibles » sont toujours une
  contrainte de degré ou de bord.
- **Un saut d'étage ne partage JAMAIS ses cellules** (paire + prisme
  vertical, même sur un corridor par ailleurs partageable) : son tube
  traverse la colonne en plein vide — tout autre corridor routé par ces
  cellules se fait percer par au-dessus (une bouche de rampe dans son
  plafond, trou infranchissable au lieu d'une jonction — le bug « on ne
  peut pas remonter » de la première session de jeu). **Y compris le
  corridor lui-même** (seed 1336) : le chemin d'une arête ne peut pas
  recouper le prisme de ses propres rampes — validé sur le chemin FINAL
  après routage (valider pendant la recherche la stérilise), rejet =
  nouvelle tentative.
- **La solvabilité est un invariant d'ÉTATS DE JEU, pas de graphe nu** :
  « tout atteignable depuis l'entrée + retour depuis le but » laissait
  des poches piégeuses (un one-way te dépose dans une zone dont la seule
  autre sortie est verrouillée, clé ailleurs). Trop fort dans l'autre
  sens (« retour depuis chaque nœud sans clé ») rejette les poches
  légitimes derrière serrure. Le bon invariant : pour chaque état
  (nœud, clés détenues) atteignable en jouant, le retour à l'entrée
  existe — clés en bitmask, espace d'états minuscule, doctesté (« rejects
  a strandable pocket »).
- **Le but s'ancre AVANT le reste, loin et profond** (retour dev,
  2026-08-14 : « l'objectif était un couloir plus loin ») : l'ordre
  Unexplored — épingler entrée ET but aux extrémités (coin opposé de la
  grille, étage le plus profond : une mine descend vers son trésor), puis
  étirer la boucle entre les deux. Le placement « au plus près du
  parent » seul agglutine TOUT autour de l'entrée, but compris. Les arcs
  du cycle principal sont aussi rembourrés (+2 salles) : un arc court
  d'une salle vers le but du donjon = donjon trivial. Doctesté (« goal
  anchors far and deep », 30 seeds).
- **Sortie de service** (retour dev, 2026-08-13) : le raccourci de salle
  du boss à la Skyrim — un nœud Key accroché au Goal + une arête Locked
  vers l'entrée, entièrement au niveau du graphe de mission
  (`MissionParams.serviceExit`) : la grille se voit à l'aller depuis
  l'entrée, le levier ne s'atteint que par le trésor, la machinerie
  barrière/levier existante fait le reste sans une ligne de plus.
- **NavGrid en sphere tracing** : la marche en colonne saute
  `|d| − marge` d'un coup dans la roche et l'air profonds (le SDF minore
  la distance, marge 1 m pour le bruit de paroi), pas fins de 0,25 m
  seulement au dernier mètre — bake nav ~10× plus rapide, résultat
  identique.
- **Nav multi-niveaux** : plusieurs sols par colonne XZ (CSR
  `{floorY, clearance}`), bakée du MÊME champ que le mesh (aucun
  désaccord possible). `world::InteriorNavigator` (A* colonnes×niveaux,
  `maxStep` 0,55 — les rampes passent, les puits non) derrière
  l'interface `nav::Navigator` inchangée.

### Identités et re-Accept

- `dungeonId` = famille `7e88a112-…` clé = seed (slot suivant du schéma
  TerrainGenTool) ; le guid du donjon EST celui du worldspace intérieur.
- Tout record/asset dérive par `Guid::combine(dungeonId, {index, salt})`
  → re-Accept = PATCH des mêmes records, jamais de doublon (doctesté) ;
  les retouches manuelles survivent en couche au-dessus (§5).
- Les records sont stagés DEUX fois : LIVE dans la base résolue (la
  session courante peut streamer/voyager immédiatement — le précédent
  `materializeCell`, plus `WorldModel::indexReference` pour les
  références) ET en drafts d'EditSession (l'export shippe un mod
  ordinaire). L'ordre est important : le draft de session AVANT l'ajout
  live, sinon `forms.find()` masque la création (règle « shadowing »
  d'EditSession::createForm).

### Assets

- `.cmesh` (`engine/assets/CookedMesh`) : MeshData verbatim, magie CMSH +
  version format + version contenu (kDungeonBakeVersion). Distinct de
  glTF à dessein : le decode glTF de MeshCache recentre au sol et bake
  l'AO disque — faux pour des chunks alignés cellule. Le MeshCache charge
  les `.cmesh` tels quels (surcharge « version-blind » : un mesh shippé
  est un mesh ; seul le producteur exige sa version).
- `.nvg` (`engine/dungeon/NavGrid`) : la grille de nav, chargée au
  `performTravel` vers un worldspace intérieur via son `NavGridForm`.

## 4. Invariants respectés

- §2.10 : `engine/dungeon/` headless (lib meadows, prouvé par le simlink
  et les doctests) ; seul changement render = 6 lignes de branche
  `.cmesh` dans MeshCache::decode.
- §2.11 : réutilise WorldspaceForm/CellForm/ReferenceForm, cellGuidFor/
  materializeCell, EditSession/exportPlugin, Guid::combine, core::Rng,
  CellStreamer, DoorForm/MarkerForm/LightForm, nav::Navigator, le pattern
  TerrainGenTool. Nouveaux mécanismes uniquement là où rien n'existait :
  volumique (D3/D4), nav multi-niveaux, format .cmesh/.nvg.
- §5 : sortie = un mod TOML ordinaire ; le thème/tuning deviendra une
  Form moddable (tiroir).
- §8 : `bakeDungeon` bit-identique par (seed, params) — doctesté ; Rng
  seedé, aucune horloge.

## 5. Tests (tous headless, `tests/Dungeon*`, `CookedMeshTest`)

- Mission : déterminisme, solvabilité des 5 patterns × 50 seeds, mix
  récursif × 100 seeds, lock sans clé rejeté, piège one-way rejeté.
- Space : déterminisme + solvabilité re-vérifiée après embedding × 60
  seeds, salles sur slots distincts, salles hautes, chutes verticales
  seulement sur one-way et vers le bas, entrée en bordure étage 0,
  échec propre (graphe vide) si ça ne rentre pas.
- Density : salles/corridors creusés, roche pleine au large, pureté,
  gradient vers la roche.
- Mesh : sphère fermée (chaque arête partagée par 2 triangles),
  orientation cohérente, RACCORD ENTRE CHUNKS (union fermée), mesh vide
  hors volume, déterminisme.
- Bake : cellules locales, couleurs bornées, pas de doublon de cellule,
  bit-identique par seed, entrée dans la salle d'entrée étage 0.
- Nav : deux sols au même XZ, pas de chemin sans rampe, chemin par la
  rampe, round-trip .nvg + refus de version, **mine bakée marchable de
  l'entrée à l'objectif** (le test end-to-end qui a trouvé les deux
  vrais bugs : balcons de rampe et lèvres de planchers), **jonction
  couloir→rampe sans ressaut** (profil de sol scanné, > 0,35 m interdit),
  **toute ancre gameplay sur un sol nav** (la mine par défaut de l'outil,
  toutes familles).
- Records : émission complète (worldspace/cellules/statics/nav/portes),
  live + session, re-Accept = patch sans doublon, export = plugin,
  ennemis stagés aux positions du bake.

## 6. État & reste à faire

- ✅ Briques 1-10 : pipeline complet, outil, câblage scène, nav — 36
  doctests donjon verts (~110 k assertions), suite complète verte.
- ✅ Brique 12 — populate gameplay v1 (2026-08-13) : les nœuds du graphe
  de mission deviennent des objets jouables. `bakeDungeon` émet des
  ancres (`barriers`/`levers`/`chests`/`oreVeins`/`enemySpawns`/
  `npcSpawns`) ; `stageDungeonRecords` les instancie depuis le
  **mine-kit** (`game/data/base/mine-kit.toml` : MineBarrier, MineLever,
  MineChest + loadout, OreChunk, MineHermit + dialogue ; les ennemis
  réutilisent le Bandit du village, hostile via `Faction.Bandits`).
  Le couple levier→barrière est apparié sans état : la barrière a pour
  guid `barrierForLever(leverRef)` (combine), inversé par la scène au
  moment du [E]. Deux `PromptKind` ajoutés (`Container` : loadout roulé
  paresseusement dans l'Inventory puis écran de transfert — un coffre
  vidé reste vidé via la couche pending ; `Lever` : la barrière est
  détruite + `disableReference`, toast loc). Torches : 13 m / 4.8
  (retour dev).
- ✅ Retours de la première session de jeu (2026-08-13) : **sortie de
  service** (cf. §3) et **densité de peuplement** — ennemis par salle
  (chance paramétrable, `enemyChancePerRoom` 0.45), filon GARANTI dans
  chaque cul-de-sac + filons bonus (`bonusVeinChancePerRoom` 0.25),
  scatter déterministe par seed hors du centre des salles.
- ✅ Session seed 1340 (2026-08-14) : `MiscItemForm`/`ArmorForm`
  n'avaient AUCUNE catégorie spawnable (`registerCoreCategories`) — les
  références de filons étaient ignorées en silence par le CellLoader
  (bug du monde préexistant, révélé par les donjons) ; et le disque de
  sol des salles corrigé (cf. leçon planchers).
- ✅ Les ennemis invisibles (2026-08-14) : `NpcSpawner` ancrait TOUT
  acteur (et les markers de patrouille) à `render::terrain::height` —
  dans un intérieur, les bandits étaient téléportés à l'altitude du
  terrain extérieur au-dessus de la mine. Gate `interiorMode` posé
  (le y auteuré est absolu en intérieur). **Règle générale à retenir :
  toute consommation de `terrain::height` côté scène doit être gated
  par `interiorMode`** — troisième membre de la famille après le
  TerrainNavigator (nav) et le ground-snap du streaming.
- ✅ Session 2026-08-14 (bandits statiques + réalisme) : **marqueurs de
  patrouille stagés par salle** (sans marqueurs `patrol` chargés, les PNJ
  n'errent jamais — figés, cône de vision immobile, détection quasi
  nulle) ; **le levier de service dans le corridor, juste derrière sa
  grille** (le raccourci Dark Souls : tirer, et on est déjà à la porte) ;
  **greffe « sucette » des sous-cycles** (un couloir intermédiaire vers
  un hub qui porte le cycle) : distribution des degrés 2 > 3, plus
  aucune salle à 4 sorties — « les couloirs mènent quelque part » ;
  relief accentué (jitter 1,5, wobble 2,0).
- ✅ **La marchabilité est un invariant de génération VÉRIFIÉ** : après
  le bake nav (avant le maillage coûteux), un flood-fill valide que
  toutes les salles sont atteignables depuis l'entrée (chutes autorisées
  vers l'avant) et que l'entrée se re-rejoint EN MONTÉE depuis le but et
  chaque levier ; un embedding fautif est re-roulé (germe d'espace
  re-dérivé, déterministe). Motif : trois sessions de jeu ont trouvé
  trois pièges géométriques distincts un seed à la fois — la classe
  entière est close par l'invariant, plus par les correctifs. Règle
  géométrique ajoutée au passage : **une rampe ne touche jamais une
  salle** (l'enveloppe d'air d'une grande salle avale la fin de la rampe
  qui plane alors au-dessus du sol) — les rampes relient des cellules de
  canal, les salles s'abordent à plat.
- ✅ Naturalisation (proposition dev 2026-08-14) : **jitter de hauteur
  par slot** (bruit 2D lissé ±1,2 m, partagé par tous les étages d'une
  colonne — l'écart d'étages reste exact) : les salles d'un même étage ne
  sont plus à niveau, les couloirs deviennent des pentes douces ; et
  **ondulation des couloirs** façon branches du TreeGenerator (point
  médian décalé latéralement ±1,5 m par segment, haché par seed) — sous
  le rayon du tube pour que l'axe nominal (torches, waypoints) reste
  dans l'air. **Les rampes restent droites** : leurs deux plans de sol
  articulés au coude créeraient une marche de nav — et un escalier de
  mine se creuse droit. Tout l'aval (mesh, nav, records, portes) en
  hérite via slotCenter/le champ, zéro changement ailleurs.
- ✅ Compactage (retour dev 2026-08-14, « trop grand, couloirs trop
  longs ») : métrique resserrée (cellSpacing 14 m, floorSpacing 11 m,
  salles r 6-9), rembourrage du cycle principal +1 (au lieu de +2),
  arcs longs ≤ 2 salles. La grille de la sortie de service se place à
  l'embouchure CÔTÉ ENTRÉE (visible dès la salle d'entrée, sortie
  lisible d'un regard une fois le levier tiré).
- ✅ Session 2026-08-14 (marche en bas des rampes + ennemis re-disparus) :
  un seul diagnostic pour les deux. **La poche sous-rampe** : la calotte
  terminale du tube plat qui débouche sur la cellule basse d'une rampe
  creuse ~2 m d'air PLAT sous le sol montant de la rampe ; le sol y reste
  au niveau du couloir puis rattrape le sol de rampe d'un coup — ressaut
  ~0,5 m, juste au-dessus du step-up Jolt (0,4) et juste sous la tolérance
  du validateur nav (0,5), donc génération acceptée ET saut obligatoire.
  Correctif dans le champ : **une rampe coupe tout air sous son plan de
  sol dans sa bande XZ** (`Pipe::cutsBelow` ; légitime car les cellules
  de rampe sont revendiquées exclusivement), la coupe strictement bornée
  à l'emprise du segment (un clamp de t étendrait le sol de chaque
  demi-tube au-delà de ses bouts et falaiserait la section d'en dessous) ;
  et l'interpolation du sol des tubes passe en **projection horizontale**
  (la projection sur l'axe incliné décalait le plan de sol près des
  extrémités — c'est ce décalage qui masquait la poche au validateur).
  **Les ancres gameplay sont désormais validées par la grille nav au
  populate** : scatter re-tiré jusqu'à un sol nav (8 essais, repli centre
  de salle), milieux Dangerous rabattus sur la cellule marchable la plus
  proche, gardien/mineur vérifiés — c'était la cause des « ennemis
  disparus » : le scatter pouvait sortir du disque de sol érodé par le
  bruit (et re-jitté), l'acteur spawnait dans la roche. **Les barrières
  s'ancrent au centre d'une cellule du corridor** (jamais déplacé par le
  wobble — un milieu de segment peut être à 2 m de l'axe creusé, la
  grille dans le mur), cellule à voisins plats préférée. Le log d'Accept
  compte désormais ennemis/filons/patrouilles. Deux tests neufs :
  jonction de rampe sans ressaut (> 0,35 m interdit le long du parcours),
  et « toute ancre a un sol nav » sur la mine par défaut de l'outil.
- ✅ Re-Accept = mise à jour LIVE (demande dev 2026-08-14) : le `Stager`
  passe en add-**or-update** — un record déjà résolu (mod exporté d'une
  session antérieure) est réécrit en place par réflexion
  (`FormDatabase::getMutable`, l'écoutille outil-seulement que le
  commentaire d'index §2.2 prévoyait ; ré-indexation de cellule via
  `WorldModel::unindexReference` si la référence change de cellule).
  Une famille qui RÉTRÉCIT (moins d'ennemis, moins de cellules) voit ses
  restes désactivés (`enabled=false`, live + brouillon d'export) par
  balayage des guids contigus au-delà du nouveau compte — leviers/
  barrières balayés par lockId borné. Donc : re-générer le même seed met
  la session courante au niveau du nouveau bake, warning de l'outil
  reformulé (« Export again or the next session reloads the old
  version »). **Sémantique de propriété** : le générateur possède TOUS
  les champs de SES records — une regen écrase les retouches manuelles
  faites sur eux (y compris un `enabled` manuel). Le travail manuel qui
  doit survivre aux regens = des AJOUTS (nouvelles références, scatter,
  prefabs — guids propres, jamais touchés) ou des retouches dans un
  plugin séparé chargé APRÈS le mod du donjon (couche §5,
  dernier-écrivain-gagne — la vue de conflits les montre). Limite
  connue : si le donjon était déjà chargé/visité dans la session, les
  entités spawnées ne se rafraîchissent qu'au prochain voyage.
- ✅ Grille en diagonale (retour dev 2026-08-14, seed 1337) : une
  barrière posée sur un VIRAGE du corridor est orientée sur la
  bissectrice et doit couvrir la diagonale de la jonction, plus large
  que le tube droit. Les ancres portent une largeur (`Anchor.width`) :
  les verrous ordinaires préfèrent une cellule DROITE (segments
  alignés) près de leur cible, la sortie de service garde la proximité
  de l'embouchure (lisibilité d'abord) et sa grille s'élargit ×1,6 sur
  un virage.
- ✅ Le PNJ qui disparaît à l'approche (seed 1338, 2026-08-14) : QUATRE
  membres oubliés de la famille `terrain::height`/interiorMode, tous du
  côté mouvement. Un PNJ immobile restait visible ; son premier tick de
  déplacement (errance/perception à l'approche du joueur, combat)
  re-snappait son y à l'altitude du terrain EXTÉRIEUR → téléporté hors
  de la mine. Gates posés : `NpcMovement` (les deux fonctions de
  locomotion — en intérieur, sonde de collision vers le bas via
  `groundAt`, nouveau helper partagé), le point de fuite de
  `NpcCombatController` (but de chemin : même étage en intérieur),
  l'ancre de package de `NpcScheduleController` (y auteuré absolu en
  intérieur), et `FollowerController::teleportNear` (rattrapage /
  arrivées de voyage — `FollowerContext` porte désormais interiorMode +
  physics). Audit exhaustif fait : StreamingController (skipsSnap
  intérieur) et SceneEditor (branche interiorMode) étaient déjà gatés ;
  TerrainSculptTool/MapRaster/TerrainCollision sont extérieurs par
  nature. La règle reste : **tout `terrain::height` côté scène est gated
  par interiorMode, sans exception** — le sol intérieur, c'est la mesh
  de collision. Complément (bandit marchant au plafond) : un raté de la
  sonde (bouche de couloir donnant sur l'air d'une salle haute, sol
  11 m plus bas hors de portée) n'est PAS « rester sur place » mais
  **un bord de vide : le pas est refusé** (`groundAt` retourne false,
  les deux movers annulent le déplacement horizontal) — un PNJ
  cinématique ne tombe ni ne marche sur l'air ; le balcon redevient ce
  qu'il doit être, un poste d'observation.
- ✅ Bandits dans les murs (2026-08-14) — le rayon d'agent : la NavGrid
  0,5 m ne connaissait que le dégagement VERTICAL ; une colonne à 10 cm
  de la roche est « marchable », donc l'A* longeait les murs et le
  scatter posait des capsules à moitié dans la paroi. Réponse = étendre
  le système existant, PAS Recast (qui reste le tiroir différé) :
  `NavGrid::airAt`/`wallAdjacent` (8-voisins, air à hauteur d'homme) ;
  l'`InteriorNavigator` paye une **surtaxe murale** (+2 cellules) sur
  les colonnes collées — le chemin se centre où le tube est large, les
  passages étroits (couloirs Hidden ~1,3 m) restent traversables car
  c'est un coût, jamais une érosion (une érosion aurait déconnecté les
  raccourcis cachés) ; et les ancres d'ACTEURS (ennemis, gardien,
  mineur, scatter ennemi) exigent une colonne `standable` (marchable ET
  non murale) — les props (filons, coffres, torches) gardent le droit de
  coller aux murs. Le test d'ancres vérifie standable pour les familles
  d'acteurs.
- ✅ Bandit « dans le plafond » (seed 1338, 2026-08-14) — diagnostic par
  dump nav headless (colonnes empilées sol −1 + niveau intermédiaire) :
  les « étagères fantômes » sont en réalité **le flanc des rampes** — à
  la jonction basse, le tube de la rampe déborde latéralement dans le
  volume du couloir et son sol y forme une poche-balcon ~1,75 m
  au-dessus du sol, qui se lit « dans le plafond » depuis le couloir.
  Trois garde-fous (les deux premiers posés au passage) : la coupe
  sous-rampe devient une **dalle bornée en profondeur** (un demi-espace
  tranchait de vraies étagères plates dans le plafond des espaces plus
  profonds croisés par la bande) ; le **budget de la sonde de sol passe
  à 1,8 m** (1,2 au-dessus + 0,6 sous les pieds = une marche — l'ancien
  3 m laissait un PNJ descendre d'un coup sur une surface 2 m plus bas à
  travers une ouverture) ; et la poche est naturellement évitée par
  l'A* (cul-de-sac surtaxé) et par les ancres (`standable`). Les ancres
  du 1338 re-vérifiées saines une à une — le bandit perché venait du
  staging d'un build antérieur : re-Accept requis après chaque
  changement de génération.
- ✅ Bandit ENFONCÉ dans un mur (1337 minimal, capture du dev,
  2026-08-14) : ancres re-vérifiées saines sur sa config exacte → le
  bandit y est entré en MARCHANT, par deux failles du mouvement direct :
  (1) la sonde de sol acceptait un impact jusqu'à +1,2 m au-dessus des
  pieds — en marchant vers un mur elle accrochait l'évasement de sa
  BASE et faisait grimper le PNJ dans la roche tick après tick ; un
  impact au-dessus du budget de marche (+0,5 m) refuse maintenant le
  pas ; (2) `steerBlocked` ne lançait qu'UN rayon central à hauteur de
  poitrine — en incidence rasante sur un mur incurvé, le rayon glisse
  parallèle pendant que l'épaule de la capsule s'enfonce ; trois rayons
  couvrent désormais la largeur (centre ± 0,35 m perpendiculaire).
  Morale pour un PNJ cinématique en caverne : le sol ET les murs se
  testent à chaque pas — la sonde verticale borne les deux sens
  (−0,6/+0,5), les rayons horizontaux couvrent la capsule. Complété
  (retour dev : « ils entrent encore trop dedans ») par une
  **dépénétration active à chaque pas** (`resolveWallOverlap`, intérieur
  seulement) : huit sondes horizontales à hauteur de poitrine, tout
  impact plus proche que le rayon de capsule + marge (0,5 m) REPOUSSE le
  PNJ le long de la sonde — couvre les recouvrements rasants que les
  rayons directionnels ne voient pas ET le suivi de chemin qui coupait
  les coins entre waypoints (qui, lui, n'avait aucun test mural).
- ✅ Fuite vers la sortie (demande dev 2026-08-14) : en intérieur, un
  combattant brisé fuit vers **l'entrée de la mine** plutôt que « 12 m à
  l'opposé » (qui l'envoyait souvent au fond d'un cul-de-sac). Le point
  de sortie est le marqueur d'arrivée du dernier voyage, mémorisé par la
  scène (`interiorArrival`) et passé par `NpcContext.interiorExit` ;
  repli sur la fuite simple quand la sortie est derrière l'attaquant
  (produit scalaire) ou déjà atteinte (< 6 m). Le re-path de 0,8 s
  réévalue la condition en continu — si le joueur coupe la route, le
  bandit repart en fuite directionnelle. **Et il passe la porte** (suite
  demandée) : arrivé à < 3 m de la sortie, il est despawné et sa
  référence désactivée par la couche pending (§2.11 — il reste absent au
  re-entry ET dans les saves) ; si le joueur sort **dans les 20 s**, le
  fuyard réapparaît dehors (spawn transient sans cellule, ~14 m passé la
  porte, échelonné/alterné par fugitif, ancré terrain) — au-delà, fuite
  réussie. Chaîne : `Npc.escapedInterior` (posé par le flee du combat) →
  balayage `updateNpcs` (disable + destruct + horodatage
  `interiorEscapes`) → respawn au `performTravel` sortant. **Protocole
  de destruct appris à la dure (SIGSEGV du playtest)** : détruire une
  entité PNJ en pleine frame exige de purger d'abord TOUS les handles
  tenus — les `combatTarget` des autres PNJ ET le lock-on du joueur
  (`FollowerController::disengage`, factorisé du purge d'`onDeath`) — et
  de retirer l'entrée `Npc` de la liste du director AVANT le destruct
  (le reste de la frame parcourt cette liste). Et **ses blessures le
  suivent dehors** : la base `health` est capturée à la porte et
  re-seedée par l'idiome du Spawner (`setBaseValue` +
  `initializeCurrent` — écriture de persistance §2.9, pas une mutation
  gameplay) — **APRÈS `refreshNpcs`** : `finalizeActorSpawn` →
  `initializeActorStats` remplit les vitals de tout acteur frais, un
  re-seed fait avant est écrasé (trouvé au playtest : fugitifs revenus
  à pleine vie). **Et pas d'amnésie** (retour dev) : le fugitif re-sort
  avec sa `Perception` en Alert épinglée sur la porte et le joueur en
  `combatTarget` — sa santé étant sous le seuil de courage, le
  contrôleur de combat reprend la fuite dès le premier tick (aucun état
  « fuite » à transporter : la règle courage/santé le re-dérive).
- 🔨 Brique 11 — validation IN-GAME (le dev) : ouvrir l'éditeur dans la
  LandscapeScene, panel « Dungeon generation » → Bake mine → Accept
  (porte posée à la caméra) → entrer par la porte : collision (fastCook
  sous fade), torches (froxels), parcours, PNJ éventuel (nav intérieure),
  re-Accept, Export + rechargement. Vigilances notées : `giOccluder` du
  chunk concave (fallback = flag par form), perf du cook Jolt (M1 Air),
  budget lumières si torches trop denses.

## 7. Tiroirs ultérieurs (conçus pour s'emboîter)

- Les 7 autres cycle patterns (même API de fonction pattern).
- Thème/biome en Form moddable (`DungeonThemeForm` : matériaux, pools de
  props/spawns, paramètres de creusage) — mine/antre/temple = données.
- Second backend géométrie : assemblage de modules/prefabs à connecteurs
  derrière le MÊME SpaceGraph (temples, parties bâties).
- Populate gameplay, suite : triggers d'éboulement (BlockedRetreat joué),
  filons récoltables par outil (mining), clés-objets en plus des leviers,
  un `DungeonThemeForm` regroupant kit + tuning par type de donjon.
- Orchestration runtime Sandbox (rupture décision 6 à faire valider).
- Lacs souterrains (l'eau est coupée en interiorMode), portals
  d'occlusion (le donjon dense sera le premier client), Recast réel,
  décimation border-locked des chunks, triplanaire roche, minerai
  émissif.
