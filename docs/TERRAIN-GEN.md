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
