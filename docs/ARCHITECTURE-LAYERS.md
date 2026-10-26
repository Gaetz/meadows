# Couches et modules — l'état mesuré du moteur (2026-10-01, garde-fou 2026-10-26)

> Référence : *Game Engine Architecture* (J. Gregory), fig. « runtime
> engine architecture » — des couches où le haut appelle le bas, jamais
> l'inverse. Ce document compare cette pile à ce que le code fait
> VRAIMENT, graphe d'inclusion à l'appui. Il se rejoue :
> `python tools/scripts/layers_audit.py` (modules, arêtes, arêtes qui
> remontent, cycles) et il est **un test** : `ctest -R layers_audit`
> compare les violations trouvées à la baseline
> `tools/scripts/layers_baseline.txt` (§5). À relire avant de créer un
> module ou de déplacer un sous-système.

## 1. Correspondance avec la pile de Gregory

| Couche Gregory (bas → haut) | Chez nous | Verdict |
|---|---|---|
| SDK tiers | SDL3, GLM, flecs, Jolt, sol2/Lua, miniaudio, RmlUi, ImGui, cgltf, stb, spdlog, glad, shaderc/Vulkan (CPM, épinglés) | ✓ |
| Indépendance plate-forme | `engine/platform` (SDL3 = la couche ; `posix/` pour le reste), en-têtes sans type natif, sélection par CMake | ✓ |
| Systèmes cœur | `engine/core` (log, asserts, jobs, math, Guid, Rng, horloge, Result, files) + `engine/reflect` (Gregory y range RTTI/sérialisation) | ✓ |
| Ressources | `engine/assets` (DB GUID, loaders, résidence) et `data/` (Forms, plugins, patchs : NOTRE gestionnaire de ressources de jeu, au-dessus de core+reflect seulement) | ✓ |
| Renderer bas niveau | `engine/rhi` (GL 4.6 + Vulkan derrière `rhi::Device`) | ✓ |
| Collision/physique, HID, audio, profilage, réseau | `engine/physics` (Jolt pimpl), input dans `platform`, `engine/audio`, FrameProbe/GpuProbe ; pas de réseau (solo) | ✓ |
| Scene graph/culling, VFX, front end, animation | `engine/render` (WorldRenderer, landscape, occlusion, GI — bas niveau ET scène dans une seule lib), `engine/fx`, `engine/ui` (RmlUi), `engine/anim` (headless) | ✓ avec deux cycles (§3) |
| Fondations gameplay | `engine/ecs` (flecs), `gameplay/` (GAS, IA, combat, événements), `world/` (worldspaces, streaming, spawner), `script/` (Lua), `quest/` | ✓ ; `world` est AU-DESSUS de `gameplay` (§3.4) |
| Sous-systèmes spécifiques au jeu | `game/` (true-adventurer : scènes, directeurs, contrôleurs, panneaux, data) | ✓ mais 36 k lignes, `LandscapeScene` dominante |

