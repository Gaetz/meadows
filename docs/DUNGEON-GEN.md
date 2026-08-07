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
  (pente 12/18 ≈ 0,67, marchable), les puits restent des à-pics one-way.
  Sans cela, les lèvres des unions de surfaces courbes bruitées cassent
  la nav (> 0,55 m) à chaque embouchure.
- **Embedding auto-grandissant** : tous les 8 essais échoués, la grille
  gagne +2 (déterministe) — l'embedding réussit par construction au lieu
  d'échouer seed par seed.
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
  vrais bugs : balcons de rampe et lèvres de planchers).
- Records : émission complète (worldspace/cellules/statics/nav/portes),
  live + session, re-Accept = patch sans doublon, export = plugin.

## 6. État & reste à faire

- ✅ Briques 1-10 : pipeline complet, outil, câblage scène, nav — 36
  doctests donjon verts (~110 k assertions), suite complète verte.
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
- Populate gameplay : coffres (reward), filons (culs-de-sac), spawns
  d'ennemis, triggers d'éboulement (BlockedRetreat joué).
- Orchestration runtime Sandbox (rupture décision 6 à faire valider).
- Lacs souterrains (l'eau est coupée en interiorMode), portals
  d'occlusion (le donjon dense sera le premier client), Recast réel,
  décimation border-locked des chunks, triplanaire roche, minerai
  émissif.
