# 3D-RENDERER — Renderer paysage GL 4.6, style Breath of the Wild

> Journal de briques + feuille de route du renderer 3D custom (chemin
> « custom renderer » des Phases 11-14 du CLAUDE.md, anticipé avant la
> validation Godot 8.5). Poussée de juillet 2026. **Lire ce fichier avant
> de toucher `engine/render/landscape/`, les extensions 3D/compute du RHI,
> ou `game/scenes/LandscapeScene`.**

## OÙ ON EN EST (mis à jour 2026-07-06)

> **Piste EN PAUSE depuis le pivot du 2026-07-05** (démo dans Meadows —
> `docs/MEADOWS-PLAN.md`) au profit de la passe horizontale (FAITE,
> `docs/HORIZONTAL-PASS.md`). **Les briques restantes 27-31 sont absorbées
> par les chantiers de MEADOWS-PLAN** (roadmap unique) : 27 (arbres canopée
> pleine) → chantier 1, lumières intérieures → chantier 2, 28-31 (grading,
> auto-exposition, cumulonimbus, pluie) → chantier 6. Ce fichier reste le
> journal ET la spec détaillée de ces briques — on l'ouvre quand un
> chantier les atteint.

| # | Brique | État |
|---|--------|------|
| 1-24 + 23b | Renderer complet (terrain→météo) + passe stylisée BotW | ✅ faites et validées |
| 25 | Frustum culling CPU des chunks | ✅ FAITE — validée |
| 26 | Occlusion culling : horizon CPU + Hi-Z GPU (1re extension compute du RHI) | ✅ FAITE — **validation visuelle dev jamais faite** (fond de vallée + rotation rapide, checkboxes A/B du panneau) |
| 27 | Arbres : canopée pleine stylisée (remplace les leaf cards) | ✅ FAITE (chantier 1 B7, validée dev 2026-07-06) — spec exécutée + **LOD de canopée par chunk** ajouté post-validation : subdiv 2 ≤ 4 chunks (~256 m), subdiv 1 (80 faces, même seed) au-delà + casters d'ombre + réflexion (sans LOD : 30 fps ; avec : max). Journal : `docs/CHANTIER-1.md`. |
| 28-29 | Grading + auto-exposition | ✅ FAITES (chantier 6 B3/B4, 2026-07-07) — NON COMMITÉES, toggles A/B défaut OFF, validation dev en cours. Journal : `docs/CHANTIER-6.md`. |
| 32 | **Eau plaçable (`WaterVolumeForm`)** — lacs d'altitude, grottes inondées | 📋 SPEC DÉCIDÉE 2026-07-07 (voir brique 32 ci-dessous) — à planifier |
| 28-31 | Grading BotW, auto-exposition, cumulonimbus, pluie | ⏸️ à faire (28-29 courts ; 30-31 branchés sur WeatherForm) — planifiées via MEADOWS-PLAN chantier 6 |
| — | Refonte herbe | ⏸️ en attente des recherches du dev (il revient avec une référence shader — ne pas itérer sans lui) |

## Ce qui est construit (briques 1-26, résumé architectural)

La démo vit dans **`LandscapeScene`** (`game/scenes/`), qui possède sa frame
(`Scene::ownsFrame`) via le seam Engine/FrameContext — les scènes 2D sont
intactes. Tout le GPU passe par `rhi::` (extensions Vulkan-shaped) ; le sim
reste headless.

**Systèmes** (`engine/render/landscape/`) :
- `TerrainSystem` — chunks 64 m streamés (workers → ConcurrentQueue → upload
  budgété, pattern TextureCache), LOD 4 niveaux + skirts, bruit world-space
  déterministe (`TerrainNoise`, bords bit-exacts), splat array sRGB 4 couches.
- `GrassSystem` — brins-rubans instanciés par chunk (rendu à refondre, infra
  agnostique au style) ; `VegetationSystem` — 12 variantes (5 arbres, 4
  rochers dont 1 glTF CC0, 3 buissons), scatter déterministe en ceintures de
  forêt, draws variant-major avec baseInstance ; `TreeGenerator`.
