# U4 — LandscapeScene (monolithe)

**Périmètre :** `game/scenes/LandscapeScene.cpp` (5914 lignes) + `LandscapeScene.hpp` (666 lignes).
Point de convergence des chantiers 1/2/3/4/5/6/7 : ~70 méthodes, ~90 membres privés, `ownsFrame()==true`.

## Verdict rapide

Le plus gros fichier du dépôt (~13 %) et le plus à risque, comme annoncé. **Aucune** violation
de pureté (fichier `game/`, autorisé à voir monde + renderer), **aucun** appel GL direct,
**aucun** `setValue` d'attribut hors pipeline (le combat passe par `applyDamage` → GAS, §2.9 OK ;
les `get_mut<Inventory/Bounty/Equipment/Survival>` sont des composants réfléchis ordinaires, pas
des attributs GAS — conforme). Le vrai problème est **structurel** : un god-object qui agrège
~12 responsabilités cohésives et **effondre le seam extract/submit (§2.10 / Phase-5)** que le
reste du codebase protège. Le levier de valeur n°1 est une décomposition, pas des corrections
ponctuelles.

## Contrôles d'invariants (explicites)

- **§2.10 pureté headless** — N/A ici (unité `game/`), mais voir F1/F2 : le seam Phase-5 est
  contourné.
