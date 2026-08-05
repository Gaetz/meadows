# GRASS-REDO — sol réaliste : herbe variée & transition herbe/roche

> Chantier du 2026-08-03 (branche `feature/realistic-textures`), suite du
> chantier TERRAIN-TEXTURING. Question du dev : « peut-on atteindre un
> rendu triple-A du sol à coût réduit ? » (référence : Crimson Desert —
> cailloux, feuilles, terre, herbes variées qui changent vite au sol).
> C'est LE « visual redo » de l'herbe que `docs/RENDERING.md` réservait.

## L'étude (sources primaires)

Constat transversal des AAA analysés :

1. **La profondeur perçue du sol vient de la géométrie instanciée, pas du
   shader de texture.** Call of Duty n'a pas de POM terrain (Etienne,
   SIGGRAPH 2023, lu intégralement) ; Ghost of Tsushima et Horizon font le
   relief par brins/touffes/clutter massifs ; Battlefront par des meshes
   dont la base échantillonne le matériau du terrain. Le POM est un
   condiment.
2. **Espèces = paramètres + clumping, pas un nouveau système** (GoT,
   GDC 2021 : un seul générateur de brins, l'espèce est une donnée au sol,
   clumps Voronoï partageant hauteur/couleur/inclinaison ; ~83 k brins ≈
   2,5 ms sur PS4).