- `SkySystem` — palette jour/nuit analytique (aube ≠ crépuscule, entre chien
  et loup), grading météo CPU (warmth/saturation/intensités), bake du champ
  de nuages 512² 1×/frame (tous les consommateurs d'ombres tapent une
  texture, pas un FBM) ; `ShadowMapper` — CSM 3 cascades 2048² PCF.
- `WaterSystem` — plan mer, réflexions planaires (miroir + clip oblique
  Lengyel), foam par depth + carte de profondeur de bassins bakée CPU
  (256², suppression d'écume des petites mares, view-independent).
- `PostFx` — bloom pyramide 5 mips, god rays screen-space, shafts
  volumétriques (marche half-res 20 pas, additif proche + rideaux
  multiplicatifs lointains — jamais compter deux fois l'air payé par le
  fog), SSAO ; tonemap ACES + debug buffers.
- `ChunkOcclusion` (CPU, horizon de hauteur sur worker) + `GpuOcclusion`
  (Hi-Z compute) + `Frustum` (`engine/render/`).
- `stylized.glsl` — passe BotW partagée terrain/arbre/herbe/feuille :
  rampe à 2 steps, `round(atten)` sur les ombres CSM, SSS gaté, rim steppé ;
  flag `uAmbientColor.w`, checkbox A/B.
- Météo : `WeatherForm` ×6 états dans `game/data/base/landscape.toml` (§5,
  moddable), crossfade ~30 s, vent unifié `uWindInfo` (temps ACCUMULÉ).
- Tuning : `LandscapeTuningForm` (TOML §5) ; `assets::GltfMesh` (cgltf).

**Extensions RHI 3D** (briques 2/10) : depth/cull/wireframe dans
PipelineDesc, framebuffers {texture, mip, layer}, samplers (comparison PCF),
formats SRGBA8/RGBA16F/R16F/R32F/Depth32F, texture arrays, mips,
copyTexture, caps() par feature. **Extension compute** (brique 26) :
BufferUsage::Storage + BufferDesc::readback, ShaderDesc::computeSource,
ComputePipelineDesc, BindGroupEntry::{storage, storageImage+imageMip},
CommandBuffer::{dispatch, memoryBarrier, copyBuffer},
Device::{createComputePipeline, readBuffer}, caps.computeShaders,
`.comp` dans ShaderLibrary (hot-reload).

**Ordre des passes** (`LandscapeScene::render`) : pumps streaming + occlusion
CPU → bake cloud map → CSM ×3 → réflexion planaire (half-res, frustum
non-oblique) → passe opaque HDR (terrain/props/feuilles/herbe/ciel, frustum +
occlusion) → copyTexture color+depth → **Hi-Z + cull compute** → eau → SSAO/
bloom/god rays/volumétrique → tonemap → sprites 2D → ImGui.

## Leçons à ne pas reperdre

- **Fill-rate cutout = l'ennemi n°1** (verdict A/B leaf cards : FPS au max
  sans les cartes). Opaque + early-Z partout où possible.
- `uWindInfo.x` = temps de vent **ACCUMULÉ** (`+= dt×force`), jamais
  `t×vitesse` — sinon nuages/vagues téléportent au changement de météo.
