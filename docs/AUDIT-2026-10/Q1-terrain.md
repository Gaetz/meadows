# Q1 — Terrain (`engine/terrain/`, `world/terrain/`, couture `game/`, outils cooker)

> Vague Q1 de la revue de qualité d'octobre 2026 (`README.md` de ce dossier :
> méthode, classes S/G/D/B/H/P, arbitrages à ne pas re-proposer). Relecture
> **intégrale** du 2026-10-23 sur HEAD `61efb65` : 50 fichiers `engine/terrain/`
> (17 769 l.), 12 `world/terrain/` (1 348 l.), 6 fichiers de couture `game/`
> (`MapBaker`, `TerrainBakeStreamer`, `TerrainGenTuning`, 2 230 l.), 8 outils
> terrain du cooker (3 300 l.) — soit ~24 600 lignes. **Aucun code modifié.**
> Les lignes citées sont celles de `61efb65` ; les symboles font foi.

---

## Verdict

Le terrain n'est pas un spaghetti de couplages : la frontière headless tient
(aucun include `rhi/`, `render/`, `platform/`, SDL ou GL dans `engine/terrain/`
ni `world/terrain/` ; `engine/*` ne lit jamais `data/*`), les contrats vivent dans
les en-têtes avec leur pourquoi, et deux fichiers servent déjà de référence
(`GridOps.hpp`, `Noise.hpp` : petits, purs, documentés « bit-identical replacements
for the per-file copies they absorbed »). Le problème est d'une autre nature, et il
est **uniforme sur tout le périmètre** :

1. **Le pipeline est écrit en blocs anonymes.** `extractMapHydrology` fait
   1 923 lignes en quinze passes rythmées par `lap("…")`, `finalizeWater` 645,
   `bakeTileStage1` 404, `landscapeReport` 908, `bakeMapCmd` 458, `bakeMap` 371,
   `extractSnapshot` 422. Les noms des passes existent (dans les `lap()`, dans les
   commentaires S1…S6, dans `TileBake.hpp`) mais pas comme fonctions : on ne peut
   ni tester, ni réordonner, ni désactiver une passe sans relire tout le corps.
2. **Les traits par type sont des chaînes de `if` / ternaires.** `PoiType`
   (28 valeurs) a six chaînes de traits dispersées et une couleur par **plage
   d'ordinaux** ; `BorderStyle` cinq sites de branchement ; les tiers de rivière
   six ternaires ; les ids de palette et de pièce sont des entiers nus dans six
   fichiers. Les tables existent à moitié (`poiTypeName`, `kCharacters`,
   `defaultZoneArchetypes`) : il manque les colonnes.
3. **Chaque champ de paramètre est défini trois à cinq fois** : la struct, son
   `hashParams` écrit à la main (six boucles FNV, dont cinq avec une constante
   fausse), la Form réfléchie qui la miroite (« Defaults MUST match »), les deux
   listes `apply`/`capture` de 155 lignes du pupitre, et parfois la copie dans une
   sous-struct (`seaLevel` est recopié dans **35 sous-structs**). Un champ oublié =
   cache périmé silencieux, et c'est **déjà arrivé** (Q1-37 : le memo des zones ne
   voit pas une édition de `seaThreshold`).
4. **Les helpers ont été copiés plutôt que partagés** : le test « ce point est sous
   un lac » neuf fois, la conversion bbox → cellules dix fois, le noyau
   virtual-pipes deux fois à la ligne près, le bootstrap de la pile de plugins six
   fois, quatre échantillonneurs bilinéaires, huit lecteurs/écrivains binaires
   maison.

Rien de tout cela n'est bloquant pour une brique ; tout cela rend chaque brique
plus chère que la précédente, et c'est exactement le profil que le dev a demandé
de traiter. La bonne nouvelle : **la quasi-totalité des généralisations proposées
est bit-exacte** (les passes partagent déjà leur état en `vector<f32>`, les tables
reprennent les mêmes constantes), donc prouvable par `cooker landscape-report` et
le hash de `MapBakerTest` sans bump de cache. Les exceptions sont nommées.

---

## Contrôles d'invariants

| Invariant | Verdict | Remarque |
|---|---|---|
| §2.10 headless (`engine/terrain`, `world/terrain` sans `rhi/render/platform/SDL/GL`) | **PASS** | grep à vide ; la lib `meadows` ne linke pas `meadows-render` |
| §4 `engine/*` n'inclut jamais `data/*` | **PASS** | la traduction Forms → params vit dans `world/terrain` et `game/` |
| §4 ordre des couches (`layers_audit.py`) | PASS avec bruit | tout `engine/terrain` est dans `namespace render::*` : la couche sim manipule des `render::terraingen::Lake` — dette de nommage assumée dans les en-têtes, mais elle fausse la lecture (Q1-46) |
| Déterminisme / bit-exact du bake (contrat du cache) | PASS | tout CPU, mono-chemin ; **mais** l'identité de cache repose sur des hash manuels (Q1-23) et un memo incomplet (Q1-37) |
| Politique de commentaires 2026-07-22 | **FAIL** | ~130 tags de journal/dates/attributions dans le périmètre (Q1-44) ; les contrats d'en-tête sont, eux, exemplaires |
| Règle dev « enum over nested ifs » | partiel | respectée dans `FireField`, `TerrainBrush`, `SpiritField` ; violée par `puddleMode` (u32 à 3 valeurs), les ids palette/pièce, le triplet de modes `plan × zones` ré-évalué par texel |

---

## Passe 1 — fichier par fichier

Verdicts : **sain** · **à surveiller** (un ou deux findings localisés) · **à refondre**
(structure à reprendre). Les fonctions > 150 l. sont comptées, pas estimées.

### `engine/terrain/generation/` — amont (plan, contrôles, zones, POI, export)

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `TerrainGen.hpp` | 463 | contrat S1 : `GridSpec`, `ControlSample` (27 champs), seam `ControlSource`, `RhythmParams`, `MacroParams`, bordures | à surveiller | Q1-15, Q1-22, Q1-45 |
| `TerrainGen.cpp` | 1 218 | `ProceduralControls::at` (218), `landHeight`, `synthesizeMacro` (176), `applyMapGridShape`, `hashParams` ×2 | **à refondre** | Q1-20, Q1-23, Q1-32, Q1-33, Q1-45 |
| `MapGrid.hpp/.cpp` | 44+89 | `MapEdgeStyle`, style d'un segment de ligne de carte (résolu une fois, mémoïsé) | **sain — le modèle** | seule copie FNV juste |
| `WorldLayer.hpp/.cpp` | 163+551 | couche monde (44 params), `worldSampleAt` (157), archipel, `hashParams` | à surveiller | Q1-14, Q1-23, Q1-36 |
| `MasterNetwork.hpp/.cpp` | 188+708 | étage 0 : réseau régional, memo + cache MNW2, `masterRiversNear` (`std::async`), empreinte des fleuves | à surveiller | Q1-7, Q1-23, Q1-44, Q1-47, Q1-48 |
| `ZonePlan.hpp/.cpp` | 146+716 | zones jitterées, rempart, `zoneSampleAt` (212) : partition, murs, portes, pièce | à surveiller | **Q1-11, Q1-37**, Q1-14, Q1-23 |
| `PoiPlan.hpp/.cpp` | 195+1 239 | `PoiType` (28), tirage par tier, `kernelAt` (196), `planSampleAt` (185), `hashParams` | **à refondre** | **Q1-10**, Q1-23, Q1-36 |
| `MapExport.hpp/.cpp` | 44+350 | `renderTerrainMap` (214), `renderRegionRelief` | à surveiller | Q1-10 (couleurs par plages), Q1-27, Q1-36 |
| `GridOps.hpp` | 143 | voisinage 8, bilinéaire/bicubique carré, blur, chamfer | **sain — la référence** | maison des Q1-24…29 |
| `Authoring.hpp/.cpp` | 69+210 | tampons/crêtes/ancrages (HighMap-like) | sain, **sans appelant** hors tests | Q1-45 |