Deux pièces n'ont pas de case évidente : `engine/terrain` (synthèse
procédurale, 11 k lignes, 41 TU) et `engine/dungeon` (le bake de mine,
2,4 k lignes). Gregory les mettrait côté « génération de contenu » (outils
ou jeu), nous les avons sous `engine/` parce que le renderer et le monde
les consomment tous deux ; c'est tenable tant qu'elles ne dépendent que de
`core`/`assets` (c'est le cas).

## 2. Ce que la mesure confirme

Graphe d'inclusion (fichiers `.hpp/.cpp`, en-têtes du projet seulement),
compté par paire de modules :

- **Aucune inclusion de `engine/*` vers `data/`, `world/`, `gameplay/`,
  `script/`, `quest/`, `game/`** : le moteur ne connaît pas le jeu.
  (`engine/*` never includes `data/*`, CLAUDE.md §4 — tenu.)
- **Aucune inclusion de la simulation (`data`, `world`, `gameplay`,
  `script`, `quest`) vers `engine/rhi`, `engine/render`,
  `engine/platform`** : l'invariant §2.10, verrouillé par
  `meadows-simlink` (édition de liens de toute la sim sans
  `meadows-render`).
- Les grandes flèches vont toutes vers le bas : `game/scenes` → gameplay
  (210), data (108), world (84), core (75), render (41) ; `gameplay` →
  core (43), data (43) ; `world` → data (35), core (30), terrain (17),
  gameplay (15) ; `data` → core (20), reflect (8) ; `render` → core (93),
  rhi (70), assets (19), terrain (12).
- Tailles : render 22 k, terrain 11 k, rhi 7,5 k, gameplay 9 k, world
  7 k, data 5 k, game 36 k (dont `game/scenes` 25 k), tests 29 k.

## 3. Les écarts trouvés (par ordre d'importance)

1. **Cycle `engine/(racine)` ↔ `engine/render` ↔ `engine/ui`.**
   Mesuré le 2026-10-01 : `Engine.hpp` et `FrameContext.hpp` incluaient
   `render/Camera2D.hpp`, `WorldRenderer.cpp` / `FxRenderer.cpp` incluent
   `engine/FrameContext.hpp` en retour, et `WorldRenderer.cpp` incluait
   `ui/UiSystem.hpp` (il dessine l'UI de jeu dans sa frame) pendant que
   `UiSystem.cpp` inclut `render/ShaderLibrary.hpp`. Trois modules de
   même étage qui se tenaient par la main.
   **Résolu pour deux arêtes sur trois (2026-10-26)** : `Engine` ne
   connaît plus `render::` — le `SpriteRenderer` + la `Camera2D` sont un
   sous-système monté du `WorldRenderer` (`RendererConfig::sprites`,
   accès `sprites()` / `camera2D()`), possédé par la scène 2D
   (`WorldDemoScene`, frame owner comme les scènes 3D) ; `FrameContext`
   ne porte plus que `Device&` + `CommandBuffer&` + dt/taille ; le
   crochet UI de `RenderView` est un `render::InPassDraw`
   (`std::function`) rempli par la scène, `WorldRenderer` n'inclut plus
   `engine/ui`. `FrameContext` (une struct RHI pure) vit sous
   `engine/rhi/FrameContext.hpp` ; `engine/FrameContext.hpp` n'est plus
   qu'un en-tête de renvoi pour la racine et le jeu, et `render` inclut
   le chemin `rhi`. L'anneau racine → ui → render → racine est fermé.
2. **`engine/assets` → `engine/anim`** (`GltfMesh.hpp` incluait
   `Anim.hpp` pour le squelette/skin) : les ressources dépendaient d'un
   module au-dessus d'elles. **Résolu dans le sens de la dépendance
   (2026-10-26)** : les importeurs `loadGltfSkeleton` /
   `loadGltfAnimations` (+ `GltfClip`) vivent dans
   `engine/anim/GltfSkeleton.{hpp,cpp}` (`anim::`), qui consomme la
   plomberie cgltf partagée `engine/assets/GltfParse.hpp`
   (`ParsedGltf`, `topologicalJointOrder` — l'unique réordonnancement
   des joints, partagé par le squelette et le maillage skinné) : `anim`
   (rang 3) inclut `assets` (rang 2), flèche descendante. `GltfMesh.hpp`
   n'inclut plus `anim` ; `NpcSpawner` et `AnimPreviewPanel` incluent
   `engine/anim/GltfSkeleton.hpp` et épellent `anim::`.
3. **Une seule bibliothèque `meadows` pour neuf dossiers** : core,
   platform, reflect, assets, terrain (+ generation), dungeon, anim, fx,
   nav sont liés en UN `STATIC`. Les couches existent dans
   l'arborescence, pas dans les unités de lien, donc rien n'empêche
   `assets` → `anim` ou `terrain` → n'importe quoi. `meadows-render`
   mélange de même rhi, render et ui. L'édition de liens n'arbitre que la
   frontière sim / rendu. Remède progressif : découper quand un module
   grossit (terrain, assets), et faire tourner `layers_audit.py` comme
   test (arêtes qui remontent = échec) — c'est le garde-fou le moins cher.
4. **`world` → `gameplay` (15 inclusions)** : `Spawner.cpp` câble les
   composants GAS, `AnimBridge` lit `CharacterForms`/`Condition`, `KillZ`
   appelle `Damage`, `TriggerSystem` l'`EventBus`, `FormCategory` les
   `FurnitureForms`. CMake le dit aussi (`meadows-world` lie
   `meadows-gameplay`). Ce n'est pas une violation : le spawner EST
   l'instanciation d'entités de §2.7, il doit connaître les composants.
   Mais CLAUDE.md §4 liste `world` avant `gameplay`, dans l'ordre inverse
   de la dépendance réelle ; la pile vraie est `data` → `gameplay` →
   `world` → `script`/`quest` → `game`. **Le script range désormais
   `world` (rang 6) au-dessus de `gameplay` (rang 5)** : ces 16
   inclusions ne sont plus un faux positif.
5. **`game` ↔ `game/scenes`** : `main.cpp` inclut les scènes (normal),
   mais les scènes incluent `game/LevelEditor.hpp`, `game/AllForms.hpp`
   (normal aussi : des services de l'application). Un cycle INTERNE à la
   couche jeu, toléré ; il mesure surtout que `LandscapeScene` et ses
   contrôleurs forment l'objet-dieu que l'audit de juillet a réduit sans
   le dissoudre.
6. **`engine/dungeon` et `engine/terrain/generation` dans `engine/`** :
   de la génération de contenu, 13 k lignes, sous le moteur. Tenable
   (voir §1), à surveiller : le jour où le bake de mine lit un Form, il
   descend dans `world/` ou monte dans `tools/`.

## 4. Verdict

La discipline de Gregory est **respectée là où elle compte et là où elle
est verrouillée** : le moteur ignore le jeu, la simulation ignore le
rendu, et les deux frontières sont tenues par l'édition de liens, pas
par la bonne volonté. Les écarts sont tous dans la moitié haute de
`engine/` (le trio racine/render/ui, assets→anim) et dans la granularité
des bibliothèques, qui n'encode qu'une couche sur six. Rien de ce qui
précède ne bloque une brique ; le remède le plus rentable est le garde-fou
(§3.3), le plus utile à la lisibilité est la correction de l'ordre dans
CLAUDE.md §4 (§3.4), les cycles (§3.1, §3.2) se défont en une demi-journée
quand le chantier RENDERER-EXTRACT (RENDERING.md §7) touchera ces fichiers.

## 5. Le garde-fou : rangs, baseline, test

**Rangs** (`RANK` dans `tools/scripts/layers_audit.py` ; un module absent
de la table vaut 9 sous `game/`, 5 ailleurs) :

| Rang | Modules |
|---|---|
| 0 | `engine/core`, `engine/platform` |
| 1 | `engine/reflect` |
| 2 | `engine/ecs`, `engine/rhi`, `engine/assets`, `engine/terrain`, `engine/nav`, `engine/dungeon`, `data` |
| 3 | `engine/anim`, `engine/physics`, `engine/audio`, `engine/fx` |
| 4 | `engine/render`, `engine/ui` |
| 5 | `engine/(root)` (Engine, Game, FrameContext), `gameplay` |
| 6 | `world` |
| 7 | `script`, `quest` |
| 9 | `game`, `game/scenes`, `game/ui` |
| 10 | `tools` |
| 11 | `tests` |

**Violations** = toute arête qui remonte (`A -> B` avec rang(A) <
rang(B)) et toute composante fortement connexe du graphe des modules
(`A <-> B [<-> C]`, membres triés).

**Baseline** : `tools/scripts/layers_baseline.txt`, une violation
tolérée par ligne (le `#` ouvre un commentaire — y écrire l'inclusion
fautive, la ligne se documente). Le verdict :

```
python tools/scripts/layers_audit.py --baseline tools/scripts/layers_baseline.txt
ctest -R layers_audit        # la même commande, enregistrée par tests/CMakeLists.txt
```

- une violation **absente** de la baseline = échec (un nouveau cycle
  casse la suite) ;
- une ligne de baseline dont la violation **n'existe plus** = échec
  aussi (« STALE ») : **le correctif qui résout un cycle retire sa
  ligne**, dans le même commit — la baseline ne peut que fondre ;
- `--write-baseline FICHIER` régénère la liste depuis l'arbre courant
  (puis relire les commentaires) ; `--quiet` n'imprime que le verdict.

Contenu toléré au 2026-10-26 (une ligne) : le cycle interne
`game <-> game/scenes <-> game/ui` (§3.5), qui ne tombera qu'avec
l'objet-dieu.
