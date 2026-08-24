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

**Calibration de la dissection (2026-08-05, retour dev « tout est ramené
bas ») : `fluvial.iterations` 100 → 80** (v35 — d'abord 60, remonté à 80
au choix du dev après essai en jeu). Mesures sur le massif
−6,5 (diagnostic caché `erosion strength`) : à 100 it. l'érosion prenait
27 % de l'altitude moyenne (médiane −30 %) sans toucher les sommets
(max −6 %) — dissection pure, plus de hauts plateaux praticables. À 60 :
médiane +160 m, p40 +190 m, les fonds de vallées restent creusés (p10
quasi inchangé), versants toujours ravinés. La relaxation de crêtes
(`rounding`) mesurée innocente. À 80 (choix final) : dissection
intermédiaire. Hydrologie re-validée : 38 lacs / 54 rivières sur la
tuile spawn (49/54 à 100 it.), spawn au sec.

### Sandbox — streaming (`game/TerrainBakeStreamer`)

Ring de prefetch autour du joueur (~1,4 km) → bake sur workers (mailbox
Phase-5, le frame thread publie) → cache disque
`terrain-cache/<seed>/tile_<x>_<z>_v<version>.trg` (+ sidecar `.twb` eau).
`kTileBakeVersion` invalide les caches quand le pipeline change.
Publication : nouveau `TerrainBase` immutable + remesh des chunks couverts
dans le ring + rebuild collision/veg + snap des cells + eau republiée.
Éviction au-delà de ~2,5 tuiles (le streamer re-demande au retour).

**Boot & invalidation (fixes 2026-08-04)** :
- *Trou au spawn corrigé* : un chunk terrain dont le build était en vol au
  moment d'une publication capturait les ANCIENS params — son mesh périmé
  atterrissait et restait affiché jusqu'à un changement de LOD (d'où le
  « je m'éloigne et je reviens et le sol apparaît »). `remeshChunks` marque
  maintenant ces chunks (`remeshOnLand`) et le pump ré-enqueue à
  l'atterrissage — le pendant terrain du flag `stale` de l'herbe/végétation.
- *Gate de chargement 70→100 % raccourci* : (1) **hold** — les rings
  terrain/herbe/végétation ne streament plus tant que la première tuile
  n'est pas publiée (tout mesh bâti sur la base vide était re-meshé à la
  publication : double travail qui volait les workers des bakes) ;
  (2) **boost** — derrière le voile opaque du boot, les budgets
  anti-stutter s'ouvrent (requêtes ×16, uploads ×8 / 12 ms) : ils
  protégeaient une frame que personne ne voit. `WorldRenderer::
  setStreamingHold/Boost`, pilotés par la scène (publication / machine
  warmup). Le voile léger du spectateur garde les budgets normaux.
  NB : le 0→70 % (bakes de tuiles) était déjà couvert par le cache disque
  ci-dessus — un cache des meshes de chunks serait lourd et sans objet.
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
cours. Les quatre features approuvées — (1) `waterFlowAt` + dérive,
(2) mode debug eau, (3) preset lave/boue, (6) subdivision adaptative —
sont LIVRÉES par le chantier v2 (voir en bas de ce document).
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

---

# Chantier v2 — cohérence, érosion avancée, eau unifiée (2026-08-01)

Plan approuvé le 2026-08-01 (recherches Hesiod/HighMap, SimpleHydrology,
SimpleErosion, TerraForge3D + intégration de la leçon Unreal). Quatorze
briques livrées le jour même, 589 tests verts, benchmark 9-stage-1 inline
50,7 s → 70,6 s (+39 %, plafond accepté 2×) — le chemin réel (streamer,
stage-1 amorti par cache/dedup) reste de l'ordre de quelques secondes par
tuile. `kTileBakeVersion` 12 → 16, `kStage1Version` séparée (14).

## Recherches (verdicts)

- **Hesiod / HighMap** (GPL — idées seulement, zéro code) : le gisement.
  Adoptés : érosion fine multi-échelle post-upsample, dépôt sédimentaire à
  capacité, stylisation strates/rock-exposure, primitives d'authoring
  (ridgelines/stamp/alter_elevation), recurve d'élévation. Leur
  `depression_filling_priority_flood`/coast valident nos S4/S1 ; leur
  stream erosion est plus simple que notre fastscape implicite.