### `engine/terrain/generation/` — bake, hydrologie, finalize, érosions

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `TileBake.hpp` | 314 | contrat du pipeline d'une carte, `TileBakeParams`, `kTileBakeVersion` | sain (doc d'en-tête de qualité) | Q1-15, Q1-45 |
| `TileBake.cpp` | 2 868 | stage-1 (404), **`extractMapHydrology` (1 923)**, drainage, ground/water par tranche, `bakeSoloTile` | **à refondre** | **Q1-1**, Q1-2, Q1-9, Q1-24, Q1-25, Q1-26, Q1-28, Q1-31, Q1-39, Q1-44, Q1-48 |
| `Hydrology.hpp/.cpp` | 287+1 194 | `Lake`/`River`, flood, perçage (231), extraction (414), étangs, tiers | à surveiller | Q1-13, Q1-24, Q1-26, Q1-28, Q1-45 |
| `Finalize.hpp/.cpp` | 139+1 020 | `finalizeGround` (295), **`finalizeWater` (645)** : S5c carve, S5d-bis lits, S5d étangs, estuaires, S6 masques | **à refondre** | Q1-2, Q1-13, Q1-26, Q1-27, Q1-29, Q1-36, Q1-45 |
| `FluvialErosion.hpp/.cpp` | 173+288 | flood prioritaire, routage, fastscape implicite | **sain — le modèle** du découpage (3 fonctions, une responsabilité) | Q1-36 (eps jitter) |
| `ThermalErosion.*`, `FineErosion.*` | 200+190 | relaxation Jacobi ; incision bornée | sain | namespaces anonymes vides |
| `WaterSolve.hpp/.cpp` | 128+415 | solveur offline à l'équilibre, `solveLevel` (277) | à surveiller | **Q1-21** |

