# Q5 — reste d'`engine/` : racine, core, reflect, ecs, platform, nav, assets, anim, physics, fx, audio, ui, dungeon

> Vague Q5 de la revue de qualité d'octobre 2026 (`README.md` : méthode, classes
> S/G/D/B/H/P, arbitrages de juillet à ne pas re-proposer). Relecture **intégrale** du
> 2026-10-23 sur HEAD `61efb65` : racine `engine/` + `core/` + `reflect/` + `ecs/` +
> `platform/` + `nav/` (41 fichiers, 3 084 l.), `assets/` + `anim/` + `physics/` + `fx/` +
> `audio/` (26 f, 3 519 l.), `ui/` + `dungeon/` (16 f, 3 920 l.) — soit ~10 500 lignes, trois
> lots en parallèle (F1, F2, F3). `engine/render`, `engine/rhi` (Q3) et `engine/terrain` (Q1)
> sont hors périmètre. **Aucun code modifié.** Lignes = `61efb65` ; les symboles font foi.

---

## Verdict

**Le socle est propre.** Les trois lots le disent chacun à leur manière : `core/`/`reflect/`/
`platform/` est « toujours l'unité la plus saine » (tout ce que juillet a demandé est livré :
`Result`, `Clock`, verrou d'ordinal, registre assertif, `JobCounter` assertif, `hashU32`
hoisté ; aucune fonction > 80 l., aucune chaîne `if` de cas particuliers, 3 écarts de
commentaires sur 3 084 l.), `assets`/`anim`/`physics`/`fx`/`audio` est « le lot le plus sain
de la série » (cinq façades pimpl exactes, aucun type Jolt/miniaudio/cgltf dans un `.hpp`,
zéro include `data/`, 1 écart sur 3 519 l.), et `engine/dungeon` est « le code le plus propre
lu dans la série » (pipeline D1→D7 d'étages purs, `core::Rng`, enums en switch plats, zéro tag
de journal, 8 doctests). `ImGuiLayer` a été **réécrit sur le RHI** : l'exception « ImGui hors
RHI » de juillet (U2-07/U8-5) n'a plus d'objet et doit être close.

Ce lot porte en revanche **trois des décisions transverses de toute la série**, parce que les
mécanismes qui manquent aux quatre autres vagues vivent ici :

1. **T-3, la réflexion** (Q5-3) : `FieldKind` = 11 kinds scalaires, `KindOf<F>` **refuse un
   `enum class` à la compilation** (d'où les champs `str kind` et `u32 id` partout), `FieldFlags`
   n'a qu'un bit, pas de `FieldMeta`. Mais `TypeInfo::isA` et `parent` **existent** (10 sites) :
   le « parent réfléchi » que Q4 proposait est déjà là. Deux extensions compatibles avec H-a
   sont instruites — (B) `FieldMeta`/`EnumTable` optionnelle sur `FieldInfo`, zéro effet
   persistance ; (A) kind `Enum` appendu, binaire = ordinal, TOML = nom, le build casse jusqu'à
   ce que chaque consommateur `visit` ait son corps. Décision dev §2.3/§5.
2. **T-2, le hash** (Q5-10) : `core::fnv1a` n'existe qu'en **32 bits** ; 13 copies u64 dans
   12 fichiers (terrain, Vulkan, tests) portent le basis tronqué `…603` (seule `MapGrid.cpp:16`
   est juste) → `fnv1a64` + `Fnv64` + `reflect::hashReflected`, ~40 lignes.
3. **T-13 vu du socle** (Q5-5) : le pont `ecs::World::registerComponent → reflectedComponent`
   (« drives generic component serialization for saves ») a **22 enregistrements et 0 appelant**
   hors test ; les saves recopient par type — c'est la cause du bundle acteur en trois listes
   (Q4-2).

Les cycles de couches sont **tous** localisés et petits : `engine/(root) ↔ render` tient à
`Engine` qui crée `SpriteRenderer` + `Camera2D` (Q3-13) ; `render → ui` tient à
`WorldRenderer.hpp:123 ::ui::UiSystem* gameUi` (la moitié `ui → render` par `ShaderLibrary&`
est saine) ; `assets → anim` tient à **une** inclusion (`GltfMesh.hpp:6`). Le script
`layers_audit.py` n'est branché nulle part et range `world` sous `gameplay` (15 UPWARD
fantômes).

Le reste est petit et bit-exact : la boucle de sommets glTF écrite deux fois, les règles de
marche écrites deux fois entre le bake de donjon et le navigateur, sept vecteurs d'ancres
parallèles que chaque consommateur parcourt famille par famille, 22 `destroy*` manuels dans
`engine/ui`, trois booléens pour un kind de blend, le bus audio en string. Trois bugs latents
(sentinelle `BodyId = 0` qui peut être un corps Jolt valide, unités fenêtre/souris divergentes
sur HiDPI, bind groups ImGui jamais purgés).

---

## Contrôles d'invariants

