# Revue de qualité — octobre 2026 (série Q)

> Point d'entrée de la **deuxième revue de qualité** du dépôt (la première,
> U1–U9, est close depuis le 2026-07-09 : `docs/AUDIT/README.md`). Objet :
> **le code spaghetti et les `if` à répétition qui gèrent des cas
> particuliers**, à repenser en mécanismes généraux, maintenables et
> extensibles. Pas d'optimisation (les systèmes ne sont pas finis), sauf
> évidences. **Aucun code n'est modifié par une vague** : chaque rapport
> observe et propose ; le dev arbitre ; les corrections viennent ensuite,
> brique par brique, un commit par brique.

---

## 0. Arbitrages

### 0.1 Décisions du dev sur cette série

| Date | Décision |
|---|---|
| 2026-10-23 | Périmètre : tout le dépôt, par vagues (Q1 → Q5, tableau §2). Tests hors périmètre. |
| 2026-10-23 | Livrable : rapport d'abord, corrections arbitrées entre deux vagues. |
| 2026-10-23 | Première vague : le terrain (chantier PAYSAGE courant). |
| 2026-10-24 | **R0 appliqué dans l'arbre de travail (non commité, build Release vert)** : Q3-43, Q1-38 (+ même motif `DungeonRecords`), Q2-42 court terme (3 kinds + éditeur `affinity`), Q2-44 (`registerAllFormTypes`), Q2-43, Q3-47 (`load()` idempotent, garde `FarTerrain` retirée), Q3-48 (contrats écrits dans `Device.hpp`/`CommandBuffer.hpp`, GL aligné : cible réelle + toutes les couches dans `copyTexture`, handle inconnu = `false` dans `timestampReady`), Q4-27 (`std::in_range`), Q4-28 (`baseDir` depuis `sourceName`), Q4-29 (contrat écrit dans `Nav.hpp` ; alignement des deux navigateurs grille laissé à T-18), Q4-30, Q5-15 (`BodyId` = id Jolt + 1), Q5-17 (purge après 120 frames sans usage), Q2-45 (`Device::validationMessageCount()`), Q1-42. Statut ◐ en §3 jusqu'au commit ; ✅ au commit. |
| 2026-10-24 | **Les dix bugs restants corrigés dans l'arbre de travail (non commité, build Release vert)** — décision dev « il faut corriger tous les bugs ». Terrain, **un seul bump `kTileBakeVersion` 121 → 122, cache v122 livré dans les deux dossiers de build** (`landscape-report` : 68 lacs / 176 rivières au lieu de 69 / 176, stage-1 17,8 s → 13,5 s, golden `MapBakerTest` ré-épinglé `10333209499799059016`) : Q1-37 (`visitParams` = une seule marche des champs pour `hashParams` ET le fingerprint du memo ; clé calculée une fois par échantillon, `zoneCell` reçoit le `Memo&` — c'est ce qui rend `zoneSampleAt` 2× plus rapide), Q1-39 (`Lake::covers()` dans `Hydrology.hpp`, 9 copies remplacées — `LandscapeReport.cpp:560` garde sa copie sans `+ 0.5f`, mesure hors bake), Q1-40 (`zoneFloorLift()` unique ; `biomeIdAt(x, z, corridor)` lit le corridor du plancher raffiné), Q1-41 (`core::Fnv64`/`kFnv64Basis` dans `Hash.hpp`, 10 sites moteur alignés ; les 4 copies des tests gardent leur constante locale), Q1-43 (`RiverSurface::tier` rempli par `WaterBodiesBuilder` — aucun consommateur gameplay encore). Render : Q3-44 (`buildSsdmPipelines` sous `Watch`, `probeGeneration`), Q3-45 (`textureSize(uShadowMap)` — identique au pixel à 2048, corrige 1024/4096), Q3-46 (`splatFamilyOf()` dans `SplatTextures.hpp`, `static_assert`). Sim : Q4-26 (`SavedEffectForm.effectId` + `EffectIdRemap` par acteur, test « rows sharing an effectId are paired again on load »). Engine : Q5-16 (souris × `SDL_GetWindowPixelDensity`, unité = pixels fenêtre). Tests lancés UNE fois, ciblés : `save forms*`, `*biomeIdAt*`, `map bake*` (Release) — verts. |
| 2026-10-25 | **R1 (le socle) et D1 option (B) appliqués dans l'arbre de travail (non commité, build Release vert, suite complète Release 818/818)** — décision dev « vas-y pour D1 et R1, par vague ». **Vague A (core/reflect/data, zéro effet disque, terrain bit-exact : golden `MapBakerTest` `10333209499799059016` inchangé, `landscape-report` même clé de cache `a3216e6312f64bc9`, même recensement)** : `core::fnv1a64`, `core::kPi/kTau` (`engine/core/Math.hpp`, 27 littéraux absorbés, même f32) ; `engine/reflect/Fields.hpp` = `copyFields` (les 2 copies), `hashReflected` (`hashTerrainGenTuning` devient un appel, flux identique), `fieldId` assertif ; `FieldFlags::ParentLink` (index enfant par flag, `QuestBranchForm.state` et `QuestTaskForm.branch` flaggés, test `FormIndexTest`) et `Deprecated` (`ssaoStrength`, `skyCloudRays`, `LightForm.castsShadow` ; loader avertit une fois, grille masque, validateur compte — `diffToRecord` continue d'émettre : aucune perte à l'export) ; `data::resolveSingleton<T>` (6 résolveurs) ; `GridSpec::cellRange/cellOf` (Finalize ×3, FireField ×3, WaterSim ×2) ; `maskTexelSize/maskTexelOf` dans `TerrainBase.hpp` (maskSample + 3 copies de `TerrainNoise`). **Vague B (data)** : `data::RecordStager` (`ensure/set/setAll/stage/live` avec rappel d'index) remplace les deux stagers de `DungeonRecords`/`MapRecords` ; les 13 `findField("…")->id` nus de `SaveGame`/`TerrainSculptTool`/`SaveState` passent par `reflect::fieldId` (ceux des tests restent) ; `core::ByteWriter/ByteReader` (`engine/core/BinaryArchive.hpp`) + `reflect::writeValue/readValue` (`BinaryValue.hpp`) portent le codec plugin — octets identiques ; **les 11 autres formats maison attendent D5**. **Vague C (descente core)** : `ChunkStreamer.hpp` et `BakeMailbox.hpp` déplacés dans `engine/core` (namespace `core`, 57 usages qualifiés) ; `core::chunkKey` remplace `HeightPatches::keyOf` (délègue) et les 4 copies (`ChunkOcclusion`, `TerrainSculptTool`, `TerrainCollision`, `VegetationCollision`) ; l'adoption par `WaterSystem`/`ChunkOcclusion`/RC reste R2. **Vague D = D1 (B)** : `reflect::EnumTable` (`names`, `allowEmpty`, `indexOf/accepts/acceptsValue/joined`), `FieldInfo::enumeration`, `REFLECT_FIELD_ENUM`, `parseEnum<E>/enumName` ; tables : `data/forms/Vocabularies.hpp` (statuts, slots, catégories de consommable, kinds/shadowMode de lumière, formes/blends de particule, compare d'anim, forme/trajectoire/aire de sort) + `kModifierOps`, `kDurationPolicies` (enum sorti du .cpp), `kExpiryModes`, `kCostPolicies`, `kConditionKinds` + `enum ClauseKind` + évaluateurs en tableau indexé (ferme Q4-8 et Q2-42 : `Keywords.cpp` n'a plus de liste), `kFactionStandings`, `kAiPackageKinds`, `kQuestStateKinds`, `kAwareStates` (i32 ordinal), `kCombatMoves` ; chaque enum consommateur épingle l'ordre par `static_assert` et parse par `parseEnum` (`parseOp/parseDuration/parseStatusType/parseStanding/parseSpell*/parseCombatMove`) ; loader : valeur hors table = champ ignoré avec le vocabulaire dans l'avertissement ; `validatePlugins` : `vocabularyErrors` (erreur) + `deprecatedWrites` (avertissement), imprimés par `cooker validate` ; grille : combo depuis `field.enumeration` (Str et I32). Pile de plugins livrée relue : aucune valeur hors table, 3 écritures `castsShadow` signalées. Tests ajoutés : `ReflectTest` ×5, `FormIndexTest` ×1, `PluginLoaderTest` ×1, `ValidateTest` ×1. Non fait : `GridView` (Q1-27), `FieldKind::Enum` option (A) (inutile tant que (B) suffit), vocabulaires ouverts (`FurnitureForm.category/screen`, `MarkerForm.kind`, `SpiritRuleForm.verb`, `damageType`) laissés à R3/D6. `.obj` purgés dans les deux dossiers de build. |
| 2026-10-25 | **R2 (la plomberie render) et R3 (les tables) appliqués dans l'arbre de travail (non commité, build Release vert, suite complète Release 818/818, smoke `MEADOWS_BOOT=sandbox` 30 s sans erreur)** — décision dev « vas-y pour R2 et R3, par vague ». **R2-A/B** : `rhi::Unique*` dans les 12 classes restantes (WaterSystem, GpuOcclusion, SkySystem, SpriteRenderer, TerrainShadeMap, ShadowMapper, FireScorchMap, TerrainLightMap, MistMap, LightClusters, UiSystem, ImGuiLayer — 0 `device.destroy*` manuel hors GpuProbe/MeshCache/caches, `UniqueShader` ajouté ; `destroy()` = idiome FarTerrain, les listes qui dérivaient disparaissent, `RadianceCascades::destroy` et `WorldRenderer::destroyOffscreenTarget` couvrent enfin tout) ; `render::WatchedPipeline` (`engine/render/WatchedPipeline.hpp`) porte le bracket de hot-reload : **59 pipelines** migrés, 15 compteurs et 12 `Watch` supprimés, `beginWatch/endWatch` privés + assert d'imbrication, les pipelines construits une fois (SSDM ×6, `rc_probe`, UiSystem) gagnent le hot-reload (ferme Q3-44) ; `FormatTraits` (`kFormatTraits[]` + `static_assert` ordre/compte dans `Rhi.hpp`, colonnes upload/gpu/bc/depth/srgb/uploadable/storage/cooked/cookCode) remplace les 9 tables des 3 fichiers, `validateTextureDesc`/`validateBindGroupDesc` partagés en tête des deux backends, `CookedTexture` lit la colonne `cookCode`. **R2-C** : `core::BakeMailbox` adopté par `WaterSystem` (4 lanes : pool, local, far, sim — `shared`/`generation`/4 `inFlight` supprimés), `ChunkOcclusion`, `RadianceCascades` (tuile) ; `BakeMailbox::strayed()` pour les 4 cartes en anneau ; `core::QuietStamp` remplace le coalesceur copié (FarTerrain, WaterSystem). **R2-D** : `engine/render/PipelinePresets.hpp` — `kDepthOpaque/ReadOnly/Backdrop/Caster/Off` (23 descripteurs), `kDepthBias/kDepthBiasSlope` (6), bindings nommés `kFrameUbo` (44) `kShadowUbo` `kLightsUbo` `kRcUbo` `kRcCascadeUbo`. **R3 terrain (bit-exact : golden `MapBakerTest` `10333209499799059016` inchangé, `landscape-report` rebaké 27 s : 43 lacs 25+18, 327 rivières, 1847/19707 nœuds, identiques)** : `PoiTypeTraits` (28 lignes, `KernelShape` 11 formes, `PoiFamily`, règles de taille et repli à sec en colonnes ; `kernelAt` switche sur la forme, `MapExport` lit la famille), `kBorderTraits[4]`, `TierTraits`/`tierProfile`. **R3 render (golden `VegetationScatterTest` `10031847806692189656` inchangé, 106 shaders validés par glslc)** : `PropFamily`/`kFamilies[]` + `familyOf` (12 sites de plages d'ordinaux), `PropEncoding.hpp` + `prop_encoding.glsl` (drapeaux carte/écorce/rigide/texturé nommés une fois, 4 shaders), `ScatterRule` (les 5 copies + 2 paliers = 7 règles, une boucle, hook `preAccept` débris), `splat_layers.glsl` miroir de `SplatTextures.hpp` (hexFamilyLayer + 7 littéraux de terrain.frag), `kLodBands`, `kZoneSpeciesBias`. **R3 sim** : `kDamageTypes[12]` + `dominantDamageType` (7 chaînes, 5 fichiers ; **seul changement visible : le cue `Cue.Hit.<type>` suit le canal dominant et non le premier**), `kTimedStates[4]` + `grantTimedState`/`tickTimedStates` (seul écrivain timer+tag), `ItemForm` parent réfléchi des 4 items (`itemWeight`/`itemUnremovable` = un `find<ItemForm>` ; ordre des champs réfléchis changé, noms inchangés), `kVitals[3]` (+ colonne `spendPausesRegen`), `gameplay/ability/Tags.hpp` (43 littéraux + 5 remplacés, `kRows[].tag` compris), colonnes `OnTrigger`/`DotSource`/`dotRate`/`dotOut` sur `kRows`, `kSpellMatrix[20]` + `spellCell` (`spellSupported` = la table, `executeSpell` = un switch plat, `SpellsTest` itère la table), `GroundProps` indexés par la table data (`classOf` résolu une fois), `PackageKind` + `parsePackageKind` (switch plat dans `NpcScheduleController`, kind inconnu = warn), `events::` + `kEventNames[25]` + `isKnownEvent`, lint `cooker validate` des champs `.event` (2 fils de données signalés : `OnAcceptQuest` sans auditeur, `OnBanditDeath` jamais émis — probable tâche de quête muette, à arbitrer), `FurnitureInteraction` + `restHours` (compat : dérivés de `category` quand absents), `CombatStyle` (default/melee/healer/archer). **R3 dungeon (bit-identique par seed, 247 tests ciblés)** : `AnchorKind` + un seul `vector<Anchor>` (+ `anchorsOf/countOf/firstOf`), `kAnchorEmits[7]` + une boucle côté records, `kEdgeKinds[4]` (6 sites). **R3 game UI** : `UiRouter` en table `{modèle, événement, handler}` (13 routes, `MenuAction` + `wait<N>`), `kSteppers[4]` dans `OptionsController`, noms d'`InventoryView`. **Non fait, à arbitrer** : Q2-18 (hostile/garde par faction — aucune `FactionRelationForm` ni faction joueur dans les données, `FactionRelations::build` n'est appelé que par les tests), Q2-23 (hotkeys modales : `LandscapeScene`), Q3-30 partiel (`DetailProfile`/`kLeafShapes` de SpaceColonizationTree), le `step(0.35)` de `shadow_prop.vert` (prouvé sans effet, conservé), `RingBakedMap` réduit à `strayed()` (le reste des 4 cartes diffère réellement). Un test touché hors périmètre : `FollowerFormsTest` (`combatStyle = "berserk"` → `"melee"`, valeur hors vocabulaire). |
| 2026-10-25 | **R4 (les extractions structurelles) appliqué dans l'arbre de travail (non commité, build Release vert à chaque vague, suite rapide 717/717, goldens inchangés, `vksmoke` 0 échec, smoke `MEADOWS_BOOT=sandbox|story` propres)** — décision dev « vas-y pour R4, par vague » + « note les choses qui doivent être réalisées plus tard » (→ **§7 Reports**, P-1…P-19). **Vague A terrain (bit-exact : golden `MapBakerTest` `10333209499799059016` inchangé, `landscape-report` rebaké identique hors chronos)** : `extractMapHydrology` → `engine/terrain/generation/MapHydrology.{hpp,cpp}` (`MapGroundSampler`, contexte `HydroReleveling` aux enveloppes figées après `relakes`, 16 fonctions de passe, table `kRelevelPasses[]` qui tient l'horloge `lap`, `HydroRelevelStats`) ; `bakeTileStage1` en 9 fonctions nommées (deux `lap` réattribués) ; `TileBake.cpp` 2752 → 1108 l. ; `finalizeWater` en 9 fonctions autour d'un `WaterPass` (`preCarve` au même point, `fanFalloff01` remplace les deux lambdas jumelles) ; `landscapeReport` en `ReportContext` (`ProceduralControls` construit UNE fois, `longestX/Z` partagés) + 14 instruments nommés ; `bakeMapCmd` en 7 étapes ; `bakeMap` en table de 7 stages ; commentaire croisé `bakeSoloTile` ↔ `bakeMap` (Q1-9). **Vague B sim/donjon** : `stageDungeonRecords` → `DungeonStageInputs` + 8 étapes (`setAll` NON généralisé : P-17) ; `populateAnchors` → `pickBarrierCell` pur + `populateCorridors/Rooms/Goal` (tirages RNG inchangés, 247 tests) ; `Followers.hpp` → 5 en-têtes par domaine (`FollowerFollow/Life/Affinity/Progression/Party`), parapluie conservé, 8 includeurs resserrés. **Vague C render (zéro pixel voulu : capture A/B dev à faire)** : `WorldRenderer` — `Mounted` (cfg ∧ caps résolu une fois, 0 `cfg.*` hors résolution), table `kSystems[]` des 15 systèmes pour les 4 verbes (destroy en ordre inverse de création : seul changement d'ordre), `FrameDecisions.{hpp,cpp}` (`pickKeyShadows`, `fillLightsUbo`, `seaVisibleInFrustum`), `SsdmChain.{hpp,cpp}` (22 membres sortis de l'orchestrateur), `array<FarTerrain,2> farRings` + `FarRingInputs` (silhouette calculée une fois par frame, demi-spans depuis `Config::span`) ; `render()` 593 → 178 l., `ensureOffscreenTarget` 244 → 155 ; `PostFx` 85 → 43 membres (`RenderTarget`, `DerivedTarget`, `TemporalHistory`, `MediaPass`, `FilteredPass`, `renderMist`/`renderSkyClouds` fusionnés — **changement assumé : `destroy()` préserve les cinq knobs ; `froxelFog` suit le mount à chaque `create`**) ; `RadianceCascades::update` 356 → 74 l. (`snapOrigins` unique, `bakeTile` statique headless, `record*`) ; `scatterGrass` 395 → 32 l. (`ChunkFrame`, `CornerLattice`, `ClumpPicker`, `sampleCell`, `scatterCell` ; **nouveau golden `GrassScatterTest` `8288326442141430962` posé avant, inchangé après**) ; Vulkan en 7 TU (`VulkanInternal.hpp` + `VulkanDevice/CommandBuffer/Shader/Transfer/Frame/Resources.cpp`, `VMA_IMPLEMENTATION` dans un seul TU), `pickQueueFamilies` (familles identiques, une seule requête), `planImageUpload` + deux exécuteurs ; `QueueLane` NON fait (P-18). **Vague D game (smoke sandbox + story à chaque brique ; validation en jeu dev à faire)** : D1 `WorldKind`/`WorldPolicy` sur `StreamingContext` décidés par `decideWorldKind()` (31 gates convertis, `sandboxActive` reste l'identité extérieure qui survit à un intérieur), `restoreMode(target, feet)` seul écrivain de mode, `BootMode` + `beginPlay` partagé menu/boot (closure `UiRouterContext::beginPlay`) ; D2 `WarmupController` (15 scalaires, 3 callbacks) ; D3 `SandboxWorldController` (20 membres, `republishWorld` ×1, `rebuildCollision(onlyIfBuilt)` ×1 pour les 6 copies, `cacheRoot`, bbox/éviction/clamp dédupliqués) ; D4a `FireDirector` (`placeFlames` pur) + `AmbientFxController` ; D4b gestes dans `SpiritDirector` via `SpiritContext` (30 champs), quatre TU par élément `SpiritWater/Earth/Fire/Wind.cpp`, `resetGestures(ResetReason)` reproduit les deux listes (console / carte) ; D5 `update()` 525 → 61 l. en 12 phases nommées (`drainCompletions` → `updateUi` → `tickPhysics` → `updateWorld` → `updateSim` → `extractScene` → `updateActorsAndFx` → `updateAmbientAndAudio` → `extractFx` → `extractViewmodel` → `updateAtmosphere` → `updateInput`), gates `FrameGates` décidés une fois ; le voyage pendant n'a plus qu'UN site (fin d'`updateWorld`, `!uiPaused`) : un voyage demandé par un trigger Lua est différé d'une frame (seul écart frame à frame). `LandscapeScene.cpp` **8637 → 5040 l.**, `.hpp` 1104 → 705. **Effets visibles à valider** : chargement de save en mode Edit ouvre désormais la console (effet du funnel) ; `onExit` passe par `exitPlayMode` complet ; ordre de destruction des sous-systèmes render inversé ; voyage par trigger +1 frame. **Non fait** : P-17/P-18/P-19 (§7). |
| 2026-10-26 | **R5 (la définition unique d'un champ, D3) appliqué dans l'arbre de travail (non commité, build Release vert, suite rapide 727/727, goldens inchangés, `landscape-report` identique, smoke sandbox + story propres)** — décision dev « fait le commit puis enchaîne sur R5 » (série Q commitée `44c9756`) + quatre décisions prises le 2026-10-26 : **plages de sliders = métadonnée réfléchie** (`REFLECT_FIELD_RANGE`), **`SavedStatsForm` GÉNÉRÉE** depuis les `TypeInfo`, **FrameUbo nettoyé** (lanes mortes supprimées, noms alignés), **params terrain réfléchis (option c) avec bump `kTileBakeVersion` 122 → 123 et cache livré**. **Vague A (sans décision, bit-exact)** : `engine/reflect/FieldLane.hpp` (`FieldLane<Params, Form, T>`, `applyLanes/captureLanes/forEachLane`, variadique sur plusieurs tables) ; `TerrainGenTuning.cpp` : 23 tables / 141 lanes remplacent les 155 × 2 copies miroir (`apply` 168 → 50 l., preuve mécanique 159/159 et 158/158 paires identiques ; restent à la main les éléments de tableaux C, `tiers`, `rampartSteps` i32↔u32, `fleuveArea` unidirectionnel), `WaterBodiesBuilder`/`BiomeMapBuilder` en lanes ; côté jeu `kAtmosphereLanes` (13, les deux sens : `bootstrapData` + `saveRenderTuning`), `kWaterSimLanesBool`, `WeatherController` sur `reflect::FieldLane`, `kWorldStateLanes*` (9, capture seule) ; **`FrameLanes.hpp`** : table X-macro `MEADOWS_FRAME_LANES(X)` (type, nom, init, type GLSL, nom GLSL, compte, doc) génère `struct FrameUniforms` ET le bloc GLSL `shaders/frame_ubo.glsl` (fichier réel inclus par `common.glsl`, épinglé octet pour octet par `FrameLanesTest` qui imprime le texte à coller en cas d'écart) + `MEADOWS_FRAME_CONSTANTS(X)` (13 constantes CPU↔GLSL : clusters, `kMaxLights` **64** et non 24, RC, matériaux d'eau, espèces d'herbe, bande de brume, portée des arbres lointains, demi-span du quad de mer — littéraux remplacés dans 8 shaders) ; Q3-10 : `clusterInfo` et l'atlas key-shadow entrent dans `FrameComposerInputs` (plus de patch après le composer) ; `SaveCoverageTest` (couverture save ↔ composants, exclusions explicites `AttributeSet.damage`, `maxHealthOverride`). **Vague B (décisions)** : `FieldRange { lo, hi, decimals, logarithmic, step }` + `FieldInfo::range/offset`, `TypeInfo::findFieldAt`, `reflect::memberOffset` (calculé sur un stockage brut, **jamais** en construisant une instance — la première version construisait chaque type à l'enregistrement et le constructeur de la Form générée re-rentrait dans sa propre `staticTypeInfo()` : initialisation récursive d'un statique local = blocage de tout processus, leçon), 179 champs stampés dans `LandscapeForms.hpp`, `drawLanes<kTable>` dans `RenderTuningPanels` (145 des 192 sliders dessinés par table, ~40 bornés par la plage réfléchie, 25 manuels ; ordre et libellés identiques, vérifié mécaniquement ; bouton « Log TOML » retiré), `PropertyGrid` dessine un slider pour tout champ borné ; `SavedStatsForm` = un seul champ manuscrit `parent` + `vector<Value>` par slot, `TypeInfo` construit une fois depuis les 10 composants persistés (mêmes noms → mêmes ids `fnv1a` → saves compatibles, défauts vérifiés égaux aux anciens, slots partagés assertés), accesseurs `get<T>("name")`, consommateurs jeu inchangés (seuls 5 tests lisaient des champs) ; FrameUbo **81 → 79 lanes, 2304 → 2224 o** (`keyShadowViewProj`, `keyShadowInfo`, `waterReserved0` prouvés morts), 4 noms GLSL alignés (`uStylizedDiffuseInfo`, `uStylizedShadowInfo`, `uStylizedSpecInfo`, `uKeyShadowAtlasViewProj`), lane `vegInfo` (x gate écorce, y intérieur, z portée impostor) libère `uSplatVarietyInfo.w`/`uCascadeSplits.w`/`uFogLayerInfo.w`, tableaux RC `[16]` → `[kRcMaxLights]` (latent seulement) ; terrain : 20 structs de params `REFLECT_BEGIN(X, void)` + `hashKey(X)` = `hashReflected` + membres explicites listés (tableaux C, `tiers`, `archetypes`, sous-structs), `networkFileKey`/`mapBakeKey` sur ces clés (plus de gate silencieuse), mémos par `operator== = default` (copie exacte, plus de fingerprint), `hashParams`/`visitParams`/`WordMixer` supprimés ; **oubli réel trouvé : `RhythmParams::zones` manquait à la clé du cache réseau** (le réseau maître sur disque pouvait servir un calcul « zones on » à un bake « zones off »). `.obj` purgés. Docs : `RENDERING.md` §2.3 (plus d'append-only : la table), `PAYSAGE.md`. **Cache v123 livré** : Debug `map_0_0`/`map_1_0`, Release les cinq cartes présentes, via `cooker landscape-report <dir> X Z`. **Non fait / reports** : les 11 knobs matière du terrain recopiés dans `saveRenderTuning` (sliders dans la scène, P-20), `hashTerrainGenTuning` désormais sans appelant (à supprimer ou garder, P-21), `mistSteps`/`vegViewRadius` : slider plus étroit que le clamp de chargement (conservé tel quel, visible dans la Form), LightsUbo reste un miroir manuel append-only. |

### 0.2 Arbitrages de juillet 2026 à NE PAS re-proposer

Repris de `docs/AUDIT/README.md` §0 ; une ré-ouverture exige une preuve nouvelle,
dite explicitement dans le finding.

- **H-a « table de descripteurs de kind »** rejetée (coupler binaire↔ImGui↔Lua
  violerait §2.10) ; `reflect::visit` est le mécanisme retenu ; Reader,
  `valueFromString`, `luaToValue` restent des switches ; `valueRepr` ≠
  `valueToString`.
- **`copyFields` / `copyMatchingFields`** restent (deux opérations, un appelant
  chacune).
- **H-c `Signal<T>`** rejeté (EventBus, CueRegistry, `UiModelEventHandler`
  dispatchent différemment).
- **H-d `Handle<Tag>`, U2-04 ShaderDesc** différés au chantier Vulkan. (Q3 note : le
  backend Vulkan compile le GLSL via shaderc à l'exécution et réfléchit les bindings —
  la dette U2-04 est résorbée de fait, à clore ; `Handle<Tag>` reste non re-proposé, la
  `HandleTable` de Q3-31 partage la table, pas le type.)
- **U5-4** (dédup des trois éditeurs d'`EditorScene`) différé à la refonte
  « l'éditeur devient une couche SceneStack ».
- **U5-6 / U5-9 / U5-10** acceptés tels quels (cache `gActive` du PropertyGrid,
  DemoScenes, reload synchrone d'EditorScene).
- **Gardés dans la scène à dessein** : corps des commandes console
  (`createConsole`), abonnements eventBus d'`onEnter`, `makeEvalContext`,
  `performTravel`, transitions de mode / `restoreMode`, ouverture du dialogue,
  moitié « application du load » d'`onEnter`, `finalizeActorSpawn`,
  `drawUi` / `drawSkyUi` / `drawGameplayUi`.
- **U4-7** : les constantes qui restent en C++ portent `[cpp-tuning]` ; seuls les
  réglages qu'un moddeur toucherait vont dans un `*TuningForm`.
- **§2.9** : les écritures du tick (DoT, regen, mort par buildup) sont des
  « execution calculations » sanctionnées ; router le DoT via `applyDamage` =
  régression (double mitigation) — rejeté.
- **ImGui hors RHI** = exception acceptée (U2-07/U8-5). **Q5 constate qu'elle est
  résorbée** : `ImGuiLayer` est réécrit sur le RHI (plus d'`imgui_impl_opengl3`), à
  clore. **Spawner** : seeding sanctionné (U7-5), poids d'includes accepté (U7-8).
  **§2.10 strict** (SDL hors de la lib de base) différé au chantier frontend/Godot.
  Pas de finding perf sans mesure FrameProbe (U1-10/U2-01/U7-7/U8-6).
- **Ré-ouverts par la série avec preuve nouvelle** (à re-trancher) : **H-b**
  `cloneFields` (« un appelant chacun ») — `DungeonRecords.cpp:78` est une 2e copie
  identique de `EditSession::copyFields` (Q4-21) ; **U8-1** scan manuel de la base —
  clone survivant dans `Factions.cpp:38` et `WorldModel.cpp:16` (Q4-25) ; **U3-4** et
  **U3-7** clos à moitié (Q3-39, Q3-33) ; **U2-04** `ShaderDesc` GLSL résorbé de fait
  par shaderc (Q3).

---

## 1. Méthode

**Deux passes par vague**, dans cet ordre :

1. **Fichier par fichier** — rôle, taille, fonctions > 150 l., chaînes
   `if/else if` de cas particuliers, duplications locales, code mort, commentaires
   hors politique 2026-07-22 (dates, « brique/chantier », anciennes valeurs,
   attributions). Sortie : un tableau par répertoire.
2. **Système par système** — relecture transversale : même enum branché en N
   endroits, même helper copié, mécanismes parallèles, frontières de modules
   (`python tools/scripts/layers_audit.py`), objets-dieux. Sortie : les
   **généralisations proposées**, chacune avec le pattern (citations), le
   **mécanisme existant à étendre** (CLAUDE.md §2.11 : jamais une couche
   parallèle), un croquis de design, l'effort et le risque.

**Classes de findings** (pas d'axe « perf ») :

| Classe | Sens |
|---|---|
| **S** | spaghetti structurel : objet-dieu, fonction > 300 l., couplages croisés |
| **G** | cas particuliers à **généraliser** : chaînes `if` par type/style/nom → table de traits, stratégie, pipeline de passes nommées, registre |
| **D** | duplication : helper copié, formule dupliquée, plomberie recopiée |
| **B** | bug trouvé au passage — signalé, jamais corrigé en silence (liste à part, §4) |
| **H** | hygiène : code mort, commentaires-journal, nommage |
| **P** | optimisation **évidente** seulement (rare) |

**Sévérité** crit / haute / moyenne / basse = **coût de maintenance futur**, pas
coût de la correction. **Effort** S / M / L. Leçon de juillet reprise : *les
observations sont fiables, les remèdes sont des hypothèses* — chaque remède est
relu contre le code avant d'être écrit, et il nomme le mécanisme qu'il étend.

**Garde-fous** : terrain → toute généralisation est soit **bit-exacte** (preuve :
`cooker landscape-report` et hash `MapBakerTest` inchangés), soit un bump de
`kTileBakeVersion` **avec cache livré** ; jamais de retrait d'un réglage dev sans
le nommer et garder une bascule A/B ; les ancres de ligne périment, les rapports
citent les **symboles**.

---

## 2. Vagues

| Vague | Module | Lignes | Statut | Rapport |
|---|---|---|---|---|
| **Q1** | `engine/terrain/` (+ `generation/`), `world/terrain/`, seam `game/MapBaker`, `game/TerrainBakeStreamer`, `game/TerrainGenTuning`, outils terrain du cooker | ~24,6 k | ✅ **rapport livré 2026-10-23** — 48 findings (S 9, G 10, D 17, B 7, H 4, P 1), arbitrage dev en attente | `Q1-terrain.md` |
| **Q2** | `game/` (scènes, contrôleurs, panneaux, console) + reste de `tools/` | ~38 k | ✅ **rapport livré 2026-10-23** — 50 findings (S 15, G 14, D 12, B 4, H 4, P 1), arbitrage dev en attente | `Q2-game-tools.md` |
| **Q3** | `engine/render/` (+ `landscape/`, shaders) + `engine/rhi/` (interface, GL, Vulkan) | ~37,6 k | ✅ **rapport livré 2026-10-23** — 50 findings (S 14, G 18, D 10, B 6, H 2, P 0), arbitrage dev en attente | `Q3-render-rhi.md` |
| **Q4** | simulation : `data/`, `world/` (hors terrain), `gameplay/`, `script/`, `quest/` | ~22 k | ✅ **rapport livré 2026-10-23** — 32 findings (S 6, G 14, D 5, B 5, H 2, P 0), arbitrage dev en attente ; **décision T-3 instruite** | `Q4-simulation.md` |
| **Q5** | reste d'`engine/` : racine, core, reflect, ecs, platform, nav, assets, anim, physics, fx, audio, ui, dungeon | ~10,5 k | ✅ **rapport livré 2026-10-23** — 19 findings (S 2, G 7, D 5, B 3, H 2, P 0), arbitrage dev en attente | `Q5-engine.md` |

**Série complète : 199 findings** (S 46, G 63, D 49, B 25, H 14, P 2) sur ~133 000 lignes
relues intégralement en cinq vagues. La vision globale et l'ordre des chantiers de correction
sont en **§6**.

Mesures de départ (2026-10-23, hors tests) : 24 fichiers > 800 l., ~45 fonctions
> 200 l. ; `game/scenes/LandscapeScene.cpp` est repassé de 1 852 l. (clôture de
juillet) à 8 635 l. ; `extractMapHydrology` (`TileBake.cpp`) fait ~1 900 l.

---

## 3. Table maîtresse

Statuts : ✅ fait (commit en §0.1) · ◐ en cours · ✖ tranché sans action (raison
en §0.1) · vide = ouvert.

| id | statut | vague | sév | classe | symbole (fichier) | description | effort | bit-exact |
|---|---|---|---|---|---|---|---|---|
| Q1-1 | ◐ | Q1 | crit | S | `extractMapHydrology` (`TileBake.cpp`) | 1 923 l., quinze passes inline (les `lap()` sont déjà la table) → contexte + une fonction par passe | L | oui |
| Q1-2 | ◐ | Q1 | haute | S | `finalizeWater`, `bakeTileStage1` (`Finalize.cpp`, `TileBake.cpp`) | 645 / 404 l. en sections inline → même découpage | M | oui |
| Q1-3 | ◐ | Q1 | haute | S | `landscapeReport`, `bakeMapCmd`, `bakeMap` | 908 / 458 / 371 l. en blocs anonymes → contexte + table d'instruments / étapes nommées | M | oui |
| Q1-4 | | Q1 | haute | S | `extractSnapshot` (`WaterSim.cpp`) | 422 l. : hystérésis + connectivité + marching squares 16 cas en code → 3 fonctions + `kMsCases[16]` | M | oui |
| Q1-10 | ◐ | Q1 | haute | G | `PoiType` (`PoiPlan.cpp`, `MapExport.cpp`) | six chaînes de traits + couleur par plages d'ordinaux → `PoiTypeTraits` + `KernelShape` | M | oui |
| Q1-11 | ◐ | Q1 | haute | G | `BorderStyle` (`zoneSampleAt`) | cinq sites de branchement → `kBorderTraits[4]` (modèle `MapEdgeStyle`) | S | oui |
| Q1-12 | | Q1 | haute | G | `pinLakes` / `pinRivers` (`WaterSim.cpp`) | dix gardes par symptôme, deux rasterisations → `WaterBodyRaster` + un BFS | L | **non** (replay + dev) |
| Q1-20 | | Q1 | haute | D | `ProceduralControls::at/refineFloor/biomeIdAt` | zone-lift ×3 (une divergente), trois arbres de modes par texel → `floorAt` + `LayerMode` | M | oui si variante gardée |
| Q1-21 | | Q1 | haute | D | `stepWindow` ≡ `solveLevel` | noyau virtual-pipes copié à la ligne, divergences non déclarées → `PipeKernelParams` | M | oui (cross-check) |
| Q1-22 | ◐ | Q1 | haute | D | `applyTerrainGenTuning`/`capture…`, builders `world/terrain` | 155 × 2 en miroir, copies champ à champ → `FieldLane` dans `engine/reflect` ; à terme params réfléchis | M / L | oui |
| Q1-23 | ◐ | Q1 | haute | D | `hashParams` ×5, FNV ×6, `networkFileKey`, `mapBakeKey` | hash manuels (un champ oublié = cache périmé) → `Fnv64` + `hashReflected` | S / M | **non** → bump |
| Q1-24 | | Q1 | haute | D | « point sous un lac » ×9 | `Lake` POD sans méthode → `Lake::covers()` | S | oui ×8 |
| Q1-25 | | Q1 | haute | D | flood local 8 m ×2 (`TileBake.cpp`) | copie de `priorityFloodFill` → `floodBasinBelow` (`GridOps`) | M | oui |
| Q1-5 | | Q1 | moyenne | S | `collectFarWater` | 283 l., cinq étapes → trois fonctions pures | M | A/B visuel |
| Q1-6 | | Q1 | moyenne | S | `WaterSimState` | onze plans sans invariant → `forEachPlane` + `resize` | S | oui |
| Q1-7 | | Q1 | moyenne | S | `masterRiversNear` | `std::async` depuis les workers de bake → exécuteur injecté / JobSystem | S-M | oui |
| Q1-8 | | Q1 | moyenne | S | `TerrainBakeStreamer.hpp` | 3 responsabilités, RHI dans la chaîne du bake → `WaterFiles.hpp`, `FarWater.hpp` | S | sans effet |
| Q1-13 | ◐ | Q1 | moyenne | G | tiers de rivière (6 sites) | ternaires par tier → `TierProfile` | S | oui |
| Q1-14 | | Q1 | moyenne | G | ids palette/pièce, spawn biomes | entiers magiques en 6 fichiers → `enum class Palette`, `ZonePiece` | S | oui |
| Q1-15 | | Q1 | moyenne | G | gates « 0 = off » (16 sites) | convention hétérogène → documentée une fois / `bool` | S | oui |
| Q1-16 | | Q1 | moyenne | G | `puddleMode`, `FireState` en u8 | u32 à 3 valeurs, 22 casts → enums | S | oui |
| Q1-17 | | Q1 | moyenne | G | `cooker/Main.cpp` dispatch | 15 `if (command == …)`, usage à part → table de sous-commandes | M | sans effet |
| Q1-18 | | Q1 | moyenne | G | `WaterSolveTool`, `ErosionBench` | bakent hors de la pile du jeu → `<gameDir>` + `--defaults` | S | — |
| Q1-26 | ◐ | Q1 | moyenne | D | bbox → cellules ×10, `lround` ×10 | → `GridSpec::cellRange/cellOf` | S | oui |
| Q1-27 | | Q1 | moyenne | D | échantillonneurs ×7 | carré vs rectangulaire → `GridView` | M | oui si expression gardée |
| Q1-28 | | Q1 | moyenne | D | géométrie de polyligne (~20 sites) | → `RiverOps.hpp`, `segmentProject`, `notchBand` | M | oui |
| Q1-29 | | Q1 | moyenne | D | pente centrale ×3 | → `slopeMagnitude` | S | oui |
| Q1-30 | | Q1 | moyenne | D | bootstrap pile de plugins ×6 | diverge de la gate de langue → `loadGameStack` | S | oui |
| Q1-31 | | Q1 | moyenne | D | horloges ×4, dispatch workers ×6 | → `PhaseClock`, `forEachIndexed` | S | sans effet |
| Q1-32 | | Q1 | moyenne | D | warp / fbm signé ~60 sites | → `noise::sfbm`, `noise::warp` | S | oui si orthographe gardée |
| Q1-33 | | Q1 | moyenne | D | `seaLevel` ×35 sous-structs, seeds ×2, `mapGrid` ×3, marges | → `BakeEnvironment`, constantes nommées | M | oui ; hash Macro → bump |
| Q1-34 | | Q1 | moyenne | D | 8 formats binaires maison, `tile_%d_%d_v%u` ×8 | → `BinaryArchive` symétrique, `sliceStem` | M | au prochain bump |
| Q1-35 | | Q1 | moyenne | D | `LakeSurface`/`RiverSurface` ×3 | → `toLakeSurface()` | S | oui |
| Q1-44 | | Q1 | moyenne | H | ~130 commentaires-journal | politique 2026-07-22 | S | sans effet |
| Q1-45 | | Q1 | moyenne | H | code mort (`trunk`, `finalizeTerrain`, TRG eau, `Authoring`…) | | S | TRG4 = bump |
| Q1-46 | | Q1 | moyenne | H | `namespace render::*` sur du headless | + `terrain::height` dans `render/` | M | sans effet |
| Q1-9 | | Q1 | basse | S | `bakeSoloTile` vs `bakeMap` | contrat d'ordre écrit deux fois | S/M | oui |
| Q1-19 | | Q1 | basse | G | dig borné ×4 | → `digBounded()` | S | oui |
| Q1-36 | | Q1 | basse | D | petites duplications (a-k) | | S | oui |
| Q1-47 | | Q1 | basse | H | `RiverHeadType` en u32 nu, MNW2 `reinterpret_cast` | | S | sans effet |
| Q1-48 | | Q1 | basse | P | diagnostics stage-1 29 Mo, `gNetworkMemo` sans borne | | S | sans effet |
| Q2-1 | ◐ | Q2 | crit | S | `LandscapeScene::update` (685 l.) | trois fils sous 35 gates, voyage exécuté 2× → séquence nommée + `WorldKind` + `SandboxWorldController` | L | jeu |
| Q2-2 | ◐ | Q2 | crit | S | magie des esprits dans la scène (2 840 l., 58 méthodes) | `SpiritDirector` propriétaire déclaré → `SpiritContext`, gestes dans le director, `FireDirector`, `AmbientFxController` | L | jeu, par élément |
| Q2-3 | ◐ | Q2 | haute | S | carte bornée / sandbox (~700 l., 20 membres) | transaction ×1,5, collision ×6, cache path ×6 → `SandboxWorldController`, `republishWorld`, `rebuildCollision` | L | jeu |
| Q2-4 | | Q2 | haute | S | `createRenderResources` (287 l.) | import de scans + 14 GUIDs en tables C++ → `ScatterPropForm` + `RendererAssets::loadScatterProps` | M | A/B visuel |
| Q2-5 | | Q2 | haute | S | `InteractionController::update` (343 l.) | 4 machines, 3 switches `PromptKind`, 12 closures → `PromptSpec[]`, événement `OnInteract`, `FadeMachine` | M | jeu |
| Q2-6 | | Q2 | haute | S | `Npc` (54 champs, 12 booléens) | priorité en chaîne if, miroirs de tags ×5 → `NpcLife`/`NpcActivity` + `decideNpcActivity` pur | M | village |
| Q2-7 | | Q2 | haute | S | `NpcDirector.hpp` hub (13 TU, RHI, `npcs()` mutable) | 4 cycles → `Npc` = sim, `NpcGpu`, `NpcRegistry` + commandes | M | sans effet |
| Q2-8 | | Q2 | haute | S | `updateSpiritFire` + ~900 l. de feu | → `FireDirector`, `placeFlames()` pur | M | A/B feu |
| Q2-16 | ◐ | Q2 | haute | G | matrice des sorts ×3, 16 `Mode`, 4 cellules hors file | → registre `(trajectoire, élément, verbe, canalisé)` | M | jeu |
| Q2-17 | ◐ | Q2 | haute | G | mobilier en strings, `"bed" → 8 h` | `FurnitureForm.screen` précédent → `restHours`, `interaction` | S + data | contenu à migrer |
| Q2-18 | | Q2 | haute | G | kinds de packages IA en strings, `combatStyle` | aucun registre dans `gameplay/ai` → `PackageKind` + parse/name, `CombatStyle` | S-M | village |
| Q2-19 | | Q2 | haute | G | 4 tables (type, champ) → widget en strings | `PropertyGrid` chaîne de if → `FieldMeta` réfléchi (T-3) ou table unique | M / S | UI dev |
| Q2-20 | ◐ | Q2 | haute | G | genre de monde = 3 booléens ×63 sites | → `WorldKind` + `WorldPolicy` sur `StreamingContext` | M | smoke 3 genres |
| Q2-30 | | Q2 | haute | D | 9 gestes canalisés, cycle upkeep ×9 | → `Upkeep` + `ChanneledGesture` | M | ordre à reproduire |
| Q2-31 | | Q2 | haute | D | identité follower ×3, RefId→ActorForm ×6, displayName ×3 | → `data::formAs<T>`, `displayNameOf`, fonction libre headless | S | nul |
| Q2-32 | ◐ | Q2 | haute | D | `RenderTuningPanels` 190 widgets, `saveRenderTuning` 24, `atmos` 13, `WorldStateForm` 14 | → `FieldLane` dans `engine/reflect` + `drawLanes` (T-1) | M | disposition |
| Q2-33 | | Q2 | haute | D | bootstrap pile ×2 sans gate, `WorldDemoScene` 2e pile | → `loadGameStack` (T-8), `game::Layers.hpp` | S | aucun |
| Q2-9 | ◐ | Q2 | moyenne | S | warmup (203 l., 14 scalaires) | boot override, veil, respawn dedans → `WarmupController` + 2 callbacks | M | boot |
| Q2-10 | | Q2 | moyenne | S | `makeInteractionContext` 12 closures | 5 actions de 20-40 l. → méthodes nommées / contrôleur | S | nul |
| Q2-11 | | Q2 | moyenne | S | `NpcCombatController::update` 258, `NpcDirector::update` 270 | cible ×2 branches, healer string → `resolveCombatTarget`, `CombatStyle`, phases nommées | M | A/B |
| Q2-12 | | Q2 | moyenne | S | `FollowerController` 39 méthodes / 3 TU ; 13 lambdas identiques | → table événement → méthode, `FollowerView` | M | plomberie |
| Q2-13 | | Q2 | moyenne | S | `SceneEditor::draw` 323 | spawn draft ×3 → `placeAt`, `spawnDraft`, palette du registre | S-M | UI dev |
| Q2-14 | | Q2 | moyenne | S | `MapController::open` 152, `cookTerrainMaterials` 227 | → `extentFor`, `ChannelPlan[4]` | S / M | `.mtex` à hasher |
| Q2-21 | ◐ | Q2 | moyenne | G | `UiRouter` strings ×42, `wait1/4/8`, `Options` steppers, `Settings` non réfléchi | → table `{model, event, handler}`, `Stepper[]`, `REFLECT_FIELD` | M | `.rml` |
| Q2-22 | ◐ | Q2 | moyenne | G | hotkeys modales ×5, 14 noms d'écrans, F5/F9 hors `ActionMap` | → `ScreenHotkey[]`, `InputAction::QuickSave/Load`, `kDevHotkeys[]` | S | navigation |
| Q2-23 | | Q2 | moyenne | G | `hostile`/`guard` par littéraux de faction | `FactionStanding` existe → `standing() == Enemy`, trait de faction | S | A/B |
| Q2-24 | | Q2 | moyenne | G | `findByEditorId` en dur ×20+, 14 GUIDs arène, `EasternMenace` câblé | → `GameplayLinksForm`, `DungeonKitForm`, kind `"turnin"` (modèle `startWorldspace`) | S | aucun |
| Q2-25 | | Q2 | moyenne | G | colliders par plages d'index de variante | → colonne `Collider` dans la table des variantes | S | `bodyCount` |
| Q2-26 | | Q2 | moyenne | G | `EditorScene` type-id en chaînes ×26 ; nom de type ×15 | → colonnes de `kCategories`, tables keyées `TypeInfo::id` | S-M | UI dev |
| Q2-27 | | Q2 | moyenne | G | 3 graph panels ~250 l. dupliquées | → `GraphGrammar` + `applyCanvasActions` | M | UI dev |
| Q2-28 | ◐ | Q2 | moyenne | G | `restoreMode` contourné ×7 ; `MEADOWS_BOOT` vs menu | → `restoreMode` exclusif, `BootMode` + `beginPlay` | S | save 3 modes |
| Q2-34 | | Q2 | moyenne | D | assemblage GAS : `StatBlock` ×11, self-cast ×6, tag ×12, frappe ×3… | → `statBlockOf`, `tryActivateSelf`, `hasStateTag`, `resolveStrikeBetween`, `sweepActors` | S | sans effet |
| Q2-35 | | Q2 | moyenne | D | dispatch `visit` ×2 dans `PropertyGrid` | → `drawValueWidget()` | S | nul |
| Q2-36 | | Q2 | moyenne | D | FX : spawn ×24, stop ×10, sweep ×3, seed ×3, handler arène | → `EmitterHandles`, `spawnFormFx`, `ReachEmitterSet`, `cosmeticSeed` | S | nul |
| Q2-37 | | Q2 | moyenne | D | 3 lectures ×3 clôture, classe dominante → string | → `showReading`, `readGround/Fire` headless, `dominant() → enum` | S | nul |
| Q2-38 | | Q2 | moyenne | D | 2 streamers de collision copiés, job raster ×2 | `ChunkStreamer` → `engine/core` (T-6), `AsyncMapRaster` | S-M | `bodyCount` |
| Q2-39 | | Q2 | moyenne | D | bundle acteur ×4 listes divergentes ; `SaveGame` chaîne par type ; `InventoryView` sonde 4 types | → `attachActorKit`, parent `ItemForm` (Q4) | S | — |
| Q2-40 | | Q2 | moyenne | D | transfert items ×3, `chargeGold` ×3, `%santé` ×3, `attr()` par frame ×12, HUD↔Router via la scène | → `moveItems`, `chargeGold`, `healthPercent`, `UiFrame` | S | nul |
| Q2-46 | | Q2 | moyenne | H | ~110 commentaires-journal, 34 « idiom » sans helper, 3 libellés ImGui tagués | politique 2026-07-22 | S | sans effet |
| Q2-47 | | Q2 | moyenne | H | en-têtes lourds (`LandscapeScene.hpp` 76 includes, `NpcDirector.hpp` RHI) | forward-decl, `NpcGpu`, `.twb` hors streamer | S | sans effet |
| Q2-15 | | Q2 | basse | S | `TreeCreationScene` → `LandscapeScene` ; `EditorScene` pile propre en overlay | → closure « back », `EditorContext` partagé | S / M | — |
| Q2-29 | | Q2 | basse | G | fade `i32`, `brushKind` i32, `castSpirit` renifle le Lua | → enums, champs `AbilityForm` | S | nul |
| Q2-41 | | Q2 | basse | D | petites duplications (a-p) | | S | nul |
| Q2-48 | | Q2 | basse | H | scories (sections vides mesurées, `(void)`, indentations, shim) | | S | sans effet |
| Q2-49 | | Q2 | basse | H | bancs : tables de noms d'enums recopiées, triade ×5, mutations directes | tables côté sim (T-4), marquer `// bench` | S | nul |
| Q2-50 | | Q2 | basse | P | `refreshNpcs` O(n²), scan PNJ par entité, `attr()` par frame | | S | sans effet |
| Q3-1 | ◐ | Q3 | haute | S | `VulkanDevice.cpp` | 4 913 l. en un TU, déjà découpé par bandeaux → `VulkanInternal.hpp` + 6 TU, interface inchangée | L | n/a (`vksmoke`) |
| Q3-2 | ◐ | Q3 | haute | S | `WorldRenderer::render` | 591 l. : key-shadow, LightsUbo, auto-skip réflexion = trois décisions pures inline → fonctions libres / `FrameComposer` | M | nul |
| Q3-3 | | Q3 | haute | S | `WaterSystem::updateSim` | sim en 7 booléens + `simEpoch`, invalidation copiée ×3, 440 l. → `SimPhase` + `simTransition()` seul écrivain | M | nul (replay) |
| Q3-4 | ◐ | Q3 | haute | S | `scatterGrass` (`GrassSystem.cpp`) | 395 l., six phases nommées en commentaires → `CornerLattice`, `ClumpPicker`, `scatterCell` ; hash à poser avant | M | nul |
| Q3-5 | | Q3 | haute | S | `TerrainNoise`, `bakedEntrySources`, `buildLocalGeometry` | logique headless compilée dans `render/` (43 TU, hydrologie pure, mesher pur) → `engine/terrain` | M | nul (hash verts) |
| Q3-6 | ◐ | Q3 | moyenne | S | `VulkanDevice::create` | 571 l., familles de files énumérées ×4 → `pickQueueFamilies()` + phases nommées | M | n/a |
| Q3-7 | ◐ | Q3 | moyenne | S | `VulkanDevice::createTexture` | ~330 l., 4 chemins d'upload, séquence immédiate ×2 → `planImageUpload()` + exécuteurs (T-12) | M | n/a |
| Q3-8 | ◐ | Q3 | moyenne | S | chaîne SSDM (`WorldRenderer`) | 22 membres, 180 l., seul effet d'écran hors `PostFx` → `SsdmChain` | M | A/B `ssdmMode` |
| Q3-9 | ◐ | Q3 | moyenne | S | `RadianceCascades::update` | 357 l., snap copié de `prepare`, worker 120 l. en lambda → `snapOrigins`, `bakeTile` statique, `record*` | M | nul |
| Q3-10 | ◐ | Q3 | moyenne | S | `FrameComposer` | deux lanes patchées après lui, portée base/resolved en commentaires → `FrameComposerInputs` + colonne | S | nul |
| Q3-11 | | Q3 | moyenne | S | `TerrainSystem::buildMaterialArrays` | 176 l., trois chemins, albédo hors helper → `MaterialSet` en `Unique` | M | nul |
| Q3-12 | | Q3 | moyenne | S | `VegetationSystem` | une classe, sept métiers, ~60 membres → `VariantMeshSet`, `PropMaterials`, `ReseedPipeline` (après Q3-19/20) | M | nul |
| Q3-13 | | Q3 | moyenne | S | `Engine` ↔ `SpriteRenderer` | la racine `engine/` possède un objet de `render/` = le cycle de couches → `cfg.sprites` ou pile de scènes — **décision dev** | M | nul |
| Q3-14 | | Q3 | moyenne | S | API de `WorldRenderer` | `terrainParams()` mutable ×72, `waterSystem()` ×17, queues nues ×9, 2 friends → knobs hissés, `request*`, `WaterSimControl`, `TerrainParams` par `RenderView` — **décision dev** | L | nul |
| Q3-15 | ◐ | Q3 | haute | G | `ShaderLibrary::Watch` | 12 `Watch` / 15 compteurs / 7 pipelines sans rien / 1 watch vide → `WatchedPipeline::ensure()`, 59 pipelines (T-14) | M | nul |
| Q3-16 | ◐ | Q3 | haute | G | `FrameUniforms.hpp` ↔ `common.glsl` ↔ `FrameComposer` | contrat des 81 lanes écrit 3× et dérivant ; constantes CPU↔GLSL par commentaire ×9 → table X-macro générant struct + GLSL (T-15) | L | oui (static_assert) |
| Q3-17 | ◐ | Q3 | haute | G | `TextureFormat` (`Rhi.hpp`, GL, Vulkan) | 9 tables dans 3 fichiers, R16F = 4 vs 2, validation éclatée → `kFormatTraits[]` + `validateTextureDesc` (T-4) | M | n/a (`vksmoke`) |
| Q3-18 | ◐ | Q3 | haute | G | `RendererConfig` / `WorldRenderer` | 80 `cfg.*` + 24 `caps()`, 4 verbes = 4 listes des mêmes 10 systèmes → `Mounted` + table ordonnée sans `virtual` | M | nul |
| Q3-19 | ◐ | Q3 | haute | G | `scatterProps` | 5 copies d'un squelette + table `PlantTier` pour 2 familles → `ScatterRule[]` (colonnes = `ScatterPropForm`, T-11) | M | golden scatter |
| Q3-20 | ◐ | Q3 | haute | G | `kFirst*` (végétation) | 7 familles = plages d'entiers en 12 sites, cailloux sans ombre par seuil 0.35 → `PropFamilyTraits[]` (T-4) | M | nul |
| Q3-21 | ◐ | Q3 | haute | G | `SplatTextures.hpp`, `terrain_zones.glsl`, `terrain.frag`, `familyColor` | table des 25 couches écrite 3×, 4e copie divergente → `kSplatLayers[]` + bloc GLSL unique | M | A/B (placeholder corrigé) |
| Q3-22 | ◐ | Q3 | moyenne | G | encodage des props | drapeaux en bits de signe définis dans 3 C++, décodés dans 4 shaders → `PropEncoding.hpp` + `prop_encoding.glsl` | M | golden scatter |
| Q3-23 | ◐ | Q3 | moyenne | G | `BakeMailbox`, `ChunkStreamer` | réécrits à la main (`WaterSystem` ×4, `ChunkOcclusion`, RC) → adoptés partout, descente dans `engine/core` (T-6) | M | nul |
| Q3-24 | ◐ | Q3 | moyenne | G | cartes cuites en anneau ×4 | `kSpan`, `center`, `strayed`, `drain` recopiés → `RingBakedMap<Baked>` | M | nul |
| Q3-25 | | Q3 | moyenne | G | lanes de file Vulkan | compute/upload = 2 jeux de 9 champs, blocs ×2-3 → `QueueLane` ×2 ; topologie inchangée | M | n/a (sync validation) |
| Q3-26 | | Q3 | moyenne | G | `kBakedDischargeBoost`, « sobriety pass » | verdict dev « débit ×3 » en `constexpr`, 3 branches GLSL mortes sans knob → lanes `WaterSimParams` A/B | S | A/B |
| Q3-27 | | Q3 | moyenne | G | `ssdmMode`, `debugMode`, `debugBuffer`, `waterDebug` | vocabulaires en `i32` testés en 4-8 sites → enums ; `writeFrameLanes()` par système | S | nul |
| Q3-28 | | Q3 | moyenne | G | `groundFinalAt` (`WaterSystem`, `HeightField`) | « sol définitif ? » écrit 2× avec genre de monde en dur, null-safety divergente → une définition (T-10) | S | nul |
| Q3-29 | ◐ | Q3 | basse | G | presets de pipeline | `Greater` ×16, caster ×6 identiques, `{ "FrameUbo", 0 }` ×44, slots 3-9 ×2 → `rhi::DepthState` presets + bindings nommés | S | nul |
| Q3-30 | ◐ | Q3 | basse | G | biais d'espèce, bandes de LOD, `appendWood`, `kLeafShapes` | quatre petites tables manquantes (switch, 4 `if`, 6 ternaires, if-chain) | S | nul |
| Q3-31 | | Q3 | basse | G | registres de handles GL/Vulkan | 19 maps, `destroyX` find/erase ×14, mode d'échec asymétrique → `HandleTable` + politique unique (pas U2-01/02) | S | n/a |
| Q3-32 | | Q3 | basse | G | palette ciel, albédos proxy | données de look en C++ (22 `Vec3`), albédos ×2 → `kMaterialProxyAlbedo[]`, décision T-11 | M | nul |
| Q3-33 | ◐ | Q3 | haute | D | `rhi::Unique` (U3-7) | dix classes en `destroy*` manuel (107), listes `destroy()` qui dérivent → `Unique` partout, `UniqueShader`, `*this = {}` | M | nul |
| Q3-34 | ◐ | Q3 | haute | D | `PostFx` | cinq copies de la famille cible ½-res, 70 handles à plat → `HalfResTarget`, `TemporalHistory`, `MediaPass` | M | A/B |
| Q3-35 | | Q3 | haute | D | shaders copiés | `shadow_prop` = `tree.vert`, `skinned.frag` = `mesh.frag`, `windowBeam`, bruits, CSM, luma, billboards → includes-corps | S | A/B |
| Q3-36 | ◐ | Q3 | moyenne | D | `farTerrain`/`farHorizon` | même code ×5 verbes, `28800 : 9000` dérivés à la main ×4 → `array<FarTerrain, 2>` + `Config` | S | nul |
| Q3-37 | | Q3 | moyenne | D | `remapBindings` (Vulkan) | parseur GLSL legacy exécuté à chaque compile → retrait (~190 l.) | S | n/a |
| Q3-38 | | Q3 | moyenne | D | niveaux de variante, chemins de dessin | sync vs async divergents, LOD/groupe/AABB ×3, 2e règle de LOD dans `drawDepth` → `buildVariantLevels`, `casterLevel` | M | nul |
| Q3-39 | ◐ | Q3 | moyenne | D | échantillonneurs, grille | bilinéaires ×3, box blur, mapping texel ×3, clé de chunk ×5 (U3-4 à moitié), `kChunkSize` ×11 → `GridView`, `maskTexelOf`, en-tête de grille en core (T-7) | S | hash terrain (`maskTexelOf`) |
| Q3-40 | | Q3 | moyenne | D | `SpriteRenderer`, `SceneView.hpp` | seul shader embarqué en C++, `#ifdef VULKAN` à la main ; `Sprite` tire `Device.hpp` dans `game/` → `sprite.*`, `Sprite.hpp` | M | nul |
| Q3-41 | ◐ | Q3 | moyenne | D | `AtmosphereParams` | 3e écriture des champs météo (24, pas 19) → `FieldLane` (T-1) | M | nul |
| Q3-42 | | Q3 | basse | D | helpers copiés (12) | bind groups 3-9, mark/sweep ×4, sun-linked ×3, mips ×4, palettes, `kTau` ×11, `angleAt`, `bindWaterPass` ×5, FNV `targetKey` | S | nul |
| Q3-43 | ◐ | Q3 | haute | B | `WaterSystem::buildPipeline` | `beginWatch()` dans le `if` → 5 shaders d'eau jamais rechargés, GL sur programme supprimé | S | dev only |
| Q3-44 | ◐ | Q3 | moyenne | B | SSDM ×6, `rc_probe` | aucun hot-reload | S (via Q3-15) | dev only |
| Q3-45 | ◐ | Q3 | basse | B | `shadow.glsl:46` | `1.0 / 2048.0` vs knob 1024/4096 → `textureSize()` | S | **change le look** → A/B |
| Q3-46 | ◐ | Q3 | basse | B | `familyColor` | couches 17-24 peintes en sable sans `.mtex` | S (via Q3-21) | placeholder |
| Q3-47 | ◐ | Q3 | basse | B | `ShaderLibrary::load` | re-appel retire le programme sans bump ; `FarTerrain` contourne | S | dev only |
| Q3-48 | ◐ | Q3 | basse | B | `timestampReady`, `copyTexture` GL/VK | trois contrats divergents (2 latents) → contrat écrit + alignement | S | nul |
| Q3-49 | | Q3 | moyenne | H | commentaires hors politique | ~195 dans `landscape/` + WorldRenderer, 8 RHI, 6 racine ; 8 périmés | S | nul |
| Q3-50 | | Q3 | basse | H | divers | `getenv` GI, littéraux composer, stubs GL41 spam, includes copiés, BOM, ternaire mort, `retired` sur délai | S | nul |
| Q4-1 | ◐ | Q4 | moyenne | S | `stageDungeonRecords` (`DungeonRecords.cpp`) | 397 l., 12 paramètres, chaque record écrit deux fois (struct + 27 `stage.set`) → inputs + 8 fonctions + `stage.record(form)` | M | export octet à octet |
| Q4-2 | | Q4 | moyenne | S | bundle acteur (Spawner / scène / save) | 9 GAS cohérents ; 6 non-GAS attachés par la scène, save en connaît 10 → `attachActorKit` ou pont `reflectedComponent` (T-13) | S | tests save |
| Q4-3 | | Q4 | moyenne | S | `packFireCells` (`SpiritFire.cpp`) | grille sérialisée en texte dans un `str`, contre la convention du fichier → child-records ou blob réfléchi (T-5) — **décision** | M | save |
| Q4-4 | | Q4 | moyenne | S | `ActiveEffect`, `ScriptVars` | non réfléchi recopié ×2 sens ; « serialization deferred » contre §2.8 → `REFLECT_BEGIN` + kind conteneur ou child-records — **décision** | M | save |
| Q4-5 | ◐ | Q4 | moyenne | S | `Followers.hpp` | catalogue 38 fonctions pures, 14 TU → cinq en-têtes par domaine | M | nul |
| Q4-6 | | Q4 | basse | S | `captureActor`, `WorldForms.hpp` | six blocs child-rows ; 17 Forms de six domaines dans un en-tête → `captureRows<Row>`, cinq en-têtes | S | nul |
| Q4-7 | ◐ | Q4 | haute | G | ~25 vocabulaires fermés (19 `str` + 6 `i32`) | 5 `parse*` + 26 `== "…"`, chacun écrit 3×, défauts silencieux (`Poison`, `Add`, `Neutral`) → **T-3** : (B) `FieldMeta`/`EnumTable` puis (A) kind Enum — **décision dev §2.3** | M | (B) nul / (A) recook |
| Q4-8 | ◐ | Q4 | haute | G | `kClauseEvaluators` + `conditionSummary` + `Keywords.cpp` | trois listes des 8 kinds → `kClauseKinds[] { name, eval, summary, fields }` (ferme Q2-42) | M | nul |
| Q4-9 | ◐ | Q4 | haute | G | `DamageType` (5 fichiers) | sept chaînes de traits, type du coup décidé 2× différemment → `kDamageTypes[12]` + `dominantDamageType` (T-4) | M | armes multi-canaux |
| Q4-10 | ◐ | Q4 | haute | G | `spellSupported` + `executeSpell` + `SpellsTest` | matrice des sorts écrite 3× → `kSpellMatrix[] { …, Mode }` dans `world/spirit` (= Q2-16, une brique) | M | nul |
| Q4-11 | ◐ | Q4 | moyenne | G | `tickBuildup` (`StatusBuildup.cpp`) | `if` par type dans la boucle sur `kRows` → colonnes `OnTrigger`, `DotSource`, `dotRate`… | M | nul |
| Q4-12 | | Q4 | moyenne | G | `Injuries.cpp` | 7 switches + 3 `EffectForm` en C++ → table C++ ou child-records (T-11) — **décision** | M | (b) `base.toml` |
| Q4-13 | ◐ | Q4 | moyenne | G | 5 compte-à-rebours (`Damage.cpp`) | timer + tag posés en 2 endroits → `kTimedStates[]` + `grantTimedState()` ; migration GAS = décision | S | nul |
| Q4-14 | ◐ | Q4 | moyenne | G | cœur GAS | vitals ×2, `attr("energy")` spécial, 125 `attr`, 34 `"State.*"` sans constantes → `kVitals[3]`, `namespace tags` | M | nul |
| Q4-15 | ◐ | Q4 | moyenne | G | `EquipmentStats` sondes ×4 | items sans base commune alors que `isA` existe → `ItemForm` parent | M | nul |
| Q4-16 | ◐ | Q4 | moyenne | G | `PackageKind`, `CombatStyle`, événements | quatre vocabulaires sans enum dans `gameplay/`, événements hashés sans registre → enums + `knownKinds()` + `cooker validate` (T-12) | S | nul |
| Q4-17 | ◐ | Q4 | moyenne | G | `GroundClass` (`SpiritFire`) | cinq classes de sol en dur vs table data (huit) → `fuelFromWeights` indexé par `classIndex` (avec Q3-21) | S | nul |
| Q4-18 | ◐ | Q4 | moyenne | G | `FieldFlags` | index enfant sur le littéral `"parent"` (index privé des quêtes), champs morts par prose → `ParentLink`, `Deprecated` | S | nul |
| Q4-19 | ◐ | Q4 | basse | G | petits vocabulaires | `EditOp` taguée, `followerStance` f32, `followerCategory` ×2, blend 3 bools, `Perception.state` i32, `editorId` dupliqué sans lint | S | nul |
| Q4-20 | ◐ | Q4 | moyenne | G | `SavedStatsForm`, défauts « MUST match », `kCoreFields` | miroirs tenus par commentaire → test de couverture puis génération (T-1) — **décision** | M | nul |
| Q4-21 | ◐ | Q4 | haute | D | deux `Stager`, `copyFields` ×2 | 14 `findField("…")->id` nus dans le dépôt → `data::RecordStager` assertif (T-9, ferme Q1-38) ; H-b ré-ouvert avec preuve | S | nul |
| Q4-22 | | Q4 | moyenne | D | trois A* (`world/ai`) | même squelette → `ai::astar<Node>` dans `engine/nav` (T-18) | M | nul (ordre conservé) |
| Q4-23 | | Q4 | moyenne | D | triade ×7, pré-scan decay verbatim ×2, `foldMul` ×5, has/add tag ×13 | → `kResonanceTriad`, `collectExpiringDecays`, `StatModifiers::multiply/add`, `hasTag/addTag` | S | nul |
| Q4-24 | ◐ | Q4 | basse | D | 6 résolveurs de singleton | → `data::resolveSingleton<T>` | S | nul |
| Q4-25 | | Q4 | basse | D | petits doublons (15) | `enterState`, disque 4 m ×4, balistique ×3, `riverAt` 11e copie, π/τ ×7, `SelfScope`, clé XZ ×13, scan U8-1 ×2, matcher ×3… | S | nul |
| Q4-26 | ◐ | Q4 | basse | B | `restoreActiveEffect` | ids appariés primaire/`attribute2` perdus au chargement (prouvé, latent) | S | save compat |
| Q4-27 | ◐ | Q4 | basse | B | `convertValue` | troncature i32/u32 silencieuse | S | nul |
| Q4-28 | ◐ | Q4 | basse | B | `readPluginBinary` | `baseDir` jamais posé (latent) | S | nul |
| Q4-29 | ◐ | Q4 | basse | B | `nav::Navigator` | contrat tenu par 1 adaptateur sur 3 (latent) | S | nul |
| Q4-30 | ◐ | Q4 | basse | B | `addItem` | `count <= 0` accepté (latent) | S | nul |
| Q4-31 | | Q4 | basse | H | commentaires, données mortes | ~60 écarts ; `SpiritForm.triad/channel/fieldKind` sans lecteur ; 7 « HOW TO FILL » à trancher | S | nul |
| Q4-32 | | Q4 | basse | H | divers | `[cpp-tuning]` manquants, `editorId[96]`, borne 64, `static_cast` sans assert, `tick` sans `stopped` | S | nul |
| Q5-1 | | Q5 | moyenne | S | cycles de couches | `Engine` possède `SpriteRenderer` (= Q3-13) ; `WorldRenderer → UiSystem` ; `GltfMesh.hpp → anim` — **décision dev** | M | nul |
| Q5-2 | ◐ | Q5 | moyenne | S | `populateAnchors` (`DungeonBake.cpp`) | 212 l., décision de 60 l. inline → `pickBarrierCell`, trois passes | M | même ordre RNG |
| Q5-3 | ◐ | Q5 | haute | G | `reflect::FieldKind` / `FieldInfo` | pas d'Enum, pas de `FieldMeta`, `isA` existe → (B) `FieldMeta`/`EnumTable` puis (A) kind Enum — **décision dev §2.3 (T-3)** | M | (B) nul / (A) recook |
| Q5-4 | ◐ | Q5 | haute | G | sept vecteurs d'ancres (`DungeonBake.hpp`) | chaque consommateur parcourt famille par famille → `AnchorKind` + `kAnchorEmit[]` (T-4, avec Q4-1) | M | bit-identique par seed |
| Q5-5 | | Q5 | moyenne | G | `ecs::World::reflectedComponent` | 22 enregistrements, 0 appelant ; les saves recopient par type → consommer (T-13) ou retirer | S | save texte |
| Q5-6 | ◐ | Q5 | moyenne | G | mise en scène du donjon, `EdgeKind` | ~15 constantes de code, traits en six sites → `PopulateParams`/`TorchParams` (T-11), `kEdgeKinds[]` | S | nul |
| Q5-7 | | Q5 | basse | G | blend 3 bools, bus string, `formatCode` ×2, `rmlKeyFor` sans garde | → enums + tables `static_assert` (T-3/T-4) | S | nul |
| Q5-8 | | Q5 | basse | G | `layers_audit.py` | non branché, rang de `world` faux → CTest avec baseline des cycles (T-17) | S | nul |
| Q5-9 | | Q5 | basse | G | `GlContext.cpp:46` vsync | en dur, sans knob ni autre appelant → `EngineConfig::vsync` | S | nul |
| Q5-10 | ◐ | Q5 | moyenne | D | `core::fnv1a` u32 seul | 13 sites u64 à basis tronqué dans 12 fichiers → `fnv1a64`, `Fnv64`, `hashReflected` (T-2) | S | terrain → bump groupé |
| Q5-11 | | Q5 | moyenne | D | `GltfMesh.cpp` | boucle de sommets ×2, nœuds ×2, AABB ×2, double parse, sRGB ×3 → `readPrimitive`, `loadGltfRig`, `core::srgbToLinear` | S | nul |
| Q5-12 | | Q5 | moyenne | D | règles de marche bake / navigateur | snap, pas, pied, flood écrits 2× → sur `NavGrid` + constantes nommées | S | nul |
| Q5-13 | ◐ | Q5 | moyenne | D | `engine/ui` | 22 `destroy*` manuels (Q3-33 → 12 classes / 129 appels), shader dev-UI embarqué → `Unique*`, `imgui.vert/.frag` | S | nul |
| Q5-14 | | Q5 | basse | D | plomberie Jolt, 4 formats maison, racines/viewport ×2 | → `addBody`, `toJph`, T-5 (12 formats), `resolveInRoots` | S | nul |
| Q5-15 | ◐ | Q5 | basse | B | `phys::BodyId = 0` | sentinelle = corps Jolt valide après 256 réutilisations du slot 0 (latent, prouvé) | S | interne |
| Q5-16 | ◐ | Q5 | basse | B | `Window::width` vs `Input::mousePosition` | pixels vs points logiques, divergent sur HiDPI (latent) | S | nul sur Windows |
| Q5-17 | ◐ | Q5 | basse | B | `ImGuiLayer::textureGroups` | jamais purgé : un bind group fuit par texture recréée | S | nul |
| Q5-18 | | Q5 | basse | H | divers | `JobCounter`/`wait` sans client, `Log::init` non idempotent, `valueFromString` laxiste, `tick` sans plafond, seuils en dur | S | nul |
| Q5-19 | | Q5 | basse | H | commentaires, CLAUDE.md §4, README §0.2 | 9 écarts ; « GL only backend » et « only common/ » périmés ; U2-07/U8-5 à clore | S | nul |

---

## 4. Bugs trouvés au passage (classe B)

Listés à part pour être corrigeables indépendamment des refontes. Aucun n'a été
corrigé par la revue elle-même ; le chantier R0 (§6.4) a ensuite traité les
quinze bugs sans risque listés en §0.1 (2026-10-24), dans l'arbre de travail, et
les dix restants ont suivi le même jour (§0.1, seconde ligne du 2026-10-24) avec
un seul bump `kTileBakeVersion` 121 → 122 et le cache v122 livré. Les 25 sont
donc traités, non commités.

| id | vague | symbole (fichier) | effet | correction | bit-exact |
|---|---|---|---|---|---|
| **Q1-37** | Q1 | `paramsKey` (`ZonePlan.cpp:49-106`) | fingerprint du memo des zones incomplet (plages massif/coast/moisture des archétypes, la plupart des params monde) → cellules périmées en jeu après un rebake depuis le pupitre | fingerprint = hash complet, ou clé calculée une fois par bake | oui |
| **Q1-38** | Q1 | `Stager::set` (`MapRecords.cpp:39`) | `findField(field)->id` sans test null : une faute de frappe = crash à l'export de carte (même motif `DungeonRecords.cpp:103`) | assert + `RecordStager` partagé | n/a |
| Q1-39 | Q1 | `underLake` (`TileBake.cpp:1908`) | une copie sur neuf du test de lac sans le `+ 0.5f` : cascade et pools jugent différemment près des rives | `Lake::covers()` | non → bump |
| Q1-40 | Q1 | `biomeIdAt` (`TerrainGen.cpp:689`) | zone-lift sans le terme corridor : l'id de biome ne lit pas le plancher qu'`at()` interpole | `zoneFloorLift()` unique | non → bump |
| Q1-41 | Q1 | 9 boucles FNV-1a | offset basis `1469598103934665603` (chiffre manquant) ; inoffensif comme clé | `core::Fnv64` | non → bump (ou garder documentée) |
| Q1-42 | Q1 | usage `cooker/Main.cpp` | `tilesPerSide=6` vs 2, flags `carrier*` disparus, `argc <= 9` rejette la combinaison complète de `bake-map` | usage généré depuis la table | sans effet |
| Q1-43 | Q1 | `RiverForm.tier` → `WaterBodiesBuilder` | exporté, jamais relu (`RiverSurface` sans tier ; l'effet survit via `flowSpeed`) | consommer ou retirer | n/a |
| **Q2-42** | Q2 | `Keywords.cpp:32` vs `Condition.cpp:76-83` | l'éditeur ne propose que 5 kinds de condition sur 8 : `FollowerAffinityAtLeast`, `FollowerActive`, `FollowerConvalescent` inaccessibles depuis le PropertyGrid, pas de branche dans `ConditionBuilder` | source unique (Q2-19) ; court terme : 3 kinds + test de cohérence | sans effet |
| **Q2-44** | Q2 | `WorldDemoScene.cpp:45-50` | 6 `register*FormTypes` sur 21 : les démos 2D ignorent tout record Visual/Anim/Audio/Ui/Spirit/… de `base.toml`/`combat.toml` (un `CueForm` n'est pas vu alors que l'arène émet des cues) — dérive U8-3 rouverte | `registerAllFormTypes` | records jusqu'ici ignorés deviennent visibles |
| Q2-43 | Q2 | `followNpcPath` (`NpcMovement.cpp:113-119`) | `before` porte déjà l'`airHeight` de la frame précédente ; pas refusé par `groundNpc` → repart de `before` + ré-ajoute `airHeight` : un PNJ projeté contre un rebord monte par frame (rare, latent) | grounder sans l'offset, ou l'ajouter après un grounding réussi | sans effet |
| Q2-45 | Q2 | `vksmoke/Main.cpp:152, 627, 668, 693` | quatre `check(true, …)` : PASS inconditionnel, la couche de validation n'est jamais comptée | `check` lit le compteur du device, ou `note()` | harnais |
| **Q3-43** | Q3 | `WaterSystem::buildPipeline` (`WaterSystem.cpp:1735-1738`) | `shaders.beginWatch()` est **dans** le `if (pipeline.id != 0)` : la watch est vide depuis la création, « an EMPTY watch never fires » → les 5 pipelines d'eau ne réagissent **jamais** à une édition de `water_surface.glsl` ; sur GL le programme recompilé est `glDeleteProgram` 0,5 s plus tard sous la pipeline vivante | sortir `beginWatch()` du `if` (1 ligne), puis Q3-15 | dev only |
| **Q3-44** | Q3 | 6 pipelines SSDM (`WorldRenderer.cpp:625-638`), `probePipeline` (`RadianceCascades.cpp:175-178`) | aucun compteur ni watch (bâtis au resize / une fois) ; même effet GL après édition | Q3-15 (`WatchedPipeline`) | dev only |
| Q3-45 | Q3 | `shadow.glsl:46` | `float texel = 1.0 / 2048.0;` alors que la résolution d'ombre est un knob live 1024/2048/4096 : PCF 3×3 à 2 texels (4096) ou ½ texel (1024), marge idem | `1.0 / float(textureSize(uShadowMap, 0).x)` ou lane (Q3-16) | **change le look** hors 2048 → A/B |
| Q3-46 | Q3 | `familyColor` (`TerrainSystem.cpp`) | placeholder sans `.mtex` : falaise 17-18, roche 19, neige 20, dépôts 21-24 peints en sable (`else → Sand`) | `familyOf(layer)` de Q3-21 | placeholder |
| Q3-47 | Q3 | `ShaderLibrary::load` (`ShaderLibrary.cpp:160-172`) | ré-appel sur un nom existant retire l'ancien programme **sans bump de génération** → un consommateur bâti le perd 0,5 s plus tard (GL) ; `FarTerrain.cpp:30-39` contourne | `load()` idempotent ou assert si bindings différents | dev only |
| Q3-48 | Q3 | `timestampReady` GL/VK, `copyTexture` GL (`GL_TEXTURE_2D` forcé), `copyTexture` VK (mip 0 transitionné, image entière trackée) | trois contrats divergents, deux latents (textures array / storage à mips) | contrat écrit dans `Device.hpp`/`CommandBuffer.hpp`, backends alignés | nul aujourd'hui |

| **Q4-26** | Q4 | `restoreActiveEffect` (`SaveState.cpp:82`) vs `applyEffect` (`GameplayEffects.cpp:277, 312`) | **prouvé** : la ligne `attribute2` partage l'`effectId` de la primaire, `SavedEffectForm` ne le porte pas, le restore alloue `nextEffectId++` par ligne → après chargement `removeEffectById` n'en retire qu'une ; latent (seul détenteur d'id runtime, effet sans `attribute2`) | `SavedEffectForm.effectId` (défaut 0 = id frais) + remap par acteur ; `ActiveEffect` réfléchi | save compatible |
| Q4-27 | Q4 | `convertValue` (`PluginLoader.cpp:39-47`) | `I32` ← `static_cast` d'un `i64` sans test, `U32` sans borne haute : une valeur de mod hors plage devient négative **sans warning** | `std::in_range` sinon `nullopt` | nul |
| Q4-28 | Q4 | `readPluginBinary` (`BinaryFormat.cpp:220-297`) | `Plugin::baseDir` jamais posé (seul `cooker uncook` consomme aujourd'hui) | `readPluginBinaryFile(path)` ou documenter | nul |
| Q4-29 | Q4 | `nav::Navigator` (`Nav.hpp:26-31`) | `acceptanceRadius` + endpoints exacts tenus par `TerrainNavigator` seul ; `InteriorNavigator`, `GridNavigator` divergent | contrat écrit + alignement (ou `astar` partagé Q4-22) | nul |
| Q4-30 | Q4 | `addItem`/`removeItem` (`Inventory.cpp:12-20`) | `count <= 0` crée ou décrémente une pile ; deux appelants se protègent eux-mêmes | assert ou early-return + contrat | nul |
| Q5-15 | Q5 | `phys::BodyId` (`Physics.hpp:21`) | sentinelle `0` = un corps Jolt valide quand le slot 0 a été réutilisé 256 fois (séquence `uint8` partant de 1) ; streaming de cellules → un corps pris pour un échec, jamais retiré | `raw + 1` ou `~0ull`, décodé dans `toJph` | interne |
| Q5-16 | Q5 | `Window::width()` (`Window.cpp:91-93`) vs `Input::mousePosition()` (`Input.cpp:175-176`) | pixels après `PIXEL_SIZE_CHANGED` vs points logiques : picking décalé ×2 sur HiDPI (macOS) | documenter l'unité, `pixelDensity()` ou conversion dans `Input::update` | nul sur Windows |
| Q5-17 | Q5 | `ImGuiLayer::groupFor` (`ImGuiLayer.cpp:~190-200`) | un bind group par id de texture, jamais purgé ; `MiniMapPanel` recrée ses textures → fuite bornée | LRU borné, `forgetTexture(id)` ou purge par frame | nul |

Hors périmètre de la série : `tests/VegetationScatterTest.cpp:18, 107`, `TileBakeTest.cpp:286`,
`MapWorldFixture.hpp:215` reprennent la constante FNV tronquée de Q1-41 (13 sites dans 12
fichiers au total, Q5-10). Notes hors classe B : `ResonanceDecays` non réfléchi ni sauvé
(« transient » par en-tête) → un chargement coupe net les décroissances en cours — décision de
design à confirmer (Q4-2).

---

## 5. Généralisations transverses (toutes vagues)

Les mécanismes dont plusieurs vagues ont besoin ; chaque ligne nomme le mécanisme
existant qu'elle étend. Rempli au fil des vagues.

| id | mécanisme existant → extension | vagues concernées |
|---|---|---|
| T-1 | `FieldLane` (`game/scenes/RenderTuningIo.cpp`) → descend dans `engine/reflect` : table compile-checkée Form ↔ params parcourue dans les deux sens (apply, capture, hash) | Q1 (pupitre, builders), Q2 (panneaux), Q4 (`LandscapeForms` miroirs) |
| T-2 | `hashTerrainGenTuning` (hash par réflexion) → `core::fnv1a64` + `Fnv64` + `reflect::hashReflected` (~40 l. : `core::fnv1a` n'existe qu'en u32 ; 13 sites u64 à basis tronqué dans 12 fichiers, seule `MapGrid.cpp:16` juste) ; les hash manuels disparaissent | Q1, Q3 (`VulkanDevice`), Q5 (`VertexAoCache`), tests |
| T-3 | `reflect::FieldKind` sans kind **Enum** ni `FieldMeta` → cause racine des ~25 vocabulaires en strings (Q4-7 : 19 `str` + 6 `i32`, 5 parseurs, 26 `== "…"`, chacun écrit 3×), des ids entiers (Q1-14), de `FieldLane` (Q1-22/Q2-32), de la table de lanes GPU (Q3-16), de `Keywords.cpp` (Q2-42). **Instruit par Q4-7 (data) et Q5-3 (reflect)** : (B) `FieldMeta`/`EnumTable` optionnelle sur `FieldInfo` — zéro changement disque, loader valide, grille → combo, compatible H-a ; (A) `FieldKind::Enum` appendu — binaire = ordinal, TOML = nom, un corps par consommateur `visit`. Recommandation des deux lots : **(B) d'abord**. `TypeInfo::isA` existe déjà (Q4-15). **Décision dev §2.3/§5** | Q1, Q2, Q3, Q4, Q5 |
| T-4 | tables de traits avec `static_assert(== Count)` (`poiTypeName`, `MapEdgeStyle`) → le patron pour tout enum à traits (PoiType, BorderStyle, tiers, DamageType, StatusType, SpellVerb… ; en Q3 : `kFormatTraits[]` pour `TextureFormat`, `PropFamilyTraits[]`, `kSplatLayers[]`, `ScatterRule[]`) | Q1, Q2, Q3, Q4 |
| T-5 | `BinaryFormat.cpp` (codec réfléchi — **c'est** déjà le `BinaryArchive` en tout sauf le nom) / `writeTrgFile` → un `core::BinaryArchive` symétrique pour les **12** formats maison (8 terrain + WSD + saves + `.cmesh` + `.mtex` + `.ao` + `.nvg`) ; le reader `.nvg` borné est le modèle ; absorbe aussi `packFireCells` texte (Q4-3) et `ScriptVars` (Q4-4) via un kind conteneur réfléchi — **décision** | Q1, Q4, Q5 |
| T-6 | `core::JobSystem` → `forEachIndexed(jobs, n, fn, onLanded)` et exécuteur injecté ; plus de `std::async` ni de boucles « enqueue ou inline » ; **Q3 confirme** : `ChunkStreamer.hpp` et `BakeMailbox.hpp` n'incluent que `engine/core` et descendent tels quels dans `core` (sans `forEachIndexed`), puis sont adoptés par `WaterSystem` ×4, `ChunkOcclusion`, `RadianceCascades` ; `RingBakedMap<Baked>` par-dessus pour les quatre cartes en anneau | Q1, Q2 (streamer), Q3 |
| T-7 | `GridOps.hpp`/`Noise.hpp` (toolkit « bit-identical replacements ») → `GridView`, `cellRange`, `sfbm/warp` ; absorbe les échantillonneurs de `engine/render/landscape` (`HeightField::Level` **est** un `GridView`, `gridHeight`, `boxBlurAxis`, `maskTexelOf` ×3) et l'en-tête de grille (`chunkKey`, `kTerrainChunkSize`) | Q1, Q3 |
| T-8 | `game::registerAllFormTypes` (source unique U8-3) → `game::loadGameStack` (bootstrap unique, gate de langue incluse) | Q1 (cooker), Q2 (scènes) |
| T-9 | `data::RecordStager` unique dans `data/plugins/` (`ensure`, `set` assertif, `stageAll(form)` par réflexion, `live`) → remplace les deux stagers maison identiques (`MapRecords`, `DungeonRecords`) ; **14** `findField("…")->id` nus dans le dépôt (`SaveGame` ×9, `TerrainSculptTool` ×3, `SaveState`, les stagers ; `LevelEditor::fieldId` est le null-safe) ; `reflect::copyFields` public (2 copies identiques, H-b ré-ouvert) | Q1, Q2, Q4 |
| T-10 | le patron `*Controller` + `make*Context()` (15 contextes, 99 appels, « rebuilt per call ») → s'applique aux trois sous-systèmes nés après juillet : `SpiritContext`, `SandboxContext`, `WarmupContext` ; `StreamingContext` porte `WorldKind` + `WorldPolicy` ; côté render, `TerrainParams::groundFinalAt()` remplace les deux tests `sandbox && base->regionAt` (Q3-28) | Q2, Q3 |
| T-11 | `FurnitureForm.screen`, `startWorldspace` (GUID + fallback editorId déprécié + warn), `StatsTuningForm` (U4-7) → le patron « la donnée porte le cas particulier » pour mobilier (`interaction`, `restHours`), liens de gameplay (`GameplayLinksForm`), kits (`DungeonKitForm`, `ArenaBenchForm`), props de scatter (`ScatterPropForm`) | Q2, Q4 |
| T-12 | `decidePlayerAction` / `chooseCombatMove` / `resolveMeleeStrike` (décision pure côté `gameplay::`, exécution côté `game/`) → `decideNpcActivity`, `CombatStyle` dans `chooseCombatMove`, `PackageKind` + exécuteurs | Q2, Q4 |
| T-13 | `gameplay::attachActorKit(Entity)` : une seule liste du bundle de composants acteur, appelée par le Spawner (§2.7), les bancs 2D et le save. **Précisé par Q4** : le 9-set GAS est cohérent (Spawner = `tickCharacter` ×3) ; la divergence porte sur les **6 composants non-GAS** attachés par la scène (`finalizeActorSpawn`) et connus du save ; et le pont `ecs::World::reflectedComponent` (Q5-5, « drives generic component serialization for saves ») a 22 enregistrements et **0 appelant** — le consommer supprime la liste elle-même | Q2, Q4, Q5 |
| T-14 | `ShaderLibrary::Watch` (dérivé de l'usage) + `rhi::UniquePipeline` → `render::WatchedPipeline::ensure()` : premier build inclus, assert sur watch vide et imbrication, bracket privé ; remplace 12 watches à la main, 15 compteurs de génération et 7 pipelines sans mécanisme (59 pipelines, ferme Q3-43/44) | Q3 |
| T-15 | le verrou `static_assert` du FrameUbo (U3-3) + l'injection de `#define` par `ShaderLibrary` → **table de lanes X-macro** générant la struct C++, les offsets et le bloc GLSL (+ préambule de `constexpr` partagés : clusters, lumières, espèces, matériaux d'eau, résolution d'ombre) ; le `FieldLane` de T-1 côté GPU ; ferme la classe « one mid-struct insertion desyncs ~14 shaders » et les constantes « must match » | Q3 |
| T-16 | la frontière sim/rendu dans `engine/` : `engine/terrain` (headless) accueille `TerrainNoise`, `entrySourcesFromBodies`, `WaterGeometry` ; `RenderView` (déjà `meshCache`) porte `TerrainParams` au lieu de `terrainParams()` mutable ; `WaterSimControl` façade ; `cfg.sprites` sur `WorldRenderer` + `RenderView::inPassUi` callback + `GltfSkeleton` dans `anim` pour fermer les **trois** cycles de couches (`engine/(root) ↔ render`, `render → ui`, `assets → anim` : tous localisés en Q5-1) — préface de RENDERER-EXTRACT (`RENDERING.md` §6), pendant du nettoyage de namespace de Q1 ; **décision dev** | Q1, Q3, Q5 |
| T-17 | `tools/scripts/layers_audit.py` (non branché, rang de `world` faux) → CTest avec une **baseline des cycles tolérés** : un nouveau cycle fait échouer la suite, un cycle résolu retire sa ligne | Q5 |
| T-18 | `nav::Navigator` (interface) → `ai::astar<Node>(start, isGoal, forEachNeighbor, heuristic, budget)` header-only dans `engine/nav/`, les trois A* de `world/ai` deviennent des adaptateurs, le contrat (acceptation, endpoints) est honoré par construction (Q4-29) | Q4 |
| T-19 | `kClauseEvaluators` / `kRows` + `static_assert` (les tables modèles) → une **table de noms par enum** `EnumNames<E>` consommée par parseur, validateur (`effectWarnings`, `cooker validate`), éditeur (`Keywords.cpp` disparaît) ; devient `FieldMeta.enumeration` si T-3 (B) est acté | Q2, Q4, Q5 |

Outillage noté pendant Q1 : le wrapper `rtk grep` du hook a renvoyé des résultats vides
sur plusieurs traces qu'un `/usr/bin/grep` direct trouvait (signalé par un lot ; non
reproduit sur un motif simple). Les traces de la série sont refaites en grep natif en cas
de doute.

---

## 6. Vision globale (série complète, 2026-10-23) — à lire avant toute correction

### 6.1 La photo

| Vague | Lignes | Findings | S | G | D | B | H | P | État de santé (verdict du rapport) |
|---|---|---|---|---|---|---|---|---|---|
| Q1 terrain | ~24,6 k | 48 | 9 | 10 | 17 | 7 | 4 | 1 | code le plus chaud : passes inline de 1 900 l., traits en chaînes, chaque param défini 3-5×, helpers copiés ×9-10 |
| Q2 game/tools | ~38 k | 50 | 15 | 14 | 12 | 4 | 4 | 1 | la racine et les contrôleurs de juillet sont propres ; la scène a regonflé par trois sous-systèmes jamais extraits ; vocabulaires en strings |
| Q3 render/rhi | ~37,6 k | 50 | 14 | 18 | 10 | 6 | 2 | 0 | unité bien tenue (§2.1, Phase 5) mais **chaque bon mécanisme adopté à moitié** ; le renderer héberge la vérité du monde ; `VulkanDevice.cpp` un TU |
| Q4 simulation | ~22 k | 32 | 6 | 14 | 5 | 5 | 2 | 0 | **l'actif durable est sain** (`data/` et `actors/combat` les plus propres de la série) ; une cause racine (T-3) et ses ~25 symptômes ; la persistance contourne la réflexion |
| Q5 reste d'engine | ~10,5 k | 19 | 2 | 7 | 5 | 3 | 2 | 0 | socle propre ; porte les trois décisions transverses (réflexion, hash, pont ECS) ; cycles de couches localisés à trois inclusions |
| **Total** | **~133 k** | **199** | **46** | **63** | **49** | **25** | **14** | **2** | |

Lecture : le code le plus sain est celui qui a été **conçu** (data, GAS core, actors/combat,
dungeon, core/reflect, les façades pimpl) ; le code le plus endetté est celui qui a été
**accumulé vite par chantiers successifs** (TileBake, LandscapeScene, WaterSystem,
WorldRenderer, VulkanDevice). La dette n'est pas du spaghetti au sens d'un contrôle de flux
illisible : ce sont (1) des **tables écrites à moitié** dont les colonnes manquantes sont des
`if` ailleurs, (2) des **mécanismes partagés qui existent et ne sont pas consommés** (`Unique`,
`Watch`, `BakeMailbox`, `reflectedComponent`, `kRows`, `kClauseEvaluators`, `BinaryFormat`),
(3) une **cause racine unique** dans la réflexion, et (4) deux **frontières mal placées**
(la vérité du monde dans le renderer ; la scène propriétaire de trois sous-systèmes).

### 6.2 Les sept décisions qui vous reviennent (elles conditionnent ~60 % des findings)

| # | Décision | Instruite dans | Débloque |
|---|---|---|---|
| **D1** | **T-3 — la réflexion** : (B) `FieldMeta`/`EnumTable` optionnelle sur `FieldInfo` (zéro effet disque, recommandée d'abord), puis (A) `FieldKind::Enum` si nécessaire | Q4-7, Q5-3 | Q1-14, Q1-22, Q2-19, Q2-32, Q2-42, Q3-16, Q3-27, Q4-7…Q4-19, Q5-7 — **la plus rentable de la série** |
| **D2** | **T-16 — la frontière sim/rendu** : `TerrainNoise`, `WaterGeometry`, `entrySourcesFromBodies` dans `engine/terrain` (mécanique) ; puis `TerrainParams` par `RenderView`, `WaterSimControl`, `cfg.sprites`/`inPassUi` (trois cycles de couches) | Q3-5, Q3-13, Q3-14, Q5-1 | préface de RENDERER-EXTRACT ; ferme `layers_audit` |
| **D3** | **T-1 / T-15 — la définition unique d'un champ** : `FieldLane` descend dans `engine/reflect` (Form ↔ params, apply/capture/hash/défauts/sliders) ; sa version GPU = table de lanes FrameUbo générant struct + GLSL | Q1-22, Q2-32, Q3-16, Q4-20 | 155×2 apply/capture terrain, 190 sliders, 81 lanes ×3, `SavedStatsForm` 55 champs, `AtmosphereParams` |
| **D4** | **T-13 — le bundle acteur** : `attachActorKit` **ou** consommer le pont `reflectedComponent` (save générique sans liste) | Q4-2, Q5-5, Q2-39 | trois listes divergentes, bancs 2D, `SaveState` par type |
| **D5** | **T-5 — les formats maison** : promouvoir `BinaryFormat` en `core::BinaryArchive` (12 formats) ; kind conteneur réfléchi pour `ScriptVars`/cellules de feu ou child-records | Q1-24, Q4-3, Q4-4, Q5-14 | §2.3 « never with ad-hoc per-type serialization » redevient vrai |
| **D6** | **T-11 — la donnée porte le cas particulier** : `Injuries` en table C++ ou child-records ; `ScatterPropForm`, `DungeonThemeForm`, palette ciel, mobilier `interaction/restHours`, `GameplayLinksForm` | Q4-12, Q3-19, Q5-6, Q3-32, Q2-17, Q2-24 | moddabilité ; vide les `if` par type dans quatre vagues |
| **D7** | **Le bump de cache unique terrain** (Q1 lot 4) regroupant les corrections non bit-exactes (Q1-23/39/40/41/45/34 + `Fnv64`), **avec cache livré** ; et deux réglages dev à remettre sous A/B avant verdict (`kBakedDischargeBoost` Q3-26, « sobriety pass ») | Q1, Q3-26, Q5-10 | règle « pas de bump sans cache » ; règle « jamais un réglage dev perdu » |

Trois arbitrages de juillet sont **ré-ouverts avec preuve** (H-b, U8-1, U3-4/U3-7 à moitié) et
deux sont **résorbés de fait** (U2-07/U8-5 ImGui, U2-04 ShaderDesc) : §0.2.

### 6.3 Les 25 bugs : deux à corriger tout de suite, le reste latent

À corriger sans attendre l'arbitrage (une ligne chacun, zéro risque) : **Q3-43** (`beginWatch()`
dans le `if` : les shaders d'eau ne se rechargent jamais, GL sur programme supprimé) et
**Q1-38** (`findField()->id` nu dans `MapRecords` : une faute de frappe = crash à l'export de
carte). Visibles en jeu : Q1-37 (memo des zones périmé après rebake), Q1-39/40 (lac et biome
divergents près des rives → bump), Q2-42 (3 kinds de condition inaccessibles), Q2-44 (démos 2D
aveugles aux cues), Q3-45 (PCF faux hors 2048, change le look → A/B), Q3-46 (placeholder sable).
Latents prouvés : Q2-43 (PNJ qui monte contre un rebord), Q3-44/47/48 (hot-reload et contrats
RHI), Q4-26 (ids d'effets appariés au chargement), Q4-27/28/29/30, Q5-15 (sentinelle
`BodyId = 0` après 256 réutilisations), Q5-16 (HiDPI), Q5-17 (fuite ImGui bornée). Harnais :
Q2-45 (`vksmoke check(true)`), Q1-41 (FNV tronquée ×13, inoffensive comme clé).

### 6.4 L'ordre des chantiers de correction proposé

Un commit par brique ; chaque brique nomme son finding. Vérification par zone : **terrain** =
`cooker landscape-report` + hash `MapBakerTest` (bit-exact) ou bump groupé D7 ; **render** =
build Release + capture A/B pour tout pixel + `vksmoke` pour `rhi/` ; **sim** = `-tse=slow`
Debug avant commit, suite Release avant push ; **game** = smoke `MEADOWS_BOOT=story|sandbox` +
validation en jeu.

| Chantier | Contenu | Préalable |
|---|---|---|
| **R0 — bugs sans risque** | Q3-43, Q1-38 (via T-9), Q2-42 court terme, Q2-44, Q2-43, Q3-47, Q3-48, Q4-27/28/29/30, Q5-15, Q5-17, Q2-45, Q1-42 | aucun |
| **R1 — le socle** (`core`, `reflect`, `data`) | `fnv1a64`/`Fnv64`/`hashReflected` (T-2, Q5-10) ; `kPi/kTau`, `packXZ`, `GridView`/`cellRange`/`maskTexelOf` (T-7, Q1-26, Q3-39) ; descente `BakeMailbox`/`ChunkStreamer` en `core` (T-6, Q3-23) ; `reflect::copyFields` public + `data::RecordStager` (T-9, Q4-21) ; `FieldFlags::ParentLink/Deprecated` (Q4-18) ; `resolveSingleton` (Q4-24) ; `BinaryArchive` (T-5, D5) ; **puis la décision D1 (B)** : `FieldMeta`/`EnumTable` + `EnumNames<E>` (T-19) | aucun — c'est ce que tout le reste attend |
| **R2 — la plomberie render** | `Unique` dans les 12 classes (Q3-33, Q5-13) → `WatchedPipeline` 59 pipelines (Q3-15, ferme Q3-44) → `BakeMailbox` dans `WaterSystem`/`ChunkOcclusion`/RC + `RingBakedMap` (Q3-23/24) → presets de pipeline (Q3-29) → `FormatTraits` + validation partagée (Q3-17) | R1 (descente core) |
| **R3 — les tables** (T-4, bit-exact, toutes vagues) | terrain : `PoiTypeTraits`, `kBorderTraits`, `TierProfile` (Q1-10…) ; render : `kSplatLayers` (Q3-21 + Q3-46), `PropFamilyTraits` + `PropEncoding` + `ScatterRule` (Q3-19/20/22, un golden recapturé), `kEdgeKinds`, `AnchorKind` (Q5-4/6) ; sim : `kDamageTypes` (Q4-9), colonnes de `kRows` (Q4-11), `kVitals` + `tags::` (Q4-14), `kTimedStates` (Q4-13), `ItemForm` (Q4-15), `kSpellMatrix` (Q4-10 = Q2-16), `kClauseKinds` (Q4-8), `PackageKind`/`CombatStyle`/registre d'événements (Q4-16), `GroundClass` (Q4-17) ; game : mobilier, factions, UI (Q2-17/18/21/22/23) | D1 pour les tables de noms ; sinon indépendant |
| **R4 — les extractions structurelles** | terrain : `extractMapHydrology` en passes (Q1-1), `finalizeWater`/`bakeTileStage1` (Q1-2), outils (Q1-3) ; game : `restoreMode` exclusif + `WorldKind`/`WorldPolicy` (Q2-28/20), `WarmupController` → `SandboxWorldController` → esprits par élément (Q2-9/3/2), `update()` en séquence (Q2-1) ; render : `Mounted` + table de systèmes (Q3-18), trois décisions pures hors de `render()` (Q3-2), `SsdmChain` (Q3-8), anneaux (Q3-36), `PostFx` familles (Q3-34), RC `update` (Q3-9), `scatterGrass` (Q3-4), Vulkan en 7 TU + `QueueLane` (Q3-1/6/7/25) ; sim : `stageDungeonRecords` (Q4-1), `populateAnchors` (Q5-2), `Followers` en cinq en-têtes (Q4-5) | R2 pour render ; R1 pour terrain (hash) |
| **R5 — la définition unique d'un champ** (D3) | `FieldLane` dans `engine/reflect` (Q1-22, Q2-32, Q4-20, Q3-41) → table de lanes FrameUbo + préambule de constantes GLSL (Q3-16, ferme Q3-45) → `SavedStatsForm` généré, test de couverture du save (Q4-20) | D1 (B) actée |
| **R6 — les frontières** (D2, D4) | `TerrainNoise`/`WaterGeometry`/`entrySourcesFromBodies` → `engine/terrain` (Q3-5) ; `SimPhase` + `simTransition` (Q3-3) ; `groundFinalAt` (Q3-28) ; `attachActorKit` ou pont `reflectedComponent` (Q4-2, Q5-5) ; `ActiveEffect` réfléchi + `effectId` (Q4-4, Q4-26) ; puis `WaterSimControl`, `TerrainParams` par `RenderView`, `cfg.sprites`/`inPassUi`/`GltfSkeleton` (Q3-14, Q5-1), `layers_audit` en CTest avec baseline (T-17, Q5-8) | R4 render ; décisions D2/D4 |
| **R7 — hygiène continue** | ~300 commentaires hors politique par fichier touché (Q1-46, Q2-46, Q3-49, Q4-31, Q5-19), `[cpp-tuning]` manquants, données mortes (`SpiritForm.triad/channel/fieldKind`, `bleedBurstDamage`, 4 lanes FrameUbo), CLAUDE.md §4 périmé ×2 (« GL only backend », « only common/ »), README §0.2 clôtures | au fil de l'eau, jamais un commit d'hygiène seul |

Les trois premiers chantiers (R0-R2) ne demandent **aucune décision** et ne changent **aucun
pixel ni aucun résultat de bake** ; ils rendent les suivants mécaniques. D1 est la décision à
prendre en premier parce que R3 et R5 en dépendent ; D2 et D4 peuvent attendre R4.

## 7. Reports — à reprendre dans un chantier dédié

Ce que les corrections R0-R4 ont **volontairement laissé** parce que la décision, la donnée
ou le chantier qui le porte n'existe pas encore. Chaque ligne nomme le chantier qui doit le
reprendre ; la table est à relire à l'ouverture de ce chantier, puis la ligne passe en ✅.

| # | Report | Origine | Chantier qui le reprend | Ce qu'il faut faire |
|---|---|---|---|---|
| P-1 | **Hostile / garde par faction** (Q2-18) : le spawner dérive `hostile`/`guard` de `tag == "Faction.Bandits"` / `"Faction.VillageGuard"` | R3 sim | **VIVANT / factions** (CLAUDE.md §6.1) | créer les `FactionRelationForm` dans les données + une faction joueur (tag) ; `FactionRelations::build` n'est aujourd'hui appelé que par les tests ; puis le spawner lit `standing(player, npc)` |
| P-2 | **Deux fils d'événements muets** (lint `cooker validate`) : `DialogueNodeForm` émet `OnAcceptQuest` que personne n'écoute ; `QuestTaskForm` « Defeat bandits » attend `OnBanditDeath` que personne n'émet | R3 sim | **QUÊTES** (données `game/data/*.toml`) | soit un auditeur/émetteur côté données (`events::` connus : `OnDeath` + `filterTag`), soit retirer les deux lignes ; la tâche de quête est probablement **injouable** aujourd'hui |
| P-3 | **Hotkeys modales** (Q2-23) : copiées ×5 avec `top->name == "map"` ; F5/F9 hors `ActionMap` | R3 game | **R4-D game** (extraction `LandscapeScene`) ou chantier UI | table `{ InputAction, écrans, Play-only, ouverture }` sur `ScreenStack`/`UiScreenForm` ; F5/F9 dans `ActionMap` |
| P-4 | **`DetailProfile`/`kLeafShapes`** de `SpaceColonizationTree` (Q3-30 partiel) | R3 render | **arbres** (chantier végétation / PAYSAGE Z5) | la table des profils de détail et des formes de feuille reste en `if` par espèce |
| P-5 | **`step(0.35)` de `shadow_prop.vert`** : prouvé sans effet, conservé | R3 render | **R7 hygiène** au prochain passage sur le shader | retirer avec capture A/B de l'ombre des props |
| P-6 | **`RingBakedMap`** réduit à `BakeMailbox::strayed()` (Q3-24) : les quatre cartes en anneau gardent `kSize/kSpan/center` et leur `drain` propre | R2-C | **RENDERER-EXTRACT** (`RENDERING.md` §6) | n'unifier que si une cinquième carte apparaît : les quatre diffèrent réellement (format, span, source) |
| P-7 | **`PendingSpiritAction::Mode`** conservé à côté de `SpellEffect` (pas 1:1) ; `Mode::EarthWall` est un énumérateur mort | R3 sim | **ESPRITS reprise** (Foudre d'abord) + **R4-D** (esprits hors de la scène) | fusionner `Mode` dans `SpellEffect` quand `applyPendingSpiritActions` descend dans `SpiritDirector` ; supprimer `EarthWall` |
| P-8 | **`GameplayAbility::canAfford`** garde son cas spécial énergie (politique différente de `kVitals`) | R3 sim | **GAS** (prochain chantier combat/stats) | décider si le coût d'énergie suit `kVitals[].spendPausesRegen` ou garde sa règle |
| P-9 | **`buildupStatusModifiers`** reste ligne par statut | R3 sim | **STATS** (`docs/STATS.md`) | une colonne `modifier` sur `kRows` quand les modificateurs par statut seront tous du même type |
| P-10 | **`kFirst*`** (plages d'ordinaux de variantes de props) gardés pour les consommateurs de `LandscapeScene.cpp` ; le drapeau conifère de `FarTerrain` reste un littéral | R3 render | **R4-D** (quand `LandscapeScene` est découpée) | remplacer par `familyOf()`/`kFamilies` ; `FarTerrain` lit `PropEncoding` |
| P-11 | **`GridView`** (Q1-27) non fait ; `FieldKind::Enum` option (A) non fait (inutile tant que (B) suffit) | R1 | **R5 FieldLane** / jamais | (A) seulement si un consommateur a besoin d'un enum typé côté disque |
| P-12 | **Vocabulaires ouverts** laissés en strings : `FurnitureForm.category/screen`, `MarkerForm.kind`, `SpiritRuleForm.verb`, `damageType` des Forms | R1/R3 | **D6 — la donnée porte le cas particulier** | fermer le vocabulaire (table `Vocabularies.hpp`) ou l'assumer ouvert et documenter dans `userdoc/` |
| P-13 | **11 formats binaires maison** hors `BinaryFormat` (Q1-24, Q4-3, Q5-14) | R1-B | **D5** | porter sur `core::ByteWriter/ByteReader` + `reflect::writeValue/readValue` ; un bump de version de cache par format |
| P-14 | **Frontière sim/rendu** (D2 : `TerrainNoise`, `WaterGeometry`, `entrySourcesFromBodies`, `TerrainParams` par `RenderView`, `WaterSimControl`) et **bundle acteur** (D4 : `attachActorKit` ou pont `reflectedComponent`) | série Q | **R6 — les frontières** (décisions dev D2/D4) | voir §6.2 ; préface de RENDERER-EXTRACT |
| P-15 | **Réglages dev à remettre sous A/B** : `kBakedDischargeBoost = 3.0f` (Q3-26) invisible du panneau Sim ; « sobriety pass » (`milkGate = 0`, `foam = 0` sous `#ifdef WATER_SIM`) sans knob | Q3 | **EAU** (verdicts en attente, `docs/PAYSAGE.md`) | exposer dans `SimConfig`/`WaterSimParams` et le panneau ; jamais un réglage dev perdu |
| P-16 | **`layers_audit.py`** pas branché en CTest ; sa table de rangs met `world` sous `gameplay` (faux-positif) ; CLAUDE.md §4 périmé (« GL seul backend », « only common/ ») | Q5 | **R6 / R7** | baseline + CTest ; corriger les rangs ; mettre CLAUDE.md §4 à jour |
| P-17 | **Asymétrie live/export des records de donjon** (Q4-1) : chaque stage pose un sous-ensemble explicite de champs (`WorldspaceForm` 4/12, `StaticForm` 5/20, `LightForm` 5/11…) ; `setAll` les élargirait et réécrirait, au re-Accept, des champs retouchés à la main | R4-B | **DONJON** (prochaine reprise `DungeonThemeForm`) | décider par record si `setAll` remplace la liste explicite (un échange d une ligne par stage, maintenant isolé) ; sinon documenter la liste comme contrat |
| P-18 | **`QueueLane` Vulkan** (Q3-25) non fait : la timeline de la lane compute est PARTAGÉE avec le submit graphique (`timeline`/`timelineNext`, `gfxDoneValue` passé à l upload) alors que l upload possède la sienne ; une struct commune toucherait la plomberie wait/signal | R4-C | **VULKAN** (prochain passage backend, avec `vksmoke` + `MEADOWS_VK_SYNC_VALIDATION=1` dans la boucle) | introduire `QueueLane` lane par lane, upload d abord (indépendante), build à chaque pas ; les blocs « begin recording ×3 / wait slot ×2 / pool+timeline+cbs ×2 » sont désormais chacun dans une fonction (`recordUpload`, `asyncComputeCmd`, `beginFrame`, `createComputeLane`/`createUploadLane`) |
| P-19 | **Aucune remise à zéro feu/FX sur `onExit`** (constat R4-D4a) : `spirit clear` et le changement de carte remettent le feu, les émetteurs et le vent à zéro, pas la sortie de scène — props en feu d un monde mort, `fireLoop`, émetteurs de rivière survivent à un rechargement F9 ; les deux listes de reset existantes diffèrent aussi (`finishSpiritEarth(true/false)`, `spiritLine`, `releaseSpiritRocks`) | R4-D4a | **ESPRITS reprise** (ou R4-D4b si le dev tranche) | décider si `onExit` appelle `resetWorldFx()` + le reset des gestes (comportement nouveau, à valider en jeu : F9 depuis un feu actif) ; unifier les deux listes en un `resetSpirits(const Reason&)` |
| P-20 | **11 knobs matière du terrain** (`splatBlendDepth`, `terrainTintStrength`, `splatDetailFade`, `pomDistance`, `splatVariety`, `pomShadowStrength`, `pomDepth`, `surfAoStrength`, `wetDarken`, `wetSheen`, `snowSheen`) recopiés à la main dans `saveRenderTuning` : leurs sliders vivent dans `LandscapeScene`, aucune plage réfléchie | R5 | **R6** ou prochain passage `LandscapeScene` | table de lanes `scène ↔ LandscapeTuningForm` + `drawLanes` ; les sliders quittent la scène pour `RenderTuningPanels` |
| P-21 | **`hashTerrainGenTuning`** (`game/TerrainGenTuning.cpp`) n a plus d appelant : `mapBakeKey` lit `hashKey(TileBakeParams)` qui le couvre | R5 | **R7 hygiène** | supprimer avec son test, ou le garder comme clé « Form seule » si un outil en a besoin |
