# Q2 — `game/` (scènes, contrôleurs, UI dev) + reste de `tools/`

> Vague Q2 de la revue de qualité d'octobre 2026 (`README.md` : méthode, classes
> S/G/D/B/H/P, arbitrages de juillet à ne pas re-proposer). Relecture **intégrale** du
> 2026-10-23 sur HEAD `61efb65` : `game/scenes/` (70 fichiers, 25 853 l.), `game/ui/`
> (41 fichiers, 6 173 l.), racine `game/` hors couture terrain déjà vue en Q1 (38
> fichiers, 4 262 l.), `tools/cooker/Main.cpp`, `TextureCook`, `vksmoke`, scripts — soit
> ~38 000 lignes, sept lots en parallèle. **Aucun code modifié.** Lignes = `61efb65` ;
> les symboles font foi.

---

## Verdict

`game/` est deux codes. La **racine** (`SceneStack`, `InputActions`, `SaveGame`,
`LevelEditor`, `MapRaster`, `SceneSubmit`) et les **contrôleurs extraits en juillet**
(`StreamingController`, `WeatherController`, `SaveController`, `FollowerCombat`,
`RideController`, `OptionsController`) sont propres : contextes plats, décisions pures
côté `gameplay::`, politique de commentaires respectée, six commentaires hors politique
sur 6 000 lignes dans le lot racine. Le pattern `*Controller` + `make*Context()` a tenu :
15 contextes, appelés 99 fois, tous « rebuilt per call (cheap) ».