| Invariant | Verdict | Remarque |
|---|---|---|
| §3.1 : headers platform-clean, SDL confiné à `platform/common/*.cpp` | **PASS** | pimpl `Window::Impl`/`GlContext::Impl`, `void*` opaques ; `macos/Activation.mm` = premier fichier per-OS, branché par CMake (CLAUDE.md §4 « only common/ » périmé) |
| §3 : pimpl, aucun type Jolt/miniaudio/cgltf dans un `.hpp` | **PASS** | grep `JPH::|Jolt/` sur tous les `.hpp` du dépôt : vide ; `ma_*` en commentaire seulement |
| §4 : `engine/*` n'inclut jamais `data/*`, `world/*`, `game/*` | **PASS** | grep vide sur tout `engine/` |
| §2.1 : aucun GL/VK direct dans `engine/ui` | **PASS** | `ImGuiLayer` sur le RHI (push constants, scissor) ; `imgui_impl_sdl3` = moitié plateforme seule |
| U2-07 / U8-5 « ImGui hors RHI = exception acceptée » | **obsolète** | plus d'`imgui_impl_opengl3` — à clore au README §0.2 |
| §2.1 : backend choisi au runtime | PASS | `Engine.cpp:49-71` chaîne Vulkan→GL avec recréation de fenêtre, commentée |
| §2.3 : clé de voûte | PASS (forme) / **lacune** (fond) | triple ordinal verrouillé (U1-01), registre assertif (U1-02), `isA` présent ; pas d'Enum, pas de meta (Q5-3) |
| §2.10 : qui inclut `engine/platform` hors `engine/`+`game/` | PASS | tests et `vksmoke` seulement |
| §7 residency | PASS | loaders synchrones par contrat, `ResidencyCache` (Q3) les appelle hors thread principal |
| §8 RAII, pas d'exceptions en hot path | PASS | `uptr` partout ; `try/catch` seulement dans `valueFromString` (chemin froid) ; `new JPH::*` = contrat refcounté Jolt |
| §8 déterminisme | PASS | `core::Rng` seedable ; `Guid::generate` sur un `mt19937_64` séparé ; `VertexAo` spirale dorée ; `ParticleSim` par seed ; `MissionGraph`/`SpaceGraph`/`populateRng` par seed ; aucun `rand/random_device` dans la sim |
| Phase 5 : jamais `JobSystem::wait()` sur le frame thread | **PASS (par absence)** | zéro appelant de `wait` ; `requestStop()` avant teardown ; `ConcurrentQueue::drain` hors verrou |
| `rhi::Unique` (U3-7) | **FAIL** (ui) | 22 `destroy*` manuels (Q5-13) ; la série passe à 12 classes / 129 appels |
| Hot-reload une façon | partiel | compteur `UiSystem` (déjà compté en Q3-15) ; `ImGuiLayer` sans reload (shader embarqué) |
| Couches (`ARCHITECTURE-LAYERS.md`) | **FAIL connu, localisé** | `engine/(root) ↔ render` (Q3-13), `render → ui` (Q5-1), `assets → anim` une inclusion (Q5-1) ; script non branché, rang de `world` faux (Q5-8) |
| Règle dev « enum over nested ifs » | PASS | `Key`/`MouseButton`/`PadButton`/`FieldKind`, `NodeKind`/`EdgeKind`/`CyclePattern` en switch plats ; écarts : blend en 3 bools, bus en string (Q5-7) |
| Politique de commentaires 2026-07-22 | **PASS** | 3 + 1 + 5 écarts sur 10 500 l. ; plans dans `Nav.hpp` et `UiSystem.hpp` (périmé) |
| Pas de finding perf sans mesure | respecté | double parse glTF, `EventBus`, `std::function` du graphe anim notés sans classe P ; DUNGEON-GEN §7 « on ne pré-optimise pas » |
| Arbitrages de juillet | respectés | H-a non re-proposée (Q5-3 instruit des *données*, pas des corps) ; U1-07/U1-10 non rouverts ; U1-03/04/05/06/08 vérifiés livrés ; `Handle<Tag>` non re-proposé |

---

## Passe 1 — fichier par fichier

### Racine `engine/`, `core/`, `reflect/`, `ecs/`, `platform/`, `nav/` (lot F1)

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `Engine.{hpp,cpp}`, `FrameContext.hpp`, `Game.{hpp,cpp}` | 323 | fenêtre, device (chaîne Vulkan→GL), boucle ; **possède `SpriteRenderer` + `Camera2D`** | propre ; cycle de couche | Q5-1 (= Q3-13) |
| `core/Hash.hpp` | 45 | `fnv1a` **u32**, `hashU32`, `HashRng` | **incomplet** (pas de 64 bits) | Q5-10 |
| `core/Jobs.{hpp,cpp}` | 180 | pool, `JobCounter`, `isStopping/requestStop`, `JobProbe` | propre ; `JobCounter`/`wait` sans client | Q5-18 |
| `core/ConcurrentQueue.hpp`, `Result.hpp`, `Rng.hpp`, `Clock.hpp`, `Guid`, `Bezier`, `FrameProbe`, `JobProbe`, `SettleTimer`, `Assert`, `Log` | 30-137 | les primitives de juillet, toutes livrées | **propres — modèles** | Q5-18 (`Log::init`), Q5-19 |
| `reflect/Reflect.hpp` | 237 | `FieldKind` (11), `Value`, `FieldInfo` (`get/set` `std::function`, `flags` 1 bit), `TypeInfo` (`parent`, **`isA`**, `findField`), builder, macros | propre ; **sans Enum ni meta** | **Q5-3** |
| `reflect/Registry`, `ValueText`, `Visit.hpp` | 39-106 | registre assertif, codec texte (parsing laxiste), `visit` exhaustif (H-a) | propre / **modèle** (`Visit.hpp`) | Q5-18 |
| `ecs/World.{hpp,cpp}` | 80 | flecs confiné ; pont composant ↔ réflexion **sans client** | propre | Q5-5 |
| `platform/Window`, `Input`, `GlContext`, `VulkanSurface`, `Paths`, `macos/Activation.mm` | 42-474 | SDL3 pimpl ; `Input` = vocabulaire fermé en enums + tables de scancodes (switch plats) ; vsync en dur | **propre — modèle** (`Input`) | Q5-9, Q5-16 (B) |
| `nav/Nav.hpp` | 40 | interface `Navigator` ; « Planned extension points » dans l'en-tête | propre | Q5-19 |

Fonctions > 80 l. : **aucune**.

### `assets/`, `anim/`, `physics/`, `fx/`, `audio/` (lot F2)

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `assets/AssetDatabase` | 68 | VFS GUID → chemin, dernier écrivain gagne ; **une** couche | **propre** | — |
| `assets/GltfMesh.{hpp,cpp}` | 679 | parse cgltf, mesh/skinned/squelette/clips ; **inclut `engine/anim/Anim.hpp`** ; boucle de sommets ×2, nœuds ×2, AABB ×2, double parse | à surveiller | Q5-1, Q5-11 |
| `assets/CookedMesh`, `CookedTexture`, `MeshData`, `MeshSimplify`, `Image` | 49-194 | `.cmesh`, `.mtex` (2 switches miroirs `formatCode`), paliers de simplification en dur | propres | Q5-7, Q5-14, Q5-18 |
| `assets/VertexAo`, `VertexAoCache` | 145+297 | AO déterministe, cache disque `.ao` ; FNV à la main, commentaire « fnv1a64 » faux | propre | Q5-10, Q5-14 |
| `anim/Anim.{hpp,cpp}` | 421 | squelette, clips, `GraphInstance` avec conditions par **callbacks** | **propre — modèle** | — |
| `physics/Physics.{hpp,cpp}` | 639 | façade Jolt, `CharacterVirtual` ; `BodyId = 0` sentinelle ; plomberie ×4/×3/×2 | propre | Q5-15 (B), Q5-14, Q5-18 |
| `fx/Particles.{hpp,cpp}` | 359 | sim CPU par seed, budget global ; `additive/flame/haze` = 3 bools | propre | Q5-7 |
| `audio/Audio.{hpp,cpp}` | 384 | miniaudio, 5 bus **en string**, backend null | propre | Q5-7 |

Fonctions > 80 l. : `buildSkinnedMesh` ~105, `loadGltfAnimations` ~85.