3. **La transition herbe→roche n'est jamais binaire** : densité des brins
   = f(poids du splat), brins nains plutôt que despawn, scatter croisé
   (cailloux dans l'herbe / brins dans les fissures), contrat couleur
   bidirectionnel (Battlefront).
4. **Notre scatter CPU déterministe par cellules = l'architecture de
   l'état de l'art** (Guerrilla : « deterministic, locally stable ») ; le
   tout-GPU et le virtual texturing sont l'outillage des mondes 10×10 km,
   pas un prérequis.
5. **M1 (TBDR, vertex-bound)** : brins opaques sans alpha-test = le bon
   choix ; éviter les billboards alpha massifs de près ; le levier perf
   est la densité.

Sources : Etienne « Large Scale Terrain Rendering in CoD » SIGGRAPH 2023
(advances.realtimerendering.com) ; Hooker « Boots on the Ground » GDC 2021 ;
Wohllaib « Procedural Grass in Ghost of Tsushima » GDC 2021 ; van Muijden
« GPU-Based Run-Time Procedural Placement » (HZD) GDC 2017 ; Sanders
« The Vegetation of HZD » GDC 2018 ; Brown & Hamilton « Photogrammetry and
Star Wars Battlefront » GDC 2016 ; Moore « Terrain Rendering in Far Cry 5 »
GDC 2018 ; imgeself « Graphics Study: RDR2 » 2020 ; analyses Digital
Foundry de Crimson Desert (2026 — displacement généralisé + densité de
props ; aucun breakdown officiel Pearl Abyss).

**Réponse : OUI** — par la combinaison espèces/clumps + zones de variantes
+ densité liée au splat + clutter + ancrage couleur, toutes compatibles
avec le scatter pur-fonction du moteur.

## Briques livrées (toutes : build + 601 tests + smoke zéro erreur)

- **H0 — Stylized lighting OFF par défaut** (corrigé 2026-08-04 : le
  premier passage avait élargi la fenêtre de snap `stylizedShadow*` à
  0..1 au lieu de couper le knob — retour dev). La bonne coupure :
  `WorldRenderer::stylizedUi = false` (l'A/B du panel « Stylized
  lighting » ; `uAmbientColor.w` mixe TOUTES les branches cel — diffuse
  steppé, snap d'ombres, spec band, rim — donc un seul bool suffit).
  Les fenêtres snap sont RESTAURÉES à 0.45/0.55 (Form/toml/moteur) :
  recocher l'A/B rend le look BotW d'origine intact.
- **H1 — Espèces par clump** : `GrassSpecies.hpp` = LA table (4 espèces :
  prairie, sèche, folle avoine, fleurs — hauteur/largeur/lean/profil/
  teintes base+tip), copiée au FrameUbo (`uGrassSpecies*`) ; instance
  herbe 48→64 o (lane species/clumpShade, location 1) ; scatter : clump =
  Voronoï jitteré ~2,4 m (sites au plus proche PAR BRIN, cache par chunk),
  espèce gagnante au score = climat (dryBias sur temperature/biomeWetness)
  × bruits basse fréq. ~9 m par espèce ; les brins s'évantaillent DEPUIS
  le centre du clump (yaw), shade partagé. Contrat tri/préfixe intouché.
- **H1b — Zones de variantes du sol (l'idée « 4 couleurs » du dev)** :
  la couche sémantique herbe fetch 4 variantes de matériau (grasse /
  sèche / tapis de feuilles / terre) zonées par Voronoï sur grille
  jitterée ~12 m, coloriage 2×2 {0,1;2,3} → deux voisines orthogonales ne
  partagent JAMAIS la variante ; `terrain_zones.glsl` + miroir
  `grassZoneAt` (TerrainNoise) ; height-blend des 2 variantes les plus
  proches dans une bande de 0,8 m (la frontière a du relief et du POM).
  Arrays passés à **8 couches** (`kSplatArrayLayers`) : procédural (3
  tuiles variantes générées) ET cooké (manifest 8 matériaux — Grass004,
  ScatteredLeaves008, Ground037 ajoutés). Les poids sémantiques ne
  changent pas ; seul le FETCH herbe change. Le zoneId biaise les scores
  d'espèces (sec→sèche, feuilles→clairsemé) et le clutter.
- **H2 — Transition herbe↔roche douce** : le cutoff booléen devient
  `density = presence × smoothstep(cutoff−0.37, cutoff, grass)` + brins
  NAINS sur la frange (hauteur × la même rampe) ; touffes sèches
  clairsemées dans les fissures où rock+cliff ≥ 0.45 (espèce Dry forcée,
  densité 5 %) ; fondu de ring décalé PAR BRIN (clé uniforme) — le bord
  des 190 m se dissout au lieu d'avancer comme un mur.
  `materialWeightsAt` inchangé (buissons/arbres ne bougent pas).
- **H3 — Cailloux (clutter v1)** : les MÊMES meshes de boulders à échelle
  6-25 cm (§2.11 — zéro nouveau slot/mesh) ; spacing 1,8 m, chance =
  bande de mélange herbe/roche (4·grass·rock) + fond rocheux, ×1.6 sur
  les zones sèche/terre ; reach 90 m. **Fix critique** :
  `VegetationCollision` ignore les instances < 0.35 d'échelle (des
  milliers de corps Jolt pour des débris = crash SIGBUS à la destruction).
- **H4 — Ancrage couleur des props (Battlefront)** : la base 0-0,4 m de
  tout prop (rochers, troncs) fond vers la teinte macro du terrain
  (tap `uTerrainShade0` par position monde dans tree.frag, varying
  `vGroundDelta`).
- **H5 — Raccord racine sur le set cooké** : `.mtex` **v2** = moyennes
  de couleur par couche calculées au cook (le CPU ne décode pas le BC7) ;
  `TerrainSystem::grassAlbedoBase(variant)` (cooké : moyennes du fichier ;
  procédural : moyennes des tuiles générées) → le bake racine des brins
  utilise la moyenne de la VARIANTE de zone × blotch ±1 % × tint ; synchro
  WorldRenderer + regen au flip du toggle A/B. Le gap documenté du
  chantier TERRAIN-TEXTURING est clos.
- **H6 — Self-shadow POM** : 2 taps d'occultation vers le soleil dans la
  zone POM ; knob `pomShadowStrength` (0.6) → `uSplatVarietyInfo.y`.

## Knobs (panel « Terrain & streaming » → « Terrain materials » + Grass)

`splatBlendDepth`, `terrainTintStrength`, `splatDetailFade`, `pomDistance`,
`splatVariety`, `pomShadowStrength` (persistés par « Save render tuning ») ;
toggle « Cooked materials (A/B) ». Les espèces v1 sont des constantes
(`GrassSpecies.hpp`) — promotion en Form §5 quand le tuning se fige.

## Correctif de direction (retour dev, 2026-08-03)

Premier passage rejeté : les 3 variantes cookées (feuilles brunes, terre
nue) faisaient des TACHES DE COULEUR — contresens. **La règle actée : les
4 variantes restent dans la même famille chromatique (gazon) ; la
variation vit dans le CONTENU et le RELIEF (POM), jamais dans la
couleur.** Correctifs :
- Cellules Voronoï 12 m → **3 m** (demande dev).
- Variantes cookées remplacées (Poly Haven CC0) : v1 =
  `leaves_forest_ground` (gazon+feuilles — alternatives validables :
  forest_leaves_03/04, forrest_ground_01), v2 = `aerial_grass_rock`
  (gazon+pierres), v3 = `sparse_grass` (gazon+terre).
- **Harmonisation au cook** : `harmonize = true` au manifest normalise la
  moyenne albedo de chaque variante vers celle du gazon v0 (par canal) —
  la famille chromatique est garantie quelle que soit la source.
- Tuiles procédurales resémantisées (leafy/stony/dirt sur base
  grassAlbedo) ; biais espèces et cailloux recalés (feuilles → clairsemé,
  pierres/terre → cailloux ×1.6, terre → herbe sèche).

Étape suivante validée par le dev : les MODÈLES d'instanciation —
palier 1 (scans Poly Haven à géométrie porteuse : stone_01,
namaqualand_stones_01, rock_moss_set_01/02, tree_stump_01/02,
dead_tree_trunk, dry_branches_medium_01, bark_debris_01,
root_cluster_01/02) intégrables en vertex-color ; palier 2 (plantes :
grass_medium_01/02, fern_02, dandelion_01, shrub_01..04) exige la brique
« variantes texturées + alpha cutout » du pipeline veg (fill-rate à
surveiller — leçon RENDERING.md).

## Second correctif (retour dev, 2026-08-04) + modèles palier 1

- **Variante feuilles retirée** (trop visible) — les feuilles sont
  réservées à un futur set « sous-bois ». v1 = `grass_path_2` (Poly
  Haven, gazon usé laissant voir le sol), procédural recalé
  (`wornGrassTexel` : traînées de sol PLUS BASSES que l'herbe).
- **Modèles palier 1 LIVRÉS** : scans Poly Haven CC0 dans
  `game/data/base/models/scans/` (~14 Mo), guids VFS dans landscape.toml,
  chargés par la scène via `loadGltfMesh` → **`assets::simplifyMesh`**
  (NOUVEAU — meshoptimizer en CPM : quadrics + fallback sloppy + compact ;
  53-102k tris → 700-1200) → `normalizeMeshFootprint` (centré XZ, base à
  y=0, extent max normalisé) → `WorldRenderer::overrideVegetationMesh`
  (relais public du hook existant `overrideVariantMesh`).
  - Rochers : `stone_01` et `rock_moss_set_01` remplacent les variantes
    générées 0-1 — boulders ET cailloux (mêmes slots) deviennent scannés.
  - **Nouvelle catégorie débris forestiers** (`kDebrisVariants = 2`,
    `kFirstDebris`) : `tree_stump_01` (souche) + `dead_tree_trunk`
    (tronc couché), scatter intérieur de forêt (forestMask > 0.55,
    spacing 14 m, reach 300 m), collision type rocher, kind rigide
    (pas de sway). Placeholder = rock généré tant que l'override n'a
    pas chargé.

**Fix scans (retour dev, 2026-08-04 : tronc qui clignote + traversable)** :
- Les UV photogrammétriques des scans atterrissaient dans la lane que
  tree.vert lit comme (poids de sway, height01) — chaque vertex suivait la
  rafale indépendamment, le mesh se déchirait/scintillait au vent (ombres
  comprises, même règle dans le caster). Le câblage LandscapeScene réécrit
  les UV après normalisation : sway 0 (scans rigides), height01 = y/topY.
- Collision débris ajustée : boîtes par variante (souche trapue ; tronc
  couché = boîte allongée 1.1×0.3×0.3 × échelle, ORIENTÉE par le yaw
  d'instance — même rotation R_y(−yaw) que le shader). L'ancienne boîte
  rocher (0.75 de demi-taille) laissait passer à travers les extrémités
  d'un tronc de ~5 m.
- Assise du tronc couché (2e retour : enfoncé à moitié, scintille encore) :
  les props n'ont que le yaw (pas d'alignement pente) — un tronc de
  plusieurs mètres posé au centre s'enterrait côté amont, et la surface
  affleurante z-fightait le terrain (le « clignotement »). Le scatter
  sonde maintenant le sol aux DEUX extrémités le long de l'axe : sol trop
  irrégulier (> 0.3×échelle d'écart) → candidat rejeté ; sinon assise sur
  la CRÊTE (jamais enterré, léger sink 0.05×échelle). Le vrai alignement
  pente (pitch par instance) = différé, extension du format d'instance.
- **LA vraie cause du clignotement** (4e retour : le tronc ENTIER
  apparaît/disparaît ~10 Hz) : `GpuOcclusion::kMaxGroups` était resté à
  44 alors que les groupes de batch indirect végétation vont jusqu'à
  `kGroupBase + variant×3 + level` = 46 avec les 14 variantes. La
  variante 13 (le tronc couché, seule hors bornes — la souche 12 passait
  encore) se faisait clamper dans le slot d'un autre batch : commandes
  aliasées + ranges lus hors tableau → l'objet blinkait au rythme du
  ping-pong de commandes. Fix : kMaxGroups 44→48 + static_assert
  (kGroupBase + kVariantCount×3 ≤ kMaxGroups) au niveau de
  collectDrawCandidates — la prochaine variante ajoutée cassera le build,
  pas l'affichage. Les fixes précédents (sway, assise, clearance)
  restent valides mais n'étaient pas la cause du blink.
- Clearance arbres (3e retour : le tronc était posé PILE sur un arbre) :
  le scatter débris ignorait les arbres — en intérieur de forêt un tronc
  couché pouvait traverser un tronc debout (écorce dans écorce =
  z-fight). Les arbres du chunk étant placés avant les débris dans le
  même scatter, chaque candidat teste la distance XZ segment↔base
  d'arbre (rayon 0.7 + 0.3×échelle de l'arbre → rejeté). Limite v1 :
  les arbres du chunk VOISIN ne
  sont pas testés (un tronc près de la frontière peut encore en frôler
  un — rare).

**Perf scans + relief (retour dev, 2026-08-04)** — les budgets shadows/
mainPass avaient gonflé depuis le texturing. Deux causes : le shader
terrain réaliste (coût assumé, réglé par les knobs) et les scans à
700-900 tris remplaçant des rochers générés à 80 tris — partagés par les
CAILLOUX (centaines/chunk) et castés plein détail dans toutes les
cascades (pas de jumeaux LOD sur les overrides, pas de filtre d'échelle
au caster). Correctifs :
- `overrideVariantMesh` accepte des jumeaux low/ultra ; la scène les
  décime à l'override (150 / 40 tris) — cascades et champ lointain
  retombent près du coût d'avant, y compris au main pass (le pick de
  niveau existant les utilise dès qu'ils existent).
- `shadow_prop.vert` : les instances d'échelle < 0.35 (les cailloux) ne
  castent plus — même seuil que le skip collision.
- **Knob `pomDepth`** (0.03 par défaut, slider « POM relief depth »
  0..0.12, persisté) : l'amplitude du relief parallax, avant codée en
  dur — philosophie Skyrim moddé actée par le dev : du relief par pixel
  et des transitions soignées plutôt que toujours plus de petits objets
  instancés.

## Palier 2 — plantes texturées (2026-08-04)

La brique « props texturés + alpha cutout » du pipeline veg, et les 4
premières plantes (Poly Haven CC0, `game/data/base/models/plants/`) :
herbe haute (`grass_medium_01`), fougère (`fern_02`), pissenlit
(`dandelion_01`), arbuste (`shrub_04`). Mécanique :

- **Flag « textured » = signe de la lane fade** (`aParams.w < 0`) — zéro
  changement de format d'instance ; pour ces variantes l'uv du mesh reste
  de VRAIES coordonnées texture (pas la convention sway/height01 des
  scans) et le sway vient de la hauteur locale (base plantée).
- **Albédo par variante** : `setVariantAlbedo` (SRGBA8 + mips
  `generateMipmaps`, copie CPU pour survivre au regenerate) → bind group
  au layout du leaf-mask atlas, swappé par variante dans draw() ET
  drawIndirect(). `tree.frag` : cutout à 0.5 (les diffuses opaques —
  fern/shrub, feuillage modélisé — ne discardent jamais).
- **Fill-rate contenu (leçon RENDERING.md)** : reach 60 m, PAS d'ombres
  portées (drawDepth saute les plantes — modèle GoT), pas de GI props,
  pas de collision, faces doublées CPU pour les feuilles single-sided
  (pipeline cull Back conservé). kMaxGroups 44→64 (static_assert).
- **Scatter par habitat** : fougère en intérieur de forêt, arbuste en
  lisière, pissenlit en prairie ouverte, herbe haute partout en zone
  herbeuse ; zones usée/terre → moitié moins de plantes. Espacement
  4.5 m, acceptances 10-22 %.
- Décimation à l'override : 1200/900/1200/1500 tris (les sources font
  6-55 k) ; ~9 Mo de glTF + 4 PNG 1k (~22 Mo VRAM RGBA+mips — la
  compression BC7 de ces albédos = différé, pipeline .mtex réutilisable).
- **Exagération de lisibilité (2e retour taille)** : à l'échelle réelle,
  la fougère (0,43 m) se noie sous les brins — convention Skyrim :
  l'étage bas est exagéré ~1,5-2×. `PlantOverride.scaleMul` par plante :
  herbe 1.3, fougère 1.75 (→ ~0,75 m), pissenlit 1.5, arbuste 1.8.
- **Fix échelle (retour dev)** : ces fichiers sont des ALIGNEMENTS
  d'exposition (17 touffes sur 5,6 m, 5 pissenlits sur 4,2 m…) —
  normaliser l'emprise de la rangée réduisait chaque plante à 1-3 cm.
  Nouveau `assets::loadGltfMeshParts` (un MeshData par nœud) : la scène
  garde la plante la plus détaillée et POSE SANS RENORMALISER
  (`groundMesh` — les modèles sont à l'échelle réelle : touffe 0,34 m,
  fougère 0,43 m, pissenlit 0,17 m, arbuste 0,22 m ; échelle d'instance
  0,9-1,5).

## Rochers PBR + rampe de densité (retour dev, 2026-08-04)

Réponse à « faut-il un système type herbe pour la densité ? » : NON — le
VegetationSystem est déjà la bonne architecture ; il lui manquait la
**rampe densité-distance** de l'herbe (près = tout, loin = sous-ensemble
déterministe). Livré :

- **Props texturés rigides** : les 6 scans (4 rochers — dont 2 NOUVEAUX,
  `rock_boulder_dry` + `boulder_01`, remplaçant les variantes générées
  2-3 — souche, tronc) gardent leurs UV photogrammétriques et bindent
  leur diffuse 1k réelle via le chemin du palier 2. Convention « rigide »
  = phase de sway NÉGATIVE en plus du fade négatif (un rocher ne se
  balance pas ; miroir dans le caster). Boulders, CAILLOUX (mêmes slots)
  et débris passent tous en photo-texture — plus aucun prop low-poly
  vertex-color au sol.
- **Rampe densité-distance** (tree.vert) : clé de hash stable (lane
  tint) × smoothstep(0.45..0.95 du fade) — le clutter texturé courte
  portée (< 150 m : cailloux, plantes) s'amincit avec la distance au
  lieu de tout dessiner jusqu'au fade. Les boulders/débris (structurels,
  longue portée) ne s'amincissent jamais.
