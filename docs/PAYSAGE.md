# PAYSAGE — terrain, eau, sol, végétation, peuplement : état, décisions, leçons, chantier

> **LE document de référence du paysage généré** (génération du relief,
> cartes bornées, eau, matériaux du sol, scatter/végétation/arbres,
> peuplement) et **le journal du chantier PAYSAGE** (courant depuis le
> 2026-10-05). Il consolide six journaux (précédent : `docs/RENDERING.md`,
> 2026-07-26) ; les journaux bruts sont conservés sous `docs/archive/`.
> Ce fichier tient *l'état, les décisions avec leur pourquoi, les leçons
> durables, l'état des lieux total du 2026-10-05 et ce qui reste*.
> À lire avant de toucher `engine/terrain/`, `world/terrain/`,
> `game/MapBaker`, `game/TerrainBakeStreamer`, `engine/render/landscape/
> {Terrain*,Water*,Vegetation*,Grass*,Tree*,FarTerrain}`, ou le panneau
> « Terrain generation ». Le chantier ESPRITS (magie, manipulation du
> monde) garde son propre journal : `docs/CHANTIER-ESPRITS.md`.

## 0. Carte d'archive

| Journal archivé (`docs/archive/`) | Ce qu'il tenait | Repris en |
|---|---|---|
| `TERRAIN-GEN.md` (1 599 l.) | génération v1/v2 (2026-07-31/08-01), cible « Paysage & peuplement » validée 2026-08-24 et briques B0-B9f, matériaux M1-M4, refonte eau W1-W4 / C0-C3 | §1.2-1.4, §2.1, §3.1-3.3, §4, §6 |
| `TERRAIN-MAPS.md` (317 l.) | chantier CARTES (monde = graphe de cartes bornées, M1-M5, bordures v2), clos 2026-09-26 | §1.1, §2.2, §3.4, §5.1 |
| `WATER-RESEARCH.md` (196 l.) | l'étude des solutions d'eau A-E (2026-08-26) | §2.3 (issue : C, pas D) |
| `WATER-RENDER.md` (389 l.) | architecture de l'eau simulée (option C), le grand livre des leçons §3, chantier EAU E1-E7 clos 2026-08-31, backlog | §1.5, §2.3, §3.5, §6.3 |
| `TERRAIN-TEXTURING.md` (≈300 l.) | matériaux/texturing du terrain (2026-08-02/03) | §1.6, §2.4, §3.6 |
| `GRASS-REDO.md` (545 l.) | sol réaliste : espèces d'herbe, hex-tiling, scans, écorce, SSAO/SSDM, éboulis, audit perf (2026-08-03→06) | §1.7, §2.5, §3.7 |

Les numéros de briques cités par d'anciens commentaires de code (B9e,
E4a, M1.5b, W2, H5…) renvoient aux journaux archivés ; tout ce qui porte
encore est ci-dessous. Doc utilisateur/moddeur : `userdoc/maps.md`.

---

## 1. Le monde tel qu'il est construit

### 1.1 Le monde = un graphe de cartes bornées (chantier CARTES, clos 2026-09-26)

- Une **carte** = un carré de **24 576 m** (6 × 6 tranches de 4 096 m),
  un `WorldspaceForm` (`bounded`, `mapX`, `mapZ`), érodée **une fois,
  globalement** (zéro frontière interne : divergence mesurée **0,281 m**
  entre tranches, critère ≤ 2,5 m), découpée ensuite en tranches `.trg`
  pour le streamer. La carte (i, j) est seedée par ses coordonnées et
  bakée en fond à la première approche ; une carte peut aussi être
  exportée en **mod ordinaire §5** (`cooker bake-map --export-plugin`,
  bouton éditeur) et retouchée.
- **Le chemin « sandbox infini fenêtré par tuile » n'existe plus** (M1.5b,
  2026-09-26) : `bakeTile`/`bakeTileStage2`, composite 3×3, résolution
  canonique des bassins, cache stage-1/`Stage1Registry`, `border-report`,
  `map-proto` sont supprimés. `bakeSoloTile` = carte 1×1 tranche, banc et
  tests seulement. Le mot « sandbox » survit dans les noms
  (`SandboxTerrain`, `sandboxTerrain`, `sandboxLakes/Rivers`…) pour dire
  « monde généré » par opposition au mode **story** (bruit démo +
  `village.toml`, toujours vivant : second générateur de hauteur).
