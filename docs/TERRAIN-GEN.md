# TERRAIN-GEN — génération de terrain réaliste (chantier 2026-07-31)

Système de génération de terrain « post-démo » : montagnes érodées (fastscape),
étages d'élévation (plateaux/mesas, falaises), hydrologie (rivières, lacs
d'altitude), littoral, biomes — pour les **deux modes de jeu** décidés le
2026-07-31 :

- **Sandbox** (mode de base) : monde **infini** généré depuis une seed —
  super-tuiles bakées en tâche de fond autour du joueur, cachées sur disque,
  authorables par records ancrés en coordonnées monde.
- **Scénario** (plus tard) : maps de taille limitée, générées par le même
  pipeline dans l'éditeur puis lourdement retouchées/moddées, expédiées en
  data §5.

Un seul générateur, deux orchestrations. État de l'art et décisions de
conception : plan du chantier (2026-07-31) ; références principales :
Braun & Willett 2013 (fastscape), Cordonnier et al. EG 2016 (uplift +
stream power), Barnes 2014 (priority-flood), Génevaux 2013 (hydrologie),
GDC 2018 Far Cry 5 (pipeline de production).

## Architecture

### La seam runtime (inchangée pour les consommateurs)

```
height(x,z) = Σ wᵢ·(bicubic(régionᵢ) + détail)  /  Σ wᵢ   — régions bakées, blend par poids de bord
              ↘ fondu vers proceduralBase(x,z) quand la couverture faiblit
            + authoredDelta(patches)                        — sculpt, TOUJOURS le dernier terme
```

- `TerrainParams` porte quatre pointeurs partagés immutables (contrat
  `patches`) : `patches` (deltas sculpt), `base` (`TerrainBase` = régions
  bakées), `sandbox` (identité du monde généré : le fallback devient la
  macro analytique S1), `biomes` (table + carte peinte optionnelle).
  Null partout = bit-identique à l'ancien monde (doctesté, y compris le
  test épinglé « village site level »).
- **Bicubique Catmull-Rom** obligatoire sur la base bakée (le bilinéaire
  facette les normales par différences centrales). Coût < les ~12 octaves
  de bruit qu'il remplace.
- Les trois couches (base bakée / détail runtime / deltas sculpt) ne sont
  **jamais aplaties** : le sculpt survit à un re-bake.

### Le pipeline de bake (headless, lib `meadows`, `engine/terrain/generation/`)