- **Densités payées par la rampe** : plantes espacement 4.5→3 m,
  acceptances +60-80 % ; cailloux recentrés sur la bande herbe/roche
  (fond hors bande 0.04→0.02, bande 0.38→0.45) — dans l'herbe pure ce
  sont les PLANTES qui portent la variété.
- Ordre des tirages rng de place() = CONTRAT (variant, scale, yaw, tint,
  phase) — le flag texturé s'est glissé sans le permuter (zéro reseed).

## P1+P2+P3 — détail, colonies, étage masse (retour dev, 2026-08-04)

Retour : fougère trop dégradée (décimée 2384→900) et trop petite, scatter
globalement trop sparse (« conviendrait à des lichens »).

- **P1 — héros fidèles** : cibles de décimation relevées à ~2400-2500
  (la fougère garde son maillage source), fougère ×3.5 (~1,5 m) ; les
  plantes reçoivent leurs jumeaux low/ultra (600/150 tris) ET un pick de
  niveau resserré (héros = chunk caméra SEUL — les rayons arbres 2/4
  chunks les gardaient plein détail partout où elles sont visibles).
- **P2 — colonies** : bruit ~12 m par espèce (smoothstep 0.45-0.75, ×2)
  sur l'acceptance — cœurs denses, clairières vides ; la densité perçue
  change à budget égal (la « carte de type » GoT en procédural).
