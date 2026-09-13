# CPU-PERF — coûts CPU des bakes workers (chantier ÉCONOMIE)

> **CHANTIER CLOS (2026-09-10 → 13, décision dev).** Toutes les phases
> livrées : E0 instrumentation, E1 dédups bit-exact, E2 hygiène des
> triggers (E2.4b reconcile déclarée non nécessaire — résiduel 11-33 ms
> par publish non ressenti en jeu, pistes documentées à l'entrée E2.4),
> E3 pyramide `render::HeightField`, E4 cas isolés. Résultats de fin :
> lightmap atterrit ~10 s après boot et suit la marche (vs jamais,
> 131-250 s/bake) ; coût de marche −45 %/distance ; rcTile ÷4-5 ;
> publish sans copie de régions (evict 12-28 ms → 0,1 ms) ; fermeture
> < 1 s. Reste ouvert : la grande session F6 comme mesure de vérité.

> Pendant du modèle GPU-PERF : **mesurer d'abord, corriger dans l'ordre mesuré.**
> L'outillage est `core::JobProbe` (table F6 « Worker jobs » + ligne one-shot
> `cpu bakes (window)` au frame 2000 + `publish breakdown` par publish de tuile).
> Le plan du chantier (phases E0-E4, briques, preuves) vit hors dépôt ; l'inventaire
> d'origine et l'architecture HeightField y sont documentés. Ce fichier tient la
> **baseline** et les chiffres avant/après de chaque brique livrée.

## Baseline — marche standard du dev (2026-09-11, avant toute optimisation)

Fenêtre : la marche standard (~60 s) sur terrain pré-baké, `cpu-perf-base.png`.

| job | runs | avg (ms) | max | total (ms) | part |
|---|---|---|---|---|---|
| **rcTile** | **209** | **161.5** | 244.5 | **33 756** | **55 %** |
| grassScatter | 399 | 20.3 | 50.8 | 8 116 | 13 % |
| waterSim | 2962 | 2.0 | 19.3 | 5 889 | 10 % |
| vegScatter | 1425 | 3.9 | 14.7 | 5 606 | 9 % |
| farTerrain | 2 | 1469.7 | 1630.5 | 2 939 | 5 % |
| occlusionHorizon | 442 | 5.7 | 14.0 | 2 537 | 4 % |
| terrainMesh | 9525 | 0.2 | 5.7 | 2 345 | 4 % |
| terrainShadeMap | 5 | 66.0 | 69.7 | 330 | <1 % |
| waterPoolMap | 7 | 28.8 | 43.9 | 201 | <1 % |
| mistMap | 7 | 6.8 | 7.1 | 48 | <1 % |
| waterLocalMesh | 1 | 41.0 | 41.0 | 41 | <1 % |
| farWater | 2 | 1.7 | 1.9 | 3 | <1 % |
| **TOTAL** | | | | **61 800** | |

Lectures :

- **Avancer coûte plusieurs secondes de CPU par seconde de marche** ; à l'arrêt il ne
  reste que waterSim (~130 ms/s) et la lightmap par pas de soleil. Le compteur
  enregistre à l'atterrissage : un bake en vol ne compte pas encore.
- **rcTile domine tout** : rebake tous les 16 m de marche (span 160 m × 0.10),
  209 fois dans la fenêtre. Cible n°1 (hystérésis + coût par bake).
- **`terrainLightMap` absent de la fenêtre** : son bake (~2 min de worker) était en
  vol pendant toute la marche — il occupe un worker en quasi-permanence dès que
  l'heure du jour avance (rebake par pas de soleil ~8 s).
- Sur terrain bakée, le chemin `height()` rapide (~40-60 ns) rend mist/pool/shade
  quasi gratuits ; les avg « à froid » hors tuiles sont 10-60× plus chers
  (spawn : mistMap 404 ms, rcTile 1686 ms).
- `tileBake.stage1/.finalize` absents : marche sur cache disque plein — le
  pré-bake (`cooker pre-bake`) tient sa promesse.

## Références de non-régression

