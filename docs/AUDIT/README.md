# Audit Meadows — Synthèse

> Point d'entrée unique de l'audit multi-unités (U1–U9 + deep-dive §2.9).
> Rapports détaillés par unité dans ce même dossier. **Aucun code modifié** :
> ce document agrège et priorise ; le dev arbitre.

---

## 0. Avancement de la remédiation (2026-07-07)

Bricks livrées (Batch 1 « gains rapides », cadence brique-par-brique, chacune
buildée + 279 tests verts) :

| Finding | Fix | Commit |
|---------|-----|--------|
| U1-09 / U2-09 | Suppression de l'arbre mort `_old/renderer_test/` (164 Mo) | `c9d97ca` |
| U6-F8 | Retrait du footgun public `setCurrentValue` | `0b86235` |
| U1-01 / U7-3 | `static_assert` verrouillant l'ordinal `FieldKind`↔`Value`↔`KindOf` | `25b0480` |
| U7-1 | `HeightPatch`/`HeightPatches` sortis de `render/` → `engine/terrain/` (§2.10) | `16c7079` |
| U1-05 / U3-2 | `hashU32`/`HashRng` hoistés dans `core/Hash.hpp` (7 fichiers dédupliqués) | `9fe48a6` |
| U8-2 | Fuite `flecs` hors des headers `gameplay/` (→ `ecs::Entity`) | `20f3e12` |
| U2-05 / U6-F9 / U5-7 / U2-08 | Label d'erreur shader + commentaires périmés | `1b88290` |
| U6-F1 §2.9 | **Option A** : « execution calculations » sanctionnées dans CLAUDE.md §2.9 + marqueurs sur les sites (aucun changement de comportement) | `a51fae7` |
| STATS (bug) | DoT ignition/électrocution réduits par **vitality** (était `will` — bug copier-coller vs STATS.md §183) | `f98b6dc` |
| STATS (bug) | **Bleed** = criticalSensitivity% de maxHealth, **ignore l'armure** (était un forfait slash mitigé par l'armure) | `4d6a71a` |

**Reporté (décision dev requise, PAS un oubli) :**
- **U6-F1 §2.9 — RÉSOLU (Option A, `a51fae7`)** avec le dev : les écritures de tick
  (DoT, regen, mort par buildup) sont des « execution calculations » sanctionnées
  (CLAUDE.md §2.9 amendé). La vérification a confirmé que l'Option B (router le DoT
  via `applyDamage`) aurait été une **régression** (double-mitigation) — la résistance
  agit sur le seuil de buildup, pas sur le dégât par tick — et a révélé en prime deux
  bugs d'équilibrage, corrigés (`f98b6dc`, `4d6a71a`, cf. table §0).
- **U8-3 (source unique d'enregistrement des Form-types)** — nécessite une lib
  d'agrégation ; l'auteur l'a explicitement reportée (« not worth one yet », commentaire
  `cooker/Main.cpp`). Changement de structure/build à valider par le dev.
- **Batch 2 (H-a/H-b, mutualisation reflect) et Batch 3 (structurel)** — haute valeur
  mais touchent la sérialisation/les saves et l'architecture ; à faire en session
  supervisée (CLAUDE.md §10).

> Note build : le suivi de dépendances de headers de `cmake-build-debug` est bien
> configuré (`CMAKE_CXX_CL_SHOWINCLUDES_PREFIX` détecté en français). Le « ninja: no
> work to do » observé après édition de headers venait d'un décalage de page de code
> du shell `vcvars` utilisé pour l'audit, PAS d'un défaut du projet — les builds CLion
> sont a priori corrects. Aucune modif CMake n'a donc été faite.

---

## 1. Résumé exécutif

Le codebase est **fondamentalement sain et bien stratifié**. Les invariants
porteurs de CLAUDE.md sont vérifiés en place et en bonne santé :

- **Réflexion (keystone §2.3)** : unique mécanisme de (dé)sérialisation, testé
  en profondeur — aucun code de sérialisation ad-hoc par type.
- **Modèle données/§5** : patch field-level, last-writer-wins, save = couche
  de patches — implémenté correctement et couvert par des tests profonds.
- **Abstraction RHI (§2.1)** : zéro appel GL hors backend ; interface
  Vulkan-shaped prête pour un 2e backend.
- **Suite headless (§2.10)** : 76 TUs, déterminisme discipliné, systèmes
  mandatés (resolver, save-layering, GAS) couverts en profondeur.

La dette est **concentrée en deux foyers**, pas diffuse :

1. **`LandscapeScene` god-object** (5914 l., ~12 responsabilités) qui **érode
   le seam extract/submit Phase-5** que tout le reste du codebase protège.
