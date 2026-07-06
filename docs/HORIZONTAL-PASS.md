# HORIZONTAL-PASS — Architecture de la démo, posée par Fable (5-6 juillet 2026)

> **Pour les sessions POST-7 JUILLET (modèle moins puissant) : ce document
> est votre contrat.** L'architecture ci-dessous a été posée et validée par
> le dev — SUIVEZ-LA, ne redessinez ni les seams ni les décisions actées
> (`docs/MEADOWS-PLAN.md` § Décisions). Remplissez UNE verticale à la fois
> (ordre macro de MEADOWS-PLAN), cadence brique par brique avec validation
> dev. Chaque module ci-dessous a sa note « comment remplir » — dans ce
> fichier ET en tête de ses sources.

## Les règles transverses (ne jamais casser)

1. **Réflexion plate + records enfants** : pas de conteneurs dans les
   Forms ; toute cardinalité variable = Form enfant avec `core::Guid
   parent`, requêtée par `data::childrenOf<T>` (`data/forms/FormQuery.hpp`).
   Un mod AJOUTE une entrée sans toucher au parent — c'est le superpouvoir
   du modèle, préservez-le.
2. **`engine/*` ne dépend jamais de `data/*`** : les runtimes moteur
   consomment des structs de params plats ; le mapping Form→params vit dans
   world/gameplay/runtime (précédents : LandscapeTuning, AnimBridge).
3. **Pimpl sur les deps externes** : aucun type Jolt/Rml/ma_* ne traverse
   un header public (§3.1).
4. **Tout outil écrit des plugins** : toute édition passe par
   `data::EditSession` → `exportPlugin()` → TomlWriter. Jamais de format
   de sauvegarde d'outil parallèle (§5).
5. **Headless d'abord** : chaque runtime se doctest sans renderer (232+
   tests au moment de la passation). Un système qui ne se teste pas
   headless est probablement mal découpé.

## État des livraisons (tout compile, 229 tests verts, smoke-run propre)

| Brique | Livré | Où |
|---|---|---|
| H1 | ~25 nouveaux types de Forms (visuel/anim/audio/UI/loc/perso/IA/mobilier/monde), helpers `forEach`/`childrenOf`/`findByEditorId`, `plugins.toml` + `loadPluginStack`, catégories spawner étendues | `data/forms/*Forms.hpp`, `data/plugins/PluginConfig.*`, `world/worldspace/*` |
| H2 | `EditSession` (drafts par réflexion, undo/redo, export diff→plugin), scène **Game DB (editor)** : property grid réflexion, gestionnaire de plugins + conflits par champ, console dev (get/set réflexion + REPL Lua) | `data/plugins/EditSession.*`, `game/scenes/EditorScene.*`, `game/ui/PropertyGrid.*`, `game/ui/ConsolePanel.*` |
| H3 | Seam Jolt 5.2 : `PhysicsWorld` (tick fixe, boxes, raycasts), `CharacterBody` (capsule cinématique, **pas de root motion**) | `engine/physics/Physics.*`, lib `meadows-physics` |
| H4 | Seam RmlUi 6.1 : rendu sur RHI (+`setScissor`, blend prémultiplié), FileInterface = **overlay par chemin sur les `ui/` des plugins** (dernier gagnant, modèle SkyUI), scène **UI (RmlUi)** | `engine/ui/UiSystem.*`, lib `meadows-ui`, `game/data/base/ui/` |
| H5 | Runtime anim headless : Skeleton/Clip/sample/blend/skin, graphe d'états (params+tags+événements timeline, anti-foot-sliding), import glTF squelette+clips, `AnimBridge` Forms→runtime | `engine/anim/Anim.*`, `engine/assets/GltfMesh.*`, `world/scene/AnimBridge.*` |
| H6 | Seam miniaudio : bus fixes, play 2D/3D, crossfade musique, **backend null pour tests**, `playTestTone` | `engine/audio/Audio.*`, lib `meadows-audio` |
| H7 | **GameplayCues** (émission sim → handlers runtime, `CueTable` avec fallback hiérarchique), `evaluateSchedule` (fenêtres+minuit+override par mod), `FurnitureOccupancy`, `fx::ParticleSim` (preuve : cue de hit → étincelles dans la Combat Arena), interface `nav::Navigator` + `GridNavigator` (A* 2D) | `gameplay/cue/*`, `gameplay/ai/ScheduleSystem.*`, `gameplay/interaction/Furniture.*`, `engine/fx/*`, `engine/nav/Nav.hpp`, `world/ai/GridNavigator.*` |
| H8 | Spawners Light/Marker/Trigger/Furniture, **expansion de prefabs** (GUIDs enfants dérivés `Guid::combine` — déterministes, ciblables par saves/patches), câblage MeshRender par réflexion, `RenderSnapshot.meshes` (contrat), **preuve MaterialForm→cube texturé stylisé** dans LandscapeScene | `world/scene/Spawner.cpp`, `world/scene/Components.hpp`, `game/SceneSubmit.*`, shaders `mesh.vert/frag` |