### `ui/`, `dungeon/` (lot F3)

| fichier | l. | rôle | verdict | findings |
|---|---|---|---|---|
| `ui/UiSystem.{hpp,cpp}` | 1 111 | façade RmlUi (aucun type Rml exposé), 3 interfaces, VFS « dernier gagnant » ; 13 `destroy*`, compteur de génération, racines copiées ×2, `rmlKeyFor` 34 `case` sans garde | sain | Q5-1, Q5-7, Q5-13, Q5-14, Q5-19 |
| `ui/ImGuiLayer.{hpp,cpp}` | 402 | dev UI **sur le RHI** ; shader embarqué `#ifdef VULKAN` à la main ; 9 `destroy*` ; `textureGroups` jamais purgé | **propre** (§2.1 PASS) | Q5-13, Q5-17 (B) |
| `dungeon/MissionGraph` | 414 | D1 : cycles, greffes, `isSolvable`, `toDot` ; enum + switch plat « adding one = one function » | **propre — modèle** | Q5-6 (traits `EdgeKind`) |
| `dungeon/SpaceGraph` | 560 | D2 : embedding, `routeCorridor` 155 l., `tryEmbed` 120 l. | propre, lisible | Q5-2 |
| `dungeon/DensityField`, `MeshExtract`, `NavGrid` | 262, 231, 287 | D3-D5 : SDF, surface nets (`extractChunkMesh` 180 l.), grille CSR + `.nvg` maison | propres | Q5-2, Q5-12, Q5-14 |
| `dungeon/DungeonBake.{hpp,cpp}` | 653 | `bakeDungeon` 150 l. en 5 phases ; `populateAnchors` **212 l.** ; `navValidates` 124 l. (BFS + CSR inline) ; **sept vecteurs d'ancres** ; ~15 constantes de mise en scène | **à refondre** (ancres, populate) | Q5-2, Q5-4, Q5-6, Q5-12 |

---

## Passe 2 — système par système

### S1. La réflexion : la clé de voûte a deux trous, et c'est toute la série qui les paie (Q5-3, Q5-5, Q5-10)