- Hash golden scatter : `10031847806692189656` (VegetationScatterTest).
- Banc tuile (`-tc="*bake benchmark*" -ns -s`, seed 1337) : **22,5 s**,
  `region.heights` hash **`4414106998705828656`** — toute brique qui bouge ce
  hash a changé le terrain.

## Mesure post-E1.2/E1.3/E2.1a — marche 2 (2026-09-11, cpu-perf-1.png)

Total 87,7 s MAIS ~1,9× la distance de la baseline (odomètre = occlusionHorizon,
848 vs 442 runs) + sortie de la zone pré-bakée + un lac interrompu. Normalisé :

- **rcTile : 168 runs à 71,1 ms = 11,9 s** vs 33,8 s baseline → **÷2,4 par mètre**
  (l'hystérésis 0,25) **et** avg ÷2,3 → **÷5,4 de coût par distance**.
- **grassScatter : 16,6 ms/chunk** vs 20,3 (**−18 %**, E1.3).
- tileBake.stage1 4×2,36 s + finalize 2×2,28 s = **14 s de terrain FRAIS**
  (hors zone pré-bakée — absent de la baseline, coût légitime).
- waterSim 8023×2,7 = 21,8 s (×3,7) : ~6 publishes de tuiles → refreshTerrain
  (66k height) + re-pin des lacs à chaque fois ; amplifié par un « lac
  interrompu » rencontré en chemin (observation à creuser côté EAU).
- Hors tuiles fraîches : ~52 s pour 1,9× la distance ≈ **coût de marche ÷2,3**
  après trois briques.

Méthode : les A/B suivants se font sur LE MÊME trajet en zone pré-bakée ;
l'odomètre occlusion (runs × 8 m) sert de normalisation quand le trajet dévie.

## Marche 3 — E1.2+E1.3+E1.4+E2.1a, avant E1.5 (cpu-perf-2.png)

54,5 s, odomètre 606 (×1,37), terrain pré-baké (zéro tileBake) →
**39,8 s par distance-baseline, −36 % vs 61,8 s.** rcTile ÷2,6 par distance
(90 vs 209 normalisé) — l'hystérésis 0,25 tient sa prédiction. Les runs par
distance des triggers inchangés collent (grass 404≈399, veg 1442≈1425,
mesh 9473≈9525) — l'odomètre est fiable. Leçons de mesure : les **avg
unitaires sont bruités inter-sessions** (rcTile 161→71→145 : contention
workers + position — bandes de recouvrement 2×, rims analytiques) ; seuls
runs/distance et total/distance font foi. L'effet E1.3 est illisible ici
(biome quasi sans herbe, 6k brins à l'écran vs 139k baseline).

## Marche 4 — + E1.5/E1.6/E1.7/E2.2 (2026-09-11, cpu-perf-3.png)

48,2 s ; odomètre : terrainMesh 13509 / vegScatter 2057 runs = ×1,04 vs
marche 3, ×1,42 vs baseline (l'occlusion ne sert plus d'odomètre : ×24 m/run
depuis E2.2). Normalisé : **33,9 s par distance-baseline → −45 % cumulés.**
occlusionHorizon **÷3,0** (E2.2, avg stable 5,7), farTerrain **−33 %/bake**
(E1.7 : 632 vs 949 ms), vegScatter −11 % (E1.5), grass −6 %, rcTile avg −10 %.
Dominants restants : rcTile 16,0 s (33 %), grass 10,6 s, veg 8,0 s,
waterSim 5,5 s — la suite : E2.1c (atterrissage RC), E2.3 (scoping), E3.

## Session longue — première mesure directe de la lightmap (2026-09-12)

Session de ~25 km de vol (odomètre occlusion 1064 × 24 m), 421,7 s de worker
(`lac-interrompu.png`) : **terrainLightMap = 2 bakes, avg 131 s, max 251 s —
262 s, 62 % du total de la session**, en n'atterrissant que deux fois (la
carte court après le soleil en permanence ; en zone fraîche ses marches
solaires payent l'analytique hors tuiles). Confirme E3.7 (grille partagée)
comme LE gros gain restant. Le reste : waterSim 84 s (21,7k ticks),
grass 15,2 s, rcTile 14,9 s (199 runs — hystérésis en place),
tileBake 14 s (4 stage-1 + 2 finalize, zone fraîche), veg 11,6 s.

## Régime « vol rapide » (2026-09-12, cpu-perf-4.png)

68,1 s de worker en vol spectateur rapide. Le régime change de dominants :
**churn des anneaux = 30 s** (veg 12,6 + grass 15,1 + mesh 2,5 — des chunks
scatterés puis évincés sans avoir été vus) devant rcTile 17 s ; les cartes
caméra-centrées s'enchérissent à l'unité en dépassant le prefetch (mist
6,8→28,6 ms, pool →66,8, farWater →184,8 avg / 1,1 s max : texels hors
régions → chemin analytique) — argument de plus pour la grille E3.
**L'odomètre occlusion casse à haute vitesse** (cadence bornée par la latence
du job, 265 runs) : en vol, étalon = runs vegScatter/terrainMesh. Zéro
tileBake : les hits du cache disque ne sont pas chronométrés (hors scopes).
Brique dérivée : E2.7 « scatter hold en vol » (seuil de vitesse → suspendre
les requêtes grass/veg via le holdRequests existant).

## Briques livrées

- **E1.2 — dédup continentalité** (2026-09-11) : `ProceduralControls::at(x, z,
  f32& outContinentalness)` réutilisée par `macroHeightAnalytic` — une seule
  évaluation de la continentalité (warp + layout kernels, ~un quart de l'appel
  analytique) au lieu de deux. Bit-exact prouvé : doctest champ-à-champ +
  `c == continentalness()` (TerrainGenTest), 703/703, hash golden inchangé,
  miroir analytique (« erosion calibration ») vert. Gain attendu ~25 % sur tout
  le chemin analytique (FarTerrain hors tuiles, rims des cartes, occlusion
  lointaine, macro des bakes) — à lire sur la table F6 des prochaines marches.
- **E1.3 — regionFieldsAt dédupliqué dans la boucle de cellules grass**
  (2026-09-11) : overload `materialWeightsAt(..., const RegionFields&)` ; la
  cellule échantillonne ses fields UNE fois (5 biomeBlended) pour les poids ET
  les scores d'espèces. Portée réelle : GrassSystem seul — les sites scatter
  pointés par l'audit étaient dans des boucles différentes (pas le même point).
  703/703, hash golden inchangé. Attendu : grassScatter (20,3 ms/chunk baseline)
  en baisse sensible sur cellules acceptées.
- **E2.1a — hystérésis de la tuile RC** (2026-09-11) : le trigger 16 m
  (`span × 0.10` codé en dur) devient le knob GI « GI tile rebake drift »
  (défaut 0,25 → 40 m, bornes 0,05-0,5, persisté via RcTuningForm §5).
  L'ancienne tuile continue de s'appliquer pendant le bake — le seul coût est
  un décalage du sol GI de N mètres. Attendu sur la marche baseline :
  rcTile 209 → ~84 rebakes (33,8 s → ~13,5 s). **Validation dev EN ATTENTE** :
  A/B en marchant (offset GI perceptible ? sinon pousser vers 0,3-0,4).
- **E1.4 — index de régions sur lattice 4096 m** (2026-09-11) :
  `TerrainBase::cellIndex` (cellule → indices ASCENDANTS des régions
  intersectantes ; ordre ascendant = ordre du blend, load-bearing pour la
  bit-exactitude de l'accumulation) ; `height()` et `regionAt()` passent au
  bucket O(1-4) au lieu du scan O(25) ; index vide ⇒ scan legacy (tests/outils
  inchangés) ; construit par `buildTerrainBase` et `publishBakedTiles` (base
  publiée immutable → jamais stale). Preuves : doctest 5×5 régions à rims
  souples dense indexé-vs-linéaire EXACT, 704/704, golden inchangé, hash banc
  tuile identique. Gain : facteur constant sur TOUT height()/regionAt en
  région — chemin golden compris (mesh, scatter, grass, collision, nav, sim).
- **E1.5 — sous-ensemble d'eau par chunk** (2026-09-11) :
  `terrain::waterBodiesInRect` (indices ASCENDANTS des corps dont la bbox
  touche le rect) + overloads `waterSurfaceAt`/`waterDepthAt`/
  `underLocalWater` sur sous-ensemble ; les bakes scatter (5 sites) et grass
  construisent le sous-ensemble une fois par chunk (rect ±8 m) au lieu de
  payer les ~158 tests bbox résidents (et les marches de segments des
  rivières à grande bbox) par candidat. Bit-exact : un corps exclu échoue son
  propre test bbox pour tout point du rect ; ordre relatif préservé. Preuves :
  doctest subset-vs-full dense EXACT, 705/705, golden inchangé, hash banc
  identique, smoke run 0 erreur. Attendu : vegScatter/grassScatter en baisse
  surtout près des réseaux de rivières.
- **E1.6 — probe de flow par chunk dans le mesher** (2026-09-11) :
  `terrain::flowMaskTouches(rect)` — un scan du sous-rect de masque (+1 texel
  de support bilinéaire) une fois par chunk ; les chunks sans flow (l'immense
  majorité) appellent `height()` direct au lieu de payer `regionAt` +
  `maskSample` par vertex dans `meshHeight`. Bit-exact : tous les texels de
  support < 0,5 ⇒ toute lecture bilinéaire < 0,5 ⇒ le gate ne tire jamais.
  Correction d'audit : le 9-tap ne frappait déjà QUE les vertex sur le canal
  (gate par vertex existant) — le gain réel est la suppression du lookup par
  vertex, pas du 9-tap massif supposé. Preuves : doctest identité (pente,
  région à flow partiel, contrôle positif « le gate tire bien sur le canal »),
  706/706, golden inchangé, hash banc identique, smoke 0 erreur.
- **E1.7 — impostors FarTerrain sur la demi-grille [VISUEL, A/B]**
  (2026-09-11) : les candidats d'arbres lointains (270k/bake, survivants ×5
  height() analytiques chacun) prennent hauteur bilinéaire + pente par
  différences centrales sur la demi-grille 35 m que le bake possède déjà —
  la bande d'impostors (±5,2 km) tient entière dans son span (±9 km).
  PAS bit-exact : placements décalés sub-texel à >1 km, les gates (mer,
  slope 0,3, treeline) basculent sur les cas limites. Toggle « Far impostors
  from grid » (panel Rendering, défaut ON, persisté LandscapeTuningForm) ;
  le flip re-bake immédiatement (état bakedFromGrid). Aussi : treeLine
  hoisté hors de la boucle candidats. 706/706, golden inchangé (système
  distinct du scatter), smoke 0 erreur. **VALIDATION VISUELLE DEV EN
  ATTENTE** : vue dégagée sur l'horizon, toggle A/B — la frange de forêt
  lointaine doit garder sa silhouette/densité.
- **E2.2 — hystérésis occlusion 8 → 24 m [knob]** (2026-09-11) :
  `ChunkOcclusion::rebuildDistance` (la constante 8 m devient un slider
  « Occlusion rebake drift » 8-64 m, panel Culling & debug, persisté
  LandscapeTuningForm). Les verdicts restent conservatifs PAR POSITION ; le
  drift ne fait que les vieillir — une crête franchie au-delà de
  l'hystérésis peut popper tard. Attendu : occlusionHorizon ÷3 en fréquence
  (442-848 runs/marche → ~150-280). 706/706, golden OK, smoke 0 erreur.
  **VALIDATION DEV** : marcher des lignes de crête, vérifier l'absence de
  pop-in ; sinon redescendre le slider.
- **E2.1b + E2.5a — settle des knobs (SettleTimer)** (2026-09-11) :
  `core::SettleTimer<T>` (débounceur générique, 0,4 s de calme) ; le tint GI
  et les knobs grass (splatUvScale/tint/rootAlbedo ×4) ne déclenchent plus
  leur rebake (tuile RC 65k texels / re-scatter des 49 chunks) qu'UNE fois
  par drag au lieu d'une par tick. 706/706, golden OK.
- **E2.1c — atterrissage RC sans marteau** (2026-09-11) :
  `rebuildTileGroups` ne rebinde que les groups référençant les textures de
  tuile (build par niveau, binding 8 + inject 8/9/12) au lieu de
  `appliedResolution = 0` (recréation de TOUS les volumes de cascades sur le
  main thread). Bonus visuel attendu : la radiance accumulée SURVIT à
  l'atterrissage — fini la re-convergence du GI à chaque pas de tuile de
  40 m. Risque : bind groups, deux backends — **VALIDATION DEV** : marcher
  en regardant le GI aux atterrissages (ligne F6 frame-max), vérifier
  l'absence de flash/reset. 706/706, golden OK.
- **E2.3 — scoping spatial du contentStamp** (2026-09-12) :
  `TerrainParams::contentEvents` (ring borné de 32 `{stamp, rect}`, poussé à
  CHAQUE bump — publish : rects publiés ET évincés ; bascule de mode : rect
  monde ; contrat documenté dans le header) +
  `terrain::contentTouchedSince(params, seen, rect)` (conservatif au
  débordement du ring via `completeFrom`). Consommateurs scopés :
  TerrainShadeMap (3 km), minimap (rect baké), pool map (le stamp fusionné
  bodies+content est séparé en deux lanes), refresh du sol de la sim
  (`simWindowIntersects`, fenêtre 512 m) et `invalidateOcclusion` (carré
  caméra du reach). FarTerrain/far-water restent sur le stamp brut (18 km —
  leur fix = coalescing, brique E2.3b à venir). Effet : un publish de tuile
  à 4 km ne rebake plus pool/minimap/shade/sim/occlusion. Preuves : doctest
  scoping + débordement conservatif, 707/707, golden inchangé, hash banc
  identique, smoke 0 erreur (publish breakdown intact).
- **E2.3b — coalescing des géants de 18 km** (2026-09-12) : FarTerrain et le
  far-water attendent **1 s de calme du contentStamp** avant leur rebake au
  stamp (les triggers stray/seed/mer/knobs restent immédiats) — une rafale de
  N publishes coûte UN bake de chaque au lieu de N (le far-water rescanne le
  cache disque à chaque collect : c'était N scans). Motif wall-clock inline
  (lastSeenStamp + quietSince), partagé entre les deux sites. Reste en
  backlog : le far-water incrémental (liste alimentée par les événements du
  streamer au lieu du rescan). 707/707, golden OK, smoke 0 erreur.
  Diagnostic du creux eau posé au passage : log « revealed CALM/CAP » à
  chaque révélation — au spawn : CALM à 2,4 s (proche du plafond 3 s).
- **E3.1 — le composant `render::HeightField`** (2026-09-12) : pyramide
  caméra-centrée 3 niveaux (L0 4 m/4 km, L1 16 m/8 km, L2 64 m/32 km),
  remplie sur workers (mailbox par niveau, origines snappées au lattice,
  stop-flag par ligne), publiée en snapshots immutables (contrat
  TerrainBase : swap sptr sur main, lectures lock-free). Politique de
  remplissage : sol couvert/story = `height()` exact (chemin baké rapide) ;
  sandbox non couvert = upsample bilinéaire du niveau plus grossier capturé —
  seul L2 paye la pile analytique. `snapshot.height()` retombe sur la
  fonction exacte hors de tout niveau (jamais faux, au pire lent).
  Invalidation par `contentTouchedSince` (rect du niveau) — **E3.2 (révisions
  de régions) rendue inutile par E2.3, absorbée**. Toggle maître « Shared
  height field » (persisté). Aucun consommateur converti (no-op visuel).
  Coûts mesurés au spawn : L0 1,86 s, L1 0,56 s, L2 0,89 s par fill.
  Preuves : 3 doctests (exact aux centres de texels, borne bilinéaire,
  fallback exact, sélection coarse, refill scopé par événement), 710/710,
  golden inchangé, smoke 0 erreur.
- **E3.3 — occlusion sur la grille partagée** (2026-09-12) : les rayons
  d'horizon lisent `heightCoarse(…, 16 m)` (L1 couvre tout le reach) quand un
  snapshot est fourni ; `Input.field` nul = chemin exact inchangé (doctest
  headless intact, toggle maître off). Direction conservative préservée : la
  grille lisse les pics vers le BAS → l'horizon ne peut que baisser → on
  dessine plus, jamais moins. Le gain frappe surtout HORS zone pré-bakée où
  chaque échantillon payait l'analytique (~20-45 ms/rebake mesurés à froid).
  710/710, golden OK, smoke 0 erreur. **VALIDATION DEV** : lignes de crête,
  A/B « Shared height field », pas de sur-cull attendu (l'inverse est
  géométriquement impossible).
- **E3.4 — mist + pool map + minimap sur la grille** (2026-09-12) :
  `MistMap::update`, `WaterSystem::update` et `MiniMapPanel::draw` prennent
  le snapshot en option (null = chemin exact — MapController/carte du monde
  et tests restent exacts) ; les bakes lisent `field->height()` (fallback
  interne du snapshot pour le non-couvert). Non-régression au smoke : la
  ligne « mist map baked » est identique (588.0 m). NB : le premier bake du
  boot part souvent avant le premier fill de la pyramide (course bénigne —
  chemin exact) ; les REBAKES en marche prennent la grille. Attendu : les
  avg « à froid » (mist 288→~10 ms, pool 172→~15 ms, minimap) s'effondrent
  hors zone pré-bakée. 710/710, golden OK, smoke 0 erreur. **VALIDATION
  DEV** : brume au fond des vallées + assombrissement des hauts-fonds +
  minimap, A/B « Shared height field ».
- **E3.5 — FarTerrain sur la grille** (2026-09-12) : la demi-grille 513²
  (263k échantillons sur 18 km, l'essentiel en analytique) lit
  `heightCoarse(…, 16 m)` (L1 où il couvre, L2 au-delà) ; les impostors
  E1.7 lisent la demi-grille et en héritent. Null = chemin exact. Au smoke :
  farTerrain 528 ms au spawn vs 875-985 avant (premier bake encore en course
  avec le premier fill). 710/710, golden OK. **VALIDATION DEV** : silhouettes
  d'horizon + frange de forêt, A/B « Shared height field ».
- **E3.7 — LA LIGHTMAP SUR LA GRILLE (le payoff de l'axe)** (2026-09-12) :
  les ~89 échantillons par texel (h0, marche solaire 24 pas, ciel 8×8)
  lisent la pyramide — h0/ciel au plus fin, marche solaire sur
  `heightCoarse(max(4, 0.03·t))` (empreinte qui grandit avec la distance :
  L0 près, L1/L2 loin). Texels 3 m et nombre de pas INCHANGÉS (tuning
  anti-banding). Le premier kick attend le premier snapshot (~2 s de boot)
  — sinon il partait en chemin exact pour 2 min. Log « terrain light map
  baked » ajouté (la carte était invisible). **Mesure : atterrissage ~10 s
  après le boot, contre JAMAIS en 25 km de session avant (bakes de
  131-250 s)** — la carte suit enfin le pas de soleil de ~8 s. Header
  corrigé (kSize²/kSpan au lieu du 512²/1,5 km périmé). 710/710, golden OK.
  **VALIDATION DEV** : pentes au couchant/levant — les fronts d'ombre
  longue doivent ramper sans banding (le scénario documenté du texel 3 m) ;
  A/B « Shared height field ».
- **E3.8 — normales RC à stencil large [EXPÉRIENCE, défaut OFF]**
  (2026-09-13) : checkbox « GI tile grid normals (exp) » (panel GI,
  persisté) — les normales de la tuile viennent d'un stencil ±2 texels sur
  une grille à apron (lisse inter-facettes, le remède candidat au banding
  mesuré des différences par facette) au lieu de 4 height() analytiques par
  texel (4/5 de l'échantillonnage de la tuile ; ON = rcTile ~÷4). Le flip
  re-bake immédiatement. À N'ADOPTER (défaut ON) qu'après validation du
  bounce au soleil rasant. 710/710, golden OK, smoke OK.
- **Banding GI résiduel — diagnostic dev (2026-09-13)** : un léger banding
  rare au soleil rasant, INSENSIBLE au toggle maître « Shared height field »
  ET au toggle grid normals, disparaît en éclairage classique sans GI →
  inhérent au chemin Radiance Cascades (antérieur au chantier, très allégé
  vs les débuts de RC ; candidats structurels : normales/albedo de tuile en
  8 bits, résolution angulaire des cascades). ACCEPTÉ par le dev — résiduel
  connu, hors chantier économie. Corollaire : grid normals ON/OFF
  indistinguables ⇒ E3.8 passe son A/B (adoption par défaut = décision dev).
- **E3.8 adoptée par défaut (décision dev 2026-09-13)** : grid normals
  ON/OFF indistinguables à l'A/B (le banding résiduel vient du chemin RC
  lui-même, voir diagnostic) → défaut ON (RcTuning + Form ; OFF = chemin
  analytique de référence). Mesure spawn : rcTile 283 ms vs 1204-1686.
  NB : un mods/render-tuning.toml sauvé avec l'ancienne valeur primerait
  (couches §5) — recocher + re-save le cas échéant.
- **E4.1 — navigator : mémo + index** (2026-09-13) : (a) mémo par requête
  du lazy-grid 1 m — chaque cellule payait le callback hauteur (pile
  complète, MAIN thread) ~9× (nœud + voisin de 8) ; valeurs identiques par
  pureté, ~÷9 d'évaluations (180k → ~20k pire cas) ; (b) `blocked()` passe
  d'un scan de toutes les boxes par voisin à un index par cellules de 16 m
  (construit dans setBlockingBoxes, une box s'enregistre dans chaque
  cellule touchée). Doctest : boxes à cheval sur les frontières de
  cellules + coordonnées négatives bloquent toujours. 711/711, golden OK,
  smoke 0 erreur.
- **E4.2 — VegetationCollision sur worker** (2026-09-13) : le
  `scatterProps` (~1400 height/chunk) quittait le MAIN thread — pattern
  TerrainCollision (§2.11) : le worker cuit la liste compacte de colliders
  (troncs/rochers/débris, probe « vegCollision »), le main ne fait que les
  `addStaticBox` Jolt (règle Phase-5 : le worker ne touche ni monde ni
  physique). Pureté du scatter ⇒ résultat identique ; chunk évincé en vol
  droppé à l'arrivée (gardes pending/chunks). Fallback synchrone sans
  JobSystem (tests headless). 711/711, golden OK, smoke 0 erreur.
  Validation dev : les troncs/rochers bloquent toujours en marchant.
- **E2.5b — tint de l'herbe sur lattice grossier** (2026-09-13) :
  `scatterGrass` payait un `regionShadingAt` (fbm3 « banni des hot
  paths » par son propre header) par coin de 0,6 m — 109² ≈ 11 880
  évals/chunk — pour un tint macro qui dérive sur ~700 m. Désormais un
  sous-lattice ~8 m (10² ≈ 100 évals) est bilerpé aux coins ;
  base/grassBlotch/grassZoneAt restent par coin (le raccord splat tient).
  Toggle `Coarse tint lattice` dans Grass → Scatter (re-scatter au flip,
  OFF = chemin exact de référence), persisté
  (`LandscapeTuningForm.grassCoarseTint`, défaut ON). Smoke au spawn :
  grassScatter 49× avg 18 ms vs 23-25 ms (−25 % — le tint n'était qu'une
  part du coût des coins, les brins dominent). Pas de golden grass : la
  validation est visuelle (cohérence de teinte herbe/sol sur les dérives
  longues, A/B au couchant).
- **E2.4 — pic main-thread du publish, ordonnée par la ventilation dev**
  (2026-09-13, logs d'une marche fraîche jusqu'après le lac interrompu) :
  mesures = evict 12-28 ms À CHAQUE publish, reconcile 0,7-16,7 ms,
  collision 0,1-0,2 ms, dedupe ≤0,6 ms (mais 28 ms une fois à 213 lacs
  résidents), republish ≤1,6 ms. Décisions :
  - **(c) LIVRÉE — régions partagées-immutables.** Le bucket « evict »
    était en réalité la copie profonde `next->regions =
    terrainBase->regions` (~25 régions × ~18 Mo) à chaque publish.
    `TerrainBase::regions` devient `vector<sptr<const TerrainRegion>>`
    (personne ne mute une région publiée : le sculpt passe par les
    HeightPatches, un re-bake remplace le handle) — la copie de publish
    coûte ~25 refcounts. **Confirmé en jeu : evict 0,0-0,1 ms partout**
    (session dev 18:36, « je n'ai pas eu de hard bake »). 711/711,
    golden OK.
  - **(a) SKIP mesurée** : collision destroy+recreate = 0,1-0,2 ms
    depuis E4.2 — le risque des updates incrémentales Jolt ne vaut
    rien.
  - **(d) SKIP mesurée** (0,1-0,6 ms typique) ; NB : peut piquer à
    ~28 ms au-delà de ~200 lacs résidents — à surveiller.
  - **(b) reconcile = le résiduel dominant** (11,9-33,3 ms sur les
    publishes multi-tuiles du build (c)) : le coût est le
    `terrain::height()` par cellule humide des masques de lacs frais
    (plancher 2 m intouchable). Pistes dessinées si le dev sent les
    pics : scan borné à l'intersection lac∩région (ne gagne que sur
    les lacs de bordure), ou déport worker post-publish (les lakes
    frais s'affichent 1-2 frames avant re-validation). Republish 11 ms
    une fois (rebuild WaterBodies à ~180 lacs) = même famille.
- **E4.3 — outil éditeur via streamer** (2026-09-13) : « Bake region
  here » appelait `bakeTile()` brut — 9 stage-1 recalculés inline à
  chaque clic, aucun cache, pas d'annulation. L'outil possède désormais
  son propre `TerrainBakeStreamer` (cache `terrain-cache/gen_<seed>_
  <taille>` — le répertoire encode ce que les noms de fichiers ne
  portent pas) : registry stage-1 partagé entre bakes voisins, 2e clic
  = lecture cache (.trg + sidecar eau, tuile identique par
  déterminisme), annulation shutdown et probes F6 hérités. Le code de
  bake ad hoc de l'outil est supprimé (§2.11).
- **E3.9 — clôture de l'axe** : note d'orientation ajoutée à RENDERING.md
  §4.1 (JobProbe + HeightField → ce doc). L'AXE E3 EST CLOS : lightmap
  ~10 s après boot (vs jamais), occlusion/mist/pool/minimap/farTerrain sur
  la grille, RC en expérience.
- **E3.6 — déjà couverte par E2.3** (trigger ShadeMap scopé ; son bake
  n'appelle pas height()).
- **E2.6 — SKIP vérifié** : le trigger arbres du FarTerrain se base sur une
  silhouette MESURÉE déterministe (mêmes params ⇒ même hauteur ⇒ pas de
  retrigger, seuil 0,5 m) ; un reseed retuné DOIT re-baker la frange
  (design documenté). L'item d'audit était faux.
- **E2.7 — REJETÉE dev** : pas de scatter-hold en vol — la vitesse
  spectateur n'existe pas en jeu ; le churn d'anneaux en vol est accepté.
- **E1.7 (validée dev 2026-09-11)** : la frange lointaine passe l'A/B — le
  toggle reste ON par défaut.
- **E1.1 — copie ProceduralControls : SKIP mesuré.** La « copie ~55 floats »
  par appel pèse ~220 octets de stack (<1 % d'un appel analytique) ; l'éviter
  demanderait un membre-référence (piège de lifetime) ou un cache d'état.
  Non rentable — décision : ne rien faire.