## Comment remplir, module par module

### Physique / contrôleur — REMPLI en partie (chantier 1, `docs/CHANTIER-1.md`)
- ✅ FAIT : `addHeightField` (façade) + `game/TerrainCollision` (tuiles
  autour du joueur, échantillonnées de TerrainNoise::height) ; le joueur
  1re personne = `CharacterBody::move()` piloté par les stats dérivées
  (B5.5). RESTE : `addStaticMesh` (triangle mesh Jolt) pour la collision
  des cellules/kits au chargement (chantier 2), triggers sensors.
- Le joueur/PNJ = `CharacterBody::move()` piloté par l'intent (le
  contrôleur POSSÈDE le mouvement — anims in-place calées dessus).
- Triggers : corps sensors Jolt → `TriggerVolume.event` sur l'EventBus.
- Le JobSystemSingleThreaded de Jolt est un choix (déterminisme §8) — ne
  le paralléliser qu'avec preuve de profiling ET validation du dev.

### Animation — REMPLI (chantier 1 : B2/B3/B6)
- ✅ FAIT : GPU skinning (`loadGltfSkinnedMesh` + remap JOINTS_0
  parents-first, `skinned.vert` palette SSBO binding 2), graphe piloté par
  la vitesse réelle, `world::resolveActorVisual` (ActorForm → rig/mesh/
  teinte). Le runtime par PNJ vit côté scène (LandscapeScene::Npc) — à
  généraliser en composant quand le chantier « vivant » multipliera les
  acteurs.
- RESTE : layers/masks haut-bas du corps, events → EventBus/GAS (fenêtres
  de hit) — chantier « vivant ».
- Les événements timeline (`AnimEventForm.name`) → EventBus/GAS : « Hit »
  ouvre la fenêtre de dégâts, « Footstep » → cue audio par matériau.

### UI
- Écrans : `UiScreenForm` → `showScreen(name)` avec pile modale + overlay
  HUD ; les roots de documents = les `ui/` de CHAQUE plugin du stack (le
  UiDemoScene n'en branche qu'un — généraliser via PluginStack.baseDir).
- Data binding : façade DataModel dans UiSystem (les types Rml restent
  dans le .cpp) ; brancher santé/inventaire dessus.
- Clavier/gamepad : compléter processKey/processText (souris déjà là).

### Audio
- Résolveur SoundForm : variantes par poids (`SoundVariantForm`), jitters,
  chemin par AssetDatabase → `play()`. Ambiances par cellule/météo via les
  slots crossfade. Cue handlers → `play()`.

### Cues / FX
- Handlers standard dans le runtime : particules (`fx::ParticleSim` +
  `ParticleForm`), son, shake — résolus par `CueTable` (fallback
  hiérarchique déjà là). Points d'émission : combat (fait pour la preuve),
  applyEffect (statuts), footsteps.
- Particules : émetteurs continus (rate/duration) puis formes ; rendu 3D
  en quads caméra dans le paysage.

### Schedules / mobilier / nav
- `ScheduleAgent` (composant runtime) : cache l'intent, réévalue au
  changement d'heure (pas chaque frame) ; interruptions par pile d'intents
  (combat/dialogue par-dessus, reprise après).
- Flux mobilier : claim → nav vers le point → anim (`animTag`) → effect
  GAS pendant l'usage → release. Le joueur ouvre en plus
  `FurnitureForm.screen`.
- Recast/Detour derrière `nav::Navigator` quand le monde 3D existe ;
  jusque-là `GridNavigator` suffit. Long chemins → pattern worker/queue.

### Monde 3D / renderer
- Résidence mesh/material : un cache façon `TextureCache` (guid → GPU,
  placeholder en attendant — JAMAIS bloquer le spawn, §7) consommant
  `RenderSnapshot.meshes`. Le cube H8 est le contrat de rendu à généraliser
  (instancing par model+material ensuite).
- Lumières locales : collecter les `LightSource` de la cellule → UBO des N
  plus proches par objet (design MEADOWS-PLAN §B) ; ombres = 1-2 lumières
  clés par intérieur max.