- **P3 — étage masse** : 4 slots clones ~220 tris (kFirstMass,
  kVariantCount 22, kMaxGroups 80), espacement 2 m, fade 35 m, MÊME
  champ de colonies que les héros — le tapis continu sous les points
  focaux. Même albédo bindé, rampe de distance active.

## P4 micro-étage + normal maps des props (2026-08-04)

- **P4 — mousse & lichen par le système d'herbe** : table des espèces
  4→6 (`GrassSpecies_Moss`/`Lichen` — brin écrasé : hauteur 0.10/0.07,
  largeur ×4.5/×3.5, profil arrondi ; teintes vert profond / gris-vert).
  Tables FrameUbo [4]→[6] (offsets recalés, common.glsl en lockstep,
  taille 1936→2032). Sélection : la mousse gagne les clumps par
  `wetBias × forestMask` (sol de forêt humide — là où les brins
  s'éclaircissent) + boost sur zones usée/terre ; le lichen partage le
  chemin des fissures rocheuses avec Dry (45/55). Même budget
  d'instances : les espèces REMPLACENT des brins, rien ne s'ajoute.
- **Normal maps des props texturés** (retour dev « les fougères aussi
  doivent être AAA ») : les `_nor_gl_1k.jpg` (déjà dans les packs) se
  bindent au slot 3 du groupe albédo ; tree.frag perturbe la normale via
  une base cotangente dérivée (Schüler — pas de tangentes de mesh, zéro
  changement de format). Fallback plat 1×1 pour les variantes sans map
  et l'atlas de feuilles (même layout de groupe). S'applique aux 6 scans
  + 4 plantes + 4 clones masse.

**Fix cutout (retour dev : « les fougères ne sont pas transparentes ? »)** :
Poly Haven livre l'alpha de découpe en MAP SÉPARÉE (`Alpha/1k`) — les
diffuses png de fern_02/shrub_04 sont du RGB pur, d'où des cards
opaques. Les `_alpha_1k.png` sont téléchargées pour les 4 plantes et
fusionnées dans le canal alpha de la diffuse AU CHARGEMENT (scène,
garde sur les dimensions).

## Hex-tiling (option B — retour dev : les frontières Voronoï se voyaient)

Le zonage Voronoï discret est REMPLACÉ par du hex-tiling stochastique
(Heitz-Neyret 2018 / Mikkelsen 2022), commité séparément pour retour
arrière facile :
- `terrain_zones.glsl` : treillis triangulaire (cellule 3 m), chaque
  sommet tire sa VARIANTE (hash & 3) et un OFFSET UV aléatoire ; poids
  barycentriques affûtés (exposant 6) → le blend se confine à des
  coutures fines organiques — plus aucune frontière discrète n'existe.
- `terrain.frag` : hauteurs/albédo/normales herbe = mélange des 3 taps ;
  le POM marche sur le tap dominant (offset porté par pomUv, soustrait
  pour les autres couches) ; au-delà de ~45-75 m les poids s'effondrent
  sur le tap dominant → retour à 1 fetch (le coût 3× reste près caméra).
  L'anti-répétition bi-fréquence est retirée de la couche herbe (les
  offsets du treillis la subsument) et conservée pour les autres.