Ce qui a regonflé `LandscapeScene.cpp` de 1 852 à **8 635 lignes** (×4,7 depuis la clôture
de l'audit de juillet) n'est pas un retour en arrière sur les extractions : ce sont **trois
sous-systèmes nouveaux jamais extraits** — la magie des esprits (2 840 l., 58 méthodes,
354 l. d'en-tête, alors que `SpiritDirector` se déclare propriétaire), la carte bornée /
sandbox (~700 l., 20 membres, transaction de swap écrite une fois et demie), et le
warmup (203 l., 14 scalaires). Autour d'eux, `update()` fait 685 lignes en trois fils
entrelacés sous 35 gates, et le « genre de monde » (cellules authorées / sandbox /
intérieur) est re-dérivé de trois booléens en 63 sites au lieu d'être une politique.

Le second axe est transversal et c'est le même qu'en Q1 : **les vocabulaires fermés sont
des strings** comparées en chaînes de `if` — kinds de packages IA, catégories de mobilier,
catégories de party, slots d'armure, événements et actions UI, noms d'écrans, styles de
combat — et **les tables à moitié existantes** (`kCategories`, `Keywords.kVocabs`,
`FieldLane`, `ConsolePanel`) attendent leurs colonnes. La cause racine est celle que Q4
nommera : la réflexion n'a pas de kind Enum ni de métadonnée de champ. Troisième axe :
**les cas particuliers sont des closures** — 24 closures dans `InteractionContext` +
`UiRouterContext`, une par action spéciale, et les deux contrôleurs UI se parlent à travers
la scène.

Les positifs sont nets et doivent servir de modèles : `decidePlayerAction` (un arbitre
pur), `resolveMeleeStrike` (une résolution partagée), `restoreMode` (deux switches plats),
`onEnter` en cinq phases nommées, `WeatherController::kWeatherLanes`, `TerrainGenTool`
(pupitre entièrement réfléchi), `ConsolePanel` (table), `NodeCanvas`, la coupe
sim/présentation des followers (17 fonctions pures côté `gameplay::`).

---

## Contrôles d'invariants

| Invariant | Verdict | Remarque |
|---|---|---|
| §2.10 : le sim ne dépend pas de `game/` | PASS | aucune règle de jeu ne vit uniquement dans `game/` (followers, combat, interruption : décision dans `gameplay::`, exécution ici) |
| §2.9 : attributs mutés par effets seulement | PASS (jeu) / note (bancs) | sprint/nage/sneak/arc/mobilier passent par `EffectForm` ; `CharacterStatsPanel`/`EffectPanel` mutent des composants de banc directement (Q2-49, acceptable, à marquer) |
| §2.7 : instanciation par le Spawner | **partiel** | bundle acteur assemblé à la main dans `StatsScene`, `CombatArenaScene`, et le save connaît des composants que le Spawner n'attache pas (Q2-39) |
| U8-3 : source unique d'enregistrement des Forms | **FAIL** | `WorldDemoScene` enregistre 6 types sur 21 à la main (Q2-44) |
| Phase 5 : le renderer ne touche pas le World ; workers → mailbox | PASS | `SceneSubmit`, `MapController`, `MiniMapPanel`, `TerrainCollision`, `SaveController`, `SpiritDirector::updateFire` appliquent l'idiome ; mais la scène écrit dans les entrailles du renderer en 8 sites (`contentEvents`, `sculptScatterQueue`…) |
| Règle dev « toutes les transitions de mode par une fonction » | **contournée** | `restoreMode` est le funnel, mais `mode = SceneMode::X` est écrit directement en 4 sites et `enterPlayMode()` appelé hors funnel en 3 (Q2-28) |
| Règle dev « enum over nested ifs » | partiel | respectée (`WarmupPhase`, `FollowerStance`, `CombatMove`, `PlayerAction`) ; violée (12 booléens d'état sur `Npc`, genre de monde en 3 booléens, `fadeDirection` i32, `brushKind` i32) |
| Politique de commentaires 2026-07-22 | FAIL (scènes) / PASS (racine, ui) | ~110 tags dans `game/scenes`, dont 44 dans `LandscapeScene.*`, 34 « idiom/precedent » dans les followers ; deux **libellés ImGui** portent des tags de brique |
| Arbitrages de juillet (§0.2 du README) | respectés | corps des commandes console, `drawUi`, `performTravel`, `restoreMode`, `finalizeActorSpawn`, U5-4 : non re-proposés ; les findings portent sur leur **forme** ou sur du code nouveau |

---

## Passe 1 — fichier par fichier

### `game/scenes/LandscapeScene.hpp` (1 102 l.) — **à refondre**

| zone | lignes | contenu | verdict |
|---|---|---|---|
| includes | 1-82 | 76 includes dont `rhi/Rhi.hpp`, `WorldRenderer.hpp`, `TerrainBakeStreamer.hpp` (→ RHI) | à surveiller (Q2-47) |
| noyau | 110-345 | `SceneMode`, renderer, atmos, forms, ECS, FX sources de props (8 structs), cellules, worldspace, éditeur | à surveiller |
| sandbox / carte bornée | 493-633 | `bakeStreamer`, 3 registres de lacs + 3 flags, `applyMapWorld`, `travelToMap`, `PendingMapTravel`… ≈ 20 membres | **à refondre** (Q2-3) |
| **esprits** | 629-982 | 9 structs d'état (`PendingSpiritAction::Mode` ×16, `SpiritEarth/Rock/Brand/Globe/FlameJet/Blow/WindHold/Line`, feu), 18 constantes, ~45 méthodes | **à refondre** (Q2-2) |
| contextes, saves, warmup, boot | 983-1099 | 15 `make*Context`, 14 scalaires de warmup, `bootPlayPending/bootSandbox` | à surveiller (Q2-9, Q2-28) |

### `game/scenes/LandscapeScene.cpp` (8 635 l.) — méthodes > 100 l.

| méthode | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `bootstrapData` | 183 | settings, pile, save = dernière couche, 13 `atmos.x = tuning.x` | à refondre | Q2-32 |
| `createRenderResources` | 287 | caches, armes, **import de scans 230 l. avec 14 GUIDs en 2 tables C++** | **à refondre** | Q2-4 |
| `setupGameplay` | 336 | physique, nav, GAS, 11 `findByEditorId`, 23 `subscribe` dont 12 identiques | **à refondre** | Q2-12, Q2-24 |
| `setupWorldAndStreaming` | 148 | ECS, cellules, worldspace, `MEADOWS_BOOT` | à surveiller | Q2-28 |
| `spawnInitialWorld` | 156 | joueur + kit, application du load (arbitré) | à surveiller | Q2-24, Q2-28 |
| **`update`** | **685** | trois fils (monde / sim / extraction-présentation) sous 35 gates | **à refondre** | **Q2-1**, Q2-20 |
| `applyMapWorld` | 193 | LA transaction de swap de carte (resets esprits ×8, collision, far-water) | à refondre | Q2-3 |
| `publishBakedTiles` | 217 | éviction, insertion, remesh, occlusion, collision, eau, sim ; 2 sections mesurées **vides** | à refondre | Q2-3, Q2-48 |
| `updateWarmup` | 203 | 5 phases + veil + boot override consommé ici | à refondre | Q2-9 |
| `makeInteractionContext` | 136 | 14 refs + **12 closures** dont 5 actions de 20-40 l. | à refondre | Q2-10 |
| `performTravel` | 156 | swap de worldspace (arbitré : reste) | à surveiller | — |
| `createGameUi` | 200 | 13 modèles UI déclarés en listes de strings | à surveiller | Q2-21 |
| `updateGameUi` | 245 | hotkeys modales ×5 copiées, noms d'écrans en strings | à refondre | Q2-22 |
| `createConsole` | 402 | 14 commandes (arbitré : reste) ; `spirit` suit Q2-2 | gardé | Q2-2 |
| `executeSpell` / `applyPendingSpiritActions` | 134 / 241 | trajectoire × élément × verbe → `Mode` → 13 blocs `if` | **à refondre** | **Q2-16** |
| `updateSpiritFire` | 200 | job + props + placement des flammes + diff d'émetteurs | à refondre | Q2-8 |
| 9 paires `updateSpiritX/endSpiritX` | 60-100 chacune | gestes canalisés, même cycle upkeep ×9 | à refondre | Q2-30 |
| `updateRiverSourceFx` / `updateRiverCascadeFx` / `updateFxSources` | 95 / 142 / 101 | émetteurs à portée, même squelette ×3 | à surveiller | Q2-36 |
| `castWater/Ground/FireReading` | 72-84 | lectures HUD, clôture copiée ×3 | à surveiller | Q2-37 |
| `drawUi` | 282 | 7 hotkeys `ImGui::IsKeyPressed`, F5/F9 = actions de jeu (arbitré : reste) | gardé | Q2-22 |
| `saveRenderTuning` | 81 | 24 copies champ à champ à côté de `captureTuning` | à surveiller | Q2-32 |

### Contrôleurs et directeurs (`game/scenes/`)

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `PlayerController.*` | 230+847 | stance + locomotion ; `updateLocomotion` 239 | à surveiller (linéaire ; assemblage GAS copié, 27 `is_alive()`) | Q2-34 |
| `NpcDirector.*` | 341+398 | `Npc` 54 champs / 12 booléens, `NpcContext` 28, hub de 13 TU ; `update` 270 | **à refondre** | **Q2-6, Q2-7**, Q2-11 |
| `NpcCombatController.*` | 112+812 | perception → cible → `chooseCombatMove`/brain Lua → exécution ; `update` 258 | à refondre | Q2-11, Q2-34 |
| `NpcSpawner.*` | 56+324 | Forms → `Npc` ; `refreshNpcs` 272 (O(n²), seeding ×2) | à surveiller | Q2-6, Q2-23, Q2-50 |
| `NpcMovement.*` | 74+244 | locomotion partagée (fonctions libres) | **sain** ; un B latent | Q2-43 |
| `NpcScheduleController.*` | 50+294 | packages par `kind == "…"` | à refondre (G) | **Q2-18** |
| `ProjectileDirector.*` | 75+194 | vol, collision, ramassage | sain | Q2-34 |
| `FollowerController.hpp/.cpp`, `FollowerSocial.cpp`, `FollowerCombat.cpp` | 377+661+681+214 | 39 méthodes en 3 TU ; logique **déjà** headless dans `gameplay::` | à surveiller (forme) ; `FollowerCombat` **sain** | Q2-12, Q2-31, Q2-46 |
| `RideController.*` | 78+140 | monture v1, `gameplay::stepRide` pur | **sain** | Q2-41 |
| `InteractionController.*` | 155+459 | `update` 343 : scan, label, [E], [F], fade ; 12 closures | **à refondre** | **Q2-5, Q2-17**, Q2-29 |
| `GameHud.*` | 129+513 | présentation RmlUi ; `HudContext` 27 champs | à surveiller | Q2-40 |
| `UiRouter.*` | 121+578 | events/actions en strings (42 `== "…"`) ; 12 closures | à surveiller | **Q2-21**, Q2-40 |
| `OptionsController.*` | 109+218 | steppers par `if`, `decideCapture` pur | sain (G local) | Q2-21 |
| `SaveController.*` | 151+178 | save async single-flight, `SaveContext` 12 + 4 | **sain — modèle** | Q2-32 (14 lignes miroir) |
| `MapController.*`, `MiniMapPanel.*` | 108+229, 55+161 | même peintre `MapRaster`, même job async ×2 ; `open` 152 | à surveiller | Q2-14, Q2-38 |
| `QuestDirector.*` | 108+291 | générique sauf `EasternMenace` câblé | à surveiller | Q2-24 |
| `SpiritDirector.*` | 242+309 | sources, jets, règles, voie du feu ; 10 tableaux FX parallèles | sain en soi ; **ce qu'il n'a pas** = Q2-2 | Q2-2, Q2-41 |
| `StreamingController.*` | 86+242 | contexte plat 8 champs, trois fixups nommés | **sain — modèle** ; foyer de Q2-20 | Q2-20, Q2-41 |
| `WeatherController.*` | 44+115 | `kWeatherLanes` | **sain — la référence** (T-1) | — |
| `FxDirector.*`, `SceneConsole.*`, `LandscapeTuning.hpp` | 184, 122, 20 | cues standard ; cycle de vie console ; shim d'alias | sain / sain / transition | Q2-36, Q2-48 |

### Autres scènes et outils d'éditeur

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `EditorScene.*` | 106+1 283 | Game DB editor (U5-4 différé) ; **nouveaux** : type-id en chaînes ×26, bootstrap copié, pile propre même en overlay | à surveiller | Q2-26, Q2-33, Q2-15 |
| `TreeCreationScene.*` | 79+379 | authoring d'arbres ; inclut `LandscapeScene.hpp` pour `replace()` ; bootstrap copié | à surveiller | Q2-15, Q2-33, Q2-41 |
| `CombatArenaScene.*` | 95+586 | banc 2D GAS ; 14 GUIDs en dur, bundle acteur à la main, handler de cue hardcodé | à surveiller | Q2-24, Q2-39, Q2-36 |
| `WorldDemoScene.*` | 84+225 | base des démos 2D ; **6 `register*` à la main**, 3 plugins par chemin | à surveiller | **Q2-44**, Q2-33 |
| `StatsScene.*` | 41+100 | banc stats ; bundle acteur à la main | sain | Q2-39 |
| `SceneEditor.*` | 94+479 | éditeur in-world ; `draw` 323 ; spawn depuis draft ×3 | à refondre | Q2-13 |
| `TerrainGenTool.*` | 76+288 | pupitre = `drawReflectedStruct` | **sain — le modèle du panneau réfléchi** | Q2-47 |
| `TerrainSculptTool.*`, `DungeonGenTool.*` | 77+147, 58+241 | brosse (`i32 brushKind`) ; kit de mine par 6 `findByEditorId` | sain | Q2-24, Q2-29 |
| `RenderTuningIo.*` | 64+457 | `FieldLane` compile-checkée | **sain — le modèle T-1** | Q2-32 |

### `game/ui/` (dev UI ImGui)

| fichier | l. | verdict | findings |
|---|---|---|---|
| `PropertyGrid.*` | 306 | sain ; dispatch des 11 kinds écrit **deux fois** | Q2-35 |
| `Keywords.*` | 149 | **3e copie des vocabulaires sim, divergente** | **Q2-19, Q2-42** |
| `EventPicker.*`, `FormPicker.*`, `ConditionBuilder.*` | 425, 165, 122 | tables (type, champ) en strings ; dispatch par **nom de type** ×15 | Q2-19, Q2-26, Q2-41 |
| `ConsolePanel.*`, `NodeCanvas.*`, `GraphPanelCommon.*`, `TestActor.*`, `OverlayBar.hpp` | 238, 356, 96, 68, 36 | **sains — modèles** | Q2-48 (builtins hors table) |
| `AnimGraphPanel.*`, `QuestGraphPanel.*`, `DialogueGraphPanel.*` | 430, 437, 335 | `drawCanvas` 278/287/213 : ~250 l. dupliquées sur 970 | Q2-27 |
| `RenderTuningPanels.*` | 956 | **190 widgets à la main** vs lanes ; `Log TOML` 60 champs | **Q2-32** |
| `CharacterStatsPanel.*` | 453 | banc linéaire 410 l. ; tables de noms d'enums du sim recopiées | Q2-49 |
| `EffectPanel.*`, `AbilityPanel.*`, `FxPanel.*`, `ClipTimelinePanel.*`, `AnimPreviewPanel.*`, `FieldWidgets.*` | 177, 157, 166, 189, 823, 89 | sains (AnimPreview bien découpé ; 3e hash réfléchi du dépôt) | Q2-41, Q2-49 |

### Racine `game/` (hors couture terrain Q1) et `tools/`

| fichier | l. | verdict | findings |
|---|---|---|---|
| `SceneStack.*`, `Scene.hpp`, `main.cpp`, `InputActions.*`, `LevelEditor.*`, `SoundResolver.*`, `Barter.*`, `AllForms.*` | — | **sains — modèles** (table de scènes, enum + `Binding`, `fieldId()` null-safe, `childrenOf`) | — |
| `SaveGame.*` | 177+529 | sain (atomique, pur, gate doctestée) ; chaîne par type ×5 ; `findField()->id` nu ×6 | Q2-39, Q2-41 |
| `Settings.*` | 44+156 | non réfléchi, TOML à la main ×2 | Q2-21 |
| `TerrainCollision.*`, `VegetationCollision.*` | 245, 274 | deux streamers d'anneau copiés ; colliders par **plages d'index de variante** | Q2-38, **Q2-25** |
| `MapRaster.*`, `MapView.*` | 290, 214 | sain / confirme Q1-33/34/35 (+1 site chacun), 3e oracle « mouillé » | Q2-41 |
| `SceneSubmit.*`, `RendererAssets.*`, `WeaponMeshes.*`, `InventoryView.*`, `ScreenStack.*` | — | sains ; petites duplications | Q2-41 |
| `tools/cooker/Main.cpp` (hors dispatch Q1-17) | 422 | `terrainPad` hors pile ; `--patch` exige `argc == 8` non documenté | Q2-41 |
| `tools/cooker/TextureCook.*` | 587 | `cookTerrainMaterials` 227 : 4 canaux inline, `channelMean()` ignoré | Q2-14 |
| `tools/vksmoke/Main.cpp` | 879 | harnais ; 4 `check(true, …)` qui ne peuvent pas échouer | Q2-45 |

**Fonctions > 200 l. du périmètre (comptées)** : `update` 685 · `drawRenderPanel` ~490 ·
`drawCharacterStatsPanel` 410 · `createConsole` 402 · `InteractionController::update` 343 ·
`setupGameplay` 336 · `SceneEditor::draw` 323 · `QuestGraphPanel::drawCanvas` 287 ·
`createRenderResources` 287 · `drawUi` 282 · `AnimGraphPanel::drawCanvas` 278 ·
`refreshNpcs` 272 · `NpcDirector::update` 270 · `NpcCombatController::update` 258 ·
`updateGameUi` 245 · `applyPendingSpiritActions` 241 · `updateLocomotion` 239 ·
`cookTerrainMaterials` 227 · `publishBakedTiles` 217 · `DialogueGraphPanel::drawCanvas` 213 ·
`updateWarmup` 203 · `updateSpiritFire` 200.

---

## Passe 2 — système par système

### S1. La scène : trois sous-systèmes à extraire, une séquence à nommer (Q2-1, Q2-2, Q2-3, Q2-8, Q2-9, Q2-10)

**Pattern.** `update()` (1354-2038) croise trois fils — monde (bake sandbox, cellules,
détection de ligne de carte, voyage **exécuté deux fois** selon le mode de pause,
prefetch), sim (physique, joueur, triggers, esprits, PNJ/followers/projectiles/fx),
extraction et présentation (snapshot, particules, viewmodel, vent, input) — sous `uiPaused`
×8, `bakeStreamer` ×8, `pendingMapTravel` ×8, `simPaused` ×5, `mode == Play` ×7,
`sandboxActive` ×3, `interiorMode` ×3. La **carte bornée** est un sous-système complet
sans contrôleur : `applyMapWorld` 193 l., `setSandboxMode` (moitié « story » de la même
transaction), `publishBakedTiles` 217, `refreshSandboxLakes`, `publishWaterBodies`,
`travelToMap`, `probeSandboxSpawn`, `loadNeighbourOverviews`, `makeFarWaterProvider`,
`finalizeSandboxSpawn` ; la transaction « le monde a changé » est écrite à deux endroits,
le rebuild collision à **six**, le chemin `terrain-cache` à six (alors que
`game::mapCacheDir` existe), la bbox de rivière à trois, l'éviction de lacs et le clamp
« lac ∩ reach → clés de chunks » à deux. La **magie des esprits** : 2 840 l., 58 méthodes
(`castSpirit`, `executeSpell`, `applyPendingSpiritActions`, 9 paires `updateX/endX`, 3
lectures, feu ~900 l., vent, grimoire, FX ambiants), 354 l. d'en-tête, 74 appels
`spiritDirector.` — alors que `SpiritDirector.hpp:25` se présente comme « the scene-side
owner of the spirit framework ». Ce que la scène prête réellement à ces méthodes tient en
~20 références (`aimGround`, caméra, joueur, `fxSim`, cues, `say`, input, `makeWaterQuery`,
params terrain, `heightPatches` + `republishTerrain`, `npcs()`, pending layer, colliders,
`collectProps`, textes, forms, tags, atmos, audio) : la taille d'un `NpcContext`. Le
**warmup** (`updateWarmup` 203 l., 14 membres) est correctement un `enum + switch`, mais
il consomme le boot override, respawne la capsule, écrit la veil RmlUi, et `enterPlayMode`
lit sa phase pour différer. `makeInteractionContext` porte 12 closures dont 5 actions de
jeu de 20-40 l. (homage, mount, chest, lever, bury) écrites dans un constructeur de
contexte.

**Mécanisme existant.** Le patron `*Controller` + `make*Context()` appliqué quinze fois ;
`onEnter` en cinq phases nommées ; `StreamingController`/`TerrainBakeStreamer` (déjà hors
scène) ; `SpiritDirector` ; `InteractionController` (possède déjà le dispatch porte/objet/
PNJ/mobilier) ; `world/spirit/*` headless.

**Design.**
- `update()` devient la séquence `ui → sim → monde → extraction → présentation`, chaque
  étape une méthode nommée (le modèle `onEnter`) ; le voyage exécuté en un seul site.
- **`SandboxWorldController`** (`SandboxContext` : params renderer, `terrainBase`/
  `waterBodies` partagés, physique/collision, tuning, cache dir) possède les 20 membres et
  expose `applyMap`, `update(focus)`, `publishTiles`, `travelTo`, `probeSpawn` ; une
  `republishWorld(rects)` unique, un `rebuildCollision()` unique, `riverBounds()` dans
  `Hydrology.hpp`, `cacheRoot()`.
- **`SpiritContext`** (`makeSpiritContext()`) ; `SpiritDirector` reçoit les gestes (`cast`,
  `update(dt, ctx)`, `extract`, `reset`) ; la **présentation du feu** (flammes, props,
  arbres, son, lumières) part dans un `FireDirector` avec `placeFlames(burning, cam)` pur
  et testable ; les FX ambiants (torches, sources/cascades de rivières — rien d'« esprit »)
  dans `FxDirector` ou un `AmbientFxController`. La commande console `spirit` et les
  `WorldActions` du VM suivent ; son `clear` énumère **7 resets** = la preuve qu'un
  `resetSpirits()` manque (à vérifier : `applyMapWorld` et `onExit` portent-ils la même
  liste ?). Découpage par élément, un commit chacun.
- **`WarmupController`** (`WarmupContext` : bakeStreamer, compteurs renderer, screenStack,
  flyCamera) avec deux callbacks `onSpawnPlaced`, `onRevealed` que la scène branche ; la
  scène garde `warmup.phase()`.
- Les cinq closures longues deviennent des méthodes nommées de la scène ou descendent dans
  `InteractionController` quand elles ne touchent que le contexte (lever : tout y est).

**Risque.** Aucune couverture headless : validation en jeu par le dev (boot story/sandbox,
traversée de ligne, intérieur, chaque geste). Effort **L** pour chacun des trois, mais
mécanique : aucune logique à réécrire.

### S2. Le genre de monde et les modes : des booléens à une politique (Q2-20, Q2-28)

**Pattern.** « Quel monde suis-je » est re-dérivé à chaque site de `sandboxActive`,
`bakeStreamer != nullptr`, `interiorMode`, `sandbox->grid.valid`,
`renderer.terrainParams().sandbox` : **63 occurrences** dans la scène, dont 8 dans le code
des esprits. `restoreMode` est bien le funnel en deux switches plats (positif, règle dev),
mais `mode = SceneMode::X` est écrit directement en 4 sites (1112, 1268, 1274, 1331 :
chargement d'une save → `Play` avec `spawnBody` + `setRelativeMouseMode` + `show("hud")`
**recopiés** d'`enterPlayMode`) et `enterPlayMode()` est appelé hors funnel en 3 : un effet
de bord ajouté à `restoreMode` ne s'applique ni au chargement ni au boot. Deux chemins
d'entrée en jeu : `MEADOWS_BOOT` (consommé dans la phase `Reveal` du warmup, qui recopie
« verbatim » — le commentaire le dit — le `play-story/play-sandbox` d'`UiRouter`).

**Mécanisme existant.** `StreamingController`/`StreamingContext` (contexte plat de 8 champs,
zéro connaissance du genre de monde — le foyer naturel), `restoreMode`, la vision dev
« 3 modes Play/Spectator/Edit, éditeur = couche SceneStack ».

**Design.** `enum class WorldKind : u8 { AuthoredCells, SandboxMap, Interior }` porté par
`StreamingContext`, **décidé en un endroit** (`enterWorld`/`performTravel`, qui restent dans
la scène), et une `WorldPolicy { snapsToTerrain, streamsBakes, hasFarWater, allowsSpirits, …
}` lue par les fixups et par `update()` ; la scène interroge la politique au lieu de trois
booléens. `enum class BootMode { Story, Sandbox }` + `beginPlay(BootMode)` appelé par
l'UiRouter (closure de contexte) et par le boot override ; `onEnter` finit par
`restoreMode(savedMode)` (une surcharge `enterPlayMode(const Vec3* feet)` pour « save →
position exacte ») ; `pendingPlayEntry` et le boot passent par `restoreMode(Play)`.
**Risque** : smoke des trois genres (`MEADOWS_BOOT=story|sandbox` + une porte d'intérieur),
chargement de save dans les trois modes. Effort **M** / **S**.

### S3. Les vocabulaires fermés : des strings aux tables (Q2-16, Q2-17, Q2-18, Q2-19, Q2-21, Q2-22, Q2-23, Q2-25, Q2-26, Q2-27)

**Pattern.** La **matrice des sorts** est écrite trois fois (`Spells.cpp:91-161`
`spellSupported` ; `executeSpell` trajectoire → `if` élément → `if` verbe → 16
`PendingSpiritAction::Mode` ; `applyPendingSpiritActions` 13 blocs `if (action.mode == X)`),
et quatre cellules (trois lectures, le mur) contournent la file du point sûr ; ajouter un
sort = trois sites + `Mode` + une struct + une paire update/end + le `clear`. Les **kinds de
packages IA** : `kind == "wander"`, `"useFurniture" || "sleep" || "eat" || "work"`, else =
travel (`NpcScheduleController.cpp:114`) — vocabulaire en commentaire dans `AiForms.hpp:28`,
copié dans `Keywords.cpp`, aucun exécuteur dans `gameplay/ai` ; un kind mal orthographié
tombe en silence dans « travel ». `combatStyle == "healer"` ×2 ; `MarkerKind.kind == "patrol"`.
Les **cas de mobilier** : `"grave" → Grave : "mount" → Mount : "container" : "lever" :
Furniture` et `category == "bed" → hours = 8` dans `InteractionController` — alors que
`FurnitureForm` est réfléchi et porte déjà `screen` (« the furniture carries its screen »).
`followerCategory` `"major"|"minor"|"mount"` (4 `==` dans `Followers.cpp`), `ArmorForm.slot`
en chaîne de 4 `if` **à deux endroits** (`UiRouter`, `EquipmentStats`). **Hostile/garde**
dérivés de `tag == "Faction.Bandits"` / `"Faction.VillageGuard"` dans le spawner alors que
`FactionRelationForm` + `FactionStanding` existent (§6.1). Côté UI : `UiRouter` 7 modèles ×
13 événements + 14 actions en `if (string == …)`, `"wait1/4/8"` décodés du libellé,
`InventoryView::Category/Column` parsés sans table ; `OptionsController` un `if` par
réglage avec pas et bornes en dur ; 13 modèles UI déclarés en listes de strings ; les
hotkeys modales copiées ×5 avec `top->name == "map"` ; 14 noms d'écrans littéraux ; 7 hotkeys
dev en chaîne `ImGui::IsKeyPressed` dont F5/F9 (quicksave/quickload) hors `ActionMap`. Côté
éditeur : quatre tables « (type, champ) → widget » en **strings** (`Keywords.kVocabs`,
`EventPicker.kEventFields`, `FormPicker.pickerTypeFor/isItemField`) consultées par une
chaîne de `if` dans `PropertyGrid` — `Keywords.hpp` demande lui-même la promotion en
métadonnée réfléchie ; `EditorScene` décide hiérarchie/conditions par 26 comparaisons de
type-id à côté de `kCategories` ; dispatch par **nom de type** `type.name == "XForm"` en 15
sites vs `isA` dans le même dossier ; les trois graph panels dupliquent la grammaire
(~250 l. sur 970). Les **colliders de végétation** dépendent de plages d'index de variante
(`kFirstRock..kFirstBush`, `kFirstDebris + 1 == log`) : le motif « plages d'ordinaux » de
Q1-10.

**Mécanisme existant.** `ConsolePanel` (table nom → `std::function`), `main.cpp` (table de
scènes), `Spawner::byCategory`, `kClauseEvaluators` (`Condition.cpp`), `parseCombatMove`/
`combatMoveName` (le couple parse/name modèle), `CombatMove`/`PlayerAction` (enum + switch
plat), `FurnitureForm.screen`, `FactionStanding`, `InputAction`/`ActionMap`, `UiScreenForm`
(écrans = data), `OptionsController::handleEvent` (déjà séparé), `kCategories[]`,
`NodeCanvas::Actions` + `GraphPanelCommon`, la table des variantes de `VegetationSystem`.

**Design.**
- Un **registre de sorts** `(trajectoire, élément, verbe, canalisé) → { supported, makeAction,
  begin }` ; `spellSupported` = `registry.contains(key)` (la cellule existe = elle est
  implémentée, plus de dérive entre `world/spirit` et la scène) ; `Mode` disparaît.
- `enum class PackageKind` + `parse/name` **dans `gameplay/ai`** (headless, parse une fois
  au build du Npc), table `kPackageTraits`, exécution en `switch` plat côté game ; `enum class
  CombatStyle` + `{ preferredBand, powerPolicy }` absorbé par `chooseCombatMove(situation,
  style)` (testable).
- `FurnitureForm` gagne `restHours` et `interaction` (string parsée une fois par une table
  `static_assert`-ée tant que la réflexion n'a pas d'Enum) ; `category` redevient descriptif ;
  table `PromptSpec[PromptKind] { labelKey, reachRatio, primary, alternate }` ; le dispatch
  émet un événement de bus `OnInteract{kind, entity}` que `FollowerController` (revive/bury),
  `RideController` (mount), le donjon (lever) écoutent : zéro closure, extensible par Lua.
- `hostile = standing(npc.faction, playerFaction) == Enemy` résolu au build ; `guard` =
  trait de faction, jamais un nom propre.
- `UiRouter` : table `{ model, event, handler }` où chaque contrôleur **enregistre** ses
  événements ; `wait` + `args[0] = heures` dans le `.rml` ; tables de noms `static_assert`-
  ées à côté de `InventoryView::Category/Column` ; `Settings` réfléchi (`REFLECT_FIELD` ≠
  Form) + table `Stepper { field, step, min, max, textId }` ; `constexpr` partagés
  `GameHud`/`createGameUi` pour les 13 modèles (à terme `UiModelForm` enfant d'`UiScreenForm`) ;
  table `ScreenHotkey { action, closes, playOnly, open }` ; F5/F9 → `InputAction::QuickSave/
  QuickLoad` lus dans `update`, pas dans `drawUi` ; table `kDevHotkeys[]` dont la top bar
  génère l'aide (tapée trois fois aujourd'hui).
- **`FieldMeta`** à côté de `REFLECT_FIELD` : `{ enumNames, pickerType, eventName, itemRef,
  range }` déclarée par le Form, lue par le grid, la console (validation de `set`), le cooker
  `validate` et le parseur sim — **c'est la décision T-3** (kind Enum / métadonnées) à
  prendre avec Q4 ; à défaut, une table **unique** `kFieldMeta` keyée sur `(TypeInfo::id,
  FieldInfo::id)` remplaçant les quatre. `EditorCategory` gagne les colonnes `hierarchy` et
  `conditionsParentField` (compatible U5-4 : on déduplique la sélection, pas les dessins).
  `GraphGrammar { node, edge, parentField, fromField, toField, newName }` +
  `applyCanvasActions`/`drawGraphPopups` dans `GraphPanelCommon`.
- Colonne `Collider { shape, half }` par variante dans la table de `VegetationSystem` ;
  `cookColliders` = une boucle.

**Risque.** UI : navigation en jeu, `.rml` à toucher en même temps (data UI moddable) ;
mobilier : contenu `base/*.toml` à migrer avec une gate de compat ; packages : smoke du
village ; factions : bandit/archer validés → A/B. Effort **S** à **M** par item.

### S4. Les données en dur dans le code : GUIDs, editorIds, plages (Q2-4, Q2-24, Q2-44)

**Pattern.** `createRenderResources` : deux tables locales `kScans[10]`/`kPlants[4]` (GUID
d'asset, slot de variante, budget de triangles, taille, double face, échelle) suivies de
230 l. de pipeline d'import (load glTF, pièce la plus dense, décimation ×3-4, textures par
convention de suffixe `_diff_1k`/`_nor_gl_1k`/`_disp_1k`/`_alpha_1k`). `findByEditorId` en
dur : 11 Forms de gameplay dans `setupGameplay` (`"SprintCost"`, `"RustySword"`,
`"PlayerAttack"`, `"Dodge"`, `"GoldCoin"`…) + `"Player"`, `"HuntingBow"` + 12 flèches ;
`"Grave"` ; `"SpiritWaterJet"` ; 6 dans `DungeonGenTool` ; `"EasternMenace"` câblé dans un
`QuestDirector` par ailleurs générique ; 14 GUIDs littéraux dans `CombatArenaScene`. Un mod
qui renomme casse en silence (pointeur nul → `if (playerWeapon)`). `WorldDemoScene`
enregistre **6 types sur 21** à la main.

**Mécanisme existant.** `LandscapeTuningForm.startWorldspace` (GUID avec fallback editorId
**documenté comme déprécié** + warn — le modèle exact), `StatsTuningForm` (précédent U4-7),
`tuning.broadleafTreeType` (une Form nomme une espèce), `RendererAssets` (« the renderer
never sees a Form nor the VFS »), `QuestStateForm.kind`, `game::registerAllFormTypes`.

**Design.** `ScatterPropForm` (slot, asset, targetTris, size, doubleSided, scaleMul,
pickDensestPart) + `RendererAssets::loadScatterProps(device, renderer, assetDb, forms)` ;
`GameplayLinksForm` (ou champs GUID sur `StatsTuningForm`) : `sprintCost`, `swimCost`,
`sneakCost`, `bowDrawCost`, `playerActor`, `startingWeapon/Bow/Arrows`, `attackAbility`,
`dodgeAbility`, `currency`, `graveForm` ; `DungeonKitForm` ; `ArenaBenchForm` ; les
`findByEditorId` deviennent le fallback déprécié avec warn. `EasternMenace` : un kind d'état
`"turnin"` sur `QuestStateForm` → le directeur mire `Quest.<EditorId>.Ready` pour toute quête.
`WorldDemoScene` → `registerAllFormTypes` (et vérifier l'équivalent ECS :
`registerCoreCategories/Spawners/SceneComponents/GameplayComponents` ×4 vs la scène
principale → `game::registerAllRuntime`). `TestLegWound`, `kDevStartInterior` + GUID
`4d7a9b30-…` = code de dev à retirer ou à passer en commande console. **Risque** : look des
rochers/fougères (A/B), sinon aucun si les GUIDs par défaut pointent les mêmes records.
Effort **S**/**M**.

### S5. Les PNJ : l'état en booléens, le hub, l'assemblage GAS recopié (Q2-6, Q2-7, Q2-11, Q2-34)

**Pattern.** `Npc` = 54 champs dont 12 booléens d'état (`sitting`, `hostile`, `guard`,
`dead`, `downed`, `attacking`, `blocking`, `weaponDrawn`, `steered`, `scheduleInterrupted`,
`escapedInterior`, `furnitureClaimed`) ; cinq sont des **miroirs de tags GAS** entretenus à
la main avec deux boucles de seeding copiées ; `alive | downed | dead` exprimé par deux
flags (`isDowned ? DownedAlly : isDead ? Corpse : Actor`). La décision « que fait le PNJ
cette frame » est une chaîne `if (inCombat) else if (inDialogue) else if (fleeingFire) else
if (staying) else if (following) else if (schedule) else if (patrol)` précédée de deux blocs
« figer la pose » identiques. `NpcDirector.hpp` est le **hub** : inclus par 13 TU, tire
`rhi/Rhi.hpp` (deux `BufferHandle` dans `Npc`) et `anim/Anim.hpp` ; quatre cycles directs
(`NpcMovement`, `NpcCombatController`, `NpcScheduleController`, `NpcSpawner`) ; `npcs()`
renvoie `vector<uptr<Npc>>&` **mutable**, placé dans `PlayerContext`, `const&` dans trois
autres, 25 usages externes à nu, scan linéaire par entité visée alors que `npcByEntity`
existe. `NpcCombatController::update` résout la cible en deux branches de 30 l. ;
`NpcDirector::update` 270 l. aux phases implicites. L'**assemblage GAS** est recopié :
`StatBlock { get_mut ×4 }` ×11, self-cast `tryActivate(…, set, system, set, system, …)` ×6,
lecture de tag d'état `tags.find("State.X") → has()` ×12 (alors que `syncStateTag` factorise
l'écriture), préparation de frappe ×3 autour d'un `resolveMeleeStrike` partagé, décocher une
flèche ×2, dégâts non mitigés ×2, drains ×4, « face à la cible » ×6.

**Mécanisme existant.** `decidePlayerAction` (un arbitre pur, une sortie — **le modèle**),
`updateInterruption` (edges purs), `FollowerStance` (enum), `npcByEntity_`/`findNpc`,
`factionOf()`, `StatBlock` (`Damage.hpp:113`), `AbilityContext`, `syncStateTag`
(`AbilitySystem.hpp:91`), `resolveMeleeStrike`/`resolveStrikeDamage`, `NpcMovement`
(fonctions libres — la maison des helpers).

**Design.** `enum class NpcLife { Alive, Downed, Dead }`, `enum class NpcActivity { Combat,
Dialogue, FleeingFire, Staying, Following, Schedule, Patrol, Idle }` résolus par **une**
fonction pure `decideNpcActivity(const NpcSignals&)` (headless, doctestable — le jumeau de
`decidePlayerAction`), `switch` plat pour l'exécution, miroirs de tags en table `{ "State.
Dead", &Npc::dead }` parcourue par un `syncMirrors()` unique (seeding ET lecture par frame),
`freezePose()`. `Npc` = données **sim** ; handles GPU dans un `NpcGpu` keyé par entité
(le commentaire dit que le draw state a déjà migré — finir le geste), `NpcDirector.hpp` perd
le RHI ; vue étroite `NpcRegistry { forEach(const Npc&), find(entityId), alive() }` +
commandes nommées (`setCombatTarget`, `addShove`, `markEscaped`, `standUp`) ; `teleportNear`
descend dans `NpcMovement`. `resolveCombatTarget()` unique ; `NpcDirector::update` en 8
appels nommés. Côté GAS : `gameplay::statBlockOf(Entity)`, `tryActivateSelf(Entity, Ability,
ctx)`, `hasStateTag(system, registry, name)` + constantes nommées pour les ~15 tags d'état,
`resolveStrikeBetween(attacker, defender, payload, options)` + `sweepActors(from, to,
candidates, exclude, fn)` (résout en passant le TODO(P1) « actor-first » des projectiles),
`looseArrow(shooter, weapon, origin, dir, speedFactor, inventory)`,
`applyUnmitigatedDamage(block, type, amount, ctx)`, `DrainChannel[]` + `tickDrains`,
`faceToward(npc, transform, to)`, `requestPath(ctx, npc, target, radius, repath)`. **Risque** :
validation en jeu (village, combat bandit/archer validés → A/B) ; la décision pure se teste.
Effort **M** (Npc), **S** chacun (GAS).

### S6. Une définition par champ, côté jeu (Q2-32, Q2-33, Q2-39, Q2-31)

**Pattern.** `RenderTuningPanels` : 190 sliders/checkbox à la main avec plages en dur,
chaque knob existant **quatre fois** (struct de params, Form, lane `RenderTuningIo`,
slider) ; `Log TOML` formate 60 champs à la main alors que `captureTreeTuning` + Save font
le vrai chemin §5 ; `applyTuning` répète les plages des sliders en clamps. `saveRenderTuning`
recopie 24 champs **à côté** de l'appel à `captureTuning` (le mécanisme est dans le même
appel et ces champs n'y sont pas — exactement le bug raconté en en-tête de
`RenderTuningIo.cpp`, « nine conifer fields silently stopped saving »). `bootstrapData` 13
`atmos.x = tuning.x` ; `SaveController` 14 lignes miroir `WorldStateForm`. Le **bootstrap de
la pile** est copié dans `EditorScene` et `TreeCreationScene` sans la gate de langue (le
Game DB editor affirme « the editor edits exactly what the game runs » : faux dès qu'un pack
de langue est dans la pile), et `WorldDemoScene` charge trois fichiers par chemin avec un
`modEnabled` câblé : une seconde définition de la pile ; six noms de layers générés
(`synthesis`, `tree-types`, `render-tuning`, `level-edits`, `terrain-gen-tuning`) connus du
code en trois endroits chacun. Le **bundle de composants acteur** est assemblé à la main dans
`StatsScene` (9 `set<>`), `CombatArenaScene` (10), en plus du Spawner (9) et du save (qui
connaît Equipment/VendorState/Bounty/FollowerState) : quatre listes, aucune garantie ;
`SaveGame::actorState` et `InventoryView::build` sondent chaque type à son tour (face `game/`
des findings Q4 « items sans base commune », « SaveState copié par type »). L'**identité
follower** (« cet acteur est un suiveur authored ») est écrite trois fois, le motif RefId →
base Form → `isA(ActorForm)` → cast six fois, la lecture `displayName` par réflexion trois
fois.

**Mécanisme existant.** `FieldLane` (`RenderTuningIo.cpp:17-38`, 22 l., **descend tel quel**
dans `engine/reflect` : aucun include ImGui ni `data/` dans le template, seules les tables
incluent les Forms) ; `WeatherController::kWeatherLanes` (une table, trois usages — la
référence) ; `drawReflectedStruct` (`TerrainGenTool` prouve qu'un pupitre réfléchi marche) ;
`registerAllFormTypes` (U8-3) ; `loadGatedPluginConfig` (la version de référence) ;
`registerCoreSpawners` (§2.7) ; `TypeInfo::isA`, `FormDatabase::typeOf`.

**Design.** `FieldLane` dans `engine/reflect/FieldLane.hpp` avec `{ label, lo, hi, Flags }`
et `drawLanes(params, lanes)` ; les ~25 widgets vraiment spéciaux restent manuels ;
`applyTuning` clampe avec `lane.lo/hi` ; `Log TOML` disparaît ; deux tables de lanes de plus
pour les 24 champs de `saveRenderTuning` et les 13 d'`atmos` ; `SaveContext` porte un
`WorldStateForm` rempli par la scène. `game::loadGameStack(dataDir, StackOptions{ language })`
(= Q1-30, T-8) appelé par les trois scènes et les outils ; `game::Layers.hpp` (nom + guid des
layers générés) ; `WorldDemoScene` passe par `loadGameStack`. `gameplay::attachActorKit
(Entity)` appelé par le Spawner, les bancs et le save (une seule liste — inter-unité Q4).
`data::formAs<T>(forms, handle)`, `data::displayNameOf(forms, handle)` dans `FormQuery.hpp` ;
`followerActorForm` = fonction libre dans `gameplay/actors/Followers.hpp`. **Risque** :
disposition du panneau (validation visuelle légère) ; des records jusqu'ici ignorés par les
démos deviennent visibles (attendu). Effort **S**/**M**.

### S7. Les helpers copiés (Q2-30, Q2-35, Q2-36, Q2-37, Q2-38, Q2-40, Q2-41)

**Pattern.** Neuf gestes canalisés portent les mêmes cinq champs `{ costPeriod, costClock,
upkeepScale, ability, emitter }` et répètent mot pour mot le cycle `held → upkeep
payAbilityCost → "spirit.refused" → endX` (×9), `endX` ×7 au même squelette ; deux variantes
non nommées (wind hold gèle le coût au relâché, rock relâche au lieu de finir). Le dispatch
`reflect::visit` des 11 kinds est écrit deux fois dans `PropertyGrid.cpp`, avec trois idiomes
de champ texte. FX : `fxSim.spawn` depuis une Form ×24, stop d'un lot ×10, graine ×4 ; le
pattern « émetteurs à portée : clé quantifiée → diff → sweep » ×3 avec la lambda `sweep`
copiée mot pour mot et une 4e orthographe de clé ; la seed cosmétique ×3 (`FxDirector` ×2,
`CombatArenaScene`) ; `CombatArenaScene::onEnter` garde un handler de cue hardcodé alors que
`FxDirector` existe. Les trois lectures copient ouverture et clôture, et la **classe
dominante** de `MaterialWeights` est calculée deux fois puis convertie en **string** pour
`classIndex`. Deux streamers de collision copiés (~120 l., `packTile`, anneau 3×3, éviction)
à côté de `render::ChunkStreamer<>` qui n'a aucune dépendance RHI ; le job de raster de carte
(`Job { params, desc, pixels, done }` + `enqueue`) ×2 entre `MapController` et
`MiniMapPanel`. UI : trois chemins de déplacement d'items (transfert, `takeAll`, barter) avec
les règles suiveur recopiées ; « charger le joueur en or » ×3 ; `% santé` ×3, arrondi ×6,
`attr("…")` résolu **par frame** ×12 ; `HudContext` recopie 5 scalaires d'état de `UiRouter`
qui relaie `GameHud` par 3 closures (les deux contrôleurs UI se parlent à travers la scène).

**Mécanisme existant.** GAS « coût = effet, upkeep périodique » (§6), `world::SpiritHold::
absorb` (geste à état headless), `FireField::kOffsets` (table), `world::readWater` (lecture
headless testée), `FxSourceEmitters`, `FxDirector`, `render::ChunkStreamer<>`,
`MapRaster.hpp`, `gameplay::transferAllItems`, `gameplay::attr()`, `payFine`.

**Design.** `struct Upkeep { tick(dt, caster, ctx) }` partagé + interface `ChanneledGesture {
update(dt, ctx); end(ctx); }` dont les 9 états dérivent (deux politiques nommées) ;
`drawValueWidget(field, value, options) -> optional<Value>` appelé par les deux grids ;
`FxSim::stopAll({…})` ou un `EmitterHandles` RAII, `spawnFormFx(guid, at, seed)` dans
`FxDirector`, `fxSeedAt(Vec3)`, `ReachEmitterSet { sync(seen, onStop) }` + `quantizedKey` ;
`showReading(kind, lines, seconds, live, cue)`, `readGround`/`readFire` headless dans
`world/spirit` à l'image de `readWater`, `MaterialWeights::dominant() → enum` ;
`ChunkStreamer` descend dans `engine/core` (T-6) et sert les deux collisions + `TerrainBake
Streamer` ; `game::AsyncMapRaster { kick, tryTake }` ; `moveItems(source, target, id, count,
TransferPolicy)`, `chargeGold(ctx, amount, refusalKey)`, `healthPercent(system)`, `AttrId`
statiques ; `UiFrame { GameHud&, UiRouter&, HudContext }` construit une fois par frame.
**Risque** : nul (mêmes appels) ; feu : A/B visuel. Effort **S** chacun, **M** pour les gestes.

### S8. Hygiène (Q2-46 … Q2-50)

~110 commentaires hors politique dans `game/scenes` (`LandscapeScene.*` 44 : M1.4, M4.1-4.4,
M5.3, E2.b-E4.b, D2b, F4, « chantier CARTES/ESPRITS/PAYSAGE », « perf audit 2026-08-06 » ;
C3 43 : D1 ×6, D2 ×4, « iso-behavior », « pre-behavior » ; followers 27 dans le seul
`FollowerController.hpp` (« Semantics (v1) », « DECISION: », « TODO(followers) ») + **34
« idiom/precedent »** qui désignent des mécanismes réels jamais nommés dans le code ;
`CombatArenaScene` 10 « Step N / Phase 8 »), deux **libellés ImGui** avec tags de brique
(`"Froxel fog (V4/H4 — off = 2D march)"`, `"Test: leg wound, 10 s (B5.5)"`, `"Drop capsule
here (B4)"`). Racine et `game/ui` : propres (6 + 5). Scories : deux `sectionMs()` mesurant
des blocs **vides** et loggés comme coûts (`publishBakedTiles:2881`), `(void)previous`,
`(void)dt`, deux indentations cassées (`NpcMovement.cpp:111,131`, `LandscapeScene.cpp:4639`),
`LandscapeTuning.hpp` shim de 7 alias, `jumpSpeed` « fallback only », bannières périmées
(`// --- Dialogue`, `// --- Console (nameplates -> GameHud)`), builtins de `ConsolePanel`
hors table, `TerrainGenTool.hpp` et `LandscapeScene.hpp` tirant le RHI via
`TerrainBakeStreamer.hpp` (= Q1-8), `castSpirit` qui **renifle le source Lua**
(`script.find("\"Water\"")`, `script.find("aim(")`) pour deviner ce qu'un sort scripté fait.
Les bancs (`CharacterStatsPanel`, `EffectPanel`) recopient des tables de noms d'enums que le
sim n'a pas (`kInjuryTypes/Parts/Sevs`, 9 tags `Status.*`, la triade onyx/amber/garnet pour
la **5e fois**) et mutent des composants directement — acceptable pour un banc, à marquer.
Perf évidente : `refreshNpcs` O(n²) (« déjà construit » par scan), disque de source et
`attr()` par frame.

---

## Bugs trouvés au passage (classe B) — aucun corrigé

| id | sév | symbole | effet | correction | vérifié |
|---|---|---|---|---|---|
| **Q2-42** | moyenne | `Keywords.cpp:32-33` vs `Condition.cpp:76-83` | Le combo du PropertyGrid ne propose que 5 kinds de condition ; le sim en enregistre 8 : `FollowerAffinityAtLeast`, `FollowerActive`, `FollowerConvalescent` sont **inaccessibles depuis l'éditeur** (une clause existante s'affiche mais toute autre valeur l'écrase) et `ConditionBuilder.cpp:60-80` n'a pas de branche pour `FollowerAffinityAtLeast` (pas de champ `value`). Même risque latent pour `AiPackageForm.kind`, `EffectForm.op/duration`. | source unique (Q2-19) ; court terme : ajouter les 3 kinds + un test qui compare `kVocabs` aux tables du sim | oui |
| **Q2-44** | moyenne | `WorldDemoScene.cpp:45-50` | Six `register*FormTypes` à la main sur les 21 de `registerAllFormTypes` : les démos 2D (`CombatArenaScene`, `StatsScene`) ignorent tout record Visual/Anim/Audio/Ui/Loc/Landscape/Spirit/Skill/Faction/Character/Follower/Ai/Furniture/Save de `base.toml`/`combat.toml` — un `CueForm` ou un `ParticleForm` n'est pas vu alors que la scène émet des cues. La dérive que U8-3 a fermée pour le cooker, rouverte pour les démos. | `registerAllFormTypes` | oui |
| Q2-43 | basse | `followNpcPath`, `NpcMovement.cpp:113-119` (et `:126-131`) | `before` contient déjà l'`airHeight` ajouté à la frame précédente ; quand `groundNpc` refuse le pas (rebord d'intérieur), la position repart de `before` **et** ré-ajoute `airHeight` : un PNJ projeté contre un rebord monte d'`airHeight` par frame tant que l'offset est non nul. Cas rare, aucun symptôme connu. | grounder `before` sans l'offset, ou n'ajouter l'offset qu'après un grounding réussi | oui |
| Q2-45 | basse | `vksmoke/Main.cpp:152, 627, 668, 693` | Quatre `check(true, "… (no validation error)")` : un PASS affiché sans condition, le compteur `failures` ne voit jamais la couche de validation. | `check` lit le compteur d'erreurs du device, ou devient `note()` | — |
| (note) | — | `LandscapeScene.cpp:4633-4640` | La commande `spirit clear` énumère 7 resets ; `applyMapWorld` et `onExit` portent-ils la même liste ? À vérifier avant Q2-2 (un `resetSpirits()` manque). | — | non |

---

## Findings

Format : `id [classe / sév] symbole (fichier)` — pattern — mécanisme à étendre — design —
effort — risque. Les sept rapports de lot (citations, lignes) sont la source.

### S — structure

- **Q2-1 [S / crit]** `LandscapeScene::update` (685 l.) — trois fils entrelacés sous 35
  gates, voyage exécuté deux fois, genre de monde re-dérivé par bloc — mécanisme : `onEnter`
  en 5 phases, `*Controller` — design : séquence nommée + `WorldKind` (Q2-20) +
  `SandboxWorldController` (Q2-3) — **L** — validation en jeu.
- **Q2-2 [S / crit]** magie des esprits dans la scène (2 840 l., 58 méthodes, 354 l.
  d'en-tête ; `SpiritDirector` propriétaire déclaré) — mécanisme : `*Context` +
  `SpiritDirector` + `world/spirit` — design : `SpiritContext`, gestes dans le director,
  `FireDirector`, `AmbientFxController`, `resetSpirits()`, commande `spirit` et `WorldActions`
  suivent — **L** (mécanique), un commit par élément — validation en jeu geste par geste.
- **Q2-3 [S / haute]** carte bornée / sandbox (~700 l., 20 membres) ; transaction « monde
  changé » ×1,5, collision ×6, `terrain-cache` ×6, bbox rivière ×3, éviction lacs ×2 —
  mécanisme : `StreamingController`, `TerrainBakeStreamer`, `mapCacheDir` — design :
  `SandboxWorldController`, `republishWorld()`, `rebuildCollision()`, `riverBounds()` — **L** /
  **S** par duplication — boot sandbox, traversée, retour story.
- **Q2-4 [S / haute]** `createRenderResources` (287 l.) : pipeline d'import de scans + 14
  GUIDs en 2 tables C++ — mécanisme : `RendererAssets`, `tuning.broadleafTreeType` — design :
  `ScatterPropForm` + `loadScatterProps` — **M** — A/B visuel rochers/fougères.
- **Q2-5 [S / haute]** `InteractionController::update` (343 l.) : scan, label, [E], [F], fade
  = 4 machines, 3 switches sur `PromptKind`, 12 closures — mécanisme : `InputAction`,
  `UiScreenForm`, bus — design : `PromptSpec[PromptKind]`, événement `OnInteract`,
  `FadeMachine` à part — **M** — ordre [E]/[F], `justMounted_` à préserver.
- **Q2-6 [S / haute]** `Npc` 54 champs / 12 booléens, 5 miroirs de tags, priorité en chaîne
  `if/else if`, « figer la pose » ×2 — mécanisme : `decidePlayerAction`, `updateInterruption`
  — design : `NpcLife` + `NpcActivity` + `decideNpcActivity` pur, `syncMirrors()` en table —
  **M** — village en jeu ; la décision se teste.
- **Q2-7 [S / haute]** `NpcDirector.hpp` hub (13 TU, RHI + anim, `npcs()` mutable, 4 cycles)
  — mécanisme : `npcByEntity_`, `findNpc`, `factionOf` — design : `Npc` = sim, `NpcGpu` à
  part, `NpcRegistry` + commandes nommées, `teleportNear` → `NpcMovement` — **M** — sans effet.
- **Q2-8 [S / haute]** `updateSpiritFire` (200 l.) + ~900 l. de présentation du feu autour
  d'un `SpiritDirector::updateFire` de 108 l. — design : `FireDirector`, `placeFlames()` pur,
  `FlameEmitterSet::sync` — **M** — A/B feu (`docs/FIRE-RENDER.md`).
- **Q2-9 [S / moyenne]** warmup : 5 phases + 14 scalaires, boot override consommé dans
  `Reveal`, veil RmlUi, `enterPlayMode` lit la phase — design : `WarmupController` +
  `onSpawnPlaced`/`onRevealed` — **M** — boot et traversées.
- **Q2-10 [S / moyenne]** `makeInteractionContext` : 12 closures dont 5 actions de 20-40 l.
  — design : méthodes nommées ou descente dans `InteractionController` — **S** — nul.
- **Q2-11 [S / moyenne]** `NpcCombatController::update` (258 l.) : cible en 2 branches,
  `combatStyle == "healer"` ×2 ; `NpcDirector::update` (270 l.) phases implicites — design :
  `resolveCombatTarget()`, `CombatStyle` enum + table dans `chooseCombatMove`, 8 appels nommés
  — **M** — Maela/Aldric validés → A/B.
- **Q2-12 [S / moyenne]** `FollowerController` : 39 méthodes / 3 TU / 3 métiers ; 13
  lambdas d'abonnement identiques dans `setupGameplay` ; `updateFollowers` 5 soucis par PNJ
  avec `continue` qui court-circuitent — mécanisme : `subscribeAll` + filtrage data
  (« the director filters, not the wiring ») — design : table `{ événement → méthode }`,
  trois classes ou un seul `subscribeAll`, `FollowerView` résolu une fois — **M** — plomberie.
- **Q2-13 [S / moyenne]** `SceneEditor::draw` (323 l.) : gizmo, sculpt, placement, pick,
  fenêtre ; « spawn depuis un draft » ×3 ; guid/chemin d'export en dur — design : `placeAt`,
  `spawnDraft`, `drawSessionBar/SelectionOps/Palette` (catégories du registre) — **S-M**.
- **Q2-14 [S / moyenne]** `MapController::open` (152 l.) ; `cookTerrainMaterials` (227 l.) :
  4 canaux inline, 3 boucles de moyenne dont une ignore `channelMean()` — design :
  `extentFor()`/`doorPois()` ; `ChannelPlan[4]` — **S** / **M** — `.mtex` = cache livré :
  hasher avant/après.
- **Q2-15 [S / basse]** `TreeCreationScene` inclut `LandscapeScene.hpp` pour `replace()` ;
  `EditorScene` possède **sa propre pile** même en overlay au-dessus du jeu (deux
  `FormDatabase` incohérentes pendant l'overlay) — design : closure « back » ou
  `SceneStack::replaceWith<T>` ; `EditorScene(engine, SharedSession&)` = le contrat
  `EditorContext` de la vision « 3 modes » — **S** / **M**.

### G — généraliser

- **Q2-16 [G / haute]** matrice des sorts ×3 (`spellSupported`, `executeSpell`,
  `applyPendingSpiritActions`), 20 cellules → 16 `Mode` → 13 blocs `if`, 4 cellules hors
  file — mécanisme : `Spawner::byCategory`, `spellSupported` — design : registre
  `(trajectoire, élément, verbe, canalisé) → { supported, makeAction, begin }` — **M** —
  nul en logique, validation en jeu.
- **Q2-17 [G / haute]** mobilier : `category` en strings (`"grave"|"mount"|"container"|
  "lever"`), `"bed" → 8 h`, `furnitureCategoryIs` ×4 par entité par frame — mécanisme :
  `FurnitureForm.screen` — design : `restHours`, `interaction` sur la Form (table
  `static_assert`-ée), compat `interaction` vide → déduit de `category` — **S** + données.
- **Q2-18 [G / haute]** kinds de packages IA en strings sans registre ; `combatStyle` string ;
  `MarkerKind.kind == "patrol"` ; `"State." + animTag` concaténé — mécanisme :
  `parseCombatMove`/`combatMoveName`, `Spawner::byCategory` — design : `PackageKind` +
  `parse/name` + `kPackageTraits` **dans `gameplay/ai`**, `switch` plat côté game ;
  `CombatStyle` — **S-M** — village en jeu, parse testé.
- **Q2-19 [G / haute]** 4 tables « (type, champ) → widget » en strings + chaîne de `if` dans
  `PropertyGrid` ; `Keywords.hpp` demande lui-même la promotion — mécanisme : `FieldInfo.flags`,
  `kClauseEvaluators` — design : `FieldMeta` à côté de `REFLECT_FIELD` (**T-3**) ou table
  unique keyée `(TypeInfo::id, FieldInfo::id)` — **M** / **S** — UI dev.
- **Q2-20 [G / haute]** genre de monde = 3 booléens en 63 sites — mécanisme :
  `StreamingContext` — design : `WorldKind` + `WorldPolicy` décidés en un endroit — **M** —
  smoke des trois genres.
- **Q2-21 [G / moyenne]** `UiRouter` 7 × 13 événements + 14 actions en strings, `wait1/4/8`,
  `Category/Column` sans table ; `OptionsController` un `if` par réglage ; 13 modèles UI en
  listes de strings ; `Settings` non réfléchi (TOML à la main ×2) — mécanisme :
  `ConsolePanel`, `OptionsController::handleEvent`, `UiScreenForm` — design : table
  `{ model, event, handler }` enregistrée par contrôleur, `wait` + arg, tables de noms,
  `Stepper[]`, `REFLECT_FIELD` sur `Settings`, `constexpr` partagés — **M** — `.rml` à toucher.
- **Q2-22 [G / moyenne]** hotkeys modales ×5 avec `top->name == "…"` (14 noms d'écrans
  littéraux) ; 7 hotkeys dev en chaîne, F5/F9 hors `ActionMap`, aide tapée ×3 — mécanisme :
  `ScreenStack`/`UiScreenForm`, `ActionMap` — design : `ScreenHotkey[]`, `InputAction::
  QuickSave/QuickLoad`, `kDevHotkeys[]` — **S** — navigation UI.
- **Q2-23 [G / moyenne]** `hostile`/`guard` par `tag == "Faction.Bandits"` /
  `"Faction.VillageGuard"` — mécanisme : `FactionRelationForm` + `FactionStanding` (§6.1) —
  design : `standing() == Enemy` au build, `guard` = trait de faction — **S** — A/B bandit/archer.
- **Q2-24 [G / moyenne]** `findByEditorId` en dur : 11 + 2 (scène), `"Grave"`,
  `"SpiritWaterJet"`, 6 (`DungeonGenTool`), `"EasternMenace"` (quête câblée dans un directeur
  générique), 14 GUIDs (`CombatArenaScene`) — mécanisme : `startWorldspace` (GUID + fallback
  déprécié + warn), `StatsTuningForm` (U4-7), `QuestStateForm.kind` — design :
  `GameplayLinksForm`, `DungeonKitForm`, `ArenaBenchForm`, kind `"turnin"` ; `TestLegWound`,
  `kDevStartInterior` à retirer — **S** — aucun si mêmes records.
- **Q2-25 [G / moyenne]** colliders de végétation par **plages d'index de variante**
  (`kFirstRock..kFirstBush`, `kFirstDebris + 1 == log`) — mécanisme : table des variantes de
  `VegetationSystem` — design : colonne `Collider { shape, half }` — **S** — mêmes corps.
- **Q2-26 [G / moyenne]** `EditorScene` : hiérarchie/conditions par 26 comparaisons de
  type-id à côté de `kCategories` ; dispatch par **nom de type** ×15 dans `game/ui` vs `isA`
  — design : colonnes `hierarchy`, `conditionsParentField` (compatible U5-4) ; tables keyées
  sur `TypeInfo::id` — **S-M** — UI dev.
- **Q2-27 [G / moyenne]** trois graph panels : ~250 l. dupliquées (actions → session,
  popups, squelette de nœud) ; `NodeCanvas.hpp` assume « no mega-abstraction » — design :
  `GraphGrammar` + `applyCanvasActions`/`drawGraphPopups` dans `GraphPanelCommon`, chaque
  panneau garde le domaine — **M** — UI dev.
- **Q2-28 [G / moyenne]** `restoreMode` contourné en 7 sites ; deux chemins d'entrée en jeu
  (`MEADOWS_BOOT` recopie « verbatim » `play-story/sandbox`) — design : `onEnter` finit par
  `restoreMode(savedMode)`, `BootMode` + `beginPlay()` — **S** — save en 3 modes, smoke boot.
- **Q2-29 [G / basse]** fade = `i32 fadeDirection` + 2 « pending » exclusifs ; `i32
  brushKind` + switch 0..3 alors que `BrushKind` existe ; `castSpirit` renifle le source Lua
  — design : `FadePhase` + variant, `BrushKind`, champs `needsWaterSim`/`aimed` sur
  `AbilityForm` — **S**.

### D — duplication

- **Q2-30 [D / haute]** 9 gestes canalisés = 9 copies du cycle upkeep (`payAbilityCost` ×9,
  `"spirit.refused"` ×13, `endX` ×7) — mécanisme : GAS §6, `SpiritHold::absorb` — design :
  `Upkeep` + `ChanneledGesture { update, end }`, deux politiques nommées — **M** — ordre
  d'`Upkeep::tick` à reproduire.
- **Q2-31 [D / haute]** identité follower ×3 (`followerActorForm`, `followerActor`,
  `livingFollower`), RefId → ActorForm ×6, `displayName` réfléchi ×3 — mécanisme : `typeOf`
  + `isA` — design : `data::formAs<T>`, `data::displayNameOf`, `followerActorForm` libre
  dans `gameplay/actors` — **S** — nul.
- **Q2-32 [D / haute]** `RenderTuningPanels` 190 widgets à la main (chaque knob ×4), `Log
  TOML` 60 champs ; `saveRenderTuning` 24 champs à côté de `captureTuning` ; `atmos = tuning`
  ×13 ; `WorldStateForm` ×14 — mécanisme : `FieldLane`, `kWeatherLanes`, `drawReflectedStruct`
  — design : `FieldLane` dans `engine/reflect` avec `{ label, lo, hi }` + `drawLanes()`,
  tables de lanes supplémentaires (**T-1**) — **M** — disposition du panneau.
- **Q2-33 [D / haute]** bootstrap pile ×2 scènes sans gate de langue (« the editor edits
  exactly what the game runs » : faux) ; `WorldDemoScene` = 2e définition de la pile ; 6 noms
  de layers générés ×3 sites — mécanisme : `registerAllFormTypes`, `loadGatedPluginConfig` —
  design : `game::loadGameStack` (**T-8**, = Q1-30), `game::Layers.hpp` — **S** — aucun.
- **Q2-34 [D / moyenne]** assemblage GAS : `StatBlock` ×11, self-cast ×6, lecture de tag ×12,
  frappe ×3 + balayage ×3, flèche ×2, dégâts non mitigés ×2, drains ×4, `faceToward` ×6,
  `findPath` + reset ×5 — mécanisme : `StatBlock`, `syncStateTag`, `resolveMeleeStrike`,
  `NpcMovement` — design : `statBlockOf`, `tryActivateSelf`, `hasStateTag` + constantes,
  `resolveStrikeBetween` + `sweepActors`, `looseArrow`, `applyUnmitigatedDamage`,
  `DrainChannel[]`, `faceToward`, `requestPath` — **S** chacun — sans effet.
- **Q2-35 [D / moyenne]** dispatch `visit` des 11 kinds ×2 dans `PropertyGrid`, 3 idiomes de
  champ texte — design : `drawValueWidget()` — **S** — nul.
- **Q2-36 [D / moyenne]** FX : `spawn` depuis Form ×24, stop de lot ×10, graine ×4, « émetteurs
  à portée » ×3 (`sweep` copiée mot pour mot), seed cosmétique ×3, handler de cue hardcodé
  dans `CombatArenaScene` vs `FxDirector`, `RiverSourceFxForm` re-cherché par frame ×2 —
  design : `EmitterHandles` RAII / `stopAll`, `spawnFormFx`, `fxSeedAt`, `ReachEmitterSet`,
  `fx::cosmeticSeed`, `FxDirector` dans l'arène — **S** — nul.
- **Q2-37 [D / moyenne]** trois lectures : ouverture/clôture ×3, `num` ×3, `ReadingKind`
  re-switché, classe dominante ×2 → **string** → `classIndex` — mécanisme : `world::readWater`
  — design : `showReading()`, `readGround`/`readFire` headless, `MaterialWeights::dominant()
  → enum` — **S**.
- **Q2-38 [D / moyenne]** deux streamers de collision copiés (~120 l.) vs
  `render::ChunkStreamer<>` sans RHI ; job de raster de carte ×2 — design : `ChunkStreamer`
  → `engine/core` (**T-6**), `game::AsyncMapRaster` — **S-M** — mêmes corps (`bodyCount`).
- **Q2-39 [D / moyenne]** bundle acteur à la main ×2 bancs + Spawner + save (4 listes
  divergentes) ; `SaveGame::actorState` chaîne par type ×5 ; `InventoryView::build` sonde 4
  types et énumère 5 slots — mécanisme : `registerCoreSpawners` (§2.7) — design :
  `gameplay::attachActorKit(Entity)` ; parent `ItemForm` réfléchi (inter-unité **Q4**) — **S**.
- **Q2-40 [D / moyenne]** transfert d'items ×3, `chargeGold` ×3 (+ `payFine`), `% santé` ×3,
  arrondi ×6, `attr("…")` par frame ×12, horloge HH:MM ×2, journal en double `forEach` alors
  que `quest/` a l'index, `HudContext` recopie 5 scalaires d'`UiRouter` / `UiRouter` relaie
  `GameHud` par 3 closures — design : `moveItems(…, TransferPolicy)`, `chargeGold`,
  `healthPercent`, `AttrId` statiques, `formatClock`, `UiFrame` — **S** — nul.
- **Q2-41 [D / basse]** petites duplications, chacune **S** : (a) focus ×8 → `focusPosition()` ;
  (b) `wireSpecies` ×2 ; (c) `spawnBody(+0.25)` ×4 ; (d) `groundAt` vs `height + 0.5f`
  (`RideController::dismount` poserait en intérieur sur le height field extérieur) ;
  (e) `EventPicker` : `kEventFields` balayée en 3 idiomes, 2 popups miroir, builtins recopiés ;
  (f) `fireCellAt` ×3 (= Q1-26) ; (g) `collides` réfléchi ×2, snap ×2 ; (h) `AnimPreviewPanel`
  3e hash réfléchi (**T-2**), `GraphInstance` ×2, layout `SkinnedVertex` à la main ;
  (i) `SceneLight` ×2 ; (j) `TreeCreationScene::createRecord` vs `diffToRecord` ;
  (k) `WeaponMeshes` 5 guids + whitelist « in sync » ; (l) `SpiritDirector` 10 tableaux FX
  parallèles → `SpiritLook` ; (m) `MapView` = 4e `mapGrid`, 9e `tile_%d`, 3e `Lake→Surface`
  (Q1-33/34/35 +1), `MapView::wet` 3e oracle « mouillé » ; (n) asset DB inline ×2 →
  `buildAssetDatabase` ; (o) `cooker --patch` exige `argc == 8` non documenté, `terrainPad`
  hors pile (= Q1-18), `readFileBytes` locaux ; (p) `findField("…")->id` nu ×6 dans `SaveGame`
  alors que `LevelEditor::fieldId` est null-safe (= Q1-38, **T-9**).

### H — hygiène

- **Q2-46 [H / moyenne]** ~110 commentaires hors politique dans `game/scenes` (détail §S8),
  34 « idiom/precedent » sans helper nommé, `FollowerController.hpp` = journal de conception
  (→ `docs/FOLLOWERS.md`), 3 libellés ImGui avec tags de brique — **S** par fichier.
- **Q2-47 [H / moyenne]** en-têtes lourds : `LandscapeScene.hpp` 76 includes (RHI,
  `WorldRenderer`, `TerrainBakeStreamer` → RHI), `NpcDirector.hpp` RHI + anim pour 13 TU,
  `TerrainGenTool.hpp` → RHI — design : forward-déclarer `TerrainBakeStreamer`, `NpcGpu`
  (Q2-7), `.twb` hors du streamer (Q1-8) — **S**.
- **Q2-48 [H / basse]** scories : 2 `sectionMs()` vides loggés comme coûts, `(void)previous`,
  `(void)dt`, indentations cassées ×3, `LandscapeTuning.hpp` shim, `jumpSpeed` fallback,
  bannières périmées ×2, builtins `ConsolePanel` hors table, `kDevStartInterior` + GUID,
  `TestLegWound` — **S**.
- **Q2-49 [H / basse]** bancs : tables de noms d'enums du sim recopiées dans l'UI
  (`kInjuryTypes/Parts/Sevs`, 9 `Status.*`, triade onyx/amber/garnet **5e copie**), mutations
  directes de composants (hors §2.9, acceptable pour un banc) — design : tables de noms côté
  sim (**T-4**), marquer `// bench` — **S**.

### P — optimisation évidente

- **Q2-50 [P / basse]** `refreshNpcs` « déjà construit » en O(n²) (`npcByEntity` reconstruit
  à la fin) ; scan linéaire des PNJ par entité visée (`npcByEntity` absent du contexte) ;
  `attr("…")` hachant une string par frame ×12 ; `RiverSourceFxForm` par `forEach` par frame —
  **S** — sans effet.

---

## Positifs à préserver

- **Racine `game/`** : `SceneStack` (ops différées, `host()` — la base de la vision « 3
  modes »), `main.cpp` (table), `InputActions` (enum + `Binding`, rebind qui vole, `parse`
  dérivé de `name`), `SaveGame` (atomique, pur, `SaveFlightGate` doctestée, un save = un
  plugin), `LevelEditor` (`fieldId()` null-safe, `Gesture` = un Ctrl+Z), `SceneSubmit`
  (extract/submit séparés, tri stable), `MapRaster` (`mapUv` unique, palette `[cpp-tuning]`
  groupée), `SoundResolver` (`childrenOf`, RNG hors gameplay).
- **Contrôleurs modèles** : `StreamingController` (contexte plat, fixups nommés),
  `WeatherController::kWeatherLanes` (une table, trois usages — **la référence T-1**),
  `SaveController` (single-flight, `shared_ptr` jamais `this`), `FollowerCombat` (rôles
  résolus une fois, `adoptOnHit` pur), `RideController`, `OptionsController::decideCapture`.
- **Décisions pures** : `decidePlayerAction` (un arbitre, une sortie — le modèle pour
  `NpcActivity`), `chooseCombatMove` + brain Lua qui ne fait que remplacer le move,
  `resolveMeleeStrike` (partagé joueur/PNJ/flèche), `updateInterruption`, les 17 fonctions
  pures de `gameplay::Followers`.
- **Dans la scène** : `onEnter` en 5 phases nommées (le modèle pour `update`), `restoreMode`
  en deux switches plats (à rendre exclusif), `loadGatedPluginConfig` (la référence du
  bootstrap), `startWorldspace` (GUID + fallback déprécié + warn — le modèle pour Q2-24),
  `subscribeAll` + filtrage data, `throwActorsOnGroundRise` (un mécanisme à un endroit),
  `pendingSpiritActions`/`pendingMapTravel` (le point sûr), `publishBakedTiles` en une
  transaction, `saveRenderTuning` écrit un plugin §5 ordinaire.
- **UI dev** : `PropertyGrid` (« adding a FieldKind without a widget is a compile error »),
  `ConsolePanel` (table + `get/set` réfléchis), `NodeCanvas` (`Actions` consommées après la
  frame, gates §5), `TerrainGenTool::drawPupitre` (pupitre = `drawReflectedStruct`),
  `TestActor`, `OverlayBar`, `AnimPreviewPanel` (caches négatifs, fingerprint réfléchi,
  `rhi::Unique`), `FxPanel`/`ClipTimelinePanel` (un `setField` au relâché).
- `SpiritDirector::updateFire` (job 10 Hz, epoch, copies-valeur, `tryPop` au point sûr),
  `fireTouch` (buildup + dégât typé = §2.9), upkeep des gestes par `payAbilityCost` (§6),
  `TerrainCollision`/`MapController`/`MiniMapPanel` (mailbox Phase 5 correcte).

---

## Ordre de correction proposé (à arbitrer)

Un commit par brique ; `game/` n'a pas de couverture headless : build Release, smoke
(`MEADOWS_BOOT=story|sandbox`), puis validation en jeu par le dev avant commit (règle de
juillet).

1. **Bugs et plomberie sans risque** : Q2-42 (3 kinds + test), Q2-44 (`registerAllFormTypes`),
   Q2-43, Q2-33 (`loadGameStack` = Q1-30 ; les deux scènes suivent), Q2-31 (`formAs`,
   `displayNameOf`), Q2-34 (helpers GAS un par commit), Q2-35, Q2-36, Q2-40, Q2-41 (a-p).
2. **Les modes et le genre de monde** : Q2-28 (`restoreMode` exclusif, `BootMode`), puis
   Q2-20 (`WorldKind` + `WorldPolicy` sur `StreamingContext`) — préalable à Q2-1.
3. **Sortir les trois sous-systèmes de la scène**, dans cet ordre : Q2-9 `WarmupController`
   (le plus petit), Q2-3 `SandboxWorldController`, puis Q2-2 esprits par élément (Eau, Terre,
   Feu avec Q2-8, Vent) **après** Q2-30 (`ChanneledGesture`) et Q2-16 (registre de sorts) qui
   réduisent ce qu'il y a à déplacer ; Q2-1 `update()` en séquence nommée en clôture.
4. **Traits et tables** : Q2-17 mobilier (données à migrer), Q2-18 packages IA + `CombatStyle`,
   Q2-23 factions, Q2-24 `GameplayLinksForm`, Q2-25 colliders, Q2-5 + Q2-10 interaction (après
   Q2-17), Q2-21 + Q2-22 UI (avec les `.rml`).
5. **PNJ** : Q2-7 (`NpcGpu`, `NpcRegistry` — sans effet), puis Q2-6 (`decideNpcActivity`,
   testé) et Q2-11, Q2-12 followers.
6. **Éditeur et panneaux** (dépend de la décision **T-3** : kind Enum / `FieldMeta`) : Q2-32
   (`FieldLane` dans `engine/reflect`, le plus gros gain mécanique partagé avec Q1-22), Q2-19,
   Q2-26, Q2-27, Q2-13, Q2-15.
7. **Hygiène** au fil de l'eau : Q2-46 par fichier touché, Q2-47, Q2-48, Q2-49, Q2-50, Q2-14.