- Streaming 3D : le pattern chunks du renderer paysage EST le modèle
  (worker → queue → upload budgété, éviction hystérésis, génération).

### Éditeur
- L'éditeur de niveau = EditSession + PropertyGrid (réutiliser tels quels)
  + picking/gizmos dans une scène 3D ; « create prefab from selection » =
  createForm(PrefabForm) + copies des refs avec `prefab` = le nouveau guid.
- Undo/redo, export, load order : déjà dans EditSession/PluginsPanel.

## Audit de compatibilité (2026-07-06, demandé par le dev)

Vérification croisée renderer ↔ plugins ↔ passe horizontale. **Corrigé
pendant l'audit :**
- **Cooker réparé** : il n'enregistrait QUE les CoreForms (lacune
  antérieure à la passe) — il enregistre désormais toutes les familles et
  linke world+narrative. Preuve : `cook`/`uncook` round-trip exact de
  `landscape.toml` (17 records) et `base.toml`, zéro warning.
- **LandscapeTuningForm/WeatherForm migrées dans meadows-data**
  (`data/forms/LandscapeForms.hpp`) : des Forms dans l'exécutable étaient
  invisibles aux outils. Le nom réfléchi est non-qualifié → TOML intact ;
  `game/scenes/LandscapeTuning.hpp` reste en shim d'alias.

**Compatible par construction (vérifié) :** types inconnus = skip logué
(un plugin exporté avec de nouveaux types ne casse aucun ancien
chargeur) ; les nouveaux champs sont APPENDus (ordinaux binaires par
fieldId — sûrs) ; les bindings GL du cube H8 (UBO 1) ne collisionnent ni
avec ShadowUbo (autre passe) ni avec les textures (namespaces GL
distincts) ; `extractScene` sur un monde 2D sans MeshRender = no-op ;
double enregistrement de types = no-op (emplace).

**Écarts CONNUS à combler dans les verticales (pas des bugs) :**
1. **`plugins.toml` n'est lu que par l'EditorScene** — WorldDemoScene/
   CombatArena chargent base.toml+mod en dur, LandscapeScene charge
   landscape.toml seul. La verticale « interfaces/monde » bascule tout le
   monde sur `loadPluginStack` (et unifie l'enregistrement des types —
   les 2 sites complets actuels : EditorScene::onEnter et cooker Main).
2. **Les exports de l'éditeur vivent dans le data/ du BUILD dir** (copié
   post-build depuis la source) : un rebuild n'écrase pas les nouveaux
   fichiers mais la divergence source/build est un piège — rapatrier à la
   main ce qu'on veut garder, jusqu'à un vrai flux d'authoring.
3. ~~`RenderSnapshot.meshes` n'a pas encore de consommateur~~ **CLOS
   (chantier 1 B1)** : `game/MeshCache` (résidence async, placeholder) +
   `drawSceneMeshes` dans LandscapeScene ; le cube H8 est supprimé.
   L'instancing par (model, material) reste l'étape suivante du contrat.
4. `forEach`/`childrenOf`/expansion de prefab = scans O(N) — l'item
   « index secondaires FormDatabase » (MEADOWS-PLAN §J, P1) les remplace
   quand le volume le justifiera.

## Pièges connus (payés une fois, ne pas repayer)

- `ma_sound_group` = nœud du graphe miniaudio → adresses STABLES (uptr),
  jamais par valeur dans un conteneur.
- RmlUi : géométrie compilée = buffers retenus ; couleurs prémultipliées ;
  UN SEUL UiSystem par process (interfaces globales).
- Jolt : Factory/RegisterTypes globaux refcountés (plusieurs mondes OK) ;
  `USE_STATIC_MSVC_RUNTIME_LIBRARY OFF` obligatoire (CRT dynamique).
- Namespace `ui` : `game::ui` (panels) masque `::ui` (UiSystem) — qualifier
  `::ui::` côté game/.
- glm : `Defines.hpp` ne tire que `glm/fwd.hpp` — inclure `glm/glm.hpp` (+
  `gtc/quaternion.hpp`) dans tout header qui déclare des membres Vec/Quat.
- Readback GPU→CPU : pattern staging (copyBuffer vers buffer `readback`),
  jamais lire le SSBO de travail (leçon brique 26 du renderer).
- Les templates de prefab ont `prefab` ≠ 0 et ne spawnent JAMAIS seuls
  (CellLoader les saute) ; l'expansion vit dans `Spawner::spawn`.