**État exact** (`Reflect.hpp`). `FieldKind` = 11 kinds scalaires (`:43-56`), `Value` =
`std::variant` miroir, `valueKind() = variant::index()`, `KindOf<T>` (`:68-80`), verrouillés par
`kindMatches` + `static_assert(variant_size == 11)` (`:90-108`, U1-01 ✅). `FieldInfo = { name,
id = fnv1a(name), kind, flags, get, set }` ; `FieldFlags` = un bit `Transient` (9 usages dans le
dépôt), `REFLECT_FIELD_FLAGS` existe. `TypeInfo = { name, id, size, parent, fields }` avec
`findField` qui remonte les parents et **`isA(typeId)`** (`:152-154`, 10 sites : `FormDatabase`,
`FormQuery`, `Resolver`, `CellLoader`, `AnimBridge`…). `TypeInfoBuilder::field<F>` déduit le
kind par `KindOf<F>::value` : un champ `enum class` **ne compile pas** — d'où les `str kind` et
`u32 id` que Q1 (Q1-14), Q2 (Q2-19, Q2-32, Q2-42), Q3 (Q3-16, Q3-27) et Q4 (Q4-7 : 19 champs
`str` + 6 `i32`, ~25 vocabulaires, ~12 parseurs) ont comptés de l'extérieur. Pas de `min/max/
label` → 190 sliders à la main (Q2-32), `FieldLane` réinventé (Q1-22) et sa version GPU (Q3-16).

**Deux extensions compatibles entre elles et avec H-a** (table de descripteurs de kind rejetée
en juillet parce qu'elle couplerait binaire↔ImGui↔Lua ; ici ce sont des **données** attachées au
champ, chaque consommateur garde son `visit` exhaustif et son propre corps, `Visit.hpp:18-21`) :

- **(B) `FieldMeta`** — pointeur optionnel sur `FieldInfo` : `struct FieldMeta { f32 min, max,
  step; const char* label, *doc, *unit; const EnumTable* enumeration; }` via
  `REFLECT_FIELD_META(name, meta)` / `REFLECT_FIELD_ENUM(slot, kArmorSlots)` ; `struct EnumTable
  { typeName; span<const char* const> names; }` fourni par `reflect::EnumNames<E>` (une table par
  enum, `static_assert(size == Count)`, T-4). Le kind reste `Str`/`I32` : **zéro changement
  disque** ; le loader valide le nom (« unknown value 'bedd' for slot »), la grille tire un
  combo, Lua idem, les parseurs deviennent `meta.enumeration->parse(text)`, les 190 sliders
  lisent `min/max`. Coût : `Reflect.hpp` ~15 l., une macro, les tables côté Forms.
- **(A) `FieldKind::Enum`** — appendu (même geste que `F64` l.54) + alternative `Value` =
  `EnumValue { i32 ordinal; const EnumTable* table }` ; `TypeInfoBuilder::field` prend une branche
  `if constexpr (std::is_enum_v<F>)` ; le **binaire** écrit l'ordinal (kind u8 + i32, `static_assert`
  bumpé à 12), le **TOML**/texte écrit le **nom** (un enum réordonné ne casse que le cuit, jamais un
  plugin texte). Coût : `Reflect.hpp` ~40 l., `ValueText.cpp` 2 cas, puis **un corps par
  consommateur `visit`** (BinaryFormat, TomlWriter/Reader, PropertyGrid → combo, Lua → string,
  console) — le build casse jusqu'à ce que chacun l'ait, c'est le contrat. Les Forms portent
  l'`enum class` directement ; les parseurs et `Keywords.cpp` disparaissent.

Les deux lots qui ont instruit la question (E1 côté `data/`, F1 côté `reflect/`) recommandent
**(B) d'abord**, (A) par-dessus si nécessaire. Avec (B), `FieldFlags` gagne `ParentLink` et
`Deprecated` (Q4-18) et devient la vraie « métadonnée de champ » minimale.

**Le pont `reflectedComponent`** (Q5-5). `ecs/World.hpp:49-58` : `registerComponent<T>()`
enregistre dans flecs **et** dans `reflectedComponents` « so saves can serialize component state
through the same reflect::Value pipeline as Forms (§2.8). The single registration point that
keeps the two from diverging » ; `reflectedComponent(id)` « drives generic component
serialization for saves ». **22 appels** à `registerComponent`, **0 appel** à
`reflectedComponent` hors `EcsTest.cpp:108-109`. `SaveState.cpp:42-60` recopie par type. Le
mécanisme générique est déclaré, prêt, et remplacé par une liste manuelle — la cause de T-13.
*Action* : `world.handle().each(entity, [&](flecs::id_t id) { if (auto* type =
world.reflectedComponent(id)) … })` remplace les listes (Q4-2), ou retirer le pont (ne pas
laisser un contrat non tenu dans l'en-tête).

**Le hash** (Q5-10). `Hash.hpp:12-19` : `constexpr u32 fnv1a(string_view)` (constantes 32 bits
justes) ; aucun `fnv1a64`/`Fnv64`/`hashReflected`. Copies u64 à basis tronqué `1469598103934665603`
(13 sites, 12 fichiers : `MasterNetwork.cpp:132`, `PoiPlan.cpp:872`, `TerrainGen.cpp:1039`,
`WorldLayer.cpp:443`, `ZonePlan.cpp:50, 659`, `MapBaker.cpp:73`, `TerrainGenTuning.cpp:416`,
`VulkanDevice.cpp:725`, `MapWorldFixture.hpp:215`, `TileBakeTest.cpp:286`,
`VegetationScatterTest.cpp:18, 107` ; un grep brut `.cpp/.hpp` en compte 16 fichiers) ; seule
`MapGrid.cpp:16` est juste ; `VertexAoCache.cpp:~190` réécrit la boucle 32 bits à la main sous un
commentaire « fnv1a64 » faux. *Action* : `fnv1a64`, `struct Fnv64 { mix(u64/u32/i32/f32 bit_cast/
string_view/bool) }`, `reflect::hashReflected(const TypeInfo&, const void*)` = `forEachField` +
`visit` → `Fnv64` (les 5 `hashParams` champ par champ de Q1 et `hashTerrainGenTuning` deviennent
un appel ; un champ oublié = impossible). Terrain → bump `kTileBakeVersion` groupé (Q1 lot 4) ;
Vulkan/tests : nul.

### S2. Les cycles de couches : trois inclusions, une décision (Q5-1, Q5-8)

`layers_audit.py` rejoué (0,25 s), **toutes** les lignes : UPWARD `engine/assets → engine/anim
(1)` = `GltfMesh.hpp:6` pour `anim::Skeleton`/`AnimClip` (`anim` n'inclut que `core` : violation
de rang par un fichier, même lib, aucun coût de link) ; UPWARD `engine/render → engine/(root)
(2)` = `WorldRenderer.cpp`/`FxRenderer.cpp → FrameContext.hpp` ; UPWARD `world → gameplay (15)`
= **faux-positif** (CLAUDE.md §4 mesure `data → gameplay → world`, le script range `world`
dessous) ; BIDIRECTIONAL `engine/(root) ↔ engine/render (4/2)` = `Engine` crée `SpriteRenderer`
+ `Camera2D` et les place dans `FrameContext.sprites` (`Engine.hpp:7`, `Engine.cpp:77, 112`,
`FrameContext.hpp:25-26`, `Game.hpp:38 draw(SpriteRenderer&)`) ; BIDIRECTIONAL `engine/render ↔
engine/ui (1/1)` = `UiSystem.cpp:16 → ShaderLibrary.hpp` (sain : l'infrastructure de shaders,
reçue par référence) et `WorldRenderer.hpp:123 ::ui::UiSystem* gameUi` + `.cpp:2652-2662`
(**la moitié qui casse**) ; `game ↔ game/scenes (4/65)` = l'objet-dieu (Q2). Le script n'est
référencé par aucun test, CMake ni CI.

*Croquis* (décision dev, avec Q3-13) : `cfg.sprites` sur `WorldRenderer` (propriétaire unique
des renderers) ou `SpriteRenderer` possédé par la pile de scènes (2 scènes sur 70 l'utilisent) ;
`FrameContext` = `Device&` + `CommandBuffer&` + `dt/size` sans type `render::` → les deux
retours deviennent des includes descendants légitimes ; `RenderView::gameUi` → `std::function
inPassUi` (ou `render::InPassDrawer`) rempli par la scène, `WorldRenderer` ne connaît plus `ui::`
(alternative cohérente : `cfg.gameUi`) ; `engine/anim/GltfSkeleton.{hpp,cpp}` (ou reranger
`assets` au rang 3 : il consomme `anim` et `rhi`, il **est** au-dessus — une ligne du script).
Le SCC `engine/(root) ↔ render ↔ ui` disparaît. Et brancher `layers_audit.py` en CTest avec
une **baseline des cycles tolérés** (un nouveau cycle fait échouer la suite ; chaque cycle
résolu retire sa ligne), rang de `world` corrigé (Q5-8). Avec, les shaders embarqués en C++
(`SpriteRenderer` Q3-40, `ImGuiLayer.cpp:46-78` avec `#ifdef VULKAN` à la main) passent dans
`shaders/` dès qu'une `ShaderLibrary` existe avant eux.

### S3. Le donjon : deux tables incomplètes dans le code le plus propre de la série (Q5-2, Q5-4, Q5-6, Q5-12)