- Échec de compile shader au premier chargement ⇒ pipeline invalide ⇒ abort
  (seul le hot-reload garde l'ancien programme) — durcissement RHI candidat.
- GLSL : `buffer`, `packed` réservés. UBO C++/GLSL champ pour champ
  (`FrameUniforms.hpp` ↔ `common.glsl`), ajouts en FIN de struct.
- **Hi-Z** : ne PAS construire la pyramide en passes fragment sur la même
  texture (feedback loop UB) — imageLoad/Store en compute.
- **Readback GPU→CPU** : jamais lire directement un SSBO de travail (navette
  VRAM↔RAM à chaque frame) — pattern staging Vulkan : `copyBuffer` vers un
  buffer `readback` host-visible, le CPU lit celui-là.
- Réflexions planaires : frustum de culling depuis la projection NON-oblique
  (le clip de Lengyel corrompt le plan far).
- Conservatisme d'occlusion : cible au-dessus de la caméra → pente depuis le
  coin PROCHE ; en-dessous → depuis le coin LOINTAIN.
- PowerShell : pas de round-trip `Get-Content|Set-Content` sur de l'UTF-8.
- L'ordre de composition lumière : fog = extinction + in-scatter non-ombré ;
  volumétrique = différentiel ombré × transmittance restante ; aucune passe
  ne paie deux fois le même air.

---

## Brique 27 — Arbres : canopée pleine stylisée (remplace les leaf cards)

**Décision (2026-07-05, validée)** : abandon des leaf cards après 5
itérations — fill-rate coupable (A/B) et rendu insatisfaisant. BotW utilise
des canopées **opaques**.

**Quoi.** La composition validée reste (tronc-colonne 4.2-6.1 m, 3-5 branches
hautes tiltées, un lobe par branche + couronne). Ce qui change :

- **Lobes = icosphères subdiv 2 (320 faces), jitter doux (~0.10-0.14)**,
  léger aplatissement vertical (~0.85) pour des couronnes étalées.
- **Normales sphérisées SUR le mesh** : normale du vertex =
  `normalize(mix(dir_lobe, dir_canopée, ~0.4))` — flaques de lumière par
  lobe, cohérentes sur l'arbre entier (le cœur du look halisavakis/BotW,
  appliqué au mesh plein).
- **Palette** : vert par lobe + gradient vertical (+25 % au sommet) + tint
  jitter par instance. Fini l'assombrissement ×0.55.
- **tree.frag** : ajouter `stylizedRim` (existant, était sur les feuilles) ;
  garder rampe 2 steps + `stylizedShadow` + wrap classique en A/B.
- **SUPPRESSION du système leaf cards** : `leaf.vert/frag`, atlas
  (`buildLeafTexturePixels` + doctest), buffers/pipeline/texture/bind group
  leaf de `VegetationSystem`, `kLeafChunkRadius`, checkbox « Tree leaf
  cards », `TreeMeshes` → retour à `MeshData generateTree(seed)`. Git garde
  tout ; la leçon vent-par-UV est notée pour l'herbe.

**Où.** `TreeGenerator.{hpp,cpp}` (le gros), `VegetationSystem.{hpp,cpp}`
(simplification nette), `tree.frag`, `TreeGeneratorTest.cpp`,
`LandscapeScene.{hpp,cpp}` (retrait checkbox).

**Validation.** Silhouettes élancées de près et à 880 m ; flaques lit/ombre
par lobe au soleil rasant ; rim contre le ciel ; FPS ≈ état « cards off » ;
doctest déterminisme à jour. Casters d'ombre = le même mesh (rien à faire).

## Brique 28 — Grading BotW (fin de la passe stylisée)

**Quoi.** Grade analytique dans `tonemap.frag` (après ACES, avant gamma) :
**vibrance** (saturation pondérée), **split-toning** léger (ombres fraîches,
hautes lumières chaudes), **contraste** pivot 0.5. Paramètres dans
`LandscapeTuningForm` (§5) + sliders. Une vraie LUT 3D reste possible plus
tard — l'analytique donne 90 % sans outil d'authoring.

**Où.** `tonemap.frag`, `LandscapeTuning.{hpp,cpp}` (+3-4 champs),
`landscape.toml`, `LandscapeScene` (3 floats dans des slots vec4 libres,
p.ex. `sunGlowColor.w` / `zenithColor.w` / `horizonColor.w`).

**Validation.** Toggle A/B ; verts qui chantent sans virer fluo à midi ;
couchers Hazy plus chauds ; modif TOML visible sans recompile.

## Brique 29 — Auto-exposition (adaptation de l'œil)