### `engine/terrain/` — runtime et base

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `WaterSim.hpp/.cpp` | 227+1 662 | sim d'eau fenêtrée : `stepWindow` (251), `scrollWindow` (192), `pinLakes` (269), **`extractSnapshot` (422)**, dump WSD | **à refondre** | **Q1-4, Q1-12, Q1-21**, Q1-6, Q1-16, Q1-33, Q1-34, Q1-36, Q1-44 |
| `WaterBodies.hpp/.cpp` | 175+204 | lacs/rivières bakés, requêtes pures, sous-ensemble rect | sain | Q1-33, Q1-35 |
| `WaterQuery.hpp/.cpp` | 42+73 | LA requête gameplay sim ∪ baké (doctrine R3) | sain | Q1-33 (marges) |
| `TerrainBase.hpp` | 250 | région bakée, index spatial, échantillonneurs | à surveiller | Q1-27, Q1-45 (champs eau legacy) |
| `FireField.hpp/.cpp` | 160+472 | champ de feu, `fireStep` (201) | **sain** (enum d'état + switch plat — le modèle) | Q1-16 (stockage u8), Q1-26 |
| `SandboxTerrain.hpp/.cpp` | 76+140 | identité du monde généré, fallback hauteur, probe de spawn | à surveiller | Q1-14, Q1-27 |
| `SpiritField.hpp` | 96 | les 9 esprits, triades (fonction de l'ordinal, zéro table) | **sain** | — |
| `RiverGeometry.*`, `BiomeMap.hpp`, `WindField.*`, `HeightPatches.hpp` | 117+57+82+36 | géométrie de polyligne, biomes, vent, deltas sculpt | sain | — |

### `world/terrain/`

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `TerrainRegions.hpp/.cpp` | 37+268 | IO `.trg` (TRG2/3), builder `TerrainBase` | à surveiller | Q1-34, Q1-22 |
| `MapRecords.hpp/.cpp` | 70+186 | carte bakée → records §5 (`stageMapRecords`, 126) | à surveiller | **Q1-38**, Q1-43 |
| `TerrainBrush.hpp/.cpp` | 63+176 | brosse de sculpt headless (`enum BrushKind` + switch) | sain | Q1-26 |
| `TerrainPatches.hpp/.cpp` | 56+175 | IO `.ter` + records de sculpt | sain | Q1-34 |
| `WaterBodiesBuilder.*`, `BiomeMapBuilder.*` | 150+167 | Forms → `WaterBodies` / `BiomeSet`, IO `.tbm` | à surveiller | Q1-22, Q1-34, Q1-35 |

### Couture `game/` et outils cooker

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `game/MapBaker.hpp/.cpp` | 95+518 | orchestration stage-1 → drainage → sols → hydrologie → eau → overview → manifest ; `bakeMap` (371) | à refondre (.cpp) | **Q1-3**, Q1-23, Q1-30, Q1-31, Q1-33, Q1-34 |
| `game/TerrainBakeStreamer.hpp/.cpp` | 187+823 | lecteur de tranches + un bake de fond ; sidecar `.twb` ; far-water (`collectFarWater` 283) | à refondre | Q1-5, **Q1-8**, Q1-31, Q1-34 |
| `game/TerrainGenTuning.hpp/.cpp` | 56+551 | pupitre `TerrainGenTuningForm` ↔ `TileBakeParams` : `apply` (168) + `capture` (168) **en miroir** | à refondre | **Q1-22**, Q1-23, Q1-33 |
| `tools/cooker/LandscapeReport.cpp` | 1 121 | le rapport de vérité : **`landscapeReport` (908)**, ~14 instruments en blocs anonymes | **à refondre** | Q1-3, Q1-30, Q1-33, Q1-36 |
| `tools/cooker/BakeMapTool.cpp` | 586 | `bake-map` : `bakeMapCmd` (458), divergence, miroir, `--pair`, `--export-plugin` | à refondre | Q1-3, Q1-27, Q1-30, Q1-36, Q1-42 |
| `tools/cooker/PreBakeTool.cpp` | 137 | `pre-bake` sur un rect de cartes | sain hors bootstrap copié | Q1-30 |
| `WaterSolveTool.cpp`, `ErosionBench.cpp` | 232+156 | bancs sur `bakeSoloTile` ; `kVariants[]` = le bon style | à surveiller | **Q1-18**, Q1-36 |
| `WaterReplayTool.cpp`, `TerrainMap.cpp` | 183+55 | replay de dump ; PNG analytique | sain | Q1-36, Q1-42 |
| `tools/cooker/Main.cpp` (dispatch) | — | 15 sous-commandes en `if (command == … && argc …)` | à refondre (G) | Q1-17, Q1-42 |

**Fonctions > 200 l. du périmètre (comptées)** : `extractMapHydrology` 1 923 ·
`landscapeReport` 908 · `finalizeWater` 645 · `bakeMapCmd` 458 · `extractSnapshot` 422
· `extractHydrology` 414 · `bakeTileStage1` 404 · `bakeMap` 371 · `finalizeGround` 295
· `collectFarWater` 283 · `solveLevel` 277 · `pinLakes` 269 · `stepWindow` 251 ·
`bakeSliceWater` 236 · `pierceLakeOutlets` 231 · `at` 218 · `renderTerrainMap` 214 ·
`zoneSampleAt` 212 · `fireStep` 201. Les trois dernières et `at` sont des pipelines
linéaires denses, pas des spaghettis : longueur ≠ finding.

---

## Passe 2 — système par système

Chaque système nomme le **mécanisme existant à étendre** et le croquis du design
général. Les ids renvoient à la table §Findings.

### S1. Le pipeline de bake : des blocs anonymes aux passes nommées (Q1-1, Q1-2, Q1-3, Q1-9)

**Pattern.** `extractMapHydrology` (`TileBake.cpp:479-2402`) : un prélude (fenêtre,
`groundAt`), quatre passes globales (fill, pierce, extract, classify), un second
prélude qui fige `fineAt` et les constantes, puis onze passes de re-nivellement dans
un bloc `{ … }` de 1 700 lignes partageant `rivers`, `floors/bounds/lakeLvls/
oldSurfaces`, `links/order` et cinq compteurs :

| # | `lap()` | lit | écrit | dépend de |
|---|---|---|---|---|
| 4 | recenter | positions, lacs | positions, `fords` | — |
| 5 | retrace | positions | positions | — |
| 6 | relakes | lacs naturels, `fineAt` | `lake.level/mask/cells` | — |
| — | prélude 2 | positions, lacs | `floors`, `bounds`, `lakeLvls`, `oldSurfaces`, `links`, `order` | 4-6 |
| 7 | fill103 | enveloppes | `surface`, `fillShift` | prélude 2 |
| 8 | pools | `surface` | `surface`, lacs (+étangs) | 7 |
| 9 | cascade | `surface`, lacs | `surface` | 8 |
| 10-12 | slope, smooth, narrow | `surface` | `surface`, `halfWidth` | chaîne |
| 13 | clamps | enveloppes, `oldSurfaces`, gués | `surface`, `lake.level`, `fords` | 4 sous-passes indépendantes |
| 14 | saddle | `surface`, `fineAt`, `preGround` | `pierceDelta`, `ground` | 11-13 |

Même structure, en plus court, pour `bakeTileStage1` (sept `lap()` pour treize blocs,
deux laps mal attribués) et `finalizeWater` (S5c → S5d-bis → S5d → estuaires → S6, S6
lisant un snapshot `preCarve`). Côté outils, `landscapeReport` enchaîne ~14 instruments
en blocs `{ … }` qui reconstruisent chacun `ProceduralControls` (6×) et dont les quatre
derniers lisent `longestX/longestZ` calculés 150 lignes plus haut ; `bakeMapCmd` et
`bakeMap` ont la même forme.

**Mécanisme existant.** `FluvialErosion.cpp` (trois fonctions, une responsabilité
chacune, options par pointeur nul = « legacy bit-exact ») ; les noms `lap()` ; la table
`kVariants[]` d'`ErosionBench.cpp` et la découpe `stats()/renderMap()/printStats()` de
`WaterReplayTool.cpp`.

**Design.** Un `struct HydroReleveling` (contexte : `MapHydrology& out`, `fineAt`,
`rivers`, enveloppes, liens, compteurs) et **une fonction par passe** (`recenterCourses`,
`retraceBuriedRuns`, `reseatWindowLakes`, `buildEnvelopes`, `fillProfiles`,
`discoverPools`, `coalesceCascades`, `capSlopes`, `smoothProfiles`, `narrowSteep`,
`reclampTributaries`, `cutSaddles`), appelées par une boucle sur `{ nom, fn }` qui tient
aussi l'horloge (Q1-31). Les enveloppes restent **figées après la passe 6** (le code
actuel ne les recalcule pas après `pools` : c'est l'invariant à écrire). Même traitement
pour `bakeTileStage1`, `finalizeWater` (snapshot `preCarve` conservé), et une
`ReportContext` + table d'instruments pour `landscapeReport`. **Bit-exact : oui** — tout
l'état intermédiaire est déjà matérialisé en `vector<f32>`, aucune passe ne dépend de
l'ordre d'évaluation d'une expression partagée. Preuve : hash `MapBakerTest` +
`landscape-report` inchangés après chaque extraction (une passe par commit).

`bakeSoloTile` vs `bakeMap` (Q1-9) : la duplication est le **contrat d'ordre** (20 l.),
pas la logique ; le risque est une sixième étape ajoutée d'un côté. Un commentaire
croisé suffit, ou `bakeSoloTile` devient `bakeMap(tilesPerSide = 1, sans fichiers)`.

### S2. Les traits par type : des chaînes aux tables (Q1-10, Q1-11, Q1-13, Q1-14, Q1-16)

**Pattern.** `PoiType` (`PoiPlan.cpp`) : prédicats `basinType`/`waterCourseType`
(:170-178), overrides de taille en `if/else if` sur 7 types (:342-360), rétrogradation
à sec `Waterfall ? Butte : Canyon ? Ridge : LoneTree` (:579), `kernelAt` = switch de 11
groupes avec ternaires **par type** à l'intérieur des cases (:646-833), rayon de rejet
(:954), et dans `MapExport.cpp:231-243` la couleur de famille par **plages d'ordinaux**
(`t >= Waterfall && t <= Confluence`) : insérer un type au mauvais rang change sa
couleur. `BorderStyle` (`ZonePlan.cpp`) : `styleFor` :508, largeur `Escarpment ? 1.75f
: 1.0f` :564, pad `st != Ridge` :586, `gateKind` en ternaire imbriqué :602, crête `st ==
Ridge` :617. Tiers de rivière : `tier == 0 ? streamDepthMax : tier == 2 ? fleuveDepthMax
: riverDepthMax` et cinq autres sites (`Finalize.cpp`, `Hydrology.hpp:111`,
`Hydrology.cpp:1105/1130`). Ids de palette 0-9 et de pièce 0-5 : entiers magiques dans
`paletteIdFor`, `characterPalette`, la règle « clairière = 8 » (×4), `pieceAt`, la table
`row(...)` et `ZoneArchetypeForm`. `puddleMode` : u32 « 0 off, 1 hold, 2 storm » consommé
par `!= 0` / `>= 2`. `FireState` : enum correct mais stocké en `vector<u8>` avec 22
`static_cast`.

**Mécanisme existant.** `MapEdgeStyle` + `mapBorderSegmentStyle` (`MapGrid.hpp`) :
décidé en un site, mémoïsé, lu par trois consommateurs — **le modèle**. `poiTypeName`
(tableau + `static_assert(== Count)`), `kCharacters[]`, `defaultZoneArchetypes()`,
`FireField` (enum + switch plat), `SpiritField` (le shifumi est une fonction de
l'ordinal).

**Design.**
- `struct PoiTypeTraits { name; PoiFamily family; bool basin, waterCourse; PoiType
  dryFallback; KernelShape shape; f32 heightCap, radiusCap, fieldCell, fieldHeight; }`
  et `constexpr kPoiTypes[Count]` ; `kernelAt` switche sur **`KernelShape`** (Cone,
  Crater, Mesa, Ridge, Step, Trench, Cirque, Basin, Field, Mound, Pad : 11 formes au
  lieu de 28 types) ; `MapExport` lit `traits.family`.
- `struct BorderTraits { GateKind gate; f32 gateWidthMul; bool pad, crest; }`,
  `constexpr kBorderTraits[4]` ; `styleFor` reste la seule décision ; le 1.75 rejoint
  `ZoneParams` (réglage dev).
- `struct TierProfile { depthMax, shoulder, flatBoost, flowSpeed; bool fords,
  estuaryFan; }` indexé par tier, alimenté par les champs `FinalizeParams` existants
  (ils restent moddables).
- `enum class Palette : u8 { Temperate, Arid, Alpine, Tundra, Subalpine, Steppe,
  DenseForest, Bocage, Clearing, Marsh, Count }`, `enum class ZonePiece`, avec
  `static_assert` sur les tables ; les Forms gardent `u32` tant que la réflexion n'a pas
  de kind Enum (finding Q4), `static_cast` au seul point `resolveZoneArchetypes`.
- `enum class PuddleRelief : u8 { Off, Hold, Storm }` ; `vector<FireState>` au lieu de
  `vector<u8>`.

**Bit-exact : oui** pour tous (mêmes constantes réécrites en table).

### S3. Les paramètres : une définition par champ (Q1-22, Q1-23, Q1-15, Q1-33)

**Pattern.** Un champ de `RhythmParams`/`MacroParams`/`WorldLayerParams`/`ZoneParams`/
`PoiPlanParams` est écrit : (1) dans la struct ; (2) dans son `hashParams` manuel
(`TerrainGen.cpp:1053/1093`, `WorldLayer.cpp:442`, `ZonePlan.cpp:658`,
`PoiPlan.cpp:871` — plus `networkFileKey` qui mélange 15 champs à la main, plus
`mapBakeKey`) ; (3) dans la `*TuningForm` réfléchie de `LandscapeForms.hpp` (1 220 l.,
« Defaults MUST match render::… ») ; (4) dans `applyTerrainGenTuning` **et**
(5) `captureTerrainGenTuning` (`TerrainGenTuning.cpp`, 155 correspondances × 2, « same
order » garanti à la main). Côté `world/terrain`, `WaterBodiesBuilder.cpp:45-55` et
`BiomeMapBuilder.cpp:96-103` copient champ à champ des structs aux **noms identiques**
des deux côtés. Six boucles FNV-1a inline, cinq avec la constante `1469598103934665603`
(un chiffre manquant). `seaLevel` est recopié dans **35 sous-structs** (`grep
'\.seaLevel\s*='`) ; `mapGrid` est rempli à trois endroits ; deux seeds (`worldSeed`,
`controls.seed`) pour une notion, recopiées à la main 8× dans les outils, et
`mapBakeKey` les hache toutes les deux. Les gates « 0 = off » sont une convention
documentée champ par champ mais **hétérogène** (`mouthFlare > 1`, `strength <= 0`,
`high <= 0 = legacy`).

**Mécanisme existant.** `hashTerrainGenTuning` (`TerrainGenTuning.cpp:415`) : hash
**par réflexion** avec `reflect::visit` exhaustif — le bon modèle, isolé. `FieldLane`
(`game/scenes/RenderTuningIo.cpp:12-40`) : table compile-checkée Form ↔ params parcourue
dans les deux sens, écrite précisément après qu'un oubli a « silently stopped saving »
neuf champs. `core/Hash.hpp` (u32 seulement). La réflexion elle-même (§2.3).

**Design**, du minimal au complet :
- (a) `core::Fnv64` (mix bytes/f32/u32/i32/bool) + `hashReflected(const TypeInfo&,
  const void*)` dans `engine/core`/`engine/reflect` ; les six boucles disparaissent.
  Trancher la constante : la corriger = **bump** de toutes les clés de cache (à grouper
  avec un bump prévu) ; la garder sciemment (documentée) = bit-exact.
- (b) `FieldLane` descend dans `engine/reflect` (il n'a besoin que de pointeurs de
  membre : respecte « engine n'inclut pas data ») ; `apply`/`capture` deviennent deux
  boucles de cinq lignes sur `kTerrainGenLanes[]` ; `WaterBodiesBuilder`,
  `BiomeMapBuilder`, `resolveZoneArchetypes` l'utilisent.
- (c) Réfléchir les structs de params eux-mêmes (`REFLECT_FIELD`) : la Form **est** la
  struct ou en dérive, `hashParams` = `hashReflected`, `makeTerrainBakeParams` =
  `copyMatchingFields`. Effort L, touche la frontière avec Q4 : à arbitrer avec le
  finding « kind Enum » de la vague sim.
- `struct BakeEnvironment { u32 seed; f32 seaLevel; }` partagé **par référence** par
  les passes, au lieu de 35 copies ; `TileBakeParams::completeMapGrid()` appelé par les
  trois remplisseurs ; une seule seed.
- Les gates : documenter la convention **une fois** dans `TileBake.hpp` et l'uniformiser
  (0 = off partout), ou promouvoir en `bool` celles qui portent aussi une magnitude.

**Bit-exact :** (b) oui ; (a) et (c) changent les octets hachés → clés de cache = bump à
livrer avec cache ; les gates oui tant que les défauts ne bougent pas.

### S4. Le toolkit de grille : une maison, pas des copies (Q1-24 … Q1-29, Q1-32)

**Pattern.** À côté de `GridOps.hpp` (bilinéaire/bicubique **carré**, blur, chamfer) :
- « point sous un lac » écrit **9×** (`underAnyLake`, `coveringLakeLevel`, `lakeCovers`,
  `underLake`, `underAcceptedLake`, `inFinalLake`, `lakeReachesPoint`,
  `Finalize.cpp:407`, `Hydrology.cpp:1012`) — `Lake` est un POD sans méthode, et une
  copie diverge (Q1-39) ;
- deux priority-floods locaux 8 m copiés (`TileBake.cpp:1240-1366` relakes,
  `:1705-1792` pools) à côté de `priorityFloodFill` ;
- bbox-monde → plage de cellules clampée écrite **6×** dans `Finalize`/`TileBake` et
  4× dans `WaterSim`/`TerrainBrush`/`TerrainBase` ; point → cellule par `lround` 10× ;
  `originX + col * texel` 9× dans `FireField` alors que `GridSpec::x()` existe ;
- quatre échantillonneurs : `bicubicGrid` ≡ `terrain::baseHeight` (texto, sur
  `TerrainRegion` rectangulaire), `bilinearGrid` ≡ `maskSample` (u8), `overviewHeight`,
  deux bilinéaires **rectangulaires** f32 inline (`Finalize.cpp:666`, `TileBake.cpp:575`),
  `fallbackAt` (`BakeMapTool.cpp:297`), `renderRegionRelief` (`MapExport.cpp:289`) —
  la cause est structurelle : `GridSpec` est carré (`n`), `TerrainRegion` est `width ×
  height`, et il n'existe pas de vue commune ;
- géométrie de polyligne (repère perpendiculaire ×3, min d'empreinte ×3, longueur d'arc
  ×5, kink gate ×2, formule de siège ×4, projeté-sur-segment ×6 dont `segmentDistance`,
  `stampRidge`, `imprintMasterChannels`, `WaterSim`, `WaterBodies` ×2, `TerrainBrush`) ;
- pente centrale `|∇h|` ×3 avec deux conventions de bord ;
- warp de domaine / fbm signé : ~60 sites `x + (fbm(…)*2-1)*strength`, deux orthographes
  (`*2-1` vs `(-0.5)*2`, **pas** bit-identiques en flottant) ;
- la même géométrie de notch (`0.15 * inner + 0.7 * lateral`) dans `pierceLakeOutlets`
  et `cutSaddles`, avec deux écritures (ground vs `pierceDelta`).

**Mécanisme existant.** `GridOps.hpp` et `Noise.hpp` — leurs en-têtes disent déjà
« bit-identical replacements for the per-file copies they absorbed » ; `kNeighbours8` ;
`core::simplifyPolylineXz`/`smoothPolyline` (`engine/core/Bezier.hpp`) montrent où
vivent les outils de polyligne.

**Design.** Dans `GridOps.hpp` : `GridView { const f32* data; u32 w, h; f32 originX,
originZ, texel; }` produit par `GridSpec` et `TerrainRegion`, servant `bilinear`,
`bicubic`, `maskSample` ; `GridSpec::cellRange(minX, minZ, maxX, maxZ)` et
`GridSpec::cellOf(x, z)` ; `slopeMagnitude(spec, grid, BorderRule)` ;
`floodBasinBelow(rect, level, seeds, groundAt)` ; `notchBand(...)`. Dans
`Hydrology.hpp` : `bool Lake::covers(x, z) const`, `coveringLakeLevel(lakes, x, z)`. Un
`RiverOps.hpp` : `perpFrame`, `footprintFloor`, `arcLengths`, `hasKinkSharperThan`,
`segmentProject`. Dans `Noise.hpp` : `sfbm` et `warp(seed, saltX, saltZ, x, z, λ, oct,
strength)`. **Bit-exact : oui** à une condition stricte — chaque site garde **son**
expression flottante d'origine (`a + (b-a)*t` ≠ `glm::mix`, `*2-1` ≠ `(-0.5)*2`) ;
unifier l'orthographe = bump. Procéder fichier par fichier avec le hash après chacun.

### S5. L'eau : un noyau, une rasterisation, une extraction (Q1-21, Q1-12, Q1-4, Q1-35)

**Pattern.** Le noyau virtual-pipes existe deux fois à la ligne près — `stepWindow`
(`WaterSim.cpp:172-304`) et `solveLevel` (`WaterSolve.cpp:175-302`) — et le code
l'avoue (« the offline solveLevel kernel verbatim … the WaterSimTest equilibrium
cross-check guards the two copies ») ; les copies ont déjà divergé sur trois points
voulus sans que le code dise lesquels. `pinLakes` (269 l.) et `pinRivers` empilent dix
gardes `if … continue` journalisées « measured dev » ×6 — le profil « rustine par
symptôme ». `extractSnapshot` (422 l.) mêle l'hystérésis de publication (trois jeux de
seuils + `puddleMode` en `if` imbriqués), la connectivité et un marching squares de 16
cas **écrits en code**. `LakeSurface`/`RiverSurface` sont construits à la main à trois
endroits (`WaterBodiesBuilder`, `LandscapeScene.cpp:2591`, `MapView.cpp:93`). Deux
« plausible » (sim vs baké) avec des marges différentes non nommées (`WaterBodies.cpp:12`
vs `WaterQuery.cpp:27`). L'état de la sim est onze `vector` parallèles sans invariant de
taille (sept tests défensifs, un plan oublié au scroll déjà arrivé — le commentaire
`:383` le raconte).

**Mécanisme existant.** `stepWindow` (le noyau le plus riche) ; `WaterSimTest` (le
cross-check) ; `FireField` (`kOffsets[8]` en table, enum + switch) ; `WaterQuery` (la
doctrine R3 dite en un endroit) ; `kWaterInfoDry`/`kDefaultSeaLevel` déjà nommés.

**Design.** `PipeKernelParams { massCap, pinnedCap, borderKeep, sourceSpread }` et un
seul noyau appelé par les deux (`solveLevel` = noyau + test de convergence) ; un
`WaterBodyRaster { covers, level, coreInset, crestGuard }` rasterisé par une passe
générique « corps d'eau → (seed, pin) » suivie d'**un** BFS commun pour lacs et
rivières ; `extractSnapshot` découpé en `publishWetMask` / `publishFields` /
`buildWaterMesh` (fichier `WaterMesh.cpp`) avec `kMsCases[16]` ; `toLakeSurface(const
terraingen::Lake&)` / `toRiverSurface` dans `WaterBodies.hpp` ; `WaterSimState::
forEachPlane()` + `resize(cells)` unique. **Bit-exact :** le noyau et l'extraction oui
(prouvables par cross-check et comparaison de `meshIndices` sur dump) ; **les pins non
garantis** (ordre des max entre lacs) — la sim n'est pas bakée : validation par replay
de dumps (`cooker water-replay`) et jugement visuel dev, pas par hash.

### S6. Caches, formats et memos (Q1-34, Q1-37, Q1-31, Q1-48)

**Pattern.** Huit lecteurs/écrivains binaires maison au même squelette (magic 4 octets,
lambda `write(const auto&)`, « cannot open / not a X / truncated ») : TWB7 (`Lake` ×11
champs, `River` ×6, listés dans les deux sens), MOV1, TRG2/3 (six canaux listés trois
fois avec un cas spécial `rockExposure`), TER1, TBM1, WSD1/2 (`sizeof(Params) -
sizeof(f32)` pour l'ancien format), MNW2 (`reinterpret_cast` du struct, layout = format),
plus le manifest texte lu **par position** (`k2 … k8`). Cinq politiques de memo
thread-local différentes (ZonePlan fingerprint + `LastSample`, PoiPlan nettoyé aux
entrées publiques, MapGrid map nue, WorldLayer trois `thread_local`, MasterNetwork
memo process + disque), dont une **incomplète** (Q1-37) et une **sans borne** (Q1-48).
Le nom de tranche `tile_%d_%d_v%u` est formaté/parsé en 8 sites. Quatre horloges de
phases de trois idiomes.

**Mécanisme existant.** `data/plugins/BinaryFormat.cpp` (codec réfléchi, `reflect::
visit`) ; `world::writeTrgFile/readTrgFile` ; `core::FrameProbe::Scope`.

**Design.** Un `core::BinaryArchive` symétrique (magic, version, `io(T&)` appelé par
write et read — le pattern archive) pour les huit formats ; `sliceStem(tx, tz)` /
`parseSliceStem()` à côté de `kTileBakeVersion` ; un `ParamsMemo<Key, Value>` keyé sur
un hash fourni par l'appelant pour les quatre thread-local, la clé calculée **une fois
par bake** et passée (un `ZoneContext`) plutôt que re-fingerprintée par texel ; un
`core::PhaseClock`. **Bit-exact :** les fichiers changent → à faire au prochain bump
(TWB8/TRG4) avec cache livré ; les memos et horloges : sans effet sur le bake.

### S7. La couture jeu ↔ outils (Q1-30, Q1-17, Q1-18, Q1-8, Q1-7)

**Pattern.** Le bootstrap « pile de plugins du jeu » (`registerAllFormTypes →
loadPluginConfigFile | defaultConfigFromDirectory + "base/" → loadPluginStack →
resolve → resolveLandscapeTuning → makeTerrainBakeParams → setMasterNetworkCacheDir`)
est copié dans `BakeMapTool`, `LandscapeReport`, `PreBakeTool` (+ `EditorScene`,
`TreeCreationScene`) et **diverge** de `LandscapeScene::loadGatedPluginConfig` (gate de
langue). Deux bancs (`water-solve`, `erosion-bench`) bakent sur `TileBakeParams{}` par
défaut, pas sur le monde du jeu — l'inverse du contrat affiché par `TerrainGenTuning.hpp`
(« the game, the cache and the offline bakes can never drift »). Le cooker dispatche 15
sous-commandes par `if (command == … && argc …)`, l'usage dérive (Q1-42), 38
`atoi/atof` sans validation. `TerrainBakeStreamer.hpp` porte trois responsabilités et
inclut `render/landscape/WaterSystem.hpp` (→ `rhi/Rhi.hpp`) pour un POD `FarWaterSet` :
`MapBaker`, `BakeMapTool`, `MapView` et les tests du bake compilent le RHI pour écrire un
fichier binaire. `masterRiversNear` parallélise avec `std::async` (seule occurrence du
dépôt), appelé **depuis** les workers de bake : threads imbriqués hors du `JobSystem`
(règle Phase 5). Le dispatch « enqueue ou inline + file de complétion + sleep 20 ms »
est écrit 6×.

**Mécanisme existant.** `game::registerAllFormTypes` (précédent U8-3 « source unique ») ;
`game::makeTerrainBakeParams` ; la table de scènes de `main.cpp:39-47` et la table de
`ConsolePanel` ; `world::writeTrgFile` (le `.twb` est son jumeau) ; `core::JobSystem`,
`setMasterNetworkCacheDir` (politique injectée).

**Design.** `game::loadGameStack(dataDir, GateOptions)` + `game::bakeParamsFromGameDir
(gameDir)` ; les bancs prennent `<gameDir>` et `--defaults` devient explicite ;
`struct Subcommand { name; minArgs, maxArgs; usage; run }` + table, usage généré, un
`Args` qui échoue proprement ; `.twb` + `clipLakesToMapRect` → `world/terrain/
WaterFiles.hpp`, `FarWaterSet` → `engine/terrain/WaterBodies.hpp`, `collectFarWater` →
`game/FarWater.hpp` découpé en `farLakeFromMask` / `farRibbonFromRiver` /
`masterFallbackRibbons` ; `setMasterNetworkExecutor` ou soumission au `JobSystem` ;
`core::forEachIndexed(jobs, n, fn, onLanded)`. **Bit-exact : oui** (mêmes appels, même
ordre ; le far-water est du rendu : A/B visuel).

### S8. Hygiène (Q1-44 … Q1-47)

~130 commentaires hors politique 2026-07-22 dans le périmètre (`MasterNetwork.hpp` 13-20,
`TileBake.cpp` 16-18, `WaterSim.cpp` 14-19, `Hydrology.hpp` 10-15, `Finalize.cpp` 9,
`LandscapeReport.cpp` 6-7…) : étiquettes de brique (E1b…E9, S4, B5/B6, P3, Z0/Z1,
M1…M5.2, « chantier CARTES v2 », « passe EAU »), dates (« dev bug report 2026-10-08 »,
« audit 2026-10-12 »), attributions (« measured dev » ×12, « the dev reported »),
anciennes valeurs (« enterres 13.6 -> 30.1 % », « −61 % »). À garder : les noms
d'étape S1-S6, les tags de **format** (TRG3, TWB7), les pointeurs `docs/PAYSAGE.md §x`,
et tous les « pourquoi » (les en-têtes sont par ailleurs exemplaires). Code mort :
`ControlSample::trunk/trunkDepth` (jamais écrits — les tests vérifient qu'ils valent
0), `TierLevel::altitude`, `etageAltitudeFor`, `PoiPlan::cellOffset` (renvoie 0, appelé
3×), `finalizeTerrain` (tests seuls), paramètres `spec`/`filled` de `smoothRiver`,
champs eau legacy de `TerrainRegion`/TRG3 (« no writer, no reader », 60 l. d'IO +
validation), `mapBorderStyleResolved` (forwarder à paramètre ignoré, 7 appelants),
`Authoring.*` (279 l. sans appelant de production, parqué depuis juillet),
`waterSolve` sur `TileBakeParams` (lu par la sim runtime de `LandscapeScene`, pas par le
bake : un paramètre runtime dans la clé de cache du bake). Nommage : tout
`engine/terrain` dans `namespace render::*` ; `terrain::height/normal` (vérité CPU)
défini dans `engine/render/landscape/TerrainNoise.cpp` et inclus par ~20 fichiers
gameplay de `game/` ; `RiverHeadType` transporté en `u32` nu à la frontière.

---

## Bugs trouvés au passage (classe B) — aucun corrigé

| id | sév | symbole | effet | correction | bit-exact |
|---|---|---|---|---|---|
| **Q1-37** | moyenne | `paramsKey` fingerprint, `ZonePlan.cpp:49-106` | Le fingerprint « cheap » hache 8 champs monde + 8 par archétype ; `rawCell` lit **tous** les params monde (`seaThreshold`, `continent*`, `massifLift`, `plateau*`…) et `fits()` lit `min/maxMassif`, `min/maxCoast`, `min/maxMoisture` — absents. Une édition de pupitre qui ne change que ceux-là laisse `last.key` inchangé → memo thread-local `memo.cells` **non vidé** → cellules périmées en jeu après un rebake dans la même session (le cache disque, lui, est juste). Le commentaire promet « every scalar ». | fingerprint = `hashParams(world) ^ hashParams(zones)` ou clé calculée une fois par bake (`ZoneContext`) | oui (invalidation seulement) |
| **Q1-38** | moyenne | `Stager::set`, `world/terrain/MapRecords.cpp:39` | `type.findField(field)->id` sans test null : une faute dans l'un des 21 noms de champ littéraux (`"surfaceLevel"`, `"halfWidth"`, …) = déréférencement nul au premier export de carte. Même motif que `DungeonRecords.cpp:103` (Q4). | `ENGINE_ASSERT` + un `data::RecordStager` unique dans `data/plugins/` | n/a |
| Q1-39 | basse | `underLake`, `TileBake.cpp:1908-1916` | Une des neuf copies du test de couverture de lac indexe le masque **sans** le `+ 0.5f` d'arrondi des huit autres : la passe cascade juge au `floor`, pools/clamps au plus proche ; un point à moins d'un demi-texel d'une rive est sec pour l'une, mouillé pour l'autre. Pas de symptôme connu. | `Lake::covers()` unique (Q1-24) | **non → bump** |
| Q1-40 | basse | `biomeIdAt`, `TerrainGen.cpp:689-693` | Troisième copie de la formule « zone lift » **sans** le terme corridor (`storeyHeightSmooth`) : le long d'un corridor, l'id de biome par texel ne lit pas le plancher que `at()` interpole, alors que son commentaire promet « the same sample `at` derives the id from ». | `zoneFloorLift()` unique (Q1-20) ; si la divergence est voulue, la nommer | **non → bump** (ids de biome le long des corridors) |
| Q1-41 | basse | 9 boucles FNV-1a (`TerrainGen.cpp:1039`, `WorldLayer.cpp:443`, `ZonePlan.cpp:50/659`, `PoiPlan.cpp:872`, `MasterNetwork.cpp:132`, `MapBaker.cpp:73`, `TerrainGenTuning.cpp:416`, `VulkanDevice.cpp:725`) | Offset basis `1469598103934665603` au lieu de `14695981039346656037` (un chiffre manquant) ; `MapGrid.cpp:16` seule juste. Inoffensif comme clé de cache (le multiplicateur est juste), preuve du copier-coller. | `core::Fnv64` (Q1-23) ; corriger la constante = **toutes les clés changent** | **non → bump**, ou garder la constante documentée |
| Q1-42 | basse | usage `tools/cooker/Main.cpp:68-83`, dispatch `:418` | L'usage dit `[tilesPerSide=6]` (`kMapTilesPerSide = 2`) ; décrit des flags `carrier*` que `TerrainMap.cpp` ne connaît plus ; `bake-map` plafonné à `argc <= 9` alors que `tilesPerSide -- --export-plugin N --pair X Z` fait 11 → la combinaison complète tombe sur `usage()` ; trois lignes de `pre-bake` rangées sous `landscape-report`. | table de sous-commandes à usage généré (Q1-17) | sans effet |
| Q1-43 | basse | `RiverForm.tier` (`WorldForms.hpp:376`), `stageMapRecords` (`MapRecords.cpp:140`) | Le tier est exporté dans les records mais **jamais relu** : `RiverSurface` n'a pas de `tier`, `WaterBodiesBuilder` ne lit que tint/flowSpeed/material. L'effet du tier survit via `flowSpeed` (dérivé à l'export) ; le tier lui-même est en écriture seule. | soit `RiverSurface::tier` consommé, soit le champ sort du record | n/a |

---

## Findings

Format : `id [classe / sév] symbole (fichier)` — pattern — mécanisme à étendre —
design — effort — bit-exact. Les rapports de lot détaillés (citations complètes,
lignes) sont la source ; ce tableau est la synthèse arbitrable.

### S — structure

- **Q1-1 [S / crit]** `extractMapHydrology` (`TileBake.cpp:479-2402`, 1 923 l.) — quinze
  passes inline dans un bloc de 1 700 l. partageant enveloppes, liens, compteurs et
  `fineAt` — mécanisme : les `lap()` + le modèle `FluvialErosion.cpp` — design : contexte
  `HydroReleveling` + une fonction par passe + boucle `{nom, fn}` (table §S1) — **L** —
  bit-exact **oui** (preuve par hash après chaque passe extraite).
- **Q1-2 [S / haute]** `finalizeWater` (645) et `bakeTileStage1` (404) — sections
  S5c/S5d-bis/S5d/estuaires/S6 et treize blocs sous sept laps (deux mal attribués) —
  même design que Q1-1, snapshot `preCarve` conservé — **M** — oui.
- **Q1-3 [S / haute]** `landscapeReport` (908), `bakeMapCmd` (458), `bakeMap` (371) —
  blocs anonymes, `ProceduralControls` reconstruit 6×, état coulant (`longestX/Z`) —
  mécanisme : `ErosionBench::kVariants[]`, `WaterReplayTool` — design : `ReportContext`
  + table d'instruments ; `bakeMapCmd` en 4 étapes nommées ; `bakeMap` sur
  `forEachSlice` (Q1-31) — **M** — oui (`cp.seed = worldSeed` à conserver, cf. Q1-33).
- **Q1-4 [S / haute]** `extractSnapshot` (`WaterSim.cpp:971-1392`, 422) — hystérésis +
  connectivité + marching squares de 16 cas en code — mécanisme : `FireField::kOffsets`
  — design : `publishWetMask` / `publishFields` / `buildWaterMesh` + `kMsCases[16]` —
  **M** — oui (comparer `meshIndices` sur dump).
- **Q1-5 [S / moyenne]** `collectFarWater` (`TerrainBakeStreamer.cpp:538-821`, 283) —
  cinq étapes, trois lambdas captives, six constantes en dur — design : trois fonctions
  pures (§S7) — **M** — rendu far : A/B visuel.
- **Q1-6 [S / moyenne]** `WaterSimState` — onze plans parallèles sans invariant, sept
  tests `size() != cells()`, `planes[]` local au scroll — design : `forEachPlane()` +
  `resize()` unique — **S** — oui.
- **Q1-7 [S / moyenne]** `masterRiversNear` (`MasterNetwork.cpp:540-552`) — `std::async`
  par super-cellule, appelé depuis les workers de bake (threads imbriqués) — mécanisme :
  `core::JobSystem`, `setMasterNetworkCacheDir` — design : exécuteur injecté ou
  soumission au JobSystem — **S-M** — oui.
- **Q1-8 [S / moyenne]** `TerrainBakeStreamer.hpp` — sidecar TWB + far-water + streamer
  ; inclut `WaterSystem.hpp` → RHI dans la chaîne bake/cooker/tests — design : `.twb` →
  `world/terrain/WaterFiles.hpp`, `FarWaterSet` → `WaterBodies.hpp`, far-water →
  `game/FarWater.hpp` — **S** — sans effet.
- **Q1-9 [S / basse]** `bakeSoloTile` vs `game::bakeMap` — même séquence de cinq appels
  écrite deux fois (contrat d'ordre, 20 l.) ; risque de sixième étape unilatérale —
  design : commentaire croisé, ou `bakeMap(tilesPerSide = 1)` sans fichiers — **S/M** —
  oui.

### G — généraliser

- **Q1-10 [G / haute]** `PoiType` (`PoiPlan.cpp:170, 342, 579, 646-833, 954` ;
  `MapExport.cpp:231`) — six chaînes de traits + couleur par plages d'ordinaux —
  mécanisme : `poiTypeName`, `kCharacters[]` — design : `PoiTypeTraits[Count]` +
  `KernelShape` (11 formes) — **M** — oui (vérifier `PoiPlanTest`).
- **Q1-11 [G / haute]** `BorderStyle` (`ZonePlan.cpp:508, 564, 586, 602, 617`) — cinq
  sites — mécanisme : `MapEdgeStyle`/`mapBorderSegmentStyle` — design :
  `kBorderTraits[4]`, 1.75 → `ZoneParams` — **S** — oui.
- **Q1-12 [G / haute]** `pinLakes` (269) / `pinRivers` (`WaterSim.cpp`) — dix gardes
  empilées par symptôme (« measured dev » ×6), deux rasterisations bbox → cellules —
  design : `WaterBodyRaster` + passe générique + un BFS — **L** — **non garanti** :
  replay de dumps + jugement dev.
- **Q1-13 [G / moyenne]** tiers de rivière (`Finalize.cpp:435, 498, 793` ;
  `Hydrology.hpp:111` ; `Hydrology.cpp:1105, 1130`) — six ternaires — design :
  `TierProfile` indexé, alimenté par `FinalizeParams` — **S** — oui.
- **Q1-14 [G / moyenne]** ids palette 0-9 et pièce 0-5 (`paletteIdFor`,
  `characterPalette`, clairière = 8 ×4, `pieceAt`, `row()`, `ZoneArchetypeForm`) +
  spawn `biome == 0 || 7 || 8` (`SandboxTerrain.cpp:63`) — design : `enum class Palette`,
  `enum class ZonePiece` + `static_assert`, `spawnable`/`gameplayTag` sur `BiomeForm` —
  **S** — oui.
- **Q1-15 [G / moyenne]** gates « 0 = off » (`keepCrestFade`, `fineSlopeReturn`,
  `lakeDepthCoef`, `mouthFlare > 1`, `strength <= 0`, `high <= 0 = legacy`, 16 sites
  documentés) — convention implicite et hétérogène, invisible au pupitre — design : la
  documenter une fois, l'uniformiser, ou `bool` pour les gates à magnitude — **S** — oui
  tant que les défauts ne bougent pas.
- **Q1-16 [G / moyenne]** `puddleMode` u32 (`WaterSim.hpp:166`, `.cpp:1009-1018`) ;
  `vector<u8> state` + 22 `static_cast<u8>(FireState::…)` — règle « enum over nested
  ifs » — design : `enum class PuddleRelief`, `vector<FireState>` — **S** — oui.
- **Q1-17 [G / moyenne]** `tools/cooker/Main.cpp:363-420` — quinze `if (command == … &&
  argc …)`, usage à part, 38 `atoi/atof` — mécanisme : table de scènes `main.cpp`,
  `ConsolePanel` — design : `Subcommand[]` + usage généré + `Args` — **M** — sans effet.
- **Q1-18 [G / moyenne]** `WaterSolveTool`, `ErosionBench` — `TileBakeParams{}` par
  défaut, pas la pile du jeu — design : `<gameDir>` + `--defaults` après Q1-30 — **S**.
- **Q1-19 [G / basse]** idiome « dig-only borné » `h = min(h, max(min(h, cible),
  floorCut))` ×4 dans `finalizeWater` — design : `digBounded()` + invariant écrit une
  fois — **S** — oui.

### D — duplication

- **Q1-20 [D / haute]** `ProceduralControls::at` / `refineFloor` / `biomeIdAt`
  (`TerrainGen.cpp:426-710`) — zone-lift ×3 (une divergente, Q1-40), `shoreGate` ×3,
  clairière ×4, gate massif ×4, trois arbres `if (plan && zones) / else if (plan) /
  else` par texel — mécanisme : seam `ControlSource` — design : `floorAt()` unique +
  `enum class LayerMode` évalué au constructeur — **M** — oui si la variante est
  conservée.
- **Q1-21 [D / haute]** noyau virtual-pipes `stepWindow` (`WaterSim.cpp:172-304`) ≡
  `solveLevel` (`WaterSolve.cpp:175-302`), trois divergences voulues non déclarées —
  design : `PipeKernelParams` + un noyau — **M** — oui (cross-check `WaterSimTest` +
  replay).
- **Q1-22 [D / haute]** pupitre ↔ params : `applyTerrainGenTuning` / `capture…`
  (`TerrainGenTuning.cpp:28-366`, 155 × 2 en miroir, « same order » manuel ; un oubli
  dans `capture` = hash de cache aveugle) ; `resolveZoneArchetypes` (24 champs) ;
  `WaterBodiesBuilder.cpp:45`, `BiomeMapBuilder.cpp:96` (noms identiques des deux côtés)
  ; `LandscapeForms.hpp` « Defaults MUST match » — mécanisme : `FieldLane`
  (`RenderTuningIo.cpp`) — design : `FieldLane` dans `engine/reflect`, tables de lanes ;
  à terme réfléchir les params (S3-c, avec Q4) — **M** (lanes) / **L** (réflexion) — oui.
- **Q1-23 [D / haute]** hash de params : six FNV inline (constante fausse, Q1-41), cinq
  `hashParams` champ par champ, `networkFileKey` (15 champs à la main, seule struct sans
  `hashParams`), `mapBakeKey` — mécanisme : `hashTerrainGenTuning` (par réflexion) —
  design : `core::Fnv64` + `hashReflected` — **S** (Fnv) / **M** (réflexion) — **non**
  (clés) → bump groupé.
- **Q1-24 [D / haute]** « point sous un lac » ×9 (`TileBake.cpp:758, 1378, 1647, 1901,
  2274, 2715, 2822` ; `Finalize.cpp:407` ; `Hydrology.cpp:1012`) — design :
  `Lake::covers()`, `coveringLakeLevel()` — **S** — oui ×8, Q1-39 à aligner (bump).
- **Q1-25 [D / haute]** flood local 8 m ×2 (`TileBake.cpp:1240-1366`, `:1705-1792`) —
  mécanisme : `priorityFloodFill` — design : `floodBasinBelow()` dans `GridOps.hpp` —
  **M** — oui (ordre des 4 voisins et `g >= level - 0.1f` conservés).
- **Q1-26 [D / moyenne]** bbox → cellules ×10 (`Finalize.cpp:471, 652, 733` ;
  `TileBake.cpp:1026, 1248, 1724` ; `WaterSim.cpp:578, 872` ; `TerrainBrush.cpp:36,
  107`), `lround` monde → cellule ×10, `originX + col * texel` ×9 (`FireField`) —
  design : `GridSpec::cellRange()`, `cellOf()` ; `GridSpec::x()/z()` existants — **S** —
  oui.
- **Q1-27 [D / moyenne]** échantillonneurs : `bicubicGrid` ≡ `baseHeight`
  (`TerrainBase.hpp:180`), `bilinearGrid` ≡ `maskSample` (`:220`), `overviewHeight`
  (`SandboxTerrain.cpp:15`), rect inline (`Finalize.cpp:666`, `TileBake.cpp:575`),
  `fallbackAt` (`BakeMapTool.cpp:297`), `renderRegionRelief` (`MapExport.cpp:289`) —
  cause : `GridSpec` carré vs `TerrainRegion` rectangulaire — design : `GridView` dans
  `GridOps.hpp` — **M** — oui **si** chaque site garde son expression (`glm::mix` ≠ `a +
  (b-a)*t`).
- **Q1-28 [D / moyenne]** géométrie de polyligne : repère ⊥ ×3, min d'empreinte ×3,
  arc ×5, kink ×2, siège ×4 (`extractMapHydrology`), projeté-sur-segment ×6
  (`PoiPlan.cpp:839`, `Authoring.cpp:110`, `MasterNetwork.cpp:643`, `WaterSim.cpp:902`,
  `WaterBodies.cpp:29/151`, `TerrainBrush.cpp:123`), notch ×2 (`Hydrology.cpp:561`,
  `TileBake.cpp:2333`) — mécanisme : `core/Bezier.hpp` — design : `RiverOps.hpp` +
  `segmentProject` + `notchBand` — **M** — oui.
- **Q1-29 [D / moyenne]** pente centrale ×3 (`Finalize.cpp:108, 219, 861`, deux
  conventions de bord) — design : `slopeMagnitude(spec, grid, BorderRule)` — **S** — oui.
- **Q1-30 [D / moyenne]** bootstrap pile de plugins ×6 (`BakeMapTool.cpp:164`,
  `LandscapeReport.cpp:217`, `PreBakeTool.cpp:55`, `EditorScene.cpp:136`,
  `TreeCreationScene.cpp:45` vs `LandscapeScene::loadGatedPluginConfig`) — divergence
  de gate de langue — mécanisme : `registerAllFormTypes` (U8-3) — design :
  `game::loadGameStack()` + `bakeParamsFromGameDir()` — **S** — oui.
- **Q1-31 [D / moyenne]** horloge de phases (`TileBake.cpp:88` ≡ `:486` ; variantes
  `Hydrology.cpp:408`, `TerrainGen.cpp:733`, `MapBaker.cpp:178/198/244`,
  `LandscapeReport`) ; dispatch « enqueue ou inline + drainage + sleep » ×6
  (`MapBaker.cpp:202, 309` ; `TerrainBakeStreamer.cpp:251, 298, 345, 371`) — mécanisme
  : `FrameProbe::Scope`, `JobSystem` — design : `core::PhaseClock`,
  `core::forEachIndexed()` — **S** — sans effet.
- **Q1-32 [D / moyenne]** warp de domaine / fbm signé ~60 sites, deux orthographes —
  mécanisme : `Noise.hpp` — design : `noise::sfbm`, `noise::warp` — **S** (mécanique) —
  oui si chaque site garde son orthographe (sinon bump) ; hash après chaque fichier.
- **Q1-33 [D / moyenne]** `seaLevel` recopié dans **35 sous-structs** (`\.seaLevel\s*=`),
  marges `seaLevel + 0.01/0.25/0.5/1/2/3/8/26` en 11 fichiers (dont trois valeurs
  différentes de « sous la mer » dans `TerrainBakeStreamer.cpp`), deux « plausible »
  (`WaterBodies.cpp:12` vs `WaterQuery.cpp:27`), deux seeds (`worldSeed` /
  `controls.seed`, recopiées 8× dans les outils, hachées toutes deux par `mapBakeKey`),
  `mapGrid` rempli 3× (`TerrainGenTuning.cpp:464`, `MapBaker.cpp:173`,
  `TerrainBakeStreamer.cpp:208`), `MacroParams::hillChainWavelength/bedWavelength`
  copiés depuis `rhythm` par chaque appelant (8 sites) — design : `BakeEnvironment {
  seed, seaLevel }` par référence, constantes nommées (`kShoreGate*`, `kCarveWetFactor`,
  `kMaxCurrentSpeed`…), `completeMapGrid()`, `synthesizeMacro` lit `rhythm` — **M** — oui
  (valeurs égales) ; le hash de `MacroParams` change → bump.
- **Q1-34 [D / moyenne]** huit formats binaires maison (TWB7 `TerrainBakeStreamer.cpp:31/
  79`, MOV1 + manifest par position `MapBaker.cpp:40/102/465/490`, TRG2/3
  `TerrainRegions.cpp` canaux ×3, TER1, TBM1, WSD1/2 `WaterSim.cpp:1434`, MNW2
  `reinterpret_cast`) ; `tile_%d_%d_v%u` en 8 sites — mécanisme : `BinaryFormat.cpp`,
  `writeTrgFile` — design : `core::BinaryArchive` symétrique, `sliceStem()` — **M** —
  fichiers changent → au prochain bump.
- **Q1-35 [D / moyenne]** `LakeSurface`/`RiverSurface` construits ×3
  (`WaterBodiesBuilder.cpp:66/109`, `LandscapeScene.cpp:2591`, `MapView.cpp:93`) —
  design : `toLakeSurface()`/`toRiverSurface()` dans `WaterBodies.hpp` — **S** — oui.
- **Q1-36 [D / basse]** petites duplications, chacune **S**, toutes bit-exactes ou sans
  effet : (a) disque de source 5×5 ×3 dans `stepWindow`, recalculé à chaque substep ;
  (b) `etageRaw` vs début de `worldSampleAt` ; (c) nearest-3 + blend de caractère ×2
  (`planSampleAt`, `planCharacterAt`) ; (d) classification gate/wall/ridge ×2
  (`MapExport.cpp:118`, `LandscapeReport.cpp:782`) → `zoneFeatureOf()` ; (e) hillshade
  PNG ×3, `styleName(MapEdgeStyle)` ×2 → `mapEdgeStyleName()` ; (f) grille d'epsilon
  jitter ×2 (`Hydrology.cpp:23`, `FluvialErosion.cpp:165`) ; (g) cinq `resolveXxxTuning`
  identiques (`LandscapeForms.cpp:44-82`) → `resolveSingleton<T>` ; (h) `kKinkGateCos`
  déclaré 2×, trois noms pour le texel 8 m (`kCellT`, `kLakeTexel`, `kPoolMaskTexel`),
  `320.0f` ×2, offsets d'empreinte ×2 + variante ; (i) `0x9e3779b9u` ×12 →
  `core::hashCombine`, `3.14159265f` ×2, `0.017453292f` ×5 → `glm::radians` ; (j)
  `kMaxSamples` 8192 ×2 + `4096` en dur ; (k) `kZoneHue[14]` et `% 14` couplés au nombre
  d'archétypes par défaut.

### H — hygiène

- **Q1-44 [H / moyenne]** ~130 commentaires hors politique 2026-07-22 (détail §S8) —
  **S** par fichier, sans build — sans effet.
- **Q1-45 [H / moyenne]** code mort et vestiges (détail §S8) : `trunk/trunkDepth`,
  `TierLevel::altitude`, `etageAltitudeFor`, `cellOffset`, `finalizeTerrain` (tests),
  `smoothRiver(spec, filled)`, champs eau TRG3, `mapBorderStyleResolved`, `Authoring.*`
  (à inscrire dans `PAYSAGE.md` §5 ou brancher), `waterSolve` sur `TileBakeParams`,
  namespaces anonymes vides — **S** chacun — TRG4 = bump ; le reste sans effet.
- **Q1-46 [H / moyenne]** `engine/terrain` dans `namespace render::*` (dette assumée
  dans `TerrainBase.hpp:15`, `HeightPatches.hpp:11`) ; `terrain::height/normal` dans
  `engine/render/landscape/TerrainNoise.cpp` inclus par ~20 fichiers de `game/` ;
  `layers_audit.py` ne distingue pas — design : `namespace terrain` racine + alias
  `render::terrain = ::terrain` le temps de la migration ; CLAUDE.md §4 à corriger
  (« GL seul backend », ordre world/gameplay) — **M** (mécanique) — sans effet.
- **Q1-47 [H / basse]** `RiverHeadType` transporté en `u32 headType` nu
  (`WaterBodies.hpp:99`, `Hydrology.hpp:138`, `MasterNode`) et re-casté ; MNW2 écrit
  par `reinterpret_cast` du struct sans `static_assert(sizeof == 28)` — **S** — sans effet.

### P — optimisation évidente

- **Q1-48 [P / basse]** `TileStage1::macroHeight/budget` (« never persisted », 2 × 1921²
  f32 ≈ 29 Mo) retenus pendant les 36 tranches ; `gNetworkMemo` ajoute un réseau par
  hash de pupitre sans jamais évincer ; disque de source recalculé par substep (Q1-36a)
  — **S** — sans effet.

---

## Positifs à préserver

- `GridOps.hpp`, `Noise.hpp`, `FluvialErosion.cpp` : la **maison** et le **modèle** de
  découpage ; les findings D y vont naturellement.
- `MapEdgeStyle` / `mapBorderSegmentStyle` : décidé une fois, mémoïsé, lu par trois
  consommateurs — le modèle pour Q1-10/Q1-11.
- `FireField` (enum + switch plat, `kOffsets[8]`, scratch `dealt`), `SpiritField`
  (shifumi = fonction de l'ordinal), `TerrainBrush` (`BrushKind` + switch) : la règle
  « enum over nested ifs » appliquée.
- `hashTerrainGenTuning` : hash par réflexion exhaustif — à généraliser, pas à imiter.
- `MapBaker.hpp` : contrats dans l'en-tête avec leurs pourquoi (clé complète vs
  seed-only, manifest écrit en dernier) ; `mapBakedAndValid` compare la clé, pas
  l'existence ; `saveTerrainGenTuning` = un plugin §5 ordinaire (invariant §2.4).
- `WaterQuery` (doctrine R3 en 73 lignes), `WaterBodiesSubset` (bit-identique au scan
  complet, documenté), `clipLakesToMapRect` (une autorité par point), le contrat
  `Lake::dug`, la gestion `cancel` (résultat partiel jamais publié), `MasterNetwork`
  (ownership par super-cellule, écriture atomique tmp + rename).
- Les paramètres portent presque tous leur **pourquoi** dans le `.hpp` ; les sels de
  bruit sont nommés un par champ ; la détermination est structurelle.
- `ErosionBench::kVariants[]`, `WaterReplayTool` : le bon style d'outil.

---

## Ordre de correction proposé (à arbitrer)

Un commit par brique ; après chaque brique terrain : build Release, `cooker
landscape-report`, hash `MapBakerTest` inchangé (ou bump explicite avec cache livré).

1. **Fondations bit-exactes, sans bump** (une session) : Q1-24 `Lake::covers` (en
   gardant la variante Q1-39 nommée), Q1-26 `cellRange/cellOf`, Q1-29, Q1-31
   `PhaseClock`, Q1-35, Q1-36 (a-k), Q1-19, Q1-13 `TierProfile`, Q1-11 `BorderTraits`,
   Q1-16 enums, Q1-6 `forEachPlane`. Chaque brique est petite et prouvable.
2. **Le pipeline en passes nommées** (deux sessions) : Q1-1 passe par passe (14
   commits), puis Q1-2, puis Q1-3 (`landscapeReport` en instruments, `bakeMapCmd`,
   `bakeMap`), Q1-25 `floodBasinBelow` et Q1-28 `RiverOps` au fil des extractions.
3. **Traits et tables** : Q1-10 `PoiTypeTraits` + `KernelShape`, Q1-14 enums palette/
   pièce, Q1-20 `floorAt` + `LayerMode` (décision dev sur Q1-40 d'abord).
4. **Paramètres, une définition** : Q1-22 `FieldLane` dans `engine/reflect` (le plus
   gros gain mécanique du périmètre), Q1-30 `loadGameStack` puis Q1-18, Q1-33
   `BakeEnvironment`, Q1-15 gates. **Puis un bump unique** portant Q1-23 (Fnv64 +
   constante corrigée), Q1-39, Q1-40, Q1-45 TRG4, Q1-34 archive — cache livré.
5. **Eau** (sessions dédiées, validation par replay + dev) : Q1-21 noyau unique, Q1-4
   `extractSnapshot`, Q1-12 pins (le seul chantier non bit-exact : après Q1-21, avec
   dumps de référence capturés avant).
6. **Couture et outils** : Q1-17 table de sous-commandes, Q1-8 `WaterFiles.hpp`, Q1-5,
   Q1-7 exécuteur, Q1-9.
7. **Hygiène** au fil de l'eau : Q1-44 par fichier touché, Q1-45, Q1-46 (migration de
   namespace mécanique, une session), Q1-47, Q1-48.

Les bugs Q1-37 et Q1-38 sont corrigeables tout de suite, indépendamment de l'ordre.