2. **Duplication à la frontière de réflexion** : le même switch/clone/diff
   `reflect::Value` réapparaît partout où la donnée traverse le seam
   `reflect::Value` (sérialisation binaire/TOML, éditeurs, save, script).

Aucune violation de correction, de save, ou de moddabilité. Les corrections
d'invariant réelles sont peu nombreuses et cadrées (§5 ci-dessous).

---

## 2. Tableau de bord des invariants (§2)

| Invariant | Verdict | Exception (fichier:ligne) |
|-----------|---------|---------------------------|
| §2.1 RHI (aucun GL hors backend) | **PASS** | ImGui via `imgui_impl_opengl3` = exception dev-UI assumée (§3) — `engine/ui/ImGuiLayer.cpp:4` |
| §2.2 Forms vs References | **PASS** | References = `ReferenceForm` ; place/move/disable = patches (U7) |
| §2.3 Réflexion keystone | **PASS** | 28+ types réfléchis, zéro sérialisation ad-hoc |
| §2.4 Une couche patches (mods+saves) | **PASS** | `resolve()` = load order plat, save appended last (U7) |
| §2.5 Identité par GUID | **PASS** | `FormHandle` = index runtime ; identité persistante = `core::Guid` (U7) |
| §2.7 Spawner par catégorie | **PASS** | dispatch + wiring 100 % réflexion, agrégés (`Spawner.cpp:264`) |
| §2.8 État script en composant réfléchi | **PASS** | `ScriptVars` réfléchi ; VM unique, `self`=proxy (U8) |
| §2.9 Attributs via GameplayEffect | **PASS-with-exception → MEDIUM** | Pipeline test-locké ; déviations réelles = sites tick périodique (regen/DoT/mort-buildup) : `CharacterTick.cpp:74-78,119` + `GameTime.cpp:22-27,49,89-90`. **`Damage.cpp:81` et `Spawner.cpp:103` NE SONT PAS des violations** (exec-calc / init seeding — voir DEEP-attribute-mutation.md) |
| §2.10 Pureté headless — `data/` | **PASS** | zéro include rhi/render |
| §2.10 Pureté headless — `world/` | **PASS-with-exception** | 1 letter-violation : `world/terrain/TerrainPatches.hpp:8` inclut `engine/render/landscape/TerrainNoise.hpp` (symboles importés headless-purs) |
| §2.10 Pureté headless — link-level | **NON VERROUILLÉ** | aucune cible de link prouvant sim-sans-render (`tests/CMakeLists.txt:78`, U9-3) |
| §3.1 Headers platform-clean | **PASS** | aucun `windows.h`/X11/Wayland/SDL/glad dans un `.hpp` ; natifs derrière pimpl (U1) |
| Façades seam pimpl (Jolt/miniaudio/RmlUi) | **PASS** | aucun type tiers ne traverse `Physics.hpp`/`Audio.hpp`/`UiSystem.hpp` ; deps PRIVATE (U8) |
| flecs confiné à `meadows-ecs` | **FAIL partiel** | `flecs.h`+`flecs::entity` fuient dans `gameplay/save/SaveState.hpp:3` et `gameplay/actors/CharacterTick.hpp` (U8-2) |

---

## 3. Table maîtresse des findings