- **§2.1 tout via `rhi::`** — PASS (aucun `gl*` ; tout passe par `rhi::Device`).
- **§2.9 attributs via GameplayEffect** — PASS (`tryPlayerAttack`→`weaponDamageEvent`/`applyDamage`,
  `performRest`→effets ; pas d'écriture directe de `health`/`energy`).
- **§2.7 spawner par catégorie** — PASS (`world::Spawner`/`SpawnContext` utilisés).
- **RAII / ownership (§8)** — FAIL partiel : ~40 handles GPU bruts gérés à la main (F4).

---

## Findings

| id | sév | axe | fichier:ligne | description | action | effort | inter-unité |
|----|-----|-----|---------------|-------------|--------|--------|-------------|
| U4-1 | **crit** | archi/factor | LandscapeScene.hpp:77-664 ; .cpp:87-5914 | God-object : une seule classe agrège ~12 responsabilités (bootstrap données, streaming, contrôleur joueur/caméra, éditeur embarqué, sculpt terrain, extraction+draw GPU, UI RmlUi, tick gameplay, console, météo, panels ImGui). Header de 666 l., 90 membres, 70 méthodes — surface ingérable, 25 touches récentes. | Décomposer en sous-systèmes nommés possédés par la scène (voir §Décomposition). | L | non |
| U4-2 | **high** | archi | .cpp:5217, 5257-5261, 4531, 4677, 4717, 4088, 3839, 1329 | Seam Phase-5 contourné : `render()`/`draw*()` interrogent le **World vivant** (`collectLights(world,…)`, queries `LightSource`/`WaterVolume`/`MeshRender`, pointeurs `Npc`, `physics`) au lieu de consommer un `RenderSnapshot`. Seul `meshes` est extrait (l.1118). La présentation lit l'état sim en direct — le découplage extract/submit (SceneSubmit.hpp §9) est perdu pour tout sauf les meshes. | Étendre `RenderSnapshot` (lights, poses NPC, water volumes, shafts) ; `render()` ne lit que le packet. Prérequis à U4-1 (split renderer). | L | oui (U5 SceneSubmit) |
| U4-3 | **high** | factor | .cpp:87-707 | `onEnter` = 620 lignes séquentielles : resolve plugins+save, tuning/météo, création de ~40 ressources GPU, spawn monde, nav, physique, UI, console. Impossible à tester/relire par morceau ; toute ré-entrée (load) rejoue le bloc entier. | Extraire `bootstrapData()`, `createRenderResources()`, `spawnWorld()`, `createGameplay()`. | M | non |
| U4-4 | **high** | qualité | .cpp:707-858 (onExit) ; hpp:195-663 | ~40 handles `rhi::*Handle` bruts créés dans `onEnter`/`build*`/`refreshNpcs` et détruits à la main dans un `onExit` de ~150 l. (miroir 16 createBuffer/19 destroyBuffer, 9/9 textures, 14 bindgroups, 9 pipelines) + gardes `if(id!=0)` répétées. Miroir fragile = fuite/double-free à la moindre désync. §8 veut du RAII. | Wrapper GPU owning (`unique`-like `RhiResource<Tag>`) qui libère au dtor ; supprime le miroir onExit. | L | oui (U2/U5 : pattern partagé) |
| U4-5 | **high** | archi/factor | .cpp:1592-1863 (drawEditorUi), 1324-1448 (pickEntity/groundUnderMouse), 1310-1323 (mouseRayDirection), 1449-1591 (sculpt) | Éditeur de niveau + outil de sculpt terrain **embarqués** dans la scène de jeu (pick ray-AABB, gizmos ImGuizmo, palette, brosses, export mod). Responsabilité qui double conceptuellement `EditorScene.cpp` (40 K). | Extraire `SceneEditor` + `TerrainSculptTool` réutilisables, partagés avec EditorScene. | M | oui (U5 EditorScene) |
| U4-6 | med | archi | .cpp:4982-5596 (render) | `render()` = 615 l. : assemblage `FrameUniforms` (~50 champs), override intérieur, grade/expo sur `.w` libres, collecte lumières, key-shadow, cascades, puis N passes. Fonction unique illisible. | Découper : `assembleFrameUniforms()`, `updateLights()`, passes en méthodes `pass*()`. Va de pair avec le split renderer (U4-1/U4-2). | M | non |
| U4-7 | med | propreté | .cpp: ~609 littéraux flottants (ex. 5223-5228 `9.0f/23.0f/1.7f/3.1f`, 5090-5093 `0.85f/1.35f`, 253-256 `0.37f/0.618f`, 3477 `0.7f`, 3530-3531 bounty) | Constantes magiques omniprésentes : réglages d'éclairage/flicker/god-rays/combat codés en dur dans les corps de fonction, hors de `LandscapeTuningForm`/`StatsTuningForm` alors qu'une partie de la famille l'utilise déjà. | Remonter les constantes de gameplay/rendu tunables dans les Forms de tuning (moddable §5) ou des `constexpr` nommés. | M | non |
| U4-8 | med | factor | .cpp:1128-1178 (weather blend), 4936-4981 (capture/apply) | Le crossfade météo lerpe **~18 champs à la main**, listés trois fois (blend, capture, apply). Ajouter un champ = éditer 3 sites → dérive garantie. | Itérer les champs via réflexion (`WeatherForm` est un Form réfléchi) ou un helper `lerpFields`. | S | oui (U1 réflexion : helper diff/lerp) |
| U4-9 | med | factor | .cpp:2589-2803 (inventory/container/barter/pushItemModels), 3310-3435 (journal/dialogue models), 2402-2458 (HUD) | ~10 méthodes de « pousser un modèle RmlUi » (item rows, detail/footer, journal, dialogue, HUD, menu) enchevêtrées avec la logique jeu dans la scène. Chantier 4 entier logé ici. | Extraire un `GameHud`/`UiPresenter` possédant `UiSystem`+`ScreenStack` et les `push*Model()`. | M | non |
| U4-10 | med | archi/factor | .cpp:1014-1207 (update) ; 4195-4425 (updateNpcs), 4132-4194 (schedule/path), 1864-2080 (updateInteraction) | `update()` orchestre 10 sous-systèmes (assets, gameUi, physique, cells, colliders, clock, charTick, extract, npcs, météo, mode Play) ; la logique NPC (patrol+schedule+combat+nav, ~350 l.) et l'interaction (~217 l.) vivent aussi dans la scène. | Extraire `NpcDirector` (IA/anim/combat NPC) et `InteractionController` (prompts/travel/rest/wait). | L | non |
| U4-11 | low | propreté | .cpp:3558-3560, 2558 (et autres talkLine) | Chaînes UI en **français** codées en dur (`"Crime observe ! Prime : …"`) alors que tous les `LOG_*` sont en anglais (§8). Mélange de langues + texte non localisé dans la scène. | Sortir les strings joueur dans les Forms/données UI (localisable) ; garder le code en anglais. | S | non |
| U4-12 | low | qualité | .cpp:243-267 (towers[8*6*4]), 5210-5215 (LightsUniforms local) | Structs/buffers GPU définis **inline** dans les corps de fonction (tableau brut `f32 towers[192]`, `struct LightsUniforms` local dupliquant la disposition GLSL). Layout GPU dispersé, non partagé avec le shader. | Déplacer les structs UBO/vertex à côté de leur système (`render/landscape/*`) comme types nommés. | S | oui (U3 render) |
| U4-13 | low | réutil | hpp:522-565 (Npc), 417-423 (MeshDraw), 433-455 (LightShaft/WaterQuad) | Plusieurs structs de runtime (Npc, MeshDraw, LightShaft, WaterQuad, RigData) déclarés dans le header de la scène : état de rendu + IA + anim mêlés. Empêche toute réutilisation et gonfle le header (compile time). | Déplacer avec leurs sous-systèmes une fois U4-1/U4-10 faits. | M | non |
| U4-14 | low | propreté | .cpp:1197 `// (Time-of-day now advances…)`, 5119-5124 commentaire `.w` orphelin | Commentaires-fossiles laissés après refactors (renvois à du code déplacé), et blocs de commentaires longs qui documentent des décisions volatiles inline plutôt que dans `docs/`. | Nettoyer les commentaires périmés ; migrer les rationales durables vers les journaux `docs/CHANTIER-*`. | S | non |

---

## Proposition de décomposition

Le fichier tangle ~12 responsabilités cohésives. La scène resterait le **chef d'orchestre mince**
(lifecycle + `update`/`render` qui *déléguent*), chaque bloc devenant un membre possédé :

1. **LandscapeBootstrap** — resolve plugin stack + save layer, tuning, météo, spawn monde initial
   (extrait de `onEnter` l.87-707). Testable headless.
2. **StreamingController** — ring de cells, `snapCellEntities`, `updateStaticColliders`,
   `refreshNavObstacles`, `refreshNpcs` (déjà groupés dans `update` l.1052-1072).
3. **PlayerController / CameraController** — `enterPlayMode`/`exitPlayMode`/`updatePlayer`,
   `flyCamera`, encombrement/sprint (hpp:606-638).
4. **SceneEditor** + **TerrainSculptTool** — pick/gizmo/place/sculpt (l.1310-1863) — **à mutualiser
   avec EditorScene** (U4-5, inter-unité U5).
5. **LandscapeRenderer** — *le split le plus rentable* : possède tous les systèmes `render::*`,
   `render()`, les `draw*`/`build*`, les ~40 handles GPU, l'assemblage `FrameUniforms`, et
   **consomme un `RenderSnapshot` étendu** au lieu du World (résout U4-2/U4-4/U4-6 ; rend un
   render-thread « cheap to add » comme prévu Phase-5).
6. **NpcDirector** — `updateNpcs`/`updateNpcSchedule`/`moveNpcAlongPath`/IA combat (l.4132-4425).
7. **InteractionController** — `updateInteraction`/`performTravel`/`performRest`/`performWait`/prompts.
8. **GameHud / UiPresenter** — `UiSystem`+`ScreenStack`, tous les `push*Model`/`handleUiEvent`
   (chantier 4, l.2164-2907, 3310-3435).
9. **WeatherController** — capture/apply/blend (l.1128-1178, 4936-4981), champs via réflexion.
10. **DevConsole wiring** — `createConsole`/`updateNameplates` (l.3060-3250).

Ordre suggéré (risque croissant) : commencer par **WeatherController** et **SceneEditor/SculptTool**
(isolés, faible couplage), puis **GameHud** et **NpcDirector/InteractionController**, garder
**LandscapeRenderer + extension du RenderSnapshot** pour la fin (le plus structurant, prérequis =
U4-2). Chaque split est une brique validable indépendamment (cadence brique-par-brique).

## Inter-unités (matière Tier-3)

- **U5** — l'éditeur/sculpt embarqué (U4-5) recoupe `EditorScene.cpp` ; le pattern de teardown GPU
  manuel (U4-4) et le contournement de `SceneSubmit`/`RenderSnapshot` (U4-2) touchent le bridge de
  `meadows-runtime`.
- **U3** — structs UBO/vertex inline (U4-12) devraient vivre dans `engine/render/landscape/`.
- **U1** — le lerp/copie de champs météo à la main (U4-8) est un cas du helper diff/réflexion
  partagé pré-identifié (H-b).