**Quoi.** Luminance moyenne GPU : target R16F 64×64 en log-luminance depuis
le HDR, `generateMipmaps` → mip 1×1 = moyenne ; micro-passe « adaptation »
lit ce 1×1 + l'exposition précédente (ping-pong 1×1 A/B) et écrit
`lerp exp(prev → target, 1-exp(-dt·vitesse))` — inertie asymétrique
(obscurité plus lente). Le tonemap multiplie par cette texture (1 tap).
« EV bias » + bornes min/max dans le TOML ; le slider Exposure devient le
bias. (Candidat compute pour l'histogramme, plus tard.)

**Où.** `PostFx.{hpp,cpp}` (2 passes + 3 textures), `luminance.frag` /
`adapt.frag`, `tonemap.frag`, `LandscapeTuningForm`.

**Validation.** Midi → nuit au slider : ajustement en ~2-4 s ; sous un ciel
d'orage : remontée douce ; debug buffer = luminance mesurée.

## Brique 30 — Cumulonimbus à l'horizon

**Quoi.** 6-10 billboards géants (2-4 km) sur l'anneau d'horizon, orientés
caméra en yaw seul, silhouette de tour cumuliforme par FBM érodé 2D, éclairés
palette ciel (cel 2 tons + silver lining), fondus par le fog. **Nouveau champ
`WeatherForm.stormFront` (0-1)** crossfadé comme le reste (Storm = 1). Pas de
raymarch 3D en v1 ; dessinés dans la passe opaque au far depth après le dôme.

**Où.** `SkySystem` (ou petit `HorizonClouds`), `cumulonimbus.vert/frag`,
`WeatherForm`/`landscape.toml`, `LandscapeScene`.

**Validation.** Storm : tours sombres qui montent pendant le crossfade ;
Clear : rien ; embers au couchant ; FPS inchangé.

## Brique 31 — Pluie (particules)

**Quoi.** Streaks instanciés dans un cylindre caméra (~30 m, 2000-6000 quads
étirés selon chute + vent) ; positions procédurales par hash(i) défilées
(`y = mod(seed - t·vitesse, hauteur)`, zéro sim CPU), alpha blend doux, après
l'eau avant le post. **Nouveau champ `WeatherForm.rainIntensity` (0-1)** +
état « Rain » dans le TOML. Splashes hors périmètre v1.

**Où.** `RainSystem.{hpp,cpp}` + `rain.vert/frag`, `WeatherForm`/
`landscape.toml`, `LandscapeScene`.

**Validation.** Rain/Storm : montée en ~30 s ; streaks penchés par le vent ;
pas de « mur » aux bords du cylindre en mouvement ; FPS stable.

**Addendum (audit Community Shaders, 2026-07-07 — idées, pas leur code,
GPL) :** à la même brique ou juste après, **wetness** (albédo assombri +
spéculaire boosté sous la pluie) et **occlusion de pluie** par depth map
top-down orthographique (la pluie ne tombe pas sous les toits) — le
pattern exact de nos cartes bakées (cloud map, pool depth).

## Brique 32 — Eau plaçable (`WaterVolumeForm`) — spec décidée 2026-07-07

**Pourquoi.** L'eau est aujourd'hui UN plan global (`seaLevel`) qui sert à
la fois de surface, de plan miroir, de référence d'écume et de test de
submersion — donc ni lac de montagne (eau au-dessus de l'altitude de
base) ni grotte inondée (eau en intérieur à hauteur arbitraire). Le bug
« intérieurs verdâtres » (chantier 6 B5bis) était déjà ce test global qui
fuyait dans les cellules intérieures.

**Quoi.** Un Form plaçable — le modèle Skyrim : une boîte (demi-étendues
+ transform de la référence) dont la **face supérieure est la surface
d'eau** et dont le **volume est le test « je suis dans l'eau »**. Posable
au gizmo, moddable (§5), sauvegardable comme toute référence. Champs v1 :
`halfExtents`, teinte/absorption, chop/vitesse de vagues (réutilise le
shader eau), `swimmable` (réservé). La mer globale RESTE le cas implicite
(aucune migration).

**Briques internes, par ordre :**
1. **Surface + submersion** : un quad d'eau au sommet du volume rendu par
   le shader eau existant (paramètres par-volume) ; le test
   caméra-dans-un-volume alimente l'effet de submersion du tonemap — le
   gate devient « dans un volume OU (extérieur ET sous seaLevel) »
   (l'exclusion intérieure actuelle devient un cas particulier de cette
   règle). Spawner : nouvelle catégorie, composant `WaterVolume` runtime.
2. **Écume/profondeur** : PAS en v1 (la pool-depth map est câblée mer) —
   bords nets stylisés suffisants pour un lac.
3. **Nage** : gameplay séparé (capsule en mode nage dans un volume,
   flottabilité, stat `breath` de STATS.md §3) — chantier gameplay.

**La limite structurelle à assumer : les réflexions.** La réflexion
planaire rend la scène miroir d'UN plan par frame. Décision : la mer
globale garde le miroir ; les volumes posés ont une surface « sourde »
(teinte ciel + fresnel stylisé, pas de miroir). Option de tuning plus
tard : basculer LE volume le plus proche de la caméra sur le plan miroir
de la frame quand un lac devient un décor clé.

**Alternative écartée :** hauteur d'eau PAR CELLULE (l'autre moitié du
modèle Skyrim) — couvrirait l'intérieur inondé mais pas les lacs
d'extérieur, et créerait un 2e mécanisme parallèle au volume (§5 : jamais
deux mécanismes pour la même chose).

