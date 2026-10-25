# Q3 — `engine/render/` + `engine/rhi/`

> Vague Q3 de la revue de qualité d'octobre 2026 (`README.md` : méthode, classes
> S/G/D/B/H/P, arbitrages de juillet à ne pas re-proposer). Relecture **intégrale** du
> 2026-10-23 sur HEAD `61efb65` : racine `engine/render/` (26 fichiers, 6 348 l.),
> `engine/render/landscape/` (53 fichiers, 15 981 l.), ses shaders (106 fichiers,
> 7 765 l. GLSL), `engine/rhi/` avec les deux backends (17 fichiers, 7 554 l.) — soit
> ~37 600 lignes, sept lots en parallèle (D1 orchestrateur, D2 eau et cartes cuites, D3
> terrain/herbe/occlusion, D4 végétation, D5 lumière/post/ciel/FrameUbo, D6 RHI, D7 racine
> + hot-reload transversal). **Aucun code modifié.** Lignes = `61efb65` ; les symboles
> font foi.

---

## Verdict

Le rendu est **l'unité la mieux tenue des trois vagues** : §2.1 passe partout (aucun appel
GL/Vulkan hors `rhi/backends/`, un seul fichier de plateforme crée la surface), la coupe
Phase 5 est respectée par les dix-sept systèmes (snapshot en entrée, workers vers des files
drainées sur le thread principal), `render()` se termine par une table des matières des
passes qui se lit comme `RENDERING.md` §2.2, et les mécanismes introduits par l'audit de
juillet — `rhi::Unique`, `ChunkStreamer`, `BakeMailbox`, `ShaderLibrary::Watch`,
`ResidencyCache`, `MeshVertexLayout`, `FrameComposer`, le verrou `static_assert` du
FrameUbo — existent, fonctionnent, et servent de modèles dans la moitié des fichiers.

Le problème de la vague tient en une phrase : **chaque bon mécanisme a été adopté à
moitié, et la moitié restante a continué à se copier.** `rhi::Unique` couvre 241 sites
mais `WaterSystem` garde 45 `destroy*` manuels, `GpuOcclusion` 14, `SkySystem` 8 (dix
classes au total). `ShaderLibrary::Watch` surveille 12 pipelines, 15 compteurs de
génération à la main en surveillent d'autres, et **sept pipelines n'ont aucun
mécanisme** ; la seule watch de l'eau est vide depuis sa création (bug Q3-43 : les cinq
shaders d'eau ne se rechargent jamais, et sur GL un rechargement laisse la pipeline sur un
programme supprimé). `BakeMailbox` sert quatre systèmes et `WaterSystem` le réécrit quatre
fois à la main, `ChunkOcclusion` et `RadianceCascades` une fois chacun. Le `FrameComposer`
est « le seul écrivain » du FrameUbo, sauf deux lanes patchées après lui ; le contrat des
81 lanes est écrit trois fois (struct C++, bloc GLSL, composer) et dérive déjà (quatre noms
divergents, quatre lanes mortes, dix-huit canaux `.w` au sens connu du seul composer).

Le second axe est celui des deux vagues précédentes, les **cas particuliers en tables
incomplètes** : `TextureFormat` a neuf tables de traits dans trois fichiers et deux
autorités d'octets qui se contredisent (R16F = 4 côté upload, 2 côté GPU, sans lien) ; la
table des 25 couches splat est écrite trois fois et la quatrième copie a divergé (bug
Q3-46) ; les sept familles de props sont des plages d'entiers comparées à la main en douze
sites, jusqu'au shader d'ombre qui exclut les cailloux par un seuil d'échelle ;
`scatterProps` répète cinq fois un squelette dont `PlantTier` est déjà la table pour deux
familles ; le « monté » du renderer est re-dérivé en 80 tests `cfg.*` et quatre listes
séquentielles des mêmes dix systèmes.

Troisième axe, **structurel et à décider par le dev** : le renderer héberge la vérité du
monde. `TerrainSystem::params` est la vérité sol lue par collision, nav et spawn ;
`WaterSystem` contient le cycle de vie de la sim d'eau (sept booléens d'état, trois copies
du bloc d'invalidation), 119 lignes d'hydrologie pure et 390 lignes de mesher pur ;
`TerrainNoise.{hpp,cpp}` est inclus par 43 TU dont 20 de `game/` sans dépendre de rien du
rendu. D'où les neuf accesseurs nus de `WorldRenderer` (`terrainParams()` mutable en 72
sites) et le pendant structurel du finding Q1 « `engine/terrain` en `namespace render::` ».
Ce n'est pas du spaghetti local, c'est une frontière mal placée : `RENDERING.md` §6 la
nomme déjà (RENDERER-EXTRACT).

Le RHI est sain dans son interface ; un seul fichier est malade, `VulkanDevice.cpp`
(4 913 l. en un TU, déjà découpé par bandeaux, `create()` 571 l. énumérant les familles de
files quatre fois).

Les positifs à ériger en modèles : `FarTerrain` (une `Config` pour deux instances,
`refreshPipeline` qui fusionne premier build et rebuild, `*this = {}`), `NoiseVolume`,
`FrameComposer`, `ChunkStreamer`/`BakeMailbox`, `ResidencyCache` avec ses deux traits,
`canopyLevel` (« ONE rule » de LOD), `materialWeightsCore` (une règle de poids, inputs
nommés), `PlantTier`, `GrassSpecies.hpp` (les espèces sont des données), `RcTuning` (knobs
séparés des ressources), `pipelineFor` Vulkan (variantes par clé, pas par `if`),
`clampStagesToQueue`, `GpuProbe`.

---

## Contrôles d'invariants