- Miroir CPU `grassZoneAt` : même treillis/hash/affûtage, sommet
  dominant → les biais espèces/cailloux/plantes suivent toujours le sol.
- L'ancien coloriage 2×2 disparaît (les variantes sont tirées au hash —
  deux sommets voisins peuvent partager une variante, sans conséquence :
  la garantie « jamais adjacentes » servait les CELLULES, le treillis
  n'a plus de cellules).

## Écorce des arbres (demande dev, 2026-08-04)

Textures Poly Haven CC0 (`textures/bark/`) : `jolcham_oak_bark_01`
(chêne) + `pine_bark` (résineux ~épicéa) :
- Flag « bois » dans les générateurs : uv.y = −1 sur les tubes (la lane
  n'était lue nulle part pour le bois — contrat du test TreeGenerator
  mis à jour). Cards/lobes/buissons inchangés.
- `tree.frag` : échantillonnage **triplanaire** en espace objet (aucune
  UV de mesh requise, pas de couture sur les branches courbes), modulé
  par la couleur vertex (l'AO bake et le gradient vertical survivent).
  Gaté par `uSplatVarietyInfo.w` (0 tant que la scène n'a pas chargé).
- Binding 7 du groupe 1 (layout partagé : leaf atlas @0, normal @3,
  bark @7 — dummies sur les groupes plantes/scans et leafMaskGroup).
- **Choix par slot dans le tree builder** : combos « Bark » (Oak/Spruce)
  par slot d'arbre — défaut chêne sur les feuillus (0-2), épicéa sur
  les conifères (3-4) ; rebuild des groupes au point sûr
  (`barkGroupsDirty`). Sampler REPEAT dédié (le triplanaire tuile).
- **v2 — le relief dans le matériau (retour dev : tronc low-poly, relief
  par le matériau)** : normal-height packée par écorce (nor_gl RGB +
  displacement en alpha, fusion au chargement) au binding 8 ;
  `tree.frag` : parallax bump-offset le long de la vue (le yaw
  d'instance est packé dans le flag — vObjPos.w = 1+yaw — pour passer
  monde↔objet) puis normal mapping triplanaire (frames par plan,
  whiteout, re-rotation monde). 6 taps/pixel d'écorce ; amplitudes :
  kBarkDepth 0.045 uv, kBarkTile 1.2/m.

## SSAO (plan « relief de côté » 1/2 — 2026-08-05)

Recherche SSDM consignée : Crimson Desert utilise du Screen Space
Displacement Mapping (Lobel 2008) — pixels déplacés en espace écran, la
silhouette gonfle vraiment. Plan acté avec le dev : **1) SSAO d'abord,
3) SSDM ensuite** (le displacement géométrique near-field reste en
poche). Livré ici le 1 :
- `ssao.frag` : Alchemy 8 taps spirale + jitter IGN, normales par
  dérivées, demi-résolution sur la copie de profondeur, fade 80-140 m,
  ciel neutre — le frère jumeau du pass contact shadows (même patron :
  cible → blur partagé → multiplié au tonemap, toggle = clear blanc,
  brins d'herbe exempts par le flag alpha).
- Lanes `uSsaoInfo` (FrameUbo append, 2032→2048) : force 0.85, rayon
  0.7 m — sliders au panel « Rendering & post-FX » + checkbox.
- Contrat lighting consigné dans RENDERING.md : rayon court seulement
  (les fissures que les sondes RC ne résolvent pas), pas de double
  comptage avec le GI, tourne aussi en intérieur (indépendant du
  soleil, contrairement au contact).

## SSDM prototype (plan « relief de côté » 2/2 — 2026-08-05)

La brique Crimson Desert (Lobel 2008), variante GATHER derrière un
toggle **OFF par défaut** (« SSDM (prototype) » + amplitude au panel) :
- Les matériaux packent leur relief dans l'ALPHA de la scène :
  0.5 = plat .. ~0.99 = crête ; < 0.5 reste le flag herbe, ≥ 0.995 =
  shaders non participants (meshes/eau inchangés). Terrain = hauteur
  splat blendée ; écorce = sa hauteur triplanaire. Les seuils existants
  (contact/SSAO/skyclouds) restent binaires (step 0.5 au tonemap).
- `ssdm.frag` : warp plein-res de l'image (copie couleur fraîche
  post-eau → offscreen) par itération de point fixe (4), déplacement le
  long de la normale écran reconstruite, borné à 10 px. Lane
  `uSsaoInfo.z` = amplitude monde (0.12 m défaut).
