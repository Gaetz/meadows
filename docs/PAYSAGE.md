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

## 4 bis. Les principes de composition du monde (plan « composition du monde », approuvé dev 2026-10-07)

Le raisonnement général qui précède toute brique de paysage — chaque
remarque du dev se relit contre ces huit principes avant de toucher au
code (jamais une correction locale qui ajoute une couche parallèle).
Références du dev : Breath of the Wild, Skyrim, Elden Ring ; ses
méthodes : points d'intérêt, règle du triangle (cacher / choisir /
révéler), verticalité, chemins avec choix entre des structures
géographiques réelles ; ses arbitrages du 2026-10-07 : escalade mixte
(petites falaises franchissables, grands escarpements = murs), **une zone
= 1 km, un monde resserré avec beaucoup d'exploration, comme Skyrim**.

1. **Le monde est une mosaïque de zones de ~1 km, chacune avec une
   identité** : un archétype (sa nature), un étage (sa hauteur, par
   paliers), une grammaire (comment son relief, ses falaises, son
   couvert, son eau se dessinent).
2. **Les porteuses longues décident l'étage, les zones décident la
   nature** (la règle d'août remise à l'échelle) : une tendance d'étage
   à ~4 km fait monter les zones par paliers ; chaque zone arrondit à son
   palier et le tient.
3. **La verticalité est aux frontières des zones** : mur proportionné à
   l'écart d'étage (bande de falaise franchissable à ses encoches, ou
   escarpement infranchissable), crête, rivière ou lisière entre zones de
   même étage ; à l'intérieur, la grammaire de l'archétype.
4. **Chaque mur a ses portes, et les portes sont des structures
   géographiques réelles** (col, brèche-rampe, gorge, gué, pont, éboulis) :
   le choix de chemin est un choix de structure.
5. **La règle du triangle** : à chaque porte, la zone suivante est cachée
   puis révélée ; les POI gardent leur silhouette conique et leurs écrans.
6. **Hiérarchie des amers** : le grand de la carte visible de presque
   partout ; un moyen toutes les 2-3 zones ; chaque zone porte son petit.
7. **Le calme est la règle dans la zone, le drame est à ses bords et à
   son POI** (le 40/35/25 du §4 : socle = l'intérieur des zones, versant =
   les grammaires, drame = les murs et les POI).
8. **Tout est réglable par le dev sans compiler, et tout est donnée
   (moddable §5)** : le pupitre (`TerrainGenTuningForm`, panneau Terrain
   generation, `cooker landscape-report`), puis les archétypes de zones.

