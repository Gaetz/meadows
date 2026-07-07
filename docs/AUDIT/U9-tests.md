# U9 — Suite de tests (`tests/`)

**Périmètre** : 76 doctest TUs, ~9063 lignes, ~360 TEST_CASE/SUBCASE.
Executable unique `meadows-tests` liant meadows, -data, -ecs, -world,
-runtime, -gameplay, -script, -narrative, -physics, -audio.

**Verdict** : suite globalement saine et sérieuse. Les systèmes « mandatés »
par CLAUDE.md §8 (resolver, save-layering) sont couverts **en profondeur** ;
le déterminisme (§8) est discipliné partout ; aucun test désactivé/skip,
aucun WARN-only, aucun pattern flaky (threads via atomics + go-gate + join,
zéro `sleep`). Les trous réels sont : le **seam RenderSnapshot** à peine
effleuré, **aucun garde-fou sur l'ordre des ordinaux de réflexion** dans le
binaire cuit, et de la **duplication de fixtures** massive.

## Points forts (pas d'action)

- **Déterminisme §8** : chaque système probabiliste seed explicitement —
  `core::Rng rng(1)` (Afflictions/Injuries), `Rng{42}` (Barter),
  `vm.seedRng(42)` (Script), bursts particules seed 42, terrain/tree gen
  seed 1337/42. `RngTest.cpp` valide la reproductibilité de la primitive.
- **Threading** : `ConcurrentQueueTest` (8 producteurs × 10k, comptage exact
  « rien perdu/dupliqué ») et `JobsTest` (counter, drain au destructeur,
  réutilisation) sont corrects — synchronisation par atomics, `join()`, pas
  de course ni de sleep.
- **Save = plugin (§2.4/§5)** : `SaveStateTest` prouve headless le round-trip
  complet acteur → TOML → resolve → apply, comparaison **par réflexion** de
  tous les composants de stats + les deux chemins d'effet (real-time +
  game-time). `CellDeltaTest`, `PrefabChildSaveTest`, `QuestSaveTest`
  complètent.
- **§2.9** : `GameplayEffectsTest` vérifie que tout passe par `applyEffect`
  (instant/infinite/duration/periodic, clamp, tags requis/bloqués, méta-dégât
  → health) — le pipeline de mutation d'attribut est bien le seul chemin.

## Findings