- **SimpleHydrology (nickmcd)** : le couplage débit→capacité (la carte de
  discharge module l'érosion) et l'érodibilité par biome (cohésion
  végétale) sont repris ; son flood de mares (retiré par l'auteur) et le
  méandrement émergent ne le sont pas (notre priority-flood et nos splines
  couvrent mieux à notre échelle).
- **SimpleErosion** : rien au-delà de ce que Hesiod industrialise.
- **TerraForge3D** : outillage desktop (nœuds, OpenCL, baker) — rien à
  emprunter.
- **Décision GPU** : la génération reste CPU (déterminisme bit-exact entre
  machines = contrat du sandbox ; génération headless doctestée ; les
  workers ne touchent pas au GPU — invariant Phase 5). Les nouvelles
  passes sont volontairement en gathers/Jacobi à support borné — la forme
  portable en compute. Déclencheurs de réouverture : bake > ~10 s/tuile ou
  preview interactif du TerrainGenTool.

## Briques livrées

- **B0 cohérence** : constantes uniques (`kDefaultSeaLevel`, `kSnowLine`,
  `kRegionDetail*`), `pondDepth` mort supprimé, garde null du stage-1
  centre, `kStage1Version` découplée, LOG des caches corrompus rejetés,
  en-têtes réalignés sur le bake deux étages, doctests couture
  stage-1/stage-2 + `routeFlow`.
- **B1 érodibilité par biome** : table `BiomeErosion` (palette 0..3),
  grids blurrés 3×3 branchés sur les hooks (déjà présents) d'`erodeFluvial`
  /`erodeThermal` ; `capacityScale`/`fineScale` alimentent B2/B4.
- **B2 dépôt sédimentaire** : balayage descendant à capacité
  (`Qc = kc·A^m·S·dt`) dans la boucle fastscape — fonds de vallées plats,
  cônes, deltas plafonnés sous le déversoir ; `kc = 0` = chemin legacy
  bit-exact ; cumul exporté (`deposit`, sidecar TS12) pour les masques.
- **B3 recurve** : PCHIP monotone 3 points au-dessus de la mer, fin de S1
  avant coastProfile + `macroHeightAnalytic` (l'érosion ré-équilibre le
  remap) ; `terrainRecurve*` dans LandscapeTuningForm/landscape.toml
  (défauts = identité bit-exacte ; vider terrain-cache après changement).
- **B4 érosion fine** : `FineErosion` — carve stream-power LOCAL à 4 m
  (receivers steepest-descent sans flood, accumulation à portée bornée
  96 m, capacité couplée au discharge du composite partagé, protection
  lits/lacs/plages, budget 3,5 m) + micro-thermique 2 m ; fenêtre fine de
  Finalize restreinte à keep+halo 192 m (rembourse l'essentiel du coût) ;
  masques detailAmp/wetness modulés par l'incision ; doctest de LOCALITÉ
  bit-exacte + divergence de bande < 2,5 m entre voisins.
- **B5 rockExposure + strates** : canal u8 `rockExposure` (pente ×
  anti-éboulis × bandes stratifiées warpées) dans TerrainRegion/TRG2 ;
  knob géométrique `strataAmplitude` livré à 0 (risque de battement avec
  le terracing S1 — activation après revue visuelle).
- **B6 matériau falaise** : `SplatLayer_Cliff` procédural (strates FBM),
  `cliffW = smoothstep(pente raide) × rockExposure` (transporté par
  vColor.r — mort sinon sur le terrain proche), banding d'altitude
  in-shader, miroir CPU lockstep (`MaterialWeights.cliff`), footsteps
  falaise = Rock. Pas de `cliffiness` par biome (le shader n'a pas l'id ;
  différé — le caractère biome passe déjà par l'érosion).
- **B7 debug eau** : combo panel > Water (Flow/Torrent/UV ruban/Info:
  surface/profondeur/flux) via `uWaterDebugInfo`.
- **B8 waterFlowAt + dérive** : requête headless (kernel `riverFlowSample`
  partagé, profil latéral miroir du shader, recouvrements pondérés par la
  berge), dérive de nage via PlayerContext (`swimDriftFactor` 0.8, §2.9
  intact — la cible de vitesse est poussée, pas l'attribut), composant
  `Floater` cinématique (props collés surface + courant, bob déterministe
  par id d'entité — v1 sans dynamique Jolt, décision dev).
- **B9 subdivision adaptative** : `RiverGeometry::subdivideRiverNodes`
  (Catmull-Rom par borne d'angle 0,14 rad, minStep 2 m) + clamp de
  demi-largeur par rayon de courbure (l'apex d'un lacet ne se replie
  plus) ; consommée par le ruban ET le raster info (même courbe).
- **B10 water-info texture (la leçon Unreal, complète)** : bake CPU worker
  `WaterInfoMap` 1024² / 1536 m (surface R32F absolue — le f16 relatif ne
  tient pas un lac à 700 m ; profondeur+flux RGBA16F), raster PAR CORPS
  (O(aire mouillée), height() sur texels mouillés seulement), jonctions
  résolues PAR TEXEL par le kernel partagé ; côté shader le flux composité
  remplace le flux du ruban quand |surface_texture − y| < 0,35 m → deux
  rubans croisés rendent identique ; flag de validité (contenu changé →
  fallback vertex data, jamais de fausses jonctions) ; suivi caméra 384 m,
  mêmes stamps que la pool map. `waterFlowAt` CPU reste analytique — une
  seule vérité, la texture est un cache de rendu dérivé.
- **B11 matériaux d'eau** : `WaterMaterialForm` (§5, moddable TOML —
  teinte+force, deep, absorption, écume+gain, émissif, flowSpeedScale,
  viscosité, waveScale) référencé par GUID depuis WaterBodyForm/RiverForm ;
  résolution en table compacte headless (slot 0 = eau par défaut, valeurs
  = les constantes shader, bit-identique) ; layout vertex local 10 floats
  + `WaterMaterialsUbo` (16 slots) ; la lave = émissif + viscosité + écume
  incandescente, en pur TOML.
- **B12 authoring** : `Authoring.{hpp,cpp}` headless — `stampKernel`
  (Add relatif / Max-union absolu / Blend plateau), `stampRidge` (crête
  Bézier max-composée, ne creuse jamais), `baseElevationAt` (anchors),
  `alterElevation` (nivelle le centre, PRÉSERVE le relief — la primitive
  site-de-village). Aucun branchement pipeline en v1 : l'intégration
  (`AuthoredControls` en S1 avant S2) appartient au chantier TerrainGenTool.

## Modding (nouveau depuis v2)

- `WaterMaterialForm` : preset d'eau complet en TOML, référencé par
  `material = "<guid>"` sur un lac ou une rivière (null = eau normale).
- `terrainRecurveLow/Mid/High` (landscape.toml) : la courbe d'altitude du
  sandbox. `BiomeErosion` reste C++ (TileBakeParams) — promotion en Form
  si le besoin modding se présente.

## Différés (revue de clôture v2, 2026-08-01)

Reconduits : peinture des cartes de contrôle (Scénario), ré-ancrage des
deltas sculpt, climat→survie, vegetationSet, splat biome-tinté,
continuité hydrologique inter-tuiles VRAIE (la water-info texture v2 est
le socle recommandé), transitions de worldspace, port compute (voir
décision GPU), plaque d'eau bbox / éviction sandboxLakes / spawn initial
(vus au banc v1). Nouveaux :
- **Strates géométriques** : knob livré à 0, activation après revue
  visuelle (battement possible avec le terracing S1).
- **Érosion droplets** : REJETÉE (pas différée) — l'interaction
  séquentielle qui fait sa qualité casse le bit-exact inter-tuiles.
- **Intégration pipeline d'Authoring** (`AuthoredControls`/S1) → chantier
  TerrainGenTool ; signatures posées.
- **Flottabilité dynamique** (corps Jolt + poussée d'Archimède) — le
  `Floater` cinématique v1 la remplace en attendant ; extension de la
  façade meadows-physics à décider.
- **`cliffiness` par biome** (le shader n'a pas l'id biome — passerait
  par le masque rockExposure au bake).
- **Assombrissement humide du terrain** près de l'eau (tap terrain.frag
  sur la water-info texture) — non fait, coupé du périmètre v2.
- **Matériau d'eau côté gameplay** (viscosité → vitesse de nage, dégâts
  lave) — la donnée est déjà headless (`WaterBodies::materials`).
- **Meshes de falaise sur pentes raides** (constat 2026-08-02, muraille
  côtière de (-58800,-21200)) : un heightfield ne fait pas de vraie
  paroi — pas de surplomb, thermique ~40-45° max, 2 m/texel. Les vraies
  falaises (côtes dures, gorges, cirques) demandent des meshes de rocher
  plaqués là où pente + rockExposure sont forts — le modèle Skyrim/BotW.
  Mécanisme : une règle de scatter par chunk (extension du système
  herbe/végétation existant), pas un nouveau système.
- **Vallées glaciaires / vrais fjords** (auges creusées sous le niveau
  de mer puis noyées) — hors scope lithologie ; à réfléchir si le besoin
  bite après les côtes dures.
- **BUG nuages : la couche s'éteint quand la caméra passe au-dessus**
  (signalé 2026-08-02, côté rendu — ciel/nuages, pas génération).
  Devenu facile à déclencher : les sommets (1000-1300 m) percent la
  couche (800-1000 m). À corriger dans le chantier ciel : le dôme/bake
  nuages doit rester rendu vu de dessus.

---

# Cible paysage & peuplement — biome tempéré (VALIDÉE dev 2026-08-24)

La description du terrain que le générateur doit viser pour le biome
tempéré (palette 0) — écrite AVANT de retoucher les paramètres, pour que
chaque réglage ait un critère de réussite mesurable. Constat de départ
(retour dev + mesures seed 1337, diagnostics `variety transect` /
`vista` de `SpawnDiagnosticTest`) : le terrain actuel est **trop érodé,
trop bas et uniformément haché** — relief médian 24-56 m par fenêtre de
250 m, 13-20 % de pentes >30°, 51 lacs sur la tuile de spawn, horizon
bouché à 0,3-1 km sur presque tous les azimuts, sommets réels (1250-
1350 m) à 25-30 km donc jamais vus. La variété statistique existe (un
« événement » tous les ~320-365 m) mais c'est toujours LE MÊME
événement : une ravine. Le problème n'est pas la quantité de relief,
c'est son ABSENCE DE HIÉRARCHIE.

## 1. Le principe : trois échelles emboîtées, chacune lisible

Référentiel de rythme : course de base 5,5 m/s (`movementSpeed` ~110 ×
1/20), sprint 11 m/s, monture 9 m/s. **45 s de course = ~250 m** —
l'unité de « changement pertinent » demandée.

- **Micro (50-250 m) — l'incident.** Un ruisseau, un bosquet, un
  affleurement rocheux, une butte, une clairière, une ruine. C'est
  l'échelle du scatter et des POI, PAS celle du relief : le sol des
  zones habitables reste calme (relief < 15 m par fenêtre de 250 m)
  pour que l'incident se détache au lieu de se noyer dans les ravines.
- **Méso (250 m - 1,5 km) — la forme de paysage.** À tout moment le
  joueur peut nommer où il est : UN fond de vallée, UN versant, UNE
  crête, UN rebord de plateau, UN bassin de lac, UNE gorge. Une seule
  forme à la fois, composée — pas un hachis isotrope. C'est l'échelle
  qui doit changer toutes les ~1-3 fenêtres de 45 s.
- **Macro (1,5-10 km) — la région et ses OBJECTIFS superposés.**
  Arbitrage dev : le vrai manque de l'ancien terrain était le nombre
  d'**objectifs sur la carte**. La cible combine l'intime et
  l'héroïque, à la Skyrim, en DEUX couches simultanées :
  - *intime* : des collines ou vallées marquantes à ~3 km (le but de
    la demi-heure) ;
  - *héroïque* : un **sommet alpin à ~6 km** (le but de la session,
    600-900 m, visible depuis les vallées) ;
  des plaines et plateaux peuvent exister entre les deux. Les très
  hauts massifs (1200-1400 m) restent des ancres d'horizon à 15-30 km.
  Depuis tout point de voyage : au moins un amer de chaque couche
  visible.

## 2. La hiérarchie du relief (le correctif « trop érodé »)

Le budget de dissection n'est plus uniforme ; il se répartit en trois
familles de terrain avec un CARACTÈRE par famille. Répartition choisie
par le dev : **40 / 35 / 25** — un monde franchement montagneux, où le
voyage se négocie par les passages et où les vues dominent :

- **Socles calmes (~40 % des terres)** : fonds de vallées, plaines,
  dessus de plateaux. Pentes < 10°, relief < 15 m / 250 m, dissection
  fine ÉTEINTE (le mécanisme `gentle` existe — l'étendre aux socles,
  pas seulement aux corridors). C'est là que vivent le peuplement, les
  chemins et les incidents micro.
- **Versants travaillés (~35 %)** : les liaisons entre socles. C'est
  ICI que la dissection actuelle est belle et doit se concentrer —
  ravines, éboulis, épaulements. Pentes 10-30°, franchissables mais
  coûteux hors passages.
- **Drame (~25 %)** : rebords de plateau, gorges, falaises, hauts
  sommets. Pentes > 30°, infranchissable hors cols/échancrures — c'est
  le relief qui FERME et qui oriente (et qui donne les vues).

Conséquence verticale : le relief de session remonte — depuis n'importe
où, un sommet alpin de 600-900 m atteignable à ~6 km ET des collines/
vallées marquantes à ~3 km (les deux couches d'objectifs du §1), les
1200-1400 m réservés aux ancres régionales. L'érosion ne
« ramène plus tout bas » : elle sculpte les versants, les socles hauts
gardent leur altitude (le hook `plateauKeep` existe — l'étendre).

## 3. Les vues (le correctif « pas de beaux paysages »)

- **Lignes de fuite** : les vallées des socles calmes sont ORIENTÉES
  (largeur 500-1500 m, axes de plusieurs km) — depuis le fond, la vue
  porte le long de l'axe au lieu de buter sur la ravine d'en face.
- **Belvédères** : tout rebord de plateau, col et crête de versant est
  un point de révélation : la région suivante se découvre d'un coup
  (le moment BotW). Les chemins CRÊTENT à ces points (§5).
- **Amers** : cf. macro §1. Silhouettes à composer : pic isolé, mesa à
  rebord franc, échine allongée — reconnaissables, pas interchangeables.
- Critères mesurables (diagnostic `vista`, aux points de voyage plutôt
  qu'au spawn actuel) : ≥ 30/72 azimuts avec horizon au-delà de 2 km ;
  ≥ 1 amer à élévation > 2° situé à plus de 3 km ; depuis un
  belvédère, profondeur de vue médiane > 6 km.

## 4. L'eau hiérarchisée (le correctif « 51 lacs »)

Quatre niveaux de cours d'eau (arbitrage dev), chacun avec son rôle de
gameplay — le rang découle de l'aire de drainage, comme la largeur
actuelle (√A), mais avec des CARACTÈRES discrets, pas un continuum :

- **Ruisseau** : l'incident micro, tous les ~500-800 m sur les
  versants ; enjambable partout, à peine creusé.
- **Rivière** : une par vallée maîtresse, nettement PLUS CREUSÉE
  (berges lisibles, lit encaissé) ; franchissable aux gués/ponts tous
  les 800-1500 m, nage facile.
- **Fleuve** : le véritable obstacle — **un par région, tous les
  ~10-15 km** : une frontière naturelle qu'on longe souvent et
  franchit rarement. Nage possible mais **dérive forte** (le
  `waterFlowAt` + `swimDriftFactor` existants) ; franchissements
  aménagés (ponts aux villes/villages, bacs ou gués ailleurs) tous les
  ~2-4 km. Les villes naissent à ses ponts et à son embouchure.
- **Embouchure — selon la côte** (la lithologie décide) : **delta**
  (plaine tressée, bras multiples, marais) sur côte basse/molle ;
  **estuaire / entrée de mer** (bras unique élargi, encaissé, profond)
  sur côte dure.

Et les plans d'eau :

- **Lacs rares et signifiants** : 2-6 par tuile de 4 km (aujourd'hui
  51) — un lac est une DESTINATION (bassin méso avec berges lisibles),
  plus une flaque de dissection. Relever `minLakeDepth`/`minLakeCells`
  et amortir la micro-dépression sur les socles.
- **Océan à deux étages** (aujourd'hui : fond plat à −30 m) :
  **plateau côtier −5..−25 m** (baies lisibles, plongée, épaves,
  mouillages des ports) puis **talus vers −100..−150 m au large** —
  l'eau foncée du large se lit depuis les falaises et donne sa
  profondeur visuelle à l'horizon marin. Les baies abritées + un
  arrière-pays plat = sites de ports (§5) ; les côtes dures gardent
  leurs falaises plongeantes.

## 5. Peuplement : emplacements, distances, chemins

Hiérarchie des lieux — chaque niveau a son site type, sa taille de
plateforme (pad `alterElevation`, primitive déjà livrée en B12) et son
espacement cible (en course de base ; diviser par ~1,6 à monture) :

| Niveau | Bâtiments | Espacement | Temps | Site type | Pad |
|---|---|---|---|---|---|
| Hameau | 3-8 | 0,8-1,2 km | 2-4 min | replat de socle, source/ruisseau < 200 m | ~60×60 m, pente < 5° |
| Village | 10-25 | 2,5-4 km | 8-12 min | confluence, tête de pont, rive de lac, croisée de chemins | ~150×150 m |
| Ville | 30+ | 8-12 km | 25-35 min | grande confluence, estuaire, butte de plaine, rebord défensif | ~300×300 m |
| Port | ville/village côtier | 10-15 km de côte | — | baie abritée, plateau d'eau peu profonde + arrière-pays plat | quai + pente d'accès |

- **Entre les lieux, la curiosité** : un POI non habité (ruine, donjon
  — générateur cyclique déjà livré —, sanctuaire, camp) à ~400-900 m de
  tout point de chemin, légèrement HORS chemin (visible, pas dessus).
- **Chemins** — le réseau suit la praticabilité (le champ `gentle`
  devient le champ de coût des routes) :
  - *sentier* (hameau↔hameau) : épouse le terrain, gués ;
  - *chemin* (village↔village) : fond de vallée d'abord, franchit aux
    cols, ponts aux rivières maîtresses ;
  - *route* (ville↔ville) : pente tenue ≤ 8-10 % (lacets, déblais),
    crête aux belvédères — la route EST la visite guidée des vues §3.
- **Passages** : toute chaîne/rebord est percé d'un col ou d'une
  échancrure tous les ~3-5 km de linéaire — jamais de cul-de-sac
  régional ; le drame §2 ferme localement, jamais globalement.
- Placement : scoring sur les champs existants (pente, eau, socle,
  connectivité aux corridors, visibilité depuis les approches) —
  sélection déterministe par seed, pads via Authoring, records ancrés
  monde (le contrat sandbox §5 du CLAUDE.md tient tel quel).

## 6. Critères d'acceptation (diagnostics à re-passer)

Sur `variety transect` (transects de 16 km, fenêtres 250 m) :
- relief médian par fenêtre : **18-30 m** (au lieu de 24-56 — plus
  haut que la première proposition, cohérent avec le 40/35/25) ;
- fenêtres plates < 8 m : **15-35 %** (concentrées sur les socles) ;
- pentes > 30° : **< 15 %** des échantillons, et « infranchissable
  continu » jamais > 400 m sans passage ;
- espacement moyen des événements ≤ 500 m, pire trou ≤ 1200 m — ET
  alternance des TYPES d'événements (relief / régime / eau / lieu),
  plus une seule famille dominante.

Sur `vista` (aux points de voyage) : les seuils du §3, PLUS les deux
couches d'objectifs — un amer intime (colline/vallée marquante) à
~3 km et un sommet alpin à ~6 km visibles depuis la majorité des
points de voyage. Sur l'hydrologie : 2-6 lacs/tuile ; ruisseaux ~tous
les 500-800 m ; 1 rivière creusée par vallée maîtresse ; fleuves
espacés de 10-15 km avec franchissement aménagé tous les 2-4 km ;
embouchures delta/estuaire selon la dureté de la côte ; océan −5..−25 m
sur le plateau, −100..−150 m après le talus. Sur le peuplement :
espacements du tableau §5 respectés à ±30 % le long des chemins.

Statut : **VALIDÉE par le dev (2026-08-24)**. Arbitrages actés :
familles de relief **40/35/25** (monde franchement montagneux) ;
objectifs superposés intime ~3 km + alpin ~6 km (le manque
d'OBJECTIFS était le vrai défaut, pas le manque de variété brute) ;
espacements de peuplement du tableau §5 tels quels ; **périmètre en
deux chantiers** — celui-ci livre relief hiérarchisé + eau + scoring
des sites (emplacements réservés, mesurables), le chantier peuplement
pose ensuite hameaux/villages/chemins dessus ; hiérarchie d'eau à
quatre niveaux et océan à deux étages (§4). Les nombres restent des
cibles de départ à retoucher en jeu ; l'implémentation se planifie
brique par brique.

Arbitrages d'implémentation (plan approuvé 2026-08-24, 14 briques
B0-B13) : **étage 0 régional intégré au chantier** — réseau
hydrologique maître par super-région ~24-32 km, grille 128 m sur la
macro analytique (priorityFloodFill + routeFlow réutilisés) → tracés
réels des fleuves, aires vraies, niveaux partagés aux frontières ;
il imprime les vallées maîtresses en S1 et servira de couche de
coût/routage aux chemins. Delta v1 = bras élargi + marais (tressage
différé) ; variantes de silhouettes de pics (cône/mesa/échine) dès la
brique objectifs ; scoring de sites analytique (pas sur tuiles
bakées) ; pads mirrorés dans `macroHeightAnalytic` ; gués par motif
déterministe sur l'abscisse curviligne ; événements « lieu » comptés
dans l'alternance dès le scoring.

## Journal du chantier

### B0 — Instruments (2026-08-24)

Diagnostics étendus (`variety transect` / `vista`,
tests/SpawnDiagnosticTest.cpp) : classification des fenêtres de 250 m
en familles socle/versant/drame (pente médiane des pas + relief),
« infranchissable continu » (plus long tronçon contenant un pas > 30°
sans appui < 15°), typage/alternance des événements, croisements
d'eau par km, lacs/rivières par tuile ; vista re-basée sur 8-12
**points de voyage** (grille jitterée déterministe 8 km, hash
splitmix local — std::hash non portable) avec les deux couches
d'objectifs (sommet > 500 m à ≤ 8 km ; colline marquante = max local
de son disque de 500 m ET > 120 m au-dessus de son anneau de 1 km,
à ≤ 4 km).

**Baseline seed 1337 (avant toute retouche)** :
- Transects 2×16 km : familles E-O **19/67/14**, N-S **41/50/9**
  (cible 40/35/25 — les versants ravinés dominent tout) ; relief
  médian 56 / 24 m ; plates 9 / 33 % ; pentes > 30° : 20 / 13 % ;
  infranchissable max 350 / 425 m ; événements dominés par le type
  relief (64 / 61 %), espacement moyen 320 / 365 m, pire trou
  2000 m ; croisements d'eau 0,31/km.
- Hydrologie : **10-63 lacs/tuile** (moy. ~38 ; cible 2-6), 22-89
  rivières/tuile.
- Vista aux 9 points de voyage : horizon ouvert (≥ 30 azimuts au-delà
  de 2 km) : **1/9** ; amer > 2° au-delà de 3 km : 7/9 ; « sommet »
  > 500 m à ≤ 8 km : 8/9 — mais ce sont des dômes arrondis 500-600 m
  sans silhouette ; colline marquante à ≤ 4 km : 9/9 **à ~0,5 km
  partout** — la couche intime existe mais sans AUCUNE hiérarchie
  (des bosses omniprésentes ≈ zéro amer nommable). Confirme le
  diagnostic : le manque n'est pas la variété brute, c'est la
  hiérarchie et les objectifs.

NB build Windows : le test d'or scatter
(`vegetation scatter: instance buffers are frozen`) échoue sous MSVC
(hash golden capturé sous clang/Fedora — math flottante différente) ;
préexistant au chantier, ne pas re-capturer sous MSVC.

### B1 — Champ « calm » (2026-08-24)

`ControlSample::calm` [0,1] : plaines vraies (sans orogenèse/chaînes/
roche dure, éclaircies par la bande `kSaltCalm` — certaines plaines
restent rugueuses) + dessus de plateaux soulevés (gatés uplift) +
corridors (sur-ensemble de `gentle`). En stage 1, fusion des fonds de
vallées post-érosion (déviation ~160 m faible + hors cuvette de lac).
Export `TileStage1.calm`, sidecar TS15. Tuile spawn : calm>0.6 =
39,6 % des cellules sèches. Caches v31/v36.

### B2 — Budget d'érosion par famille (2026-08-24)

Quatre mécanismes, tous des extensions de hooks existants :
- `fineScale ×= 1−0,95·calm` (stage 2 — remplace le terme gentle) ;
- socles BAS (< 150-400 m) : érodibilité ×(1+0,8·calm·low)
  (S = U/(k·A^m) — MONTER k aplanit : même sens que les facteurs
  plain/gentle, le plan de brique avait le signe inverse), capacité
  sédimentaire ×(1−0,5·calm·low) (baisser la capacité fait déposer :
  les fonds se comblent), talus ×(1−0,2·calm) ;
- socles HAUTS (> 150-400 m) : `keep` étendu (`kCalmKeep` 0,25,
  mirroré dans `macroHeightAnalytic`) — le plateau garde l'altitude au
  lieu d'être disséqué (gate d'abord posé à 60-250 m : le re-mix du
  macro brut rendait les terres moyennes PLUS rugueuses, remonté à
  150-400) ;
- **relaxation calm** (stage 1, sœur de roundRidges) : blend 0,65·calm
  vers la moyenne 160 m — l'érodibilité ne fait qu'incliner les
  ravines, la relaxation les efface sur les socles.

Mesures (spawn) : massif étalon intact (max 1238 vs 1260, moyenne 780
inchangée) ; altitude moyenne des transects +8/+18 m ; **fenêtres
socle : médiane 12,4 m < 15 ✓ — là où elles existent**. Mais le
recensement 2-D (nouveau diagnostic `family census`) montre le vrai
état : 6,4 % de fenêtres socle / 79 % versant sur la tuile spawn,
médiane globale 118 m — le calm est FRAGMENTÉ sous les 250 m (fonds
de vallées étroits). Élargir les socles = structurellement B4
(vallées orientées) et B5 (vallées maîtresses à fond de 800-1500 m) ;
le budget 40/35/25 se joue là, pas dans les hooks d'érosion. Caches
v32/v37 ; `erosion calibration` re-passé : le fit a dérivé
(sous-promesse jusqu'à ~175 m sur le massif étalon, sur-promesse
~230 m sur une bande rare de piémont) — le re-fit complet est du
périmètre B6 comme prévu au plan ; le doctest d'accord au rim
(fallback vs tuiles) reste vert, la couture proche n'est pas
affectée.

### B3 — Objectifs : grilles d'amers jitterées (2026-08-24)

`landmarkLayer` (TerrainGen.cpp) : grille jitterée déterministe par
couche — le fbm ne garantit pas d'espacement, la grille borne la
distance au plus proche amer, et le miroir analytique est automatique
(même chemin `controls.at` → `landHeight`). Par cellule, le hash
(`noise::lattice`) décide position (jitter 0,2-0,8), hauteur, rayon,
orientation/élongation et VARIANTE de silhouette :
- **couche alpine** (cellules 7 km, +600-900 m sur `plateau`, rayons
  1,2-2 km) : cône / échine allongée (aspect 2,8-4,5) / mesa à rebord
  franc — ajoutée au socle de base donc protégée par le `keep`
  (sommet conservé, flancs disséqués = le caractère alpin), atténuée
  sous les ancres houle/massif (plateau > 450-750) et gatée inland ;
- **couche intime** (cellules 3,5 km, 120-250 m, rayons 0,6-1,2 km) :
  2/3 dôme-colline (atténué en pays de chaînes), 1/3 **clairière** —
  bol de suppression du relief via le nouveau
  `ControlSample::reliefScale` (multiplie les porteuses oscillantes
  dans `landHeight`, jamais le socle) + `calm` → 1 ; gatée uplift
  (sur l'orogenèse active, les replats sont le rôle des corridors).

Mesures : doctest « landmark guarantee » — pire distance à un haut
relief > 500 m depuis 171 points intérieurs = **4,6 km** ✓ ;
vista : sommet ≤ 8 km depuis **5/5** points de voyage (0,5-4,3 km),
colline marquante ≤ 4 km 5/5 ; transect N-S : max 583 → **701 m**,
moyenne +48 m (un pic sur la ligne) ; massif étalon sain (1299/782).
L'horizon reste fermé (1/5 points ouverts) — attendu, c'est B4/B5.
Bench bake : 18,2 s/tuile sur la machine Windows/MSVC (~15 s
pré-chantier sur la machine dev) — ajouts du chantier ≈ 1-2 s, passe
perf au périmètre B6. Caches v33/v38.

### B4 — Vallées orientées, vallées maîtresses, cols (2026-08-24)

**Architecture retenue (leçon de quatre échecs de continuité
successifs, doctest à l'appui)** : tout est fonction lisse et
INVARIANTE PAR TRANSLATION de (x, z) — un `ValleyField` : potentiel φ
(fbm 2 oct, `valleyAxisWavelength` 9 km — n.b. le champ porte le
double rôle rythme/orientation), axe local = perpendiculaire de ∇φ,
force = band-pass sur |∇φ| normalisé (0,15-0,4 / 0,9-1,4).
- **Vallées maîtresses = strates d'isolignes de φ** : continues par
  construction, elles serpentent avec le champ ; l'index de strate est
  l'identité GLOBALE de la vallée (un hash règle profondeur/phase de
  bout en bout). Masques mesurés EN UNITÉS DE PHASE (la conversion en
  mètres par 1/|∇φ| cisaille aux inflexions) ; distance au plus proche
  des trois centres candidats (accord des deux côtés d'une frontière
  de strate) ; gate inland PROPRE à rampe large (~1 km — le `inland`
  partagé claque en dizaines de mètres au trait de côte warpé : c'était
  LA déchirure finale). Profondeur 40-80 m creusée dans `landHeight`
  avec fondu sous 90 m d'altitude (pas de fosse sous le niveau de
  mer) ; fond → `trunk` [0,1] exporté (ControlSample, MacroResult,
  TileStage1, sidecar TS16) → gentle ≥ 0,9·trunk, calm suit,
  reliefScale ×(1−0,6·trunk).
- **Étirement anisotrope = warp AXIAL local** dans `landHeight`
  (déplacement le long de l'axe, ampli ∝ (valleyStretch−1)) — la
  formulation en repère tourné/contracté est mathématiquement fausse
  dans un monde infini (θ variable × rayon = balayage de coordonnées).
- **Cols garantis = strates fines d'un second potentiel ψ**
  (~colSpacing), gatées `rangeNeed` — statistique, plus de peigne en
  repère.

Échecs documentés pour la suite : (1) repère (u,v) global tourné →
déchirure ∝ distance à l'origine ; (2) distance métrique via 1/|∇φ| →
cisaillement aux inflexions ; (3) hash par strate asymétrique aux
frontières ; (4) gate inland partagé trop raide. Le doctest « valley
axis and trunk valleys » (continuité < 0,3 par pas de 20 m sur 61 km
de lignes, bornes, sur-ensemble gentle, déterminisme) pince tout ça.

Mesures : fonds de tronc 4,8 % des terres / creusé 10,5 % (léger —
élargissement en B6 via trunkSpacing/floorHalfWidth) ; transect :
pentes > 30° en baisse nette (E-O 20→15 %, drame 15,6→12,5 %),
médianes 49,5→46 / 31,9 ; vista : amer > 2° au-delà de 3 km **5/5**
points (4/5 avant). **Régression assumée** : « infranchissable
continu » 1-D remonte à 875/700 m (crêtes allongées = murs plus longs
sur une ligne droite ; les cols tous les 4-5 km ne sont pas sur la
ligne) — recalibrage B6 (colSpacing, largeur de strate ψ, seuil
rangeNeed). Caches v34/v39, sidecar TS16.

### B5 — Étage 0 : réseau hydrologique maître (2026-08-24)

`MasterNetwork.{hpp,cpp}` : routage grossier PUR par super-région
(24,6 km + apron 8,2 km, texel 128 m) sur `macroHeightAnalytic` —
`priorityFloodFill` + `routeFlow` réutilisés tels quels — extraction
des cours dont l'aire de drainage VRAIE dépasse `fleuveArea`.
Propriété par cellule-mère de la TÊTE (deux appelants bit-identiques),
continuation tracée jusqu'à ~8 km au-delà (la couture inter-super
complète reste le différé connu). Aucun état, sûr sur les workers ;
`masterRiversNear(aabb)` pour S4/scoring/chemins. Doctests :
déterminisme bit-exact, surfaces monotones aval, têtes dans le cœur,
accord entre deux requêtes ; diagnostic caché `master network`.
AUCUN changement de bake par le module lui-même ; en revanche le
band-pass de force des vallées B4 a été rouvert (0,08-0,2 / 1,0-1,5 —
les masques en phase n'ont plus le cisaillement qui l'avait fait
resserrer) : fonds 5,8 %, creusé 11,7 %, continuité 0,289 < 0,3 ✓.
Caches v35/v40.

**CONSTAT DE CONCEPTION (mesuré, arbitrage dev demandé)** : dans la
macro ACTUELLE (mer 29 %, continent λ 4 km), les bassins versants
sont courts et côtiers — en ±24 km : 51 systèmes atteignant la mer
(un par ~7 km — deux fois la densité cible), plus long cours 10,9 km,
et 4,5 % seulement des nœuds sur les corridors trunk (les dépressions
de 40-80 m sur ~6 % de couverture ne captent pas le drainage). **Le
« fleuve-frontière intérieure tous les 10-15 km » n'émerge pas de
cette macro**, quel que soit le seuil d'aire. Trois options :
(a) fleuves CÔTIERS assumés — larges (≥ 20 m de demi-largeur),
courts, un estuaire majeur par ~10 km de côte ; la structure de
voyage intérieure reste les vallées maîtresses (sans fleuve dedans) ;
(b) retune macro pour de vrais bassins longs (continentWavelength ↑,
seaThreshold ↓, trunks plus profonds/couvrants) — grosse conséquence
visuelle, à faire en B6 avec validation en jeu ;
(c) hybride : (a) court terme + retune (b) mesuré en B6.

### Expérience « porteuse continentale » (2026-08-24, cartes à l'appui)

Outil `cooker terrain-map` livré (MapExport : hypsométrie + hillshade
+ réseau maître + croix centrale), cartes 100 km comparées dans
`terrain-maps/`. La carte de l'état actuel confirme le diagnostic à
l'échelle macro : **tapis d'archipel statistiquement uniforme**, zéro
région nommable. Expérience (knobs `continentCarrierWavelength/Amp`
de ProceduralControlParams, **défaut 0 = OFF, monde bit-identique**,
aucun bump) : une porteuse très lente ADDITIVE sur la continentalité.
Trois leçons d'itération :
1. additive brute → les continents se forment mais criblés de lacs
   (le fbm local plonge sous le seuil partout) ;
2. compression du détail local gatée par |lift| → inopérante : le fbm
   porteur traîne autour de sa médiane, presque tout le monde reste
   dans la bande molle ;
3. **remap contrasté du porteur (smoothstep 0,38-0,62) + compression
   ×0,6 du détail dans les zones tranchées** → continents FRANCS aux
   côtes articulées, océans ouverts avec îles, ceinture côtière qui
   garde tout le caractère local — et surtout le réseau maître se met
   à produire des cours de 10-30 km dendritiques avec confluences :
   **les fleuves de la cible émergent d'eux-mêmes** de cette macro.
Variantes générées : 40 km/0,30 (un continent + un océan + îles par
fenêtre de 100 km — recommandée : garde des côtes riches pour les
ports) ; 60 km/0,35 (quasi tout continent). Adoption = décision dev ;
si adoptée : activation dans les params par défaut, bump caches,
re-calibration B6 sur la nouvelle macro (proportions/spawn/fit).