- Limites v1 documentées dans le shader : les bords contre le CIEL ne
  s'extrudent pas (un gather n'a pas de graine sur les pixels vides —
  le scatter pyramidal du papier est l'étape suivante si le rendu
  convainc) ; étirements aux falaises de profondeur (la famille
  d'artefacts que CD assume) ; la profondeur n'est PAS déplacée (les
  passes écran lisent la copie non warpée — écart ≤ 10 px à l'échelle
  du relief).

**v2 — scatter pyramidal** (retour dev : le gather « creuse » sans
étendre — l'asymétrie structurelle) : la méthode complète de Lobel.
Chaîne `ssdm_flow` (delta px + profondeur déplacée par pixel, math
partagée `ssdm_common.glsl`) → `ssdm_bounds0` + 4 downsamples (bboxes
min/max des positions déplacées, textures séparées par niveau — le
pattern bloom) → `ssdm_resolve` : descente de quadtree (seed = tuiles
16 px joignables, pile bornée 64/160 itérations), candidat le plus
PROCHE gagne (reversed-Z déplacé) — les crêtes extrudent et OCCLUENT,
ciel compris ; les pixels orphelins (creux désoccludés) retombent sur
le gather v1 inline — les deux sens du relief coexistent. Troncs
passés à 12 côtés (tubeSides data) au préalable : le scatter ébrèche un
profil déjà rond. Critères posés : ≤ ~2,5 ms (scope F6 `ssdm`),
scintillement sans TAA = critère d'abandon.

## À valider par le dev

Prairie (mélange d'espèces + clumps + zones qui changent la texture ET la
végétation ensemble), frontière herbe/roche (rampe + nains + touffes de
fissure + cailloux croisés), toggle A/B (raccord racine des deux côtés),
compteurs F6 (cible herbe+clutter ≤ ~3 ms ; densité de cailloux, pic
d'upload herbe au boot avec les instances 64 o).

## Différés

Touffes-meshes (fougères/broussailles — silhouettes que les brins ne font
pas ; le socle H3 les accueille), cartes de type peintes (mode Scénario),
decals splinés (chemins), hex-tiling, virtual texturing, `vegetationSet`
par biome, promotion des espèces en Form, collision fine des gros
cailloux (seuil 0.35 actuel), alignement pente des props couchés (pitch
par instance — extension du format d'instance veg).
