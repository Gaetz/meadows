# TERRAIN-MAPS — l'architecture cartes bornées (étude TERRAIN-RECUL)

> Étude du 2026-09-14, déclenchée par les falaises linéaires et lacs coupés
> aux frontières de tuiles. Statut : **architecture validée par prototype,
> plan de migration proposé — décision dev en attente.** Ce document est la
> référence durable ; le journal de la génération reste `docs/TERRAIN-GEN.md`.

## 1. Le constat qui a tout déclenché

Le pipeline actuel érode chaque tuile de 4096 m dans sa propre fenêtre
(apron 1536 m), chaque fenêtre épinglant son niveau de base à SON rim
(`FluvialErosion.cpp isBaseLevel`). Mesure `cooker border-report` sur 5
frontières du monde seed 1337 (cache dev ET re-bake à neuf bit-identiques —
cache sain) :

| Frontière | Divergence max de bande | Mur réel (marche blendée) |
|---|---|---|
| x=8192  | 313 m | 82° |
| z=4096  | 200 m | 83° |
| z=8192  | 339 m | 88,5° |
| z=12288 | **441 m** | 88° |
| z=16384 | 7 m (saine) | — |

Quand un système de vallées traverse une frontière, la fenêtre qui voit
l'exutoire creuse, l'autre laisse une montagne : **deux équilibres
d'érosion incompatibles**. Le « lac interrompu » (x≈8192) est ce mur —
le flood s'arrête contre la montagne de la voisine. L'apron ne « couvre »
pas (l'hypothèse de TERRAIN-GEN B9 est falsifiée) ; aucun apron fini ne
peut borner un bassin versant. L'érosion est un phénomène global calculé
à travers des fenêtres locales — contradiction de principe, pas bug.

## 2. La décision d'architecture

Plan de développement du dev : phase 1 = boucle de gameplay sur un monde
procédural virtuellement infini ; phase 2 (financée) = monde fini
scénarisé + modding radical. Décision :

**Le monde devient un ensemble de CARTES BORNÉES** (8-24 km, un biome
dominant par carte, voyage de carte en carte) :
- une carte = **UNE érosion globale offline** (zéro frontière interne par
  construction), découpée ensuite en tuiles `.trg` pour le streamer
  runtime EXISTANT ;
- phase 1 : cartes générées procéduralement à la demande — carte (i,j)
  seedée par ses coordonnées, bakée en fond quand le joueur approche →
  monde virtuellement infini comme graphe de cartes finies ;
- phase 2 : cartes autorées ; un mod AJOUTE une carte (plugin §5
  ordinaire, worldspace + assets) ;
- c'est le modèle worldspace de Skyrim, que le data model possède déjà
  (`worldspace → cell → reference`, `stageDungeonRecords` = le précédent
  complet « un outil crée un worldspace entier en records §5 »).

## 3. Le prototype qui la valide (`cooker map-proto`)

Un stage-1 aux dimensions de la carte (tileSize = N×4096, apron =
kBasinResolveMargin) sert de 9 voisins au stage-2 de chaque tuile — le
composite est identique des deux côtés de chaque frontière intérieure
par construction. Carte 8×8 km (2×2 tuiles), seed 1337 :

```
stage-1 global (897²)  :  9,6 s
finalize des 4 tuiles  :  6,6 s     (~16 s la carte)
divergences intérieures : 0,000 / 0,216 / 0,000 / 8,290 m
                          (baseline fenêtrée : 200-441 m)
```

Le résidu 8,29 m = les passes de finalize par tuile (carve rivière
≤7,5 m + érosion fine ≤3,5 m) dont la fenêtre hydrologique diffère
encore par tuile. **Il tombe à ~0 en faisant l'hydrologie (S4) UNE fois
par carte** au lieu d'une fois par tuile — prévu en M1. Extrapolation
16×16 km ≈ 1-2 min de bake offline, compatible bake-en-fond phase 1.

## 4. Ce que dit l'inventaire (état des lieux total, 2 agents)

Sur ~30 150 lignes de code terrain : **≈7 % meurent, ≈30 % sont
ré-orchestrées (tous les noyaux physiques — fastscape, thermal,
priority-flood, hydrologie, finalize, imprint — sont grid-agnostiques et
survivent), ≈63 % intactes.** Ce qui meurt est exactement « l'appareil à
prétendre que neuf fenêtres sont un monde » :

- `composeWindow` + gather 3×3, résolution canonique des bassins (145 l.),
  ownership des lacs, dedup inter-tuiles du publish (83 l.),
  `reconcileWaterWithTerrain` runtime (55 l.), apron/overlapMargin/
  waterMargin, cache stage-1 + Stage1Registry (~230 l.), le module
  `Stage0Erosion` mort-né (697 l., jamais branché) ;
- ~443 lignes de tests qui épinglent des hypothèses de tiling,
  remplacées par « les tranches sont bit-identiques dans leur bande ».

Historique : le mode borné était prévu au jour 1 (TERRAIN-GEN D1 :
« Scénario (maps bornées, même pipeline) ») ; le veto sur l'érosion à
gouttelettes (D6) était un veto de tiling — il saute ; 4096 m est un
artefact de perf du commit de naissance (4,4 s/tuile à l'époque), jamais
re-questionné.

Côté monde/jeu : le data model est déjà multi-cartes (cellules implicites
agnostiques du worldspace, voyage par portes résolu par records, saves
par worldspace). **16 hypothèses mono-monde à casser**, les majeures :
`TerrainParams` sans dimension carte, `LandscapeTuningForm` singleton
(seed/seaLevel/snowLine globaux), aucun Form terrain/eau/biome ne porte
`worldspace`, `performTravel` ne swappe jamais le terrain, FarTerrain
18 km centré caméra sans notion de bord, worldspace actif résolu par
editorId (deux « Overworld » en collision dans base.toml/adventure.toml).

## 5. Les ABSENTS (le travail réel de la migration)

1. La notion de carte dans la couche terrain : rect, params par carte,
   manifest (bounds, seed, version, biome, liste de tranches).
2. L'orchestrateur de bake global + la fonction de découpe (map-proto en
   est le prototype ; il faut annulation, progression, plan mémoire — le
   grid fin 2 m d'une carte 24 km = 151 M texels : le finalize RESTE par
   tranche).
3. La condition de bord de carte (mer / anneau de crêtes / fondu — à
   décider) et son masque dans la génération.
4. Le voyage carte-à-carte (étendre `performTravel` : swap du terrain,
   gate de warmup = « la carte voisine est-elle bakée », triggers de col).
5. Le pont §5 : bake bulk → émission de records (la moitié émission
   existe = `TerrainGenTool::accept` ; la moitié bulk = `pre-bake`) +
   champ `worldspace` sur `TerrainRegionForm`/eau/biome.
6. Identité stable de l'eau entre tranches ; ré-ancrage des deltas sculpt
   au re-bake (dette déjà connue, plus aiguë).

## 6. Plan de migration proposé — chantiers M1-M5

- **M1 — Le bake de carte réel** : généraliser map-proto en
  `bakeMapGlobal()` (S1-S3 carte entière) + hydrologie S4 UNE fois par
  carte (tue le résidu 8 m) + `sliceTile()` (S5-S6 par tranche) ;
  manifest de carte ; cache `terrain-cache/<seed>/map_<mx>_<mz>/` ;
  edgeBlend=0 entre tranches d'une même carte ; mesure = divergence
  interne 0,000 partout + budget de bake. Le monde change (re-bake
  global) — validation visuelle dev.
- **M2 — La couche monde multi-cartes** : `worldspace` sur les Forms
  terrain/eau/biome ; `LandscapeTuningForm` → tuning par worldspace ;
  worldspace actif par GUID ; TerrainParams par carte active.
- **M3 — Bords et horizon** : masque de bord dans la génération (mer/
  crêtes selon la carte), FarTerrain/fog/minimap apprennent le rim,
  clamp du streamer au rect de carte, comportement hors-carte (barrière
  naturelle + garde).
- **M4 — Voyage carte-à-carte** : performTravel étendu (swap terrain +
  collision), pré-bake de la carte voisine en fond (déclencheur =
  approche d'un col/bord), gate de chargement réutilisant le warmup.
- **M5 — Cartes en §5** : le pont bake→plugin (records + assets GUID),
  `stageDungeonRecords` comme template ; l'outil éditeur devient
  « Bake map » ; un mod ajoute une carte.
- **Nettoyage transversal** : la kill list (~1130 l. prod), tests
  re-ancrés, `Stage0Erosion` supprimé, docs (TERRAIN-GEN pointe ici).

Ordre : M1 seul livre déjà la valeur (le monde actuel re-baké en cartes
sans falaises) ; M2-M5 peuvent suivre par valeur. Chaque chantier se
planifie brique par brique à son ouverture (cadence habituelle).

## 7. Décisions dev (2026-09-14)

1. **Taille de carte : 24×24 km** — la super-région du MasterNetwork :
   une carte = une super-cellule, le réseau maître devient par-carte et
   son « stitching cross-super différé » disparaît. Grille d'érosion
   (24576 + 2×3072)/16 + 1 = 1921² ≈ 3,7 M cellules ; bake estimé
   ~1-2 min stage-1 + 36 tranches de finalize (parallélisables workers).
2. **Bords : mixtes par carte** — chaque carte décide (mer / crêtes
   selon les côtés), via un masque de bord dans ses params.
3. **Transition : fondu type porte** — le mécanisme de voyage existant
   étendu au swap de terrain (M4 minimal) ; le couloir continu reste au
   backlog.