**Où.** `data/forms/VisualForms.hpp` (WaterVolumeForm), spawner/
Components (world/scene), `WaterSystem` (draw des quads par-volume),
tonemap (le test de submersion passe par un uniform « dans l'eau » CPU),
`LandscapeScene` (collecte des volumes autour de la caméra).

**Validation.** Un lac posé dans une cuvette sculptée au-dessus de 14 m ;
une salle du hall d'essai inondée à y = 1 m ; entrer/sortir de l'eau =
la teinte de submersion suit le volume, pas seaLevel ; l'extérieur mer
inchangé (miroir compris).

## Brique 33 — Passe lumière extérieure (SSS contact, ombres de terrain, skylighting) — spec 2026-07-07

**Pourquoi.** Demande dev après la passe intérieure : trois effets issus de
l'audit Community Shaders (idées, pas leur code — GPL) qui « assoient » le
monde extérieur. Ordre = valeur/effort.

**33a — Screen-space contact shadows** (l'algo Bend Studio, GDC). Une passe
demi-res sur `sceneDepthCopy` (le pattern SSAO existant, à cloner) : depuis
chaque pixel, marcher ~16 pas en screen-space VERS le soleil ; si un
échantillon de depth s'interpose (avec une limite d'épaisseur contre les
halos), le pixel est en ombre de contact. Sortie R8 ; le tonemap multiplie
comme le SSAO. Toggle SANS uniform : quand désactivé, on saute la passe et
on clear la texture à blanc (aucun slot .w libre — ils sont tous pris,
inventaire dans CHANTIER-6.md). Pose l'herbe, les petits props, les pieds
des PNJ que le CSM 2048² laisse flotter.

**33b — Ombres de terrain lointaines + 33c skylighting : UN SEUL bake.**
Le pattern cloud-map/pool-map réutilisé : une texture « TerrainLightMap »
256² (~1,5 km autour de la caméra) bakée sur WORKER depuis la fonction de
hauteur (patches inclus), DEUX canaux :
- **R = visibilité du soleil** : marche de la hauteur de terrain vers le
  soleil (~40 pas) — les montagnes projettent sur les vallées au-delà des
  480 m du CSM. Re-bake déclenché par le pas d'hystérésis du soleil des
  cascades (~8 s réels — déjà en place) et par le déplacement du centre
  (le déclencheur du cloud map).
- **G = ouverture du ciel** : 8 azimuts × 12 pas d'horizon → fraction
  d'hémisphère visible ; multiplie l'AMBIANTE (terrain/mesh/grass) — les
  fonds de vallée et canyons s'assombrissent naturellement (le « BotW
  grounding »).
Échantillonnée dans terrain/mesh/skinned/grass comme le cloud map (mêmes
slots d'info : centre XZ + 1/span sur un vec4 à trouver — AUCUN .w libre,
il faudra un vrai APPEND de FrameUbo en FIN de struct, leçon UBO).
Coût bake : 256²×~50 évals FBM ≈ 0,2 s de worker toutes les ~8 s — budget
cloud-map. v1 terrain seul (pas les bâtiments).