- **Transitions de bordure v2** (`TerrainGen.cpp`, `MapGridSpec`,
  `applyMapGridShape`, `mapBorderStyleResolved`) : chaque LIGNE de
  frontière est hachée par (seed, identité de ligne) en **Mer** ou
  **Montagnes** ; la ligne serpente (`kMapBorderWander` ±900 m / 9 km) ;
  montagne = chaîne progressive des deux côtés (`kMapBorderMountainHalf`
  2 560 m, lift 620 m, keep d'érosion 0,8) **seulement sur la terre** ;
  mer = bras creusé `min(h, sea − 40)` sur 2 048 m, îlots = terre noyée ;
  cross-fade des styles aux coins sur 1,8 km ; **veto Mer** (< 34 % des
  9 sondes sous la mer ⇒ Montagnes). Une seule fonction pure partagée
  par le bake, le fallback analytique, l'overview et le far-water.
- Voyage : `travelToMap(mx, mz [, x, z])` (C++, Lua, console `map`,
  triggers de col) → `LandscapeScene::applyMapWorld` = la transaction de
  swap ; `base/passes.toml` = **un** col posé à la main (Col de l'Est).
  Carte active dans les saves (`WorldStateForm.sandboxMap/activeMapX/Z`).
  Écran M borné (`MapController`).

### 1.2 La seam runtime (contrat des consommateurs, inchangé depuis v1)

```
height(x,z) = Σ wᵢ·(bicubic(régionᵢ) + détail) / Σ wᵢ   régions bakées 2 m, blend par poids de bord (edgeBlend 128 m)
              ↘ fondu vers proceduralBase(x,z)           = overview 64 m si couvert, sinon macroHeightAnalytic + applyMapGridShape
            + authoredDelta(patches)                     = sculpt (.ter), TOUJOURS le dernier terme, jamais aplati
```

- `TerrainParams` porte quatre pointeurs partagés immutables : `patches`
  (sculpt), `base` (`TerrainBase` = régions `.trg`), `sandbox` (contrôles,
  macro, grille de carte, overview), `biomes`, plus `water` (exclusion du
  scatter). Tout consommateur (collision Jolt heightfield 1 m, mesh
  5 LOD, lightmap, shade map, FarTerrain 18 km, raster de carte, scatter,
  sim d'eau) lit cette seule fonction. Bicubique Catmull-Rom obligatoire
  (le bilinéaire facette les normales).
- Modding en trois étages : (1) `TerrainPatchForm` + `.ter` par chunk
  64 m ; (2) patch field-level (`TerrainRegionForm.detail*`,
  `WaterBodyForm.surfaceLevel`, `BiomeForm.*`, `WaterMaterialForm`) ;
  (3) remplacement d'asset par GUID (`.trg`, `.tbm`). Les saves ne
  portent pas de terrain, sauf les `.ter` du sculpt d'esprit (E2.b).

### 1.3 Le pipeline de bake d'une carte (`game::bakeMap`, headless `engine/terrain/generation/`)

```
[étage 0] computeMasterNetwork — 128 m, super-cellule = LA carte (24 576 + apron 8 192 m = 321²), sur macroHeightAnalytic, mémo process
[stage 1] bakeTileStage1 — 16 m, carte + apron kMapApron 3 072 m = 1921² (~80 s)
   synthesizeMacro (contrôles sur lattice 64 m, biome par texel) → applyMapGridShape → imprintMasterChannels (fleuves CONSTRUITS avant érosion)
   → modulations biome/gentle/plaine/lithologie/calm → grille keep (plateau·0,0008 + calmHigh·0,25 ≤ 0,5, fondu de crête, keep bordure 0,8)
   → erodeFluvial (fastscape implicite, 80 it., dépôt à capacité, channel keep) → erodeThermal (60 it.) → roundRidges → relaxation calme → calm ∪ fonds de vallée
[hydrologie] extractMapHydrology — UNE fois par carte, 16 m, carte + 1 024 m = 1665² (~2 s)
   priorityFloodFill → routeFlow → lacs (minLakeDepth 0,6 / minLakeCells 12) → rivières (riverArea 1,5e5 m²) → étangs → classifyRivers (tiers + gués)
[tranches ×36] bakeMapSlice — workers, 4 096 + 64 + 192 m, 2 m (~12 s)
   finalizeTerrain : bicubique 16→4→2 m, amplifyFine (érosion fine 4 m, keep+halo 192 m), micro-thermique, relief 42 m, carve des lits par tier
   (0,5 / 4 / 7 m, gués 0,4 m), lits de lacs, étangs, masques u8 16 m (flow, wetness, beach, detailAmp, biome, rockExposure)
   crop → lacs (propriété = centre de bbox) → reconcileLakesWithTerrain v2 (par composante) → rivières clippées → reconcileRiversWithTerrain
[publication] tile_<tx>_<tz>_v68.trg (TRG3) + .twb (TWB3) + overview.bin (MOV1, 64 m) + manifest.txt (écrit EN DERNIER)
[runtime] TerrainBakeStreamer = lecteur de tranches + orchestrateur d'UN bakeMap de fond ; prefetch 1 408 m ; far-water depuis les .twb
```

- **Sortie publiée** : hauteurs 2 m (2113² par tranche, ~18 Mo résidents)
  + masques 16 m ; lacs (niveau + masque + `dug`) ; rivières (nœuds
  x/z/surface/demi-largeur, `tier` 0/1/2, `fords`). Les grilles stage-1
  `calm`/`gentle`/`trunk`/`deposit`/`hardness` sont **jetées** après le
  bake (voir §5.3).
- **Versions** : `kTileBakeVersion` **68** (seul invalidant réel : nom de
  fichier + manifest) ; `kMapBakeVersion` 1 ; `kStage1Version` 46 est un
  **orphelin** (aucun lecteur). Clé de cache = seed seule : changer
  `seaLevel`/`terrainRecurve*` n'invalide rien → **vider `terrain-cache`**.
- Déterminisme pour (params, carte) = contrat du cache et des tests ;
  tout est CPU (décision 2026-08-01), mono-chemin, bit-exact entre
  machines (sauf flottants MSVC vs clang, §3.3).

### 1.4 Les contrôles analytiques (`ProceduralControls::at`, fonction pure de la seed)

`ControlSample{tier, uplift, sea, biome, plateau, hillRelief, gentle,
calm, reliefScale, axis*, trunk, trunkDepth, hardness}` à toute
coordonnée, et `macroHeightAnalytic` = S1 sans érosion (fallback,
condition aux limites, support de l'étage 0, miroir avec compression
« érosion-aware » `threshold = 60 + 900·keep + 600·uplift`, pente 0,35 —
fittée sur l'ancien bake fenêtré, jamais refittée).

- **Layout continental** : porteuse 20 km / 0,30 (remap contrasté 0,38-0,62)
  + `continentLayout` (cellules 700 km, noyaux elliptiques, îles 280 km,
  cellule (0,0) ancrée sur l'origine, centre à 0,72·rayon) ; `tierSpread`
  0,35 ; `seaThreshold` 0,42 ; houle longue 22,5 km.
- **Familles de relief** (cible 40/35/25 terre seulement) : socle
  **calm** (plaines sans orogenèse × bande 2,6 km, dessus de plateaux,
  clairières, fonds de vallées post-érosion, fond de tronc), versant,
  drame. `gentle` = corridors praticables (1 375 m) + **cols garantis**
  (strates de ψ, `colSpacing` 4 000 m, gatés `rangeNeed`) + 0,9·trunk.
- **Objectifs** (`landmarkLayer`) : grilles jitterées déterministes —
  alpine 7 km (+600-900 m, cône/échine/mesa), intime 3,5 km (120-250 m,
  1/3 clairières, `hillRadiusMax` 1 600).
- **Vallées** (`ValleyField`) : potentiel φ (9 km), axe = ⊥∇φ, **vallées
  maîtresses = strates d'isolignes** (`trunkSpacing` 9 500,
  `trunkFloorHalfWidth` 900, `trunkShoulder` 2 100, 40-80 m), warp axial.
  Règle absolue : fonctions lisses invariantes par translation.
- **Étage 0** (`MasterNetwork`) : cours dont l'aire vraie ≥ `fleuveArea`
  6e6 m², têtes à cœur demi-ouvert ; consommé par l'empreinte S1, le tier
  fleuve (S4, < 260 m, majorité des points), le far-water, les sources de
  la sim. Ses consommateurs annoncés « site scoring » et « road network »
  n'existent pas.
- **Biomes & climat** : palette 0-5 (tempéré, aride, alpin, toundra,
  subalpin, steppe) ; climat = **deux fbm T/H** à 2 800 m (5 octaves) +
  décret « pays de départ » (`startMeadowRadius/Fade` 8,5/12 km autour
  de l'ORIGINE (0,0) — vestige du monde infini) + seuils d'étage ;
  `biomeIdAt` par texel ; ligne de neige = champ (900 m + wander ±80 m +
  offsets-accents) ; `regionFieldsAt` blende les attributs de `BiomeForm`
  (rockiness, sandiness, grassPresence, temperature, wetness,
  snowLineOffset) ; `materialWeightsAt` = la seam du scatter et de l'herbe.
- **Océan** : profil 3 segments (rivage −6 à 90 m, plateau −14 jusqu'à
  600 m, plancher −70 à 2,5 km) ; nappe de mer ±9 000 m.

### 1.5 L'eau

- **Bake** (§1.3) : lacs = composantes inondées au col de déversement ;
  rivières tracées aval, lissées, largeur `0,008·√A` ; tiers ruisseau /
  rivière (aire ≥ 2e6 m²) / fleuve (réseau maître) ; gués sur les
  rivières seulement (grille monde 2 000 m, portée 700 m, carve 0,4 m sur
  9 m) ; aucun gué sur les fleuves (sites de ponts = non faits) ; carve
  des lits par tier dans le fin ; réconciliation v2 sur le sol final.
- **Données headless** `engine/terrain/WaterBodies` (mer + `LakeSurface`
  + `RiverSurface` + matériaux), immuables, republiées à chaque
  publication de tuile (`publishWaterBodies`) : rendu, exclusion du
  scatter (`underLocalWater`), requêtes.
- **Sim runtime = option C** (`engine/terrain/WaterSim`, virtual pipes
  Kellomäki, **fenêtre 512 m @ 2 m = 257², 30 Hz sur UN job worker**) :
  `initWindow` part SEC (mer seule) ; lacs = **pins** (niveau tenu, sortie
  bornée `reservoirOutflow` 2 m³/s/cellule, intérieur érodé du masque),
  rivières entrantes = **sources** par loi de largeur inversée (runoff
  ×1,6), fleuves maîtres = sources de bord, sources d'esprits ajoutées au
  pas ; cap de vidange 25 %/substep ; bords ouverts ; scroll bit-exact
  (bandes entrantes sèches + miettes) ; pre-roll = `solveSteadyWater`
  multigrille (~2 s worker) puis 450 pas ; cache LRU de session (4
  fenêtres, jamais sérialisé) ; **fenêtres gelées** pour l'eau vue de
  loin ; révélation **settle-gated** (|Δvolume| < 2e-3 × 8 résultats,
  plafond 3 s) ; flaques de pluie. `solveSteadyWater` (`WaterSolve`) ne
  survit qu'en pre-roll, oracle des tests et `cooker water-solve`.
- **Géométrie** : `extractSnapshot` (worker) = mouillé hystérétique →
  connexité → champs → **marching squares duaux** (16 cas, murs
  plafonnés à la colonne), UN maillage fermé ; étendue par géométrie,
  jamais par discard.
- **Requêtes** : `WaterQuery{sim, bodies, seaLevel}` = LA sonde gameplay
  (nage + dérive `swimDriftFactor` 0,8, submersion caméra, douse du feu,
  sorts d'eau, lecture d'eau, spawn hors eau) — la sim est autoritaire
  (eau ET sécheresse) dans son rect de confiance (retrait 64 m), le baké
  ailleurs, la mer en ceinture.
- **Rendu** (`WaterSystem`, `water_surface.glsl`) : mer analytique
  (réflexion planaire, foam) ; lacs masqués (quads par texel + étagère
  ≤ 6 m / jupe / chanfrein) et rubans bakés (`waterlocal`) qui ne se
  rendent qu'au-delà du rect ; far-water (lacs 64 m + rubans ≥ 4 m +
  fleuves maîtres, rayon 9 km) ; sim + gelé ; `WaterMaterialsUbo` 16
  slots (slot 0 = défaut bit-identique) ; volumes posés (`WaterVolume`)
  à part. Recette couleur UNIQUE (absorption par épaisseur optique,
  fresnel ×0,75, `skyWithSun`), « sobriety pass » sur la sim.

### 1.6 Le sol (matériaux)

- **Splat hybride** : poids calculés in-shader (`terrain_weights.glsl`)
  ↔ miroir CPU `materialWeightsCore` édités en lockstep ; canaux bakés
  (biome, wetness, rockExposure, beach) ; pas de splatmap.
- Arrays BC7/BC5/R16 cookés en `.mtex` (conteneur maison, dérogation
  actée ; Vulkan-only, GL garde le procédural) par `cooker
  cook-terrain-materials` (manifest `game/data/base/terrain/
  materials.toml`, sources CC0 `assets-src/` gitignorées, recook auto) ;
  **`kSplatArrayLayers` 25** : familles harmonisées herbe / roche /
  neige / sable / falaise (procédurale, `cliffW = smoothstep(pente) ×
  rockExposure`), scree 16, dépôts par pixel (givre, lande subalpine,
  steppe) ; **hex-tiling** 3 taps (2 au loin) pour tout terme par-pixel ;
  ORM bindé ; wetness + sheen par pixel ; `TerrainShadeMap` 512² / 3 072 m
  (`regionShadingAt` = la source unique de teinte macro) ; height
  blending, normal mapping, POM, bi-fréquence ; `screeFactor` (bande de
  pente sous le seuil rocheux × exposition). Knobs live au panneau
  « Terrain & streaming → Terrain materials ».
- Verdicts figés : footsteps = biome (pas le height blending) ; GI =
  teinte macro, jamais les normales de détail ; **scatter intouchable**
  (`materialWeightsAt` garde ses masques historiques).

### 1.7 Scatter, végétation, arbres, herbe (`engine/render/landscape/`)

- **Deux scatters purs CPU déterministes** rejoués sur workers par chunk
  64 m via `ChunkStreamer` : `scatterProps()` (`VegetationScatter.cpp`,
  ring 12, **un pool GPU unique 1,3 M instances ≈ 40 Mo**) et
  `scatterGrass()` (`GrassSystem.cpp`, ring 3, un buffer par chunk). RNG
  par candidat (`candidateRng(salt, index)`) ; l'ORDRE des tirages est un
  contrat gelé par le golden `VegetationScatterTest`.
- **26 slots de variantes** : arbres 0-4 (feuillus 0-2, conifères 3-4),
  rochers 5-8 (scans Poly Haven décimés), buissons 9-11, débris 12-13
  (souche, tronc couché), plantes héros 14-17 (glTF texturés), masse
  18-21, cailloux 22-25. Grilles jitterées 8 / 7,5 / 5 / 14 / 3 / 2 /
  1,8 m ; gates : `forestMask` × aridité, pente, mer + 3 m,
  `underLocalWater`, `materialWeightsAt`, bandes d'altitude `treeLine` —
  **tout hardcodé** (ni Form ni panneau). Impostors lointains = scatter
  séparé dans `FarTerrain` (même `forestMask`, espacement ×3,5).
- **Une instance = 32 octets** `{xyz, scale, yaw, teinte, phase signée,
  fadeEnd signé}` : **aucun identifiant stable, ni espèce** (implicite
  par slot). Seule copie CPU : `GiProp{pos, scale, kind 0/1/2}` par chunk
  (GI et feu).
- **Collision** (`game/VegetationCollision`) rejoue `scatterProps` sur un
  ring 3×3 : boîtes **statiques** Jolt pour troncs (0,28 × 2,5·s),
  rochers (0,75 × 0,6·s), souche, tronc couché (OBB) ; buissons /
  plantes / masse / cailloux traversables ; recréée intégralement à 7
  sites de `LandscapeScene` ; aucun lien body ↔ instance.
- **Arbres** : colonisation spatiale (Runions 2007 : enveloppe, 350
  attracteurs, pipe model, évasement racinaire), feuillage en cartes
  billboard dans une coquille SDF, atlas de masques de feuilles 8 slots,
  écorce triplanaire + normal-height, 4 LOD + proxy d'ombre métaballes,
  AO sommet cachée sur disque ; `ColonizedTreeTuningForm` (46 champs,
  tous consommés) / `LobeTreeTuningForm` (A/B) ; espèces par slot depuis
  `LandscapeTuningForm.broadleaf/conifer/bushTreeType` ; bibliothèque
  `mods/tree-types.toml` (TreeCreationScene).
- **Feu** : herbe = shader seul + repousse `FireGrid.regrow` ; arbres =
  état runtime par arbre (`TreeFire` Cold→Burning→Burnt→Cold, 6/25/900 s,
  clé = base quantifiée au ¼ m), feuillage via canal B du masque de feu,
  **jamais retiré, non persisté** ; buissons/débris/rochers ignorés.
  Règle dev 2026-09-28 : **brûlé = transitoire, un arbre ne disparaît
  jamais par un masque** (le gate `FireBurntMask` a été retiré).
- **Herbe** : 6 espèces constexpr (`GrassSpecies.hpp`), clumps Voronoï
  2,4 m, densité = présence × rampe de matériau, brins nains + touffes
  sèches/lichen dans les fissures, racine à l'albédo du sol, fade
  140-190 m, pliage joueur.

### 1.8 Le contenu placé sur le terrain généré

- Modèle §5 : `WorldspaceForm` → `CellForm` (grille `cellSize` 64) →
  `ReferenceForm` (`cell = 0` = persistante) ; cellules extérieures
  **implicites** (`cellGuidFor`, `materializeCell`, `ensureCell`) ; la
  brique « contenu procédural par cellule » n'est pas construite
  (décision IMPLICIT-CELLS) ; `CellStreamer` ring 2/3 budgété ; prefabs
  (0 en données) ; `MarkerForm` (seul `"patrol"` consommé).
- Seul générateur de contenu : le **donjon cyclique** (`bakeDungeon` +
  `stageDungeonRecords`, porte ancrée à la caméra). Aucun placement
  piloté par le terrain.
- **Sonde de spawn** `probeSandboxSpawn` : spirale depuis le centre de la
  carte (12 288, 12 288), rayons 2 600 → 9 728 m, critère `sea + 8 < h <
  95 ∧ biome == 0` sur l'overview (ou l'analytique), puis `armWarmup`.
  Le spawn historique (8 197, 230) du monde infini **n'est plus le spawn
  du jeu** (12,7 km du centre).
- Authoring : `TerrainBrush` (Raise/Lower/Flatten/Smooth/Wall, partagé
  avec l'esprit Terre) + `TerrainPatchForm`/`.ter` ; `cooker terrain-pad`
  (pad contre le bruit démo story, inapplicable aux cartes) ;
  `Authoring.{hpp,cpp}` (`stampKernel`, `stampRidge`, `alterElevation`)
  **livré, testé, jamais branché**.

### 1.9 Outils, éditeur, tests

- **Cooker** : `bake-map <gameDir> <mx> <mz> [tps] [--] [--export-plugin
  <name>]` (+ sweep de divergence + rapport miroir), `pre-bake <gameDir>
  <rect | cx cz r>` (cache hors session, far-water complet),
  `erosion-bench`, `terrain-map` (PNG analytique 100-1 000 km),
  `water-solve`, `water-replay <dump.wsd>` (TOUT correctif de sim se
  vérifie sur un dump d'abord), `terrain-pad`, `cook-terrain-materials`,
  `validate`. Aucun banc Python versionné (`tools/scripts/` n'en a pas).
- **Éditeur** : panneau « Terrain generation → Bounded map » (map X/Z,
  Bake, Accept → records, Accept cached) — **zéro paramètre de
  génération exposé** ; sculpt (brosse, « Save terrain to mod ») ;
  panneaux Water (sim, dump), Terrain materials, Grass, Vegetation, Tree
  builder.
- **Tests** (suite 803 cas ; `meadows-tests -tse=slow` avant commit,
  complète en Release avant push) : génération 18 fichiers / 129 cas
  (23 `skip` dont les **21 diagnostics de `SpawnDiagnosticTest`** — ni
  rapide ni slow, à la demande), eau 63 cas dédiés (`WaterSimTest` 29 en
  slow, oracle hors-ligne |Δ| < 0,25 m, perf gate < 100 ns/cellule/
  substep), scatter 2 (+ golden), arbres 8, cartes `MapRecordsTest`
  (export → resolve → base bit-identique). **Aucun test de `bakeMap` ni
  du streamer de carte.** Hashes de non-régression : scatter
  `10031847806692189656`, `region.heights` (banc tuile) `4414106998705828656`.

### 1.10 Ce qui est moddable (et ce qui ne l'est pas)

| Moddable (§5) | Pas moddable (C++) |
|---|---|
| `LandscapeTuningForm` : `terrainSeed`, `seaLevel`, `terrainRecurveLow/Mid/High`, `sandboxTerrain`, `sandboxSnowLine` — **5 champs lus par le bake** ; + 11 `waterSim*`, végétation/herbe (vegViewRadius…, 22 + 7 champs herbe, `broadleaf/conifer/bushTreeType`, `treeLineFactor`, saisons, écorces), matériaux (4 guids d'arrays, knobs splat) | **~150 paramètres de génération** : `ProceduralControlParams` (≈45 : layout, longueurs d'onde, régimes, amers, vallées, cols, lithologie, climat), `MacroParams`, fluvial/thermal/hydrologie/finalize/fine, `BiomeErosion`, gates, `kMapBorder*` — défauts dans `makeMapBakeParams` |
| `BiomeForm` (palette 0-5 : offsets neige, rockiness, sandiness, grassPresence, temperature, wetness) ; `BiomeMapForm`/`.tbm` (mécanisme complet, 0 record, aucun outil de peinture) | espèces d'herbe, habitats des plantes, scans (GUID/slot/tris), constantes du scatter (espacements, salts, seuils, boîtes de collision) |
| `ColonizedTreeTuningForm` / `LobeTreeTuningForm` (100 %), `WaterMaterialForm`, `WaterBodyForm`, `RiverForm`/`RiverPointForm`, `TerrainRegionForm`, `TerrainPatchForm`, `SpiritForm.tree*` (feu des arbres) | `WaterSimParams` hors les 11 persistés (`gravity`, `friction`, `evaporationRate`, `borderDrainPerSecond`, `marginCells`, `dryThreshold`) |
| `WorldspaceForm.bounded/mapX/mapZ` | `WorldspaceForm.mapSize/mapSeed/seaLevel/snowLine/dominantBiome/edge*` : **écrits ou déclarés, jamais lus** |

---

## 2. Décisions actées (avec leur pourquoi)

Chronologie condensée ; les dates sont celles des journaux. « dev » =
arbitrage explicite du développeur.

### 2.1 Génération du relief

| Date | Décision | Pourquoi |
|---|---|---|
| 2026-07-31 | Sculpt = dernier terme ; trois couches (base bakée / détail / deltas) **jamais aplaties** ; bicubique obligatoire ; pas de subsystème save terrain (l'édition est un acte d'authoring → plugin) | le sculpt survit à tout re-bake ; le bilinéaire facette les normales |
| 2026-08-01 | **La génération reste CPU** ; nouvelles passes = gathers/Jacobi à support borné (portables en compute) | bit-exact entre machines = contrat ; headless doctesté ; les workers ne touchent pas au GPU. Réouverture si > ~10 s/tuile ou preview interactif (déclencheur atteint : ~20 s/tuile, jamais rouvert) |
| 2026-08-01 | **Défauts = identité bit-exacte** (`kc = 0`, recurve identité, slot 0 matériau d'eau) | toute brique se compare A/B contre le monde précédent |
| 2026-08-01 | `strataAmplitude` livré à 0 ; pas de `cliffiness` par biome | battement possible avec le terracing ; le shader n'a pas l'id biome |
| 2026-08-01 | **Érosion droplets REJETÉE** (bit-exact inter-tuiles) → **veto LEVÉ le 2026-09-26** par la résolution globale par carte | le veto était un veto de TILING ; il saute avec le tiling |
| 2026-08-02 | Vraies falaises = meshes de rocher plaqués par règle de scatter (pente + rockExposure), pas un nouveau système | un heightfield ne fait pas de paroi (thermique ~40-45° max, 2 m/texel). Non fait |
| 2026-08-05 (dev) | `fluvial.iterations` 100 → **80** | à 100 l'érosion prenait 27 % de l'altitude moyenne sans toucher les sommets (dissection pure) |
| 2026-08-24 (dev) | **Familles 40 / 35 / 25** (socle / versant / drame), monde franchement montagneux | le voyage se négocie par les passages, les vues dominent |
| 2026-08-24 (dev) | **Deux couches d'objectifs** : colline/vallée marquante à ~3 km + sommet alpin 600-900 m à ~6 km ; 1 200-1 400 m = ancres à 15-30 km | « le vrai manque de l'ancien terrain était le nombre d'OBJECTIFS », pas la variété brute |
| 2026-08-24 | Amers par **grille jitterée**, pas par fbm ; vallées = fonctions lisses **invariantes par translation** (après 4 échecs de continuité) | espacement borné, miroir analytique automatique ; continuité |
| 2026-08-24 | Le budget 40/35/25 se joue dans la **structure** (vallées orientées, maîtresses, étage 0), pas dans les hooks d'érosion | census 2-D : 6,4 % socle / 79 % versant malgré les hooks |
| 2026-08-24 (dev) | Porteuse **20 km / 0,30** = le paysage ; **layout continental forcé** (cellule (0,0) sur l'origine, centre à 0,72·rayon, gain ×0,5 plein cœur/océan) ; `tierSpread` 0,35 ; élargisseurs de socle | aucune porteuse ne fait de continents sur 1 000 km ; 0,55 laissait la mer à 40-60 km ; le lift saturait la rampe |
| 2026-08-25 (dev) | Budgets **terre seulement**, plateaux première classe (> 80 m dans le 40 %), médiane terrestre 20-40 m | les fenêtres marines comptaient socle plat (45 %) |
| 2026-08-25 (dev) | Climat régional : `climateWavelength` 350 → **2 800 m** ; décret « pays de départ » | « un biome doit être un LIEU » ; le critère tempéré seul envoyait le départ à 17 km |
| 2026-08-25 (dev) | **Climat GÉOGRAPHIQUE = étape ultérieure** (spec §6.1), après l'eau | consomme les décrets de vent de la météo |
| 2026-09-14 (dev) | **Monde = cartes bornées, une érosion globale par carte, 24 km** (= la super-région du réseau maître) | l'érosion est globale, calculée à travers des fenêtres locales = contradiction ; 200-441 m de divergence, murs 82-88° |
| 2026-09-14 (dev) | Hydrologie **une fois par carte** ; finalize par tranche | partager la surface ne suffit pas (103,8 m) ; le grid 2 m d'une carte = 151 M texels |
| 2026-09-14 (dev) | Bordures v2 : la transition appartient à la **ligne** (hash), serpente, lit le sol entrant, cross-fade aux coins, veto Mer | symétrie structurelle entre voisines ; fini les chaînes sur l'océan et les canaux en plein continent |
| 2026-09-26 (dev) | **Suite = PAYSAGE** : repasser la génération elle-même (gouttelettes dévetotées), scatter interactif, peuplement | 4 096 m était un artefact de perf du commit de naissance, jamais re-questionné |

### 2.2 Cartes, voyage, contenu

| Date | Décision | Pourquoi |
|---|---|---|
| 2026-09-14 (dev) | Phase 1 = cartes procédurales à la demande ; phase 2 = cartes autorées, **un mod AJOUTE une carte** (plugin §5) | le modèle worldspace de Skyrim, déjà possédé (`stageDungeonRecords` = le précédent) |
| 2026-09-14 (dev) | Transition carte-à-carte = fondu type porte ; couloir continu au backlog | réutilise le voyage existant |
| IMPLICIT-CELLS | Cellules extérieures implicites livrées ; grille pleinement virtuelle (contenu procédural par cellule) **non construite** | pas nécessaire pour éditer partout — mais c'est l'accroche d'un peuplement généré |
| 2026-09-27 (dev) | **Le scatter doit spawner des objets interactifs** : rochers (puis troncs, buissons) deviennent des références **à la demande** (saisie, feu, impact) | l'emprise de pierre ne saisit que les `StaticForm` placés |

### 2.3 Eau

| Date | Décision | Pourquoi |
|---|---|---|
| 2026-08-01 | Réconciliation à chaque publication ; surface en **R32F absolue** ; `Floater` cinématique sans Jolt (dev) | l'eau ne flotte jamais ; le f16 relatif ne tient pas un lac à 700 m |
| 2026-08-24/25 (dev) | **Quatre tiers** à caractères discrets ; **océan à deux étages** ; **« les flaques, pas les lacs »** (cible 2-6 lacs/tuile caduque, critère = qualité) ; gués ~2 km (grille monde), fleuve 24-36 m de large sans gué, ruisseau guéable partout | les vraies flaques étaient les mares de confluence (r 5-6 m → 15 m) |
| 2026-08-26 | **Le fleuve est CONSTRUIT, pas reconnu** (imprint S1 lower-only ≥ 1,5 m/km, plaine ≤ 14 m, jamais de barrage) ; channel keep `0,0018·A^0,44` | « le fleuve à l'embouchure est une espèce de lac » ; le plafond de dépôt aggradait chaque lit à sa ligne d'eau |
| 2026-08-26 (dev) | Refonte eau : **option D** (solve hors ligne à l'équilibre) ; peuplement gelé | « les surfaces d'eau sont forcément plates » |
| 2026-08-27 (dev) | **Bascule D → C** (sim fenêtrée temps réel) ; eau lointaine = lacs + rubans bakés restaurés ; eau transitoire **jamais sérialisée** | 3 validations échouées : la grille de solve 8 m ne suit pas le terrain 2 m — **structurel** |
| 2026-08-27 | UNE géométrie fermée par snapshot ; étendue par géométrie, discards interdits ; mouillé/sec hystérétique ; le baké ne se rend qu'au-delà du rect | trois géométries empilées = clignotements et orphelines ; les discards effaçaient des surfaces entières |
| 2026-08-28 (dev) | **« Le modèle D ne revient pas »** : pas de re-bake D, **pas de carve du terrain depuis une lame résolue** ; régénération au retour = cache LRU + settle-gate ; **fenêtres gelées** plutôt qu'« agrandir la fenêtre » | +13 s/tuile pour rien ; le terrain est déjà adapté par la topologie de drainage ; coût n² de la fenêtre |
| 2026-08-29 | **Marching squares duaux** ; anneau From Dust reverté | le bord d'eau est en marches, l'anneau escaladait les berges |
| 2026-08-30 (dev) | Étagère de rive ≤ 6 m (frange de lac qui se remplit en 2-3 min = accepté) | la fenêtre couvre le lac ~250 m avant |
| 2026-08-31 | EAU E1-E7 clos ; **banc offline obligatoire avant chaque bump** ; `cooker pre-bake` = le véhicule des bumps ; brique 3 lacs par composante (v68) | 31/31 lacs inchangés + 9 bassins récupérés |

### 2.4 Sol

| Date | Décision | Pourquoi |
|---|---|---|
| 2026-08-02 | Splat hybride (poids in-shader + canaux bakés), pas de splatmap ; **`.mtex` maison, pas KTX2** (dérogation CLAUDE.md §3) ; BC **Vulkan-only** | monde généré/streamé ; 1 fread, zéro transcodage ; le dev envisage d'arrêter GL |
| 2026-08-02/03 | Footsteps = biome ; GI = teinte macro (≠ normales de détail) ; **scatter intouchable** | le pas sonne comme le sol visible ; zéro reseed |
| 2026-08-03 (dev, REJET) | **Les variantes d'un sol restent dans la même famille chromatique ; la variation vit dans le contenu et le relief, jamais la couleur** ; cellules 3 m ; `harmonize` au cook | feuilles brunes/terre nue = taches de couleur |
| 2026-08-04 (dev) | **Philosophie Skyrim moddé** : relief par pixel et transitions plutôt que toujours plus de petits objets ; pas de « système herbe » pour la densité (VegetationSystem + rampe) | perf scans 24 → 11 fps |
| 2026-08-04 (dev) | Hex-tiling remplace le Voronoï ; **transition herbe↔neige = dépôt par pixel** (« l'herbe se dépose sur la roche, la neige sur l'herbe ») ; flip de variante = même famille harmonisée (albedo ET ORM) | les frontières Voronoï se voyaient ; un seuil de poids découpe des formes dures |
| 2026-08-05 (dev) | Éboulis V1 = règle de pente + couche scree ; **V2 = canal de déposition baké par l'érosion, différé** ; FarTerrain = vraie règle des poids × albédos moyens, 18 km | « les paysages distants doivent avoir la vertex color du sol » |
| 2026-08-05/06 (dev) | SSAO d'abord, SSDM prototype OFF par défaut (critères ≤ ~2,5 ms et scintillement = abandon) ; audit perf 106,9 → 78,4 ms | Crimson Desert = SSDM |

### 2.5 Végétation et scatter

| Date | Décision | Pourquoi |
|---|---|---|
| 2026-08-04 | Ordre des tirages RNG de `place()` = **contrat** (variant, scale, yaw, tint, phase) ; cailloux = mêmes meshes de boulders (§2.11) | zéro reseed ; réuse |
| 2026-08-04 (dev) | Palier 1 scans en vertex-color ; palier 2 plantes texturées + alpha cutout ; exagération ×1,5-2 de l'étage bas (convention Skyrim) | fill-rate TBDR ; la fougère se noie à l'échelle réelle |
| 2026-09-28 (dev) | **Brûlé = transitoire ; l'herbe repousse (plus vite irriguée) ; jamais de suppression d'arbres par un masque ; un arbre brûle par mécanisme progressif dédié** (feuillage seulement) | « les plantes repoussent, un arbre ne disparaît pas comme ça » |

---

## 3. Leçons mesurées (à ne pas repayer)

### 3.1 Numérique et physique du relief

- **Monter k aplanit** (S = U/(k·A^m)) ; baisser la capacité fait déposer.
  Le plan B2 avait le signe inverse.
- 100 itérations fastscape = −27 % d'altitude moyenne sans toucher les
  sommets ; `rounding` innocent.
- **Les hooks d'érosion ne font pas le budget de familles** ; élargir les
  socles est structurel.
- **Continuité (4 échecs)** : repère (u,v) global tourné (déchirure ∝
  distance), distance métrique 1/|∇φ| (cisaillement), hash par strate
  asymétrique, gate `inland` trop raide. Règle : fonctions lisses
  invariantes par translation, masques en unités de PHASE, warp axial.
- Porteuse continentale : additive brute = continents criblés de lacs ;
  compression gatée par |lift| inopérante ; seul le remap contrasté marche.
- Têtes du réseau maître : cœur **demi-ouvert**, sinon double troncature
  sur z = 0.
- Aggradation : le **plafond de dépôt** est le coupable, pas le plancher
  de pente (`capacitySlopeFloor` bit-identique).
- Lacs naturels : aucune queue de flaques ; les « flaques » étaient des
  mares de jonction. Le trace ne casse qu'aux lacs **acceptés** (sinon
  trous secs).
- Le carve S5c est le dernier écrivain de hauteur : lits sains même si S2
  aggradait.
- « Terre au loin » : mesurer avant d'accuser le miroir analytique (c'était
  la portée de la nappe de mer).
- **Une crête artificielle sans keep d'érosion est rabotée** (670 → 313 m).
- **L'apron ne couvre pas** : deux fenêtres voisines convergent vers deux
  équilibres d'érosion incompatibles ; **partager la surface ne suffit
  pas, il faut partager l'eau** (441 → 103,8 → 0,281 m). Un dedup
  inter-tuiles qui ÉCARTE une vue de lac est fragile : partager à la source.
- Cache dev et re-bake à neuf bit-identiques : quand le bug est de
  principe, le cache est innocent.

### 3.2 Instruments

- Fenêtres marines comptées socle plat (45 %) → exclure ; horloge
  d'événements en pause sur l'eau ; filtre des points de voyage 320 → 450 m.
- Deux transects droits ne calibrent pas un monde hétérogène : **le tour
  du dev en jeu est l'instrument suivant**.
- Un instrument qui sonde LE LONG des runs ne voit pas les trous ENTRE eux.
- `std::hash` n'est pas portable → hash splitmix local pour tout jitter.
- Un biome à λ 350 m est un confetti : mesurer la couverture avant de
  juger une texture invisible.
- **Depuis CARTES, les diagnostics `SpawnDiagnosticTest` sont ancrés sur
  un monde qui n'existe plus** (seed 1337, spawn (8 197, 230), tuiles
  isolées sans bordures, `bakeSoloTile`) — à re-baser avant toute mesure.

### 3.3 Bit-exact, caches, build

- Déterministe pour (params, carte) ; changer un défaut S1 impose de
  **vider `terrain-cache`** (la clé est la seed) ; une donnée runtime
  (`landscape.toml` neige/offsets) ne bump rien ; recook `terrain_*.mtex`
  après tout changement de manifest ; **rebuilder le cooker** après tout
  changement de défauts terrain.
- **Golden scatter `instance buffers are frozen` échoue sous MSVC** (hash
  capturé clang/Fedora) — seul rouge toléré, **ne jamais re-capturer sous
  MSVC**.
- `FrameUbo` append-only ; après tout changement de layout partagé,
  rebuild propre (purge des .obj hors `_deps`) — leçon Phase 5.
- `LNK1104` sur `true-adventurer.exe` = le jeu du dev tourne (transient).
  Jamais deux configureurs CMake sur un même build dir.
- Le test calm stage-1 cherche sa tuile sèche par sonde analytique en
  spirale (l'origine d'une seed de test peut être humide).
- **Toute brique qui bouge le hash `region.heights` a changé le terrain**
  — l'érosion par gouttelettes le bougera par design : ré-épingler.

### 3.4 Eau — simulation (le grand livre, compact)

1. Cap de vidange 25 %/substep (100 % = paquets sautant une cellule).
2. Réservoirs épinglés : niveau TENU + sortie bornée (~2 m³/s/cellule) ;
   sortie libre = 6 200 m³/s mesurés.
3. Épingler l'INTÉRIEUR du masque (érodé d'un texel) ; jamais les étangs
   sans masque.
4. Films rapides : publier dès ~4 mm si v > 1,5 m/s (le seuil 2 cm gomme
   les chutes).
5. CFL : dt ∝ √texel ; 1/30 s stable à 2 m.
6. Sol de sim = `terrain::height` COMPLET (base + détail + patches).
7. **L'eau dormante vient du BAKÉ, jamais de l'init** (lac fantôme 138 m /
   9,5 M m³) : la sim déplace l'eau, elle n'en invente pas.
8. **Les bandes de scroll entrent SÈCHES** (3,9 M m³ peints sinon) ; lacs
   re-rasterisés après chaque scroll ; wetMask décalé avec les plans.
9. La sim attend la tuile bakée sous la caméra.
10. **La vitesse caméra n'invalide jamais la fenêtre : elle SCROLLE** ;
    seul un téléport > demi-fenêtre ré-initialise.
11. Miettes : rafraîchir l'empreinte courante toutes les 4 s.
12. Option D : évap ≥ pluie gèle tout ; bord-mur empile la pluie ; la
    profondeur casse les rivières en flaques (**le flux est le tracé**) ;
    8 m de solve vs 2 m de rendu = échec structurel, non rattrapable.
13. `cooker water-replay` : tout correctif se vérifie sur un dump AVANT
    de re-déranger le dev.

### 3.5 Eau — shading et rivages

14. Un NaN empoisonne le bloom par tuiles : garde sur `normalize(cross)`.
15. **La COULEUR ne lit jamais le champ simulé** (taches évolutives) :
    absorption par épaisseur optique + teinte constante ; le champ ne sert
    qu'à l'advection.
16. **Recette sim = recette mer, à l'identique** (sobriété symétrique) ;
    simplifier l'eau sim seule redessine le rect par contraste. Tout ou rien.
17. La peau d'un lac dans le rect appartient à la SIM ; passation par
    IDENTITÉ : rubans = bande de fondu, lacs = coupe géométrique exacte ;
    l'eau calme reste PLEINE jusqu'à la coupe.
18. Underside = décision uniforme (caméra immergée), pas par fragment.
19. Murs : seuil de pente ~81°+ ; face jamais plus haute que la colonne.
20. **Rivages : l'anneau From Dust a échoué, les marching squares duaux
    tiennent** ; tout lissage reste interne à la grille de sim.
21. Étagère ≤ 6 m dont le sol remonte = nappe continue ; sinon crête =
    flush + jupe ; un film de crête n'exporte jamais son niveau.
22. **Backend d'abord quand l'artefact erre** (ronds → croix → cyan =
    données indéfinies : whitelist d'upload GL46) ; vérifier le HANDLE.

### 3.6 Sol et matériaux

- Tout terme par-pixel passe par les 3 taps hex, sinon le lattice
  transparaît (prouvé 3×).
- Un bruit de frontière qui échantillonne une couche est calibré pour UN
  set de matériaux → bruit analytique partagé bit-à-bit (`borderWander`).
- La composition de teinte (fbm) ne se paie jamais sur le chemin chaud du
  scatter (+70-100 % de chargement) → `regionFieldsAt` / `regionShadingAt`
  scindés.
- MoltenVK : bindings de samplers = espace GLOBAL par closure d'includes ;
  un échec MSL empoisonne `vulkan-pipeline-cache.bin` (le supprimer).
- `bc7enc` : init complète avant les poids ; mips rééchantillonnées depuis
  la base ; ambientCG = NormalGL (pas de flip).
- **La crainte « textures qui tilent = cher » est infondée** : les coûts
  sont le travail fragment (hex × familles, POM, fill/discard végétation
  TBDR), le SSDM plein écran, les ombres, la GI, le miroir.

### 3.7 Scatter, arbres, perf

- **Clignotement ~10 Hz = `GpuOcclusion::kMaxGroups` < groupes réels**
  (commandes aliasées) → `static_assert` : la prochaine variante casse le
  build, pas l'affichage.
- Des milliers de corps Jolt pour des débris = SIGBUS à la destruction
  (d'où les slots cailloux sans collision).
- UV photogrammétriques dans la lane sway = mesh déchiré au vent → réécrire
  les UV après normalisation.
- Boîte de collision orientée par le yaw ; un prop long sondé aux DEUX
  bouts, assise sur la crête.
- Scans partagés par des centaines d'instances castés plein détail dans
  toutes les cascades = budget shadows gonflé → jumeaux LOD + filtre.
- Poly Haven : plantes = alignements d'exposition (ne pas renormaliser),
  alpha en map séparée.
- Perf CPU (ÉCONOMIE, `docs/CPU-PERF.md`) : marche 60 s = rcTile 55 %,
  grassScatter 13 %, waterSim 10 %, vegScatter 9 % ; churn des anneaux en
  vol 30 s ; la lightmap suit la marche (~10 s) sur `render::HeightField`.

---

## 4. La cible « Paysage & peuplement — biome tempéré » (validée dev 2026-08-24)

Constat d'origine (seed 1337) : trop érodé, trop bas, uniformément
haché — « le problème n'est pas la quantité de relief, c'est son
ABSENCE DE HIÉRARCHIE ». Rythme : course 5,5 m/s, sprint 11, monture 9 ;
**45 s = ~250 m** = l'unité de changement pertinent.

- **§1 Trois échelles** : micro 50-250 m = l'incident (scatter, POI ; sol
  habitable calme < 15 m / 250 m) ; méso 250 m-1,5 km = UNE forme
  nommable à la fois (fond de vallée, versant, crête, rebord, bassin,
  gorge) ; macro 1,5-10 km = la région et ses objectifs superposés
  (intime ~3 km, héroïque ~6 km à 600-900 m, ancres 1 200-1 400 m à
  15-30 km). Depuis tout point de voyage, un amer de chaque couche visible.
- **§2 Relief 40 / 35 / 25** : socles calmes (pentes < 10°, relief
  < 15 m, dissection fine éteinte — où vivent peuplement, chemins,
  incidents) ; versants (10-30°, ravines, éboulis) ; drame (> 30°,
  infranchissable hors cols, ferme et oriente). Les socles hauts gardent
  leur altitude.
- **§3 Vues** : vallées orientées (500-1 500 m de large, axes de
  plusieurs km) ; belvédères aux rebords/cols/crêtes où les chemins
  crêtent ; silhouettes reconnaissables. Critères `vista` : ≥ 30/72
  azimuts > 2 km ; ≥ 1 amer > 2° à > 3 km ; profondeur médiane > 6 km
  depuis un belvédère.
- **§4 Eau** : ruisseau ~500-800 m (enjambable) ; rivière une par vallée
  maîtresse, encaissée, gués/ponts tous les 800-1 500 m ; **fleuve un par
  région tous les 10-15 km**, dérive forte, franchissements aménagés tous
  les 2-4 km, villes aux ponts/embouchures ; embouchure = delta (côte
  basse) ou estuaire (côte dure) ; lacs = destinations (critère qualité) ;
  océan −5..−25 m puis −100..−150 m.
- **§5 Peuplement** :

| Niveau | Bâtiments | Espacement | Temps | Site type | Pad |
|---|---|---|---|---|---|
| Hameau | 3-8 | 0,8-1,2 km | 2-4 min | replat de socle, source/ruisseau < 200 m | ~60×60 m, pente < 5° |
| Village | 10-25 | 2,5-4 km | 8-12 min | confluence, tête de pont, rive de lac, croisée | ~150×150 m |
| Ville | 30+ | 8-12 km | 25-35 min | grande confluence, estuaire, butte, rebord défensif | ~300×300 m |
| Port | ville/village côtier | 10-15 km de côte | — | baie abritée + eau peu profonde + arrière-pays plat | quai + pente d'accès |

  (à monture ÷ ~1,6). POI non habité à ~400-900 m de tout chemin,
  légèrement hors chemin. Chemins : sentier (hameau↔hameau, gués), chemin
  (village↔village, fond de vallée, cols, ponts), route (ville↔ville,
  pente ≤ 8-10 %, crête aux belvédères = la visite guidée des vues).
  Passages : un col tous les ~3-5 km de linéaire, jamais de cul-de-sac
  régional. Placement = scoring sur les champs existants, sélection
  déterministe par seed, pads `alterElevation`, records ancrés monde.
- **§6 Critères** (amendés 2026-08-25, terre seulement) : `variety
  transect` 16 km / 250 m : relief médian **20-40 m**, plates < 8 m
  **15-35 %**, pentes > 30° **< 15 %**, infranchissable continu ≤ 400 m
  sans passage, événements ≤ 500 m (pire trou ≤ 1 200 m) à TYPES alternés ;
  `vista` aux points de voyage ; hydrologie et peuplement aux espacements
  ci-dessus à ±30 %. Mer ~20-25 % du monde.

**Plan 2026-08-24 (14 briques) — statut** : B0 instruments, B1 calm, B2
budget par famille, B3 amers, B4 vallées/cols, B5 étage 0, B6 adoption
du layout + calibration, B7 océan (**validé dev**), B8 vasques, B9 tiers
+ gués + imprint fleuve (B9b/c/e/f) : **livrées** (2026-08-24→26).
**B10-B13 = le peuplement** (embouchures delta/estuaire, scoring de
sites et sites de ponts, pads, re-fit analytique complet + calibration
en jeu) : **gelées le 2026-08-26**, jamais reprises — c'est le périmètre
de PAYSAGE. Leur découpage exact n'a jamais été journalisé dans le dépôt :
on le renomme ici sans numéro hérité (§7).

**Baseline du monde adopté (seed 1337, avant CARTES)** : mer 22,6 % ;
plus long cours 41,6 km ; sommets 1 534 m à ~4 km du spawn ; familles
E-O 38/38/24 (M4), médian 25,3 m, > 30° 12,8 % ; N-S 13/72/15 ; 5 tuiles :
110 ruisseaux / 15 rivières / 7 fleuves, 7 gués ; neige 5,8-7,3 % plein.
**Ces chiffres datent du monde infini** : la carte bornée (0,0) est un
autre monde (bordures, hydrologie globale, spawn au centre).

---

## 5. ÉTAT DES LIEUX TOTAL — 2026-10-05 (ouverture du chantier)

Inventaire lecture seule du dépôt à `b17d1f3` (six rapports : génération,
eau, scatter, peuplement, et les deux digests de journaux). Faits
vérifiés par grep ; rien de proposé ici.

### 5.1 Ce qui existe — où est chaque chose

| Capacité | Où |
|---|---|
| Bake de carte (érosion globale + hydrologie une fois + 36 tranches) | `game/MapBaker` (`bakeMap`, `mapBakedAndValid`, `loadMapOverview`), `engine/terrain/generation/TileBake` (`bakeTileStage1`, `extractMapHydrology`, `bakeMapSlice`, `reconcile*`) |
| Contrôles, macro, bordures | `TerrainGen` (`ProceduralControls::at`, `synthesizeMacro`, `landHeight`, `coastProfile`, `recurveLand`, `macroHeightAnalytic`, `applyMapGridShape`, `mapBorderStyleResolved`) |
| Étage 0 | `MasterNetwork` (`computeMasterNetwork`, `masterRiversNear`, `masterBoundarySources`, `imprintMasterChannels`) |
| Érosion | `FluvialErosion` (fastscape + dépôt + channel keep), `ThermalErosion` (+ `roundRidges`), `FineErosion` (`amplifyFine`) |
| Hydrologie, tiers, gués | `Hydrology` (`extractHydrology`, `extractLakes`, `classifyRivers`, `makePond`) |
| Finalisation, masques | `Finalize::finalizeTerrain` (S5a-d, S6) |
| Streaming carte, far-water, prefetch | `game/TerrainBakeStreamer` (`request`, `update`, `prefetchMap`, `ringStatus`, `collectFarWater`) |
| Seam runtime | `engine/render/landscape/TerrainNoise` (`height`, `normal`, `proceduralBase`, `biomeAt`, `regionFieldsAt`, `materialWeightsAt`, `treeLine`, `underLocalWater`) |
| Régions, patches, brosse, biomes | `world/terrain/{TerrainRegions, TerrainPatches, TerrainBrush, BiomeMapBuilder, MapRecords}` |
| Carte → mod | `world/terrain/MapRecords::stageMapRecords`, `cooker bake-map --export-plugin`, panneau « Bounded map » |
| Swap de monde, spawn, voyage | `LandscapeScene::applyMapWorld`, `probeSandboxSpawn`, `travelToMap` ; Lua `travelToMap` ; `passes.toml` |
| Eau : bodies, sim, requête, rendu | `engine/terrain/{WaterBodies, WaterSim, WaterSolve, WaterQuery, RiverGeometry}`, `world/terrain/WaterBodiesBuilder`, `engine/render/landscape/WaterSystem` (+ `water_surface.glsl`) |
| Sol | `TerrainSystem`, `TerrainShadeMap`, `SplatTextures.hpp`, `terrain_weights.glsl`/`terrain_zones.glsl`, `engine/assets/CookedTexture`, `cooker cook-terrain-materials` |
| Scatter, arbres, herbe, collision | `VegetationScatter/System/Streaming/Assets`, `GrassSystem`, `GrassSpecies.hpp`, `TreeGenerator`, `SpaceColonizationTree`, `FarTerrain` (impostors), `game/VegetationCollision` |
| Feu × végétation | `world/spirit/SpiritFire` (`TreeFire`), `engine/terrain/FireField` (`FireGrid.regrow`), `LandscapeScene::updateSpiritFireTrees` |
| Rochers saisissables (esprit Terre) | `LandscapeScene::seizeRock` (entités `StaticForm` `surfaceMaterial == "rock"`), `world::Displaced`, `SaveGame.cpp` |
| Donjons | `engine/dungeon/*`, `world/dungeon/DungeonRecords`, panneau « Dungeon generation » |
| Navigation | `nav::Navigator` (interface), `world/ai/TerrainNavigator` (A* 1 m, 20 000 expansions ≈ 140 m), `InteriorNavigator` ; Recast absent |
| Diagnostics | `tests/SpawnDiagnosticTest.cpp` (21 cas `skip`), `MasterNetworkTest` diagnostic, `TileBakeTest` benchmark |

### 5.2 Code mort ou vestigial (preuves grep)

| # | Élément | Où | Constat |
|---|---|---|---|
| 1 | `kStage1Version = 46` | `TileBake.hpp` | **0 lecteur** ; le cache stage-1 est mort en M1.5b ; son commentaire décrit un système à deux versions qui n'existe plus |
| 2 | `Authoring.{hpp,cpp}` (279 l.) | `engine/terrain/generation/` | inclus uniquement par `tests/AuthoringTest.cpp` ; le slot « authored → S1 » n'est occupé que par `imprintMasterChannels` ; aucun miroir dans `macroHeightAnalytic` (contrairement à ce que la cible §5 présente) |
| 3 | `strataAmplitude = 0` | `Finalize` | branche jamais prise hors test, depuis sa livraison |
| 4 | Champs eau de `TerrainRegion` (TRG3 : `waterSurface/Depth/Vel*/Flux`) | `TerrainBase.hpp`, `TerrainRegions.cpp` | « no writer, no reader » ; encore sérialisés/validés (`validWater`) ; lane `waterReserved0`, bindings 5/6 gelés ; `WaterSolve.hpp` dit « real-time NOT built » (faux) ; `WaterBodies.hpp` renvoie à `WaterInfoMap.cpp` (supprimé) |
| 5 | `WorldspaceForm.edgeNorth/East/South/West` | `WorldForms.hpp` | mort-né (design v1 des bordures) ; `mapSize/mapSeed/seaLevel/snowLine/dominantBiome` écrits par `stageMapRecords` ou déclarés, **jamais lus** (le « tuning par worldspace » n'est pas câblé) |
| 6 | `MacroParams.hillChainWavelength = 0` | `TerrainGen.hpp` | toujours écrasé ; doublon |
| 7 | `kBasinResolveMargin` (nom), `TileBakeParams.apron = 1536` + commentaires de tête de `TileBake.hpp`, `TerrainGen.hpp` (« two providers »), `TerrainBakeStreamer.hpp`, `SandboxTerrain.hpp` | — | vivants mais décrivent le composite 3×3 / les tuiles indépendantes / un second fournisseur de contrôles qui n'existent plus |
| 8 | `BiomeMapForm` / `.tbm` / `paintedIndexAt` | `BiomeMap.hpp`, `BiomeMapBuilder` | mécanisme complet, 0 record, aucun outil de peinture — dormant |
| 9 | `startMeadowRadius/Fade` (distance à l'origine (0,0)) | `TerrainGen.cpp` | vestige du monde infini : le centre de la carte (0,0) est à 17,4 km de l'origine ; la sonde fait le travail par `biome == 0` |
| 10 | `cooker terrain-pad` | `Main.cpp` | pad contre le **bruit démo story**, inapplicable aux cartes |
| 11 | Ancrages de `SpawnDiagnosticTest` (1337, (8196.77, 230.072), tuiles (2,0)/(3,0), `bakeSoloTile` sans `mapGrid`) | tests | monde pré-CARTES ; `biome locator` ne reflète plus les critères de la sonde |
| 12 | `WaterSimParams::dryThreshold` | `WaterSim` | jamais lu par `extractSnapshot` (seuils codés en dur) ; seul `water-replay` le lit |
| 13 | Texture `simMapA` (R32F `display`) + `uWaterSimA` + `waterSimUv()` | `WaterSystem`, `water_surface.glsl` | le shader ne lit que `uWaterSimB` ; la texture est pourtant détruite/recréée à chaque tick |
| 14 | Composant `Floater` | `world/scene/Floaters` | enregistré et tické, **aucun producteur** (ni spawner, ni Form, ni script) ; vit dans `FloatersTest` seulement |
| 15 | Boîtes « Volumes debug » (mode 3) | `WaterSystem::draw` | dessinées seulement s'il existe une fenêtre gelée — bug latent |
| 16 | Foam/whitewater/foamGate/pool map des corps bakés | `waterlocal.frag` (`WATER_FAR` pour TOUS les bakés) | calculés puis annulés (`foam = 0`) ; seule la mer garde l'écume |
| 17 | `WaterBodyForm.tint/chop`, `RiverForm.tint` ; `vMaterial = 0` sur la sim | builder / shader | recopiés, jamais consommés ; un lac « lave » redevient eau dans le rect |
| 18 | Dédup inter-tuiles des lacs | `publishBakedTiles` | n'existe plus (propriété réglée au bake) — `WATER-RENDER §5` le décrivait encore comme filet |
| 19 | `VegetationScatter.cpp` : `kPropCasterShader` local + 7 includes orphelins | — | reliquat du split en 4 TU |
| 20 | `VegetationCollision.cpp` filtre `w < 0.35` | — | mort depuis les slots cailloux (rochers 0,5-2,0) |
| 21 | `WorldRenderer.cpp` charge `rock_cc0.gltf` dans `kFirstRock` | — | écrasé par `stone_01` dans `LandscapeScene` |
| 22 | `generateRock`/`generateBush` | `TreeGenerator` | placeholders écrasés par les scans / « bush » |
| 23 | `BiomeForm.vegetationSet` → `BiomeParams.vegetationSet` | `BiomeMapBuilder`, `TerrainNoise` | copié trois fois, **jamais lu** ; `BiomeVegetationForm` enregistré, 0 record, 0 `forEach` |
| 24 | `Instance.params.w` « free » ; `GiProp.kind` débris = rocher | commentaires | périmés |
| 25 | `MarkerForm.kind` « spawn »/« idle » ; `MapRaster.hpp` « every marker/POI » | — | vocabulaire non consommé ; écran M = joueur + portes |
| 26 | Cellules story Overworld (x 0-192, z 320-384) | `adventure.toml` | posées dans la bande de chaîne de bordure (2 560 m) du worldspace `bounded` (0,0) |

### 5.3 Absences confirmées (grep 0)

| Besoin (cible §4 / passation) | Verdict |
|---|---|
| **Érosion par gouttelettes / particules** | absente (rejetée 2026-08-01, dévetotée 2026-09-26, jamais écrite). Le pipeline est grid-agnostique et global par carte : une passe séquentielle y est désormais techniquement possible |
| **Scoring de sites, hameaux/villages/villes/ports, POI** | absents ; `trunk`/`calm`/`gentle` portent des commentaires « future site scoring » que personne ne lit |
| **Pads appliqués au pipeline / miroir analytique** | absents (`alterElevation` sans appelant) |
| **Chemins / routes / sentiers, Dijkstra sur raster de coût** | absents ; le champ `gentle` n'est ni rasterisé ni persisté ; seul routage grossier = hydrologique |
| **Ponts, gués comme contenu** | ponts : rien ; gués = terrain carvé, listés dans `.twb`, **perdus à l'export** (`RiverForm`/`RiverPointForm` n'ont ni `tier` ni `fords`) |
| **Delta / estuaire** | absents (l'embouchure = le fleuve passe sous `seaLevel`) |
| **Climat géographique** (latitude, lapse rate, advection, ombre pluviométrique) | absent ; T/H = deux fbm |
| **Cartes de contrôle peintes / `AuthoredControls`** | absents ; `ControlSource` n'a qu'un fournisseur de production |
| **Cols générés / exportés, points de voyage en jeu** | absents (un col à la main ; la grille de voyage n'existe que dans un test) |
| **Contenu procédural par cellule** (IMPLICIT-CELLS brique 4) | non construit |
| **Persistance de `calm`/`gentle`/`trunk`/`deposit`/`hardness`** | jetés après le bake |
| **Ré-ancrage des deltas sculpt au re-bake** | absent (dette journalisée depuis v1) |
| **Meshes de falaise** (règle de scatter), `cliffiness` par biome, vallées glaciaires | absents |
| **Scatter interactif** (instance → référence) | rien de commencé ; voir §5.5 |
| **`vegetationSet` / espèces en Form / pitch par instance / BC7 des albédos de plantes** | absents |
| **Flottabilité Jolt, polish FX eau (cascade, foam de rive, whitewater), cues cascade/pluie, `updateTexture` RHI, plages lacustres, unification `WaterVolume` dans `WaterQuery`, critère d'étendue humide du settle-gate** | absents |
| **Test de `bakeMap` / du streamer de carte ; banc Python terrain ou eau versionné** | absents |
| **Recast/Detour** | absent (décision) |

### 5.4 Écarts doc ↔ code tranchés (la vérité courante)

1. **Eau = option C** (sim fenêtrée). L'étude recommandait D ; D a échoué
   structurellement ; `solveSteadyWater` = pre-roll + oracle. Point final.
2. **Le chemin fenêtré est mort** : `reconcileLakesWithTerrain` v2/v68
   survit mais tourne dans `bakeMapSlice` sur une hydrologie par carte ;
   le « lac interrompu » est résolu à la racine ; les budgets `tileBake`
   de CPU-PERF (2,36 s stage-1 fenêtré) ne sont plus le budget — **~94 s
   par carte de 24 km, en fond**.
3. **Rochers : trois modèles coexistaient** (instances de scatter sans
   entité ; `StaticForm` placés saisissables avec corps convexe + `Displaced` ;
   demande « références à la demande »). Le mécanisme unifié est à
   construire dans PAYSAGE (§7).
4. **Arbres et masques** : masques → herbe/buissons/densité ; arbres →
   mécanisme progressif par état (`TreeFire`), jamais par masque.
5. **Hex-tiling** : livré (2 taps + collapse 25-40 m), malgré deux listes
   de différés qui le gardent.
6. FarTerrain accordé au sol et raccord herbe/sol : réglés (GRASS-REDO).
7. Tap live water-info et `WaterInfoMap` : **supprimés** (E5).
8. **Numérotation B10-B13** : trois sens dans les journaux (water-info/
   matériaux d'eau/authoring de v2 ; peuplement du plan 2026-08-24 ;
   « peinture de splat B10 » de TEXTURING). Ici, le peuplement se nomme
   **peuplement**, sans numéro.
9. Plages/berges lacustres : un seul item, deux chemins possibles
   (shading wetness/beach vs masque au bake) — à porter une fois.
10. Éboulis V2 (canal de déposition baké) rejoint naturellement l'érosion
    par gouttelettes (dépôt de sédiments) et la publication TRG de `hardness`.
11. RENDERING.md : « 12 variants » (26), pool « ~12 MB » (40 Mo),
    `kGroupBase` 4 (5). TERRAIN-GEN gardait le préambule « monde infini »,
    `bakeTile`, apron 1 km, palette 0-3, « Bake region here », `solveWater
    OFF` (purgé), « la sonde retombe sur (8196, 230) » — tout périmé.
12. `MasterNetwork.hpp` et `TerrainGen.hpp` annoncent des consommateurs et
    un second fournisseur qui n'existent pas ; `TileBake.hpp` documente un
    bump `kStage1Version` qui ne protège rien.
13. La cible §4 présente les pads comme disponibles (« primitive déjà
    livrée en B12 ») : la primitive existe, isolée, sans miroir ni appelant.

### 5.5 Le point dur « instance de scatter → référence » (demande dev 2026-09-27)

Faits : (1) **aucune identité d'instance** — une instance n'est
identifiable que par (chunk, slot, index de candidat) à `TerrainParams`
fixes, ou par sa position quantifiée (ce que fait `treeKey` au ¼ m,
privé à `LandscapeScene`) ; (2) re-scatter déterministe par candidat
(`candidateRng(salt, index)`) — un candidat survit tant que ses gates
passent, mais tout sculpt/tuile/changement d'eau peut déplacer ou
supprimer des instances ; (3) **aucun mécanisme de suppression d'UNE
instance** (ni masque d'exclusion, ni liste de trous ; le seul gate
externe a été retiré ; granularité = chunk entier) ; (4) **aucun
`StaticForm` pour les scans** (les GUID des glTF sont des assets) — le
spawner ne sait rien en faire ; (5) `VegetationCollision` ne sait pas
retirer une boîte ; (6) trois copies du scatter (rendu ring 12, collision
ring 3×3, `giProps`) ; (7) la couche pending sait créer/désactiver des
références et `Displaced` sait sauver une pose libre — **la persistance
existe, l'identité manque**.

### 5.6 Chiffres

| Quoi | Valeur |
|---|---|
| Génération (`engine/terrain/generation/`) | 7 297 l. (dont `WaterSolve` 541, `Authoring` 279) ; `TerrainGen` 1 682, `TileBake` 1 388, `Hydrology` 915, `Finalize` 792 |
| Eau runtime | `WaterSim` 1 827, `WaterSystem` 2 447, `water_surface.glsl` 658 |
| Scatter/végétation/arbres/herbe | 4 950 l. + 974 de shaders ; `LandscapeScene.cpp` 7 990 (≈400 végétation) |
| Rendu consommateur du terrain | ~3 200 l. (`TerrainNoise`, `TerrainSystem`, `FarTerrain`, lightmap, shade map) |
| `game/` terrain | ~1 600 l. (`MapBaker` 386, `TerrainBakeStreamer` 697) ; `world/terrain/` ~1 200 |
| Tests | 803 cas au total ; génération 129 (23 skip), eau 63 dédiés (29 slow), scatter/arbres ~15 ; `SpawnDiagnosticTest` 2 004 l. |
| Grilles (carte 24 576 m) | réseau maître 321² @ 128 m ; stage 1 1921² @ 16 m (80 s) ; hydrologie 1665² @ 16 m (2 s) ; tranche ~2305² @ 2 m (36 × ≈ 12 s workers) ; région 2113² + masques 265² ; overview 481² @ 64 m |
| Sim d'eau | 257² @ 2 m, 30 Hz, 9,6-16,5 ns/cellule/substep (~2 ms/tick, ~130 ms/s à l'arrêt) ; pre-roll ~2 s |
| Scatter par chunk 64 m | arbres 64, rochers 64, cailloux 1 225, débris 16, plantes 441, masse 1 024, buissons 144, herbe ~181 k ; pool 1,3 M instances / 40 Mo |
| Données de base | 6 `CellForm`, 272 `ReferenceForm`, 19 `StaticForm` (2 « rock »), 1 `TerrainPatchForm`, 0 `TerrainRegionForm`/`WaterBodyForm`/`RiverForm`/`PrefabForm`/`BiomeMapForm`, 1 col ; aucune carte exportée versionnée |
| Versions | `kTileBakeVersion` 68, `kMapBakeVersion` 1, TRG3, TWB3, MOV1, TER1, TBM1 |

### 5.7 Validations dev jamais consignées closes (une seule table)

| Sujet | Source |
|---|---|
| Tour en jeu validant B2-B6 (familles/hauteurs) ; B8 vasques ; B9 (nager le fleuve, gué à pied, largeur) ; B9e/f embouchure | TERRAIN-GEN |
| M3b-5 départ en prairie ; `strataAmplitude` (revue visuelle avant activation) | TERRAIN-GEN |
| Verdict DA photo vs stylisé (A/B procédural vs CC0) ; transition neige/herbe post-wander ; coût POM/variety au F6 | TEXTURING / MEADOWS-PLAN |
| Prairie (espèces/clumps/zones), frontière herbe/roche, toggle A/B racine, compteurs F6 (herbe + clutter ≤ ~3 ms), SSDM garder/abandonner | GRASS-REDO |
| Settle-gate « petit creux » (critère d'étendue humide) | WATER-RENDER |
| Lightmap sur grille (pentes au couchant sans banding), GI rebake drift 0,25, normales RC stencil, grande session F6 | CPU-PERF |
| Comportement hors-carte (barrière + garde) | TERRAIN-MAPS |

---

## 6. Backlog consolidé (dédupliqué, hors chantier courant)

### 6.1 Génération et structure
- **Climat géographique** (spec dev 2026-08-25) : température = gradient
  latitudinal décrété par seed − **lapse rate ~6,5 °C / 1 000 m** sur
  l'altitude analytique + continentalité ; humidité = advection depuis la
  mer sous un **vent dominant décrété par seed**, distance à la mer sous le
  vent, **ombre pluviométrique**, bonus vallées maîtresses/côtes ; mêmes
  seuils (T, H, tier) ; le fbm actuel reste en octave de détail ; partage
  les décrets de vent de la météo.
- Peinture des cartes de contrôle (étages/uplift/biomes/mer au pinceau,
  `.tbm` en entrée) = mode Scénario complet ; `AuthoredControls` en S1.
- Ré-ancrage des deltas sculpt au re-bake (absTarget = oldBase + delta).
- Port compute S3/S5 (décision CPU à rouvrir ou confirmer : déclencheur
  ~10 s/tuile dépassé).
- Strates géométriques (revue visuelle), `cliffiness`/meshes de falaise,
  vallées glaciaires/fjords, tressage des deltas, recalibrage des cols,
  re-fit analytique complet, `BiomeErosion` en Form, tuning par
  worldspace (champs déjà déclarés).
- Transitions : couloir continu carte-à-carte ; franchissement d'un
  détroit sans col (auto-voyage ?).
- Mode story = second générateur de hauteur (à garder, fusionner ou retirer).

### 6.2 Végétation et sol
- `vegetationSet` par biome, espèces d'herbe/plantes en Forms, constantes
  du scatter en données ; set « sous-bois » ; pitch par instance ; BC7 des
  albédos de plantes ; clearance débris vs arbres du chunk voisin.
- Éboulis V2 (canal de déposition baké) ; publication TRG de `hardness` ;
  berges sableuses/plages lacustres ; decals splinés (chemins) ; virtual
  texturing (outillage 10×10 km) ; sheen roughness v2 ; override de splat
  peint (Scénario) ; retouche stylisée des sources CC0 (verdict DA).
- Collision eau/arbre (« l'eau rencontre les troncs ») ; esprit
  Végétation : canal `growth` unifié avec `FireGrid.regrow` (masques →
  herbe/buissons seulement).

### 6.3 Eau
- Settle-gate : stabilité de l'étendue humide ; polish FX (voile/écume de
  cascade, foam de rive des grands lacs, whitewater/stries/glints) ;
  cues cascade/pluie sur bodies + snapshot ; `updateTexture` RHI (et
  retirer `simMapA`) ; unification `WaterVolume` dans `WaterQuery` ;
  flottabilité Jolt (`Floater` à poser ou retirer) ; réflexion planaire
  des lacs ; Manning sur pentes douces ; `kSimCacheCap` si la traînée doit
  s'allonger ; matériaux d'eau sur la sim (lave) ; mapping tier → matériau.
- Export : `tier` et `fords` dans `RiverForm`/`RiverPointForm`.

### 6.4 Outillage et tests
- Tests de `bakeMap` et du streamer de carte ; re-basage des diagnostics
  sur la carte bornée ; banc Python offline versionné ; purge du code
  mort §5.2 ; mise à jour des en-têtes périmés (§5.4.12).

---

## 7. Le chantier PAYSAGE (courant depuis le 2026-10-05)

### 7.1 Périmètre (passation CARTES + demandes dev)

1. **Repasser la génération elle-même** — l'érosion par gouttelettes
   (dévetotée : solve global par carte) et tout ce que le solve global
   permet maintenant ; retomber sur la cible §4 sur la **carte bornée**.
2. **Le scatter spawne des objets interactifs** — rochers, puis troncs,
   buissons = références à la demande (saisie, feu, impact), avec
   persistance (§5.5).
3. **Le peuplement** (ex-B10-B13) — embouchures, scoring de sites, pads,
   chemins/cols/ponts, POI, calibration en jeu.

### 7.2 Règles de travail (rappel des mémoires et de CLAUDE.md)

- État des lieux total AVANT tout plan (fait : §5) ; plan **brique par
  brique**, une validation dev entre chaque ; un commit par brique.
- `meadows-tests -tse=slow` (Debug, ~9 s) avant chaque commit ; suite
  complète en **Release** avant chaque push ; jamais la complète en Debug.
- Tout changement de bake : banc offline d'abord, puis bump de
  `kTileBakeVersion`, puis `cooker pre-bake` ; tout correctif de sim d'eau :
  `cooker water-replay` sur dump. Relire §3.4-3.5 avant toute retouche eau.
- Graphisme = validation visuelle dev, **A/B obligatoire**, rendu commité
  seulement après validation ; perf : version qualité d'abord, knobs live.
- §2.11 reuse before build : le scatter interactif étend la couche pending
  + `Displaced` + le spawner ; le scoring lit les champs analytiques et
  l'étage 0 ; les chemins étendent `gentle` ; jamais de mécanisme parallèle.
- Défauts = identité bit-exacte ; une brique qui bouge `region.heights`
  ré-épingle le hash ; le golden scatter MSVC reste le seul rouge toléré.
- `MEADOWS_BOOT=sandbox` + `MEADOWS_BOOT_SECONDS` pour tester soi-même
  (Release, cwd = l'exe) ; le dev lance `cmake-build-debug-visual-studio`.

### 7.3 Plan d'assainissement retenu (dev, 2026-10-05) — paliers A → D avant tout réglage

Le but du dev est de **modifier les équilibres de la carte générée** ;
avant ça, le code s'assainit en quatre paliers ordonnés par dépendance
et par risque. Chaque brique est commitable seule ; critère commun :
suite rapide verte avant commit, complète en Release avant push.

**Palier A — purge à comportement nul** (hash `region.heights` du banc
tuile et golden scatter inchangés) :
- A1 génération : supprimer `kStage1Version` (orphelin) et réécrire le
  bloc de versions ; rafraîchir les en-têtes qui décrivent le composite
  3×3, les tuiles indépendantes et le « second fournisseur » de
  contrôles ; renommer `kBasinResolveMargin` en `kMapApron` ; retirer
  `WorldspaceForm.edge*` (mort-né) ; garder `mapSize/mapSeed/seaLevel/
  snowLine/dominantBiome` pour le palier C.
- A2 eau : retirer `simMapA`/`uWaterSimA`/`waterSimUv()`/`display` ;
  documenter `dryThreshold` comme seuil de statistiques du replay (ou le
  brancher) ; corriger les boîtes « Volumes debug » conditionnées à une
  fenêtre gelée ; corriger les commentaires `WaterSolve.hpp`/
  `WaterBodies.hpp` ; `Floater` gardé (support de la flottabilité) et
  dit tel quel. `WATER_FAR` (écume des corps proches) = changement
  visuel, hors palier A.
- A3 scatter : retirer `kPropCasterShader` local et les includes
  orphelins de `VegetationScatter.cpp`, le filtre `w < 0.35` de la
  collision, le préchargement `rock_cc0` écrasé (l'asset reste : il sert
  au kit de mine), les commentaires faux ; retirer `BiomeForm.
  vegetationSet` et `BiomeVegetationForm` (zéro lecteur, zéro record).
- A4 diagnostics : supprimer de `SpawnDiagnosticTest` les cas de
  calibration ponctuelle du monde infini ; garder les instruments
  (transects, vista, census, lacs, fleuves) pour B2.

**Palier B — la vérité des instruments** : B1 `MapBakerTest` (suite
slow : petite carte, manifest, overview, lecture des tranches, refus
hors rect, hash des hauteurs = le golden de la carte) ; B2 re-baser les
diagnostics sur la carte bornée (bordures, hydrologie globale, centre
de carte, critère de spawn extrait en fonction headless partagée avec
`probeSandboxSpawn`, `vista` sur l'overview érodé) ; B3 baseline de la
carte (0,0) seed 1337 consignée ici (§7.4).

**Palier C — paramètres en données et UI** : C1 `TerrainGenTuningForm`
réfléchi (groupes touchés par le rééquilibrage d'abord : familles,
fluvial, thermique, érosion fine, lacs, océan, bordures ; défauts = les
valeurs C++, test bit-exact contre B1) ; C2 clé de cache = hash des
paramètres résolus dans le manifest ; C3 panneau « Terrain generation »
exposant le Form (modifier → Bake → comparer → Accept), lecture
effective des champs de `WorldspaceForm` ou suppression.

**Palier D — un seul bump de format** : D1 TRG4 = champs eau morts
retirés + canaux `calm`/`gentle`/`trunk` (+ `deposit`/`hardness`) à 16 m,
un bump de `kTileBakeVersion`, un `pre-bake` ; D2 `tier` et `fords`
dans `RiverForm`/`RiverPointForm`.

Hors assainissement, volontairement : `Authoring` (attend le
peuplement), le mode story, l'identité des instances de scatter
(refonte « un scatter exécuté une fois, partagé rendu/collision/GI »,
fondation du scatter interactif, après D).

**Révision du 2026-10-05 (après B, décision dev)** : le dev veut, après
cette passe de réparation, **revoir la manière de générer les cartes**
(trop de bugs, paysages qui ne lui conviennent pas). Les paliers C et D
tels qu'écrits exposent et figent le générateur ACTUEL : ils sont
**remis à plus tard** — la promotion en Form vient quand le nouveau
générateur se fige (le précédent GRASS-REDO : « promotion des espèces en
Form quand le tuning se fige »), le bump TRG4 attend la refonte pour
n'être payé qu'une fois. Ne restent de C/D que deux briques
d'infrastructure, utiles quel que soit le générateur : **C'1 la clé de
cache = hash des entrées de données** (`mapBakeKey` dans le manifest,
`kMapBakeVersion` 2) et **C'2 le tier des rivières dans l'export**
(`RiverForm.tier`, `flowSpeed` dérivé par la loi unique
`riverFlowSpeedForTier`). Les gués restent côté bake (`.twb`) : ce sont
du terrain carvé, sans consommateur runtime — un record sans lecteur
serait la donnée morte purgée en A3. Après ça : **la refonte s'ouvre par
un document de design avant tout code** (§7.5).

**Ensuite, le réglage** : gouttelettes (passe séquentielle sur le
stage 1 global, dépôt = éboulis V2) → cible §4 re-mesurée →
embouchures → scoring de sites + pads (brancher `Authoring`, miroir
analytique) → chemins/cols/ponts (détection de selles) → POI → scatter
interactif → calibration en jeu.

### 7.4 Journal

- **2026-10-05** — Ouverture. Six journaux consolidés dans ce document
  (originaux sous `docs/archive/`), état des lieux total (§5) par six
  agents (génération, eau, scatter, peuplement, deux digests). Plan
  d'assainissement A-D retenu par le dev (§7.3).
- **2026-10-05 — Palier A livré (un commit par brique, comportement
  nul).** A1 génération : `kStage1Version` supprimé, bloc de versions
  réécrit autour de `kTileBakeVersion` seul ; `kBasinResolveMargin` →
  `kMapApron` ; en-têtes de `TileBake.hpp`, `TerrainGen.hpp`,
  `TerrainBakeStreamer.hpp`, `SandboxTerrain.hpp` réécrits pour le
  pipeline par carte ; `WorldspaceForm.edge*` retirés
  (`hillChainWavelength` doublon GARDÉ : le défaut 0 de `MacroParams`
  sert aux tests « chain-free », pas un comportement nul). A2 eau :
  `simMapA`/`uWaterSimA`/`waterSimUv()`/`WaterSimSnapshot::display`
  supprimés (binding 7 gelé comme 5/6) ; boîtes « Volumes debug »
  dessinées sans fenêtre gelée ; `dryThreshold` documenté comme seuil
  du replay ; en-têtes `WaterSolve.hpp`/`WaterBodies.hpp` corrigés ;
  `Floater` gardé et dit sans producteur. A3 scatter : includes
  orphelins et `kPropCasterShader` local de `VegetationScatter.cpp`,
  filtre `w < 0.35` de `VegetationCollision`, préchargement `rock_cc0`
  de `WorldRenderer` (l'asset reste, le kit de mine s'en sert),
  commentaires `Instance.params.w`/`GiProp.kind` ; `BiomeForm.
  vegetationSet` et `BiomeVegetationForm` retirés. A4 diagnostics :
  six cas ponctuels du monde infini supprimés (`spawn diagnostic` ×2,
  `regime`, `height`, `spawn debris`, `analytic sea mismatch`) ; les 15
  instruments restent (en-tête du fichier = la dette B2). Vérification :
  suite rapide 684/684 après chaque brique, golden scatter inchangé,
  smoke-run Debug sandbox après A2 (Vulkan, 0 erreur de validation),
  **A/B du banc tuile** (`-tc="*bake benchmark*"`, Debug/MSVC) : hash
  `region.heights` **8562263924616419862** avant (`e3ae2b0`) et après
  (`781d476`) le palier — bit-identique. Ce hash est la référence
  Debug/MSVC du banc à `kTileBakeVersion` 68 ; le `4414106998705828656`
  de `docs/CPU-PERF.md` date d'avant CARTES (bakeSoloTile fenêtré) et
  n'est plus un comparateur. Non fait : la suite complète en Release
  (gate de push).
- **2026-10-05 — Palier B livré.** B1 : `render::sandboxFallbackHeight`
  (UNE implémentation du sol de repli : overview puis analytique +
  bordures, consommée par `proceduralBase`, bit-exact), `spawnCandidateOk`
  + `probeMapSpawn` (la sonde du jeu extraite en headless, la scène
  l'appelle), `tests/MapWorldFixture.hpp` (la carte bornée derrière
  `TerrainParams`, depuis `MEADOWS_MAP_CACHE`, le cache du jeu s'il est
  valide, sinon un bake temporaire), `tests/MapBakerTest` (suite slow,
  carte 2×2 de 8 km avec bordures : manifest, overview, tranches,
  streamer headless, dérive overview/sol 23,9 m, **hash de contenu
  6429626604634228367** — MSVC, Debug == Release, v68 ; 23 s en Release,
  3 min en Debug). B2 : les 14 instruments de `SpawnDiagnosticTest`
  re-basés sur la carte (0,0) (spawn = la sonde partagée, census de
  l'intérieur hors bande de chaîne 2 560 m, vista sur l'overview érodé,
  `fleuve continuity` = continuation des runs aux lignes de tranche).
  **B3 — BASELINE de la carte (0,0), seed 1337, v68 (Release, cache du
  jeu, spawn (14559, 32, 11023))** — le point zéro de tout réglage :

  | Instrument | Mesure | Cible §4 |
  |---|---|---|
  | family census (intérieur, fenêtres 250 m) | **socle 3,0 % (plateau 0,4) / versant 81,6 % / drame 15,5 %** ; relief médian **114 m** (socle 12 m, versant 100 m) ; 1 275 fenêtres en eau, 3 440 en bande de bordure | 40 / 35 / 25 ; 20-40 m |
  | variety transect E-O (16 km par le spawn) | socle 16 % / versant 51 % / drame 32 % ; médian 64 m ; plates 2,7 % ; > 30° 18,7 % ; infranchissable 625 m ; événements tous les 257 m (pire trou 1 025 m), type dominant 77 % (relief) ; 0,32 croisement d'eau/km | plates 15-35 %, > 30° < 15 %, ≤ 400 m, types alternés |
  | variety transect N-S | socle 34 % (plateau 14) / versant 63 % / drame 3 % ; médian 24 m ; plates 17 % ; > 30° 4,4 % ; événements tous les 412 m (pire 1 250 m), dominant 68 % | idem |
  | calm coverage (stage-1 de carte, intérieur) | calm > 0,6 : **60,9 %** des cellules sèches (contrôles seuls 51,4 %) ; calm > 0,3 : 71,2 % | ~40 % de socle |
  | vista (5 points de voyage retenus sur 9) | horizon ouvert ≥ 30/72 : **3/5** ; amer > 2° à > 3 km : 5/5 ; sommet alpin ≤ 8 km : 5/5 (à 0,25-1,4 km) ; colline marquante ≤ 4 km : 5/5 (0,8-1,3 km) | ouvert sur la majorité ; objectifs à 3 et 6 km |
  | erosion calibration (publié − analytique, par bande d'altitude) | +2 m sous 100 m ; **−60 m (100-200), −122 (200-300), −194 (300-400), −230 (400-500), −242 (500-600), −241 (600-700), −194 (700-800), −196 (800-900), −360 (900-1000), −398, −452 (1100-1200)** ; keep 0,04 → 0,50 | miroir à refitter (palier C/réglage) |
  | erosion strength (tranche (5,5), max publié 1 749 m, solo sans bordures) | défaut : max 1 259, moyenne au-dessus de la mer 318 m (p50 272, p90 522) ; sans arrondi : 318 ; **sans érosion ni arrondi : 745 m** (p50 638, p90 1 340) ; fluvial 60 it : 355 ; 40 it : 437 | — |
  | lake census (carte entière) | **772 lacs naturels = 20,5 par 4×4 km** (+ 114 étangs placés) : < 0,1 ha 11, 0,1-0,5 ha 224, 0,5-2 ha 261, 2-10 ha 171, > 10 ha 105 (le plus profond 200 m) ; runs : ruisseau 579 / rivière 89 / fleuve 56 (24,8 km) ; 43 gués | lacs = destinations, critère qualité |
  | river wetness | 724 runs, 291,7 km, 86 runs < 100 m ; axe à sec 1,9 % (pire 232 m) ; plus long bief plat de fleuve 169 m | — |
  | fleuve continuity (lignes de tranche intérieures) | 22 bouts de runs, **22 continués, 0 orphelin, 0 écart de tier** | — |
  | fleuve locator | 22 cours maîtres touchent la carte ; le plus long 17 km ; le plus proche à 871 m du spawn | un fleuve par région |
  | biome locator (intérieur, terre) | **alpin 46,2 %**, tempéré 30,2 %, toundra 9,9 %, subalpin 7,8 %, aride 3,1 %, steppe 2,8 % | — |
  | snow coverage (adopté 900/+150/−180/−300) | plein 1,6 %, touché 6,1 % ; par bande : 600-900 m 16 %, 900-1 200 m 56 %, > 1 200 m 88 % | — |
  | proportion (contrôles, carte) | mer 23,5 % de la carte ; terre : plaines 16,5 %, collines+plateaux 55,6 %, montagnes 27,8 % | mer ~20-25 % |
  | coast (analytique) | 351 échantillons de rivage, 6 % en mode falaise (48 % d'entre eux > 40 m de rebord) | — |

  **Lecture** : le socle calme existe dans le stage-1 (61 % de calm) mais
  n'arrive **pas** au sol publié (3 % de fenêtres socle, relief médian
  114 m) — la dissection des versants mange tout, les « sommets » sont
  partout (à 0,25-1,4 km de chaque point de voyage), 20 lacs par 4×4 km,
  le miroir analytique surestime de 200 à 450 m au-dessus de 300 m. C'est
  le diagnostic que le réglage doit attaquer, dans l'ordre socle → lacs →
  miroir. Les instruments sont maintenant comparables entre deux bakes.
- **2026-10-05 — C'1 + C'2 livrées, assainissement CLOS.** C'1 : le
  manifest de carte porte `key` = FNV de (`kTileBakeVersion`,
  tilesPerSide, seed, tileSize, seaLevel, recurve ×3, bordures) ;
  `mapBakedAndValid(..., &params)` le compare — streamer, éditeur
  (« Accept cached map »), `cooker pre-bake` (bordures posées AVANT le
  test de validité : un pre-bake et le jeu ont la même clé) et la fixture
  passent leurs params. `kMapBakeVersion` 1 → 2 : **les caches existants
  sont re-bakés une fois** (≈ 94 s en fond au prochain boot sandbox).
  C'2 : `RiverForm.tier` exporté, `flowSpeed` dérivé par
  `riverFlowSpeedForTier` (la scène et l'export partagent la loi) ;
  `MapRecordsTest` vérifie le round-trip d'un fleuve.

### 7.5 La refonte de la génération — document de design (2026-10-05, à arbitrer)

#### 7.5.1 Le constat du dev (ses mots) et ce que les mesures en disent

| Reproche / bug | Ce que l'inventaire et la baseline montrent | Où ça se joue |
|---|---|---|
| **Résidus de rivières aériennes, rivières qui courent au-dessus du sol** (réglé en partie, pas totalement) | `river wetness` : 1,9 % de l'axe des runs publiés est à sec, pire tronçon continu 232 m, 86 runs < 100 m sur 724 ; les rubans bakés se rendent au-delà du rect de la sim à la surface de leurs nœuds (profil rebâti aval→amont sur le sol FINAL, mais le détail runtime et le sculpt s'ajoutent après) ; le far-water trace les fleuves maîtres sur l'analytique là où aucune tranche n'est publiée — et l'analytique surestime le sol de 200 à 450 m (erosion calibration) ; les 56 runs de fleuve de la carte (0,0) courent à 300-500 m d'altitude le long du bord ouest, suspects | `reconcileRiversWithTerrain`, `buildLocalGeometry`, `collectFarWater`, le miroir analytique |
| **Cellules qui changent sans continuité** | à l'arrivée d'une tranche, le sol passe de l'overview 64 m au 2 m : dérive mesurée jusqu'à **23,9 m** (MapBakerTest) — un saut sous les pieds et sous les objets ; la première session d'une carte neuve roule entièrement sur l'analytique (overview chargé au boot seulement) ; les anneaux scatter/herbe/collision sont recréés entiers à chaque publication | `publishBakedTiles`, `proceduralBase`, `applyMapWorld` (pas de hot-swap de l'overview) |
| **Les distances sont trop longues** | une carte fait **24,6 km de côté ≈ 600 km²** (intérieur hors bandes de bordure ≈ 380 km²) ; Skyrim ≈ 37 km² (~6 km de côté), Hyrule de BotW ≈ 60-80 km² (~8-10 km), l'Entre-terre d'Elden Ring ≈ 80 km² (estimations publiques, ordre de grandeur) : **nos cartes font 8 à 16 fois ces mondes** ; traverser un côté = 75 min de course (5,5 m/s) contre ~20-30 min chez les références ; les structures sont taillées pour un monde infini (vallées maîtresses tous les 9,5 km, pics tous les 7 km, cols tous les 4 km, villes tous les 8-12 km au §5) | `kMapTilesPerSide`, `ProceduralControlParams` (toutes les longueurs d'onde), le tableau §4-§5 |
| **Manque de variété par endroits, paysages très étonnants ailleurs** | `family census` : socle 3 % / versant 82 % / drame 15 %, relief médian 114 m — un hachis uniforme de versants (le « toujours le même événement » du constat 2026-08-24, jamais résolu) ; `vista` : un « sommet alpin » à 0,25-1,4 km de CHAQUE point de voyage — le drame est partout donc nulle part ; alpin = 46 % de la terre intérieure ; 772 lacs (20 par 4×4 km, 105 de plus de 10 ha, le plus profond 200 m) ; le miroir analytique a 60 % de `calm` que l'érosion ne respecte pas (socle publié 3 %) | la couche contrôles/macro S1 (layout continental, familles, amers, vallées), le budget d'érosion |

Diagnostic : **les noyaux marchent, la couche du dessus fabrique le
mauvais monde.** Fastscape, thermique, priority-flood, hydrologie,
finalize, bordures, pipeline par carte, eau C : tous grid-agnostiques,
survivants de CARTES, instrumentés. Ce qui ne va pas, c'est ce qu'on leur
donne à manger — une macro de 1 000 km de continent, calibrée par
briques successives sur un monde infini, puis découpée en cartes de
600 km² — et le budget d'érosion qui la dissèque uniformément.

#### 7.5.2 Pourquoi le mode histoire paraît plus naturel (ses règles)

Le mode histoire n'a **aucune érosion** et presque aucune structure : un
fbm de collines à **500 m de longueur d'onde, ±75 m** (5 octaves), des
montagnes ridgées à **2 km, 270 m**, masquées (seuils 0,45-0,75 : plaines
ailleurs), mer à 21 m, neige à 165 m — et **253 références placées à la
main** (`village.toml`) sur un pad nivelé. Trois règles s'en dégagent :

1. **Le relief est à l'échelle du joueur** : une colline se traverse en
   une minute, une montagne se contourne en cinq ; rien ne dépasse 350 m
   au-dessus de la plaine. Nos cartes mettent 1 200-1 700 m de pics et des
   versants ravinés partout : l'échelle d'un massif réel, pas d'un jeu.
2. **Le calme est la règle, le drame l'exception** : le masque garde des
   plaines partout où le bruit de montagne n'est pas fort ; le sandbox
   fait l'inverse (socle 3 %).
3. **Les points d'intérêt sont placés, pas émergents** : le village, les
   portes, les marqueurs sont des décisions ; le sandbox n'a aucun
   placement piloté par le terrain (§5.3).

Ce que le mode histoire n'a pas et que le sandbox doit garder : les
vallées lisibles et les lits de rivières (érosion + hydrologie), l'eau
vivante, les bordures, les biomes, la carte exportable en mod.

#### 7.5.3 Décision de fond proposée : repartir sur des bases saines, garder les capacités

- **La carte devient petite et lisible : 2×2 tranches = 8,2 km de côté
  (≈ Skyrim), 3×3 = 12,3 km (≈ Hyrule) au plus.** C'est le premier levier,
  pas un réglage : il change l'échelle de toutes les structures, divise le
  bake par 4 à 9 (**23 s en Release pour une carte 2×2, mesuré par
  MapBakerTest**), rend le banc offline et l'A/B visuel quotidiens, et
  ramène les distances du §5 à celles des références (ville ↔ ville = la
  carte, village tous les 2-3 km, hameau ou curiosité tous les 500-800 m).
- **Une nouvelle couche macro (« S1 v3 ») à l'esprit du mode histoire** :
  un **plan de carte** explicite et déterministe par seed — quelques
  grandes pièces posées (UN massif ou plateau dominant, UN fleuve ou lac,
  UNE côte ou deux, deux ou trois crêtes-amers avec leurs cols), un fond de
  collines douces à l'échelle histoire (500 m / ±75 m) partout ailleurs,
  et un **budget d'érosion modeste** qui sculpte les versants sans toucher
  aux socles (le calm devient une contrainte dure du fastscape, pas une
  modulation). Le layout continental à 1 000 km, les grilles d'amers
  jitterées, le champ de vallées à 9 km, la porteuse, le décret « pays de
  départ » sont **remplacés** par ce plan de carte, à l'échelle de la
  carte.
- **Les points d'intérêt par règles, à la densité du mode histoire** : le
  plan de carte réserve les sites (pads, confluence, col, rive, belvédère)
  et le peuplement les remplit — c'est l'ex-B10-B13, qui devient simple
  sur une carte de 8 km.
- **Les bugs d'eau et de continuité se règlent dans le même mouvement** :
  les rubans et le far-water n'ont plus d'analytique sous eux (une petite
  carte est entièrement bakée avant d'être jouée : pre-bake à la création,
  overview chargeable à chaud), les rivières se rebâtissent sur le sol
  COMPLET (détail + sculpt inclus, la règle 6 de l'eau).

#### 7.5.3 bis — Décisions dev du 2026-10-05 (discussion après le premier jet) et correction du plan

**Cadre acté par le dev :**
- **Le monde de travail est le SANDBOX : un nombre INFINI de cartes**,
  chaque carte se générant quand le joueur s'en approche, **différente
  selon la seed**. C'est sur ce monde que se construit la boucle de
  gameplay.
- **Le mode histoire est un mode « tiré puis peint et édité »** : une
  carte tirée par le générateur, puis retouchée à la main (peinture,
  édition, contenu placé). **Ce n'est pas la première priorité.**
- **Les paysages doivent s'étendre de carte en carte** : une petite carte
  n'est pas une île ; une vallée, une côte, un massif continuent chez le
  voisin.
- **Le rythme ne préjuge pas de l'altitude** : le rythme du mode histoire
  (relief à l'échelle du joueur) s'applique à toutes les cartes, mais
  certaines sont en altitude, d'autres sur des côtes et des îles,
  d'autres en haute montagne. L'élévation globale change de carte en
  carte.

**Ce que ça corrige dans le premier jet (§7.5.3) :**
- Le « plan de carte » posé par seed carte par carte ne suffit pas : il ne
  garantit ni la continuité entre cartes ni la distribution des étages.
  La macro v3 devient **deux couches** :
  1. **L'étage monde** — continu sur tout le graphe de cartes, basse
     fréquence (texel ~500 m-1 km), **fonction pure et bon marché de
     (seed, x, z)** pour que le monde reste infini et seedé : altitude de
     base de la région, mer et îles, massifs qui enjambent plusieurs
     cartes, climat. Il remplace le layout continental à 1 000 km et ses
     ~45 paramètres par quelque chose de plus simple, à l'échelle de
     cartes de 8-12 km. (Une version PEINTE de cette couche est l'outil
     du mode histoire : plus tard.)
  2. **Le rythme local** — à l'échelle du joueur, RELATIF à l'étage :
     collines ~500 m de longueur d'onde, massif local, lits, dimensionnés
     par l'étage (grand massif en haute montagne, falaises et criques sur
     une côte) ; plus les points d'intérêt placés par règles.
- **Les frontières prennent leur style de l'étage monde, pas d'un
  hachage** : chaîne ou bras de mer là où l'étage met un massif ou une
  côte, **ouverte** partout ailleurs (ni lift ni creusement). Hypothèse à
  MESURER (sweep de divergence de `bake-map`) : avec un relief à érosion
  modeste et le calme en contrainte dure, la divergence sur une frontière
  ouverte devient petite ; si elle reste trop grande quelque part, la
  règle chaîne/mer reste disponible localement. L'hydrologie maître est
  déjà continue entre cartes ; les ruisseaux locaux peuvent différer au
  bord : à instrumenter.
- Les briques R1-R3 se réordonnent : la taille de carte vient AVEC
  l'étage monde et les frontières ouvertes, pas avant (une carte de 8 km
  ceinturée de chaînes tous les 8 km serait pire que l'actuel).

#### 7.5.4 Ce qui survit tel quel / ce qui est refait / ce qui tombe

| Survit tel quel | Refait | Tombe |
|---|---|---|
| noyaux : `erodeFluvial`, `erodeThermal`, `priorityFloodFill`/`routeFlow`, `extractHydrology`/`classifyRivers`, `finalizeTerrain`, `amplifyFine`, reconcile v2 | **`ProceduralControls::at` + `synthesizeMacro` + `landHeight`** → le plan de carte (S1 v3) | layout continental 700 km, porteuse 20 km, grilles d'amers 7/3,5 km, `ValleyField` 9 km, `startMeadowRadius`, le re-fit « érosion-aware » du miroir (il n'y a plus de miroir à grande distance à mentir) |
| pipeline par carte (`bakeMap`, tranches, overview, manifest + clé, streamer), bordures v2, `MasterNetwork` (re-dimensionné à la carte), eau C, matériaux, scatter, instruments B2, hash B1 | `kMapTilesPerSide` et toutes les longueurs d'onde (rescalées à la carte) ; le budget d'érosion (keep dur sur calm) ; `probeMapSpawn` (lit le plan) | `SpawnDiagnosticTest` ancrages 24 km (à rescaler) |
| modding §5 (carte = plugin, `tier` exporté), cols/voyage | `collectFarWater` et les rubans hors rect (plus d'analytique sous l'eau) ; hot-swap de l'overview | le mode story comme second générateur ? **à décider** : il peut devenir « une carte dont le plan est vide » (bruit histoire + contenu placé) — un seul générateur, deux orchestrations, enfin |

#### 7.5.5 Méthode (déjà en place)

Hash de carte (B1) et instruments B2 avant/après chaque brique, banc
offline (`bakeSoloTile`, `erosion-bench`) avant tout bump, A/B visuel
dev avec la carte 2×2 (23 s), un commit par brique, suite rapide avant
commit, complète en Release avant push. Le golden de la carte bouge par
design à chaque brique de génération et se ré-épingle.

#### 7.5.5 bis — Journal de la passe « nouvelle base »

- **2026-10-05 — N1 livrée : la carte de 8 km.** `kMapTilesPerSide` 6 → 2
  (une source, le streamer la lit), `WorldspaceForm.mapSize` 8 192,
  bandes de bordure rescalées (chaîne 900 m / lift 260 m, mer 900 m,
  méandre ±300 m / 3 km, crête 2 km, fondu 900 m), sonde de spawn en
  anneaux de 350 m depuis 600 m (la spirale était VIDE à 8 km), écran M
  sur la carte active (clé de raster = carte, plus l'id de worldspace),
  **passage de carte automatique** (sortie du rect de 8 m → `travelToMap`
  + voile ; triggers du Col de l'Est retirés, `passes.toml` reste pour
  les mods), prefetch des diagonales, **overviews des voisines**
  (`SandboxTerrain::neighbourOverviews`, lus par `sandboxFallbackHeight`
  après l'active, chargés à `applyMapWorld` et toutes les 2 s quand un
  prefetch atterrit, avec un événement de contenu sur ce rect),
  far-water sur les dossiers des 3×3 cartes, `masterRiversNear` élargi
  de l'apron, `kTileBakeVersion` 69, hash `MapBakerTest`
  13372118313942275723 (MSVC). Bake de la carte (0,0) : **18 s en Release**
  (stage-1 14 s + 4 tranches 2,3 s), 126 s en Debug. Smoke-run Debug :
  overview 225² chargé, 4 tranches publiées, 0 erreur de validation.
  **Baseline 8 km « taille seule »** (carte (0,0) seed 1337, même macro) :
  la carte est un **plateau de 334 à 1 026 m sans mer** (spawn au centre
  (4096, 608) — aucun candidat tempéré bas : le décret de départ vient
  en N2) ; socle **10,9 %** / versant 79,4 % / drame 9,8 %, relief médian
  **69 m** (socle 10, versant 71) ; calm stage-1 63 % ; transects E-O
  33/42/25 (médian 29 m), N-S 29/71/0 (33 m) ; 60 lacs naturels (14 par
  4×4 km) ; 65 runs, 23 km, fleuve 5 runs (1 km), 4 gués ; axe à sec
  1,4 % ; vista 6 points : ouvert 4/6, sommet ≤ 3 km 6/6 (à 0,5-2 km),
  colline ≤ 1,5 km 4/6 ; alpin 68 % de la terre, tempéré 22 % ;
  calibration : analytique +180 à +217 m SOUS le baké à 100-200 m,
  −100 à −390 m au-dessus de 400 m. Non vérifié en jeu : le passage
  automatique de carte (validation dev).
- **2026-10-05 — N1, retours dev sur le passage de carte.** (1) Le
  passage ne se déclenchait pas en spectateur : le test de sortie du
  rect vivait sous `simPaused` — sorti du bloc, gardé par `uiPaused`
  seul. (2) « On a l'air de se retourner » : `placeStartCamera` remettait
  la pose de départ (yaw π) à chaque passage — un passage automatique
  (arrivée fournie) ne re-seate plus que la position
  (`sandboxKeepHeading`, couvre aussi le re-placement du warmup).
  (3) « La végétation et la nature du sol changent » : `applyMapWorld`
  repartait d'une base vide (régions d'auteur seules) — le sol derrière
  le voyageur tombait sur l'overview de 64 m de la carte quittée, et
  matériaux/scatter se recalculaient sur ces hauteurs. Désormais les
  tranches (et lacs/rivières) de la carte précédente **hors du nouveau
  rect restent résidentes** ; l'éviction par distance de
  `publishBakedTiles` les élague comme celles de la carte active ; les
  tranches DANS le nouveau rect sont lâchées et rechargées du cache par
  le streamer neuf (son ensemble publié part vide — pas de doublon).
  C'est le « passage sans couture » minimal ; la divergence réelle des
  deux bakes DE PART ET D'AUTRE de la ligne (sol et végétation
  différents à la ligne même) reste le sujet de N3/N4.
- **2026-10-05 — N1, le départ sur une pente (retour dev).** Sans
  candidat tempéré bas (la carte (0,0) à 8 km est un plateau alpin),
  la sonde renvoyait le centre : (4096, 608, 4096), une pente. Désormais
  `probeMapSpawn` fait une seconde passe : **l'endroit sec le plus doux**
  sur les mêmes anneaux (pire marche de hauteur sur 100 m en 8
  directions, sur l'overview ; l'anneau proche gagne un quasi ex æquo).
  Leçon mesurée au passage : sans oracle d'eau, la passe choisissait le
  **fond du lac le plus bas** (332 m, le minimum de la carte — un lit de
  lac est le sol le plus plat qui soit), et la relocalisation « sec »
  du warmup posait le joueur sur sa rive à **17,7 m / 30 m**. La sonde
  prend donc un oracle `wet` : en jeu, les lacs (masques grossiers) et
  rubans de fleuve du cache `.twb` via `collectFarWater` (nouveau
  `farWaterWetAt`, `makeFarWaterProvider` partagé avec la couche far du
  WaterSystem) ; dans les instruments, les `WaterBodies` réels. Résultat
  carte (0,0) : départ (5113, 605, 4353), sec, **2,9 m / 30 m**, à 1 km
  du centre sur le plateau. Nouvel instrument `spawn slope diagnostic`
  (sonde, relocalisation du jeu, pentes, plus doux par anneau).

- **2026-10-05 — N2 livrée : l'étage monde + le rythme local (« macro
  seule », érosion inchangée).** Nouveau `engine/terrain/generation/
  WorldLayer.hpp/.cpp` : `worldSampleAt` (porteuse continent 45 km
  contrastée ×1,6, déformée 12 km / 1 500 m, détail de côte 5 km sur la
  ceinture seule, étage 28 km contrasté ×1,3, massif 26 km fenêtre
  0,50-0,74 gaté par l'étage, climat 9 km + dérive 200 km + lapse
  0,25/km) → `WorldSample { base, continent, etage, massif, coast,
  temperature, moisture, sea }` ; table de provinces linéaire (0→10 m,
  0,40→50, 0,52→150, 0,60→300, 0,72→520, 0,85→800, 1→1 100) + lift de
  massif 350 m ; rampe intérieure sur la PORTEUSE (pas le détail de
  côte, qui falaisait) de largeur 0,08·(1,5+4e) ; **décret de départ sur
  le plancher lui-même** (rayon 6 km, fondu 16 km : `alt → min(80,
  40+0,06·(alt−40))`, massif ×(1−pull), climat tiré vers 0,50/0,55) —
  un tirage sur le champ d'étage traversait la pente raide de la table
  (22 % mesurés) ; `etageIndexFor`/`etageAltitudeFor`/`paletteIdFor`.
  `ProceduralControlParams { seed, world, rhythm }` (layout continental,
  porteuse régionale, régimes, houle, grilles d'amers, champ de vallées,
  trunks, climat local : SUPPRIMÉS, ~600 lignes) ; `RhythmParams`
  (pièces 7 km / 80 % / 900-1 800 m, hauteurs par étage, cols de crête
  1 300 m, crêtes 1 800 m par étage 30/60/90/200, lits 3 000 m par
  étage 14/22/20/35, cols 2 500 m, dureté 4 km). `ControlSample +=
  base, hasBase, bedDepth` ; `pieceLayer` (dôme / échine modulée à
  cols / mesa, hauteur à l'étage du CENTRE, **la cellule du départ a
  toujours sa pièce**) ; `at` v3 dérive tout d'un échantillon monde ;
  `biomeIdAt` = même échantillon. `landHeight` v3 : plancher = mer +
  base (table d'étages = repli peint/test, colonne altitude 40/150/450/
  1 200, relief 25/700, 45/900, 28/750 terrasse 0,5, 120/1 100) + relief
  + pièce + crêtes − lits (ridged 3 km, nouveau) ; `macroHeightAnalytic`
  sans compression (égal à la synthèse ponctuelle hors côte, testé),
  proxy de rivage `kShoreProxy` 0,5 (non calibré). `kCalmKeep` et la
  relaxation calme restent (N3). Outil `terrain-map` : options
  porteuse/layout retirées. `kTileBakeVersion` 70, hash `MapBakerTest`
  16738992311582594802. Tests : 3 cas supprimés (régimes, amers,
  trunks), 7 nouveaux (continuité à la ligne, départ garanti 8 seeds,
  distribution 200 km, bornes v3, calme = la règle, ~1 pièce/carte,
  analytique = synthèse) + instrument `macro transect diagnostic` ;
  `MasterNetworkTest` « imprint » auto-ancré sur le plus long cours.
  **Mesures monde (200 km, seed 1337)** : mer 31,8 % ; terre < 150 m
  47,8 %, 150-450 32,7 %, 450-800 15,6 %, ≥ 800 3,9 % ; massif > 0,5 :
  11,7 % ; calme > 0,6 : 86,9 % ; pièces 0,97 par carte (pire cellule
  4) ; saut de plancher à la ligne 0,75 m ; pire pente de plancher 13 %
  (bout raide de la table — borne de test 15 %). Rendus `terrain-map`
  24/100/400 km : continents et mers lisibles, provinces, massifs,
  pièces ; le disque de départ ne fait plus cratère.
  **Baseline 8 km « macro seule »** (carte (0,0), bake 18 s Release) :
  la carte est la plaine du décret (plancher 60-100 m, aucune mer, un
  dôme forcé ~250 m) ; census intérieur **socle 68,6 % / versant
  31,4 % / drame 0 %**, relief médian **12,5 m** (socle 9,5) ; transects
  plats (< 8 m) 52-68 %, médian 5-7 m ; vista : ouvert 9/9, sommet ≤ 3 km
  0/9, colline ≤ 1,5 km 0/9 ; 33 lacs naturels (7,9 par 4×4 km), 70
  runs ; spawn (3 852, 73, 3 548), 1,6 m / 30 m ; calibration
  analytique − baké : bande 0-100 m −28 m, 100-200 **−97 m**, 200-300
  **−156 m** (keep 0,12). **Lecture** : l'étage et le rythme sont là
  (pièce, plancher, climat tempéré), mais l'érosion actuelle (80 it.,
  calme ×3 érodable, relaxation 0,75 vers la moyenne 160 m) rabote le
  rythme ±25 m à ±7 m et creuse la pièce de 150 m — exactement le
  budget que N3 doit imposer. **À traiter en N3/N4** : (1) le budget
  dur `maxCut` (le socle doit garder ses ±25 m, la pièce son sommet) ;
  (2) les **fleuves rectilignes** du réseau maître sur la plaine du
  départ : le disque bas en pays haut est un bassin fermé que le
  priority-flood remplit, et le routage sur la surface remplie trace des
  droites (visible sur le rendu 24 km et dans les 58 runs fleuve à
  hw 72) ; (3) 33 lacs sur une plaine plate. Leçon outillage : voir la
  mémoire « ninja header deps » (les en-têtes ne déclenchent AUCUNE
  recompilation dans les dossiers VS2026 : purge des .obj après tout
  changement d'en-tête partagé).

- **2026-10-05 — Retour dev sur N2 : « tout est plat, une plaine aqueuse
  morne ; je veux des paysages qui changent souvent et qui donnent envie
  d'aller de point d'intérêt en point d'intérêt. »** Deux causes dans
  N2 : le décret de départ aplatissait TOUTE la carte (rayon 6 km +
  fondu 16 km → un disque bas en pays haut = un bassin fermé que le
  priority-flood du réseau maître remplit : fleuves rectilignes, lacs)
  et les pièces étaient rares (une par 7 km). **N2b** (`424cd30`) :
  le champ d'étage est **re-basé sur le départ** (`etageAnchorShift`,
  décalage constant mémoïsé par seed qui s'efface entre 12 et 36 km —
  le pays autour garde sa propre variation, aucune cuvette), la prairie
  n'est que les premiers 1,2 km (fondu 4 km), l'anneau de pays bas
  (4-12 km) n'impose que terre / pas de massif / climat tempéré ;
  **bancs** à 7 km (±20 % du plancher) ; **pièces tous les 2,4 km**
  (85 %, rayon 350-1 000 m, hauteurs par étage 80-220 / 100-280 /
  80-220 / 200-500 m — à 40-140 m une butte disparaissait derrière le
  roulis de la plaine), visibles jusqu'au rivage (porte 2-12 m) ;
  rampe côtière sur la porteuse seule. Monde mesuré (200 km hors
  ancre) : mer 34,9 %, bas 51 / collines 31 / plateau 14,5 / haut
  3,7 %, massif 13 %, calme 77 %, pièces 7 par carte (jamais 0 sur une
  carte terrestre), pente de plancher intérieure ≤ 13 %. Rendus 10/24/
  100 km : roulis vert, pièces tous les 2-3 km, rivières qui méandrent.
- **2026-10-05 — N3 livrée : le budget d'érosion dur.** `erodeFluvial`
  prend `maxCut` (plancher par texel dans le sweep implicite : un
  receveur plancher résout ses donneurs contre la valeur plancher, le
  knickpoint s'arrête ; `nullptr` = legacy bit-exact, testé). Stage-1 :
  `calmCut` 6 m sur le calme, `roughCut` 250 m ailleurs (fondu sur
  `calm` 0,25-0,75), 24 m sur les pièces (un amer dessiné se dissèque
  légèrement, jamais jusqu'à la plaine), rough sur l'empreinte et les
  cols de crête de bordure ; boost d'érodabilité des plaines ×1,3 (au
  lieu de ×2), **érodabilité calme ×(1+0,8·calm) retirée, relaxation
  calme SUPPRIMÉE, `kCalmKeep` supprimé**, `kMapBorderRidgeKeep` 0,6 ;
  fluvial 48 it. / uplift 1,5 (l'étage porte l'altitude). **Comblement
  des creux locaux** avant l'érosion : sur une copie à 64 m, passe-haut
  (moyenne ~1 km retirée — un flood de la surface brute noyait la carte
  entière jusqu'à son déversoir lointain), priority-flood des creux à
  leur déversoir local, profondeur rendue bilinéairement sur le calme
  (142 → 108 lacs seulement : les lacs ne viennent pas des creux de la
  macro). `TileStage1 += macroHeight, budget` (diag). Banc
  `erosion-bench` : variantes {calmCut, roughCut, it., uplift}. Tests :
  « maxCut floors the carve » (colline sans uplift), « stage-1 budget »
  (pire perte sur le calme = 6 m), bug corrigé au passage : `Finalize`
  lisait hors d'un masque de lac d'UNE ligne (`maskHeight < 3` non
  gardé — l'assert vector ne se voyait qu'en Debug ; la suite complète
  tourne en Release, qui ne vérifie pas les indices : à garder en tête).
  `kTileBakeVersion` 71, hash `MapBakerTest` 6716548419360545234.
  **Baseline 8 km « budget »** (carte (0,0), 14 s Release) : hauteurs
  41-154 m (plancher 40-80 + pièces), census intérieur **socle 17,9 % /
  versant 81,9 % / drame 0,2 %**, relief médian **21,9 m** (socle 13,
  versant 25 — le roulis ±25 m / 700 m survit : 12,5 m → 22 m), transect
  N-S plat 27 %, E-W 54 % ; vista : ouvert 8/9, amer > 2° à 3 km 7/9,
  **colline ≤ 1,5 km 3/9** (0/9 avant), sommet alpin 0/9 (carte basse
  par décret) ; calibration analytique − baké : 0-100 m +2 m, 100-200 m
  −26 m, 200-300 m −46 m (les pièces perdent un cinquième, plus 150 m) ;
  spawn (3 881, 68, 4 656), 5,2 m / 30 m ; 20 runs fleuve (7,9 km), 90
  ruisseaux. **Reste rouge : l'eau** — 129 lacs naturels (31 par
  4×4 km, surtout 0,5-10 ha, 7-9 m de profondeur) + 77 étangs posés
  (confluences/épingles) pour une cible §4 de 3-6 lacs par carte. Ce
  n'est pas N3 (33 lacs avant le budget, 60-68 en N1/N2, 21 par 4×4 km
  sur la carte 24 km) : les bassins naissent dans la surface ÉRODÉE
  (dépôt, thermique, arrondi ?) et pas dans la macro — enquête dédiée
  (banc : variantes sans transport sédimentaire, sans thermique, seuils
  `minLakeDepth`/`minLakeCells` 0,6 m / 12 cellules) à faire AVANT N4.
  Idem les étangs posés (une règle de rendu des rubans, 77 sur 8 km).

- **2026-10-05 (soir) — Retour dev sur N2b+N3 : « ça reste trop plat et
  trop réaliste, je t'avais donné comme exemple le monde story » ; et
  « tu as supprimé les features qui servaient à construire les maps
  avant ? » (oui : layout continental, porteuse, régimes, houle, grilles
  d'amers, champ de vallées, trunks — remplacés par la couche monde et
  les pièces, comme §7.5.3 l'écrivait ; le mode histoire ne les a
  jamais utilisées, il n'a ni érosion ni structure : `proceduralBase`).
  **Brique « rythme histoire »** : la table d'étages prend le rythme
  du mode histoire — plaines **±75 m / 500 m sur 5 octaves** (au lieu
  de ±25 m / 700 m sur 4, l'arbitrage du plan qui a donné la plaine
  plate), collines ±90, plateau ±60 terrassé 0,3, haute montagne ±150 /
  700 ; les **montagnes du mode histoire** (ridged 2 km, 270 m,
  masque 0,45-0,75) entrent dans la levée de base (`max(pièce,
  montagne)`, le keep les protège, le calme les laisse) ; le socle
  garde le relief BRUT : `calmCut` 0,5 m, thermique et arrondi
  rebranchés par texel sur le terrain rude seulement (`roughW` = 1 −
  calme) ; le comblement des creux locaux reste. `kTileBakeVersion` 72,
  hash 13401478122622314894. Tests : plafond analytique avec la
  montagne histoire, « landmark summits » 5-60 par carte, calme > 45 %,
  tolérance du premier texel de plage 30 %.
  **Baseline 8 km « rythme histoire »** (carte (0,0), 17 s) : hauteurs
  60-297 m ; census intérieur **socle 5,9 % / versant 93,6 % / drame
  0,5 %**, relief médian **38 m** (le roulis ±75 m, partout) ; transects
  plats 14-29 %, médian 11-14 m ; vista : ouvert 6/9, colline ≤ 1,5 km
  **4/9**, amer > 2° 8/9 ; calibration analytique − baké : 0-100 m
  +13 m, 100-200 −6, 200-300 −5 (la macro survit telle que dessinée) ;
  **79 lacs** naturels (19 par 4×4 km, 129 avant) + 84 étangs posés ;
  137 ruisseaux, 15 rivières, 3 runs fleuve ; spawn (3 881, 93, 4 656).
  Rendus 10/24 km : le froissé du mode histoire, pièces et montagnes
  masquées tous les 1-2 km. Jugement dev en jeu : EN ATTENTE. L'eau
  (lacs + étangs) reste le sujet suivant.

- **2026-10-05 (nuit) — « Tu as mesuré les montées et descentes du mode
  histoire ? » Non : mesuré maintenant.** Nouvel instrument `rhythm
  diagnostic` (pente moyenne par pas de 10 m, inversions par km avec
  1,5 m d'hystérésis, relief médian par 250 m, montée sur 100 m p95/p5,
  amplitude par km, part du sol > 100 m au-dessus de son minimum local
  à 2 km), sur quatre transects de 6 km. **Le mode histoire autour de
  son départ (32, 400)** : pente 24,8 %, 6,5 inversions/km, relief 38,8 m,
  montée p95 +36 / p5 −39 m, amplitude/km 97 m (max 187), **32 % du sol
  à > 100 m de son minimum local** — c'est une région riche en montagnes
  masquées (ailleurs le même mode histoire donne 19 %, 28 m, 6,6 %).
  Sandbox v72 autour du spawn, avant retouche : 17,6 %, 9,3/km, 23,7 m,
  70 m, 9,7 %. Deux retouches : masque des montagnes histoire 0,45-0,75
  → **0,36-0,62** (la densité du départ histoire, pas sa moyenne), et le
  comblement des creux plafonné à **20 m** (un creux entre deux crêtes
  est une vallée qu'on descend, pas un étang). Après : **pente 20,6 %,
  7,3/km, relief 30,2 m, montée p95 +32 / p5 −38, amplitude/km 91 m (max
  195), 19 % du sol > 100 m** ; carte entière : versant 97,8 %, relief
  médian 68 m, colline ≤ 1,5 km **8/9**, ouvert ≥ 30 azimuts 3/9 (les
  montagnes ferment les vues), 107 lacs + 66 étangs, spawn 7,2 m / 30 m.
  Hash 10607966812941411636 (v72 inchangé). Tests : calme > 25 %, bande
  partagée des tuiles fenêtrées < 10 m (le comblement lit une moyenne de
  fenêtre : la divergence qui compte est celle des lignes de carte, à
  mesurer en N4). Reste pour le feel : l'écart résiduel avec le départ
  histoire (pente 21 vs 25 %, prominence 19 vs 32 %) se règle par
  `storyMountainMask*` et `storyMountainAmplitude` ; **l'eau est le
  sujet suivant** (107 lacs : les creux conservés deviennent des lacs
  faute de drainage — une passe de percée d'exutoires, pas un remplissage).

#### 7.5.6 Arbitrages du dev (2026-10-05) et plan approuvé : la passe « nouvelle base » (N1-N5)

**Arbitrages** : cartes **2×2 tranches = 8 192 m** (échelle Skyrim), bandes
de bordure rescalées ; **étage monde large** (mer → haute montagne
~1 500 m) ; **carte (0,0) = prairie tempérée basse garantie** ; **plaines =
collines douces ±25 m / 700 m** (la cible §4 « socle < 15 m / 250 m » est
gardée, le relief vivant vient des pièces locales et des crêtes de massif).

**Le plan** (fichier de plan de la session, approuvé ; un commit par
brique, `kTileBakeVersion` bumpé par brique, hash `MapBakerTest`
ré-épinglé, A/B visuel dev) :

| # | Brique | Contenu | Mesure de sortie |
|---|---|---|---|
| N1 | Carte 8 km (plomberie, macro inchangée) | `kMapTilesPerSide` 2, bandes 900 m / lift 260 m / méandre ±300 m, sonde de spawn rescalée, écran M sur la carte active, **passage de carte automatique** (sortie du rect → `travelToMap` + voile ; les triggers du Col de l'Est retirés), overviews et far-water des voisines, `masterRiversNear` élargi de l'apron | baseline 8 km « taille seule » |
| N2 | Étage monde + rythme local | `WorldLayer` (9 fbm : continent 45 km, côte 5 km, étage 22 km, massif 26 km, climat 9 km + lapse, décret de départ sur (4096, 4096)), `ProceduralControls` v3 (champs dérivés de l'étage ; layout continental, grilles d'amers, champ de vallées supprimés), `landHeight` v3 (plancher = base de l'étage, table d'étages, pièces 7 km, crêtes, lits), analytique sans compression | distribution des étages, continuité, census « macro seule » |
| N3 | Budget d'érosion dur | `maxCut` par cellule dans `erodeFluvial` (plancher dans le sweep implicite : 6 m sur le calme, 250 m ailleurs), calme plus érodable retiré, relaxation supprimée, 48 itérations / uplift 1,5 ; banc `erosion-bench` sur ces variantes | socle ≥ 40 %, relief médian 20-40 m |
| N4 | Frontières lues sur l'étage | poids `ridge`/`sea` continus le long de la ligne depuis `worldSampleAt` (hash, veto, mémo, cross-fade supprimés), Ouvert ailleurs ; `cooker bake-map --pair` (divergence par style, runs orphelins) | divergence des lignes ouvertes |
| N5 | Réglage + instruments | amplitudes, pièces, lits, climat ; `proportion` en étages ; journal | jeu d'instruments vs cibles §4 |

Ordre imposé : N3 avant N4. Hors passe : Form/UI des paramètres, bump
TRG4, passage sans couture (cartes voisines résidentes), peuplement/POI,
gouttelettes, mode histoire.

### 7.6 Le plan de POI — le terrain au service des points d'intérêt (chantier COURANT, 2026-10-06)

**Décision dev (nuit du 5 au 6 octobre)** après la brique « rythme
histoire » : « des jeux comme Breath of the Wild ou Skyrim sont marqués par
le fait qu'on voit plusieurs points d'intérêt où que l'on soit » — les POI
viennent d'abord, le terrain est sculpté autour. Arbitrages : **POI = formes
de terrain** (les structures humaines viendront plus tard sur les endroits
visibles ou les sites propices aux villes, que le plan réserve) ; **~16 POI
moyens par carte de 8 km** ; **visibilité garantie : 1 grand + 1 moyen depuis
90 % des points marchables** ; **sol entre les POI = roulis du mode histoire
(±75 m / 500 m), POI 2-3× plus hauts** ; ce chantier passe **avant N4-N5**.
La règle du triangle de Nintendo (cacher / choisir / révéler, trois tailles,
gravité, sommet-belvédère) est intégrée principe par principe. Le plan
détaillé (briques P1-P5, architecture, règles, annexe) est le fichier de plan
de la session ; la référence durable des types et des paysages est
**`docs/POI-CATALOGUE.md`** (relief et roche, eau, côte, végétation ;
caractères par biome ; règles de répartition F1-F8 ; la règle du triangle
en une table).

**Architecture** : `ProceduralControls::at` est étendu (pas de second
provider) par un module `engine/terrain/generation/PoiPlan` — sites de trois
étages sur des réseaux de cellules ancrés monde (grand 8 192 m = la carte,
moyen 2 048 m, petit 350 m à 60 %), typés par les tables du catalogue depuis
`worldSampleAt(site)`, liés par un graphe de voisinage relatif (symétrique et
local : identique des deux côtés d'une ligne), mémoïsés par cellule en
thread-local. Les briques : P1 plan + rendu + tests ; P2 noyaux, corridors,
écrans dans `at()` (derrière une bascule pour l'A/B) ; P3 caractères ; P4
visibilité et triangle (instrument `poi visibility`) ; P5 pads de ville,
départ, export du plan dans `MapRecords`.

#### Journal

- **2026-10-06 — P1 livrée : le plan de POI.** `PoiPlan.hpp/.cpp`
  (`poiSitesNear`, `poiEdgesNear`, `PoiPlanParams` dans
  `ProceduralControlParams.poi`), 28 types, tables par contexte (mer / disque
  de départ / côte / massif / étage ≥ 450 / ≥ 150 / plaine ; aride, froid),
  règle F2 (grands voisins différents) et F3 (moyens adjacents différents,
  comparés sur le tirage de base, un niveau), le grand de la cellule de
  départ repoussé à ≥ 1 500 m du spawn, tailles par type (bassins en creux,
  pads plats). `cooker terrain-map … poi` dessine sites (disque par étage,
  couleur par famille) et marches (jaune, par leur point de passage).
  Mesuré (seed 1337, 16 cartes) : 16 grands, 256 moyens (16 par carte), paire
  la plus proche 1 112 m, 5 325 petits ; graphe sur 144 moyens : 192 arêtes,
  0 isolé, 5 paires adjacentes de même type (2,6 %), 7 types distincts ;
  symétrie à la ligne x = 8 192 vérifiée (une marche traversante, trouvée
  identique depuis les deux cartes — la densité des traversées est à
  regarder en P4). `docs/POI-CATALOGUE.md` écrit. Aucun changement de terrain
  encore (le plan n'est pas lu par `at()` avant P2).

---

## 8. Glossaire

| Identifiant | Sens |
|---|---|
| `TerrainParams` / `TerrainBase` / `HeightPatches` | params runtime (patches, base, sandbox, biomes, water) / régions bakées immutables / deltas sculpt |
| `macroHeightAnalytic`, `proceduralBase` | macro S1 sans érosion (fallback, condition aux limites, étage 0) / fallback runtime (overview puis analytique) |
| `MapGridSpec`, `applyMapGridShape`, `mapBorderStyleResolved`, `kMapBorder*` | grille de cartes, bordures v2 (lignes hachées Mer/Montagnes, méandre, veto) |
| `kTileBakeVersion` 68 / `kMapBakeVersion` 1 / `kStage1Version` (orphelin) | versions du cache ; TRG3, TWB3, MOV1, TER1, TBM1 = formats |
| `kMapApron` 3 072 m (ex-`kBasinResolveMargin`) | apron de production de la carte ; `kFineErosionHalo` 192 m |
| `tier`, `tierSpread` 0,35, `uplift`, `plateau`, `hardness` | étages, rampe de tier, orogenèse, dessus de plateau, lithologie |
| `calm` / `gentle` / `trunk` / `reliefScale` | socle calme [0,1] / corridors + cols / fond de vallée maîtresse / multiplicateur des porteuses (clairières) |
| `ValleyField` (φ, ψ), `trunkSpacing` 9 500, `colSpacing` 4 000, `rangeNeed` | vallées orientées, maîtresses (strates), cols garantis |
| `landmarkLayer`, `peakCellSize` 7 000, `hillCellSize` 3 500, `hillRadiusMax` 1 600 | amers alpins et intimes |
| `continentCarrierWavelength/Amp` 20 km / 0,30, `continentLayout`, `layoutAmp` 0,34 | porteuse et layout forcé |
| `startMeadowRadius/Fade` 8,5 / 12 km, `climateWavelength` 2 800 | décret pays de départ (origine), fbm climat |
| `BiomeSet`, `biomeIdAt`, `regionFieldsAt`, `regionShadingAt`, `materialWeightsAt`, `grassZoneAt` | seams biome / attributs blendés (chaud) / teinte (bake) / poids de matériau (scatter, herbe) / hex-tiling CPU |
| `MasterNetwork`, `fleuveArea` 6e6, `imprintMasterChannels`, `masterBoundarySources` | étage 0 à 128 m et sa consommation |
| `keep`, `plateauKeep`, `kCalmKeep` 0,25, channel keep `0,0018·A^0,44`, `lakeKeepDepth` | protections contre l'érosion |
| `fluvial.iterations` 80, `kc`/`sedimentCapacity`, `depositMax`, `BiomeErosion` | fastscape, dépôt, érodibilité par biome |
| `riverArea` 1,5e5, `riviereArea` 2e6, `kRiverWidthCoef` 0,008, `fordSpacing` 2 000, `fordReach` 700 | tiers et gués |
| `minLakeDepth` 0,6 / `minLakeCells` 12, `reconcileLakesWithTerrain`, `lakeReachesPoint` | lacs et réconciliation |
| `amplifyFine`, `fineScale`, `strataAmplitude` 0, `rockExposure`, `cliffW`, `screeFactor` | érosion fine, strates, falaise, éboulis |
| `WaterBodies`, `WaterQuery`, `waterSurfaceAt/DepthAt/FlowAt`, `underLocalWater`, `swimDriftFactor` 0,8 | eau headless et requêtes |
| `WaterSim` (`initWindow`, `scrollWindow`, `pinLakes`, `pinRivers`, `stepWindow`, `extractSnapshot`), `WaterSimParams`, `SimConfig`, `kSimCacheCap` 4, settle-gate 2e-3 × 8 / 3 s | sim option C |
| `solveSteadyWater`, `preRollWindow`, `cooker water-solve / water-replay / pre-bake` | solveur (pre-roll + oracle) et outils |
| `WaterMaterialForm` / `WaterMaterialsUbo`, `WaterVolumeForm`, `SpiritSourceForm` | matériaux d'eau, volumes posés, sources d'esprits |
| `scatterProps`, `scatterGrass`, `candidateRng`, `kVariantCount` 26, `InstancePool`, `GiProp`, `treeFadeEnd` | scatter et ses copies |
| `VegetationCollision`, `seizeRock`, `Displaced`, `takeStaticCollider`, `addDynamicConvex` | collision du scatter ; rochers saisissables (entités) |
| `TreeFire`, `advanceTreeFire`, `fireCanopy`, `FireGrid.regrow` | feu des arbres et de l'herbe |
| `generateColonizedTree`, `ColonizedTreeTuningForm`, `LobeTreeTuningForm`, `GrassSpecies.hpp` | arbres et espèces d'herbe |
| `kSplatArrayLayers` 25, `.mtex`, `harmonize`/`harmonizeOrm`/`flattenLowFreq`, `TerrainShadeMap` | matériaux du sol |
| `Authoring` : `stampKernel`, `stampRidge`, `baseElevationAt`, `alterElevation` | primitives d'authoring (non branchées) |
| `TerrainBrush`, `applyTerrainWall`, `TerrainPatchForm`, `.ter` | sculpt |
| `probeSandboxSpawn`, `armWarmup`, `travelToMap`, `applyMapWorld`, `passes.toml` | spawn, voyage, swap |
| `stageMapRecords`, `riverThin` 48 m, `WorldspaceFilter`, `cellGuidFor`/`materializeCell` | carte → mod ; cellules implicites |
| `7e88a111` / `7e88a112` | familles de GUID déterministes (records terrain / assets de save) |
| Hashes `10031847806692189656` / `4414106998705828656` | golden scatter / `region.heights` du banc tuile |