Briques (fichier de plan de session) : **Z0** le pupitre · **Z1** la
performance de génération (stage-1 ≤ 20 s, chargement instantané avec
cache, pas de bump de version sans cache livré) · **Z2** les zones ·
**Z3** murs et portes · **Z4** POI emboîtés et visibilité · **Z5** chemins
et sites. Méthode : un essai = Release + `landscape-report` + cache copié ;
suite rapide au commit, complète au push, jamais par prompt.

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
- **2026-10-06 — P2 livrée : le terrain lit le plan.** `planSampleAt`
  (`PoiPlan.cpp`) : noyaux par type — cône à flanc linéaire (pente hachée
  18-28°, 40° pour l'aiguille, pointe arrondie sur 8 %) pour sommet /
  butte / cap / îlot / stack ; cratère (cône tronqué + bol) ; mesa (plateau
  plat, rebord raide, `mesaTop`) ; échine à cols et col (selle) ; gradin /
  falaise de mer / cascade (demi-plan lissé) ; canyon et fjord (échine
  négative à fond plat) ; cirque (amphithéâtre ouvert d'un côté) ; bassins
  (lac de plaine, tarn, crique : dôme négatif à fond plat) ; champs de
  hoodoos / tours karstiques / blocs (réseau de petits cônes) ; bosquet /
  arbre (bosse de 12 m) ; oasis (−3 m) ; source = marqueur seul ; pads de
  ville / confluence (`padFlat`). Marches : corridor (tube de 60-120 m,
  `gentle`/`calm` = 1, relief ×0,5) sur la polyligne a → point de passage
  → b, et **écran** à 45-60 % de la marche (échine transversale de
  250-500 m, 40-90 m, entaillée à 75 % sur le corridor : la destination
  disparaît derrière, réapparaît à l'encoche). `ControlSample +=
  basinDepth` (soustrait du plancher par `landHeight` avec la porte du
  rivage), `MacroResult += basin` (le comblement des creux saute les
  bassins). `RhythmParams::plan` = bascule A/B (`terrain-map … noplan`),
  `planStoryScale` 0,5 : les montagnes histoire restent une texture à
  mi-hauteur sous le plan. Mémo : effacé seulement aux points d'entrée
  (une référence vivante dans le mémo se faisait couper pendant un calcul
  de réseau maître : segfault à 10 km). v73, hash 922232772222690693.
  Tests : « the terrain reads the plan » (36 sommets levés ≥ 0,9×, 21
  bassins creusés, 30 pads plats, corridors gentle/calm ≥ 0,5, plan off ≠
  plan on sur 50+ sites). **Baseline « plan »** (carte (0,0)) : autour du
  spawn pente 21,9 %, 9 inversions/km, relief 29,5 m / 250 m, montée p95
  +40 / p5 −40 m, amplitude/km 92 m (max 311), 18,9 % du sol > 100 m ;
  carte entière socle 5,3 % / versant 94,7 %, relief médian 43,6 m ; 83
  lacs + 90 étangs ; spawn (3 881, 70, 4 656), 1,9 m / 30 m. Rendus :
  les cônes se lisent comme des disques ombrés, les marches comme des
  lignes douces, les écrans comme des traits transversaux. Jugement dev
  EN ATTENTE (A/B : `noplan`).

- **2026-10-06 — « C'est toujours beaucoup trop plat ; tu es sûr que
  l'érosion ne ramène pas tout au sol ? »** Mesuré au bon endroit cette
  fois : nouvel instrument `spawn flatness diagnostic` (±1,5 km autour du
  spawn, huit transects, pente moyenne par 10 m, part de sol plat < 2 %,
  relief par 250 m, montée sur 100 m), sur l'analytique, la macro de
  stage-1 (après comblement), l'érodé de stage-1, le sol final, puis les
  variantes sans comblement, sans transport sédimentaire, sans fluvial.
  **Verdict** : analytique plat 7,5 % / relief 26 m ; comblement ≈ 0 ;
  **érodé plat 28 %** / 20 m ; sans transport 31 % ; **sans fluvial 8 %**
  / 24 m — le fastscape aplanissait bien, mais pas sur le socle à budget
  0,5 m : sur la **texture des montagnes histoire**, que la porte calme
  lisait comme une levée dessinée (`calm = 1 − ss(20, 70, lift)` avec
  `lift = max(POI, montagnes)`) → calme 0 → budget 250 m → fonds de vallée
  plats. **Correction** : la porte calme ne lit que la levée DESSINÉE
  (POI, pièces) ; la texture histoire est du socle, gardée brute par le
  budget. Avec, au passage : `planStoryScale` 0,8, POI moyens 220-340 m
  et grands 500-700 m (au-dessus de la texture), **départ sur un
  belvédère** (le sommet, la butte ou la mesa du plan la plus proche du
  centre de carte, à moins de 2,5 km — l'ancien spawn choisissait par
  construction le point le plus plat de la prairie, le pire endroit pour
  juger le relief), `TileBakeParams::dimpleFillMax` (bouton, 0 = sans).
  **Après** (autour du spawn) : analytique pente 22,9 % / plat 5,4 % /
  relief 38,7 m ; érodé plat 21 % / 33,5 m ; **sol final pente 19,6 % /
  plat 13 % / relief 34 m, montée p95 +39 / p5 −35 m** ; `rhythm` : pente
  23,8 %, 6,6 inversions/km, amplitude/km 108 m, **30 % du sol > 100 m
  au-dessus de son minimum local** — le départ du mode histoire fait
  24,8 % / 39 m / 97 m / 32 %. Hash 7920876824618097595 (v73). Test
  « calm is the rule » : le socle texturé reste calme, les sommets des
  POI quittent la famille.
- **2026-10-06 — Cadre dev corrigé : « une plaine n'est pas un problème,
  une plaine de plus de 30 s de marche l'est ; dans Skyrim il y a un point
  d'intérêt ou un nouveau paysage toutes les 45 secondes » ; et « les
  éléments d'août peuvent servir à créer la variété entre les POI ».** Les
  vallées maîtresses ne sont qu'une suggestion de chemin parmi d'autres :
  les chemins réels viendront d'un **A\* de moindre pente entre les
  installations humaines** (peuplement, après P5). **P3 réécrite « la
  variété à 45 s »**, livrée : les sélecteurs d'août reviennent sous le
  plan — **régime de relief** à 875 m (pays de collines : crêtes ridgées
  90 m sur la longueur d'onde des crêtes ; vieux massif : plancher +110 m,
  hors porte calme ; plaine), **bande de calme** à 1 200 m (roulis ×0,6 à
  ×1,3 : plaines calmes contre plaines rugueuses), **grille intime** à
  1 000 m (85 % : colline marquée 60-120 m, échine, mesa ou clairière —
  `PieceGrid` : `pieceLayer` paramétré, clairière = bol sans levée,
  jamais sur un massif ni sur un POI), **couvert** à 600 m
  (`WorldSample::cover`, `paletteIdFor(…, cover)` : dans le tempéré la
  lande (4) au-dessus de 0,62, la prairie sèche (5) sous 0,36). Instrument
  `variety transect` étendu : événements POI (moyen/grand à < 250 m,
  petit à < 80 m) et couvert (changement d'id de palette) ; nouveau test
  « variety at 45 s » sur contrôles + analytique (6 transects au départ,
  fenêtres de 250 m : 128 événements — relief 87, régime 31, couvert 63,
  POI 24 — **espacement moyen 277 m, pire écart 750 m**). v74, hash
  16056817174982315739. **Baseline « variété » sur le baké** (carte (0,0),
  transects par le spawn) : E-O espacement **338 m** (pire 1 250), N-S
  **261 m** (pire 775), relief médian 53 m par 250 m ; carte : versant
  95 %, relief médian 58 m ; palette intérieure tempéré 49 % / lande 22 %
  / prairie sèche 29 % ; colline ≤ 1,5 km 6/9 ; 92 lacs + 57 étangs ;
  autour du spawn (rhythm) pente 20,6 %, 34 m / 250 m, 21 % du sol > 100 m.
  Reste : la cible de 250 m n'est atteinte que N-S ; levier suivant = les
  ruisseaux comme événements (tous les 500-800 m selon §4) et la grille
  intime à 900 m ; jugement dev EN ATTENTE.
- **2026-10-06 — « C'est mieux mais on manque encore de variété » → P3b
  « les trois axes de variation », livrée.** La variété ne vient pas que
  du relief : trois axes, un commit chacun. **Axe 1, les caractères de
  région** (`8d252e9`) : le site moyen ou grand le plus proche porte un
  caractère haché parmi six (prairie roulante, bocage, collines boisées,
  marais, plateau rocailleux, lande — `kCharacters`, PoiPlan.cpp) ; les
  multiplicateurs d'amplitude et de longueur d'onde du roulis, l'humidité
  des lits, la dureté et le biais de couvert se fondent sur les trois sites
  les plus proches (poids 1/d², `planCharacterAt`) : aucune marche aux
  frontières (pire saut ×0,27 sur 200 m). `ControlSample +=
  reliefWavelengthScale, character`. Mesuré sur 24 km : six caractères à
  14-18 % de la terre chacun. **Leçon** : la longueur d'onde du roulis ne
  peut pas être une fréquence variable dans un seul bruit (`1/(λ·scale)`
  avec un `scale` qui dépend de la position chirpe en arcs concentriques
  là où il change) ; `landHeight` fond trois porteuses à échelle FIXE
  (×0,7, ×1, ×1,8) selon le caractère. **Axe 2, le couvert** :
  `BiomeForm::forestDensity` (réfléchi, moddable) → `BiomeParams` /
  `RegionFields` → `forestAt(params, x, z)` = masque forêt × densité, lu
  par les arbres, les débris, les plantes, les buissons, le FarTerrain et
  l'herbe (un seul oracle au lieu du masque nu) ; quatre palettes nommées
  — 6 forêt dense (densité 1,9), 7 bocage (0,9), 8 clairière (0,08), 9
  marais (humidité 0,95, 0,35) — choisies par le caractère
  (`characterPalette` : collines boisées → 6, bocage → 7, marais → 9) et
  par la grille intime (clairière → 8, `biomeIdAt` lit le même oracle par
  texel) ; `biomeErosion` étendue aux lignes 6-9 (le marais comble à plat).
  **Axe 3, l'eau par les formes** : règle F5 du catalogue — cascade, canyon
  et confluence n'existent que SUR un cours maître : `poiNearestCourse`
  (réseau maître lu sur les contrôles sans plan, `fleuveArea` 1,5e6,
  portée `waterPoiReach` 450 m) ; un site trop loin se re-tire (3 fois)
  puis se replie sur la forme sèche de sa famille (butte, échine, arbre
  seul) ; un site gardé s'aimante sur le cours et prend sa direction
  (`PoiSite::theta` : le canyon le long, la marche en travers). La table du
  disque de départ gagne la cascade et la confluence (tout le rect de test
  était dans le disque de pays bas). Le cirque n'est plus un bassin mais un
  rebord de 120-200 m qui creuse son tarn (0,25 h). Mesuré : 144 moyens,
  47 à portée d'un cours, **7 POI d'eau, tous sur un cours** (4 cascades,
  3 confluences). Le départ accepte le bocage et la clairière comme sol de
  prairie. v75, hash 15270338236042570239. Variété sur les contrôles : 125
  événements (relief 85, régime 31, couvert 65, POI 24), **espacement moyen
  286 m**, pire écart 1 250 m. **Baké** (carte (0,0), transects par le
  spawn) : E-O **305 m** (pire 800), N-S **288 m** (pire 525) ; carte
  socle 6,9 % / versant 93 %, relief médian 52 m ; palettes intérieures
  tempéré 24 % / bocage 20 % / prairie sèche 19 % / forêt dense 14 % /
  lande 8 % / clairière 7 % / marais 7 % ; 120 lacs + 47 étangs ; autour
  du spawn pente 20,2 %, 5,6 inversions/km, 33,7 m / 250 m, montée p95
  +36 / p5 −40 m, amplitude/km 96 m (max 415), 25 % du sol > 100 m.
  Suite Release 796/796, rapide Debug verte, smoke-run Debug sur le cache
  v75 sans erreur. Jugement dev EN ATTENTE (le rendu `p3b-10km.png`
  montre les caractères et les POI d'eau sur leurs cours). Reste de P3b :
  l'eau rouge (120 lacs contre 3-6 visés) est systémique, pas un effet des
  axes — enquête à part ; la cible 250 m tient N-S, pas E-O.
- **2026-10-06 — « Les reliefs ne sont pas assez accidentés : dans
  Breath of the Wild et Elden Ring c'est la verticalité qui permet de
  lire les paysages ; il faut exagérer l'aspect vertical » → brique
  « verticalité », livrée.** Mesuré d'abord (nouvel instrument
  `verticality diagnostic` : parts de pas de 4 m plus raides que 30 /
  45 / 60°, murs — suites de pas > 45° montant ≥ 5 m — par km, hauteur
  médiane et maximale, montée sur 100 m p95 ; sur l'analytique, le
  stage-1 avant et après thermique, le baké, le mode histoire) :
  autour du spawn le baké avait **2,3 % de pas > 30°, 0,2 % > 45°,
  0,3 mur/km** ; le départ du mode histoire fait 13,9 % / 1,4 % / 0,8 ;
  et la relaxation thermique de stage-1 (angle de repos 33°) effaçait
  les rares murs (0,29 → 0 mur/km). Le problème était d'abord dans la
  synthèse (formes douces), ensuite dans la relaxation. **Trois
  mécanismes existants étendus, derrière un bouton
  `PoiPlanParams::verticality` (0 = le look P3b, 1 = plein)** :
  (1) **gradins** — la quantification `terrace` de l'étage 450 m
  généralisée : `ControlSample::terrace` [0,1] vient du plan
  (caractère : plateau rocailleux 0,9, lande 0,55, collines boisées 0,4,
  prairie 0,25, bocage 0,1, marais 0 ; flancs de cône 0,7 × flanc),
  nul sur un corridor, un pad, un col ou une clairière (la praticabilité
  promise) ; `landHeight` marche le sol par `MacroParams::cliffStep`
  30 m avec un ressaut de 5 % du pas (`cliffEdge`) : une pente devient
  un escalier de murs et de bancs ; (2) **noyaux plus raides** — cônes
  à 30-42° (`coneSlopeSteep*`) en gardant l'EMPREINTE de la pente douce
  et en montant le sommet (hauteur × tan(raide)/tan(doux) ≈ ×1,5-1,9 ;
  la première version, qui gardait la hauteur et réduisait l'empreinte,
  faisait des aiguilles et perdait le sol haut : 25 → 17 % du sol à plus
  de 100 m) ; rebord de mesa sur 18 % du rayon au lieu de 35, marche
  d'escarpement/cascade sur ±5 % au lieu de ±12 ; (3) **la thermique
  respecte les murs** — `MacroResult::cliff` (le `terrace` par texel)
  multiplie l'angle de repos de stage-1 jusqu'à ×3 ; la micro-thermique
  du finalize tient ce que le stage-1 a gardé (échelle 1 → 3 sur la
  pente du grossier entre 31 et 45°). v76, hash 18360944717357650365. **Mesuré après**
  (autour du spawn, baké) : **29 % de pas > 30°, 6,7 % > 45°, 4,8
  murs/km (médiane 18 m, le plus haut 61 m)**, montée sur 100 m p95
  81 m (était 47), pente moyenne 25 % (était 20), amplitude/km 96 m
  (max 729), 27 % du sol > 100 m ; l'analytique seule : 25 % / 4,8 % /
  7,7 murs/km. Soit deux fois le départ du mode histoire et dix fois le
  sandbox d'avant. Variété intacte (E-O 290 m, N-S 288 m) ; 90 lacs +
  21 étangs ; drame 3,5 % des fenêtres (était 0). Le rendu
  `terrain-map … poi` montre les cônes étagés en courbes de niveau (le
  grand au centre en pyramide à gradins) et les caractères rocailleux
  en escaliers. Tests : « verticality: the plan terraces the country
  into cliffs » (pas > 45° ≥ 1,5 % et ≥ 3× le réglage 0 ; 214
  échantillons de corridor à terrace ≤ 0,003). Jugement dev EN ATTENTE
  (A/B : `verticality` 0 = le bake précédent) ; leviers si « encore
  trop doux » : `cliffStep` 30 → 45 m (murs plus hauts), `cliffEdge`,
  les forces par caractère, `coneSlopeSteep*`.
- **2026-10-06 — « La montagne très haute est bien, mais le reste est
  toujours assez bas ; comme avant la refonte, des plateaux, des zones de
  hauteur différentes, plus de variété de hauteur sur une distance
  horizontale plus courte » → brique « plateaux », livrée.** Le
  diagnostic : la couche monde N2 fait un étage à 28 km et des bancs
  à 7 km à ±20 % d'une altitude de province basse (±20 m au départ), et
  le disque de pays bas aplanit tout le rayon de la carte (0,0) : un seul
  niveau entre les POI. **Le mécanisme : un étage court dans
  `WorldLayer`** — un champ à 3 km (`plateauWavelength`) quantifié en
  niveaux de 120 m (`plateauStep`, `plateauLevels` 2 : la plaine, un
  plateau, un plateau plus haut), additif (une province basse a les
  siens), avec un escarpement entre deux niveaux (`plateauEdge` 0,18 de
  la bande = ressaut d'environ 100 m à 45°, puis étagé par les gradins
  de la brique précédente) ; absent de la prairie de départ, il monte à
  partir de 1,6 km au-delà d'elle (`plateauStartGap`). `WorldSample +=
  baseSmooth` (le même sol avec la rampe linéaire au lieu des marches)
  et `scarp` (1 sur l'escarpement) ; `ProceduralControls::at` lit
  `base = mix(base, baseSmooth, corridor)` — **une marche du plan
  traverse l'escarpement en rampe, pas en mur** — recalcule l'étage
  (`tier`) sur ce sol, et marque `terrace = max(terrace, scarp)` : la
  thermique tient l'escarpement comme un mur dessiné. Tests : « world
  layer: plateaus make height zones within a short walk » (27 disques
  de 2 km sur la carte de départ hors prairie, 27 avec ≥ 100 m
  d'étendue de socle p10-p90 ; escarpement sur 16 % des échantillons) ;
  la continuité à la ligne se mesure en dérivée seconde (0,3 m, un
  escarpement de 97 % de pente n'est pas une couture) ; portes de
  répartition des étages élargies (terre < 150 m 30 %, 150-450 45 %,
  450-800 18 %, ≥ 800 7 % : le monde a monté d'un demi-pas). v77, hash
  16652696545088885290. **Mesuré après** (carte (0,0), baké) : relief
  médian de la carte **31 → 70 m par 250 m** (versant 75 m), **sol à
  plus de 100 m au-dessus de son minimum local 15 → 35 %** (autour du
  spawn 27 → 46 %) ; autour du spawn pente moyenne 25 → 31 %, relief
  34 → 48 m / 250 m, amplitude/km 96 → 142 m ; murs 5,4/km, le plus
  haut 105 m ; variété 288-290 m inchangée ; 72 lacs + 45 étangs (pas
  d'ajout) ; départ (3 325, 553, 4 739) à 553 m. Le rendu
  `terrain-map … poi` montre les plateaux en aplats à deux niveaux
  bordés d'escarpements, les marches les traversant en rampe. Au
  passage, deux seams rendus honnêtes : le socle est relu PAR TEXEL
  dans la synthèse (`ControlSource::refineFloor`, le même seam que
  `biomeIdAt` : un escarpement de 100 m lerpé sur la grille de 64 m
  changeait de phase d'une fenêtre à l'autre) et l'escarpement garde
  le budget d'érosion d'un relief dessiné (`MacroResult::scarp`, 4 × le
  budget calme : jamais creusé jusqu'à la plaine). **Reste, mesuré** :
  le banc de deux tuiles voisines (`bakeSoloTile`, aprons différents)
  diverge de 77 m en un point de sa bande de recouvrement alors que
  les deux stage-1 y sont à 5 m près (macro identique, érodé 135 vs
  130) — c'est APRÈS le stage-1 (hydrologie, lacs, finalize) qu'un
  bassin fermé de plateau est résolu différemment selon la fenêtre ;
  mécanisme à identifier en N4 (`bake-map --pair`, apron partagé sur
  les lignes de carte) ; la borne du banc passe à un pas de plateau
  (130 m) avec ce constat en commentaire. Jugement dev EN ATTENTE ;
  leviers : `plateauStep`, `plateauLevels` (3 = jusqu'à 360 m),
  `plateauWavelength`, `plateauEdge`.

### 7.7 La composition du monde — zones, murs et portes, pupitre, performance (chantier COURANT, 2026-10-07)

**Le constat du dev (2026-10-07)** : les briques verticalité et plateaux
étaient des corrections locales, pas un raisonnement de level design ; le
résultat restait plus simple que le monde d'août ; les chaînes de tests
brûlaient les crédits ; et **le paysage recharge lentement** là où il
apparaissait instantanément après ÉCONOMIE. L'inventaire (agents) a
montré qu'août faisait des ZONES (paliers 40 → 110 → 270 → 520 m sur une
onde de 4 km, régime à 875 m, vallées orientées) que la refonte d'octobre
a lissées (étage 28 km, disque de départ aplani, roulis uniforme) ; que
le bake d'une carte est passé de 18 s (N1) à 41 s (plateaux) par
lectures par texel ajoutées (`biomeIdAt`, `refineFloor`) ; et que cinq
bumps de version du cache en un jour ont fait re-baker la carte au dev à
chaque lancement. Les principes de réponse sont au **§4 bis** ; les
briques Z0-Z5 dans le fichier de plan.

#### Journal

- **2026-10-07 — Z0 livrée : le pupitre.** `TerrainGenTuningForm`
  (`data/forms/LandscapeForms.hpp`, motif `StatsTuningForm`, guid
  `1a4d5c00-…-00a`) : 103 champs scalaires réfléchis (couche monde,
  rythme, plan de POI, macro, bake — tableaux aplatis `…Etage0..3`),
  défauts = les défauts C++ (test `terrain gen tuning: the form's
  defaults equal the C++ defaults`, champ par champ par réflexion).
  **Un seul mappage** `game::applyTerrainGenTuning` / `capture…`
  (`game/TerrainGenTuning.cpp`, deux listes dans le même ordre) et
  `makeTerrainBakeParams(tuning, gen)` utilisé par la scène
  (`makeMapBakeParams`), `cooker pre-bake`, `bake-map` et le rapport ; le
  repli analytique et la sonde de spawn lisent les params du bake
  (`applyMapWorld` copie `controls`/`macro` du bake, plus les cinq champs
  à la main). **Clé de cache** : `mapBakeKey` hache tous les champs du
  Form (`hashTerrainGenTuning`) : une édition = une carte différente,
  re-bakée par le streamer ; les mémos thread-local (`PoiPlan`, le
  start-shift de `WorldLayer`, le veto de bordure de `TerrainGen`) sont
  clés sur **tous** les paramètres (`hashParams` par struct). **Panneau** :
  `drawReflectedStruct` (la grille de propriétés sans EditSession) dans
  « Terrain generation » (mode Édition) : filtre par groupe, **Apply &
  re-bake map** (= `travelToMap` de la carte active au prochain update :
  nouveau streamer, cache raté, bake en fond derrière le voile), **Save
  overlay** (`data/mods/terrain-gen.toml`, listé dans `plugins.toml` — un
  patch §5 sur le record canonique, lu aussi par le cooker), **presets**
  (`data/mods/terrain-gen-presets/<nom>.toml`, sauver / charger = l'A/B du
  dev), **Reset to C++ defaults**. **`cooker landscape-report <gameDir>
  [mapX mapZ]`** : bake si le cache est périmé (pupitre inclus), `plan.png`
  à côté des tranches, et le recensement (pente, pas > 30/45°, murs/km,
  relief par 250 m, montée p95, sol > 100 m, lacs, rivières), une ligne
  d'historique dans `landscape-report.log` — la mesure unique d'une brique,
  40 s en Release, aucune suite de tests. `game/MapView` = la carte bakée
  relue derrière la seam `TerrainParams` (le fixture de tests en est la
  copie ; à fusionner). La clé change pour tout le monde : le cache est
  re-baké et **livré** dans les deux dossiers de lancement (règle « pas de
  bump sans cache »). Les caractères (six grammaires) ne sont pas exposés :
  ils deviennent la table d'archétypes des zones en Z2.
- **2026-10-08 — Z1 livrée : la performance (mesurer d'abord).**
  Chronomètres de chargement (`Load: bootstrap …`, `Load: sandbox …`) et
  de bake (phases du stage-1, de la synthèse, du réseau maître), bloc
  `cost:` et mesures à froid dans `cooker landscape-report`. Le
  coupable : le réseau maître des fleuves recalculé sur l'analytique à
  chaque chargement (sonde de spawn 7,7 s, 43 s en Debug) et à chaque
  bake (imprint 6,6 s) — 25 super-cellules × 321² échantillons à 6 µs.
  Correctifs : cache disque des super-cellules (clé = tous les
  paramètres, à côté des cartes) et calcul parallèle des cellules
  manquantes. **Release** : reconstruction de scène 10,8 → 3,0 s, sonde
  7,7 s → 9 ms, lightmap à +5 s (était +13), bake à froid 25 → 15 s
  (stage-1 19,6 → 12,5). La régression 18 → 41 s du 6 octobre était pour
  moitié mes chaînes Debug en parallèle. Détail et restes (planSampleAt
  3,6 µs, double `worldSampleAt` par texel) : `docs/CPU-PERF.md` § Z1.
- **2026-10-08 — Z2 livrée : les zones de 1 km.** `ZonePlan`
  (`engine/terrain/generation/ZonePlan.hpp/.cpp`) : réseau de cellules
  de 1 000 m jitterées ancrées monde, Voronoï à frontières gauchies
  (150 m), mémo thread-local par cellule ; par zone un **archétype**
  tiré d'une table (14 lignes : prairie, bocage, collines, replat boisé,
  plateau-mesa, haut plateau, bassin, marais, badlands, pierrier, lande,
  crête-massif, falaises de côte, côte basse — conditions sur étage,
  massif, côte, humidité du monde au centre de la zone), un **palier**
  (la tendance longue à 4 km arrondie par pas de 60 m, 0..4, + le biais
  de l'archétype ; **règle anti-cuvette** : une zone ne descend sous
  toutes ses voisines que si son archétype tient l'eau), une
  **grammaire** (amplitude et longueur d'onde du roulis, force et pas
  des gradins, crêtes ridgées, dureté, humidité, couvert, palette) et
  sa **pièce** (butte, clairière, mare, bloc, bosquet — l'ancienne
  grille intime, une par zone). Deux zones de paliers différents se
  rencontrent sur un **mur** dont la largeur suit l'écart (90 m pour un
  pas = bande franchissable, 60 m au-delà = escarpement, `scarp` tenu
  par la thermique et le budget), les corridors des marches le
  traversent en **rampe** (400 m) ; le relief se calme sur 250 m de
  part et d'autre d'un mur. `ProceduralControls::at` lit la zone
  (`RhythmParams::zones`, A/B : off = plateaux + caractères + régime +
  bande de calme + grille intime, qui tombent après validation) ;
  `refineFloor` et `biomeIdAt` par texel aussi (résultat partagé entre
  les deux) ; `ControlSample::cliffStep` par zone ; les plateaux de
  `WorldLayer` passent à 0 niveau. **La table est une donnée** :
  `ZoneArchetypeForm`, 14 records dans `landscape.toml` (ordonnés par
  `rank`), résolus par `makeTerrainBakeParams(…, forms)` ; la clé de
  cache les hache ; 13 réglages de zones dans le pupitre ;
  `terrain-map` teinte la mosaïque par archétype. Au passage, **le mémo
  du réseau maître comparait les paramètres octet par octet, padding
  compris** : les tests headless (sans cache disque) ont tourné 2 h 17
  avant que le mémo soit clé sur le hash (CPU-PERF.md, addendum Z2).
  v78, hash 4975195498282976419. **Mesuré** (carte (0,0), `landscape-report`) : bake
  à froid 21 s (stage-1 18,7 : synthèse 6,2) ; autour du spawn pente
  48 %, pas > 30° 38,9 %, > 45° 4,2 %, murs 2,5/km (médiane 17 m, max
  149 m), relief 126 m / 250 m, montée p95 91 m, sol > 100 m 61 % ;
  carte : relief 35 m / 250 m, sol > 100 m 68 % ; archétypes sur 32 km
  de terre : prairie 48 % (le disque de départ en est), bocage 18 %,
  lande 13 %, bassin 6 %, plateau-mesa 3 %, replat boisé 3 %, marais
  0,6 %, badlands 0,9 % ; 27 disques de 2 km hors prairie, 24 avec
  ≥ 100 m d'étendue de socle. **Rouge : les lacs**, 229 (179 naturels +
  61 mares de POI, 42 par 4×4 km) contre 117 avant les zones — 55 à
  moins de 200 m d'un mur, 3 sur une mare de zone, 171 ailleurs ; le
  calme du relief le long des murs n'en a retiré que 11 : c'est la
  percée des exutoires (enquête eau du plan) qui doit les traiter, pas
  un réglage de zone. Jugement dev EN ATTENTE ; leviers : la table
  d'archétypes (poids, conditions, grammaires), `zoneStepHeight`,
  `zoneStoreys`, `zoneTrendWavelength`, `zoneStartGap`.
- **2026-10-08 — « Je ne comprends pas pourquoi tu me mets ce disque de
  départ plat, plus grand qu'une carte ; je n'en ai pas besoin ; en plus
  on commence sur une gigantesque colline. » Le départ est le monde tel
  quel.** Retirés : la prairie aplatie (1,2 km + 4 km), l'ancre d'étage
  (12 km + 24 km), les zones forcées en prairie (1,6 km) et le spawn
  « belvédère » (le sommet du POI le plus proche du centre, que j'avais
  mis pour juger le relief). Il reste un anneau de 4 km + 4 km de fondu
  (terre, pas de massif, climat tempéré) qui n'aplatit rien : sans lui,
  la carte (0,0) de la seed 1337 est en mer. Le spawn redevient la
  spirale « doux, sec, tempéré » depuis le centre. Deux bugs au passage :
  `noise::smoothstep01` divisait par zéro sur une bande nulle (un fondu
  à 0 m donnait des hauteurs NaN lues comme de la mer : « 4 lacs,
  0 rivière ») ; et le socle des zones, calculé sur les deux sites les
  plus proches, **pliait à chaque sommet de Voronoï** de la moitié de
  l'écart de palier (11 m mesurés à la ligne x = 8 192) — il est
  maintenant une **partition lisse sur les neuf sites voisins** (poids
  qui s'éteint à `zoneWallWidthOne` derrière le plus proche), continue
  partout ; la largeur par pas (`zoneWallWidthHigh` × pas) ne sert plus
  qu'au masque de mur. Le test de continuité à la ligne lit la dérivée
  seconde du socle avec la courbure d'un ressaut lisse pour borne (3 m).
  **Mesuré** (v78, hash 15662755146518221576) : carte (0,0) spawn
  (4 248, 860, 5 135) — le départ de la seed 1337 est à 860 m, pays haut
  sans ancre ; autour du spawn pente 26,5 %, pas > 30° 12,7 %, > 45°
  5,7 %, murs 3,7/km (médiane 23 m, max 324 m), relief 34 m / 250 m,
  sol > 100 m 97 % ; carte : relief 36 m / 250 m, 234 lacs (94 près
  d'un mur, 8 sur une mare, 132 ailleurs), 152 rivières. Carte (1,0)
  spawn (12 128, 1 035, 4 407) : pente 48,8 %, pas > 30° 20,5 %, > 45°
  13,1 %, murs 5,0/km (max 417 m), 176 lacs. Si le départ doit redevenir
  bas : `worldAnchorRadius`/`worldAnchorFade` dans le pupitre.
- **2026-10-08 — « C'est pas mal mais les variations ne sont pas assez
  rapides : des zones de 512 m plutôt que 1 km. »** `zoneCellSize`
  512 m par défaut (code et pupitre) ; les rayons des pièces de zone,
  écrits pour 1 km, suivent la taille de cellule (une mare de 180 m ne
  remplit pas une zone de 512 m). Les tests de continuité à la ligne
  (zones et couche monde) ont pour borne la courbure d'un ressaut lisse
  (3 m), pas 2. **Mesuré** (v78, hash 5026885998955634159) : carte
  (0,0) spawn (3 944, 969, 3 057) ; autour du spawn pente 73,7 %, pas
  > 30° 23,2 %, > 45° 14,0 %, **murs 8,3/km** (médiane 24 m, max 327 m
  — était 3,7/km à 1 km), relief 53 m / 250 m, montée p95 199 m ;
  carte : relief 69 m / 250 m (était 36), sol > 100 m 90 %, 222 lacs
  (113 près d'un mur) ; carte (1,0) : pente 64 %, murs 7,4/km (max
  442 m), 174 lacs. Les disques de 2 km avec ≥ 100 m d'étendue de socle :
  45 sur 49. Jugement dev EN ATTENTE.
- **2026-10-08 — « Et ajouter un peu plus d'érosion en haut ? »** Chaque
  archétype porte son **budget d'érosion** (`ZoneArchetype::erosionCut`,
  en données : 0 prairie / bocage / marais / bassin / côte basse, 10
  lande, 15 plateau-mesa, 20 replat boisé et haut plateau, 40 collines,
  60 falaises de côte, 80 pierrier, 120 badlands, 200 crête-massif) :
  un plancher sous la règle calme/versant du stage-1, hors flancs des
  POI, les murs gardent leur budget léger. Avant, l'intérieur des zones
  était calme par la règle (0,5 m) et les crêtes de 150 m restaient
  brutes. Mesuré (clé 873a480d5fe2a38d) : carte (0,0) spawn déplacé
  (5 095, 933, 2 249), pente 30,6 % (était 73,7), pas > 30° 10,3 %,
  murs 3,4/km, relief carte 66 m / 250 m, 211 lacs ; carte (1,0)
  quasi inchangée (pente 65 %, murs 7,3/km, 167 lacs) — le budget ne
  mord qu'autant que le fastscape a d'itérations (48,
  `bakeFluvialIterations` dans le pupitre). **Rappel dev le même jour :
  plus de rebuild Debug ni de tests avant un commit sauf nécessité** —
  règle durcie en mémoire ; le hash de `MapBakerTest` est ré-épinglé
  au push. Jugement dev EN ATTENTE.
- **2026-10-08 — « Mets plutôt 160 en budget érosion pour crête-massif.
  Attention, j'ai repéré un bug quand une rivière longe une frontière de
  map. »** Crête-massif à 160 m. Le bug, mesuré par deux nouveaux
  recensements du rapport (« rivers vs map lines » : nœuds à moins de
  150 m d'une ligne, pire série avec son étendue LE LONG de la ligne,
  son tier, le profil du sol et de l'analytique en travers ; « master
  courses vs map lines » : la même chose sur le réseau maître) : un
  fleuve (tier 2) longeait la ligne nord de la carte (1,0) sur 1,5 km
  et un autre la ligne sud de la (0,0) sur 1 km, et le réseau maître
  lui-même y avait des séries de 1 à 2,5 km. **Cause** : le réseau de
  zones est aligné sur les axes, donc ses murs sont parallèles aux
  lignes de carte ; un mur à moins de 300 m d'une ligne qui descend
  VERS elle, plus la chaîne de bordure (un relief additif), font une
  gouttière entre les deux ; le réseau maître (analytique, sans la
  chaîne) la suit, l'empreinte la creuse, le fastscape la garde. Le
  profil pointu de la crête (t²) et un budget léger dans la bande de
  crête n'y ont rien changé ; décaler le réseau d'une demi-cellule
  (gardé : une rangée de zones chevauche chaque ligne) n'a fait que
  déplacer les murs à ±256 m. **Règle retenue (une règle de zone, pas
  une rustine d'hydrologie) : le rempart.** La rangée de cellules
  qu'une ligne de style crêtes traverse n'est jamais en dessous d'une
  voisine (`ZonePlan` : `ridgeLineCrossesCell`, max des 8 étages bruts
  + `rampartSteps`, pupitre `zoneRampartSteps`, 0 par défaut) : la
  chaîne de bordure naît sur une ligne de partage des eaux, aucun mur
  ne descend vers la ligne. Pour que les zones lisent le style des
  segments, `MapGrid.hpp/.cpp` porte désormais `MapEdgeStyle`,
  `mapBorderStyle` et le **veto Mer lu sur la couche monde**
  (`mapBorderSegmentStyle`, 9 échantillons `worldSampleAt`, sous
  2 m = océan) ; `mapBorderStyleResolved` de TerrainGen y délègue (le
  veto lisait l'analytique, qui dépend des zones : circulaire).
  Mesuré (clé 7d8cc932079f62e4) : nœuds à moins de 150 m d'une ligne
  173 → 79 sur (0,0), 127 → 42 sur (1,0) ; **pire série le long d'une
  ligne 126 m sur chaque carte** (des traversées obliques ; avant
  1,5 km et 1 km) ; réseau maître : séries max 20 → 8 nœuds ; l'étage
  de zone est plat en travers des lignes (180/240 m). Effets de bord :
  carte (0,0) spawn pente 74 % (le départ tombe maintenant sur un
  versant, 213 lacs, relief 55 m / 250 m) ; carte (1,0) 161 lacs,
  relief 42 m. Test différentiel `zones: the rampart` (taux de
  violation avec/sans la règle). Jugement dev EN ATTENTE ; la suite =
  Z3 murs et portes.
- **2026-10-09 — Z3 livrée : murs et portes.** Chaque frontière entre
  zones a un **style** (`BorderStyle`, lu au `zoneSampleAt`) : un
  palier d'écart = **bande de falaise** (franchissable à ses encoches),
  deux ou plus = **escarpement** (brèches-rampes), deux zones de massif
  (`minMassif ≥ 0,3`) de même étage = **crête** (`ridgeHeight` 30 m,
  coupée à ses cols ; aucune paire de ce genre sur les cartes (0,0) et
  (1,0) : 0 col). Les **portes** sont hachées par paire de zones (sans
  ordre), une par frontière, une seconde avec `gateExtra` 0,35,
  glissées le long de la médiatrice ; leur masque vit dans le repère
  gauchi comme la frontière (la rampe est sur le ressaut où qu'il
  serpente) ; toutes les paires candidates sont lues (continu aux
  sommets de Voronoï), pondérées par « la paire est bien les deux plus
  proches ». Une porte = le **couloir** existant (`corridor` = max du
  couloir de marche et de la porte : pas de scarp, terrasses nulles,
  relief calmé) avec **sa propre rampe** (`gateFloor` : les deux étages
  joints sur une demi-longueur = 0,6 × largeur du ressaut + 60 m, ou
  ce qu'il faut pour rester sous 35 %), largeur `gateHalfWidth` 40 m
  (×1,75 pour une brèche), et un **replat de révélation** en haut de
  la rampe (`gatePad`, règle du triangle : le mur cache, l'encoche
  révèle). Première version à 600 m de tube sur la rampe de 400 m des
  marches : 43 % de la carte en « porte » sur le `plan.png` — ramenée
  à des coupes courtes. Pupitre : `zoneGateHalfWidth`, `zoneGateExtra`,
  `zoneRidgeHeight`. Instrument « walls and gates » (raster 16 m,
  composantes 4-connexes, transformée de distance chanfrein) :
  carte (0,0) 506 portes (463 encoches, 43 brèches), **tout point de
  mur à moins de 267 m d'une porte (p95 123 m)** — la règle d'août
  « aucun mur de 400 m sans passage » tenue ; carte (1,0) 252 portes,
  277 m / 117 m. Les « tronçons » en bbox (976 / 1 408 m) chaînent
  plusieurs frontières par les sommets : ce n'est pas la mesure utile.
  Lacs 213 → 165 sur (0,0) (les rampes drainent des creux). Le
  `plan.png` dessine murs (sombre), portes (blanc), crêtes (violet).
  Test « zones: gates through the walls ». Jugement dev EN ATTENTE ;
  la suite = Z4 POI emboîtés et visibilité.
- **2026-10-09 — « D'où vient le fleuve entouré de murs sur la carte
  zéro ? »** Le rapport liste maintenant les cours maîtres qui touchent
  la carte (source, fin, bassin, mer / confluence / bord de routage).
  Celui de la carte (0,0) naît à (5 504, 256), juste sous la ligne
  nord, bassin de 289 km², et **atteint la mer 38 km plus loin, à
  (19 712, 23 936)** — carte (2,2), au sud-est ; il reçoit un affluent
  de 15 km venu de 3 km au nord de la carte (96 km²) et deux autres de
  59 et 75 km². Les murs qui l'encadrent : un cours maître suit la
  ligne la plus basse de l'analytique, qui avec les étages de zones
  est le pied des murs — il coule donc entre les ressauts, et
  l'empreinte creuse son lit de 23 m en plus.
- **2026-10-09 — Push de la refonte (33 commits depuis le 2026-10-05) :
  la suite complète en Release a trouvé cinq cas rouges, dont trois
  vrais bugs.** (1) **Porte en miroir** : le repère de la porte
  dépendait de la zone la plus proche ; en traversant la frontière, la
  glissière changeait de signe et la rampe se pliait (3,2 m de seconde
  différence) — repère ordonné par le hash des deux sites, et le sol
  des portes est une moyenne pondérée par masque des rampes de toutes
  les paires (l'argmax sautait d'une rampe à l'autre). (2) **Rempart
  du stage-1 seul** : `zones.mapSize` était posé sur une copie locale
  dans `bakeMap` ; les tranches et le runtime lisaient les params de
  l'appelant sans rempart — 180 m d'écart entre overview et tranches.
  Une seule source : `makeTerrainBakeParams` et la fixture de test.
  (3) **Entaille des cours d'eau dans les falaises** : la gravure des
  rivières (lit parabolique + épaulement de berge) et celle des lits
  de lac abaissaient à la ligne d'eau tout texel du ruban, falaise
  comprise — une entaille de 200 m sur 2 m de large là où un ruisseau
  longe un mur de zone (diagnostic : texels bruts du stage-1 contre la
  tranche au même endroit). Invisible avant les murs raides. Borne
  `FinalizeParams::bankMaxCut` 3 m : une coupe d'eau ne descend jamais
  sous le sol intact de plus que le lit plus une berge. Trois jauges
  re-exprimées, pas des bugs : l'overview à 64 m ne peut pas suivre un
  ressaut de 240 m tenu dans un texel de 16 m (jauge = part des
  échantillons à plus de 60 m ≤ 8 %, pire < 400 m) ; le pli de 4,7 m
  sur x = 8 192 est le ressaut d'un mur du rempart sous la compression
  du gauchissement (jauge 6 m, un raccord se lit en dizaines de
  mètres) ; le bouton `verticality` n'ajoute plus que moitié aux pas
  > 45° (les murs de zones et leurs portes tiennent à 0 aussi) —
  jauge 1,5×. `biomeIdAt` lit le même plancher que `at()` (rampe de
  porte, crête). Hash de `MapBakerTest` ré-épinglé
  (7395177285592674686, v78).

### 7.8 La passe EAU/CONTINENT (2026-10-09 →, avant Z4)

Déclencheur dev : « un gros fleuve sur la carte (0,0) alors que
j'imagine que c'est la source » — un cours maître naît au seuil
`fleuveArea` à ~38 m de large (plancher 12 m × 1,6), sans amont ; et la
mer n'apparaît qu'au cran 100 km de la minimap. Vision validée : un
monde cohérent fait de continents, les fleuves coulent jusqu'à la mer à
travers les cartes ; en montagne les sources sont des résurgences, de
la fonte ou des exutoires de gros lacs — les lacs font partie du
réseau ; la far view et la carte traversée racontent la même eau.

**Décisions dev (2026-10-09).** (1) **Archipel infini** : continents
finis sur une grille déterministe, océan entre, le sandbox reste
infini ; chaque continent = unité hydrologique complète. (2)
**Continent ≈ 3×3 cartes** : la masse terrestre tient dans le CŒUR
d'une super-région du réseau maître (propriété des têtes demi-ouverte)
— diamètre ~21-22 km, R ≈ 10,5-11 km, un continent par super-cellule
(i·P, j·P), P = `worldContinentCellSupers` (défaut 2 → un continent
tous les 49 km) ; plus aucune couture cross-super (zéro tête chez les
voisins). (3) **Maintenant, avant Z4/Z5** : la passe absorbe l'enquête
eau des lacs (percée des exutoires) et le cœur de N4 (frontières lues
sur l'étage monde, Ouvert ailleurs). Principes : l'accumulation devient
un DÉBIT (`qArea`, ruissellement climatique), la loi de largeur est
UNIQUE et continue (plancher 12 m supprimé, ×1,6 en rampe), le tier est
PAR POINT (guéable en amont, obstacle en aval, monotone par
construction), les lacs sont des nœuds. Le plan détaillé vit dans le
plan de session ; l'érosion reste NON pondérée (knob réservé).

| # | Brique | Contenu | Mesure de sortie |
|---|---|---|---|
| E0 | Pupitre eau | params réseau/hydro/empreinte/pluie dans le Form (la clé de carte les suit enfin) | zéro delta visuel ; round-trip + clé testés |
| E1 | Archipel | treillis de continents dans `worldSampleAt` (A/B : `worldContinentCellSupers` 0 = porteur hérité) | terre ⊆ cœur de super-cellule ; (0,0) à terre ; (3,3) océan |
| E2 | Frontières lues sur le monde | `Open` + styles depuis `worldSampleAt`, hash/veto supprimés ; `bake-map --pair` | répartition Mer/Ridges/Open ; divergence Open < 10 m RMS ; baseline orphelins d'eau |
| E3 | Routage façonné | le réseau maître route sur `applyMapGridShape` (remparts = partages des eaux) | 0 cours le long des lignes Ridges ; % cours à la mer stable+ |
| E4a | Débit | `routeFlow` pondéré (climat au 128 m, biome au 16 m), `qArea` (MNW2) | poids=1 ⇒ bit-exact ; ratio largeur humide/sec mesuré |
| E4b | Continuum | extraction à 1,5e6, largeur continue sans plancher, tier par point (TWB4), gués tier 1, far-water unifié | tête ≤ 2× le gabarit local ; tiers monotones ; gués tier 1 seuls |
| E5 | Lacs | bassins maîtres nœuds, percée des exutoires, `catchment` → largeur d'exutoire | 3-6 lacs/carte (vs 165) ; 0 lac sans exutoire hors retenus |
| E6 | Sources | typologie des têtes (source/névé/glacier/lac), taper + bonus par type | histogramme des types ; exutoires 100 % typés lac |
| E7 | Cohérence | bilan par continent, `--pair` surfaces d'eau, vérif far-view | ≥ 95 % des cours à la mer ; 0 orphelin > 10 m |

Différés de la passe : deltas/estuaires, traversée des détroits,
habillage visuel des sources, couche peinte histoire, flowSpeed par
nœud, pondération de l'érosion, peuplement (Z4/Z5 suivent sur
l'hydrologie définitive).

- **2026-10-09 — E0 livrée : le pupitre eau.** Treize knobs entrent au
  Form (et donc dans la clé de carte, qui ne hachait NI `network` NI
  `hydrology` NI `imprint` NI `waterSolve` — changer un défaut C++
  était invisible du cache) : `networkFleuveArea`, `networkTexel`,
  `hydroRiverArea`, `hydroRiviereArea`, `hydroWidthCoef`,
  `hydroWidthExponent`, `hydroFleuveWidthScale`, `hydroMinLakeDepth`,
  `hydroMinLakeCells`, `imprintBedDepth`, `imprintPlainFactor`,
  `imprintMinGradient`, `waterRainRate` (référence = le défaut
  `TileBakeParams::waterSolve` 4e-6, pas celui de la struct). Défauts =
  identité bit-exacte ; pas de bump (la clé bouge via le hash du Form).
  Mesuré : les 2 cas `TerrainGenTuningTest` verts (défauts champ à
  champ, round-trip + clé). Jugement dev EN ATTENTE.
- **2026-10-09 — E1 livrée : l'archipel.** Le porteur fbm 45 km cède à
  un **treillis de continents** dans `worldSampleAt` : un continent par
  super-cellule hôte (i·P, j·P), P = `worldContinentCellSupers` 2 (soit
  un tous les 49 152 m), champ radial 1 − d/R_eff (R
  `worldContinentRadius` 11 000, forme fbm 15 km ± 18 %), converti en
  unités porteur par `kContinentGain` **0,5** — à 0,35 la rampe côtière
  (`coastBand` × 5,5 = 0,44) ne saturait jamais : les cœurs de
  continents perdaient leurs hauts étages (≥ 800 m : 1,2 % → 4,1 %).
  Warp 12 km et détail de côte 5 km inchangés (le warp n'entre que par
  le fbm de forme : la géométrie reste exacte). **Garde de cœur** :
  champ signé continu (marge 640 m, falloff 2 048 m) qui plafonne le
  porteur APRÈS le décret de départ — aucune terre ne sort jamais du
  cœur de la super-cellule hôte, donc chaque tête de cours appartient à
  sa fenêtre et la couture cross-super meurt par construction
  (`WorldLayerTest` : terre ⊆ cœur sur 5 seeds, départ à terre,
  continuité 16 m). Jitter des centres ±788 m, **forcé sud-ouest pour
  le continent (0,0)** (le départ (4096,4096) est à 11,6 km du centre
  de cellule : l'offset le met ~500 m à l'intérieur de la côte
  nominale — départ côtier, quadrant SO). Le proxy de distance de côte
  de `macroHeightAnalytic` devient exact en treillis (R/gain au lieu de
  longueur d'onde × `kShoreProxy`). **Les POI d'océan profond ne se
  posent plus** (`typedSite` : `sea && coast ≤ 0` = absent) — sans ça
  le treillis moyen semait des îlots-cônes de 80 m au pas de 2 km dans
  chaque détroit ; les îlots/fjords de la ceinture côtière restent.
  A/B : `worldContinentCellSupers` 0 = porteur hérité intégral.
  Pupitre : `worldContinentCellSupers/Radius/Jitter/ShapeWavelength/
  ShapeAmp`. Bump **v79**. Mesuré (seed 1337) : dans les cœurs hôtes
  sur 200 km — mer 38,3 % (la ceinture côtière du continent ; l'océan
  inter-continents porte le reste), terre < 150 m 29,5 %, 150-450
  49,4 %, 450-800 17,1 %, ≥ 800 4,1 %, massif 11,1 % ; cœur (0,0) :
  9 cartes dont 2 pleinement terrestres, 9 grands / 121 moyens / 2 397
  petits, paires adjacentes de même type 5/79, 1 moyen isolé toléré
  (bout de côte sans voisin terrestre à 2,2 km) ; carte 8 km : stage-1
  6,6 s + tranches 4,1 s (M1 Release). Suite complète Release
  **807/807** ; tests ré-ancrés : distribution d'étages lue dans les
  cœurs (mer ≤ 45 %), sommets-amers 6 cartes de terre min, densités
  POI sur cartes terrestres seules. Hash cartes AppleClang
  14247880702678614143 ; le pin MSVC (7395177285592674686, v78) est
  périmé — à ré-épingler à la première suite dev. Jugement dev EN
  ATTENTE (valider au `landscape-report` : l'archipel au cran 100 km,
  le départ côtier, la silhouette du continent).
- **2026-10-10 — E2 livrée : les frontières lues sur le monde (le cœur
  de N4).** `MapEdgeStyle` gagne **`Open`** — la ligne n'est RIEN, le
  paysage continue — et `mapBorderSegmentStyle` lit le monde le long du
  segment (les 9 échantillons existants) : majorité océanique ⇒ Mer,
  moyenne de `massif` ≥ `worldBorderMassifMin` (0,45) ⇒ Ridges, sinon
  Open. Le hachage 50/50 et le veto disparaissent du chemin par défaut
  (`worldBorderFromWorld` off = styles hachés hérités, le veto Mer
  seul conservé). `sampleBorders` passe en poids séparés crête/mer
  (un segment Open ne contribue à rien — il n'est pas « l'autre
  style ») ; le rempart des zones ne s'applique qu'aux lignes Ridges,
  comme avant. Nouvel instrument **`cooker bake-map … --pair <bx>
  <bz>`** : bake des deux cartes adjacentes, RMS + max des hauteurs
  sur la bande partagée (±`overlapMargin`), appariement des points de
  rivière de part et d'autre de la ligne (orphelins à > 50 m, delta de
  surface des appariés), style du segment dans la ligne de log — la
  jauge de couture de la passe (E7 la consommera). Mesuré (seed
  1337) : continent (0,0), 24 segments internes/périphériques → **10
  Open, 1 Ridges, 13 Mer** (le pays de départ s'ouvre ; le massif y
  est rare — `worldBorderMassifMin` est le levier si le dev veut plus
  de murs) ; tests rempart et formes de bordure re-ancrés sur les
  styles hérités (ils testent la règle et les profils, pas
  l'emplacement des lignes). Bump **v80**. Suite complète Release
  **808/808**. Jugement dev EN ATTENTE.
- **2026-10-10 — E3 livrée : le réseau maître route sur la surface
  façonnée.** `computeMasterNetwork` échantillonne
  `applyMapGridShape(macroHeightAnalytic)` quand
  `MasterNetworkParams::mapGrid` est valide (nouveau membre, haché dans
  la clé du cache MNW avec `routeOnShaped`) : les remparts Ridges
  deviennent de vraies lignes de partage des eaux et les cours
  franchissent aux COLS de la crête — fini le cours maître qui ignore
  un mur que le bake élève ensuite autour de lui (le « fleuve entouré
  de murs » du journal du 2026-10-09). Le spec est plombé partout où un
  `MasterNetworkParams` se construit : l'empreinte et la promotion
  (TileBake), le far-water et les sources de sim (LandscapeScene), les
  outils (`water-solve`, `landscape-report`), la fixture de test — et
  `makeTerrainBakeParams` remplit désormais le spec complet (seed,
  mapSize, seaLevel) à la source. Le réseau POI reste VOLONTAIREMENT
  non façonné en plus d'être plan-free (le façonnage lit les zones qui
  lisent le plan qu'il type — même cycle) ; l'image `terrain-map`
  (dev, échelle continent) dessine le réseau nu, différence aux cols
  seulement. Défaut périmé corrigé : `MapGridSpec::mapSize` 24 576 →
  8 192. A/B : `networkRouteOnShaped` (pupitre). Bump **v81**.
  Mesuré : suite complète Release **808/808** ; hash cartes AppleClang
  14247880702678614143 (E1) → 14335405370102949114 (E2+E3 : frontières
  monde + routage façonné actifs dans le bake). Jugement dev EN ATTENTE
  (au `landscape-report` : « master courses vs map lines » ≈ 0 run le
  long des lignes Ridges, % cours à la mer stable, surcoût
  d'échantillonnage du routage dans la ligne « analytic samples »).
- **2026-10-10 — E4a livrée : le débit (qArea).** `routeFlow` accepte
  des poids de ruissellement par cellule (multiplicateur de l'aire de
  cellule avant accumulation) ; le réseau maître les tire du
  `WorldSample` déjà sous la main (`macroHeightAnalytic` gagne une
  surcharge qui le rend) : `mix(hydroRunoffDryMin 0,4,
  hydroRunoffWetMax 1,6, moisture) × (1 + hydroSnowBonus 0,8 ×
  froideur sous hydroSnowlineTemp 0,25)` — les montagnes humides ou
  enneigées portent plus d'eau que leur aire nue, les plaines sèches
  moins ; ~1 en tempéré, les seuils gardent leur sens. Le nœud maître
  porte **`qArea`** (accumulation pondérée, second balayage du même
  graphe de receveurs) à côté de l'aire topographique brute — les LOIS
  (largeur, tiers, débits de bord) la liront en E4b ; il réserve aussi
  `flags` (bit lac, E5) et `headType` (typologie, E6) → format
  **MNW2** en un seul bump, nœud sans padding. L'érosion reste NON
  pondérée (décision actée, `erodeFluvial` passe nullptr). A/B :
  `hydroRunoffWeights` ; poids neutres ⇒ `qArea == area` bit-exact
  (testé). Pas de bump tuile (aucun consommateur encore : sortie
  publiée identique, vérifié par le hash cartes stable). Mesuré (seed
  1337, super (0,0)) : qArea s'écarte de l'aire de > 1 % sur **481 des
  517 nœuds**. Jugement dev EN ATTENTE.
- **2026-10-10 — E4b livrée : le continuum ruisseau → rivière →
  fleuve.** La réponse à la question d'origine du chantier (« un gros
  fleuve naît sur la carte zéro ») : (1) **l'extraction descend au
  gabarit rivière** — `networkRiverExtractArea` 1,5e6 m² de qArea
  (demi-largeur ~5 m, le minimum lisible au texel 128) remplace
  `fleuveArea` comme seuil d'extraction du réseau ; `fleuveArea` 6e6 ne
  marque plus que le TIER obstacle. (2) **Une seule loi de largeur
  continue** (`RiverWidthLaw`, Hydrology.hpp) partagée empreinte /
  classification / far-water : `min(coef·q^exp, 45) × rampe(fleuve)` —
  le **plancher 12 m est supprimé** (une tête naît à ~10 m de large et
  grossit à chaque confluent) et le ×1,6 « GRAND » devient une rampe
  smoothstep sur le débit (`hydroFleuveRampLow` 4e6 →
  `hydroFleuveRampHigh` 1e7) au lieu de multiplier le cours entier.
  (3) **Le tier est PAR POINT** (`RiverPoint::tier`, u32 sans padding)
  depuis le qArea du nœud maître apparié (≥ riviereArea = 1, ≥
  fleuveArea = 2), monotone aval par running-max : un même cours est
  rivière guéable dans ses montagnes et fleuve-obstacle dans sa
  plaine — remonter le fleuve pour le franchir devient un motif de
  jeu. Garantie obstacle : `hydroTier2MinHalfWidth` 12 m au point
  tier 2. `River::tier` = max des points (compat lecteurs) ; gués
  adoptés sur les seuls points tier 1 ; la porte « ≥ 50 % des points
  matchés » disparaît (le match est par point). (4) **TWB4** :
  `mouthArea`, `lakeFed` sérialisés (la reclassification au rechargement
  suit le bake), `sourceLake`/`headType` réservés (E5/E6). (5) Le
  far-water et `masterBoundarySources` lisent la loi et le qArea
  (largeurs et débits de bord cohérents avec l'empreinte). Différé :
  le ruissellement par biome au 16 m local (le climat entre déjà par
  le qArea maître ; à ajouter si le dev veut des ruisseaux locaux
  sensibles au biome). Bump **v82**. Mesuré : tête de cours au seuil =
  ~9,8 m de demi-largeur par la loi (contre 19,2 plancher avant) ;
  suite complète Release **809/809**, hash cartes AppleClang
  1720697667797144396. Jugement dev EN ATTENTE (remonter le fleuve de
  la carte (0,0) : continuum lisible, gués en amont, obstacle en
  aval).
- **2026-10-10 — E5 livrée : les lacs — la percée des exutoires
  (l'enquête eau) et le réseau lake-aware.** `pierceLakeOutlets`
  (Hydrology) tourne AVANT l'extraction, sur la fenêtre hydrologique de
  la carte : flood, composantes inondées, et toute dépression qui ne
  MÉRITE pas son lac se vide par une **encoche descendante taillée à
  son déversoir** (gate au plancher + 0,3 m, rampe `riverMinGradient`
  le long de la descente du filled, 96 pas max), itéré **8 passes**
  (une cuvette vidée révèle la suivante — mesuré : 92 lacs à 4 passes,
  14 à 8, saturé à 12). Le mérite : `hydroLakeKeepCells` **400**
  (~10 ha — un lac-destination est GRAND) ; la rétention par
  profondeur est OFF par défaut (`hydroLakeKeepDepth` 1000 : avec des
  paliers de zone à 60 m, chaque fosse de pied de mur lit 10-40 m de
  profond — profond ≠ destination). **Liaison lac↔rivière** :
  `Lake::catchment` (le max d'accumulation du bassin — l'exutoire
  porte tout), `River::sourceLake` nommé à la tête (le clip par
  tranche recopie désormais mouthArea/lakeFed/sourceLake/headType —
  ils se perdaient, 0 exutoire nommé avant la correction) ; la largeur
  d'exutoire suit déjà la loi (l'accumulation traverse la surface
  remplie). Côté maître : `kMasterNodeLake` sur les nœuds traversant
  un bassin rempli de plus de `networkLakeDepth` 2 m (far-water et
  typologie E6 le liront). **TWB5** (catchment). Mesuré (seed 1337,
  carte (0,0)) : **14 lacs naturels** (vs 165 au journal Z3 ; les 14
  sont les vrais ≥ 10 ha — la cible 3-6 est un cran de
  `hydroLakeKeepCells` à arbitrer au pupitre), 56 étangs posés (règle
  de rendu, hors passe), 3 exutoires nommés sur la carte ; test
  synthétique « bol percé / tarn retenu » ; garde `MapBakerTest` ≤ 24
  lacs naturels. Bump **v83**. Jugement dev EN ATTENTE (le nombre et
  la place des lacs-destinations au `landscape-report`).
- **2026-10-10 — E6 livrée : la typologie des sources.**
  `RiverHeadType` (Spring / Neve / Glacier / LakeOutlet) décidé à la
  TÊTE de chaque cours maître — née dans un bassin rempli (>
  `networkLakeDepth`) = exutoire de lac, sinon la température du monde
  à la tête tranche (`hydroGlacierTemp` 0,12, `hydroNeveTemp` 0,22) —
  et écrit sur tous les nœuds du cours (le match par point le lit
  n'importe où) ; `River::headType` local copié du premier point
  apparié, un `sourceLake` nommé prime. Deux décisions
  d'implémentation : pas de bonus de débit par type (le `snowBonus`
  d'E4a EST déjà le débit de fonte — pas de double compte) et le taper
  de source inchangé (il court AVANT la classification ; `lakeFed`
  couvre déjà le cas exutoire). L'habillage visuel (langue glaciaire,
  névé, résurgence, cascade d'exutoire) reste le différé annoncé de la
  passe. Bump **v84** (le .twb embarque les types). Mesuré (seed 1337,
  super (0,0)) : 63 têtes — 30 sources, 1 névé, 0 glacier (le
  continent du départ est tempéré), 32 exutoires de bassins. Jugement
  dev EN ATTENTE.
- **2026-10-11 — E7 livrée : le bilan par continent — la passe tient sa
  jauge.** `landscape-report` gagne la ligne « continent (super i, j) » :
  cours maîtres de la super-région hôte, part atteignant la mer
  (TRANSITIVE : atteint ou rejoint un cours qui atteint — une
  confluence termine un tracé par construction), plus long cours,
  histogramme des nœuds par tier (qArea vs riviere/fleuveArea) et des
  têtes par type ; la ligne « map » sépare lacs naturels et étangs
  posés. La vérif far-view annoncée au plan est devenue TAUTOLOGIQUE :
  depuis E4b le far-water appelle la même `riverLawHalfWidth` que
  l'empreinte — une seule fonction, rien à comparer. Le volet
  « surfaces d'eau au corridor » vit déjà dans `bake-map --pair` (E2).
  **Mesuré (seed 1337, continent (0,0), v84)** : **64 cours maîtres,
  64 à la mer (100 % — jauge ≥ 95 % tenue : la garde de cœur E1 + le
  routage façonné E3 ont tué les troncatures de fenêtre)**, plus long
  cours 15,9 km de la source à la mer ; nœuds 210 ruisseau / 657
  rivière / 550 fleuve (le continuum existe en volume) ; têtes
  31 sources / 1 névé / 0 glacier / 32 exutoires ; 14 lacs naturels +
  56 étangs sur la carte (0,0) (jauge 3-6 : un cran de
  `hydroLakeKeepCells`, arbitrage dev) ; « 0 orphelin > 10 m aux
  lignes » : à mesurer au `bake-map --pair` sur une paire Ouvert et une
  paire Ridges (machine dev, deux bakes). Fin de la passe EAU côté
  code : restent le jugement dev (archipel au cran 100 km, départ
  côtier, remonter le fleuve, lacs) et les différés listés en tête de
  section.
- **2026-10-11 — « Les îles sont étrangement régulières : rondes,
  réparties uniformément — et celle de départ a un coin carré.
  L'ancien algo d'août n'était pas exploitable ? »** Retour dev fondé
  des deux côtés : les continents E1 étaient des champs radiaux
  1 − d/R à peine bruités sur un treillis à jitter minuscule, et le
  coin carré était la garde de cœur rectangulaire (le jitter forcé SO
  du départ l'appuyait pile dans l'angle). **E1b livrée.** Deux
  enseignements de la refonte, mesurés sur images (`terrain-map`,
  comparées par Claude avant livraison — avant/après dans
  `terrain-cache/1337/archipel-*.png`) : (1) le porteur 45 km d'août
  ne peut PAS dessiner la côte d'un continent de ~20 km — il est
  tout-haut ou tout-bas à l'échelle d'une cellule, la côte retombe sur
  l'enveloppe (rond) ou le cœur (carré) ; (2) la bonne recette est
  celle de l'ancien `continentLayout` à la bonne échelle : **un disque
  fortement warpé** — rayon modulé par un fbm de rive par continent
  (`worldContinentShapeAmp` monté à 0,4) lu à travers une déformation
  du domaine de ±3,2 km à 9 km (golfes profonds, presqu'îles, aucun
  cercle lisible), le cœur arrondi (rayon de coin 3 072 m) ne restant
  qu'un filet de sécurité sur coordonnées NON warpées (l'invariant
  têtes-de-cours tient). S'y ajoutent : **quinconce** (rangées impaires
  décalées d'une super-cellule), **variance de taille** (R × 0,55-1),
  **cellules vides** (`worldContinentChance` 0,8, la cellule du départ
  toujours pleine), jitter 2 200 m (+ large pour les petits), R défaut
  9 500 (11 000 laissait zéro place à la côte dans le cœur), départ
  décalé SO de 2 048 m et rive du départ clampée ≥ 0 (le départ reste
  dans la côte, les golfes restent). Proxy de côte recalé
  (`kContinentCoastSlope` 0,30). Conséquence de composition : la rampe
  côtière sature au cœur des îles → plus de haut pays qu'avant (terre
  ≥ 800 m : 18 % — jauge du test élargie à ≤ 25 %, part finale à
  arbitrer en N5) ; le test d'étages mesure la terre sans filtre (la
  terre n'existe que dans les cœurs par invariant), la part de mer
  par continent vit dans la ligne « continent » du report. Vérifié sur
  3 cadrages (160 km seeds 1337 et 777, 260 km) : tailles d'îlot à
  continent, formes toutes différentes, ~1 cellule sur 5 vide, aucun
  coin. Quatre jauges ré-ancrées en mesures PAR CARTE DE TERRE
  (l'archipel a réduit la terre du balayage, pas sa densité) :
  sommets-amers 263 sur 4 cartes terrestres (≥ 25/carte), densités POI
  dès 1 carte pleine, 2 moyens isolés de bout de côte tolérés, ratio
  verticality ≥ 1,3 (1,38 mesuré — part à arbitrer en N5). Bump
  **v85** ; suite complète Release **811/811**. Jugement dev EN
  ATTENTE sur les nouvelles images.
- **2026-10-11 — « Doublons la taille des continents et rendons les
  côtes plus déchiquetées » (retour dev) — E1c livrée.** (1) **La
  super-région double** : `superRegionSize` 49 152 (fenêtre de routage
  513², échantillonnage 0,34 s/super, cache Z1 inchangé), un continent
  par super-cellule (`continentCellSupers` 1), **R 19 000** → ~38 km
  nominal. (2) **Le treillis est DÉSAXÉ** de −16 384 m
  (`superOrigin` = `continentLatticeOrigin`, hachés partout) : le
  départ (4096, 4096) tombe près du CENTRE du cœur (0,0) au lieu de
  son coin — le continent de départ ne clippe plus jamais sur la
  garde, plus d'offset forcé. L'invariant têtes change de forme : les
  cœurs pavent le plan, la propriété devient « aucune terre à portée
  d'une ligne de treillis » (la garde), testée sur 5 seeds le long des
  lignes. (3) **Côtes fractales** : rive 4 octaves à ±45 %
  (`worldContinentShapeAmp` 0,45), warp de domaine 4 octaves ±4 200 m,
  pente littorale `kContinentCoastSlope` 0,22 — l'effet demandé
  « surface réelle ×1,5-2 pour deux fois le diamètre » vient de la
  découpe, vérifié sur images (90 et 300 km, dans
  `terrain-cache/1337/archipel-v6-*.png` : golfes, îlots satellites,
  aucun continent semblable). (4) Le ring de départ gagne un **plafond
  d'étage** (`min(e, 0,42)` fondu sur le ring) : au cœur d'un grand
  continent la rampe littorale sature et le spawn devenait du haut
  pays subalpin — le décret « doux, sec, tempéré » est restauré ; il
  se lit comme un plateau vert de ~8 km au centre du continent
  (adoucissable via `worldStartLowFade`), au jugement dev. (5)
  **Percée** : rampes 192 pas et 16 passes (hydrologie de carte
  0,4 → 2,2 s) ; une carte de CŒUR alpin chaîne les bassins de marches
  d'étages — ~32 lacs ≥ 51 ha à convergence (`hydroLakeKeepCells`
  2 000 ; rétention par taille seule) : la **percée des exutoires de
  paliers côté ZONES est le prochain levier**, différé chiffré ; garde
  du test à ≤ 48. Jauges re-mesurées : appartenance des têtes sur
  l'origine désaxée, paire de moyens ≥ 900 m, gradient littoral macro
  ≤ 0,45 (le warp raidit le PLANCHER ; coastProfile et l'érosion
  possèdent la pente finale — N5). Bump **v86**. Jugement dev EN
  ATTENTE (les deux images, le plateau du départ, le nombre de lacs
  de cœur).
- **2026-10-11 — « Il y a un effet strié de la carte » (retour dev,
  screenshot) — E8 livrée : l'eau cesse de suivre la grille.** Le
  diagnostic a demandé trois zooms d'images (22 → 3,3 m/px, A/B
  `noplan`) : les stries n'étaient ni le plan POI ni le terrain, mais
  les **cours traversant les bassins remplis** — sur une surface de
  remplissage plate, la rampe epsilon du priority-flood est l'ordre de
  balayage de la file : chenaux rectilignes alignés grille, en
  faisceaux (le différé N2 « fleuves rectilignes » mot pour mot,
  amplifié par l'extraction abaissée d'E4b et le plateau du ring
  d'E1c) ; par ricochet les LACS de la carte M se lisaient en peigne
  (le raster coupe la nappe sur chaque crête de sillon imprinté).
  Trois remèdes empilés, chacun vérifié sur image : (1) **jitter de
  l'epsilon du remplissage** (`priorityFloodFill` gagne un `epsScale`
  par cellule, hash blanc sur coordonnées MONDE — deux fenêtres
  d'accord sur leur recouvrement ; la monotonie du fill tient par
  construction de la file) branché au réseau maître, à
  `extractHydrology` et à la percée E5 — c'est LUI qui tue les
  faisceaux ; (2) **dither de routage** `networkRouteDither` 1,2 m à
  1,6 km 3 octaves, vu du routage seul (le D8 en colonnes sur pentes
  douces exige des mètres, pas des centimètres) ; (3) **Chaikin une
  passe** sur les polylignes maîtresses (nœuds ×2, escaliers 128 m
  arrondis, extrémités exactes, surfaces re-monotonisées). L'érosion,
  le dimple et le water-solve passent nullptr : bit-exacts. Vérifié :
  le 20 km avant/après (`terrain-cache/1337/stries-avant/apres-20km
  .png`) passe de faisceaux H/V à un réseau dendritique en arête de
  poisson. Bump **v87**. Jugement dev EN ATTENTE (re-regarder l'écran
  M au cran 5 km).
- **2026-10-12 — « Toute l'érosion et les rivières sont strictement
  alignées X/Z sur la carte de départ » (retour dev) — E8b livrée, et
  l'instrument qui manquait.** Deux découvertes. (1) E8 n'avait
  jittéré que l'HYDROLOGIE : le fill de l'ÉROSION fastscape restait
  uniforme — sur le grand plat du ring, l'incision elle-même gravait
  des vallons D8 en grille que l'eau (même jitterée) suivait ensuite.
  Le jitter d'epsilon est centralisé (`fillEpsJitterAt`,
  FluvialErosion.hpp — hash blanc seed-free sur coordonnées monde, UNE
  maison au lieu de trois copies) et branché au fill de
  `erodeFluvial` ; dimple et water-solve restent nullptr. (2) Aucun
  instrument ne MONTRAIT le terrain publié : le `plan.png` dessine
  l'analytique, où l'érosion n'existe pas — d'où deux allers-retours
  dev sur des stries que mes images ne pouvaient pas voir. Le
  `landscape-report` écrit désormais **`relief.png`** (hillshade 1024²
  du sol publié, mer teintée). Ce relief a aussitôt montré le défaut
  suivant : les CANAUX D'EMPREINTE en marches H/V/45° à coudes nets et
  stamps en perles sur les zigzags serrés — le Chaikin seul (rayon
  ~50 m) est invisible contre un canal de 100 m de large. Les cours
  maîtres prennent une **moyenne glissante 5 nœuds ×2** (640 m) avant
  le Chaikin : le relief.png passe de circuits imprimés à un éventail
  dendritique de courbes fluides (vérifié avant livraison).
  Résiduel noté : quelques pastilles circulaires isolées (étangs
  posés probables), au jugement dev. Trois raffinements arrachés aux
  images et aux jauges pendant la livraison : le jitter est un **value
  noise bilinéaire sur grille monde 64 m** (le hash par texel faisait
  diverger l'overview 64 m du stage-1 16 m — 11 % d'échantillons à
  plus de 60 m ; les blocs constants imprimaient des chevrons — le
  champ interpolé ne fait ni l'un ni l'autre, dérive 5-6 %) ; la
  détection `lakeFed`/`sourceLake` passe au rayon 2 (le jitter décale
  la tête d'exutoire d'une cellule du masque) ; la ligne « continent »
  du report ne compte que les cours NÉS dans le cœur hôte (les queues
  des voisins tronquées par le rect étaient comptées perdues) avec un
  rayon de jonction transitive de 320 m (le box-smooth déplace le
  parent). Bump **v88**. Mesuré (seed 1337, continent (0,0)) : **150
  cours, 142 à la mer (95 % — jauge tenue)**, plus long cours **37,6
  km de la source à la mer** sur un continent de 38 km ; nœuds
  718/2544/3484 r/R/F ; relief.png : éventail dendritique, zéro
  alignement X/Z ; dérive overview 16/256 > 60 m (jauge ≤ 8 % : 6 %) ;
  29 lacs naturels sur (0,0). Jugement dev EN ATTENTE (relancer le jeu
  — rebake v88 — et re-regarder la carte de départ, le grain de
  versants en jeu, le relief.png).
- **2026-10-12 — « L'eau simulée n'apparaît pas partout où il y avait
  l'eau prébakée » (retour dev) — enquête eau RUNTIME (état des lieux,
  rien codé).** La chaîne bake → far → sim → shader cartographiée.
  Aucun bug d'implémentation : trois décisions de design et quatre
  hypothèses périmées par la passe fabriquent l'écart. (1) Dans le
  rect de confiance de la sim (~384 m), le shader JETTE toute eau
  bakée/far inconditionnellement (coupe 64-80 m du bord,
  water_surface.glsl:215-233 ; l'ancien check par texel retiré à
  dessein) : tout ce que la sim ne mouille pas devient un trou. (2)
  Les cours < `pinRiverHalfWidth` 4 m n'ont AUCUN visuel near (ni
  quad, ni pin — seulement le film sim ≥ 8 mm), mais le mesh baké les
  LIFTE de ≥ 0,5 m et ×1,15 de loin (WaterSystem.cpp:745-780) ;
  depuis E4b (largeurs honnêtes) le monde est plein de 1-4 m → « belle
  rivière au loin, lit à sec de près » est devenu la règle. (3) Les
  sources sous-alimentent : une tête DANS la fenêtre n'injecte rien
  (seuls les accroissements de largeur comptent, WS:113-140) et
  l'inverse largeur→débit défait la seule racine carrée (ignore le
  resserrement de pente ×0,45, le plancher 1,2 m, la rampe fleuve, le
  plancher tier 2) ; `kRunoffBoost` 1,6 tuné v67. (4) Un ruban ≥ 4 m
  dont le sol 2 m n'est pas 0,3 m sous la surface .twb : ni pin, ni
  seed, ni source. Recoins : bords de lacs rétrécis d'un texel +
  crest-guard (film 8 cm), strips entrants à sec + drain de bord
  0,4/s + settle 3 s (transitoires légitimes), trois littéraux 4.0
  indépendants (filtre far TBS:547, plancher du fallback maître
  TBS:603, knob WSh:146). Options en arbitrage dev : (A) visuel near
  honnête pour les < 4 m (ruban mince non simulé) + fin du lift
  mensonger de loin ; (B) re-vérifier « la sim a de l'eau ici » avant
  de jeter le baké ; + trois correctifs sans arbitrage (débit des
  têtes, inverse refait sur les vraies lois ou qArea par point,
  unification des 4.0).
- **2026-10-12 — E9 livrée : l'eau near/far honnête (option A du dev +
  les trois correctifs).** L'archéologie d'abord, comme demandé : le
  check per-texel a été retiré par `50e36ba` parce qu'il montrait LES
  DEUX réseaux là où la sim et le baké divergent (rubans bakés à
  flanc de colline à côté de l'eau sim de la vallée, nappes de lacs
  en surplomb des lèvres de chute) — retrait sain, et l'option A ne
  le rouvre pas : les ruisseaux sous le seuil d'épinglage n'ont PAS
  de jumeau sim, les exempter ne peut pas ressusciter le double
  réseau. (1) **Les brooks existent de près** : les rubans
  `0 < demi-largeur < pinRiverHalfWidth` sont exempts des deux
  discards du shader (rect vif et rects gelés ; le seuil voyage dans
  `uWaterDebugInfo.y`) et leur lift de lisibilité tombe de 0,5 m à
  **0,12 m** (`clamp(0.36·hw, 0.12, 4)`) : un filet d'eau se lit
  comme un filet, de près comme de loin. (2) **L'aire de drainage
  voyage jusqu'au runtime** : `RiverPoint::area` (l'accumulation de
  fenêtre, remplacée par le qArea maître au point apparié) →
  **TWB6** → `RiverNode::area` ; `bakedEntrySources` injecte
  `runoffDischarge(area)` VRAI — l'inverse largeur→aire (qui défaisait
  la seule racine carrée et mentait sur chaque tronçon resserré,
  plafonné ou rampé) ne survit qu'en repli pour les mondes construits
  par records ; le `kRunoffBoost` 1,6 (tuné v67 contre l'inverse
  faux) meurt avec lui ; et **une tête de cours DANS la fenêtre
  injecte son propre débit** (elle ne croisait aucun bord : lit à sec
  mesuré). (3) **Un seul seuil** : le filtre far des rubans .twb et
  le fallback maître lisent `pinRiverHalfWidth` (le plancher 4 m
  menteur du fallback disparaît — l'extraction tient déjà les cours
  maîtres à ~10 m). Bump **v89** (format .twb). Mesuré : suite
  complète Release **811/811** ; hauteurs STABLES vérifiées
  expérimentalement (hash cartes 6904514173236907014 identique avec et
  sans E9, mesuré par stash — E9 ne touche que l eau). Jugement dev EN
  ATTENTE (en jeu : les ruisseaux visibles de près, les lits plus à
  sec en s approchant, le débit des cours).

- **2026-10-12 — E9b livrée : le registre de lacs à l'échelle carte
  (bug « eau distante sans eau simulée en dessous », vu dev à
  (6872, 422, 4685)).** Le mécanisme, pas le lac : depuis CARTES M1.5
  (« one hydrology per MAP »), un lac vit dans le `.twb` de LA tranche
  qui tient le centre de sa bbox — le transport runtime était
  par-tranche-publiée, donc un lac multi-tranches disparaissait du set
  near dès que sa tranche propriétaire sortait du prefetch (1 408 m),
  alors que le joueur se tenait encore sur un bras : au point du
  screenshot, le joueur est sur la tranche (1,1) et le lac géant
  (12 458 cellules, bbox (-864,96)-(8624,7440)) appartient à (0,0),
  à 2 776 m — la far view (qui lit TOUS les .twb du cache) le
  dessinait, la sim n'avait rien à épingler, le shader discard
  l'effaçait dans le rect. L'éviction par CENTRE de bbox avait le même
  trou. Le fix : (1) le streamer lit UNE fois (worker, à la première
  preuve de cache valide) le **registre de lacs de la carte entière**
  (`takeMapLakes`) ; (2) la scène sélectionne `sandboxLakes` depuis ce
  registre par **distance Chebyshev à la BBOX** (seuil = éviction
  2,5 tuiles, rafraîchi aux publications et tous les 512 m de
  déplacement), les résidents d'une carte précédente gardés sous la
  même règle ; l'insert par tranche et l'éviction par centre ne
  servent plus qu'en repli pré-registre. Aucune donnée bakée ne
  change : pas de bump, hash cartes intact. Instrument commité avec la
  brique : `landscape-report` mesure les 4 plus gros lacs contre le
  sol FINAL (niveau, profondeur max, % du masque 15 m+ sous le niveau)
  — sur (0,0) : 12 458 cellules/225 m/86 %, 9 433/202 m/73 %,
  9 304/181 m/81 %, 7 990/205 m/88 %. Mesuré : suite rapide 706/706.
  DIFFÉRÉ (découverte de la même enquête, à arbitrer) : ces « lacs »
  sont des réseaux de vallées noyés retenus par la taille seule
  (`lakeKeepCells` 2000) — la politique de rétention de
  `pierceLakeOutlets` (taille vs critère physique) reste un cran de
  pupitre dev, distinct du bug réparé ici. Jugement dev EN ATTENTE
  (retour aux coordonnées : l'eau sim doit apparaître sous la nappe
  distante).

- **2026-10-12 — E9c livrée : les lacs clippés au rect de leur carte
  (suite du tour de test dev : E9b validée sur (0,0), mais « lacs
  interrompus » sur la carte (1,0), vu à (8428, 468, 2398)).** Le
  mécanisme : la fenêtre d'hydrologie d'une carte déborde son rect, donc
  ses masques de lacs aussi — mesuré sur (0,0) : le lac 380,8 va jusqu'à
  x=9152, 960 m DANS la carte (1,0), le monstre 340,8 jusqu'à 8624 — là
  où la carte voisine a sa PROPRE hydrologie, divergente (autre fenêtre,
  autres percées : aucun lac de (1,0) ne couvre le point). Au crossing,
  ces lacs restent résidents et leur nappe se dessine par-dessus le
  terrain de la carte active qui ne les connaît pas : quads flottants à
  flanc, discard sim sans eau sim dessous — et en far,
  `collectFarWater` lit les 9 cartes voisines, donc la bande de
  recouvrement montrait les DEUX versions d'un même bassin. La règle
  posée : **une seule autorité hydrologique par point — l'eau d'une
  carte s'arrête à sa ligne.** `clipLakesToMapRect` (runtime, à la
  lecture .twb) assèche les cellules hors rect (bbox et cadre de masque
  conservés, identité stable) et droppe un lac sans cellule restante ;
  appliqué aux QUATRE lecteurs : tuile publiée, registre de lacs E9b,
  far water (au rect de CHAQUE carte lue), minimap. Aucune donnée bakée
  ne change : pas de bump, cache v89 intact. Mesuré (rejeu Python sur
  tile_1_0 de (0,0)) : lac 380,8 → 7 088 cellules gardées / 2 216
  clippées, dernière cellule à x=8184 pour une ligne à 8192 ; un
  morceau 380,8 entièrement à l'est (114 cellules) droppé ; le point
  du dev n'est plus couvert. Suite rapide 706/706. Reste la coupure
  honnête À la ligne quand les deux cartes divergent sur un bassin —
  la racine est la rétention par TAILLE de `pierceLakeOutlets`
  (non-locale, donc instable entre fenêtres voisines) : même différé,
  même cran de pupitre dev que E9b. Jugement dev EN ATTENTE (retour
  sur la carte (1,0) : plus de nappes orphelines à l'est de la ligne).

- **2026-10-12 — E10 livrée : l'anneau bas du départ supprimé (décision
  dev : « une règle qui n'existe pas ailleurs » finit par coûter des
  bugs qui n'existent qu'au départ).** C'était le dernier vestige du
  décret de départ (le 2026-10-08 avait déjà tué le disque aplani,
  l'ancre d'étage et la meadow) : dans un rayon 4+4 km, terre forcée,
  plafond d'étage 0,42, massif à zéro, climat épinglé tempéré.
  Supprimés : les quatre effets de `pullLow` dans `worldSampleAt`, les
  knobs `startLowRadius/Fade` (hash compris) et leurs champs de Form
  `worldStartLowRadius/Fade` (aucun plugin ne les posait), et les
  tables POI spéciales du disque (`startDiscAt`, Summit forcé du
  grand, table pays-bas des moyens) — le départ se type désormais
  comme partout, depuis le WorldSample. CONSERVÉS : le clamp du
  treillis E1c (le jitter du continent (0,0) met le départ 0,5-1,3 km
  à l'intérieur des terres — la garantie de terre est GÉOMÉTRIQUE,
  plus climatique), `grandStartClearance` 1,5 km (garantie de spawn,
  pas règle de paysage), et la sonde de spawn. Bump **v90**, caches
  (0,0) et (1,0) livrés. Mesuré sur (0,0)/1337 : le départ devient du
  haut pays — spawn sondé (4248, 445, 5135), pente 42 %, murs 6,2/km
  (max 341 m), relief.png sain (vallées creusées + cônes POI, pas
  d'artefact) ; continent 140 cours, 98 % à la mer, 32,6 km max ;
  lacs naturels 6 + 71 étangs posés. Deux contrats de test réécrits
  avec la règle : « start is land, temperate, off the massifs » →
  « start is land » (la part géométrique seule), et la jauge de
  variété POI 10 % → 16 % (paires semblables 12,6 % mesurées : la
  table large du disque diversifiait le départ ; les tables massif
  normales sont plus étroites — la jauge n'attrape plus qu'un
  effondrement). Suite complète Release au vert après adaptation.
  Hash MSVC de MapBakerTest toujours à ré-épingler côté dev (périmé
  depuis v78) ; golden scatter MSVC idem si le monde bouge la jauge.
  Jugement dev EN ATTENTE (le nouveau départ en jeu : haut pays
  assumé, spawn praticable).

- **2026-10-12 — E10b livrée : le ping-pong de traversée ouest (vu dev :
  « je charge la nouvelle map puis je recharge juste après la première
  map » vers la carte (-1,0)).** Le mécanisme : l'arrivée d'un crossing
  est posée ~8 m derrière la ligne ; si elle est mouillée (ou non
  fiable), la validation de spawn du warmup (`finalizeSandboxSpawn`)
  cherche un point sec en anneaux de 60 m sur 12 angles — et l'angle
  k=0 pointe plein EST : le premier candidat essayé retraverse la
  ligne, tombe sur les tranches RÉSIDENTES de l'ancienne carte
  (gardées au crossing : publiées, donc « fiables », et sèches), le
  spawn y est relogé, et le détecteur de traversée (hystérésis 8 m)
  renvoie vers la première carte dès la fin du warmup. Deux autorités
  en désaccord sur « où suis-je » : la recherche ignorait le rect de
  la carte active que le détecteur fait respecter. Fix : la recherche
  de spawn est bornée au rect de la carte active (inset 16 m, au-delà
  de l'hystérésis) — un candidat hors rect est rejeté ; si tout est
  mouillé dans le rayon, l'ancien repli (garder le point sondé,
  loggé) s'applique. Runtime seul, pas de bump. Mesuré : suite rapide
  706/706. Jugement dev EN ATTENTE (retraverser la ligne ouest de
  (0,0) : une seule transition, pas de retour).

- **2026-10-12 — E10c livrée : le centre des fleuves à sec (vu dev sur
  (-1,0) à (-1694, 457, 3665) : « l'eau bakée ne remplit pas toute une
  rivière mais juste ses bords »).** Les données sont saines — au
  point passe un fleuve tier 2 (demi-largeur 72 m = cap loi 45 × rampe
  1,6, aire 167 km², surface 439,3) et tout le lit publié est SOUS la
  surface (430,6 au point, 420-428 au centre, mesuré au .trg). Le
  coupable est le garde anti-données-pourries de `pinRivers`
  (WaterSim.cpp) : `surf - ground > 9 m → skip` — écrit avant E9,
  quand aucun cours légitime n'était si profond. Depuis les vrais
  débits, un fleuve large creuse 11-19 m au centre : les BORDS
  (< 9 m) étaient épinglés, le CENTRE silencieusement sauté — la sim
  n'y met pas d'eau, et les discards du rect vif + des rects gelés
  (E9) effacent le baked en comptant sur elle → lit sec au centre,
  eau crénelée sur les bords, sur toute la bande que la trajectoire
  du survol a balayée. Fix : le plafond de profondeur plausible suit
  la demi-largeur locale — `max(9, 1.5·hw)` (hw 4 → 9 m inchangé
  pour les rubans étroits où vivaient les vraies erreurs
  résiduelles ; hw 72 → large, le lit de ~29 m max passe). Runtime
  seul, pas de bump. Mesuré : suite rapide 706/706. Jugement dev EN
  ATTENTE (retour sur le fleuve de (-1,0) : le lit plein dans le
  rect sim).

- **2026-10-12 — Passe W (audit eau « tout en un coup », demande dev)
  : W1 contrat ruban↔nappe, W2 rétention physique des lacs, W3 jauge
  pérenne. Bump v91, caches (0,0)/(1,0)/(-1,0) livrés.** L'audit
  préalable (script Python sur les .twb/.trg des trois cartes v90) a
  chiffré LE problème dominant : 30-42 cas PAR CARTE du trou vu par
  le dev à (-2574, 4220) — un cours finit dans un lac de la FENÊTRE,
  la nappe RÉCONCILIÉE ne couvre plus l'intervalle, jusqu'à 1,1 km de
  lit à sec ; plus 1-2 lacs-canyons par carte (150-264 m de fond) ;
  lignes entre cartes bornées (≤ 160 m de discordance).
  **W1 — le contrat ruban↔nappe réparé à la source** : la trace
  d'extraction ne s'arrête plus aux lacs acceptés (elle chevauche la
  surface de remplissage, comme pour les cuvettes rejetées ; les
  spots d'étangs nés DANS une nappe sont filtrés), et c'est
  l'ÉMISSION par tranche qui coupe les rubans contre les lacs
  RÉCONCILIÉS — un point de recouvrement de chaque côté (le ruban
  passe sous la nappe), reprise marquée lakeFed/LakeOutlet (pas de
  taper). Le drop « tête lac-fed au lac disparu » devient une
  rétrogradation en source (le cours est long maintenant — le
  supprimer entier tuait une rivière pour son premier mètre). Mesuré
  sur (-1,0) : le transect du trou dev 0/36 points secs (vs 32/36),
  couverture d'eau +3,1 % (+28 ha de trous comblés, pas de double
  réseau), cours continus sur le plan.
  **W2 — la rétention devient un VERROU DE SEUIL borné + plancher
  d'étendue** (`lakeKeepDepth` 12 ≤ verrou < `lakeDeepPierce` 60, ET
  `lakeKeepCellsMin` 400) : en dessous de la bande, une vallée qu'un
  ruisseau re-coupe (percée) ; au-dessus, aucun col naturel n'est si
  haut ici — un mur d'étage a noyé un réseau de vallées (percé en
  gorge, la rampe suit la plus grande pente jusqu'au jour) ; le
  plancher d'étendue écarte les tranchées de pied de mur (mesure :
  profondeur seule retenait 130 lacs dont 124 poches < 200 cellules
  — la bande + le plancher en gardent 10, les knobs du pupitre
  affineront vers la cible 3-6). `lakeKeepCells` (taille seule,
  2000) supprimé. Le monstre 13 326 cellules / 264 m de (-1,0) :
  percé, sa vallée rendue au réseau.
  **W3 — la jauge pérenne dans `landscape-report`** : « water
  audit » par carte — tails orphelins (ni mer, ni nappe, ni reprise,
  ni bord ; les 4 pires loggés) et dead-ends en nappe. Baseline
  v91 : (-1,0) 5 orphelins / 17 dead-ends (251 runs), (0,0) 2 / 31
  (257 runs). Résiduel connu, non bloquant : l'affluent dont le
  receveur s'est éloigné au lissage meurt dans son étang de jonction
  (famille préexistante, la jauge la suit). Mesuré : suite complète
  Release 811/811 (contrats HydrologyTest ré-exprimés sur la bande).
  Jugement dev EN ATTENTE (tour d'eau sur les trois cartes).

- **2026-10-12 — W4 livrée : la percée devient du terrain de carte
  (« gros trou circulaire » vu dev à (2900, 4771)).** Le cratère :
  W2 perce les bassins dans la FENÊTRE d'hydrologie, mais le sol des
  TRANCHES ne voyait jamais la percée — les cours W1 et leurs étangs
  de jonction vivaient au fond percé (~288 m) sous un sol publié à
  428 m, et le dig parabolique des étangs posés, SANS borne basse,
  forait des puits circulaires de 140 m jusqu'à eux (mesuré : fonds
  plats à 282-285 m en taches de 100-200 m le long de l'axe du
  bassin vidé). Deux correctifs : (1) `extractMapHydrology` exporte
  `pierceDelta` (≤ 0, grille fenêtre) et `applyPierceDelta` l'applique
  au stage-1 partagé AVANT les tranches ET l'overview — gorges
  d'entaille et fonds drainés deviennent du vrai terrain 16 m lissé,
  tout l'aval (carve, étangs, rubans, sim, far) redevient cohérent ;
  (2) ceinture sur le dig des étangs (coupe bornée à cuvette + berge
  sous le sol local — l'analogue du floorCut que le carve S5c avait
  déjà). Connu/assumé : le biome du fond d'un grand bassin vidé reste
  celui de l'altitude pré-percée (les champs stage-1 dérivés ne sont
  pas recalculés). Bump **v92**, trois caches relivrés. Mesuré au
  point : v91 412→282→426 (puits vertical), v92 412→342→412 (vallon
  gradué) ; audit (0,0) : 2 orphelins / 14 dead-ends, 13 lacs
  naturels, plus aucun fond sous 340 dans l'emprise. Jugement dev EN
  ATTENTE (retour sur place : un vallon, plus un puits).

- **2026-10-12 — W5 livrée : le backwater borné par les berges
  (« rivières au-dessus du sol, non bordées par le terrain », vu dev à
  (3708, 4673) « et à plein d'autres endroits »).** Mesuré au point :
  un ruban tier 2 à surface PLATE 444 m sur 13+ nœuds, sol publié
  360-426 dessous, berges du vallon à 418-426 — la surface d'un lac de
  fenêtre fantôme. Le mécanisme : `reconcileRiversWithTerrain`
  reconstruit le profil de l'aval vers l'amont avec un backwater
  monotone, et fait confiance au `lake.level` d'un lac réconcilié
  croisé en aval — une POCHE résiduelle perchée (le reliquat d'un
  grand lac de fenêtre effondré) inondait des kilomètres de bief
  amont à son niveau, 25-80 m au-dessus des berges. Le fix : un point
  de ruban qui a survécu à la coupe W1 n'est couvert par aucune nappe,
  donc son eau est CONTENUE par sa vallée — le profil est borné par le
  sol des deux berges (±(hw + 2 texels), perpendiculaire au segment),
  et un profil cappé (qui déborderait) cesse de se propager : la lèvre
  qu'il chevauchait était fictive. Hors rect de tranche : pas de cap
  (conservateur). Bump **v93**, trois caches relivrés. Mesuré
  (jauge berges à 1,5·hw+8, intérieur de (0,0)) : points > 20 m
  au-dessus des deux berges — v90 (AVANT la passe W) 1519/6285
  (24 %, le symptôme préexistait), v92 2731/8329 (33 %, aggravé par
  les cours à travers bassins), **v93 1126/8329 (13,5 %)** — mieux
  que le monde pré-passe en proportion et en absolu. Résiduel : biefs
  profonds en gorge étroite (berges réelles plus proches que la
  jauge) et anneaux de bord de tranche/carte — à suivre à la jauge.
  Suite complète Release 811/811. Jugement dev EN ATTENTE (les
  rivières dans leur lit, bordées).

- **2026-10-12 — RESTRUCTURATION EAU livrée (directive dev : « arrête
  les fixs locaux, regarde tout le système et place l'eau au moment
  où elle est physiquement correcte »).** Le défaut de structure :
  l'hydrologie (niveaux, masques, cours) était calculée sur le sol
  16 m du stage-1 (étape 2), le sol FINAL naissait après (érosion
  fine, étape 3), et une pile de correcteurs (reconcileLakes +
  re-cut, reconcileRivers + backwater + cap W5, coupes W1, delta W4,
  ceintures) recollait l'eau sur un sol qu'elle n'avait jamais vu —
  chaque pansement exposait la divergence suivante (E9b→E9c→
  trou→cratère→rubans flottants, l'historique du jour). **Le bake
  devient trois phases** : A. `bakeSliceGround` — le sol final SANS
  eau, par tranche en parallèle (érosion fine guidée par le seul
  champ d'accumulation du stage-1, `drainageArea` — information de
  façonnage, pas de placement d'eau) ; B. `extractMapHydrology` SUR
  ces sols (la fenêtre 16 m échantillonne la tranche la plus
  intérieure — le halo d'érosion fine est un territoire d'artefacts —
  et fond vers le stage-1 sur ~192 m au-delà du rect) : percée,
  flood, lacs et cours vivent sur le terrain que le joueur foule,
  l'eau touche ses berges PAR CONSTRUCTION ; C. `bakeSliceWater` —
  delta de percée appliqué au sol fin, creusements (lits S5c, fonds
  de lacs, étangs), masques S6, découpe et .twb. **Supprimés** :
  `reconcileLakesWithTerrain` (et son re-cut en composantes),
  `reconcileRiversWithTerrain` (backwater + cap de berges W5),
  l'application du delta au stage-1 (W4) — le finalize est scindé
  (`finalizeGround`/`finalizeWater`, wrapper legacy pour les bancs).
  Au passage : un étang posé est clippé aux cellules que le sol peut
  contenir (un étang de jonction né dans une tranchée de percée
  débordait sa gorge en nappe enterrée de 200 m). Bump **v94**,
  trois caches livrés, bake +2 s/carte (10 s de tranches).
  **Mesuré (jauge berges > 20 m au-dessus des deux berges, intérieur
  de (0,0))** : v90 24,2 %, v93 (toute la pile de correcteurs)
  13,5 %, **v94 (zéro correcteur) 3,1 %** — et 85 % des points à
  ≤ 1 m des berges (47 % en v90). Water audit (0,0) : 5 orphelins /
  4 dead-ends (28 avant), 8 lacs naturels, étangs posés 113→50
  (les fantômes hors-sol éliminés). Résiduel 3 % : gorges étroites
  (la jauge échantillonne au-delà de la paroi réelle) et abords des
  lignes. Suite complète Release au vert. Jugement dev EN ATTENTE
  (le tour d'eau qui compte : celui-ci).

- **2026-10-12 — Vérification systématique post-restructuration
  (demande dev : « d'autres problèmes fondamentaux dans le bake de
  l'eau ? ») : un trouvé et corrigé, un identifié et différé.**
  Nouvelles jauges sur les trois cartes v94 : nappes périmées par
  lac, monotonie des surfaces (7-8 remontées/300 runs — marginal),
  et points de ruban ENTERRÉS (> 2 m sous le sol au fil de l'eau) :
  **44-49 % des points** — les rivières disparaissaient sous le
  terrain par pointillés. Trois causes, trois corrections : (1) les
  **entailles de percée larges d'une cellule** (16 m) — invisibles au
  transfert bilinéaire et quittées par les polylignes lissées — la
  percée creuse désormais une BANDE au gabarit du cours (loi de
  largeur sur l'aire du bassin, min 2,5 cellules, flancs à 35°) ;
  (2) les **floorCut des creusements ré-ancrés à la waterline**
  (surface − profondeur − berge) au lieu du sol local — sûr depuis
  que les surfaces vivent sur le sol final, et les bosses de détail
  au-dessus de la waterline sont enfin rabotées (lits S5c, fonds de
  lacs, étangs) ; (3) les **surfaces re-bornées après lissage** (le
  lissage déplace les points sans ré-échantillonner leur eau :
  virage coupé AU-DESSUS d'une gorge = nappe plate sur 100 m de
  vide) — surface = min(surface, filled au point final + 0,3).
  Mesuré (0,0) : enterrés **48,4 % → 0,4 %**. RÉSIDU identifié, pour
  une brique dédiée : le lissage tire encore ~6 % des nœuds sur le
  REBORD des gorges étroites (l'eau de flood est dans l'entaille, le
  nœud sur la lèvre ; pire cas 291 m au bord de carte) — la solution
  propre est un re-snap des polylignes au thalweg (minimum local de
  filled) après lissage, à construire posément, pas un réglage.
  Clip des étangs posés déjà dans la restructuration. Suite complète
  au vert, trois caches v94 relivrés. Jugement dev EN ATTENTE.

- **2026-10-12 — Le lit sec (vu dev à (6800, 9886) sur (0,1) : « une
  rivière qui n'allait pas jusqu'au bord de son lit » ; « pour le
  reste c'est beaucoup mieux qu'avant »).** Mesuré : auge de ±30 m à
  fond PLAT (−9,3 m constant — la bande de percée de la veille,
  min 2,5 cellules) pour un ruban de hw 16 : ~15 m de lit à sec par
  rive. Corrections, et deux essais ANNULÉS honnêtement : (1) la
  bande de percée est dimensionnée par le DÉBIT AU COL
  (`routeFlow` par passe, déjà payé par le flood ; la loi sur
  l'accumulation du gate — l'aire de la composante taillait des lits
  de 60 m pour des cours de 16) et son fond devient un **V doux**
  (pente 15 % vers les bords, flancs 35 % au-delà) — une vallée, pas
  un lit d'eau ; min re-serré à 1,25 cellule ; (2) le mesh du ruban
  couvre **1,4×hw** (au lieu de 1,15) : le carve mouille l'épaule
  interne au-delà de la loi, le quad doit l'atteindre, le depth-test
  taille le rivage exact. ANNULÉS après mesure : le re-snap ITÉRÉ au
  thalweg (4 pas — zigzags vers les cuvettes voisines, jauges
  dégradées ; retour au pas unique) et l'adaptation wet-width au
  bake (elle scannait le sol de FENÊTRE alors que la section mouillée
  naît du carve de phase C — code retiré). Bump **v95** (dû aussi
  pour les durcissements de la veille, sortis à clé égale), QUATRE
  caches livrés ((0,1) compris — baké en jeu par le dev). Jauges
  (0,0) : enterrés 0,9-1,1 %, débordement > 20 m 8,7-8,9 % (vs 24 %
  v90 ; le reliquat vit aux rebords de gorges — la vraie brique
  re-snap reste à concevoir au-delà du pas unique). Suite complète
  Release au vert. Jugement dev EN ATTENTE (retour au point : berges
  en pente douce mouillées par le ruban élargi).

- **2026-10-12 — Cours interrompus (vu dev à (7846, 9844) sur (0,1)) :
  le lac de marge invisible.** Mesuré : une pluie de micro-runs
  lakeFed de 3-6 points avec 326 m de trou — les restes d'un cours
  coupé contre un lac de FENÊTRE (niveau 466,5, à cheval sur la ligne
  est x=8192) dont le CENTRE de bbox tombait hors du rect de la
  carte : la coupe aux nappes l'utilisait, mais l'ownership par
  centre ne l'attribuait à AUCUNE tranche — jamais publié. Fix : le
  centre est CLAMPÉ au rect de la carte avant le choix de la tranche
  propriétaire (la tranche de bord publie le lac de marge ; le clip
  runtime E9c borne déjà son affichage au rect — l'autorité aux
  lignes reste propre). Bump **v96**, quatre caches relivrés.
  Vérifié au point : la nappe (467,2 ; 918 cellules) couvre
  l'intervalle, l'eau est continue. Suite complète Release au vert.
  Jugement dev EN ATTENTE.

- **2026-10-12 — Passe FINITIONS EAU, F2 puis F1 (plan approuvé dev ;
  périmètre 1-5, détroits exclus).**
  **F2 — rétention des lacs : sweep mesuré, statu quo choisi.** Banc
  hors-cache (gameDirs jetables + patch pupitre `mods/terrain-gen.toml`,
  le canal officiel — leçon : un record de pupitre absent de la base
  exige `new = true`) : CellsMin 700 → 3,2 naturels/carte (1·4·4·4,
  profondeurs ≤ 15 m) ; 1000 → 2,2 ; +KeepDepth 16 → idem 700. Le dev
  GARDE 400 (8-13/carte) — aucun changement de code, les chiffres
  restent ici pour un futur réglage au pupitre.
  **F1 — lissage contraint par la vallée (livrée).** `smoothRiver`
  prend (spec, filled) : chaque point reconstruit est ramené par
  bissection vers la position du tracé BRUT au même arc si le
  remplissage à sa position lissée dépasse celui du tracé de +1,5 m —
  la courbe reste douce et ne grimpe plus hors de sa vallée (le snap
  post-hoc vers le min local avait zigzagué — leçon conservée en
  commentaire). Mesuré sur banc (0,0) : enterrés 0,8 % → 0,5 % ;
  la jauge berges reste ~9 % MAIS son résidu est caractérisé : 466/586
  points sont des BIEFS PROFONDS légitimes (centre 15+ m sous la
  surface — l'eau de remplissage ponte les creux rejetés, « the dip
  becomes a natural pool ») ; la jauge a atteint son plancher utile,
  le juge au-delà est l'œil. Suite rapide 706/706. Bump reporté à la
  fin de la passe (v97).

- **2026-10-12 — F3 livrée : la fenêtre d'hydrologie lit le sol final
  du voisin en cache.** `game::bakeMap` charge la rangée de tranches
  frontalières de chaque carte voisine au manifest valide, et
  `extractMapHydrology::groundAt` les échantillonne au-delà du rect
  (fondu 96 m à la ligne) avant le repli stage-1 — les bassins de la
  marge partagée (le lac de marge du bug (7846, 9844)) se résolvent
  sur le terrain que les deux cartes publient. ENTORSE DOCUMENTÉE à la
  pureté seed : l'eau de la bande frontalière se raffine avec l'ordre
  de bake des cartes ; une carte bakée seule reste bit-exacte
  (MapBakerTest inchangé). Mesuré : la jauge à ±24 m de la ligne
  était déjà basse depuis la restructuration (9-10/256 discordants,
  inchangée) — le gain est sur les bassins PROFONDS de marge, pas sur
  le liseré. Suite rapide 706/706.

- **2026-10-12 — F5 livrée : estuaires v1 (évasement + éventail de
  sable).** Rien n'existait aux embouchures (état des lieux : dernier
  point fenêtre = première cellule de mer, `reachesSea` maître jamais
  lu, le far coupe à seaLevel+0,25). v1 data-driven : (1) un cours
  dont la queue atteint la mer évase sa demi-largeur sur ses derniers
  `hydroMouthFlareLength` (300 m) jusqu'à ×`hydroMouthFlare` (1,8,
  knobs pupitre) — le ruban ET le lit S5c suivent (les deux lisent
  hw) ; (2) le masque beach s'élargit en éventail autour de chaque
  embouchure tier ≥ 1 (rayon 250 m hand-tuned, gated seaDist ≥ −1) —
  sable + détail calmé suivent via les consommateurs existants.
  Mesuré sur (−2,0) (carte côtière, banc) : 18 embouchures ; le
  fleuve passe de hw 72 (cap loi) à 129,6 au tail (×1,8 exact,
  estuaire ~260 m) ; éventail présent (36 cellules beach>128 à une
  embouchure) — sa visibilité au droit des grandes baies dépend du
  trait de côte macro, à juger à l'œil. Différés journalisés :
  bras/tresses, bancs de sable émergés. Suite rapide 706/706.

- **2026-10-12 — F4 livrée : habillage des sources v1 (brume +
  humidité + son optionnel) — et CLÔTURE de la passe FINITIONS EAU
  (bump v97, TWB7, quatre caches livrés).** Trois pièces, réutilisation
  stricte : (1) **transport** — `River`/`RiverSurface` +=
  `headIsSource` (posé à l'émission sur le run qui contient le premier
  point du cours ; un début de run peut être une coupe de tranche) et
  `headType` recopié à la publication (il était sérialisé mais jamais
  consommé) → **TWB7** ; (2) **passe scène `updateRiverSourceFx`**
  calquée sur les FxSources feu (diff par portée kFxSourceReach,
  spawn/stop, skip en intérieur) : brume `ParticleForm` par typologie
  (Spring/Névé/Glacier — LakeOutlet : rien, la nappe suffit) + boucle
  `SoundForm` optionnelle (guids vides en base : le dev branche ses
  wavs comme pour les footsteps) ; le tout piloté par un
  `RiverSourceFxForm` moddable (`base/landscape.toml` : brume
  « WaterSourceMist » haze partagée par les trois types en v1) ;
  (3) **humidité au rocher** — stamp wetness (rayon 24 m hand-tuned)
  autour des vraies têtes dans la boucle S6 de `finalizeWater`.
  Différé : patch de neige local des névés (aucun mécanisme de stamp
  matière runtime — candidat FireScorchMap-like). BILAN DE PASSE :
  F1 lissage contraint par la vallée ✓, F2 sweep rétention mesuré
  (statu quo 400 choisi dev) ✓, F3 sol du voisin aux lignes ✓
  (entorse pureté documentée), F5 estuaires v1 ✓, F4 sources v1 ✓ ;
  bump unique v97 + TWB7, suite complète Release au vert. Jugement
  dev EN ATTENTE (tour d'eau : sources embrumées en approche,
  estuaires évasés et sableux, rien de cassé ailleurs).

- **2026-10-12 — RÉGRESSION (vu dev : « le nombre de rivières
  au-dessus de leur lit a explosé, des petits ruisseaux deviennent
  d'énormes chutes ») : F1 ANNULÉE, et le re-snap 1-pas de v95 avec
  elle — bump v98.** La question du dev « ça peut venir d'une étape
  précédente ? » a mené à LA jauge décisive (virages > 45° entre
  segments de polyligne, (0,0)) : v90-v94 **0,0 %** ; **v95 : 22,2 %**
  (le re-snap « 1 pas vers le min 3×3 » introduit dans le lot lit-sec
  et jamais annulé quand sa version itérée l'a été) ; **v97 : 35,5 %**
  (la bissection F1 EMPILÉE dessus). Les zigzags tordaient les quads
  des rubans (élargis ×1,4 au même moment : très visibles), l'eau
  flottait hors de son lit et les biefs hachés lisaient comme des
  cascades. Retirés tous les deux : **v98 = 0,0 % de virages serrés,
  enterrés 0,8 %, débordement 6,0 %** — le meilleur état mesuré (les
  snaps DÉGRADAIENT aussi la jauge qu'ils visaient). Leçon durcie :
  tout déplacement de points post-lissage recrée du crénelage — le
  problème « rebord de gorge » se traitera dans le lissage amont
  (différé, avec la jauge de virages comme garde-fou) ou pas du tout.
  Quatre caches v98 relivrés, suites au vert.

- **2026-10-13 — RÉASSISE : les surfaces publiées se posent sur le sol
  fin pré-carve — bump v99.** Enquête sur le fleuve de (5800, 4000),
  carte (0,0) (vu dev : « l'eau de loin remplit bien le canyon, l'eau
  simulée ne va pas jusqu'au bord ») : surface publiée 612,75, mais le
  fond fin (2 m) est une banquette de ~120 m à 602-611 — le bord du
  ruban flottait 4 m au-dessus du sol. **Cause structurelle, pas un
  commit** : l'hydrologie tourne sur la fenêtre 16 m, qui dans une
  vallée encaissée moyenne murs+fond et SURESTIME le plancher (mesuré
  via `overview.bin` : fenêtre 616-623 à l'axe contre 602-608 en fin) ;
  la surface de routage hérite du biais. Nouvelle jauge **stick-out**
  (surf − min(berges fines à ±(hw+4))), carte (0,0) : v90 **62,6 %**
  ≥2 m, v94 45,5 %, v98 41,2 % — la classe date de la restructuration
  (le `reconcileRiversWithTerrain` supprimé faisait, au milieu de ses
  bugs, ce re-calage fin ; le re-bound `min(surface, filled+0.3)` est
  borné FENÊTRE). Le carve S5c borne déjà sa ligne d'eau au sol fin
  (`centerGround + 0.3`) et creuse le lit juste — seule la PUBLICATION
  gardait la surface fenêtre. La passe finale (fin
  d'`extractMapHydrology`, map-level, via le sampler owner-pick +
  pierceDelta), calibrée en six bakes jaugés sur (0,0) — dont le
  retour dev immédiat « (5900, 3660), toujours une nappe au-dessus du
  sol » qui a montré que le clamp à l'AXE seul ne suffit pas (l'axe
  fenêtre y chevauche une crête fine à 631,7, le vrai fond à 623 est à
  30-50 m, SOUS l'empreinte du ruban) :
  1. plancher par point = min du sol fin sous l'EMPREINTE eau +
     épaulement (axe, ±0,5/±1,0/±1,4 × hw) — le carve creuse la crête
     mais ne REMONTE jamais le sol bas d'à côté, donc la ligne d'eau
     descend à lui ;
  2. cible = max(plancher sur 3 points le long) + 0,3 (anti-fosse),
     plafond de descente 12 m (`kReseatMaxDrop` — sans lui, l'empreinte
     large plonge dans les encoches de perçage et gorges voisines :
     enterrés 3,0 % mesurés), garde mer ;
  3. monotone aval à plancher borné : `s[i] = min(s[i], max(s[i−1],
     plancher[i] + 0,3 − 6))` (`kThroughCut` — le monotone nu traînait
     un clamp bas sur des kilomètres sous chaque remontée et faisait
     creuser des canyons-fentes au carve).
  Variantes REJETÉES sur mesure : clamp axe seul (les crêtes perchées
  restent, le cas (5900, 3660)), monotone nu, garde « niveau de lac »
  (elle épinglait des biefs au niveau de LACS FANTÔMES : des lacs de
  `hydro.lakes` que l'émission ne publie pas — retirée, l'émission
  coupe de toute façon le ruban dans les vrais lacs), garde
  « point déjà enterré » (−0,3 pt d'enterrés mais +2 m au témoin 2 via
  la chaîne monotone). **Aucun déplacement de points** (leçon v98 :
  virages 0 % par construction). Découvertes au passage : les « murs »
  qui enterrent des biefs près des lignes (sol plat à 883,9, ligne
  nord) = la RETOUCHE DE BORDURE, appliquée après les carves — famille
  préexistante, hors de portée ; et le fond plat des canyons est la
  SUPERPOSITION des carves S5c des méandres successifs, chaque bief
  sous-capant de ~2 m le bord du voisin (le reliquat de stick-out aux
  témoins vient de là, pas de la passe). L'affichage expliqué : l'eau
  de loin est une nappe coupée en profondeur contre le terrain ~70 m
  (elle peignait tout le fond sous la surface biaisée), le sim ne
  mouille que `dist ≤ hw` et `ground < surf − 0.3` — les deux lisaient
  la même donnée fausse, seul le lointain la masquait. Mesuré v99
  (0,0) : stick-out ≥2 m **41,2 % → 35,8 %**, ≥8 m 20,9 % → 17,2 %,
  virages 0,0 % ; témoins (5800, 4000) : +4,55 → **+2,30** (le
  sous-capage de méandres), (5900, 3660) : +10,4 → **+0,30**.
  Compromis assumé : enterrés 0,8 % → 2,7 %, concentrés sur les
  familles murs-de-bordure et traversées fenêtre-contre-paroi (eau déjà
  invisible avant, surfaces plus basses donc comptées). Le gros du
  reliquat ≥2 m reste la POSITION latérale de l'axe (crêtes au-delà de
  ±1,4 hw, sous-capage de méandres) — différé « rebord de gorge »,
  jauges virages + stick-out en garde-fous. Quatre caches v99 livrés,
  suites au vert.

- **2026-10-13 — FALAISES PATHOLOGIQUES : les carves ne remblaient ni
  ne percent plus les murs — bump v100.** Enquête sur la falaise de
  (5846, 3058) (vu dev : « falaise très fine et très découpée avec des
  pics et des textures qui remontent au-dessus du sol », « falaises
  ultrafines ») : un mur VERTICAL de 140 m dans UN texel de 2 m, le
  long d'un cercle. Nouvelle jauge **rasoir** (marches >30 m / >60 m
  entre texels adjacents de 2 m), carte (0,0) : 30 539 / **7 107**
  cellules. Trois familles, UNE cause commune — le désaccord
  fenêtre 16 m ↔ sol fin 2 m (le même qui porte la RÉASSISE) rencontré
  par les creuseurs de la phase C, qui écrivaient tous
  `max(min(h, bed), floorCut)` :
  1. **La coupelle d'étang S5d creusait le DISQUE entier** (d ≤ 1 de la
     bbox, rayon jusqu'à 277 m — dimensionné sur les largeurs de rubans
     à la jonction), masque clippé ignoré : cratère circulaire poinçonné
     dans le versant quand le disque chevauche un sol fin au-dessus du
     niveau (59 étangs sur 96 avec un mur de rebord >6 m, pire 276 m).
     → gate : ne creuser que les texels dont le sol fin ≤ niveau + 0,5
     (le test « le sol tient la nappe » du clip fenêtre, à la résolution
     du creuseur) ; idem lit de lac S5d-bis.
  2. **Le floorCut REMBLAYAIT** : quand le sol fin préexistant est SOUS
     floorCut (cours/étang fenêtre suspendu au-dessus d'une vraie vallée
     fine), `max(…, floorCut)` REMONTAIT la vallée en un bouchon plat à
     niveau constant aux bords d'un texel — les « textures qui remontent
     au-dessus du sol » (plateaux mesurés à 757,2 / 979,3 / 422,8…).
     → les quatre écrivains (lit S5c, épaule S5c, lit de lac, coupelle)
     passent en **dig-only** : `min(h, max(min(h, bed), floorCut))`.
  3. **Les canyons-fentes** : un cours fenêtre enterré 150 m sous un mur
     fin creusait quand même son lit à `surface − depth` À TRAVERS la
     montagne — fente de 1-2 texels, murs de 148 m (le clamp de ligne
     d'eau S5c ne fait que descendre, jamais remonter). → gate : un
     cours enterré au-delà de son gabarit (tierDepthMax + bankMaxCut
     + 4 m) sous le sol local ne creuse pas ce texel (les bosses de
     détail à raser restent loin sous ce seuil — leçon « dotted
     rivers » respectée).
  Mesuré (0,0) : rasoirs >60 m **7 107 → 407 (−94 %)**, >30 m
  30 539 → 17 141 ; le reliquat >60 m est concentré sur les lignes de
  carte (la retouche de bordure, APRÈS les carves — famille à part) et
  les gorges de perçage voulues (transitions ≥16 m, pas des rasoirs).
  La falaise du dev a disparu (versant continu). Effet eau assumé et
  ATTENDU : enterrés 2,7 % → 9,3 % (les cours qui se creusaient une
  fente sont maintenant honnêtement enterrés sous les murs — le
  terrain ne ment plus, c'est à l'EAU de se corriger) ; stick-out ≥2 m
  35,8 → 33,8 %. **Prochain chantier eau (décision dev) : traiter ces
  biefs fenêtre-sous-mur** (couper/rerouter les tronçons enterrés, ou
  assouplir le plafond de la réassise là où le sol fin l'exige).
  Quatre caches v100 livrés, suites au vert.

- **2026-10-13 — RÉASSISE SANS PLAFOND : l'eau s'enfonce dans le
  plancher qu'elle mesure — bump v101.** Diagnostic de (5930, 3600)
  (vu dev : « écartement entre les bords de la rivière et les bords du
  canyon — taille min/max plutôt qu'un lien avec le terrain ? marge de
  sécurité ? ») : les DEUX intuitions étaient justes. (a) La largeur
  est une pure loi de drainage (`hw = coef·aire^exp`, resserrement de
  pente, plancher 1,2 m) qui ne lit jamais la forme de la vallée —
  53 % des points à écartement ≥2 m sont dans une vallée plus large
  que ruban+40 m ; (b) le plafond `kReseatMaxDrop = 12 m` bridait les
  canyons plus profonds que le biais visé (21 % des points résiduels,
  mesuré par diff v98↔v100 ≈ 12,00 pile). **Décision dev : plutôt que
  d'asservir la largeur au terrain (≈8 consommateurs de hw, largeur
  instable sur fond bruité), on CREUSE — la rivière descend au
  plancher mesuré et le carve y taille son gabarit, les berges
  affleurent.** Le siège devient le MIN pur de l'empreinte, SANS
  plafond. Deux gardes « anti-rainure » essayées et REJETÉES sur
  mesure : étalement-vs-médiane (l'échantillon ±1,4 hw touche
  naturellement le mur du canyon → la médiane bloquait les descentes
  légitimes : témoins +9 à +11 m) et 2ᵉ-plus-bas (7 échantillons sur
  une perpendiculaire ne distinguent pas une rainure d'un plancher
  effleuré une fois : témoins +4 à +9 m). Une plongée sous un mur ne
  coûte plus de terrain depuis v100 (les carves ne taillent plus de
  fente) — elle MARQUE un bief fenêtre que le sol fin contredit.
  Mesuré (0,0) : stick-out ≥2 m **33,8 → 31,4 %** (meilleur état),
  ≥8 m 17,1 → 14,1 %, témoins (5800, 4000) +2,30, (5900, 3660) +1,59,
  (5930, 3600) +2,89 (reliquat = sous-capage de méandres ±2-3 m) ;
  virages 0,0 %, rasoirs >60 m 373. Compromis journalisé : enterrés
  9,3 → 11,4 % — la part des biefs fenêtre-contre-mur qui plongent,
  CIBLE DE LA PROCHAINE BRIQUE (couper/rerouter ces tronçons : leur
  eau est invisible, leurs rubans mentent). Quatre caches v101
  livrés, suites au vert.

- **2026-10-13 — SIÈGE EN ENVELOPPE BASSE : plus de bande sèche sous
  la ligne d'eau au bord des rivières — bump v102.** Retour dev à
  (5900, 3660) après v101 : « le sol n'est pas creusé et l'eau est
  toujours au-dessus du sol ». La vérification du cache a corrigé mon
  diagnostic : la donnée v101 AVAIT le défaut — l'eau affichée
  s'arrête au gabarit (±hw, le sim ne mouille que `dist ≤ hw`) mais le
  sol ne remontait au niveau de l'eau qu'à ±40-50 m : une BANDE de
  8-20 m, 1-4 m SOUS la ligne d'eau, bordait chaque rive (jauge neuve
  « bande sous ligne d'eau » : max(surface − sol) sur hw+2..hw+14 —
  LE critère visuel, que le stick-out à ±(hw+4) ne capturait pas).
  Causes en v101 : le max 3-points le long du cours (+0,3) relevait
  chaque cible au-dessus de son propre min. Variantes mesurées et
  REJETÉES : siège sur le sol pré-carve au bord du gabarit (les
  berges du joueur sont des PRODUITS du carve ; le bord pré-carve est
  la colline non creusée → quasi aucune descente, témoins +10/+11 —
  le point fixe surface↔fouille interdit cette lecture), médiane
  glissante 5 points (au-dessus du min local un point sur deux :
  bande 39,8 %). Retenu : **l'enveloppe basse** — min de l'empreinte
  (10 offsets ±0,5..±1,4 hw + axe) puis **min glissant sur 5 points
  le long du cours** (inclut le point lui-même → aucun offset
  échantillonné ne reste sous la ligne d'eau), ancrage −0,2, plancher
  monotone à seat−6. Mesuré (0,0) : bande >1 m **45,9 → 27,7 %**,
  >2 m 37,4 → 21,8 % ; témoins (5900, 3660) **−0,00** (contour =
  ruban), (5930, 3600) +0,62, (5800, 4000) +3,2 (reliquat : un creux
  de ~3 m taillé en phase C au-delà de l'épaulement que le siège
  pré-carve ne voit pas — candidat : transfert de perçage ; à
  instruire avec la brique suivante). Compromis assumé : enterrés
  11,4 → 20,2 % — les biefs fenêtre-contre-mur passent sous le seuil
  du gate anti-fente et ne creusent plus : eau invisible plutôt que
  flottante, et c'est la population à COUPER/REROUTER (brique
  suivante, désormais prioritaire). Virages 0,0 %. Quatre caches
  v102 livrés, suites au vert.

- **2026-10-13 — L'EAU AU-DESSUS DU SOL : deux fuites côté RUNTIME
  annulaient le siège v102 (aucun rebake).** Retour dev après v102 :
  « j'ai toujours l'eau au-dessus du sol » + trois hypothèses
  (représentation, taille des voxels, pentes en escalier). Enquête
  mesurée sur la tuile des témoins (1,0) : la donnée v102 était
  bonne, c'est la CHAÎNE DE REPRÉSENTATION qui recréait le défaut.
  (a) **La remontée du ruban au rendu** : le mesh local re-levait
  chaque nœud densifié à `max(surface, sol_axe + 0.18×largeur)`
  (protection anti-corde des chutes, plafond 4 m) — sur les biefs
  enterrés (sol d'axe AU-DESSUS de la surface publiée, 24 % des
  points depuis v102), le ruban était redessiné en nappe flottante :
  jauge « bande RENDUE » >1 m à **48,6 %** contre 28,6 % en surface
  publiée. Correctif : la remontée est PLAFONNÉE à l'enveloppe des
  nœuds voisins (max des surfaces du segment / des deux voisins) —
  une chute remplit légitimement jusqu'à la surface de son nœud
  amont, un bief enterré ne remonte jamais au-dessus de sa
  publication. Mesuré (simulation offline du cap sur le cache) :
  bande rendue >1 m **48,6 → 29,5 %** (= la publiée + 0,9 point de
  chutes légitimes), >2 m 40,4 → 24,6 %. (b) **La pose du sim
  s'arrêtait au gabarit** : `pinRivers` ne mouillait que
  `dist ≤ hw` alors que le carve taille sous la ligne d'eau jusqu'à
  ~±1,4 hw — le tablier d'épaulement (0-2 m sous l'eau sur 10-20 m
  aux témoins) restait sec derrière le rideau du settle, à la merci
  du déversoir plafonné (2 m³/s/cellule). Correctif : la POSE couvre
  l'empreinte du carve (±1,4 hw), gates de profondeur inchangés
  (berges jamais touchées), cœur pinné inchangé (hw − texel), relief
  ruisseau inchangé (≤ hw). Mesuré : **+25 % de texels posés** (le
  tablier). Hypothèses écartées en chemin : taille des voxels (sim
  2 m = sol fin ; mais lacs 16 m et eau de loin 64-70 m expliquent
  l'incohérence loin/près), maillage en escalier (marching squares
  sous-texel, innocent). Confirmé et DIFFÉRÉ : le profil
  LONGITUDINAL publié descend en marches (paliers 60-100 m, chutes
  4-13 m — produit du routage par biefs + min monotone) ; à traiter
  si encore visible après cette brique. Runtime pur : aucun bump,
  caches v102 inchangés.

- **2026-10-13 — HYDROLOGIE FINE, brique 1 : le profil d'eau est
  RECONSTRUIT depuis le sol fin, les bassins découverts deviennent des
  lacs (bump v103).** Ouverture du chantier (plan approuvé) : les
  surfaces naissaient du priority-flood 16 m (paliers plats aux
  niveaux de déversement) et tous les correctifs v99-v102 ne savaient
  que rabaisser — d'où l'escalier (paliers 60-100 m, chutes 4-13 m,
  flancs qui se vidangent, témoin (5978,3577)). La passe de siège est
  REMPLACÉE par une reconstruction en fin de phase B : (1) plancher =
  min de l'empreinte (axe + 10 offsets ±0,5..±1,4 hw) puis min
  longitudinal 5 points ; (2) liens de jonction reconstruits (plus
  proche point d'une autre rivière à portée max((hwA+hwB)·2,2, 15) du
  dernier point, tri topologique, cycle → coupe du lien le plus
  haut) ; (3) remplissage amont par mise en commun : s = max(plancher
  − 0,2, s_aval), bornes d'embouchure (mer / surface du receveur /
  bord de fenêtre / médiane anti-fosse), tête lakeFed plafonnée à son
  lac ; (4) l'eau ne monte jamais au-delà du plus bas plancher amont
  + 2,5 m (enveloppe prefix-min à récupération 0,01 m/m — SANS elle,
  un bief longeant une crête fine que la fenêtre n'a pas vue poolait
  38 m au-dessus de sa vallée amont, mesuré r38 ; le kWallCut par pas
  du plan était FAUX sur les murs soutenus, écart au plan assumé) ;
  (5) **bassins découverts** : tout bief poolé >1 m sur ≥3 points est
  inondé en flood PRIORITAIRE sur treillis 8 m aligné monde — si le
  flood atteint la borne (320 m), le terrain fin offre une fuite que
  le routage 16 m n'a pas vue : le niveau est RABAISSÉ à la selle de
  fuite (l'eau réelle s'écoulerait là) et le bief re-assis ; le bassin
  contenu (≥12 cellules) est émis comme LAC naturel masqué (dug=0,
  anneau sec de rivage) — S5d-bis creuse son lit, la nappe le rend
  loin et près, pinLakes le réservoire, l'émission coupe le ruban
  dessous : toute la machinerie existante, zéro format nouveau (TWB
  reste 7). Étangs de jonction re-nivelés sur le s recalculé ; gués
  épurés des segments >0,08 de pente (le cap gué taillait un gradin
  de pataugeoire en pleine goulotte). SUPPRimés : le siège v102
  (min-5 + clamp + kThroughCut) et le grounding lift du ruban runtime
  (WaterSystem — le lit carvé ne peut plus percer la corde, algèbre
  vérifiée ; les biefs enterrés restent enterrés). Variantes mesurées
  et rejetées en route : poolage pur sans bassins émis (bande hors-pin
  45,7 %), poolage borné local 0,5-1 m (reconverge vers v102 + marches
  arrière +4 m). **Mesuré** (cartes 0,0, tuiles 0_0/1_0, bande = sol
  sous la ligne au-delà du pin 1,4 hw ET hors nappe) : bande sèche
  >1 m 30,5/24,4 % (v102) → 34,5/32,0 % (+2-8 pts assumés : rives de
  bassins et descentes, population visée par la brique 2), >2 m
  25,2/20,3 → 26,9/24,1 % ; lèvres >2 m/segment 16,2/13,6 →
  15,0/12,1 % ; non-monotone 148/202 → 67/95 ; lacs émis 22/23 →
  57/55 (les bassins réels, désormais représentés loin ET près) ;
  virages 0 %. Témoins : (5978,3577) une seule cascade franche
  (620,5→612,0) puis bassin calme continu à 612,0, berges au-dessus
  de la ligne ; (5900,3660) bassin 612,0 ; (5800,4000) 607,1 constant.
  Juge final : tour dev. Briques restantes du chantier : recentrage
  latéral (v104), chutes explicites + habillage (TWB8), clôture.

- **2026-10-13 — HYDROLOGIE FINE, brique 3a : fusion des cascades en
  escalier (bump v104).** Retour dev après v103 (« il reste des
  cascades en escalier » en remontant depuis le témoin) : à la tête de
  la rivière du canyon, la rampe de déversoir du lac percé descendait
  645→622 en trois gradins de 5-11 m à paliers de 7-22 m — des
  banquettes RÉELLES du sol pré-carve (v102 avait le même escalier au
  point près : famille « gradins réels », pas un artefact du
  remplissage). Correctif (avancé depuis la brique 3, le recentrage
  passe après) : **fusion des grappes** — les chutes ≥1,5 m séparées
  de paliers <30 m sont re-profilées en UNE rampe raide du haut au bas
  de la grappe ; le carve recreuse une goulotte continue à travers les
  banquettes et le shader habille la pente en torrent. Deux règles
  d'intégration apprises en mesurant : la fusion passe APRÈS la
  découverte des bassins (la rampe brise les paliers constants que le
  rabaissement-à-la-selle doit voir — inversée, la tête remontait à
  +47 m sol) et ne traverse jamais un point couvert par une nappe (le
  déversoir d'un lac reste UNE lèvre franche depuis la nappe).
  **Mesuré** (tuile 1_0) : motifs lèvre→palier-plat→lèvre (<30 m)
  **27 (v102) → 24 (v103) → 9 (v104)** ; la tête témoin : nappe
  645,8 → lèvre unique 10,8 m → rampe continue 635→622. Leçon de
  jauge : compter les segments raides flagge les rampes voulues — le
  critère escalier est la présence du palier PLAT entre deux lèvres.
  Reste pour la brique 3b (habillage) : vasque de plongée au pied,
  écume de lèvre, embruns d'impact (réf. l'habillage des sources F4).

- **2026-10-13 — HYDROLOGIE FINE : l'eau de loin se bat contre le bon
  décor (bump v105).** Retour dev après v104 : « les bandes qui
  montrent de loin les rivières semblent calculées sur le décor low
  poly, pas sur le décor final ». Exact — et structurel :
  l'**overview** (MOV1 64 m, le sol du FarTerrain contre lequel l'eau
  lointaine est testée en profondeur) était décimé depuis la fenêtre
  16 m percée d'AVANT les carves (MapBaker), jamais depuis les sols
  fins finaux. Tant que les surfaces étaient des valeurs fenêtre
  (≤v102), ça collait ; assises sur le plancher fin creusé (v103+),
  elles passaient PLUSIEURS MÈTRES SOUS un décor lointain sans canyon
  → rubans lointains enterrés. Deux étages de correctif : (1) chaque
  tranche **splatte son sol fin final dans l'overview** pendant le
  bake (rects de tuile = partition, écritures disjointes sans verrou,
  la queue de complétion ordonne la lecture ; apron/anneau fenêtre
  gardent le repli fenêtre/stage-1) — le décor lointain montre enfin
  les gorges ; (2) l'aliasing 64 m fait encore straddler les picks
  lit/crête (mesuré : 51 % des points fleuve/rivière sous le sol
  overview bilinéaire, v104 comme v105 — la résolution, pas l'ordre),
  donc les **nœuds du ruban LOINTAIN sont clampés au sol overview
  + 0,4** à la construction du far set (TerrainBakeStreamer) — une
  réconciliation d'affichage à l'échelle 64 m, invisible à des
  kilomètres, à ne pas confondre avec le lift fin par-texel supprimé
  en v103 (le champ proche appartient au mesh local et au sim).
  Leçon : chaque consommateur doit être réconcilié avec le sol À SA
  RÉSOLUTION — le proche contre le 2 m (v103), le lointain contre son
  64 m (v105).

- **2026-10-13 — AUDIT « mauvais étage / mauvaise résolution » (3
  explorations) et brique A : l'eau de loin des bassins.** À la
  demande du dev, audit systématique de la famille de bugs des
  briques v103-v105 (un consommateur lisant un étage périmé ou une
  résolution étrangère) sur trois fronts : placement, ordre du bake,
  consommateurs runtime. 13 trouvailles classées (journal de l'audit :
  cette entrée et les 3 suivantes). Brique A (runtime pur, pas de
  bump) — le point le plus visible : **les bassins découverts
  n'avaient AUCUNE eau de loin** — la re-rasterisation 64 m des
  nappes lointaines exigeait une majorité (≥32 texels par bloc 8×8)
  qu'un bassin de 1-3 texels de large n'atteint jamais, et le ruban
  dessous est coupé à l'émission → trou d'eau lointaine de la
  longueur du bassin. Trois correctifs dans collectFarWater :
  (1) **couverture any-texel** des blocs (le depth-test contre le
  FarTerrain coupe déjà les débords aux berges, comme pour toute
  nappe) ; (2) **le nœud terminal d'un run n'est plus mangé** par la
  décimation 48 m (c'est le point de chevauchement sous la nappe —
  jusqu'à 48 m de trou sec à chaque entrée de lac) ; (3) les nappes
  lointaines ENTERRÉES sous l'overview bilinéaire sont **relevées
  jusqu'à affleurer leur bloc le plus bas** (borné +4 m — même
  réconciliation de résolution que le clamp des rubans v105 ; un
  bassin sous-résolution reste caché plutôt que flottant). Bonus :
  le probe de spawn lointain (farWaterWetAt) voit à nouveau les
  bassins.

- **2026-10-13 — AUDIT, brique B : lacs fins (bump v106).** Les lacs
  avaient encore le biais fenêtre que v103 a ôté aux rivières, et le
  remplissage les traversait par le lit. Trois correctifs en fin de
  phase B : (1) **ré-assise des lacs fenêtre** — chaque lac naturel
  est inondé en flood prioritaire 8 m depuis ses propres cellules ;
  une échappée sous niveau−0,1 est le VRAI seuil : le niveau descend
  à la selle (plancher mer) et le masque perd les cellules que la
  nappe plus basse ne tient plus (règle level+0,5 du dig) ; un lac
  touchant le bord de fenêtre est laissé tel quel. Mesuré : les 3
  lacs fenêtre de la tuile 0_0 descendent de 30-38 m (seuils fins
  réels, souvent via une gorge percée voisine) — à valider à l'œil,
  ce sont des lacs de montagne qui rapetissent ; celui de 1_0 est
  contenu (inchangé). (2) **Anti-tunnel (« barrage de verre »)** :
  les points de cours sous un masque de lac accepté sont planchérés
  au niveau du lac dans le remplissage (own et re-seat d'échappée) —
  un cours tracé à travers un lac était tunnelé sous son propre
  seuil par le plafond prefix-min (pris sur le LIT), et S5c sciait
  une tranchée à travers la rive : nappe suspendue au-dessus d'une
  fente. Mesuré : points tunnelés (<niveau−1 m sous masque)
  **77→0 (0_0) et 25→0 (1_0)**. (3) **Balayage global des étangs** :
  tout étang creusé s'aligne sur la plus basse surface recalculée
  traversant son empreinte (plus seulement les jonctions re-liées —
  les étangs d'épingle et les jonctions orphelines flottaient).
  Mesuré : étangs flottants (>rivière+0,5) **7/19 et 9/22 → 0**.

- **2026-10-13 — AUDIT, brique C : ordre & coutures (bump v107).**
  (1) **Re-clampe des affluents** après les passes qui modifient le
  receveur (re-seat d'échappée, fusion des cascades) : abaissement
  SEUL, receveurs d'abord, pour que les queues retombent sur le
  nouveau niveau du tronc. Mesuré : queues suspendues >1 m
  10→8 (0_0), 13→13 (1_0) — le reliquat est GÉOMÉTRIQUE (l'affluent
  se termine sur la berge, à 10-20 m du chenal, son plancher y est
  au-dessus du receveur) : périmètre de la brique recentrage, pas
  d'une clampe de surface. (2) **Adoucissement de couture** : hors du
  rect de carte, fineAt lit le sol DÉJÀ CARVÉ du voisin (double
  creusement, marche d'eau aux lignes de carte) — la surface
  recalculée est fondue vers la surface de routage sur l'approche du
  recouvrement (96 m, smoothstep) ; le bake du voisin fait foi dans
  SON rect. (3) **S6 voit tous les lacs** : le waterMask n'était
  nourri que par le flood fenêtre (lakeDepth), figé avant étangs et
  bassins — leurs rives n'avaient ni wetness ni plage et leurs lits
  gardaient le bruit de détail ±0,35 m ; les masques de lacs sont
  maintenant rasterisés dans le waterMask. (4) **Harmonisation
  demi-texel** : les tests de masque côté bake (lakeCovers,
  underLake, inFinalLake) passent du floor au centre (+0,5, la
  convention runtime de LakeSurface::covers) ; clipLakesToMapRect et
  le recensement du rapport échantillonnent le treillis de POINTS
  (min + m·texel) au lieu des coins.

- **2026-10-13 — AUDIT, brique D : lot runtime & outillage (sans
  bump).** Les huit points restants de l'audit, tous runtime ou
  outillage : (1) le **mesh des nappes proches est centré** sur son
  treillis de points (l'ancrage par coin décalait toute nappe d'un
  demi-texel vers +x/+z : débords aux crêtes +, manques aux crêtes
  −) ; (2) **sources d'entrée du sim dédupliquées** — seule une VRAIE
  tête injecte (headIsSource ou LakeOutlet) : chaque reprise après
  nappe/bassin/couture ré-injectait le plein débit du cours (ruisseaux
  visiblement gonflés après chaque bassin) ; (3) la **requête d'eau
  rivière** passe à 1,4·hw (l'épaule que le carve mouille et que le
  ruban et le sim affichent — nage/scatter lisaient « sec » dans une
  marge visiblement en eau) ; (4) les **rochers** testent enfin l'eau
  douce (seule classe de scatter sans test : blocs et colliders dans
  les lits et sous les nappes ; marge 0,3 — les rochers de rive
  restent) ; (5) les **impostors lointains** sondent aussi 16 m plus
  bas (la hauteur ~35 m moyenne les berges : des panneaux d'arbres
  trônaient en pleine rivière de gorge — la canopée lointaine suit) ;
  (6) un **arrivage lacs-seul re-scatte** les chunks résidents que
  les nappes couvrent (l'herbe plantée avant l'arrivée restait dans
  la nappe jusqu'au re-stream) ; (7) la **garde de crête de pinLakes
  teste le masque ET la distance au bord du bbox** : l'anneau sec des
  bassins (le texel extérieur) redevient une crête (colonnes d'eau
  pleine hauteur aux lèvres de sortie sinon), mais un texel sec
  PROFOND dans le bbox reste exempté — la version au masque seul
  ré-ouvrait les cratères de film autour des trous de rasterisation
  mi-lac (leçon du test « mask hole mid-lake », rattrapée par la
  suite complète) ;
  (8) **MapView remplit ses rivières** (le spawn du rapport pouvait
  s'asseoir sur un ruban). Préexistants pour la plupart — révélés par
  l'audit demandé par le dev après v105.

- **2026-10-14 — AUDIT, brique E : la roche connaît le sol final
  (bump v108).** La pente du masque rockExposure (S6) était calculée
  sur le stage-1 d'avant phase A et carves. Deux correctifs :
  (1) **pente sur le sol final** rééchantillonné à la même grille
  16 m, après les carves — même formule, mêmes seuils : le look ne
  bouge que où le terrain a bougé (mesuré : 0,06 % des texels).
  (2) En mesurant, découverte que le vrai suppresseur des parois de
  gorge est le terme **scree** (zones de dépôt → (1−scree) écrase la
  roche) : une face TAILLÉE par le carve est pourtant de la roche à
  nu — ajout du terme de **coupe fraîche** (max du delta
  preCarve−final sur le voisinage ±16 m : la paroi n'est pas creusée,
  son voisin-lit l'est) qui contourne le scree seul ; le gate de
  pente garde les lits plats sans roche. Mesuré : +0,69 % de texels
  en roche, concentrés aux faces réellement taillées (fentes de
  sortie de lacs, gorges fraîches). Vérité d'enquête consignée : les
  grandes parois du canyon témoin sont NATURELLES (le V de 65 m
  préexiste au carve, le lit n'a été approfondi que de ~7,5 m) et
  sous dépôt — leur habillage herbeux est un réglage scree/deposit
  existant, pas un bug d'étage ; à retoucher à l'œil du dev si
  voulu.

- **2026-10-14 — Brique F : recentrage latéral des biefs (bump
  v109).** La brique 2 du chantier : ~19 % des points couraient le
  long de murs fins (axe des mètres au-dessus du sillon de leur
  empreinte → eau enterrée invisible). Chaque point glisse vers le
  plus bas échantillon fin de sa propre empreinte, par petits pas
  LISSÉS (champ latéral moyenné sur ~±25 m), dans un repère
  perpendiculaire FIGÉ sur la polyligne d'origine ; extrémités,
  queues de jonction et points sous nappe immobiles ; les gués
  re-snappent sur l'axe déplacé. Trois leçons mesurées en route :
  (1) le repère recalculé sur des voisins déjà bougés replie la ligne
  en épingles (174/176 rivières invalidées) — d'où le repère figé ;
  (2) une traction non freinée MARCHE sans fin descendant les flancs
  (le min d'empreinte recule à mesure) — d'où le gain de convergence
  (ne bouge que si l'axe domine son sillon de >1 m, s'éteint en
  l'atteignant) ; (3) le gate virages tout-ou-rien par rivière jetait
  les biefs sains avec le pli — remplacé par un **rollback
  CHIRURGICAL** (±4 points autour de chaque virage >45°, itéré,
  convergence par croissance monotone de l'ensemble restauré).
  Budget : 10 passes × 4 m — mesuré optimal (14 passes sur-marche et
  les reverts mangent les biefs sains). **Mesuré : enterrés
  19,6→12,4 % (0_0) et 18,5→13,6 % (1_0), virages >45° inchangés
  (2 préexistants / 0), bande hors-pin stable.** Le reliquat (~13 %)
  = biefs contre-mur hors de portée d'un glissement de 40 m : leur
  traitement est la COUPE/REROUTE en corridor (re-trace 8 m sous
  contrainte), consignée au backlog du chantier.

- **2026-10-14 — Brique G : vasques de plongée + habillage des
  cascades (bump v110, fin de la brique 3).** Trois étages :
  (1) **vasque de plongée** — le carve ne sait que creuser depuis la
  surface, le pied concave d'une chute restait un plateau sec ;
  chaque cascade fusionnée (chute ≥4 m, pied hors nappe) émet un
  étang creusé à son pied (dug=1, rayon max(1,2·hw, 8), texel 8) —
  S5d creuse la cuvette, la nappe la rend, pinLakes la tient : la
  machinerie étangs, comme les bassins. Mesuré : +125 vasques sur la
  tuile 1_0 (morphologie step-pool des torrents) ; la tête de lac du
  témoin a la sienne (620,4 au pied de la rampe de déversoir).
  (2) **Habillage FX runtime** : updateRiverCascadeFx, calqué sur
  l'habillage des sources F4 — détection des chutes PAR LA PENTE des
  nœuds publiés (aucun marqueur baké, TWB inchangé ; enchaînement de
  segments ≥0,25 de pente, chute cumulée ≥3 m), embruns + grondement
  au pied, clés par position quantifiée, balayage par portée ;
  RiverSourceFxForm gagne mistCascade/soundCascade (moddable, repli
  sur la brume de source tant que les particules dédiées ne sont pas
  auteurées). (3) **Ruban lointain** : les nœuds des segments raides
  échappent à la décimation 48 m (une chute de 10 m s'étalait en
  toboggan de loin).

- **2026-10-14 — Embruns visibles + vue survol (runtime, sans
  bump).** Retour dev : « je ne vois pas les embruns ». Causes :
  portée héritée des sources (90 m — une cascade se lit de bien plus
  loin) et gerbe de brume de source trop discrète. Correctifs dans
  updateRiverCascadeFx : portée dédiée 250 m ; gerbe PROPORTIONNÉE à
  la chute (rate/burst ×clamp(chute/4, 1..3), rayon d'émission ≥
  0,25·hw, sprites grossis) ; et garde « eau visible » — un profil
  raide que le sol fin contredit est un bief enterré, pas une chute
  (67 sites détectés sur la tuile 1_0 dont des « chutes » de
  100-330 m qui sont des murs : pas d'embruns sur la roche sèche).
  **Vue survol** (demande dev : « voir quasi une map entière pour
  repérer les points d'intérêt ») : case « Overview flight (14 km
  view) » dans le panneau Rendering — farPlane 14 km (le FarTerrain
  couvre 18 km, c'était la caméra qui bornait à ~1,6-2 km) et
  brouillard aminci À LA SOUMISSION seulement (×0,08, fogStart
  ≥2 500 m) : le tuning persisté (mods/render-tuning.toml) n'en voit
  rien. Le champ proche (arbres, mesh d'eau fin) reste au rayon de
  vue ; le lointain 64 m + nappes/rubans lointains suffisent pour
  repérer canyons, lacs et cascades.

- **2026-10-14 — Les « triangles » au pied des chutes : masques de
  vasques non clippés (bump v111).** Retour dev : « les embruns, ce
  sont les triangles en bas des chutes ? ça ne marche pas du tout ».
  Ce n'étaient pas les embruns (particules) mais les NAPPES des
  vasques : les étangs de l'hydrologie clippent leur masque contre le
  sol (la nappe ne couvre que ce qu'elle tient, ≥4 cellules
  mouillées) — les vasques de la brique G émettaient leur disque
  PLEIN. Sur la pente d'une chute, le mesh dessinait des dalles à
  niveau au-dessus du vide : coins chanfreinés + jupes de crête = des
  triangles à chaque pied. Correctif : le masque de vasque ne garde
  que les cellules dont le sol fin ≤ niveau + 0,5 (la règle du dig),
  et un pied sans cuvette réelle (<4 cellules) n'émet RIEN — une
  goulotte qui file n'a pas de bassin à montrer. Mesuré (tuile 1_0) :
  **125 → 28 vasques**, masques limités à la poche réelle. Leçon : la
  machinerie réutilisée inclut ses GARDES — makePond sans son clip
  n'est pas makePond.

- **2026-10-14 — Les chutes deviennent des pentes (bump v112).**
  Direction dev : « enlever l'effet d'escalier des chutes pour
  revenir à des pentes, qui montrent mieux l'eau simulée ». Passe de
  PENTE MAXIMALE après la fusion des cascades : toute descente est
  bornée à 0,4 m/m — l'approche amont d'une lèvre s'incline À TRAVERS
  elle (la ligne d'eau passe sous le sol pré-carve = la consigne de
  creusement ; le carve taille la goulotte, et le sol FINAL repasse
  sous l'eau), jamais sous le niveau d'un lac traversé (anti-tunnel)
  ni plus de 6 m sous le plancher fin local (kTiltCut, dans la marge
  du gate anti-fente — non bornée, l'inclinaison enterrait 30 % des
  points sous les grandes lèvres que le carve refuse de percer,
  mesuré). Mesuré : segments >0,45 de pente 12,6 % → **7,7-8,6 %**,
  enterrés 12,4-13,6 → 14,6-15,6 % (le prix des rampes creusées) ;
  le résiduel raide = les grandes lèvres hors gate (lèvre franche
  plutôt qu'eau invisible) et les déversoirs de lacs (chute libre
  légitime depuis la nappe). Diagnostic embruns : LOG_INFO au spawn
  de chaque émetteur de cascade (« cascade fx: spray at (x, z) ») —
  lisible dans la console du jeu pour vérifier qu'on est à portée
  (250 m d'un pied de chute d'eau VISIBLE).

- **2026-10-14 — VASQUES SUPPRIMÉES : l'amphithéâtre de terrasses
  (bump v113).** Capture dev à (4837, 428, 5031) : un bol entier en
  terrasses d'eau empilées. Diagnostic dans la donnée : la rivière y
  fait hw 56 (fleuve de 112 m) → chaque vasque de plongée faisait
  **134 m de diamètre** (rayon 1,2·hw), et la descente en lacets en
  semait une par grappe fusionnée — 5 nappes géantes autour du seul
  point témoin, en échelle sur tout le versant. Les vasques sont des
  MARCHES par construction : contraires à la direction « des pentes,
  pas d'escaliers » prise la veille. Supprimées entièrement (le
  profil à pente bornée + le carve suffisent : des rapides ; les
  plats restants sont les bassins hydrauliques réels et les étangs
  d'hydrologie). Conservés : les embruns/grondement des chutes
  (détection par pente, indépendante des vasques) et le ruban
  lointain non décimé aux segments raides. Mesuré (tuile 1_1) :
  vasques 8 m **→ 0**, 3 étangs 16 m restants au niveau de la
  rivière. Leçon : un habillage dimensionné par hw devait être
  PLAFONNÉ — à 56 m de demi-largeur, « proportionné » devient
  monumental ; et surtout, tester l'esthétique sur un fleuve autant
  que sur un ruisseau avant de généraliser.

- **2026-10-14 — LISSAGE LONGITUDINAL : les vrais escaliers étaient
  le profil suiveur de banquettes (bump v114).** Retour dev après
  v113 : « ça a rien changé » — exact, les vasques n'étaient qu'un
  figurant. Le profil amont complet du bol témoin (4837, 5031) l'a
  montré : descente réelle de **96 m sur ~150 m** (pente 0,64), que
  la passe v112 ne pouvait pas rectifier — son plancher « ≤6 m sous
  le sol » la forçait à SUIVRE les banquettes du terrain : segments
  quasi plats alternant avec des plongeons de 3-10 m tous les 8 m,
  qu'un ruban de 156 m de large rend en terrasses empilées.
  Correctif : **lissage longitudinal du profil** (boîte ±5 points
  ≈ ±38 m, 2 itérations) — sur un tronçon raide l'eau taille une
  goulotte RECTILIGNE à travers les nez de banquettes (le gate du
  carve, 7,5-14 m, les creuse), remontée bornée à +2 m au-dessus de
  l'ancienne ligne, planchers lac/mer conservés, monotonie
  re-imposée. Mesuré : le bol passe d'une alternance
  +2,6/+9,9/+9,0 m par nœud à une rampe PROGRESSIVE 0,15→0,8 sans un
  plat ; alternances plat/plongeon sur la tuile 2,12 % → 1,18 % ;
  enterrés stables (14,9 %). Chaîne du jour consignée honnêtement :
  v112 (pente max, nécessaire mais insuffisante) → v113 (vasques
  supprimées, bonne décision mais mauvais diagnostic) → v114 (le
  vrai correctif).

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