Triée par sévérité (crit→high→med→low) puis effort (S d'abord) — les gains
rapides remontent dans chaque bande. Sévérité §2.9 : **F1 rétrogradé HIGH→MEDIUM**
par le deep-dive.

| id | unité | sév | axe | fichier:ligne | description | effort | inter |
|----|-------|-----|-----|---------------|-------------|--------|-------|
| U4-1 | U4 | crit | archi/factor | LandscapeScene.hpp:77 / .cpp:87-5914 | God-object : 1 classe agrège ~12 responsabilités | L | non |
| U1-01 | U1 | high | archi/qual | reflect/Reflect.hpp:39-77 | Triple ordinal FieldKind/Value/KindOf = contrat binaire, sans static_assert | S | oui |
| U3-2 | U3 | high | réutil | TerrainNoise.cpp:10 (+5 fichiers) | murmur3 `hashU32` copié dans 6 fichiers | S | oui |
| U7-1 | U7 | high | archi | world/terrain/TerrainPatches.hpp:8 | `world/` inclut `engine/render/` (letter-violation §2.10) | S | oui |
| U4-3 | U4 | high | factor | LandscapeScene.cpp:87-707 | `onEnter` = 620 l. séquentielles, ré-entrantes | M | non |
| U4-5 | U4 | high | archi/factor | LandscapeScene.cpp:1592-1863 | Éditeur+sculpt embarqués dans la scène de jeu (double EditorScene) | M | oui |
| U5-2 | U5 | high | factor | game/TextureCache.* ; MeshCache.* | Machinerie async-residency dupliquée entre 2 caches | M | non |
| U7-2 | U7 | high | factor | BinaryFormat.cpp:45 ; TomlWriter.cpp:25 ; PluginLoader.cpp | Switch per-FieldKind répliqué (H-a) | M | oui |
| U8-3 | U8 | high | factor | tools/cooker/Main.cpp:189-205 | Liste `registerXxxFormTypes` recopiée à la main (déjà cause d'un bug) | M | oui |
| U3-1 | U3 | high | factor | GrassSystem.cpp:244 ; TerrainSystem.cpp:223 ; VegetationSystem.cpp:305 | Ring chunk-streaming implémenté 3× | L | non |
| U4-2 | U4 | high | archi | LandscapeScene.cpp:5217,4531,3839 | Seam Phase-5 contourné : `render()` lit le World vivant | L | oui |
| U4-4 | U4 | high | qual | LandscapeScene.cpp:707-858 / hpp:195-663 | ~40 handles GPU bruts, miroir onExit fragile (§8 RAII) | L | oui |
| U5-1 | U5 | high | factor | PropertyGrid.cpp:69-258 ; EditorScene.cpp:34 | Switch FieldKind écrit 3× (face U5 de H-a) | L | oui |
| U1-02 | U1 | med | qual | reflect/Registry.cpp:7-14 | Collision type-id loggée non assertée ; 2e type droppé | S | non |
| U1-04 | U1 | med | réutil | core/FrameProbe.hpp:20 (+épars) | Aucun clock primitive partagé ; std::chrono re-dérivé 4+ sites | S | oui |
| U3-4 | U3 | med | factor | TerrainSystem.hpp:112 (+15 sites) | Pack/unpack clé u64 chunk à la main | S | non |
| U3-5 | U3 | med | factor | TerrainSystem.cpp:371 (+5 pipelines) | Layout attributs `MeshVertex` réécrit ~5× | S | non |
| U4-8 | U4 | med | factor | LandscapeScene.cpp:1128-1178,4936 | Crossfade météo lerpe ~18 champs à la main, listés 3× | S | oui |
| U5-6 | U5 | med | qual | game/ui/PropertyGrid.cpp:20 | `ActiveEdit gActive` = global mutable (§8) | S | non |
| U7-3 | U7 | med | archi | BinaryFormat.cpp:14,45 ; Reflect.hpp:39 | Stream binaire dépend de l'ordre numérique de l'enum FieldKind | S | oui |
| U8-1 | U8 | med | réutil | quest/Quest.cpp:15 ; Dialogue.cpp:14 | `forEachForm<T>` réimplémenté, duplique `data::forEach` | S | oui |
| U1-03 | U1 | med | réutil | (core, absence) | Aucun type Result/expected (§8) ; optional+log perd la raison | M | oui |
| U2-01 | U2 | med | qual | GlDeviceBase.cpp:134-312 | Hot path draw utilise `unordered_map::at` (exceptions + hash/draw) | M | non |
| U2-02 | U2 | med | réutil | Rhi.hpp:25-31 | 7 handle structs quasi-identiques, aucun type partagé (H-d) | M | oui |
| U3-3 | U3 | med | archi/qual | FrameUniforms.hpp:13 vs common.glsl:3 | Struct C++ ↔ bloc GLSL synchro par commentaire seul (H static_assert) | M | oui |
| U3-6 | U3 | med | factor | PostFx.cpp:199,81 | Targets half-res + somme shaderGeneration écrits en double | M | non |
| U4-6 | U4 | med | archi | LandscapeScene.cpp:4982-5596 | `render()` = 615 l. illisibles | M | non |
| U4-7 | U4 | med | propreté | LandscapeScene.cpp (~609 littéraux) | Constantes magiques hors Forms de tuning | M | non |
| U4-9 | U4 | med | factor | LandscapeScene.cpp:2589,3310 | ~10 méthodes push-modèle RmlUi mêlées à la logique jeu | M | non |
| U5-3 | U5 | med | factor | CombatArenaScene.cpp:190 ; DemoScenes.cpp:347 | Enregistrement runtime gameplay-tags copié par scène | M | oui |
| U5-5 | U5 | med | archi | game/SaveGame.cpp:24 vs :58 | 2 chemins de capture reference divergent en jeu de champs (H-f) | M | oui |
| U6-F2 | U6 | med | factor | CharacterTick.cpp:73 vs GameTime.cpp:19 | `applyBuildupResult` dupliqué ET divergent (H-e) | M | oui |
| U6-F3 | U6 | med | factor | GameplayEffects.cpp:302 vs :352 | 2 boucles tick quasi-identiques (H-e) | M | oui |
| U6-F4 | U6 | med | réutil | event/EventBus.hpp:36 ; cue/GameplayCues.hpp:41 | EventBus + CueRegistry = 2 dispatch parallèles (H-c) | M | oui |
| U6-F5 | U6 | med | factor | save/SaveState.hpp:33 | Clone/diff réflexion duplique resolver/EditSession (H-b) | M | oui |
| U6-F7 | U6 | med | qual | Combat.cpp:5 ; CharacterTick.cpp:99 ; GameTime.cpp:47 | Détection de mort incohérente (base vs current) | M | non |
| U6-F10 | U6 | med | archi | GameTime.cpp:131 | `recomputeCurrent` 2-arg laisse les derived transitoirement faux | M | non |
| U7-4 | U7 | med | factor | EditSession.cpp:12,193 ; Synthesis.cpp | Clone/diff réflexion dupliqué (H-b) | M | oui |
| U8-2 | U8 | med | archi | gameplay/save/SaveState.hpp:3 ; CharacterTick.hpp | flecs fuite dans headers gameplay | M | oui |
| U8-4 | U8 | med | qual | script/Vm.cpp:161,265 | Coroutines gardent pointeurs bruts, dangling si entité meurt | M | non |
| U9-1 | U9 | med | qual | tests/SceneSubmitTest.cpp:1-40 | Seam RenderSnapshot à peine testé (2 cas) | M | oui |
| U9-2 | U9 | med | factor | tests/CookerTest.cpp:18 | Aucun golden test figeant les ordinaux réflexion réels | M | oui |
| U9-3 | U9 | med | archi | tests/CMakeLists.txt:78 | Aucune cible link prouvant sim-sans-render (§2.10) | M | oui |
| U6-F1 | U6 | med | archi | CharacterTick.cpp:74-78,119 ; GameTime.cpp:22-27,49,89-90 | §2.9 : regen/DoT/mort-buildup écrivent la base hors pipeline (voir DEEP) | L | oui |
| U4-10 | U4 | med | archi/factor | LandscapeScene.cpp:1014,4195 | `update()` orchestre 10 sous-systèmes ; NPC+interaction inline | L | non |
| U5-4 | U5 | med | factor | EditorScene.cpp:128-704 | 3 éditeurs dev réimplémentent la même forme (~700 l.) | L | non |
| U1-05 | U1 | low | réutil | fx/Particles.cpp:10-26 | `hashU32` dupliqué avec landscape scatter | S | oui |
| U1-07 | U1 | low | propreté | Window.cpp:46 ; GlContext.cpp:43 | `new` littéral dans factories à ctor privé | S | non |
| U1-08 | U1 | low | archi | assets/GltfMesh.hpp:8 | Loader asset dépend de `render::MeshData` | S | oui |
| U1-09 | U1 | low | propreté | _old/renderer_test/ | Arbre dead-code au racine | S | oui |
| U2-05 | U2 | low | propreté | GlDeviceBase.cpp:46-63 | Compile compute mislabellé "fragment" dans le log | S | non |
| U2-06 | U2 | low | factor | GlDevice41.cpp:9 ; GlDevice46.cpp:11 | Helpers libres re-déclarés à la main par sous-classe | S | non |
| U2-07 | U2 | low | archi | engine/ui/ImGuiLayer.cpp:4 | ImGui bypasse le RHI (exception assumée) | S | oui |
| U2-08 | U2 | low | propreté | Rhi.hpp:44 | Commentaire "(later brick)" périmé (copyTexture implémenté) | S | non |
| U2-09 | U2 | low | propreté | _old/renderer_test/ | Renderer pré-RHI mort dans l'arbre | S | oui |
| U3-8 | U3 | low | propreté | FrameUniforms.hpp:17 vs common.glsl:7 | Dérive de commentaires entre les 2 miroirs | S | non |
| U4-11 | U4 | low | propreté | LandscapeScene.cpp:3558,2558 | Strings UI en français codées en dur (§8 anglais) | S | non |
| U4-12 | U4 | low | qual | LandscapeScene.cpp:243,5210 | Structs GPU inline dans corps de fonction | S | oui |
| U4-14 | U4 | low | propreté | LandscapeScene.cpp:1197,5119 | Commentaires-fossiles post-refactor | S | non |
| U5-7 | U5 | low | propreté | game/TextureCache.cpp:59-63 | Bloc de commentaire collé en double | S | non |
| U5-8 | U5 | low | factor | CombatArenaScene.cpp:290 ; DemoScenes.cpp:222 | WASD→vecteur réécrit par scène | S | oui |
| U5-9 | U5 | low | propreté | game/scenes/DemoScenes.* | 523 l. groupent 6 classes de scène | S | non |
| U5-10 | U5 | low | qual | EditorScene.cpp:97-108 | `reload()` resolve sync sur thread UI | S | non |
| U6-F6 | U6 | low | réutil | EventBus.hpp:34 ; GameplayCues.hpp:46 ; AbilitySystem.hpp:37 | 3 schémas ad-hoc `nextId` (H-d) | S | oui |
| U6-F8 | U6 | low | qual | ability/AbilitySystem.hpp:74 | `setCurrentValue` public bypasse recompute (footgun, test-only) | S | non |
| U6-F9 | U6 | low | propreté | AbilitySystem.hpp:43 (+4) | Commentaires "deferred to Phase 8" périmés (save existe) | S | non |
| U7-5 | U7 | low | archi | world/scene/Spawner.cpp:103 | Spawner seed via setBaseValue direct (init sanctionné) | S | non |
| U7-8 | U7 | low | propreté | world/scene/Spawner.cpp:10 | Spawner tire ~12 headers gameplay/stats | S | non |
| U8-5 | U8 | low | archi | engine/ui/ImGuiLayer.cpp:4 | ImGui backend GL propre hors RHI (toléré) | S | oui |
| U8-7 | U8 | low | qual | script/Vm.cpp:252 | Handler Lua avale les erreurs (échec silencieux) | S | non |
| U8-9 | U8 | low | réutil | script/Vm.cpp:26-59 | `valueToLua`/`luaToValue` = chaîne if-constexpr par kind (H-a) | S | oui |
| U8-10 | U8 | low | propreté | tools/cooker/Main.cpp:172 | `new-guid count` via atoi non borné | S | non |
| U9-4 | U9 | low | factor | tests/*.cpp (~147 occ) | `makeTypes()`/guid/TOML recopiés dans ~30 TUs | S | non |
| U9-5 | U9 | low | qual | tests/GameClockTest.cpp:1 | Seam GameClock→effets game-time non couvert (H-e) | S | oui |
| U9-6 | U9 | low | couverture | tests/QuestTest.cpp | Aucun cas d'alias quête | S | non |
| U9-7 | U9 | low | couverture | game/ui/PropertyGrid.cpp | Value↔widget sans assertion propre (H-a) | S | oui |
| U9-8 | U9 | low | couverture | tests/CuesSchedulesTest.cpp:175 | Usage mobilier (begin/end use) non testé | S | non |
| U1-06 | U1 | low | qual | core/Jobs.hpp:14-26 | Durée de vie JobCounter comment-only ; dangling ref UB | M | non |
| U4-13 | U4 | low | réutil | LandscapeScene.hpp:522,417 | Structs runtime (Npc/MeshDraw/…) dans le header de scène | M | non |
| U7-6 | U7 | low | qual | Resolver.cpp:78 ; Record.hpp:35 | Load order (dependencies) non validé | M | non |
| U7-7 | U7 | low | reuse | data/forms/FormQuery.hpp:23,38,67 | Scans FormQuery non indexés (O(n)) | M | non |
| U8-6 | U8 | low | qual | quest/Quest.cpp:48 ; Dialogue.cpp:65 | Scans O(total forms) par événement quête/dialogue | M | non |
| U8-8 | U8 | low | factor | engine/ui/UiSystem.hpp:47 | `UiModelEventHandler` = 3e canal dispatch (H-c) | M | oui |
| U1-10 | U1 | low | qual | reflect/Reflect.hpp:94-98 | `std::function` per-field sur hot path save/diff | L | non |
| U2-03 | U2 | low | factor | GlDevice41.cpp:105 ; GlDevice46.cpp:256 | `createPipeline` copie 8 champs identiques dans 2 backends | S | non |
| U2-04 | U2 | low | archi | Rhi.hpp:139-162 | `ShaderDesc` porte du GLSL brut (dette Vulkan-readiness) | L | non |
| U3-7 | U3 | low | qual | PostFx.cpp:107 ; VegetationSystem.cpp:268 | Free RHI par listes manuelles paires (leak invisible) | L | oui |
| U3-9 | U3 | note | archi | ChunkOcclusion.hpp:31 ; GpuOcclusion.hpp:30 | Double producteur d'occlusion (staging intentionnel) | — | oui |

> Note : U2-03 est effort S mais sévérité low (placé après les low-M/L par tri
> sévérité-d'abord — corrigé ici à titre indicatif). Le tri principal reste
> sévérité → effort.

---

## 4. Primitives à mutualiser (le cœur de la synthèse)

La réponse à « qu'est-ce qui est factorisable / réutilisable par plusieurs
systèmes ? ». Chaque item nomme **tous les call-sites** qu'il unifierait.

### H-a — Un visiteur / table-descripteur `reflect::Value` unique
**Le plus haut levier.** Un seul `visit(reflect::Value, F&&)` ou une table
`FieldKind → {writeBin, readBin, writeToml, parseToml, editWidget, display, parse}`
effondre le switch per-FieldKind écrit partout :
- `data/plugins/BinaryFormat.cpp:45` (write) + Reader (~23 occ), `TomlWriter.cpp:25`,
  `PluginLoader.cpp` (**U7-2**)
- `game/ui/PropertyGrid.cpp:28-258` (valueToString + valueFromString + drawPropertyGrid),
  `game/scenes/EditorScene.cpp:34` (valueRepr) (**U5-1**)
- `script/Vm.cpp:26-59` (`valueToLua`/`luaToValue`, if-constexpr par kind) (**U8-9**)

Sources : **U5, U7, U8**. ~5 sites repo-wide.

### H-b — Un helper reflect clone/diff/defaults-diff partagé
`diffToRecord(type, form, reference)` + `cloneFields(type, src, dst)` unifie la
règle §5 « le record ne porte que ce qui change » :
- `data/plugins/EditSession.cpp:12,193` (copyFields + export diff-vs-reference),
  `Synthesis.cpp` (assemblage manuel) (**U7-4**)
- `gameplay/save/SaveState.hpp:33` (`createRecord`/`copyMatchingFields`) (**U6-F5**)
- (bonus) crossfade météo `LandscapeScene.cpp:1128` = cas d'un `lerpFields` réflexion (**U4-8**)

Sources : **U6, U7** (+ U4).

### H-c — Une primitive `Signal<Payload>` de dispatch
Alloc-id ordonnée + dispatch + ré-entrance, sur laquelle bâtir les 3 canaux :
- `gameplay/event/EventBus.hpp:36` (bus multi-abonnés) (**U6-F4**)
- `gameplay/cue/GameplayCues.hpp:41` (CueRegistry) (**U6-F4**)
- `engine/ui/UiSystem.hpp:47` (`UiModelEventHandler`, callback unique) (**U8-8**)

Sources : **U6, U8**.

### H-d — Un strong-typedef `Handle<Tag>` (id + valid() + ==)
Remplace les structs `{u32 id}` et allocateurs `nextId` dispersés :
- 7 handle structs RHI `Rhi.hpp:25-31` (**U2-02**)
- `FormHandle` (data), `BodyId` (physics), `SubscriptionId`/cue handle/`effectId`
  `EventBus.hpp:34`,`GameplayCues.hpp:46`,`AbilitySystem.hpp:37` (**U6-F6**)

Sources : **U2, U6**.

### hashU32 / murmur3 + HashRng → `engine/core/Hash.hpp`
Le finaliseur murmur3 identique (constantes `0x7feb352d`/`0x846ca68b`) est
copié dans **6+ fichiers** ; `HashRng` dans 3 :
- `fx/Particles.cpp:10-26` (**U1-05**)
- `render/landscape/` : `TerrainNoise.cpp:10`, `GrassSystem.cpp:35`,
  `VegetationSystem.cpp:21`, `SplatTextures.cpp:13`, `TreeGenerator.cpp:11`,
  `MeshBuilder.cpp:10` (**U3-2**)

Home naturel : `engine/core/Hash.hpp` à côté de `fnv1a`. Sources : **U1, U3**.

### Enregistrement des Form-types : source de vérité unique
`registerAllFormTypes(registry)` existe mais la liste est recopiée à la main
ailleurs (déjà cause d'un bug « records droppés ») ; il **manque** aussi les
agrégateurs composants/tags/spawners :
- `tools/cooker/Main.cpp:189-205` (commentaire « Keep in sync with EditorScene ») (**U8-3**)
- `game/scenes/EditorScene.cpp::onEnter`, `AllForms` (**U7/U5**)
- runtime gameplay-tags copiés par scène : `CombatArenaScene.cpp:190`,
  `DemoScenes.cpp:347`, LandscapeScene → `registerRuntimeGameplayTags()` manquant (**U5-3**)

Sources : **U5, U7, U8**.

### Template `ResidencyCache<Payload>`
`game/TextureCache.*` et `game/MeshCache.*` dupliquent toute la machinerie
async (queue, generation, pending, Residency enum, resolve→placeholder→enqueue,
pump-drain, no-wait dtor) ; seuls décode + upload diffèrent (**U5-2**). Source : **U5**.

### `ChunkStreamer<Instance>` (ring de streaming)
Le ring budgeté (nearest-first `Candidate{cx,cz,dist2}`, evict-hystérésis,
minY/maxY, generation) implémenté 3× dans `GrassSystem.cpp:244` /
`TerrainSystem.cpp:223` / `VegetationSystem.cpp:305` ; absorbe aussi le
pack/unpack clé u64 (**U3-4**). Source : **U3-1**.

### Static-assert guards pour les contrats on-disk / GPU
- **Ordinal FieldKind** : `variant index == FieldKind == KindOf` — contrat
  binaire de chaque plugin cuit + save, gardé par commentaire seul
  (`reflect/Reflect.hpp:39-77`, `BinaryFormat.cpp:14,45`) — **U1-01, U7-3** ;
  + golden ordinal test manquant (**U9-2**).
- **FrameUniforms ↔ common.glsl** : struct C++ ↔ bloc GLSL synchro par
  commentaire (`FrameUniforms.hpp:13` vs `shaders/common.glsl:3`) — un membre
  désaligné corrompt ~14 shaders — **U3-3** (rempli par U4).

Sources : **U1, U7, U9** (binaire) ; **U3, U4** (GPU).

### Primitives core manquantes
- **`core::Result<T>`/`Status`** — §8 mandate `std::expected` ; introuvable hors
  CLAUDE.md. De-facto `optional`+`LOG_*` jette la raison de l'erreur (**U1-03**).
- **`core::Clock`** — pas d'alias steady_clock/`nowMs()` ; `std::chrono` re-dérivé
  dans 4+ sites (FrameProbe, Engine, TerrainSystem, UiSystem, SaveGame) (**U1-04**).

---

## 5. Violations d'invariant à corriger

Les vraies, avec la nuance du deep-dive §2.9 :

1. **flecs fuit dans des headers gameplay** (§3 « confiné à meadows-ecs »).
   `gameplay/save/SaveState.hpp:3` et `gameplay/actors/CharacterTick.hpp`
   incluent `flecs.h` + exposent `flecs::entity` (**U8-2**). À trancher côté U6 :
   abstraction ecs ou déplacement des signatures. Effort M.

2. **§2.10 letter-violation : `world/` inclut `engine/render/`.**
   `world/terrain/TerrainPatches.hpp:8` inclut `TerrainNoise.hpp` (symboles
   headless-purs mais mal placés sous `render/`). Relocaliser `HeightPatch`/
   `HeightPatches` vers un home headless (**U7-1**). Effort S. **Corollaire** :
   le seam n'est **pas verrouillé au link** — aucune cible prouve sim-sans-render
   (**U9-3**). Effort M.

3. **§2.9 tick-loop regen / DoT / mort-buildup — MEDIUM** (voir
   `DEEP-attribute-mutation.md`). Sites réels : `CharacterTick.cpp:74-78,119`
   (DoT buildup + regen énergie) et `GameTime.cpp:22-27,49,89-90` (DoT + mort +
   regen game-time) écrivent `vitals.*` (base) hors `applyEffect`. **Le pipeline
   d'effet lui-même est correct et test-locké** ; ces écritures de base
   préservent le contrat de save. `Damage.cpp:81` (exec-calc) et `Spawner.cpp:103`
   (init seeding) **ne sont PAS des violations**. Remède : router DoT/mort via
   `applyDamage` (S) + soit sanctionner le regen comme execution-calc dans §2.9
   (S), soit bâtir des effets périodiques à magnitude-capturée (M-L).

4. **Seam Phase-5 contourné dans `LandscapeScene::render`** (**U4-2**).
   `render()`/`draw*()` interrogent le **World vivant** (`collectLights`,
   queries `LightSource`/`WaterVolume`/`MeshRender`, pointeurs `Npc`) au lieu de
   consommer un `RenderSnapshot` — seuls les meshes sont extraits. **Le type
   `RenderSnapshot` lui-même est propre** (U5 : POD auto-possédant vérifié
   ligne-à-ligne) ; c'est la scène qui le bypasse. Étendre le snapshot
   (lights/poses NPC/water/shafts) ; `render()` ne lit que le packet. Effort L.

---

## 6. Verdict de santé par unité

| Unité | Verdict | 1-ligne |
|-------|---------|---------|
| U1 Fondations & keystone | **Sain** | La partie la plus saine ; primitives RAII-propres, invariants tenus ; gaps = garde-fous absents |
| U2 RHI + backend GL | **Sain** | Interface Vulkan-shaped réelle ; split 41/46 propre ; résidus factor + hot-path `at` |
| U3 Renderer paysage | **Correct** | 14 systèmes cohésifs, zéro GL fuité ; dette = 3 duplications transverses (streaming/hash/UBO) |
| U4 LandscapeScene | **À risque** | God-object 5914 l., ~12 responsabilités, seam Phase-5 érodé — foyer de dette n°1 |
| U5 Runtime/scènes/UI | **Correct** | Seam RenderSnapshot PASS vérifié ligne-à-ligne ; leviers = factor (H-a, caches, éditeurs) |
| U6 GAS + stats | **Correct** | Pureté/RNG/réflexion PASS ; §2.9 tick MEDIUM ; convergences H-b/H-c/H-d/H-e |
| U7 Data + monde | **Sain** | §5/§2.5/§2.7 exemplaires, propreté excellente ; 1 letter-violation + refactors |
| U8 Narratif/script/seams | **Sain** | §2.8 au mot, façades étanches, cooker réutilise le pipeline ; hotspots = liste form-types + coroutines |
| U9 Tests | **Sain** | Systèmes mandatés couverts en profondeur, déterministe ; trous = seam snapshot, ordinaux, link headless |

---

## 7. Notes d'état

- **`_old/renderer_test/`** : arbre dead-code au racine (raw GL/shader pré-RHI),
  pollue chaque scan engine-wide. À supprimer (**U1-09, U2-09**).
- **Chantier-6 axe B (lighting)** : la MEMORY signalait ce travail **non commité**
  dans le working tree. **Vérifié au moment de l'audit : `git status` propre** —
  le lighting est désormais commité. (Confirmer avant tout ré-audit du lighting.)
- **Strings UI en français codées en dur** dans `LandscapeScene`
  (`"Crime observe ! Prime : …"`, cpp:3558) alors que les `LOG_*` sont en
  anglais (§8). Sortir les strings joueur en données localisables (**U4-11**).

---

## 8. Feuille de route de remédiation suggérée

**Proposition à trier par le dev** — cadence brique-par-brique, aucune brique
ne modifie de code du fait de l'audit. Trois batches par risque croissant.

### Batch 1 — « gains rapides » (effort S, faible risque)
Quick wins isolés, chacun une brique validable :
- Supprimer `_old/renderer_test/` (**U1-09/U2-09**).
- Supprimer/rendre test-only le footgun `setCurrentValue` (**U6-F8**).
- `static_assert` guards : ordinal FieldKind (**U1-01/U7-3**) + golden ordinal
  test (**U9-2**) ; FrameUniforms↔common.glsl (**U3-3**, partie assert).
- Déplacer `HeightPatch`/`HeightPatches` hors `render/` → `world/` (**U7-1**).
- Corriger la fuite flecs des headers gameplay (**U8-2**, si S par déplacement
  de signature ; sinon Batch 3).
- Nettoyer commentaires périmés/fossiles (**U2-08, U6-F9, U4-14, U5-7, U3-8**).
- Hoister `hashU32`/`HashRng` → `core/Hash.hpp` (**U1-05/U3-2**).
- Router DoT-buildup + mort-buildup via `applyDamage` (§2.9 quick consistency,
  DEEP §3 option B — S).

### Batch 2 — « mutualisation reflect » (le plus haut levier)
Le refactor à plus fort ROI, deux briques couplées :
- **H-a** : visiteur/table `reflect::Value` unique → effondre les switches
  FieldKind de BinaryFormat/TomlWriter/PluginLoader/PropertyGrid/EditorScene/Vm
  (**U7-2, U5-1, U8-9**).
- **H-b** : helper reflect clone/diff/defaults-diff partagé →
  Resolver/EditSession/Synthesis/SaveState (+ lerpFields météo) (**U7-4, U6-F5, U4-8**).
- Adjacents naturels : agrégateur form-types/tags/spawners unique (**U8-3/U5-3/U7**),
  primitives core `Result` + `Clock` (**U1-03/U1-04**).

### Batch 3 — « structurel » (effort L, à séquencer)
- **Décomposition `LandscapeScene`** selon la proposition U4 (§Décomposition) :
  commencer par WeatherController + SceneEditor/SculptTool (isolés), puis GameHud
  + NpcDirector/InteractionController, garder **LandscapeRenderer + extension du
  RenderSnapshot pour la fin** (**U4-1, U4-3, U4-5, U4-6, U4-9, U4-10**).
- **Restauration du seam Phase-5** : étendre `RenderSnapshot`
  (lights/poses/water/shafts) ; prérequis au split renderer (**U4-2/U4-4**).
- Consolidations **H-c** (`Signal<Payload>`) et **H-d** (`Handle<Tag>`)
  (**U6-F4/U8-8, U2-02/U6-F6**).
- Templates `ResidencyCache` (**U5-2**) et `ChunkStreamer` (**U3-1**).
- Wrapper RAII GPU owning (supprime les miroirs onExit / free-lists manuels :
  **U4-4, U3-7**).