**Sept vecteurs d'ancres parallèles** (Q5-4) : `DungeonBake.hpp:63-76` `barriers, levers, chests,
oreVeins, enemySpawns, npcSpawns, patrolPoints` (+ `torches` avec son struct). Chaque consommateur
les énumère : `bakeDungeon` 7 `.clear()` (`:443-449`) ; `DungeonRecords.cpp` (Q4) 8 boucles
`for (… bake.X)`, 6 `disablePast(kXRefBase, bake.X.size())`, un log à 5 tailles ; le tiroir
**promu** `DungeonThemeForm` (DUNGEON-GEN §7 : « pools de props/spawns ») ajoutera des
familles = 4 fichiers chacune. *Croquis* : `enum class AnchorKind : u8 { Torch, Barrier, Lever,
Chest, OreVein, EnemySpawn, NpcSpawn, PatrolPoint }`, `struct Anchor { kind, position, yawDeg,
lockId, width, wallNormal }`, `vector<Anchor>` ; côté records `kAnchorEmit[kind] = { refBase,
kitSlot, formKind, needsWallClear }` + `static_assert` (T-4) et UNE boucle. `DungeonBakeTest`
« bit-identique par seed » (itérer par kind dans l'ordre d'émission actuel) ;
`DungeonRecordsTest` re-Accept sans doublon. **Partagé avec Q4-1.**

**Les règles de marche écrites deux fois** (Q5-12) : snap au niveau le plus proche
(`navValidates:77-93` tolérance `2.5f` / `InteriorNavigator.cpp:28-44` `snapRange 2.5`), colonne
d'un niveau (boucle binaire à la main / `upper_bound`), pas maximal (`maxStep = 0.5f` « slightly
under the navigator's own budget » / `0.55`), tolérance de pied (`0.6f` / `0.6f`), flood
4-voisins par colonne (`flood:106-156` / `findPath:107-140`). Deux constantes liées par
commentaire (même famille que Q3-16) et un BFS parallèle à l'A*. *Croquis* : `NavGrid` porte
`columnOfLevel`, `snapLevel`, `neighbours(level, maxStep, fn)`, `floodFrom(level, allowFalls)` et
des constantes nommées `kStepBudget`, `kSnapRange`, `kFootTolerance` consommées par le bake
(`kStepBudget - 0.05f` explicité) et le navigateur. Bit-exact. Nuance au plan : « trois A* »
côté `engine/dungeon` est **faux** — `NavGrid` est une donnée (CSR) ; les trois A* sont dans
`world/ai` (Q4-22).

**`populateAnchors` 212 l.** (Q5-2) : trois passes (corridors : barrières + override levier +
ennemis ; salles par `NodeKind` ; coffre/gardien/PNJ) + six lambdas ; le choix de la cellule de
barrière (`:222-283`, 60 l.) est une décision pure sur `edge.path` → `pickBarrierCell(edge,
entrance) -> { index, straight }` statique et testable ; `populateCorridors`, `populateRooms`,
`populateGoal`. Même ordre de tirages `rng`. `routeCorridor` 155 l., `extractChunkMesh` 180 l.
(`vertexOf` 70 l. en lambda), `navValidates` 124 l. : lisibles, découpables sans urgence.

**La mise en scène en constantes** (Q5-6) : poste du gardien `Vec3{2,0,2}`, PNJ `{1.5,0,-1.5}`,
torches `1.8/8.0/0.4/0.2`, barrière sur virage `1.6f`, `scatter` 8 essais, AO 14 sondes, teintes
`baseRock/paleRock`, apron `±2.0f`, `navAttempt < 16` ; `DungeonParams` n'expose que 9 lanes et
`DungeonGenTool` 4 sliders. `DungeonThemeForm` (« mine/antre/temple = données ») est la table
où ces nombres vont (T-11) ; d'ici là `struct PopulateParams` + `TorchParams` dans
`DungeonParams` (mêmes défauts = bit-exact) et `[cpp-tuning]` (U4-7). Traits d'`EdgeKind` en six
sites (`shareable`, rayon `Hidden ? 0.7 : 1.0`, pas de torche si `Hidden`, ennemi si `Dangerous`,
barrière si `Locked`, style DOT) → `kEdgeKinds[] { shareable, radiusScale, lit, spawnsEnemy,
barred, dotStyle }` + `static_assert` (T-4) ; `buildCycle` (switch plat sur `CyclePattern`) est au
contraire la bonne forme.

### S4. Les petites duplications et les petits vocabulaires (Q5-7, Q5-11, Q5-13, Q5-14)

`GltfMesh.cpp` (Q5-11) : `appendPrimitive` (`:~98-170`) et `buildSkinnedMesh` (`:~455-525`)
recopient `findAttribute` ×4-6, `baseColor`, la lecture position/normale/uv/couleur, la boucle
d'indices avec repli « pas d'indices = séquence » (différences réelles : `world` non appliqué,
joints/poids) ; `loadGltfMeshParts` recopie la boucle de nœuds de `buildMesh` ; `normalizeMesh`/
`groundMesh` recalculent le même `lo/hi/pivot` ; `loadGltfSkeleton` + `loadGltfAnimations` parsent
le fichier **deux fois** pour le même appelant ; `pow(x, 2.2f)` = 3e écriture de sRGB→linéaire
(`SkySystem.cpp:22`, shaders). → `readPrimitive(primitive) -> PrimitiveAttributes`, `appendIndices`,
`forEachMeshNode`, `footprintPivot`, `loadGltfRig(path) -> { Skeleton, clips }`,
`core::srgbToLinear`. Bit-identique.

`engine/ui` (Q5-13) : 22 `destroy*` manuels (`UiSystem.cpp` 13 : `ReleaseGeometry`,
`ReleaseTexture`, `releaseAll`, `destroy` ×4 ; `ImGuiLayer.cpp` 9 : destructeur ordonné à la
main, `growBuffers` ×2) ; `Geometry { BufferHandle ×2 }` et `Texture { TextureHandle,
BindGroupHandle }` sont les candidats évidents à `Unique*` (les maps libèrent par `erase`). La
série Q3-33 passe à **12 classes / 129 appels**. Le shader dev-UI embarqué (`ImGuiLayer.cpp:46-78`,
`layout(std140, binding = 15)` écrit à la main = ce que `compat.glsl:31 MEADOWS_PUSH_CONSTANTS`
fait pour le corpus) suit `SpriteRenderer` (Q3-40) dès que Q5-1 est tranché.

Les petits vocabulaires (Q5-7) : `EmitterParams.additive/flame/haze` et `Particle.additive/
flame/haze` (`Particles.hpp:49-57, 76-78`) = trois booléens pour un kind exclusif à 4 valeurs
que `GameplayCues.cpp:57-59` décode en trois comparaisons et `FxRenderer` re-dispatche (rien
n'empêche `flame && haze`) → `enum class BlendKind : u8` ; `Audio.cpp:12-13 kBuses[]` +
`busOf(string_view)`, `SoundParams.bus { "sfx" }` → `enum class Bus : u8` + `array<ma_sound_group,
5>` (un bus inconnu = erreur de validation data, plus un `// silently ignored` runtime) ;
`CookedTexture.cpp:14-36 formatCode/formatFromCode` = 2 switches miroirs → colonne `cookCode` de
`kFormatTraits[]` (Q3-17) ; `rmlKeyFor` (`UiSystem.cpp:~355-395`) : 34 `case` sans garde (une
touche ajoutée tombe dans `KI_UNKNOWN` en silence) → `constexpr array<KeyIdentifier,
size_t(Key::Count)>` + `static_assert` (T-4), et les modificateurs incohérents
(`processMouseButton` passe `0`, `Alt` sans `KM_ALT`). Tous T-3/T-4.

Les petits doublons (Q5-14) : plomberie Jolt « settings → `CreateAndAddBody` →
`GetIndexAndSequenceNumber` » ×4, triplet de filtres ×3, `ExtendedUpdate` ×2, `BodyID id {
static_cast<uint32>(body) }` ×5 → `addBody()`, `characterFilters()`, `toJph(BodyId)`,
`toGlm(Quat)` ; trois petits formats maison (`CookedMesh`, `CookedTexture`, `VertexAoCache`) +
`.nvg` (`NavGrid.cpp:129-215`, magic + version + lambdas `read/write`, 86 l.) — T-5 compte
maintenant **12 formats** (8 terrain + WSD + saves + ces 4), le reader `.nvg` avec ses bornes
(`kMaxDim`, « a corrupt header must fail as nullopt, not as a multi-GB allocation ») est le modèle
à généraliser ; `UiSystem` : racines copiées dans `renderInterface.roots` ET `fileInterface.roots`
avec deux boucles « dernier gagnant » → `resolveInRoots()`, viewport posé par `resize()` ET
réécrit par `render(w, h)`, `ri.device` fixé à `create` ET réassigné à chaque `render`.