| id | sév | axe | fichier:ligne | description | action | effort | inter-unité |
|----|-----|-----|---------------|-------------|--------|--------|-------------|
| U9-1 | med | qualité | tests/SceneSubmitTest.cpp:1-40 | Le seam sim/présentation (§7, Phase-5) n'est testé que par `spriteFor` (mapping 2D, 2 cas). Le contrat central du `RenderSnapshot` — paquet POD auto-possédant, extract→submit, aucun pointeur vivant vers le World — n'a **aucune** assertion headless ; le commentaire renvoie le reste au « run the game ». | Ajouter un test qui construit un World, appelle `extractScene`, et assert que le snapshot est self-contained (handles résolus, survit au teardown du World). | M | oui (U5) |
| U9-2 | med | factor | tests/CookerTest.cpp:18-40 | `CookerTest` (kitchen-sink) round-trippe le binaire avec des field-ids **synthétiques** (1-9). Aucun test ne fige l'ordre des ordinaux `REFLECT_FIELD` d'une **vraie** Form : un réordonnancement (la fragilité documentée) round-trip proprement et livre du binaire cuit corrompu **sans qu'aucun test échoue**. | Test « golden ordinal » : cuire une Form réelle et asserter la séquence exacte des field-ids attendus. | M | oui (H reflect-ordinaux) |
| U9-3 | med | archi | tests/CMakeLists.txt:78-81 | Le binaire de test lie `meadows` (donc render), et `world/terrain/TerrainPatches.hpp` inclut déjà `render/`. Il n'existe **aucune cible de link qui prouve** que gameplay/world/data/script/narrative se compilent+tournent **sans** render — la « preuve headless » (§2.10) n'est pas verrouillée au link. Une régression de dépendance passerait inaperçue. | Cible optionnelle `meadows-tests-headless` liant uniquement les libs sim, sur le sous-ensemble de TUs sans render. | M | oui (U7 §2.10) |
| U9-4 | low | factor | tests/*.cpp (≈147 occ. / 33 TUs) | `makeTypes()`, un helper `guid()`/`Guid::fromString`, et des littéraux TOML de plugin sont recopiés dans ~30 TUs. Boilerplate + risque de dérive entre copies. | `tests/Fixtures.hpp` : builders de registry, helper guid, 2-3 plugins TOML canoniques partagés. | S | non |
| U9-5 | low | qualité | tests/GameClockTest.cpp:1-22 | `GameClock` n'est testé que sur advance/dérivation (2 cas). Le branchement double-horloge (H-e : `tickGameTimeEffects` piloté par le temps-jeu) est exercé avec un **argument secondes en dur** dans Afflictions/Drugs, jamais **à travers l'horloge** qui l'alimente en jeu. Le seam GameClock→effets game-time n'est pas couvert. | Un test qui advance un `GameClock` et fait expirer un effet game-time via le temps ainsi produit. | S | oui (H-e) |
| U9-6 | low | couverture | tests/QuestTest.cpp | `QuestTest` couvre stages/machine à états mais **aucun** cas d'alias (fill/resolve), alors que les alias sont un concept quête de premier ordre (`quest/`). | Ajouter un cas alias (résolution d'un acteur/référence via alias dans une condition de transition). | S | non |
| U9-7 | low | couverture | game/ui/PropertyGrid.cpp (aucun TU) | Le `switch FieldKind` recopié dans `PropertyGrid` (hotspot H-a) n'est exercé qu'indirectement via `EditSessionTest`/`LevelEditorTest` (logique éditeur, pas rendu ImGui). Acceptable pour de l'UI, mais le chemin de conversion Value↔widget n'a pas d'assertion propre. | Si H-a est unifié en visiteur, y attacher un test de round-trip Value→display→edit. | S | oui (H-a) |
| U9-8 | low | couverture | tests/CuesSchedulesTest.cpp:175-191 | `FurnitureOccupancy` est testé (claim/full/release) mais la **logique d'usage** (begin/end use, liaison à un acteur, effet appliqué en s'asseyant) ne l'est pas — seul le compteur d'occupation l'est. | Étendre d'un cas « acteur utilise le mobilier → effet/tag appliqué ». | S | non |

## Table de couverture (système → test présent ? → profondeur)

| Système | Test | Profondeur |
|---|---|---|
| Réflexion (keystone §2.3) | ReflectTest | profonde |
| Resolver / patch layering (§8) | ResolverTest (9 cas) | **profonde** |
| Save = couche de patches (§2.4/§5) | SaveState, SaveForms, CellDelta, PrefabChildSave, QuestSave | **profonde** |
| Cooker texte↔binaire | CookerTest (kitchen-sink) | bonne, mais ids synthétiques (voir U9-2) |
| Ordinaux réflexion figés (binaire) | — | **absent (U9-2)** |
| Synthesis §5.1 | SynthesisTest | **profonde** (provenance, round-trip, arbitrage) |
| EditSession éditeur→plugin | EditSession, LevelEditor | bonne |
| GAS effets / mutation §2.9 | GameplayEffects, GameplayAbility | **profonde** |
| Double tick real+game-time (H-e) | GameplayEffects + Afflictions/Drugs | bonne (seam horloge non couvert, U9-5) |
| GameClock | GameClockTest | superficielle (U9-5) |
| Attributs/stats/derived/resonance/injuries/survie/status | ~15 TUs | **profonde** |
| Cell streaming | CellStreamer, CellLoader, CellDelta | **profonde** |
| RenderSnapshot / seam présentation | SceneSubmitTest | **superficielle (U9-1)** |
| Spawner par catégorie (§2.7) | SpawnerTest | profonde |
| Quête (machine à états) | QuestTest | bonne (alias non couverts, U9-6) |
| Dialogue | DialogueTest | bonne |
| Script Lua (sol2) | ScriptTest | bonne |
| Condition evaluator | ConditionTest | bonne |
| Factions / tags | Factions, GameplayTags | bonne |
| Cues / Schedules / Furniture | CuesSchedulesTest | bonne (usage mobilier partiel, U9-8) |
| Nav (grid A*) | TerrainNavigator, Ai, CuesSchedules | bonne |
| Physique (Jolt facade) | PhysicsTest | bonne |
| Audio (null backend) | AudioTest | bonne (headless CI-safe) |
| ECS (flecs wrapper) | EcsTest | bonne |
| Anim skeletal | AnimTest (349 l.) | **profonde** |
| Assets DB / résidence | AssetsTest | bonne |
| JobSystem / ConcurrentQueue | Jobs, ConcurrentQueue | **profonde** |
| Terrain/mesh CPU (noise/tree/frustum/gltf/occlusion/collision) | 8 TUs | bonne |
| game/ui panels ImGui | — (indirect) | absent, acceptable (UI) |
| RHI / backend GL | — | absent (besoin contexte GL), acceptable |

**Plus gros TU** : AnimTest (349), ResolverTest (324), SpawnerTest (320),
SaveStateTest (320), HorizontalFormsTest (340).
**Plus petits** : GameClockTest (22), AudioTest (32), EventBusTest (34),
MovementTest (38), SceneSubmitTest (40).

**Churn chantiers 5-8 → tous ont un test** : save (SaveState/CellDelta ✓),
streaming (CellStreamer ✓), éditeurs (EditSession/LevelEditor ✓), synthesis
(Synthesis ✓), quest (Quest/QuestSave ✓). Aucun système à churn récent sans
test correspondant.
