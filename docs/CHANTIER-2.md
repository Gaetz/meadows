# Chantier 2 — Monde habitable : « un village et une maison où entrer »

> **FAIT (2026-07-06, exécuté d'une traite sur demande du dev — cadence
> brique-par-brique suspendue ; 244 tests / 77 817 assertions verts) —
> VALIDATION VISUELLE DEV EN ATTENTE** (liste de tests transmise). B10
> (splat) non tirée. Écarts au plan, décisions prises en route :
> - **Ordre réel** : B1→B2→B8 (terrain AVANT le village : l'esplanade en
>   dépend)→B5→B6→B7→B3→B4→B9.
> - **B8** : le pointeur d'overlay voyage DANS `TerrainParams` (copié par
>   valeur jusque dans les workers → zéro changement de signature) ; les
>   instances publiées sont IMMUABLES et jamais libérées avant la fin du
>   process (workers). Outil offline : `cooker terrain-pad` (a généré
>   l'esplanade du village, 109.6 m — hauteur gardée par un doctest).
> - **B6** : kit = MedievalVillageMegaKit (glTF, mètres) ; ses textures
>   sont bakées en COULEUR MOYENNE par matériau dans les vertex colors à
>   l'import (look albédo plat ; l'échantillonnage réel = pipeline KTX2
>   plus tard). Nouveau champ `StaticForm.snapToGround` (false = y absolu,
>   modules de bâtiment) ; le snap saute aussi les cellules intérieures.
> - **B7** : `DoorForm.targetMarker` = guid d'une RÉFÉRENCE de marker (la
>   destination se résout 100 % en records, cellules déchargées incluses) ;
>   interaction par proximité+regard (pas de raycast physique) ; gel du
>   joueur pendant le fondu (couvre le cook async des colliders du sol).
> - **B3/B4** : ImGuizmo épinglé sur un COMMIT master (aucun tag ne suit
>   imgui 1.92 ; sources sous src/ ; IMGUI_DEFINE_MATH_OPERATORS requis).
>   Le gizmo écrit l'entité en drag et COMMIT au relâchement (1 undo).
>   Export → `data/mods/level-edits.toml`, rechargé au prochain run.
>   Undo/redo ne resynchronise pas les entités vivantes (v1, documenté).
> - **Piège payé (post-validation)** : en intérieur, NE PAS couper la
>   copie couleur/profondeur de la scène — le **SSAO lit la copie de
>   profondeur chaque frame** ; la couper laissait l'AO de l'extérieur en
>   surimpression fantôme. Ne couper que l'eau et l'occlusion Hi-Z.
> - **B9** : brosses raise/lower/flatten/smooth ; publication d'un overlay
>   NEUF par coup de pinceau (relâchement) + regenerate global (invalidation
>   par chunk = optimisation future) ; « Save terrain to mod » écrit les
>   .ter + records TerrainPatchForm dans la session (guids d'assets
>   déterministes par chunk).

## Contexte

Deuxième chantier de `docs/MEADOWS-PLAN.md`. Objectif : cellules 3D
chargées autour du joueur, un éditeur de niveau qui écrit des plugins
(le dev assemble son village AVEC), lumières locales, un intérieur où
entrer par une porte, et le **sculpt de terrain** (décision dev
2026-07-06 : inclus dans ce chantier — heightmaps auteurées moddables).

**⚠ Exécution probablement post-Fable (abonnement fini le 7/07).** Ce plan
est donc un contrat : suivre les seams cités (HORIZONTAL-PASS + ce
fichier), ne PAS redessiner ; une brique à la fois, build vert + tests
verts + preuve visuelle + STOP validation dev ; relire
`docs/CHANTIER-1.md` § pièges payés avant de commencer.

**Décisions dev (2026-07-06)** :
- Kit de modules : **pack Quaternius CC0 déposé par le dev** (workflow
  UAL) ; fallback boîtes si absent au moment de la brique.
- **Sculpt de terrain INCLUS** (B8-B9) ; peinture de splat = brique
  optionnelle de fin (B10).
- Gizmos : **ImGuizmo** (MIT, single-header, pin CPM) — outil dev ImGui,
  conforme §3.

## Acquis à réutiliser (ne pas recréer)