| Étape | Fichier | Algo |
|---|---|---|
| Contrôles | `TerrainGen` (`ControlSource`) | `ProceduralControls` (seed → continentalité/étages/uplift/mer/biome par climat) ; cartes peintes = variante Scénario à venir |
| S1 macro | `TerrainGen::synthesizeMacro` | étages blendés + relief warpé + terrasses adoucies (mesas) + profil côtier (distance chamfer au masque mer → rampe plage ou falaise selon l'étage) |
| S2 fluvial | `FluvialErosion::erodeFluvial` | uplift + stream power, solveur **implicite** (receivers sur surface priority-flood, tri, aires de drainage, sweep ascendant) ; routage réutilisé `routingInterval` itérations (~3×) ; k règle la pente d'équilibre S = U/(k·A^m) |
| S3 thermique | `ThermalErosion::erodeThermal` | relaxation Jacobi à l'angle de talus ; dépôt cumulé = masque d'éboulis |
| S4 hydrologie | `Hydrology::extractHydrology` | priority-flood ε → **lacs au niveau de déversoir** ; flow accumulation → polylignes de rivières (largeur ∝ √A) |
| S5 finalisation | `Finalize::finalizeTerrain` | upsample bicubique 8→2 m, creusement des lits (profil parabolique sous la surface d'eau + épaulement de berge), relief moyen amorti près de l'eau |
| S6 masques | idem | flow (log), wetness, beach, detailAmp, biome — canaux u8 de la région |
| Tuile | `TileBake::bakeTile` | S1→S6 sur tuile + apron (1 km), crop tuile + marge de recouvrement (64 m) ; les régions adjacentes se **fondent entre elles** dans la bande (blend par poids de bord dans `height()`) |

Déterministe pour (params, tuile) — c'est le contrat du cache. Le rim de
l'apron sert de niveau de base ; la macro analytique
(`macroHeightAnalytic`) est le fallback hors tuiles (FarTerrain) et la
condition aux limites implicite.

**Perf (machine dev, release)** : bake complet 4×4 km (apron 1 km, 100 it.
fluviales, sortie 2 m = 2113²) ≈ **4,4 s** — benchmark caché
`meadows-tests -tc="*bake benchmark*" -ns`. Région résidente ≈ 18 Mo.

### Sandbox — streaming (`game/TerrainBakeStreamer`)

Ring de prefetch autour du joueur (~1,4 km) → bake sur workers (mailbox
Phase-5, le frame thread publie) → cache disque
`terrain-cache/<seed>/tile_<x>_<z>_v<version>.trg` (+ sidecar `.twb` eau).
`kTileBakeVersion` invalide les caches quand le pipeline change.
Publication : nouveau `TerrainBase` immutable + remesh des chunks couverts
dans le ring + rebuild collision/veg + snap des cells + eau republiée.
Éviction au-delà de ~2,5 tuiles (le streamer re-demande au retour).
Activation : `sandboxTerrain = true` dans `landscape.toml` (seed =
`terrainSeed`) — off par défaut, le monde démo est intact.

### Eau (`engine/terrain/WaterBodies` + extension `WaterSystem`)

- Data headless : mer + `LakeSurface` (niveau propre, bbox) +
  `RiverSurface` (nœuds x/z/surface/demi-largeur, monotone aval).
  Requêtes pures doctestées `waterSurfaceAt` (avec plausibilité du probe :
  une galerie sous un lac reste sèche) / `waterDepthAt`.
- Gameplay : la lambda nage de `makePlayerContext` interroge
  `waterSurfaceAt` — nage dans les lacs d'altitude et les rivières ;
  `WaterVolumeForm` (boîtes) reste pour les intérieurs.
- Rendu : quads instanciés par lac à leur niveau + rubans de rivières
  (mesh monde), **même shading** que la mer (corps partagé
  `water_surface.glsl`, pipeline `waterlocal.vert` en positions monde) ;
  la pool-depth map passe par `waterDepthAt` → la mousse marche partout.
  Limite v1 : la réflexion planaire reste calée sur la mer (les lacs
  utilisent surtout réfraction+ciel).
- Forms : `WaterBodyForm` (lac), `RiverForm` + `RiverPointForm`
  (child-records §C.1) — un lac se monte en TOML pur.

### Biomes (`engine/terrain/BiomeMap` + `BiomeForm`)

- `BiomeSet` = table de `BiomeParams` (indexée par le u8 d'id) + carte
  peinte optionnelle ("TBM1"). L'id vient du canal biome de la région
  bakée (sandbox) ou de la carte (Scénario) ; **0 = neutre** → règles
  legacy exactes (les masques de scatter existants ne bougent pas).
- `terrain::biomeAt(params, x, z)` = LA seam (aussi pour le climat :
  `temperature`/`wetness` — le branchement survie attend un système de
  température côté gameplay, différé).
- `materialWeightsAt(params, x, z, h, n)` : neige décalée, roche sur
  pentes plus douces, bande de sable élargie + masque beach baké, herbe
  multipliée — consommé par GrassSystem (cutoff) et VegetationSystem
  (rochers, buissons).
- Palette par défaut (contrat avec le générateur, `landscape.toml`) :
  0 tempéré, 1 aride, 2 alpin, 3 toundra.

### Éditeur (`game/scenes/TerrainGenTool`, panel SceneEditor)

Seed + taille (1/2/4 km) → « Bake region here » (worker) → preview
publiée en live (même chemin que les tuiles sandbox) → « Accept » →
`.trg` + `TerrainRegionForm` + `WaterBodyForm`/`RiverForm`/`RiverPointForm`
via l'EditSession (GUIDs déterministes famille `7e88a111`, re-Accept =
patch des mêmes records) → Export = le mod. Retouche : brushes sculpt
existants, couche séparée.

## Moddabilité (§5)

Trois étages : (1) deltas `TerrainPatchForm` par-dessus (composable) ;
(2) patch field-level des records (`TerrainRegionForm.detail*`,
`WaterBodyForm.surfaceLevel`, `BiomeForm.*`…) ; (3) remplacement d'asset
par GUID (`.trg`/`.tbm`, last-writer-wins). Les saves ne portent pas de
terrain (l'édition est un acte d'authoring → EditSession → plugin).
Champs promus moddables : `terrainOctaves/Lacunarity/Gain`,
`mountainMaskLow/High`, `sandboxTerrain` (append, LandscapeTuningForm).

## Tests

`TerrainRegionsTest` (composition, TRG1, edge-blend, Forms),
`TerrainGenTest` (S1 : étages, terrasses, côtes, contrôles),
`FluvialErosionTest` (fastscape : flood, dendrites, plateauKeep, mer),
`HydrologyTest` (talus/masse, lac de cratère au déversoir, rivière),
`FinalizeTest` (upsample, carve, masques), `TileBakeTest` (déterminisme,
continuité inter-tuiles, fallback, benchmark), `WaterBodiesTest`
(requêtes + Forms), `BiomeMapTest` (neutre = legacy, TBM, table).

## Emprunts Waterways (analyse 2026-08-01 du dépôt Arnklit/Waterways)

Adoptés : flow mapping 2 phases, écume de berge en UV de ruban, profil de
vitesse latéral, edge fade, LOD des ondulations, dissolution de fin de
cours. **Approuvés dev, à faire post-commit** : (1) `waterFlowAt(x,z)` +
dérive/flottabilité des objets (l'extension gameplay de WaterBodies),
(2) mode debug eau (visualiser torrent/UV/flux), (3) preset lave/boue du
shader local, (6) subdivision adaptative des rubans par courbure.
**Différés** : gradient de couleur near/far à courbes d'easing (teintes
moddables), snapping des points de rivière au terrain dans l'éditeur, et
leur exclusivité — la flowmap bakée avec obstacles (dilate + pression +
blurs, pipeline détaillé dans `river_manager.gd`/`filter_renderer.gd`).
Sur la fusion : Waterways n'a AUCUNE logique de confluence (superposition
rasterisée) — notre chaîne jonction/fusion/étangs reste la référence.

## Rivières dans les autres moteurs (recherche 2026-08-01)

- **Unreal Water plugin (UE5)** : plans d'eau = SPLINES d'auteur (lac =
  spline fermée à Z fixe ; rivière = spline à Z monotone + largeur/
  profondeur/vitesse par point) ; conformité du terrain par CARVE du
  landscape le long de la spline (≡ notre passe S5) ; et surtout tout est
  composité dans une **« water info texture »** par zone (RT top-down :
  hauteur de surface, hauteur du fond, vitesse, par pixel) que le mesh
  d'eau unique (quadtree) et les requêtes gameplay échantillonnent — les
  jonctions/chevauchements se résolvent PAR PIXEL au compositing, pas en
  géométrie. C'est la réponse industrielle à nos conflits de rubans : à
  terme, une water-info texture locale autour de la caméra (la pool map
  en est l'embryon) remplacerait les coutures géométriques. Waterways
  (Godot) fait pareil en miniature avec sa system map.
- **Papiers** : Génevaux 2013 (réseau d'abord — déjà cité) ;
  Peytavie et al. 2019 « Procedural Riverscapes » (trajectoires, berges
  et creusement de rivières procéduraux — la référence à lire avant la
  vraie fusion inter-tuiles).
- **Réconciliation runtime (implémentée 2026-08-01)** : à chaque
  publication de tuile, les lacs (masques) et rivières (surfaces) qui
  touchent la zone re-blendée sont re-validés contre le `height()` LIVE
  — l'eau ne flotte jamais au-dessus du terrain composé, quel que soit
  l'ordre d'arrivée des tuiles. La fusion hydrologique inter-tuiles
  VRAIE (un seul réseau continu, niveaux partagés aux frontières) reste
  le grand différé ; la voie recommandée est la water-info texture
  ci-dessus + un bake hiérarchique des bassins frontaliers.

## Différés (revue de clôture 2026-07-31)

- **Peinture des cartes de contrôle** (étages/uplift/biomes/mer au pinceau
  dans TerrainGenTool, format TBM en entrée du bake) — le mode Scénario
  complet ; v1 dérive les contrôles de la seed.
- **Ré-ancrage des deltas sculpt au re-bake** (absTarget = oldBase+delta)
  — aujourd'hui les deltas relatifs glissent avec la base.
- Rendu de l'eau **courante** (scroll de flow des rubans, écume de
  rapides) ; réflexion des lacs.
- **Climat → survie** (système de température gameplay absent),
  `vegetationSet` par biome (espèces), splat shader biome-tinté,
  `BiomeVegetationForm` consommé.
- **Continuité hydrologique inter-tuiles** (une rivière traversant un
  bord de tuile est tronquée au bord ; l'apron limite la casse).
- Port compute S3/S5 (template GpuOcclusion) si l'itération le réclame ;
  bucket-sort du tri fastscape ; f16 résident si mémoire.
- Transitions de worldspace (maps Scénario multiples).
- **Vu au banc (validation 2026-07-31, sandbox actif, 41 FPS, vol multi-
  tuiles OK)** : (1) une **plaque d'eau rectangulaire** peut apparaître —
  le quad d'un lac couvre sa bbox entière, et une dépression voisine SOUS
  le niveau du lac mais hors du bassin est mouillée à tort → passer le
  rendu des lacs au masque de cellules (ou découper le quad au bassin) ;
  (2) `sandboxLakes/Rivers` s'accumulent sans éviction (417 lacs / 1184
  rivières après quelques tuiles) et la pool-map les scanne linéairement
  → évincer l'eau avec les régions + index spatial des requêtes.
- Blocage du spawn initial sandbox jusqu'à la première tuile (v1 : le
  joueur peut voir la macro non érodée quelques secondes au premier
  lancement).