### S5. Hygiène (Q5-9, Q5-18, Q5-19)

`GlContext.cpp:46 setVsync(true)` sans knob ni autre appelant (règle dev « perf knobs dans
l'UI » : c'est l'absence du knob, pas une perf) → `EngineConfig::vsync` câblé aux deux backends
(Q5-9). `JobCounter` + `JobSystem::wait` : zéro client hors test — **preuve positive de Phase 5
par absence**, mais code sans client contre la règle `Result.hpp:23-25` (« core primitives
arrive with a concrete client ») → garder documenté comme réservé à `forEachIndexed` (T-6,
remède de `MasterNetwork` Q1-25) ou retirer ; `Log::init` non idempotent
(`spdlog::stdout_color_mt("meadows")` lève au second appel, trois points d'entrée) ;
`valueFromString` laxiste (`Bool` = `"true" || "1"` sinon `false` sans `nullopt`, `stol` accepte
`"12abc"`, `try/catch` en chemin froid) → `std::from_chars` + contrôle de fin, `Bool` strict
(avec Q4-27) ; `Physics::tick` accumulateur sans plafond (spirale après un hitch de 2 s : 120
sous-pas) → `min(accumulator, kFixedStep * 8)` ou documenter ; `MeshSimplify` trois seuils en dur
commentés par l'histoire du bug ; repli `../Textures` Quaternius dans le loader générique ;
`kDungeonBakeVersion = 1` sert `.cmesh` ET `.nvg` (acceptable, à écrire) (Q5-18). Commentaires
(Q5-19) : **PASS** — 3 écarts F1 (`Input.hpp:155` « audit U5-8 », `Bezier.hpp:9` « MEADOWS-PLAN
P1 », `Nav.hpp:13-19` plan dans l'en-tête), 1 F2 (`Anim.cpp:150` « used to »), 5 F3
(`SpaceGraph.cpp:197, 299` « seed-1336 playtest », `DensityField.cpp:75-79` ancienne valeur,
`DungeonBake.hpp:45-47` backlog, `UiSystem.hpp:57-65` « Planned extension points » **périmé** —
les trois sont livrés) ; `VertexAoCache.hpp:11` « fnv1a64 » faux ; « Subsystem map:
docs/AUDIT/U1 » en tête de 3 headers (pointeur vers un audit clos → `HORIZONTAL-PASS.md`) ;
**CLAUDE.md §4 périmé** ×2 : `/gl # OpenGL 4.6 backend (only backend for now)` alors que Vulkan
est livré depuis 2026-07-19, et « only common/ exists today » alors que `platform/macos/
Activation.mm` existe.

---

## Bugs trouvés au passage (classe B) — aucun corrigé

| id | sév | symbole (fichier) | effet | correction | compat |
|---|---|---|---|---|---|
| Q5-15 | basse (latent, prouvé par les sources Jolt) | `phys::BodyId` (`Physics.hpp:21` « 0 = invalid ») vs `GetIndexAndSequenceNumber()` ×4 | Jolt compose l'id = index \| (séquence `uint8` << 23), séquence partant de 1 → **l'id 0 réapparaît quand le slot 0 a été réutilisé 256 fois** ; le streaming de cellules crée/détruit des corps en continu : sur une longue session un corps valide est pris pour un échec (`body != 0` dans `StreamingController.cpp:118`, `VegetationCollision.cpp:120`, `LandscapeScene.cpp:5955`) → jamais `removeBody`, collision fantôme | `BodyId = raw + 1` (ou sentinelle `~0ull` comme `cInvalidBodyID`), décodé dans `toJph` | interne |
| Q5-16 | basse (latent HiDPI) | `Window::width()` (`Window.cpp:91-93`) vs `Input::mousePosition()` (`Input.cpp:175-176`) | `width/height` passent en **pixels** après `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`, la souris reste en **points logiques** (`SDL_GetMouseState`, documentée « screen pixels » `Input.hpp:85`) : identiques à facteur 1 (Windows du dev), décalés ×2 sur HiDPI (machine de voyage macOS) pour tout `screenToWorld`/picking | documenter l'unité, `pixelDensity()` ou convertir la souris dans `Input::update` | nul sur Windows |
| Q5-17 | basse (fuite bornée) | `ImGuiLayer::groupFor` (`ImGuiLayer.cpp:~190-200`) | un bind group par id de texture rencontré, jamais purgé ; `MiniMapPanel.cpp:127` passe des textures runtime recréées → un groupe fuit par recréation sur une texture détruite | cache LRU borné, `forgetTexture(id)`, ou purge des ids non vus depuis N frames | nul |

---

## Findings

### S — structure

**Q5-1** [S / moyenne / M — **décision dev**, avec Q3-13] — les trois cycles de couches, localisés : `Engine` possède `SpriteRenderer` + `Camera2D` (`Engine.hpp:7`, `Engine.cpp:77, 112`, `FrameContext.hpp:25-26`, `Game.hpp:38`) = `engine/(root) ↔ render` (4/2) ; `WorldRenderer.hpp:123 ::ui::UiSystem* gameUi` + `.cpp:2652-2662` = la moitié de `render ↔ ui` qui casse (`UiSystem.cpp:16 → ShaderLibrary.hpp` est sain) ; `GltfMesh.hpp:6 → anim/Anim.hpp` = `assets → anim`. *Action* : `cfg.sprites` sur `WorldRenderer` ou pile de scènes ; `FrameContext` sans type `render::` ; `RenderView::inPassUi` callback (ou `cfg.gameUi`) ; `engine/anim/GltfSkeleton` ou `assets` au rang 3. Nul fonctionnellement.

**Q5-2** [S / moyenne / M] — `populateAnchors` (`DungeonBake.cpp:194-405`) 212 l. : trois passes + six lambdas, choix de la cellule de barrière 60 l. inline (`:222-283`) ; `routeCorridor` 155, `extractChunkMesh` 180 (`vertexOf` 70 l. en lambda), `navValidates` 124 — lisibles. *Action* : `pickBarrierCell()` statique, `populateCorridors/Rooms/Goal`, lambdas → membres de `NavGrid` (Q5-12) ; `bfsCorridor/pathSelfPierces/claimPath`, `cellVertex()`. Même ordre de tirages ; `DungeonSpaceTest`, `DungeonMeshTest`.

### G — généraliser

**Q5-3** [G / haute / M — **décision dev §2.3 (T-3), côté réflexion**] — `Reflect.hpp` : `FieldKind` 11 kinds, `KindOf<F>` refuse un `enum class`, `FieldFlags` un bit, pas de `FieldMeta` ; `TypeInfo::isA` **existe** (10 sites). *Action* : (B) `FieldMeta`/`EnumTable` optionnelle (~15 l., zéro effet disque) d'abord ; (A) `FieldKind::Enum` appendu (~40 l. + un corps par consommateur `visit`, bump binaire, TOML = nom) si nécessaire. Compatible H-a. Débloque Q1-14, Q1-22, Q2-19, Q2-32, Q2-42, Q3-16, Q3-27, Q4-7…Q4-19.

**Q5-4** [G / haute / M — T-4, avec Q4-1] — sept vecteurs d'ancres parallèles (`DungeonBake.hpp:63-76`) : 7 `.clear()`, 8 boucles + 6 `disablePast` + un log à 5 tailles côté `DungeonRecords`, 4 fichiers par famille ajoutée. *Action* : `AnchorKind` + `vector<Anchor>` + `kAnchorEmit[kind] { refBase, kitSlot, formKind, needsWallClear }` + `static_assert`, UNE boucle. `DungeonBakeTest` bit-identique par seed.

**Q5-5** [G / moyenne / S — T-13] — le pont `ecs::World::registerComponent → reflectedComponent` (`World.hpp:49-63`) : 22 enregistrements, **0 appelant** hors test ; `SaveState.cpp:42-60` recopie par type. *Action* : le save fait le tour des composants réfléchis de l'entité (plus de liste, Q4-2) ou retirer le pont. Save = texte, comparable.

**Q5-6** [G / moyenne / S — T-11, T-4] — mise en scène du donjon en ~15 constantes de code (`DungeonBake.cpp:19-60, 275, 325, 386, 394, 424, 470-535`) alors que `DungeonParams` expose 9 lanes et `DungeonThemeForm` est le tiroir promu ; traits d'`EdgeKind` en six sites (`SpaceGraph.cpp:343-345`, `DensityField.cpp:62`, `DungeonBake.cpp:222, 285, 505`, `MissionGraph.cpp:306-312`). *Action* : `PopulateParams` + `TorchParams` (mêmes défauts), `[cpp-tuning]` d'ici là ; `kEdgeKinds[]` + `static_assert`. Bit-exact.

**Q5-7** [G / basse / S — T-3/T-4] — petits vocabulaires : blend particules en 3 bools (`Particles.hpp:49-57, 76-78`, décodé `GameplayCues.cpp:57-59`) → `BlendKind` ; bus audio en string (`Audio.cpp:12-13`) → `enum class Bus` ; `formatCode/formatFromCode` 2 switches (`CookedTexture.cpp:14-36`) → colonne de `kFormatTraits[]` (Q3-17) ; `rmlKeyFor` 34 `case` sans garde + modificateurs incohérents (`UiSystem.cpp:~355-395, ~672-678`) → table `static_assert`-ée + `rmlModifiers()` partout. Nul.

**Q5-8** [G / basse / S] — `layers_audit.py` : non branché (aucune référence tests/CMake/CI), rang de `world` faux (15 UPWARD fantômes). *Action* : rang corrigé, CTest avec baseline des cycles tolérés (les deux SCC), un nouveau cycle fait échouer la suite, un cycle résolu retire sa ligne.

**Q5-9** [G / basse / S] — vsync en dur (`GlContext.cpp:46`), `setVsync` sans autre appelant, pas d'équivalent documenté côté Vulkan. *Action* : `EngineConfig::vsync` (ou knob render) câblé aux deux backends. Règle « perf knobs dans l'UI ».

### D — duplication

**Q5-10** [D / moyenne / S — T-2] — `core::fnv1a` u32 seul (`Hash.hpp:12-19`) ; 13 copies u64 dans 12 fichiers avec le basis tronqué `…603` (Q1-41, Q3-42), seule `MapGrid.cpp:16` juste ; `VertexAoCache.cpp:~190` boucle 32 bits à la main sous « fnv1a64 » faux. *Action* : `fnv1a64`, `Fnv64 { mix(…) }`, `reflect::hashReflected(TypeInfo, object)` (~40 l.) ; les 5 `hashParams` de Q1 et `hashTerrainGenTuning` deviennent un appel. Terrain → bump groupé ; reste nul.

**Q5-11** [D / moyenne / S] — `GltfMesh.cpp` : boucle de sommets ×2 (`appendPrimitive:~98-170` / `buildSkinnedMesh:~455-525`), boucle de nœuds ×2, AABB/pivot ×2, double parse squelette/clips, sRGB→linéaire 3e écriture (`:82-84`). *Action* : `readPrimitive`, `appendIndices`, `forEachMeshNode`, `footprintPivot`, `loadGltfRig`, `core::srgbToLinear`. Bit-identique ; `GltfMeshTest`.

**Q5-12** [D / moyenne / S] — règles de marche ×2 bake/navigateur : snap `2.5`, colonne CSR (boucle à la main / `upper_bound`), pas `0.5` « slightly under » / `0.55`, pied `0.6` / `0.6`, flood 4-voisins / A* (`DungeonBake.cpp:69-193` vs `InteriorNavigator.cpp:28-140`, `NavGrid.cpp:46`). *Action* : `NavGrid::columnOfLevel/snapLevel/neighbours/floodFrom` + `kStepBudget/kSnapRange/kFootTolerance` nommées. Bit-exact ; `DungeonNavTest`.

**Q5-13** [D / moyenne / S — Q3-33 étendu] — `engine/ui` : 22 `destroy*` manuels (`UiSystem.cpp` 13, `ImGuiLayer.cpp` 9) → `Unique*` sur `Geometry`/`Texture` (série : 12 classes / 129 appels) ; shader dev-UI embarqué `#ifdef VULKAN` à la main (`ImGuiLayer.cpp:46-78`) → `imgui.vert/.frag` avec Q3-40 après Q5-1 ; compteur de génération `UiSystem.cpp:~627` (déjà Q3-15). Nul.

**Q5-14** [D / basse / S] — plomberie Jolt ×4/×3/×2/×5 (`Physics.cpp:~163-258, ~287`) → `addBody()`, `characterFilters()`, `toJph`, `toGlm(Quat)` ; quatre petits formats maison (`CookedMesh`, `CookedTexture`, `VertexAoCache`, `.nvg`) → T-5 (12 formats), reader `.nvg` borné = modèle ; `UiSystem` racines ×2, viewport ×2, `ri.device` ×2 → `resolveInRoots()`, `render()` sans taille. Nul.

### B — bugs

Voir la table : Q5-15 (sentinelle `BodyId = 0`, prouvé par les sources Jolt), Q5-16 (unités fenêtre/souris HiDPI), Q5-17 (fuite de bind groups ImGui).

### H — hygiène

**Q5-18** [H / basse / S] — `JobCounter`/`JobSystem::wait` sans client (`Jobs.hpp:18-64`) → documenter « réservé à `forEachIndexed` » ou retirer ; `Log::init` non idempotent (`Log.cpp:8`) ; `valueFromString` laxiste (`ValueText.cpp:42-55`) → `from_chars` + `Bool` strict ; `Physics::tick` accumulateur sans plafond (`:~150`) ; `MeshSimplify.cpp:24-45` seuils en dur commentés par l'histoire ; repli `../Textures` (`GltfMesh.cpp:~60-66`) ; `kDungeonBakeVersion` sert deux formats (`DungeonBake.hpp:23`, à écrire) ; `SpiritJetList`/`EventBus` hors lot. Nul.

**Q5-19** [H / basse / S] — commentaires : 9 écarts sur 10 500 l. (`Input.hpp:155`, `Bezier.hpp:9`, `Nav.hpp:13-19` plan dans l'en-tête, `Anim.cpp:150`, `SpaceGraph.cpp:197, 299`, `DensityField.cpp:75-79`, `DungeonBake.hpp:45-47`, `UiSystem.hpp:57-65` **périmé**), `VertexAoCache.hpp:11` « fnv1a64 » faux, « Subsystem map: docs/AUDIT/U1 » ×3 → `HORIZONTAL-PASS.md` ; **CLAUDE.md §4** : « only backend for now » (GL) et « only common/ exists » périmés ; **README §0.2** : U2-07/U8-5 à clore (ImGui sur le RHI). Nul.

### P — optimisation évidente

Aucune.

---

## Positifs (modèles à citer)

- **`reflect/Visit.hpp`** : « exhaustif par construction, un corps par consommateur » (H-a) — le
  modèle pour ajouter un kind sans table centrale ; `Reflect.hpp:82-108` : le verrou du contrat
  on-disk en `static_assert` (modèle de Q3-16 et de Q5-3 A).
- **`platform/Input`** : vocabulaire fermé = enum + tables de scancodes en switch plat exhaustif
  (`Key::Count: break`), `applyDeadzone` pure et testée hors SDL, canal polled + canal
  événementiel séparés.
- **`core`** : `ConcurrentQueue::drain` (swap sous verrou, application hors verrou) ; `JobSystem`
  « simplest thing », `isStopping/stopFlag` sondés par les kernels ; `Result` « introduced with
  its first real call path » (la règle d'or des primitives) ; `Guid::combine` déterministe ;
  `Rng` avec `rawState` ; `Clock` unique ; `SettleTimer`.
- **`Engine.cpp:44-48`** : la chaîne de repli explique *pourquoi* elle vit là — commentaire
  durable exemplaire.
- **Les cinq façades** `Physics.hpp`, `Audio.hpp`, `Anim.hpp`, `Particles.hpp`, `AssetDatabase`
  (§5 en 30 lignes, « knows nothing about plugins ») : narrow interface + pimpl + flat params ;
  `GraphInstance` conditions par callbacks (le graphe ne voit jamais un type gameplay) ;
  `CookedMesh` « content version » séparée de « format version » ; `VertexAoCache` entrées
  strength-free, prune, « never fatal » ; `ParticleSim` « FX degrade, never grow the frame » ;
  `MeshSimplify` trois paliers avec la raison de chacun.
- **`engine/dungeon` entier** : étages purs, « low resolution first », chaque invariant écrit au
  point de décision et doctesté, `isSolvable(mission, space)` **réutilise** D1 par projection,
  grille auto-grandissante déterministe, `readNvgFile` bornes avant toute allocation,
  `MissionParams`/`SpaceParams`/`DensityParams` « le cadran de design : une mine et un temple
  diffèrent par ces valeurs, pas par du code », `buildCycle` « adding one = one function ».
- **`UiSystem.hpp`** : façade étroite, aucun type Rml exposé, VFS « dernier gagnant » (modèle
  SkyUI), localisation par lambda (la lib reste `data`-free), `setRuntimeTexture` avec
  l'invalidation Rml expliquée ; `RenderGeometry` push constants « not a UBO write » avec la raison
  Vulkan.
- **`ImGuiLayer`** : renderer sur le RHI en 330 l., aplatissement en un VB/IB (leçon Vulkan
  appliquée et expliquée), mode dégradé GL 4.1 légal.
- Politique de commentaires **tenue** sur les 10 500 lignes.

---

## Ordre de correction proposé (à arbitrer)

1. **Bugs** : Q5-15 (sentinelle, S), Q5-17 (purge ImGui), Q5-16 (documenter l'unité, convertir
   la souris).
2. **Le socle que les autres vagues attendent** : Q5-10 (`fnv1a64`/`Fnv64`/`hashReflected` —
   préalable de Q1 lot 4 et Q3-42), `core::kPi/kTau` + `packXZ` (Q3-39, Q4-25), descente de
   `BakeMailbox`/`ChunkStreamer` (Q3-23, aucun symbole manquant), `reflect::copyFields` public
   (Q4-21), `FieldFlags::ParentLink/Deprecated` (Q4-18).
3. **La décision T-3** (Q5-3 avec Q4-7) : (B) `FieldMeta`/`EnumTable` ; puis Q5-7 (blend, bus,
   `rmlKeyFor`) et tout ce que Q2/Q4 en attendent.
4. **La décision T-13** (Q5-5 avec Q4-2) : save par le pont `reflectedComponent` ou
   `attachActorKit`.
5. **Les couches** (Q5-1 avec Q3-13, décision dev) puis Q5-8 (`layers_audit` en CTest avec
   baseline), Q5-13 (`Unique` ui, shaders dev-UI dans `shaders/`), Q5-9 (vsync).
6. **Le donjon** : Q5-12 (règles de marche sur `NavGrid`), Q5-2 (`populateAnchors`), Q5-4
   (`AnchorKind`, avec Q4-1), Q5-6 (tables, avant `DungeonThemeForm`).
7. **Dédup et hygiène** : Q5-11 (glTF), Q5-14, Q5-18, Q5-19 (dont CLAUDE.md §4 et README §0.2).