- `WorldspaceForm` (cellSize, interior) / `CellForm` (grid, interior) /
  `WorldModel` (cellAt/referencesIn) / **`CellLoader`** (load/unloadCell,
  l'interface EST déjà la bonne — `world/streaming/CellLoader.hpp`).
- `data::EditSession` (drafts par réflexion, undo/redo, **exportPlugin**)
  + `game/ui/PropertyGrid` + PluginsPanel — l'éditeur de niveau se
  construit LÀ-DESSUS. `game/WorldEditor.{hpp,cpp}` (meadows-runtime) est
  l'embryon PRÉ-EditSession : le remplacer, pas l'étendre.
- Expansion de prefabs dans `Spawner::spawn` (H8, doctestée).
- `game/MeshCache`/`TextureCache` (résidence async), `drawSceneMeshes`,
  `extractMeshes` (chantier 1).
- `phys::PhysicsWorld` (+`addHeightField`), `game/TerrainCollision`.
- `render::terrain::height/normal/noise01` (`TerrainNoise.hpp`) — PUR ;
  consommé par : TerrainSystem (workers), GrassSystem, VegetationSystem
  (scatter), WaterSystem (pool bake), TerrainCollision, LandscapeScene
  (snaps sol). B8 les rebranche TOUS sur un seam unique.
- `LightForm` + `LightSource` (composant) + spawner (H1/H8) — le rendu
  manque. Design HORIZONTAL-PASS : UBO des N lumières proches.
- Catégorie `Door` existe (enum FormCategory) — le DoorForm reste à créer.
- Touche `E` libre en mode Play (interaction).

## Les briques (1 brique = build vert + 234+ tests verts + preuve + STOP dev)

### B1 — Cellules 3D + chargement par distance — ✅ FAITE (2026-07-06)

> Livré : `world/streaming/CellStreamer` (anneau charge-2/décharge-3,
> hystérésis, doctesté avec entités qui vont et viennent) ; adventure.toml
> = Overworld 64 m + 3 cellules + toutes les refs cellées (le joueur reste
> SANS cellule = persistant, convention actée) ; scène : refs persistantes
> au démarrage, `snapCellEntities` (snap IDEMPOTENT : y = terrain +
> y auteuré relu du record — l'ancien `+=` ne survivait pas au respawn),
> `refreshNpcs` (prune les entités mortes + construit les nouvelles — les
> PNJ suivent leur cellule), compteur « cells loaded » (F1). Preuve
> pop-in : gros rocher à (150, 360), 2 cellules à l'est.
- adventure.toml : `WorldspaceForm` "Overworld" (cellSize **64** — alignée
  sur les chunks terrain) + CellForms de la zone démo ; les ReferenceForms
  existantes gagnent leur `cell`.
- LandscapeScene : remplacer la boucle « spawn tout » (B1 chantier 1) par
  WorldModel + CellLoader, chargement SYNCHRONE des cellules dans un rayon
  autour du joueur (2 cellules), déchargement avec hystérésis (rayon 3) —
  le pattern TerrainCollision. L'async + persistance = chantier 5, ne pas
  l'anticiper.
- Doctests : mapping position→cellule, load/unload par déplacement du
  focus (headless, TOML inline pattern CellLoaderTest).
- Preuve : compteur « cells loaded » dans le panneau F1 ; s'éloigner/
  revenir → les rochers/PNJ disparaissent/réapparaissent proprement.

### B2 — Collision des statics (`addStaticMesh`) — ✅ FAITE (2026-07-06)
- Façade : `PhysicsWorld::addStaticMesh(vertices, indices, position,
  rotation, scale)` (Jolt MeshShapeSettings ; AUCUN type Jolt en header).
- Au spawn d'un Static avec `StaticForm.collides` : créer le collider
  depuis le mesh CPU (MeshCache garde/refournit les données CPU — petite
  extension : `cpuMesh(guid)`), `removeBody` au despawn de la cellule.
- Doctest : capsule bloquée par un mesh box incliné ; preuve : le joueur
  ne traverse plus les rochers.

### B3 — Éditeur de niveau v1 : sélection & placement — ✅ FAITE (2026-07-06)
- Dep CPM : **ImGuizmo** (pin dernier tag, MIT).
- Mode « Édit » dans LandscapeScene (3e mode après Fly/Play, section F1) :
  - **Picking** : rayon caméra→curseur vs AABB des entités à MeshRender
    (CPU, pas besoin de physique) ; surbrillance de la sélection (teinte).
  - **Gizmo** ImGuizmo translate/rotate/scale + snap ; écrit via
    **EditSession.setField** sur la ReferenceForm (PAS sur l'entité seule)
    puis respawn de la référence — l'éditeur édite des RECORDS (§5).
  - **Palette** : liste des StaticForm/LightForm/FurnitureForm/MarkerForm/
    PrefabForm (forEach + recherche editorId), clic → placement au point
    terrain sous le curseur (createForm ReferenceForm dans la cellule
    visée). Duplication (Ctrl+D), suppression (`enabled=false` si record
    de base, vrai delete si créé dans la session).
  - Undo/redo : EditSession (existant).
- Doctest : round-trip setField position → exportPlugin → re-résolution =
  position patchée (pattern EditSessionTest, sur ReferenceForm).
- Preuve : déplacer un rocher au gizmo, poser 3 nouveaux props.

### B4 — Éditeur v2 : export & prefabs — ✅ FAITE (2026-07-06)
- « Save as plugin » (exportPlugin existant, fichier dans data/mods/ —
  rappel écart n°2 HORIZONTAL-PASS : les exports vivent dans le BUILD
  dir).
- **« Create prefab from selection »** : createForm(PrefabForm) + copies
  des ReferenceForms sélectionnées en templates (`prefab` = nouveau guid,
  transforms RELATIVES au pivot = centroïde) ; placement de prefabs par
  la palette (l'expansion H8 fait le reste).
- Doctest : sélection → prefab → export → reload → expansion identique.
- Preuve : grouper un banc de rochers en prefab, le poser 3 fois,
  recharger la scène — tout est là. **Le dev peut assembler son village.**

### B5 — Lumières locales — ✅ FAITE (2026-07-06)
- `render/landscape` : collecte des entités `LightSource` de la scène →
  **UBO `LightsUbo`** (jusqu'à 16 lumières : position, couleur×intensité,
  rayon, cône spot ; sélection = les plus proches de la caméra, tri
  stable). Binding nouveau (3 est pris par cloud map dans la passe opaque
  → prendre 5). Évaluation dans `mesh.frag`/`skinned.frag` (+ kit) :
  falloff lisse au carré du rayon, PAS d'ombres (v1 ; ombres 1-2 lumières
  clés = brique ultérieure). `flicker` : modulation sin+hash sur
  l'intensité, phase par index.
- Doctest : sélection des N proches déterministe. Preuve : 2 torches
  (LightForm posées à l'éditeur !) qui flickent dans le village de nuit.
- NE PAS toucher terrain.frag/grass (paysage sun-only, décision B-table) —
  v1 éclaire les meshes/personnages seulement ; noter si ça se voit trop.

### B6 — Kit de modules + la maison (intérieur) — ✅ FAITE (2026-07-06)
- Kit : pack modulaire Quaternius déposé par le dev dans models/ (sinon
  fallback : StaticForms sur des .gltf boîtes minimales écrits à la main).
  StaticForms du kit (mur/sol/porte/toit...) dans adventure.toml (ou un
  village.toml dédié — 2e plugin, bon test §5).
- Worldspace intérieur : `WorldspaceForm` (interior=true) + CellForm(s) +
  la maison assemblée à l'éditeur (sols, murs, table, lit FurnitureForm,
  2 LightForm).
- **Mode intérieur du renderer** (LandscapeScene) : quand le worldspace
  courant est interior → SKIP terrain/grass/vegetation/water/sky/shadows
  soleil/occlusion ; clear couleur sombre ; ambiant constant faible +
  lumières locales (B5) ; fog off. Piloter par un état `activeWorldspace`.
- Preuve : scène de la maison rendue correctement (pas de ciel qui fuit,
  pas de terrain).

### B7 — Portes & transitions — ✅ FAITE (2026-07-06)
- `DoorForm` (nouveau, world/worldspace) : model/material (visuel via le
  câblage MeshRender réflexif existant), `targetMarker` (guid MarkerForm —
  position/orientation d'arrivée), champs append-only. Catégorie Door
  (enum déjà là) + spawner (MeshRender + composant `DoorTarget`).
- Interaction : en Play, raycast caméra 3 m (physique B2 ou AABB) ; si
  porte visée → prompt ImGui « [E] Entrer » ; E → transition :
  unloadAll du worldspace courant, load des cellules du worldspace cible
  autour du marker, téléporter la capsule au marker, fondu au noir ~0.3 s
  (quad plein écran alpha — PAS de nouveau système).
- États de porte persistés = chantier 5 (saves) ; verrous = P1.
- Preuve : **LA preuve du chantier** — marcher dans le village, E sur la
  porte, être dans la maison, ressortir.

### B8 — Terrain auteuré : le modèle de données — ✅ FAITE (2026-07-06)
- **Principe** : hauteur finale = bruit procédural de base + **delta
  auteuré**. Les deltas sont des GRILLES par chunk de 64 m stockées en
  ASSETS binaires (la réflexion est plate — jamais de tableau dans un
  Form) : format `.ter` minimal (magic, taille n, n×n f32 deltas).
  Un `TerrainPatchForm { chunkX, chunkZ, asset guid }` (world/) référence
  chaque grille. **Moddabilité gratuite** : un mod override le `.ter` par
  guid (VFS §5) ou patche le record.
- **Le seam** : `world::TerrainHeightField` — { TerrainParams, patches
  résolus (map chunk→grille, immuable après build) } avec
  `height(x, z)` = bruit + delta bilinéaire. **Rebrancher TOUS les
  consommateurs** dessus : TerrainSystem (workers — la struct est
  read-only donc thread-safe), GrassSystem/VegetationSystem (scatter),
  WaterSystem (pool bake), TerrainCollision, snaps de LandscapeScene.
  Mécanique : remplacer `terrain::height(params, x, z)` par
  `field.height(x, z)` — AUCUN changement d'algorithme.
- Doctests : delta appliqué bilinéairement, bords de chunks sans couture
  (delta partagé), sans patch = bit-identique au bruit actuel (test de
  non-régression IMPORTANT), TerrainCollision suit le champ patché.
- Preuve : un .ter écrit à la main bombe une colline visible (rendu +
  collision d'accord).

### B9 — Sculpt dans l'éditeur — ✅ FAITE (2026-07-06)
- Mode « Sculpt » de l'éditeur : brosses **raise/lower/flatten/smooth**
  (rayon/force sliders), appliquées au point terrain sous le curseur →
  édition des grilles delta en mémoire (chunks touchés marqués dirty).
- Régénération live : chunks terrain dirty re-générés (le pattern
  regenerate existant), tuiles de collision invalidées, scatter regénéré
  au save (pas en live — trop cher).
- « Save terrain » : écrit les `.ter` (dossier du plugin de sortie) +
  records TerrainPatchForm via EditSession → le plugin exporté.
- Preuve : aplatir une esplanade pour le village, sauver, relancer — le
  terrain est là, le village posé dessus, la collision suit.

### B10 (OPTIONNELLE) — Peinture de splat — ⏭ PASSÉE (option non tirée)
- Même modèle que B8 : grilles d'override des poids de matériaux par
  chunk (assets + Form), brosse « paint » (herbe/roche/neige/sable),
  terrain.frag échantillonne l'override si présent. À faire seulement si
  B1-B9 sont validées et qu'il reste l'envie.

### B11 — Clôture
- `docs/CHANTIER-2.md` (journal + pièges payés), MEADOWS-PLAN (coches
  A/B/D + table d'état, chantier 3 = PROCHAIN), HORIZONTAL-PASS (seams
  remplis : addStaticMesh, éditeur, lumières), userdoc (world-and-levels :
  cellules/portes/terrain auteuré ; tools : éditeur de niveau), mémoire.

## Fichiers principaux

- Nouveaux : `world/terrain/TerrainHeightField.{hpp,cpp}` (B8, meadows-
  world ou engine/render/landscape — ATTENTION DAG : les patches viennent
  de data/, le renderer est engine/ → le champ construit côté world/
  runtime et passé aux systèmes render par valeur/référence de struct
  plate, règle n°2 HORIZONTAL-PASS), `world/worldspace/DoorForm` (dans
  WorldForms), `game/LevelEditor.{hpp,cpp}` (remplace WorldEditor),
  formats `.ter`, `game/data/base/village.toml`.
- Étendus : Physics (addStaticMesh), MeshCache (cpuMesh), CellLoader
  (rien ? il est prêt — c'est la scène qui l'adopte), LandscapeScene
  (cellules, mode Édit, mode intérieur, transitions), mesh/skinned.frag
  (LightsUbo), TerrainSystem/Grass/Vegetation/Water (TerrainHeightField),
  adventure.toml.
- CMake : ImGuizmo (CPM, pin).

## Vérification

Par brique : build + suite headless complète + preuve visuelle + STOP dev.
Tests nouveaux : cellules (mapping/load-unload), mesh collider, EditSession
round-trip ReferenceForm, prefab depuis sélection, sélection lumières,
TerrainHeightField (non-régression bit-exacte sans patch + bilinéaire +
coutures + collision). Fin de chantier = B11.

## Risques / garde-fous

- **B8 est LE morceau d'architecture** : ne pas inventer un autre stockage
  (pas de Form à tableau, pas de base parallèle — assets + records, §5).
  Le test de non-régression bit-exacte protège tout le paysage existant.
- L'éditeur écrit des RECORDS via EditSession — jamais l'entité seule
  (le WorldEditor embryonnaire faisait ça : c'est ce qu'on remplace).
- Ne pas anticiper le streaming async/persistance (chantier 5).
- Renderer : ne toucher les passes que pour le mode intérieur et le
  LightsUbo — pas de gold-plating (§1 CLAUDE.md).
- ImGuizmo : vérifier la convention de matrices (column-major glm OK) et
  le mode « local/world » ; si friction > 1 brique, fallback = drag
  d'axes manuel simple (translate seulement) et noter.