**Où.** 33a : `PostFx` (+1 passe, cloner SSAO), `contactshadow.frag`,
tonemap binding 6, `LandscapeScene` (toggle + clear-blanc). 33b/c :
`TerrainLightMap.{hpp,cpp}` (nouveau, pattern SkySystem::bakeCloudMap
CPU), FrameUbo APPEND (info de carte), taps dans terrain/mesh/skinned/
grass.frag, re-bake câblé sur le pas du shadowSunDirection.

**Validation.** 33a : herbe/props/pieds posés au sol en plein soleil,
toggle A/B, FPS stable. 33b : coucher de soleil — les crêtes projettent
sur les vallées au loin (au-delà du CSM). 33c : fond de vallée plus
sombre qu'une crête à midi, transition douce. L'extérieur ne change PAS
toggles off (byte-identique).

## Brique 34 — Light shafts intérieurs à poussière (le FXShaft de Skyrim) — spec 2026-07-07

**Pourquoi.** Demande dev : les faisceaux de fenêtre avec poussière de
Skyrim. Là-bas ce ne sont NI du fog NI des particules : des MESHES
translucides additifs (prismes) avec texture de poussière défilante.
Notre twist : la géométrie est GÉNÉRÉE depuis les paramètres de la
lumière — donc elle peut suivre l'heure du jour.

**Quoi.** Champs APPEND sur `LightForm` : `shaft` (bool), `shaftLength`,
`shaftSoftness`, `dustDensity`, `sunLinked` (bool). Renderer : pour
chaque lumière shaft, un prisme/cône effilé procédural (~6 lames, régénéré
quand direction/angle changent) ; fragment = dégradé axial × fondu radial
× atténuation par la tranche × 2 couches de bruit défilant le long du
faisceau (dérive de poussière) + bande de bruit fin seuillé (motes qui
scintillent). Additif, pas de depth write, après les opaques avant le
post. **`sunLinked`** : direction/couleur/intensité recalculées du soleil
QUANTIFIÉ des ombres (hystérésis en place) — shaft rasant doré le matin,
raide blanc à midi, éteint sous l'horizon. Le cône du spot devient ainsi
visible (le spot éclaire la flaque, le shaft montre le volume — mêmes
données). V2 : quelques particules fx::ParticleSim dans le faisceau.

**Où.** `data/forms/VisualForms.hpp` (APPEND), spawner/LightSource (+
champs), nouveau `lightshaft.vert/frag`, `LandscapeScene` (génération des
prismes + draw translucide, MAJ sunLinked au pas du shadowSunDirection).

**Validation.** Une fenêtre du hall d'essai avec un shaft sunLinked :
l'angle et la teinte suivent le cadran (accéléré via « Animate time ») ;
poussière qui dérive visible de côté ET de face ; nuit = éteint ; FPS
inchangé.

---

## Backlog (après cette poussée)

- **Refonte herbe** — EN ATTENTE des recherches du dev. Infra
  scatter/instancing/streaming agnostique au style. Leçon à réutiliser :
  vent par déplacement d'UV (article halisavakis).
- Vraie LUT 3D texture, TAA, caustiques, vent global unifié (direction),
  particules d'ambiance, biomes, eau Gerstner/micro-vagues, **mode dégradé
  GL 4.1** (implémentations 4.1 des créations RHI + features off — l'étage
  occlusion CPU est déjà le fallback du Hi-Z), réévaluation luminosité de
  midi (l'auto-expo 29 la règle peut-être).

## Vérification (commune à toutes les briques)

- Une brique = build vert + 208+ doctests verts + smoke-run + critère visuel,
  **validation dev entre chaque brique** (FPS via le compteur du panneau).
- Doctests headless quand c'est pur CPU : déterminisme generateTree (27),
  résolution TOML des nouveaux champs (28-31). Existants : Frustum,
  ChunkOcclusion, TerrainNoise, GltfMesh.
- Pas de campagne de tests 2D ; ne pas casser la compile des scènes
  existantes. Build Windows à chaque brique ; pas de `<windows.h>` dans les
  headers (§3.1).