| Invariant | Verdict | Remarque |
|---|---|---|
| §2.1 : aucun appel GL/Vulkan hors `rhi/backends/` | **PASS** | grep sur `engine game world gameplay tools data quest script` : seul `engine/platform/common/VulkanSurface.cpp` (rôle plateforme, symétrique de `GlContext.cpp` accepté en juillet) ; ImGui = U2-07 ✖ |
| §3.1 : headers platform-clean, backend choisi au runtime | PASS | `VulkanDevice.hpp` pimpl sans `<vulkan/*>` ; `Device::create` + chaîne Vulkan→GL dans `Engine.cpp:59-70` |
| Décision 2026-07-12 « pas de down-port GL 4.1 » | PASS | `GlDevice41` = repli 2D vivant, caps 3D à `false`, aucune feature 3D ajoutée |
| Phase 5 : le renderer ne lit que snapshot + vue ; workers → files drainées sur main | PASS (forme) | 17 systèmes conformes ; mais le canal sim → renderer passe par `terrainParams()` mutable et 13 méthodes de `waterSystem()` (Q3-14) |
| §7 residency : handle + placeholder + upload main-thread | PASS | `ResidencyCache` unique, `MeshCache`/`TextureCache` = traits |
| FrameUbo append-only + verrou std140 (U3-3) | PASS (forme) / **dérive (fond)** | 80 `static_assert` ; 4 lanes mortes, 4 noms divergents, commentaires « reserved » périmés sur des lanes écrites (Q3-16) |
| « Un seul écrivain du FrameUbo » (`FrameComposer`) | **contourné** | `WorldRenderer.cpp:2388, 2466` écrivent `clusterInfo` et l'atlas après le composer (Q3-10) |
| U3-7 : `rhi::Unique` partout | **partiel** | 241 usages / 15 fichiers, mais dix classes en `destroy*` manuel, 107 appels (Q3-33) |
| Hot-reload « dérivé de l'usage » (`Watch`) | **partiel** | 12 `Watch` / 15 compteurs / 7 pipelines sans rien / 1 watch vide (Q3-15, Q3-43, Q3-44) |
| Règle dev « enum over nested ifs » | partiel | respectée (`FlyCamera::LookTrigger`, `levelMesh`, `canopyLevel`) ; violée (sim d'eau 7 booléens, `ssdmMode`/`debugMode`/`debugBuffer`/`waterDebug` en `i32`) |
| Règle dev « jamais un réglage dev perdu, bascule A/B » | **FAIL** (2 sites) | `kBakedDischargeBoost = 3.0f` « tune or revert on verdict » en `constexpr` ; « sobriety pass » = 3 branches GLSL mortes sans knob (Q3-26) |
| Règle dev « perf knobs dans l'UI » | partiel | `shadow.glsl 1/2048` ignore le knob de résolution (Q3-45) ; `RcTuning` exemplaire |
| Pas de finding perf sans mesure FrameProbe | respecté | aucune classe P ; « TerrainLightMap 2 min/bake » (mémoire) est périmé (le code dit ~1 s via `HeightField`, non mesuré) |
| Politique de commentaires 2026-07-22 | **FAIL** (systèmes) / PASS (RHI, racine) | ~195 tags dans `landscape/` + WorldRenderer (D5 seul : 79) ; 8 écarts sur 7 100 l. de RHI ; 6 sur 2 134 l. de racine |
| Arbitrages de juillet (README §0.2) | respectés | U2-01 (`.at`), U2-02 (`Handle<Tag>`), U2-07, U3-8, U3-9 non re-proposés ; U3-4 et U3-7 **clos à moitié** = preuve nouvelle (Q3-33, Q3-39) ; U2-04 (`ShaderDesc` GLSL) : Vulkan compile le GLSL via shaderc à l'exécution, la dette est **résorbée de fait**, à clore |

---

## Passe 1 — fichier par fichier

### `engine/render/WorldRenderer.{hpp,cpp}` (686 + 2 808 l.) — à refondre par parties

| zone / méthode | l. | contenu | verdict | findings |
|---|---|---|---|---|
| `.hpp` API publique | 132-258 | `terrainParams()` **mutable**, `waterSystem()`, `vegetationSystem()`…, `sculptRemeshQueue()`/`sculptScatterQueue()` = `vector<u64>&` nus, 2 `friend` lisant 12 privés | **à refondre** | Q3-14 |
| `.hpp` `RenderTuning` | 264-392 | 75 knobs, chacun avec son A/B ; `ssdmMode`/`debugBuffer`/`waterDebug` en `i32` | ok / à surveiller | Q3-27 |
| `.hpp` handles | 542-684 | 67 `rhi::Unique*`, chaîne SSDM = 22 membres, 5 compteurs `u64 *ShaderGeneration` | à refondre | Q3-8, Q3-15 |
| `create` | 258 | monte 10 systèmes sous 18 gates `cfg.*`, charge 15 shaders | à refondre | Q3-18, Q3-29 |
| `ensureOffscreenTarget` | 256 | recrée tout (HDR, copies, réflexion, SSDM 7 tex/7 fb/9 groupes/6 pipelines) sur 4 clés | à refondre | Q3-8, Q3-44 |
| `destroy` | 70 | 40 `.reset()` listés à la main qui omettent les 22 membres SSDM, sous un commentaire disant la liste superflue | à surveiller | Q3-33 |
| `drawWaterVolumes` | 141 | pipeline lazy + 2 registres mark/sweep | à surveiller | Q3-15, Q3-42 |
| `recordPostFx` | 141 | bloc SSDM 80 l. (lambda `fullscreen` + 3 blocs recopiés) | à refondre | Q3-8 |
| `recordMainPass` | 157 | 7 `if (group.id != 0) setBindGroup(n)`, pipeline pluie **reconstruit dans la passe** | à surveiller | Q3-15, Q3-42 |
| `pumpStreaming` | 152 | 13 gates ; deux `update` identiques de 12 l. (farTerrain/farHorizon) ; ternaire mort | à surveiller | Q3-36, Q3-50 |
| `pumpPipelinesAndRequests` | 80 | `refreshPipeline` ×10 sous 10 gates ; requêtes safe-point | ok (modèle) | Q3-18 |
| `render` | **591** | logs one-shot 40 l., CSM 70, auto-skip réflexion 40, `composeFrameUniforms` 160, **key-shadow 75**, **LightsUbo 90**, table des passes 70 | **à refondre** | Q3-2, Q3-42 |
| `recordGiUpdate` | 144 | listes d'injection GI, corps indenté d'un cran | ok | Q3-50 |

Infirmé par la lecture : `destroy()` fait 70 l. (pas 324), le bloc SSDM 100 + 80 l. (pas
540), « ~70 `cfg.*` » = **80 `cfg.*` + 24 `caps()` + 100 `tuning.*`**, `depthBias` 0
occurrence ici.

### `engine/render/landscape/` — eau et cartes cuites (lot D2)

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `WaterSystem.hpp` | 445 | mer + lacs/rubans bakés + eau lointaine + sim option C + fenêtres gelées + debug : 25 méthodes publiques, ~75 membres, 0 `Unique` | **à refondre** | Q3-3, Q3-5, Q3-23, Q3-33 |
| `WaterSystem.cpp` | 1 955 | `updateSim` **440** l. (9 phases, bloc d'invalidation ×3), `buildLocalGeometry` **390** l. (mesher pur, 12 lambdas), `bakedEntrySources` 119 l. (hydrologie pure), `buildPipeline` 138 l. (6 pipelines, même état ×5), `destroy` 85 l. (22 `destroy*` + 35 remises à zéro) | **à refondre** | Q3-3, Q3-5, Q3-23, Q3-26, Q3-27, Q3-28, Q3-33, Q3-43, Q3-49 |
| `FarTerrain.{hpp,cpp}` | 564 | anneau lointain worker-baké, 2 instances par `Config`, `BakeMailbox`, `Unique`, watch correcte | **propre — modèle** | Q3-39 (bilinéaire) |
| `HeightField.{hpp,cpp}` | 271 | pyramide de hauteurs 4 niveaux, snapshots immuables ; `Level` = `GridSpec + vector<f32>` | propre | Q3-28, Q3-39 |
| `MistMap.{hpp,cpp}` | 243 | carte vallée/plafond 256², `BakeMailbox` | propre | Q3-24, Q3-39 |
| `FireScorchMap.{hpp,cpp}` | 190 | masque feu + `FireLook` | propre | Q3-33 |
| `water_surface.glsl` | 655 | un shading pour 4 pipelines par `#define` (bonne direction) ; 14 `#ifdef` imbriqués, 3 branches « sobriety pass » mortes | à surveiller | Q3-26, Q3-49 |
| `water*.vert/frag` (9), `watervolume.*`, `mist.frag`, `far_terrain.*`, `firescorch/firemask.glsl` | 2-271 | prologues et includes ; constantes « must match » CPU ↔ GLSL | propre / à surveiller | Q3-16 |

Infirmé : `kickLocalGeometry` et `simMapInfo` font 19 et 12 l. (le scan avait pris des
lambdas pour des fonctions).

### `engine/render/landscape/` — terrain, herbe, occlusion (lot D3)

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `TerrainSystem.{hpp,cpp}` | 1 214 | chunks + pools par LOD + arrays de matériaux ; `buildMaterialArrays` 176 l. en trois chemins, `buildChunkVertices` 101 l. (ok) ; 100 % `Unique` | à surveiller | Q3-11, Q3-21, Q3-30, Q3-39, Q3-46 |
| `TerrainNoise.{hpp,cpp}` | 1 022 | la vérité CPU du relief et des poids (`materialWeightsCore` = LA règle) ; aucune fonction > 80 l. ; **zéro include render**, inclus par 43 TU | propre, **mal placé** | Q3-5, Q3-39 |
| `TerrainLightMap`, `TerrainShadeMap` | 226, 178 | cartes cuites en anneau (`BakeMailbox`) ; `destroy*` manuels 5 + 7 | copie de forme | Q3-24, Q3-33 |
| `SplatTextures.{hpp,cpp}` | 172 | table des 25 couches (constexpr) — **incomplète** (pas de `familyOf`) | propre | Q3-21 |
| `GrassSystem.{hpp,cpp}` + `GrassSpecies.hpp` | 974 | `scatterGrass` **395** l. en six phases nommées en commentaires ; espèces = 3 tables de données ; 100 % `Unique` | **à refondre** (une fonction) | Q3-4, Q3-30 |
| `ChunkOcclusion.{hpp,cpp}` | 292 | horizon CPU pur et testé ; mailbox worker réécrite à la main | à surveiller | Q3-23, Q3-39 |
| `GpuOcclusion.{hpp,cpp}` | 418 | Hi-Z compute sans readback, barrières explicites ; 14 `destroy*`, 3 compteurs de génération | propre, handles nus | Q3-15, Q3-33 |
| `ChunkStreamer.hpp`, `BakeMailbox.hpp` | 178, 90 | les deux mécanismes partagés (U3-1 clos) ; n'incluent que `engine/core` | propre, **à descendre** | Q3-23, Q3-39 |
| `terrain.frag` + 6 includes, `grass.*`, `chunk_cull/hiz_*.comp`, `contactshadow.frag` | 1 420 | 10 littéraux de couche dans `terrain.frag`, `hexFamilyLayer` = 2e écriture de la table | dense, cohérent | Q3-21, Q3-16 |

Infirmé : `regionShadingAt` fait 30 l. (pas 242) ; la bilinéaire par brin de `GrassSystem`
est locale au chunk (pas une copie de `GridOps`) ; « TerrainLightMap 2 min » périmé.

### `engine/render/landscape/` — végétation et arbres (lot D4)

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `VegetationSystem.hpp` | 551 | une classe, sept métiers, ~60 membres ; BOM UTF-8 (seul du dossier) | à surveiller | Q3-12, Q3-20 |
| `VegetationSystem.cpp` | 563 | `canopyLevel` « ONE rule » + `levelMesh` switch plat (17 l.) ; mais `drawDepth` tient un second choix de LOD, formules LOD/groupe/AABB ×3 chemins | à surveiller | Q3-38 |
| `VegetationScatter.cpp` | 502 | `scatterProps` **447** l. : 5 copies d'un squelette + 1 table `PlantTier` ; worker pur | **à refondre** (table) | Q3-19, Q3-20, Q3-22 |
| `VegetationAssets.cpp` | 614 | meshes + LOD, écorce, masque de feuilles, reseed async (**sans `bushSpecies`**, overrides ignorés) | à surveiller | Q3-38, Q3-42 |
| `VegetationStreaming.cpp` | 270 | pool first-fit + cooling, `ChunkStreamer` partagé ; `giKind` en ternaire sur `kFirst*` | propre | Q3-20 |
| `TreeGenerator.{hpp,cpp}`, `SpaceColonizationTree.cpp` | 391, 837 | 46 knobs documentés ; `growColonizedTree` ~200 l. en 4 phases nommées (ok) ; `appendWood` ~290 l. avec LOD en 6 ternaires parallèles ; forme de feuille en if-chain | à surveiller | Q3-30, Q3-42 |
| `tree.vert/frag` | 437 | 4 drapeaux en bits de signe décodés ; `main` ~200 l. en 4 blocs | à surveiller | Q3-22, Q3-16 |
| `shadow_prop.vert/frag` | 100 | **copie** de 45 l. de `tree.vert` + `step(0.35, scale)` ; cutout copié de `tree.frag` | **à refondre** (include) | Q3-35, Q3-20 |
| `skinned.frag` vs `mesh.frag` | 57 / 65 | corps identiques, cinq « KEEP IN SYNC » | à refondre (include) | Q3-35 |
| `far_tree.*`, `shadow_mesh/skinned/terrain.*` | 115, 61 | 4200/5200 retapés depuis `FarTerrain::Config.treeFar` ; trois `.frag` vides identiques | à surveiller / H | Q3-16, Q3-35 |

Infirmé : `levelMesh` fait 17 l. (pas 407) ; « 6 boucles » = 5 copies + 1 table ; seul
`shadow_prop` duplique vraiment un shader ; la rafale de vent n'existe qu'en GLSL.

### `engine/render/landscape/` + racine — lumière, GI, post, ciel, FX, FrameUbo (lot D5)

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `FrameUniforms.hpp` ↔ `common.glsl` | 348 / 190 | 81 lanes, 80 `static_assert`, miroir « par commentaire » ; 4 noms divergents, 4 lanes mortes, 18 canaux cachés | à surveiller | Q3-16 |
| `FrameComposer.{hpp,cpp}` | 485 | composition pure, headless ; `composeFrameUniforms` 283 l. ; politique base/resolved en 12 commentaires ; littéraux 48/800/2 400/1 400/2 500 | propre, à surveiller | Q3-10, Q3-50 |
| `RadianceCascades.{hpp,cpp}` | 1 303 | `RcTuning` exemplaire ; `update` **357** l. (snap copié verbatim de `prepare`, worker de 120 l. en lambda, `getenv` dans le chemin chaud) ; 5 compteurs + `probe` jamais rechargé ; `destroy()` incomplet | **à refondre** (`update`) | Q3-9, Q3-15, Q3-23, Q3-33, Q3-50 |
| `PostFx.{hpp,cpp}` | 1 139 | 70 handles `Unique` à plat = cinq copies de la « famille cible ½-res » ; `renderMist`/`renderSkyClouds` quasi identiques ; `destroyTargets` 56 `x = {}` redondants | **à refondre** | Q3-34 |
| `SkySystem.{hpp,cpp}` | 346 | palette pure (~22 littéraux `Vec3`), dôme, bake ; 8 `destroy*` ; `*this = {}` réinitialise `timeOfDay` | à surveiller | Q3-32, Q3-33 |
| `ShadowMapper.{hpp,cpp}` | 214 | 3 cascades, snap texel ; 6 `destroy*` | propre | Q3-33, Q3-45 |
| `LightClusters.{hpp,cpp}` | 103 | 100 l., une responsabilité ; 3 `destroy*`, 1 compteur ; `kClusterX/Y/Z` ↔ GLSL par commentaire | propre | Q3-15, Q3-16, Q3-33 |
| `NoiseVolume.{hpp,cpp}` | 113 | 100 % `Unique`, `destroy() = *this = {}`, fallback analytique | **propre — modèle** | — |
| `FxRenderer.{hpp,cpp}` + `FxInstance.hpp` | 255 | `ensurePipelines` en table `make(blend, shader)` | propre | — |
| `GpuProbe.{hpp,cpp}` | 288 | anneau 4 frames, back-pressure, `Scope` null-tolérant | **propre** | — |
| `AtmosphereParams.hpp` | 48 | 24 scalaires (le commentaire dit 19) = 3e écriture des champs météo | sain | Q3-41 |
| ~60 shaders (`rc_*`, `froxel_*`, `bloom_*`, `tonemap`, `ssao`, `ssdm_*`, `sky*`, `clouds`, `fx*`, `rain`, includes) | ~3 500 | bibliothèque d'includes solide (`temporal_resolve`, `volumetric_media`, `clusters`, `view_util`) ; copies résiduelles (`windowBeam` verbatim, `giSlabDir`↔`rcOctDecode`, 3 bruits 2D, 2 bruits 3D, projection CSM ×2, luma ×6, coins de billboard ×3) | bonne base | Q3-35, Q3-16 |

Infirmé : `destroyTargets` 56 l. (pas 310), `clearSkyClouds` 19 l. (pas 198),
`createVolumes` 123 l. (pas 264) ; « aucun `.w` libre dans le FrameUbo » (mémoire) est
**faux** : au moins neuf lanes libres documentées ; `GpuProbe` ne duplique pas les
backends ; le chemin sans compute des nuages volumétriques est correctement gaté.

### `engine/rhi/` (lot D6)

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `Rhi.hpp`, `Device.{hpp,cpp}`, `CommandBuffer.hpp` | 475, 156, 101 | interface explicite (PSO, bind groups, passes, barrières scopées, async compute, caps par feature), contrats au bon endroit ; factory runtime | **bon** | Q3-17, Q3-48, Q3-49 |
| `UniqueHandle.{hpp,cpp}` | 110 | RAII, 6 alias, 241 usages ; `UniqueShader` manque | bon | Q3-33 |
| `gl/GlConvert.hpp`, `GlDevice.{hpp,cpp}` | 17, 133 | U2-06 livré ; factory 4.6 → 4.1 | bon | — |
| `gl/GlDevice41.{hpp,cpp}` | 191 | repli 2D **vivant** ; stubs qui `LOG_ERROR` à chaque appel | bon | Q3-50 |
| `gl/GlDevice46.{hpp,cpp}` | 384 | DSA ; 3 tables de format locales (dont la whitelist d'upload où « R32F/RGBA16F were MISSING ») | bon | Q3-17 |
| `gl/GlDeviceBase.{hpp,cpp}` | 978 | command buffer sans branche, registres, fences ; 24 `.at(` (U2-01 ✖) ; `copyTexture` 2D seul | bon | Q3-31, Q3-48 |
| `vulkan/VulkanDevice.hpp` | 96 | pimpl, aucun type Vulkan | bon | — |
| `vulkan/VulkanDevice.cpp` | **4 913** | `create` **571** l. (14 phases, familles ×4), `createTexture` ~330 (4 chemins d'upload, séquence immédiate ×2), `createShader` ~200 (dont parseur legacy), `pipelineFor` ~190 (lisible), `endFrame` ~190, `pushGroup` 172, `beginFrame` ~150, `remapBindings` 144 (legacy), `bindMissingDummies` 139, dtor 138, `flushPendingFrees` 133 | **à découper, pas à réécrire** | Q3-1, Q3-6, Q3-7, Q3-25, Q3-37, Q3-42, Q3-48 |

Infirmé : conversions `BlendMode`/`CompareOp` « dupliquées » sont des traductions par API ;
le cycle fences/timestamps diffère par nature (seul le registre single-use est commun, et
la divergence réelle est contractuelle, Q3-48) ; `GlDevice41` n'est pas mort ;
`UniqueHandle` n'est pas lacunaire (sauf `UniqueShader`), les `destroy*` manuels sont des
migrations render non faites.

### Racine `engine/render/` hors orchestrateur (lot D7)

| fichier | lignes | rôle | verdict | findings |
|---|---|---|---|---|
| `ShaderLibrary.{hpp,cpp}` | 372 | includes expansés (liste de fichiers surveillés dérivée), poll 2 Hz, générations, `Watch` dérivé de l'usage ; bracket `begin/end` manuel, imbrication effacée sans assert, watch vide ne tire jamais | **à durcir** | Q3-15, Q3-47 |
| `SpriteRenderer.{hpp,cpp}` | 330 | 2D vivant (2 scènes) ; **seul shader embarqué en C++** du dépôt, `#ifdef VULKAN` à la main, 8 `destroy*` | sain, à migrer | Q3-40, Q3-33 |
| `MeshBuilder.{hpp,cpp}` | 327 | primitives déterministes ; `angleAt` ×2, base orthonormale ×3 | propre | Q3-42 |
| `MeshCache`, `TextureCache`, `ResidencyCache.hpp` | 196, 109, 199 | traits sur `ResidencyCache` — pas de recopie | **propre — modèle** | — |
| `MeshVertexLayout.hpp` | 39 | U3-5 livré ; layout skinned encore inline ×2 dans WorldRenderer | propre | Q3-42 |
| `SceneView.hpp` | 138 | `RenderSnapshot` POD ; inclut `SpriteRenderer.hpp` (donc `Device.hpp`) pour un POD de 6 champs, tiré par `game/SceneSubmit.hpp` et `NpcDirector.hpp` | sain | Q3-40 |
| `Camera2D/3D`, `FlyCamera`, `Frustum`, `Projection` | 39-121 | reversed-Z, enum + switch plat, header-only testés (`FrustumTest`) | **propre** | Q3-49 (« ProjectionTest » inexistant) |

### Hot-reload des shaders — table exhaustive (mission transversale du lot D7)

| système | pipelines | mécanisme | site |
|---|---|---|---|
| `TerrainSystem` | lit + caster | `Watch` ×2 | `TerrainSystem.cpp:653-692` |
| `VegetationSystem` | tree + prop caster | `Watch` ×2 | `VegetationSystem.cpp:47-112` |
| `GrassSystem` | grass | `Watch` | `GrassSystem.cpp:589-629` |
| `FarTerrain` ×2 | far_terrain + far_tree | `Watch` + garde `id == 0` contre le double `load()` | `FarTerrain.cpp:30-39, 73-106` |
| `SkySystem` | sky, cloud_bake | `Watch` ×2, bake aussi sur `id == 0` | `SkySystem.cpp:194-224` |
| `WaterSystem` | 5 pipelines | `Watch` ×1 — **vide** (`beginWatch` dans le `if`) | `WaterSystem.cpp:1735-1738, 1870-1877` |
| `PostFx` | 15 pipelines | `Watch` ×1 via lambda `rebuild` (table) | `PostFx.cpp:154-195` |
| `FxRenderer` | 4 | `Watch` + `id() != 0` | `FxRenderer.cpp:40-62` |
| `WorldRenderer` casters | 2 | `Watch` | `WorldRenderer.cpp:1079-1127` |
| `WorldRenderer` | mesh, skinned, watervolume, rain, blit | **5 compteurs à la main**, testés au site de draw (pluie dans la passe) | `.cpp:711-950, 1592-1601, 2608` |
| `WorldRenderer` SSDM | 6 | **aucun** (bâtis au resize) | `.cpp:625-638` |
| `RadianceCascades` | inject, build, merge, extend, debug ; probe | **5 compteurs** ; probe **jamais** rechargé | `RadianceCascades.cpp:143-184` |
| `GpuOcclusion` | 3 | **3 compteurs** via lambda `refresh` | `GpuOcclusion.cpp:111-127` |
| `LightClusters` | 1 | **1 compteur** | `LightClusters.cpp:27-37` |
| `NoiseVolume` | bake unique | aucun — acceptable | — |
| `ui::UiSystem` | ui | **1 compteur** | `UiSystem.cpp:104-119, 627` |

Bilan : 12 `Watch` dans 9 systèmes, 15 compteurs à la main dans 5 systèmes, 7 pipelines
sans mécanisme, 1 watch inopérante (5 pipelines). Les systèmes ont contourné `Watch` pour
trois raisons lisibles dans le code : antériorité (compteurs d'avant la revue d'août),
`Watch` ne couvre pas le premier build (« an EMPTY watch never fires »), et le bracket
`beginWatch/endWatch` sans RAII ni assert ne protège ni de l'oubli ni de l'imbrication.

---

## Passe 2 — système par système

### S1. Le hot-reload : un mécanisme, trois usages, sept trous (Q3-15, Q3-43, Q3-44, Q3-47)

**Pattern.** Le même triplet (handle de pipeline, gate de génération, recette de
construction) est écrit de trois façons (table ci-dessus). Les deux bugs (Q3-43 eau,
Q3-44 SSDM + `rc_probe`) sont exactement la classe que le commentaire de `Watch`
(`ShaderLibrary.hpp:56-63`) annonçait vouloir tuer, et qui a ressurgi parce que le bracket
reste manuel. Deux failles d'API l'aggravent : `beginWatch()` imbriqué efface la liste de
l'appelant sans assert (`:82-85`) ; un `Watch` vide ne tire jamais, ce qui force chaque
consommateur à écrire son propre `|| pipeline.id() == 0` (`FarTerrain.cpp:75`,
`FxRenderer.cpp:41`, `SkySystem.cpp:215`, `WorldRenderer.cpp:937, 1593`). Troisième piège :
`ShaderLibrary::load()` ré-appelé sur un nom existant retire le programme sans bump de
génération (Q3-47), ce que `FarTerrain.cpp:30-39` contourne explicitement.

**Mécanisme à étendre.** `ShaderLibrary::Watch` + `rhi::UniquePipeline`.

**Croquis.** `render::WatchedPipeline { UniquePipeline p; Watch w; Build build; ensure(device,
shaders) }` : rebâtit au premier appel OU quand une génération consommée a bougé, asserte si
la recette n'a consommé aucun shader (watch vide = bug) ; `beginWatch/endWatch` deviennent
privés avec `ENGINE_ASSERT(!watching)`. Migration mécanique de **59 pipelines**
(Terrain 2, Vegetation 2, Grass 1, FarTerrain 2, Sky 2, Water 5, PostFx 15, Fx 4,
WorldRenderer 5 + 2 + 6, RC 6, GpuOcclusion 3, LightClusters 1, UiSystem 1), −15 compteurs,
−12 champs `Watch`, ~110 l. de glue. Dans RC et WorldRenderer, une table `{ nom, &pipeline,
recette }` remplace les blocs. Deux recettes lisent un état (`wireframe`
`TerrainSystem.cpp:664`, `froxelScatter[0]` `PostFx.cpp:176`) : à capturer. **Préalable** :
la migration `Unique` des systèmes restants (S3).

**Risque.** Nul visuellement (mêmes descripteurs) ; le premier build glisse de `create()` à
la première frame. GL/Vulkan neutres. Court terme indépendant : sortir le `beginWatch()` du
`if` (une ligne) corrige Q3-43 seul.

### S2. Le FrameUbo et les constantes partagées CPU ↔ GLSL : un contrat écrit trois fois (Q3-16, Q3-10)

**Pattern.** `FrameUniforms.hpp` (81 membres + 80 `static_assert`), `common.glsl` (81
déclarations, « mirrors this struct by COMMENT only »), `FrameComposer.cpp` (le sens réel
de chaque lane). Dérives mesurées : 4 noms divergents (`stylizedDiffuseInfo` ↔
`uStylizedDiffuse`, `keyShadowAtlasViewProj` ↔ `uKeyShadowAtlas`…) ; commentaires « yzw
reserved » périmés sur `splatDetailInfo`, `splatVarietyInfo`, `ssaoInfo`, `windDirInfo`
que le composer écrit (`FrameComposer.cpp:118-146`) ; 4 lanes mortes gardées append-only
(`keyShadowViewProj` 64 o, `keyShadowInfo`, `waterReserved0`, `time.y`) ; 18 canaux `.w`/`.z`
dont le sens n'est qu'au composer (`sunDirection.w = dt`, `horizonFarColor.w =
autoExposureMin`, `cascadeSplits.w = interior`…). Les systèmes squattent des lanes
d'autres systèmes : gate écorce = `uSplatVarietyInfo.w` (`tree.frag:175`), drapeau intérieur
= `uCascadeSplits.w` (`mesh.frag:33`), reach impostor = `uFogLayerInfo.w` (`tree.vert:38`).
À côté, les constantes appariées **par commentaire** : `kClusterX/Y/Z` ↔ `clusters.glsl`,
`kMaxLights = 24` ↔ `locallights.glsl`, `kMaxWaterMaterials = 16` ↔ `uWaterMaterials[16]`,
`MistMap::kTopOffset/kTopRange` ↔ `mist.frag:38-39`, `treeFar 5200` ↔ `far_tree.vert:40`,
`kGrassSpeciesCount 6` ↔ `grass.vert:111` et `common.glsl:150`, résolution d'ombre ↔
`shadow.glsl:46` (bug Q3-45), demi-portée `9000` ×4 (`WorldRenderer.cpp:2345`,
`FrameComposer.hpp:142`, `FrameUniforms.hpp:254`, `water.vert:20`). Enfin `WorldRenderer`
patche deux lanes **après** le composer (`:2388 clusterInfo`, `:2466 keyShadowAtlasViewProj`)
alors que `FrameComposer.hpp` promet « pure values in, FrameUniforms out » (Q3-10).

**Mécanisme à étendre.** Le verrou U3-3 (`static_assert`) + l'expansion d'includes de
`ShaderLibrary` qui injecte déjà `#define VULKAN 1`.

**Croquis.** Une **table de lanes** X-macro `MEADOWS_FRAME_LANES(X)` avec colonnes type,
nom, portée (Base/Resolved), doc → génère la struct, les offsets, et `frame_ubo.glsl`
servi par `ShaderLibrary` ; un préambule généré depuis les `constexpr` C++
(`MEADOWS_CLUSTER_X`, `MEADOWS_GRASS_SPECIES`, `MEADOWS_MAX_LIGHTS`…) ou un
`constants.glsl` écrit par le build ; `clusterInfo` et l'atlas entrent dans
`FrameComposerInputs`. C'est le `FieldLane` de T-1 côté GPU (T-15). Les lanes squattées
reçoivent un `uVegInfo` en fin d'UBO.

**Risque.** Nul si les offsets générés sont identiques (le `static_assert` final le prouve) ;
bit-exact. Effort L, mais c'est la seule généralisation de la vague qui ferme une classe
entière de bugs silencieux (« one mid-struct insertion desyncs ~14 shaders », RENDERING §2.3).

### S3. La propriété des handles : une migration arrêtée à mi-chemin (Q3-33, Q3-34)

**Pattern.** `rhi::Unique` est adopté à 100 % dans WorldRenderer (67), PostFx (70), RC (26),
Vegetation (25), Terrain (10), FxRenderer (8), NoiseVolume (6), FarTerrain (5), Grass (4).
Dix classes n'ont pas suivi : `WaterSystem` **45** `device.destroy*` (0 `Unique`),
`GpuOcclusion` 14, `SkySystem` 8, `SpriteRenderer` 8, `TerrainShadeMap` 7, `ShadowMapper` 6,
`FireScorchMap` 6, `GpuProbe` 6 (légitime : handle à usage unique), `TerrainLightMap` 5,
`MistMap` 5, `MeshCache` 4 (légitime : traits), `LightClusters` 3 — 107 appels. Conséquence
directe : des `destroy()` en listes manuelles qui dérivent — `WaterSystem::destroy` 85 l.
oublie six scalaires ; `RadianceCascades::destroy` oublie `extendPipeline`, `probePipeline`,
`probeBuffer`, `probeGroup`, `havePrev`, `appliedCascadeCount` ; `WorldRenderer::destroy`
omet les 22 membres SSDM sous un commentaire disant la liste superflue ; `PostFx::destroy`
fait `*this = {}` mais réinitialise alors quatre knobs logés dans la classe ressource.
`PostFx` lui-même, bien que 100 % `Unique`, porte **cinq copies de la « famille cible
½-res »** à plat (Q3-34) : `{tex, fb, group}` ×12, `{historyTex, historyFb, historyGroup,
temporalUbo, prevViewProj, prevCamera, frame, historyValid}` ×3 (mist, skyCloud, froxel),
`{raw, blur}` ×2 ; `renderMist` (66 l.) et `renderSkyClouds` (57 l.) quasi identiques ;
l'alpha temporel recopié 3× ; `destroyTargets` = 56 `x = {}` que `resize` rend redondants.

**Mécanisme à étendre.** `rhi::Unique` (U3-7) et l'idiome `FarTerrain::destroy` /
`NoiseVolume::destroy` = `*this = X {}` (+ mailbox sauvée) ; `RcTuning` comme modèle de
séparation knobs/ressources.

**Croquis.** Les dix classes passent en `Unique` (déclarer les groups après les textures :
destruction en ordre inverse) ; `UniqueShader` ajouté ; `PostFxTuning` extrait puis
`*this = {}` ; `struct HalfResTarget { tex, fb; create(); clear(); }`, `struct
TemporalHistory { ubo, prevViewProj, prevCamera, frame, valid; f32 begin(blend, reach) }`,
`struct MediaPass { out, history, blur, temporal }` : ~70 membres → ~15.

**Risque.** Nul (mêmes textures, mêmes passes) ; capture A/B pour `PostFx` par règle dev
« graphics need visual validation ».

### S4. Les mailbox et anneaux de bake : les mécanismes existent, trois systèmes les réécrivent (Q3-23, Q3-24, Q3-9)

**Pattern.** `BakeMailbox<T>` (`create/kick/drain/busy/ready/reset`, génération, flag
`stop`) sert `HeightField`, `MistMap`, `FarTerrain`, `TerrainLightMap`, `TerrainShadeMap`.
`WaterSystem` le réécrit **quatre fois** à la main (`Shared{ baked, simDone, localMesh,
farMesh }` + 4 booléens `inFlight` + `generation` + 5 `isStopping()` + 4 boucles `tryPop …
if (gen != generation) continue`, `WaterSystem.hpp:269-274`, `.cpp:876, 896, 952, 1255`) ;
`ChunkOcclusion` une fois (`Shared`, `generation`, `inFlight`, `pump()`, `rebuild()`,
`ChunkOcclusion.hpp:88-98`) ; `RadianceCascades::pumpTileBake` une fois (« the
TerrainLightMap pattern », annoncé dans son en-tête). Les quatre « cartes cuites en anneau »
qui utilisent bien `BakeMailbox` recopient la moitié non partagée : `kSize/kSpan` (3072 ×3),
`center`, le test `strayed = !mailbox.ready() || distance(want, center) > kSpan * 0.25f`
(`TerrainLightMap.cpp:128`, `TerrainShadeMap.cpp:60`, `MistMap.cpp:101`,
`FarTerrain.cpp:141`), le `drain` qui détruit texture+group puis recrée, `destroy()` en
liste manuelle. Le coalesceur de `contentStamp` « (FarTerrain pattern) » est recopié dans
`WaterSystem.cpp:975-985`.

**Mécanisme à étendre.** `BakeMailbox` et `ChunkStreamer` — qui n'incluent que
`engine/core/{Clock, ConcurrentQueue, Defines, Jobs}` : **T-6 confirmé**, ils descendent tels
quels dans `engine/core/` (namespace `core`), sans `forEachIndexed`.

**Croquis.** Trois lanes de `WaterSystem` → `BakeMailbox<BakedMap/LocalMesh/FarMesh>` ; la
lane sim garde son `epoch` dans le payload (`drain` avec prédicat) ; `ChunkOcclusion` →
`BakeMailbox<Result>` ; `template <Baked> class RingBakedMap { BakeMailbox<Baked>; Vec2
center; f32 span; bool strayed(want); void land(device, …) }` pour les quatre cartes ;
`core::QuietStamp{seen, since}` pour le coalesceur. Dans RC, le worker de 120 l. en lambda
devient `static BakedTile bakeTile(...)` testable headless, et le snap des origines copié
verbatim entre `prepare` (`:566-595`) et `update` (`:673-700`) devient `snapOrigins()`.

**Risque.** Aucun côté image ; threading inchangé (une mailbox = un job en vol, Phase 5).

### S5. Les tables incomplètes : formats, couches splat, familles de props, règles de scatter (Q3-17, Q3-21, Q3-19, Q3-20, Q3-22, Q3-30, Q3-46)

**Pattern.** Même diagnostic qu'en Q1 (`PoiType`) et Q2 (vocabulaires) : la table existe à
moitié, les colonnes manquantes sont des `if` ailleurs.

- **`TextureFormat`** (Q3-17) : neuf tables dans trois fichiers — `Rhi.hpp` `formatBlockBytes`
  (octets d'upload, R16F = 4, commenté « the single authority ») et `isBlockCompressed` ;
  `GlDevice46.cpp` `toGlInternalFormat`, `acceptsPixelUpload` (whitelist où « R32F/RGBA16F
  were MISSING here » : la famille d'artefacts d'eau venait d'une ligne manquante),
  `uploadFormatFor` ; `GlDeviceBase.cpp` `imageFormat` ; `VulkanDevice.cpp` `toVkFormat`,
  `isDepthFormat`, `bytesPerTexel` (octets GPU, **R16F = 2**, contredit la ligne 1 par design
  sans lien), prédicat storage inline. Ajouter un format = neuf sites, l'oubli est silencieux
  (`default` GL → `GL_RGBA8`). La validation des descs est **éclatée** en sous-ensembles
  différents : GL refuse « buffer ET texture » dans un bind group, Vulkan n'a aucune
  validation ; `createTexture` vérifie des règles backend-agnostiques différentes de chaque
  côté. → `constexpr FormatTraits kFormatTraits[] { uploadBytes, gpuBytes, bc, depth, srgb,
  uploadable, storage }` + `static_assert(size == count)` dans `Rhi.hpp` ; chaque backend ne
  garde que sa colonne API ; `rhi::validateTextureDesc` / `validateBindGroup` dans
  `Device.cpp` appelés en tête des deux backends. **T-4.**
- **Couches splat** (Q3-21, bug Q3-46) : `SplatTextures.hpp:30-74` (référence constexpr),
  `terrain_zones.glsl:71-100 hexFamilyLayer` (chaîne `if (family == …)` avec littéraux
  `19`, `20`, `16`, `17..18`, `7+g`…), dix littéraux `21.0/22.0/23.0/24.0/2.0` dans
  `terrain.frag`, et `TerrainSystem.cpp familyColor` (`if (layer < 8) grass ; < 11 rock ; <
  13 snow ; else sand`) **faux** pour 17-24 → le placeholder sans `.mtex` peint falaise,
  roche, neige et dépôts en sable. → `constexpr array<SplatLayerInfo, kSplatArrayLayers>
  kSplatLayers { family, variant }` ; `familyOf(layer)`, `variantLayer(family, variant)`
  dérivés ; côté GLSL un bloc `const int kLayer…` unique inclus par les deux shaders (ou
  généré, S2).
- **Familles de props** (Q3-20) : `kFirstRock/Bush/Debris/Plant/Mass/Pebble` redérivées par
  arithmétique en ~12 sites (`VegetationSystem.cpp:146, 453`, `VegetationStreaming.cpp:195-197`
  kind GI en ternaire, `VegetationAssets.cpp:143, 183, 190`, `barkParamsFor`) et
  `shadow_prop.vert:42 step(0.35, aPosScale.w)` : « les cailloux ne projettent pas d'ombre »
  exprimé par un **seuil d'échelle** (le même 0.35 que `game/VegetationCollision.cpp`), cassé
  le jour où un rocher nain ou un gros caillou apparaît. → `enum class PropFamily` +
  `constexpr PropFamilyTraits kFamilies[] { first, count, castsShadow, giKind,
  heroRadiusOverride, texturedRigid, aoStrength, hasLodTwins }` avec `static_assert` sur la
  somme = `kVariantCount` ; le shader reçoit le drapeau par l'instance.
- **`scatterProps`** (Q3-19) : cinq copies du squelette `perSide → candidateRng → jitter →
  height/normal/slope → gates (mer, pente, eau locale) → acceptation → place()`
  (`VegetationScatter.cpp:103-180` arbres, `:183-222` rochers, `:225-269` cailloux,
  `:273-383` débris hors `place()`, `:465-500` buissons) à côté de la table `PlantTier {
  salt, spacing, fade, firstVariant, scaleMin, accept[4] }` qui couvre déjà plantes et masse
  (`:386-462`). Ce qui diffère réellement : 3 marges, la fonction d'acceptation, la plage
  d'échelle, le reach, l'enfoncement Y, le drapeau texturé/rigide, le choix de variante. →
  `ScatterRule[]` étendant `PlantTier`, une boucle, un hook `preAccept` pour les débris ;
  les colonnes sont les champs du futur `ScatterPropForm` (Q2, T-11).
- **Protocole de drapeaux** (Q3-22) : rigide = `params.z < 0`, texturé = `params.w < 0`,
  carte feuille = `uv.x < -5` avec slot en pas de −20, écorce = `uv.y < -0.5`, conifère =
  `aParams.z < 0` — définis dans `VegetationScatter.cpp:88-90` (recopié pour les débris
  `:370-372`), `SpaceColonizationTree.cpp:88-96`, `TreeGenerator.cpp:140`, décodés dans
  `tree.vert:45-57,130`, `shadow_prop.vert:27-31`, `tree.frag:107,143,175`,
  `far_tree.frag:36`. Aucune constante commune. → `PropEncoding.hpp` + `prop_encoding.glsl`
  (`isLeafCard`, `cardSlot`, `isTextured`, `isRigid`) ; à terme un lane entier de bits.
- **Petites tables** (Q3-30) : biais d'espèce d'herbe en `switch (variantA)` ×4 cas → colonne
  `kGrassSpeciesZoneBias[variant][species]` (`GrassSystem.cpp:271-293`) ; bandes de LOD
  terrain en 4 `if` + `kLod3CoreSide = 13 // 2*6+1` dérivé à la main → `kLodBands { 1, 3, 6,
  12 }` (`TerrainSystem.hpp:69-79`) ; échelle de LOD d'`appendWood` en six ternaires
  parallèles sur `detail` → `DetailProfile kDetail[4]` (`SpaceColonizationTree.cpp:338-362`) ;
  forme de feuille en if-chain → `kLeafShapes[5]` (`:766, 800-813`).

**Risque.** Bit-exact pour toutes (mêmes valeurs ; `x * 1.0f == x` ; ordre des tirages RNG
conservé si `variant, scale, yaw, tint, phase` reste celui de `place()`) ; golden
`VegetationScatterTest` (hash `10031847806692189656`) et `TreeGeneratorTest` sont les
preuves ; le placeholder change de couleur pour 17-24 = la correction du bug.

### S6. Le « monté », les anneaux et les passes : l'orchestrateur re-dérive au lieu de résoudre (Q3-18, Q3-2, Q3-8, Q3-36, Q3-27, Q3-29)

**Pattern.** `WorldRenderer` teste `cfg.*` **80 fois** (`create` 18, `pumpPipelinesAndRequests`
17, `pumpStreaming` 13, `render` 10, `recordMainPass` 7, autres 15) et `caps()` 24 fois, avec
des conjonctions recopiées (`cfg.water && caps().copyTexture` ×3 ; `cfg.postFx &&
offscreenTargets && hdrFormats && copyTexture` ×2 ; deux formes différentes de la gate
d'occlusion `:244` vs `:1394`). Les quatre verbes de cycle de vie sont **quatre listes
séquentielles des mêmes dix systèmes** (create `:103-361`, destroy `:363-432`,
refreshPipeline `:1988-2010`, regenerate `:2030-2043`), l'ordre des dépendances implicite
(cluster SSBO avant le frame group, `terrain.params.seed` avant `vegetation.create`). Les
deux anneaux lointains (`farTerrain`/`farHorizon`) sont le même code ×5 verbes (`update` ×2
**identiques à 12 l.**, `vegetation.treeSilhouette()` — 143 l. — recalculé deux fois par
frame) avec les demi-spans dérivés à la main (`28800.0f : 9000.0f` = 57 600/2 et 18 000/2
de `FarTerrain::Config`) et `horizonReach()` à 3 cas. `render()` fait 591 l. parce que
**trois décisions pures** n'ont pas suivi `FrameComposer` : sélection des 4 lumières
key-shadow (`:2398-2462`, 75 l.), remplissage `LightsUniforms` (`:2470-2560`, 90 l.),
auto-skip réflexion (`:2178-2208`, 40 l.) — chacune ne lit que snapshot/vue et n'écrit qu'une
valeur. SSDM est le **seul effet d'écran resté dans l'orchestrateur** (22 membres, 100 l.
de création, 80 l. d'enregistrement, 2 clés de recréation) alors que bloom, god-rays, mist,
SSAO, contact et auto-expo vivent dans `PostFx`. Les pipelines sont décrits à la main dans
tout `render/` : `CompareFunc::Greater` ×16 + `// reversed-Z` ×6, caster `Less + depthBias
4.0f + depthBiasSlope 2.5f` ×6 copies identiques (Terrain, Vegetation, Water ×4),
`{ "FrameUbo", 0 }` ×44 dans 11 fichiers, slots 3-9 liés par 7 `if` recopiés entre
`recordMainPass` et `recordReflection`. Les modes sont des `i32` : `ssdmMode` 0/1/2 en 4
ternaires, `debugBuffer`, `waterDebug`, `SimConfig::debugMode` 0..3 testé en 8 sites C++ et
2 GLSL.

**Mécanisme à étendre.** `RendererConfig` (R3 juillet), `FrameComposer`, `PostFx`,
`FarTerrain::Config`, les `record*` « une méthode par passe », la règle dev « enum over
nested ifs ».

**Croquis.** (a) `struct Mounted = cfg ∧ caps` résolu une fois dans `create()`, seul testé
ensuite ; (b) table ordonnée `kSystems[] = { {&terrain, mounted.terrain, create/destroy/
refresh/regenerate} … }` (pointeurs de méthode, **pas de `virtual`**) parcourue par les quatre
verbes, l'ordre portant les dépendances en commentaire ; (c) `pickKeyShadows(...) ->
vector<KeyShadowPick>`, `fillLightsUbo(...) -> LightsUniforms`, `reflectionWanted(...)` en
fonctions libres ou dans `FrameComposer` : `render()` tombe à ~250 l. ; (d) `SsdmChain` sur
le modèle `PostFx` (`create/resize/refreshPipelines/record`), `ensureOffscreenTarget` tombe
à ~150 l. ; (e) `array<FarTerrain, 2> farRings`, `FarRingInputs` construit une fois par
frame, `seaQuadHalfSpan = activeRing().config.span * 0.5f` ; (f) `rhi::DepthState` presets
nommés (`kReversedZOpaque`, `kReversedZReadOnly`, `kCasterDepth`) et bindings nommés
(`kFrameUboBinding = 0`, `kLightsUboBinding = 5`, slots 3-9) dans `FrameUniforms.hpp` miroir
de `common.glsl` ; `bindSceneGroups(cmd, …)` ; (g) `enum class SsdmMode { Off, Half, Full }`,
`enum class WaterSimView { Live, ForceBaked, SeamOverlay, Volumes }`, et une
`WaterSystem::writeFrameLanes(FrameUniforms&)` sur l'idiome `MistMap::info()`.

**Risque.** Aucun visuel (code motion, mêmes valeurs) ; ordre de création/destruction à
écrire comme commentaire de la table ; HORIZON « ridé 16 m sous l'anneau interne »
(`extraSink`) à vérifier après (e).

### S7. La vérité du monde dans le renderer : la frontière à décider (Q3-5, Q3-14, Q3-3, Q3-26, Q3-28, Q3-13)

**Pattern.** Trois faits convergent. (1) `TerrainNoise.{hpp,cpp}` — la vérité CPU du relief
(`terrain::height/normal`, `materialWeightsCore`) — n'inclut que `engine/core`,
`engine/terrain` et glm, et est inclus par **43 TU** : 13 `engine/render`, 20 `game/`
(collisions, minimap, PNJ, esprits, sculpt), 8 tests, 2 cooker, 0 `world/gameplay` (§2.10
PASS). (2) `WaterSystem` contient `bakedEntrySources` (119 l. d'hydrologie pure sur
`WaterBodies`/`GridSpec`, jumelle de `pinLakes/pinRivers/preRollWindow` qui sont dans
`engine/terrain/WaterSim.hpp`) et `buildLocalGeometry` (390 l. de mesher pur, `terrain::height`
+ `LakeSurface`, dont `covered(mc, mr)` est la **dixième copie** du prédicat lac de Q1), et
**le cycle de vie de la sim** : sept booléens d'état (`simInFlight`, `simValid`,
`simSettling`, `simGroundDirty`, `simHasLastCam`, `simHasLastCrumb`, `simFrozenClearPending`)
+ `simEpoch`, la **même séquence d'invalidation copiée 3×** dans `updateSim` (`:1327-1333`
off, `:1358-1364` téléport, `:1388-1394` knob) + une variante dans `destroy`, le `GridSpec`
caméra copié 2×, et `simIsPreRolling()` qui **dérive** déjà un état de deux booléens — preuve
qu'un état manque. (3) D'où l'API de `WorldRenderer` : `terrainParams()` **mutable** ×72
sites côté `game/` (5 écritures de champs), `waterSystem()` ×17 (13 méthodes, toutes du cycle
de vie : `setSimSources`, `notifySimGroundChanged`, `simIsValid/Settling/PreRolling`,
`requestSimDump`…), `sculptRemeshQueue()`/`sculptScatterQueue()` = `vector<u64>&` nus ×9,
`fireScorchMap()`, `vegetationSystem()` ×7, et deux `friend` (`RenderTuningPanels` touche
`tuning` 91×, `vegetation` 19×, `terrain` 8× ; `RenderTuningIo` 44/17/10/8). Côté réglages,
le verdict dev « débit ×3 » attendu est codé `constexpr f32 kBakedDischargeBoost = 3.0f;
// Tune or revert on verdict` (`WaterSystem.cpp:108-114`), invisible du panneau Sim, et le
« sobriety pass » a laissé trois branches `#ifdef WATER_SIM` mortes (`milkGate = 0`, `foam =
0`) sans knob — deux violations de la règle « jamais un réglage dev perdu, bascule A/B ».
Enfin « le sol est-il définitif ici ? » est écrit deux fois avec le genre de monde en dur
(`WaterSystem.cpp:1402` déréférence `params.base` sans test ; `HeightField.cpp:132` le
teste) — T-10 de Q2 qui descend dans `render/`.

Et la racine `engine/` possède un objet de `render/` : `Engine` crée `SpriteRenderer` +
`Camera2D` et les place dans `FrameContext.sprites` (`Engine.cpp:77, 112`,
`FrameContext.hpp:25-26`) — ce sont **tous** les includes montants du cycle `engine/(root)
↔ render` de `layers_audit.py` (4 montants, 2 retours), et le 2D vit à deux niveaux (Q3-13).

**Mécanisme à étendre.** `engine/terrain` (headless, déjà en `namespace render::` — Q1-H) ;
`RenderView` comme contrat sim→rendu (`meshCache` y passe déjà) ; les requêtes safe-point
`requestRegenerate()`/`requestReseedVegetation()` (le bon pattern) ; la règle « enum over
nested ifs », modèle `restoreMode` ; `SimConfig`/`WaterSimParams` pour les lanes A/B ;
`WorldRenderer` « multi-instance, per-subsystem config » pour absorber le 2D.

**Croquis.** (a) **Mécanique, sans décision** : `TerrainNoise.{hpp,cpp}` →
`engine/terrain/` (43 chemins d'include, aucun code) ; `bakedEntrySources` →
`terrain::entrySourcesFromBodies(bodies, spec, master, params)` dans `WaterSim.hpp` avec
`WaterSimParams::bakedDischargeBoost` exposé au panneau et replay-able par `cooker
water-replay` ; `buildLocalGeometry`/`buildFarGeometry` → `engine/terrain/WaterGeometry`
(`meshLakeSheet`, `meshRiverRibbon`, une fonction par passe) testables au golden ;
`TerrainParams::groundFinalAt(x, z)` null-safe, une définition. (b) **La sim en enum** :
`enum class SimPhase { Off, AwaitingGround, PreRolling, Settling, Live }`,
`simTransition(next, why)` seul écrivain de phase/epoch, `simInvalidate(reason)` = le bloc
copié ×3, `simWindowSpecAt(cam)`, `updateSim` découpé en `simDrainResults / simApplyPolicy
/ simMaybeEnter / simTick` ; la lambda de tick (90 l.) devient une fonction statique
headless ; vérifiable par `cooker water-replay` sur un dump. (c) **Décision dev** :
`WaterSimControl` façade côté sim (ou `WaterSystem` scindé en `WaterSim` headless +
`WaterRenderer`) ; `TerrainParams` possédé par la scène/le streamer et passé au renderer
par `RenderView` ; les knobs des sous-systèmes hissés dans `RenderTuning` (le `friend`
tombe) ; les queues de sculpt → `requestRemesh(span<u64>)`/`requestRescatter(span<u64>)` ;
`cfg.sprites` sur `WorldRenderer` (ou `SpriteRenderer` passé par la pile de scènes),
`FrameContext` ne portant plus qu'un `CommandBuffer&` + `Device&`, `Engine` ne connaissant
plus `render::`. C'est la préface de RENDERER-EXTRACT (`RENDERING.md` §6) et le pendant du
nettoyage de namespace de Q1.

**Risque.** (a) et (b) : nul visuellement si bit-exact (hash des `verts` du mesher ; même
séquence de phases) ; Phase 5 préservé. (c) : L, touche la couture §2.10 et une frontière
déclarée dans `ARCHITECTURE-LAYERS.md` — **arbitrage dev**.

### S8. Le backend Vulkan : découper, pas réécrire (Q3-1, Q3-6, Q3-7, Q3-25, Q3-31, Q3-37, Q3-48)

**Pattern.** `VulkanDevice.cpp` est **une suite de modules déjà séparés par des bandeaux**
(`--- Format / state conversion`, `--- Resource records`, `--- Descriptor binding remap`,
`--- Device state`, `VulkanCommandBuffer`, `--- Transfer helpers`, `--- Swapchain`, `---
Lifetime`, `--- Frame`, `--- Buffers/Textures/Samplers/Render targets`, `--- Pipelines`,
`--- GPU markers`, `--- Creation`) ; seule la visibilité de `struct Impl` (déclaré dans le
.cpp, l.760-1140) empêche le découpage. `create()` (571 l.) appelle
`vkGetPhysicalDeviceQueueFamilyProperties` par paire à **quatre endroits** (score GPU
`:4420`, famille async `:4479`, famille upload `:4507`, timestamps `:4804`), chacun avec sa
`vector<VkQueueFamilyProperties>` locale et son barème. `createTexture` (~330 l.) écrit deux
fois la séquence `createStaging + memcpy + vmaFlushAllocation + immediateSubmit([transition,
copy, transition]) + pendingBuffers.push_back(asyncParkFrame())` (`:3330-3356`,
`:3386-3420`) et une troisième variante dans `generateMipmaps` ; la décision (ring vs
immédiat vs conversion f32→f16) est mêlée à l'exécution (`else if (VkBuffer staging =
VK_NULL_HANDLE; true) {` `:3382`). Deux « lanes » de file (compute, upload) sont deux jeux
parallèles de 9 champs écrits champ par champ : bloc « begin recording » ×3 (`:2187, :2238,
:2711`), « pool + timeline + N command buffers » ×2 dans `create` (~45 l. chacun), « wait
slot value » ×2 dans `beginFrame`, submits timeline ×2 dans `endFrame`. `remapBindings`
(144 l.) + `promoteVersion` = parseur GLSL « transitoire » exécuté à **chaque compile** sous
« Remove with it once the corpus has cycled through a few releases » — le corpus a cyclé
(HORIZON clos). Registres de handles : 9 + 10 `unordered_map<u32,T>` + `nextId`, `destroyX`
find/erase écrit 7× côté GL et 7× côté Vulkan, mode d'échec asymétrique (GL asserte / `.at`
lance ; Vulkan `return` silencieux partout). Trois contrats divergent (Q3-48) :
`timestampReady` sur handle inconnu (GL `true` + 0, Vulkan `false`) ; GL `copyTexture`
force `GL_TEXTURE_2D` quand Vulkan copie toutes les couches ; Vulkan `copyTexture`
transitionne mip 0 seul puis tracke l'image entière `READ_ONLY`.

**Mécanisme à étendre.** Le modèle `GlDeviceBase.hpp` (header interne de backend sous
`backends/`), `clampStagesToQueue` (une fonction pour toute une règle), `pipelineFor`
(variantes par clé), la deletion queue à politique unique ; `vksmoke` 40 PASS validation
propre = barrière.

**Croquis.** `VulkanInternal.hpp` (Impl, records, `toVk*`, `transitionLayout`,
`clampStagesToQueue`) puis `VulkanShader.cpp`, `VulkanCommandBuffer.cpp`,
`VulkanTransfer.cpp`, `VulkanFrame.cpp`, `VulkanResources.cpp`, `VulkanDevice.cpp` (create,
dtor) — interface inchangée, `VMA_IMPLEMENTATION` dans un seul TU. `struct QueueFamilies`
résolu **une fois** par `pickQueueFamilies(gpu, surface)` (3 barèmes = 3 lambdas sur la même
liste) ; `create()` en phases nommées retournant `bool`. `ImageUploadPlan planImageUpload(
desc, pixels, caps)` pur + `uploadImageViaRing(plan)` / `uploadImageImmediate(plan)` (T-12).
`struct QueueLane { family, flags, queue, pool, cbs, timeline, next, doneValue, slotValue,
recording; init(); beginRecording(frame); waitSlot(frame); submit(frame, waits…) }` ×2.
Retirer le parseur legacy (~190 l.) ; si l'on veut garder la preuve, un cas `vksmoke` qui
compile le corpus. `engine/rhi/HandleTable.hpp` backend-agnostique (`find/insert/take`)
partagé — ce n'est **pas** U2-02 (le type de handle reste, la table se partage) — avec une
politique d'échec unique (assert debug + no-op loggé une fois en release) ; les 24 `.at(` GL
deviennent mécaniquement des `find`, **sans** argument perf (U2-01 ✖ respecté). Écrire les
trois contrats dans `Device.hpp`/`CommandBuffer.hpp` et aligner.

**Risque.** Q3-1/6/7 : nul fonctionnellement (déplacement, mêmes phases). Q3-25 : c'est la
zone des crashs driver résolus le 2026-10-01 — la topologie (qui attend quoi, timelines
séparées) ne change pas, seule l'écriture ; valider par `vksmoke` +
`MEADOWS_VK_SYNC_VALIDATION=1` sur une session sandbox. Q3-37 : nul (le parseur ne fait que
logger).

### S9. Hygiène (Q3-49, Q3-50)

~195 commentaires hors politique dans `landscape/` + `WorldRenderer` (D5 seul : 79 — tags
**G2–G7c** ~40 dans RC, **V1–V4/H1/H4/PG2/PG3**, ids d'audit **U3-3/U3-6/U3-7/P0**,
**E5/B5/B6** ; D3 : 33 dont 2 **périmés** — `terrain_weights.glsl:57` cite un `snowPatchAt`
inexistant, `TerrainNoise.cpp:504-508` décrit un tap vert remplacé ; D2 : 30 dont « measured
dev » ×10 et « The x1.6 boost died with the width inverse » ; D1 : ~20 (`H3`, `E3`, `PG2`
×4, `§5 B6` ×4, « used to re-kick ») ; D4 : 17 (`U3-7` ×4, `brique 27b` ×3) ; 8 sur 7 100 l.
de RHI (récits « found the hard way », « 1-in-5 vksmoke flake », sigles PG1-3 ×7) ; 6 sur
2 134 l. de racine (« review 2026-08 », « ProjectionTest » inexistant, « 19 » pour 24
champs, « Embedded until the asset system… » périmé). Deux commentaires promettent une API
RHI inexistante (`MistMap.cpp:51-53`, `WaterSystem.cpp:1064-1066` : `updateTexture`) — à
croiser avec le RHI puis retirer. Divers : `getenv("MEADOWS_GI_PROBE_LIGHT")` et sonde de
boot à `readBuffer` bloquant dans `RadianceCascades::update` ; littéraux du composer
(48/800/2 400/1 400/2 500/40/60/140) ; stubs GL 4.1 qui `LOG_ERROR` **à chaque appel** ;
blocs d'includes copiés dans les 4 TU de végétation (`Hash`, `Jobs`, `VertexAoCache`, `Paths`
inutilisés), `(void)device` ×3 vestigiaux, BOM UTF-8 sur `VegetationSystem.hpp` ; ternaire
mort `view.interiorMode ? 0 : …` sous `if (!view.interiorMode)` (`WorldRenderer.cpp:1838`),
corps de `recordGiUpdate` indenté d'un cran ; `lastDrawn` périmé hors culling ; `retired`
de `ShaderLibrary` détruit sur délai (0,5 s) et non sur preuve.

---

## Bugs trouvés au passage (classe B) — aucun corrigé

| id | sév | symbole (fichier) | effet | correction | visuel |
|---|---|---|---|---|---|
| **Q3-43** | haute | `WaterSystem::buildPipeline` (`WaterSystem.cpp:1735-1738`) | `shaders.beginWatch()` est **dans** le `if (pipeline.id != 0)` (indentation décalée = insertion tardive) ; au premier build depuis `create()` la watch n'est jamais ouverte, `endWatch()` renvoie une liste vide (ou héritée du dernier système observé avant l'eau dans `WorldRenderer::create`), et « an EMPTY watch never fires » (`ShaderLibrary.hpp:62`) : les 5 pipelines d'eau ne réagissent **jamais** à une édition de `water_surface.glsl` (655 l.) et consorts. Sur GL, pire : le programme recompilé est retiré puis `glDeleteProgram` 0,5 s plus tard pendant que `GlPipeline.program` (copie du GLuint) pointe dessus → `glUseProgram` d'un nom supprimé ; Vulkan protégé (pipelines autonomes, destroy différé) | sortir `beginWatch()` du `if` (1 ligne) ; puis Q3-15 pour tuer la classe | dev only, rendu inchangé |
| **Q3-44** | moyenne | 6 pipelines SSDM (`WorldRenderer.cpp:625-638`), `probePipeline` (`RadianceCascades.cpp:175-178`) | aucun compteur ni watch : bâtis au resize / une fois (`id() == 0`) ; même conséquence GL que Q3-43 après édition de `ssdm_*.frag` / `rc_probe.comp` | Q3-15 | dev only |
| Q3-45 | basse | `shadow.glsl:46` `float texel = 1.0 / 2048.0;` | la résolution d'ombre est un knob live 1024/2048/4096 (`RenderTuningPanels.cpp:664-668`, `WorldRenderer.cpp:2050-2053` recrée `ShadowMapper`) ; à 4096 le noyau PCF 3×3 fait 2 texels (ombres deux fois plus floues), à 1024 un demi-texel ; la marge `uvScale - texel` idem | `1.0 / float(textureSize(uShadowMap, 0).x)` ou une lane (S2) | **change le look** hors 2048 → A/B |
| Q3-46 | basse | `familyColor` (`TerrainSystem.cpp`, placeholder de `buildMaterialArrays`) | `if (layer < 8) grass ; < 11 rock ; < 13 snow ; else sand` : falaise 17-18, roche 19, neige 20, dépôts famille herbe 21-24 peints en **sable** sans bibliothèque `.mtex` | `familyOf(layer)` de la table Q3-21 | placeholder seulement |
| Q3-47 | basse | `ShaderLibrary::load` (`ShaderLibrary.cpp:160-172`) | ré-appelé sur un nom existant : `compile()` retire l'ancien handle **sans bump de génération** ; un consommateur déjà bâti ne rebâtit pas et perd son programme 0,5 s plus tard (GL) ; `FarTerrain.cpp:30-39` l'a découvert et contourne par `if (shaders.get(...).id == 0)` | `load()` idempotent (retourne l'entrée) ou `ENGINE_ASSERT` si bindings différents ; retirer le garde | dev only |
| Q3-48 | basse (2 latents) | `timestampReady` (GL `GlDeviceBase.cpp:≈700` / VK `VulkanDevice.cpp:≈4230`) ; `GlCommandBuffer::copyTexture` (`:≈440`) ; `VulkanDevice::copyTexture` (`:≈2010-2050`) | (a) handle inconnu : GL `true` + 0, Vulkan `false` — un appelant autre que `GpuProbe` pollerait indéfiniment sur Vulkan ; (b) GL force `GL_TEXTURE_2D, depth 1` quel que soit `arrayLayers/depth`, Vulkan copie toutes les couches — inoffensif (3 appelants 2D) ; (c) Vulkan transitionne mip 0 seul puis `s->layout = t->layout = READ_ONLY` pour l'image entière — latent sur une texture storage à mips | contrat écrit dans `Device.hpp`/`CommandBuffer.hpp` (inconnu = `true` + 0 comme les fences ; « base mip, 2D monocouche » ou cible dérivée de `GlTexture`) ; transitionner `mipLevels` entier | nul aujourd'hui |

Signalé hors périmètre : `tests/VegetationScatterTest.cpp:107` reprend la constante FNV
tronquée `1469598103934665603` (onzième copie de Q1-41) ; `VulkanDevice.cpp:725 targetKey`
est la dixième (clé de cache RAM, inoffensive).

---

## Findings

Format : `**Qn-k** [classe / sévérité / effort]` — constat (citations) ; *Action* ; risque.
Les sévérités mesurent le coût de maintenance futur, pas le coût du fix.

### S — structure

**Q3-1** [S / haute / L] — `VulkanDevice.cpp` : un TU de 4 913 l., déjà découpé par bandeaux ; seule la visibilité de `struct Impl` (l.760-1140) l'empêche. *Action* : `VulkanInternal.hpp` + 6 TU par concern (shader, command buffer, transfer, frame, resources, device), interface inchangée. Risque nul ; `vksmoke` = barrière.

**Q3-2** [S / haute / M] — `WorldRenderer::render` 591 l. : trois décisions pures inline (key-shadow `:2398-2462`, LightsUbo `:2470-2560`, auto-skip réflexion `:2178-2208`). *Action* : `pickKeyShadows`, `fillLightsUbo`, `reflectionWanted` libres ou dans `FrameComposer` ; `render()` ~250 l. Code motion, aucun visuel.

**Q3-3** [S / haute / M] — `WaterSystem` : cycle de vie de la sim en 7 booléens + `simEpoch`, bloc d'invalidation copié 3× (`:1327, :1358, :1388`), `GridSpec` caméra ×2, `updateSim` 440 l., `simIsPreRolling()` dérive un état de deux booléens. *Action* : `enum class SimPhase`, `simTransition()` seul écrivain, `simInvalidate()`, `updateSim` en 4 fonctions, tick statique headless. Même séquence ; `cooker water-replay` = preuve.

**Q3-4** [S / haute / M] — `scatterGrass` (`GrassSystem.cpp:78-472`) 395 l., six phases nommées en commentaires, cache de clumps dans une closure. *Action* : `CornerLattice`, `ClumpPicker`, `ScatterCell`, `scatterCell()` ; poser un hash du scatter **avant** (aucun golden aujourd'hui), ordre des `rng.next()` par brin conservé.

**Q3-5** [S / haute / M] — logique headless compilée dans `render/` : `TerrainNoise.{hpp,cpp}` (43 TU, zéro include render), `bakedEntrySources` (`WaterSystem.cpp:83-201`, hydrologie pure), `buildLocalGeometry` (`:384-773`, mesher pur, 10e copie du prédicat lac). *Action* : `engine/terrain/TerrainNoise`, `terrain::entrySourcesFromBodies` dans `WaterSim.hpp`, `engine/terrain/WaterGeometry` (`meshLakeSheet`, `meshRiverRibbon`). Bit-exact par hash des `verts` ; préface de RENDERER-EXTRACT.

**Q3-6** [S / moyenne / M] — `VulkanDevice::create` 571 l., 14 phases inline, familles de files énumérées ×4 (`:4420, :4479, :4507, :4804`). *Action* : `pickQueueFamilies()` une fois (3 barèmes = 3 lambdas), phases nommées retournant `bool`. Ordre conservé.

**Q3-7** [S / moyenne / M] — `VulkanDevice::createTexture` ~330 l., 4 chemins d'upload, séquence staging+submit immédiat ×2 (`:3330-3356`, `:3386-3420`) + variante dans `generateMipmaps`, `else if (VkBuffer staging = VK_NULL_HANDLE; true)` (`:3382`). *Action* : `planImageUpload()` pur + deux exécuteurs (T-12) ; la règle `mipLevels == 1` pour le ring visible dans le plan.

**Q3-8** [S / moyenne / M] — chaîne SSDM à plat dans l'orchestrateur : 22 membres (`WorldRenderer.hpp:644-668`), 100 l. de création (`:573-637`), 80 l. d'enregistrement (`:1256-1335`), seul effet d'écran hors `PostFx`. *Action* : `SsdmChain` sur le modèle `PostFx` ; `ensureOffscreenTarget` ~150 l. A/B = `ssdmMode`.

**Q3-9** [S / moyenne / M] — `RadianceCascades::update` 357 l. : snap des origines copié verbatim de `prepare` (`:566-595` / `:673-700`), worker de 120 l. en lambda (`:434-553`), debug dans le chemin chaud. *Action* : `snapOrigins()`, `static BakedTile bakeTile()`, `update` → `fillUniforms / recordInject / recordBuild / recordExtend / recordMerge / recordBootProbe`. Bit-exact.

**Q3-10** [S / moyenne / S] — `FrameComposer` n'est pas le seul écrivain (`WorldRenderer.cpp:2388 clusterInfo`, `:2466 keyShadowAtlasViewProj`), politique base/resolved en 12 commentaires. *Action* : les deux lanes entrent dans `FrameComposerInputs` ; la portée devient une colonne de la table Q3-16.

**Q3-11** [S / moyenne / M] — `TerrainSystem::buildMaterialArrays` 176 l., trois chemins, albédo chargé hors du helper `loadArray` (`:219-262`), libération partielle = seul `destroyTexture` du fichier. *Action* : `loadArray` renvoie `{ handle, averages }` ; `loadCookedSet / makePlaceholderSet / bindSet` ; `struct MaterialSet` en `Unique`.

**Q3-12** [S / moyenne / M] — `VegetationSystem` : une classe, sept métiers, ~60 membres (`.hpp` 551 l.) ; ajouter une famille touche le header et trois TU. *Action* : `VariantMeshSet`, `PropMaterials`, `ReseedPipeline` — après Q3-19/Q3-20 qui fixent les frontières.

**Q3-13** [S / moyenne / M — **décision dev**] — la racine `engine/` possède un objet de `render/` : `Engine` crée `SpriteRenderer` + `Camera2D` (`Engine.cpp:77, 112`, `FrameContext.hpp:25-26`, `Engine.hpp:7`, `Game.cpp:4`) = **tout** le cycle `engine/(root) ↔ render` du `layers_audit`. *Action* : `cfg.sprites` sur `WorldRenderer` (propriétaire unique des renderers), `FrameContext` = `CommandBuffer&` + `Device&` ; ou minimal : `SpriteRenderer` passé par la pile de scènes (2 scènes sur 70 l'utilisent). Frontière déclarée dans `ARCHITECTURE-LAYERS.md`.

**Q3-14** [S / moyenne / L — **décision dev**] — l'API publique de `WorldRenderer` expose les entrailles : `terrainParams()` mutable ×72, `waterSystem()` ×17 (13 méthodes du cycle de vie de la sim), `sculptRemeshQueue()`/`sculptScatterQueue()` = `vector<u64>&` ×9, `fireScorchMap()`, `vegetationSystem()` ×7, 2 `friend` lisant 12 privés (`RenderTuningPanels` `tuning` 91×). *Action* : (a) knobs des sous-systèmes hissés dans `RenderTuning` → le friend tombe ; (b) `requestRemesh(span<u64>)`/`requestRescatter(span<u64>)` sur le pattern `requestRegenerate()` ; (c) `WaterSimControl` façade côté sim, `TerrainParams` possédé par la scène et passé par `RenderView`. (c) = S7, touche §2.10.

### G — généraliser

**Q3-15** [G / haute / M] — hot-reload en trois façons + une absence : 12 `Watch` bracketés à la main, 15 compteurs `u64 xxxGeneration` (WorldRenderer 5, RC 5, GpuOcclusion 3, LightClusters 1, UiSystem 1), 7 pipelines sans rien (6 SSDM, `rc_probe`), 1 watch vide ; `beginWatch` imbriqué efface sans assert, watch vide ne tire jamais → `|| id() == 0` ×5. *Action* : `render::WatchedPipeline::ensure()` (premier build inclus, assert watch vide, bracket privé) ; 59 pipelines migrés mécaniquement, −15 compteurs ; table `{ nom, &pipeline, recette }` dans RC et WorldRenderer. Après Q3-33. **T-14.**

**Q3-16** [G / haute / L] — le contrat des 81 lanes du FrameUbo écrit 3× (`FrameUniforms.hpp`, `common.glsl`, `FrameComposer.cpp`) et dérivant (4 noms, 4 lanes mortes, 18 canaux cachés, « reserved » périmés sur des lanes écrites) ; lanes squattées par d'autres systèmes (`tree.frag:175`, `mesh.frag:33`, `tree.vert:38`) ; constantes CPU ↔ GLSL appariées par commentaire ×9 familles (clusters, lumières, matériaux d'eau, mist, `treeFar`, espèces d'herbe, résolution d'ombre, `9000` ×4). *Action* : table de lanes X-macro générant struct + offsets + `frame_ubo.glsl` ; préambule de `constexpr` injecté par `ShaderLibrary` (ou `constants.glsl` généré) ; `uVegInfo` en fin d'UBO. Bit-exact (les `static_assert` le prouvent). **T-15** (le `FieldLane` de T-1 côté GPU).

**Q3-17** [G / haute / M] — `TextureFormat` : 9 tables de traits dans 3 fichiers, deux autorités d'octets contradictoires (`Rhi.hpp:157 formatBlockBytes` R16F = 4 vs `VulkanDevice.cpp:122 bytesPerTexel` R16F = 2), whitelist d'upload GL où une ligne manquante a causé la famille d'artefacts d'eau ; validation des descs éclatée en sous-ensembles différents (GL valide les bind groups, Vulkan non ; `createTexture` règles agnostiques différentes). *Action* : `constexpr FormatTraits kFormatTraits[]` + `static_assert` dans `Rhi.hpp`, chaque backend ne garde que sa colonne API ; `rhi::validateTextureDesc` / `validateBindGroup` dans `Device.cpp`. **T-4.** Parité : `vksmoke` + rendu GL 4.6.

**Q3-18** [G / haute / M] — le « monté » re-dérivé : 80 `cfg.*` + 24 `caps()` (conjonctions recopiées ×3, gate d'occlusion en deux formes `:244`/`:1394`), 4 verbes = 4 listes des mêmes 10 systèmes (`:103-361`, `:363-432`, `:1988-2010`, `:2030-2043`), dépendances d'ordre implicites. *Action* : `struct Mounted = cfg ∧ caps` résolu une fois ; table ordonnée `kSystems[]` (pointeurs de méthode, pas de `virtual`) parcourue par les quatre verbes ; `destroy()` perd sa liste manuelle. Étend `RendererConfig` (R3).

**Q3-19** [G / haute / M] — `scatterProps` (`VegetationScatter.cpp:55-502`, 447 l.) : cinq copies d'un squelette (arbres, rochers, cailloux, débris, buissons) à côté de la table `PlantTier` qui couvre déjà plantes et masse (`:386-462`) ; ne diffèrent que 3 marges, l'acceptation, l'échelle, le reach, l'enfoncement, le drapeau, la variante. *Action* : `ScatterRule[]` étendant `PlantTier`, une boucle, hook `preAccept` pour les débris ; colonnes = champs du futur `ScatterPropForm` (T-11). Golden `VegetationScatterTest` = preuve bit-exacte.

**Q3-20** [G / haute / M] — familles de variantes = plages `kFirst*` comparées à la main en ~12 sites (`VegetationSystem.cpp:146, 453`, `VegetationStreaming.cpp:195-197`, `VegetationAssets.cpp:143, 183, 190`) ; `shadow_prop.vert:42 step(0.35, aPosScale.w)` exprime « les cailloux ne projettent pas » par un seuil d'échelle (même 0.35 que `game/VegetationCollision.cpp`). *Action* : `enum class PropFamily` + `constexpr PropFamilyTraits kFamilies[]` (`castsShadow`, `giKind`, `aoStrength`, `texturedRigid`…) avec `static_assert` sur la somme ; le shader reçoit le drapeau par l'instance (Q3-22). **T-4.**

**Q3-21** [G / haute / M] — table des 25 couches splat écrite 3× (`SplatTextures.hpp:30-74`, `terrain_zones.glsl:71-100 hexFamilyLayer`, 10 littéraux de `terrain.frag`) + 4e copie divergente `familyColor` (bug Q3-46). *Action* : `constexpr array<SplatLayerInfo, kSplatArrayLayers> kSplatLayers { family, variant }`, `familyOf()`, `variantLayer()` ; bloc GLSL unique inclus par les deux shaders (ou généré, Q3-16). Pixels identiques (capture A/B) sauf le placeholder corrigé.

**Q3-22** [G / moyenne / M] — protocole de drapeaux en bits de signe (rigide `params.z < 0`, texturé `params.w < 0`, carte `uv.x < -5` + slot −20, écorce `uv.y < -0.5`, conifère `aParams.z < 0`) défini dans 3 fichiers C++ (`VegetationScatter.cpp:88-90, 370-372`, `SpaceColonizationTree.cpp:88-96`, `TreeGenerator.cpp:140`), décodé dans 4 shaders, aucune constante commune. *Action* : `PropEncoding.hpp` + `prop_encoding.glsl` ; à terme un lane de bits. Re-capture du golden scatter si l'encodage change — à grouper avec Q3-19.

**Q3-23** [G / moyenne / M] — `BakeMailbox` réécrit à la main : `WaterSystem` ×4 (`Shared{4}` + 4 `inFlight` + `generation` + 5 `isStopping()` + 4 boucles `tryPop … gen`, `.hpp:269-274`, `.cpp:876, 896, 952, 1255`), `ChunkOcclusion` ×1 (`.hpp:88-98`), `RadianceCascades::pumpTileBake` ×1 ; coalesceur `contentStamp` recopié (`WaterSystem.cpp:975-985` vs `FarTerrain.cpp:134-139`). *Action* : `BakeMailbox<T>` partout (lane sim : `epoch` dans le payload, `drain` avec prédicat), `core::QuietStamp` ; `BakeMailbox.hpp` + `ChunkStreamer.hpp` descendent dans `engine/core` (zéro dépendance render — **T-6 confirmé**, `forEachIndexed` non requis). Phase 5 inchangé.

**Q3-24** [G / moyenne / M] — « carte cuite en anneau » : la moitié non partagée recopiée ×4 (`TerrainLightMap`, `TerrainShadeMap`, `MistMap`, `FarTerrain`) — `kSize/kSpan` (3072 ×3), `center`, test `strayed` identique (`TerrainLightMap.cpp:128`, `TerrainShadeMap.cpp:60`, `MistMap.cpp:101`, `FarTerrain.cpp:141`), `drain` détruit/recrée, `destroy()` manuel. *Action* : `template <Baked> class RingBakedMap { BakeMailbox; center; span; strayed(); land() }` ; textures en `Unique` (Q3-33). Mêmes pixels, même cadence.

**Q3-25** [G / moyenne / M] — deux lanes de file Vulkan (compute, upload) = deux jeux parallèles de 9 champs : « begin recording » ×3 (`:2187, :2238, :2711`), « pool + timeline + cbs » ×2 dans `create`, « wait slot » ×2 dans `beginFrame`, submits ×2 dans `endFrame`. *Action* : `struct QueueLane { init(); beginRecording(frame); waitSlot(frame); submit(frame, waits…) }` ×2. Zone des crashs driver résolus le 2026-10-01 : topologie inchangée, valider par `vksmoke` + `MEADOWS_VK_SYNC_VALIDATION=1`.

**Q3-26** [G / moyenne / S] — réglages dev hors A/B : `constexpr f32 kBakedDischargeBoost = 3.0f; // Tune or revert on verdict` (`WaterSystem.cpp:108-114`) = le verdict « débit ×3 » attendu, invisible du panneau Sim ; seuils `90 / 360 m³/s`, `0.05`, `80 m` de la même famille ; « SOBRIETY PASS (dev directive) » (`water_surface.glsl:425`) laisse 3 branches `#ifdef WATER_SIM` mortes (`milkGate = 0`, `foam = 0`, murs sans stries) sans knob. *Action* : `WaterSimParams::bakedDischargeBoost` exposé et replay-able (avec Q3-5) ; les branches mortes → knob `uWaterDebugInfo` (A/B) ou retrait **nommé** dans `docs/PAYSAGE.md`. Règle dev « jamais un réglage perdu ».

**Q3-27** [G / moyenne / S] — vocabulaires en `i32` : `ssdmMode` 0/1/2 en 4 ternaires (`WorldRenderer.cpp:559-564, 2266, 1294`), `debugBuffer`, `waterDebug`, `SimConfig::debugMode` 0..3 testé `== 1/2/3/!= 1` en 8 sites C++ + 2 GLSL (3 = volumes non documenté) ; 4 feeds FrameUbo de l'eau assemblés par `WorldRenderer.cpp:2299, 2365-2367`. *Action* : `enum class SsdmMode`, `enum class WaterSimView { Live, ForceBaked, SeamOverlay, Volumes }` ; `WaterSystem::writeFrameLanes(FrameUniforms&)` sur l'idiome `MistMap::info()` — à généraliser à tous les systèmes qui nourrissent le FrameUbo. Règle dev « enum over nested ifs ».

**Q3-28** [G / moyenne / S] — « le sol est-il définitif ici ? » écrit 2× avec le genre de monde en dur : `WaterSystem.cpp:1402 if (params.sandbox && !params.base->regionAt(x, z))` (déréférence sans test) vs `HeightField.cpp:132 if (!params.sandbox || (params.base && params.base->regionAt(x, z)))`. *Action* : `TerrainParams::groundFinalAt(x, z)` null-safe, une définition. **T-10** (`WorldKind` de Q2 descend dans `render/`).

**Q3-29** [G / basse / S] — presets de pipeline à la main dans tout `render/` : `CompareFunc::Greater` ×16 + `// reversed-Z` ×6, caster `Less + depthBias 4.0f + slope 2.5f` ×6 copies identiques (`TerrainSystem.cpp:674-679`, `VegetationSystem.cpp:95-97`, `WaterSystem.cpp:1762/1788/1809/1829`), `{ "FrameUbo", 0 }` ×44 dans 11 fichiers, `{ "LightsUbo", 5 }` ×2, slots 3-9 liés par 7 `if` recopiés (`recordMainPass:1465-1489` / `recordReflection:1657-1672`), `clearDepth = 0.0f // reversed-Z far` ×2. *Action* : `rhi::DepthState kReversedZOpaque / kReversedZReadOnly / kCasterDepth` (constexpr), `render::kFrameUboBinding = 0`, `kLightsUboBinding = 5`, slots nommés dans `FrameUniforms.hpp` ; `bindSceneGroups(cmd, …)`. Nul.

**Q3-30** [G / basse / S] — petites tables manquantes : (a) biais d'espèce d'herbe en `switch (variantA)` ×4 → colonne `kGrassSpeciesZoneBias[variant][species]` (`GrassSystem.cpp:271-293`, `x * 1.0f == x`) ; (b) bandes de LOD en 4 `if` + `kLod3CoreSide = 13 // 2*6+1` → `kLodBands { 1, 3, 6, 12 }`, `coreSide(lod)` (`TerrainSystem.hpp:69-79`) ; (c) `appendWood` : 6 ternaires parallèles sur `detail` (`SpaceColonizationTree.cpp:338-362`, `:620`, `:518`, `:590`) → `DetailProfile kDetail[4]` ; (d) `generateLeafMaskPixels` if-chain sur `shape` (`:766, 800-813`) → `kLeafShapes[5]`, index borné par `static_assert`. Bit-exact ; `TreeGeneratorTest`.

**Q3-31** [G / basse / S] — registres de handles : 9 + 10 `unordered_map<u32,T>` + `nextId`, `destroyX` find/erase ×7 GL + ×7 Vulkan, `findX` ×5 ; mode d'échec asymétrique (GL `ENGINE_ASSERT` / `.at` lance ; Vulkan `return` silencieux partout) — un bug d'appelant est fatal sur GL et invisible sur Vulkan. *Action* : `engine/rhi/HandleTable.hpp` (`find/insert/take`, backend-agnostique) + une politique d'échec (assert debug, no-op loggé une fois en release). Ce n'est **pas** U2-02 (le type reste) ni U2-01 (aucun argument perf) ; `UniqueShader` manquant ajouté.

**Q3-32** [G / basse / M] — données de look en C++ : palette du ciel `SkySystem::evaluate` ~22 littéraux `Vec3` + poids (seule donnée de look non réfléchie, `WeatherForm` existe) ; albédos proxy des matériaux écrits 2× (`RadianceCascades.cpp:30-33 kGrass/Rock/Sand/SnowAlbedo`, `SpaceColonizationTree.cpp:740 { 0.158f, 0.358f, 0.116f }`). *Action* : `terrain::kMaterialProxyAlbedo[]` à côté de `MaterialWeights` ; palette ciel → décision « la donnée porte le look » (T-11). Look identique.

### D — duplication

**Q3-33** [D / haute / M] — migration `rhi::Unique` inachevée : dix classes en `device.destroy*` manuel, 107 appels — `WaterSystem` **45** (0 `Unique`), `GpuOcclusion` 14, `SkySystem` 8, `SpriteRenderer` 8, `TerrainShadeMap` 7, `ShadowMapper` 6, `FireScorchMap` 6, `TerrainLightMap` 5, `MistMap` 5, `LightClusters` 3 (`GpuProbe` 6 et `MeshCache` 4 légitimes) ; `destroy()` en listes manuelles qui dérivent (`WaterSystem` oublie 6 scalaires ; `RadianceCascades::destroy:124-148` oublie `extendPipeline`, `probePipeline`, `probeBuffer`, `probeGroup`, `havePrev`, `appliedCascadeCount` ; `WorldRenderer::destroy:364-366` omet les 22 membres SSDM sous un commentaire disant la liste superflue ; `PostFx::destroy` = `*this = {}` réinitialise 4 knobs). *Action* : `Unique` partout (groups déclarés après textures), `UniqueShader`, `PostFxTuning` extrait, `*this = X {}` + mailbox sauvée (idiome `FarTerrain`/`NoiseVolume`). **Préalable de Q3-15.** Nul.

**Q3-34** [D / haute / M] — `PostFx` : cinq copies de la « famille cible ½-res » — `{tex, fb, group}` ×12, `{historyTex, historyFb, historyGroup, temporalUbo, prevViewProj, prevCamera, frame, historyValid}` ×3 (`PostFx.hpp:190-199, 202-215, 222-233`), `{raw, blur}` ×2 ; `renderMist` (66 l.) / `renderSkyClouds` (57 l.) quasi identiques ; alpha temporel recopié ×3 (`:489, :596, :792`) ; `destroyTargets` 56 `x = {}` redondants. *Action* : `HalfResTarget`, `TemporalHistory`, `MediaPass` : ~70 membres → ~15. Capture A/B (règle « graphics need visual validation »).

**Q3-35** [D / haute / S] — shaders copiés au lieu d'inclus : `shadow_prop.vert:18-78` = 45 l. de `tree.vert:31-124` (yaw, décodage carte, fade 0.86, rafale, hash de chute) à deux différences près ; `shadow_prop.frag:13-19` = `tree.frag:111-116` ; `skinned.frag:27-57` = `mesh.frag:28-65` avec cinq « KEEP IN SYNC » ; `windowBeam` verbatim (`locallights.glsl:52` / `froxel_inject.comp:32`) ; `giSlabDir` ↔ `rcOctDecode` « KEEP IN SYNC » ; bruit 2D ×3 (`fxhaze.frag:20`, `grass.vert:56`, `watervolume.frag:19`), bruit 3D ×2 ; projection CSM ×2 (`shadow.glsl:22-41` / `fog_march.glsl:6-21`) ; luma ×6 GLSL + ×2 C++ ; coins de billboard ×3 (`fxparticle/fxhaze/fxflame.vert:39-44`) ; trois `.frag` casters vides identiques. *Action* : includes-corps sur le modèle `shadow.glsl`/`ssdm_resolve_body.glsl` : `prop_placement.glsl`, `leaf_cutout.glsl`, `mesh_shade.glsl`, `noise.glsl`, `window_beam.glsl`, `billboard.glsl`, `csmProject()` ; `gi.glsl` inclut `rc_common.glsl` ; `depth_only.frag`. Même code ; capture ombre/arbre.

**Q3-36** [D / moyenne / S] — deux anneaux lointains = le même code ×5 verbes (`create` `:159-171`, `destroy`, `refreshPipeline`, `update` ×2 **identiques** `:1898-1925` avec `treeSilhouette()` 143 l. recalculé 2×/frame, `draw`), `horizonReach()` à 3 cas, demi-spans dérivés à la main `28800.0f : 9000.0f` (`:2345`) redoublés par `FrameComposer.hpp:142`, `FrameUniforms.hpp:254`, `water.vert:20`, `WaterSystem.cpp:1007`. *Action* : `array<FarTerrain, 2> farRings`, `FarRingInputs` une fois par frame, `seaQuadHalfSpan = activeRing().config.span * 0.5f`, `horizonReach()` = premier anneau `ready()`. Mêmes valeurs ; vérifier `extraSink` HORIZON.

**Q3-37** [D / moyenne / S] — parseur GLSL legacy `remapBindings` (144 l.) + `promoteVersion` exécutés à **chaque compile** comme « Transitional cross-check … Remove once the corpus has cycled » (`VulkanDevice.cpp:441-585, ≈3690-3725`) ; la réflexion SPIR-V est l'autorité, le corpus a cyclé. *Action* : retirer ~190 l. ; un cas `vksmoke` compilant le corpus si l'on veut garder la preuve. Nul (ne fait que logger).

**Q3-38** [D / moyenne / M] — végétation : deux chemins pour construire les niveaux d'une variante — sync `createVariantMeshes` (`VegetationAssets.cpp:124-212`, avec `bushSpecies`, overrides) vs async `reseedVariantMeshesAsync` + `pumpReseed` (`:507-595`, arbres seuls, pas de `bushSpecies`, overrides ignorés), forces AO 0.6/0.55/0.5 inline ×2 ; formules recopiées dans les trois chemins de dessin : `levels` (`VegetationSystem.cpp:284, 405`), `group` (`:365, 412`), `BarkPush` ×3, AABB paddée ×3 ; `drawDepth:489-510` tient un **second** choix de LOD en 3 ternaires ×3 à côté du « ONE rule » `canopyLevel`. *Action* : `buildVariantLevels(seed, species, family)` pur appelé par les deux ; `casterLevel()` dans `levelMesh`, `levelCount()`, `drawGroupOf()`, `barkPushFor()`, `Chunk::aabb(key)` (idem `GrassSystem.cpp:671`, `TerrainSystem.cpp:732/815`). Nul.

**Q3-39** [D / moyenne / S] — échantillonneurs et grille à côté de `GridOps` (T-7) : bilinéaires `HeightField.cpp:7-24 Level::sample` (`Level` **est** `GridSpec + vector<f32>` = le `GridView` de T-7), `FarTerrain.cpp:≈327-340 gridHeight`, `authoredDelta` (`TerrainNoise.cpp:533-555`) ; `MistMap.cpp:15-44 boxBlurAxis` vs `GridOps.hpp:82 boxBlur3` ; mapping texel des masques de région réécrit ×3 dans `TerrainNoise.cpp` (`:229-243`, `:333-349`, `:684-699` « maskSample's own u/v mapping ») ; `wanderLattice/wanderValue01` (`:112-132`) = second value-noise (miroir bit-exact de `terrain_weights.glsl:25-49` — à garder sous un nom qui dit son contrat si `noise::value` diffère) ; clé de chunk à la main en 4 sites non convertis (`ChunkOcclusion.cpp:54-55`, `TerrainSculptTool.cpp:106`, `TerrainCollision.cpp:14`, `VegetationCollision.cpp:17`) + `HeightPatches::keyOf` qui réécrit `chunkKey` parce que les helpers vivent dans render (U3-4 clos à moitié) ; `TerrainSystem::kChunkSize` lu par 11 TU hors `TerrainSystem`. *Action* : `Level` porte un `GridSpec`, `sample = bilinearWorld` ; `boxBlurAxis` dans `GridOps` ; `MaskTexel maskTexelOf(region, x, z)` dans `TerrainBase.hpp` ; `kTerrainChunkSize` + `chunkKey` dans l'en-tête de grille descendu en core (Q3-23). Bit-exact pour `maskTexelOf` (**touche `engine/terrain`** → hash `MapBakerTest`) ; `Level::sample` non bit-exact (`mix`) mais `HeightField` est hors consommateurs golden par contrat (`HeightField.hpp:20-24`).

**Q3-40** [D / moyenne / M] — `SpriteRenderer.cpp:11-68` : seul shader embarqué en C++ du dépôt (`#version 410`, `#ifdef VULKAN` recopié `:27-35, 57-61` — le travail de `compat.glsl` et `promoteVersion`), « Embedded until the asset system… » périmé (`ShaderLibrary` sert 70+ shaders) → pas de hot-reload 2D ; `SceneView.hpp:5` inclut `SpriteRenderer.hpp` (donc `Device.hpp`) pour le POD `Sprite` de 6 champs, tiré par `game/SceneSubmit.hpp` et `NpcDirector.hpp` (hub de 13 TU, Q2). *Action* : `sprite.vert/.frag` dans `shaders/`, `SpriteRenderer::create(device, shaders)` ; `render/Sprite.hpp` (précédent exact : `FxInstance.hpp`). Dépend de Q3-13.

**Q3-41** [D / moyenne / M] — `AtmosphereParams.hpp:13-46` : 24 `f32` (le commentaire dit 19) « mirror WeatherForm's fields (capture/apply convert between the two) » = troisième écriture des champs météo (struct, Form, capture/apply) — le `FieldLane` de T-1 que Q2 a vu dans `WeatherController::kWeatherLanes`. *Action* : vérifier que les lanes de `WeatherController` couvrent les 24 champs ; sinon instance de T-1 côté render.

**Q3-42** [D / basse / S] — petits helpers copiés : bloc « lier les groupes optionnels 3-9 » ×2 (`WorldRenderer.cpp:1465-1489` / `:1657-1672`) ; quatre registres mark/sweep à la main (`skinnedDraws`, `waterQuads`, `waterMeshSlots`, recherche linéaire dans `drawCastersInto` `:1187-1196`) → `SlotRegistry<T>` ; modèle « lumière liée au soleil » ×3 (`:2409-2425`, `:2496-2515`, `:2753-2765`) → `sunLinkedLight()` ; layout `SkinnedVertex` inline ×2 ; `1 + log2(max(w,h))` ×4 (`VegetationAssets.cpp:33, 349, 418, 437`) → `rhi::fullMipLevels()` ; lazy-sampler `if (x.get().id == 0)` ×4 ; bind group à 4 entrées ×3 → `makePropMaterialGroup()` ; palettes écorce/feuilles ×2-3 (`TreeGenerator.cpp:38, 103` / `SpaceColonizationTree.cpp:335, 681, 739` / `far_tree.frag:65`), `6.2831853f` ×11 dans 4 fichiers (pas de `core::kTau`) ; `MeshBuilder.cpp` `angleAt` ×2 (`:56-67`, `:92-104`), base orthonormale ×3 ; `WaterSystem::draw` quadruplet `setPipeline + setBindGroup(0,1,2)` ×5 (`:1878-1935`) → `bindWaterPass()` ; FNV `targetKey` (`VulkanDevice.cpp:725`) → `core::Fnv64` (T-2). Tous bit-identiques par construction ; `WeaponMeshesTest`/`TreeGeneratorTest`.

### B — bugs

Voir la table « Bugs trouvés au passage » : **Q3-43** (watch de l'eau vide, haute), **Q3-44** (6 SSDM + `rc_probe` sans hot-reload, moyenne), Q3-45 (`shadow.glsl 1/2048` vs knob), Q3-46 (`familyColor` couches 17-24 en sable), Q3-47 (`ShaderLibrary::load` sans bump), Q3-48 (trois contrats RHI divergents GL/Vulkan).

### H — hygiène

**Q3-49** [H / moyenne / S] — ~195 commentaires hors politique 2026-07-22 dans `landscape/` + `WorldRenderer` (tags G2–G7c, V1–V4, H1-H4, PG1-3, E3/E5/B5/B6, I5/I6, R3, U3-x, P0/P4, « brique 27b », « measured dev » ×10, anciennes valeurs, « used to », « lived here ») ; 8 dans `rhi/` ; 6 dans la racine ; **périmés** : `terrain_weights.glsl:57` (`snowPatchAt` inexistant), `TerrainNoise.cpp:504-508` (tap vert remplacé), `Projection.hpp:25` (« ProjectionTest » = `FrustumTest.cpp:114`), `AtmosphereParams.hpp:7` (« 19 » pour 24), `SpriteRenderer.cpp:11`, `GlDevice46.cpp:≈112` (« RGBA8/SRGBA8 only » alors que R16F/R32F/RGBA16F acceptés), `Device.hpp:71` (« base mip only » périmé depuis `pixelsIncludeMips`), `Rhi.hpp:150` (« single authority » contredit) ; promesses d'API (`updateTexture` ×2). *Action* : tri par fichier touché ; les contrats durables (`WorldRenderer.cpp:120-124, 723-727, 1456-1459`, invariants Phase 5 de `WaterSystem`) sont excellents et restent.

**Q3-50** [H / basse / S] — divers : `std::getenv("MEADOWS_GI_PROBE_LIGHT")` + sonde de boot `readBuffer` bloquant + trace throttlée dans `RadianceCascades::update` (`:623-647, :719, :915-955`) → `RcTuning.debugView` / `recordDebug` ; littéraux du composer (`FrameComposer.cpp:121, 209, 277-281` : 48/800/2 400/1 400/2 500/40/60/140, `kFogTailKnee = kVolumetricReachMax + 100`) → constantes nommées ; stubs GL 4.1 `LOG_ERROR` à chaque appel (`GlDeviceBase::generateMipmaps/createSampler/createFramebuffer/dispatch/copyTexture`) → une fois ; includes copiés dans les 4 TU de végétation, `(void)device` ×3, BOM UTF-8 `VegetationSystem.hpp` ; ternaire mort `WorldRenderer.cpp:1838/1975`, `recordGiUpdate` indenté d'un cran ; `lastDrawn` périmé hors culling (`VegetationSystem.cpp:209`) ; `retired` de `ShaderLibrary` détruit sur délai 0,5 s et non sur preuve (`ShaderLibrary.cpp:209-214` — devient structurel avec Q3-15) ; `destroyOffscreenTarget(device)` paramètre inutile.

### P — optimisation évidente

Aucune. Les pistes de performance repérées (`treeSilhouette()` 143 l. recalculé deux fois
par frame, parseur legacy à chaque compile, `TerrainLightMap`) ne sont pas classées P faute
de mesure FrameProbe ; Q3-36 et Q3-37 les absorbent pour des raisons de structure.

---

## Positifs (modèles à citer dans les corrections)

- **`FarTerrain`** : une `Config` pour deux instances sans duplication, `refreshPipeline` qui
  fusionne premier build et rebuild, `BakeMailbox`, `Unique`, `destroy() = *this = {}` +
  mailbox sauvée — le modèle du lot eau et de Q3-33.
- **`NoiseVolume`**, **`FxRenderer::ensurePipelines`** (table `make(blend, shader)`),
  **`PostFx::buildPipelines`** (table `rebuild`) : la bonne forme du hot-reload.
- **`FrameComposer`** : composition pure, headless, « the ONE place » de `volumetricReach` ;
  le verrou `static_assert` de `FrameUniforms.hpp` (U3-3).
- **`ChunkStreamer`** + **`BakeMailbox`** : deux mécanismes nets, sept systèmes, stop flag
  propagé ; **`HeightField`** snapshots immuables et discipline « jamais pour les
  consommateurs golden ».
- **`ResidencyCache`** + traits `MeshCache`/`TextureCache` : §7 exactement, zéro recopie.
- **`canopyLevel`** (« ONE rule »), **`levelMesh`** switch plat, **`PlantTier`**,
  **`GrassSpecies.hpp`** (les espèces sont des données), **`forestAt`** source unique,
  **`materialWeightsCore(WeightRuleInputs)`** (une règle, inputs nommés, miroir GLSL
  documenté), **`flowMaskTouches`** (optimisation prouvée bit-identique dans son commentaire).
- **`RcTuning`** : knobs coût/qualité séparés des ressources, structurel vs live documenté.
- **`render()`** se termine par la table des matières des passes ; `record*` « une méthode
  par passe » ; `requestRegenerate`/`requestReseedVegetation` au safe-point ; `SettleTimer`
  contre les rebakes par drag ; « la texture est le toggle » pour contact/SSAO/mist.
- **RHI** : `DeviceCaps` granulaire, `BarrierStage` scopé, `textureDataBytes` partagé
  cooker/loaders/backends ; Vulkan `clampStagesToQueue`, `pipelineFor` (variantes par clé),
  deletion queue à politique unique, `ValidationCounters`, dummies par forme déclarée
  (`bindMissingDummies` = parité GL en un lieu) ; GL `GlCommandBuffer` sans branche,
  `makePipelineState`, `GlConvert.hpp`.
- **`GpuProbe`** : anneau 4 frames, back-pressure, `Scope` null-tolérant.
- **`FlyCamera::LookTrigger`**, `Camera3D::proj` reversed-Z, `obliqueProjection` : math
  header-only prouvée par `FrustumTest`.
- La **politique de commentaires** est tenue dans tout `rhi/` et la racine `render/`.

---

## Ordre de correction proposé (à arbitrer)

Un commit par brique ; build Release (purge `.obj` si en-tête partagé), `vksmoke` pour tout
ce qui touche `rhi/`, capture A/B pour tout ce qui peut changer un pixel (règle « graphics
need visual validation »), `landscape-report` + hash `MapBakerTest` si `engine/terrain` est
touché.

1. **Bugs et plomberie sans risque image** : Q3-43 (une ligne), Q3-47, Q3-48 (contrats),
   Q3-37 (retrait du parseur), Q3-42 (a-l, un helper par commit), Q3-27 (enums).
2. **RAII puis hot-reload** : Q3-33 (dix classes, une par commit, `UniqueShader`), puis
   Q3-15 (`WatchedPipeline`, 59 pipelines par système) qui ferme Q3-44 — zéro visuel.
3. **Mailbox et anneaux** : Q3-23 (descente de `BakeMailbox`/`ChunkStreamer` en core, puis
   `WaterSystem` ×3, `ChunkOcclusion`, RC), Q3-24 (`RingBakedMap`), Q3-39 (grille et
   échantillonneurs — la partie `maskTexelOf` avec hash terrain).
4. **Tables** : Q3-17 (`FormatTraits` + validation partagée, `vksmoke`), Q3-21 + Q3-46
   (couches splat, A/B), Q3-20 + Q3-22 + Q3-19 (familles, encodage, `ScatterRule` — une
   brique, golden recapturé une fois), Q3-30 (a-d), Q3-29 (presets).
5. **L'orchestrateur** : Q3-18 (`Mounted` + table), Q3-2 (trois décisions pures), Q3-8
   (`SsdmChain`), Q3-36 (anneaux), Q3-10 ; puis Q3-16 (table de lanes FrameUbo, L — le plus
   gros gain contre les bugs silencieux, après décision T-15) et Q3-45 avec elle.
6. **Les systèmes lourds** : Q3-34 (`PostFx`, A/B), Q3-9 (RC `update`), Q3-4 (`scatterGrass`,
   hash posé avant), Q3-11, Q3-38, Q3-12 ; Q3-26 (lanes A/B de l'eau — à faire **avant** le
   verdict « débit ×3 » pour que le verdict soit un réglage, pas un commit).
7. **Vulkan** : Q3-1 (découpage, après que 1-2 ont stabilisé l'interface), Q3-6, Q3-7, Q3-25
   (`QueueLane`, avec `MEADOWS_VK_SYNC_VALIDATION=1`), Q3-31.
8. **La frontière (décision dev)** : Q3-5 (a) mécanique d'abord (`TerrainNoise`,
   `entrySourcesFromBodies`, `WaterGeometry`), Q3-3 (`SimPhase`), Q3-28 ; puis Q3-14 (c)
   `WaterSimControl` / `TerrainParams` par `RenderView`, Q3-13 + Q3-40 (le 2D sous
   `WorldRenderer`), Q3-41, Q3-32 — préface de RENDERER-EXTRACT.
9. **Hygiène** au fil de l'eau : Q3-49 par fichier touché, Q3-50.
