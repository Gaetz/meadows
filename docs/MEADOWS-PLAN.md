# MEADOWS-PLAN — Fonctionnalités moteur & outils pour le Skyrim-like

## OÙ ON EN EST (mis à jour 2026-07-22)

| Piste | État |
|---|---|
| Décisions de cadrage (7, dont **jeu 1re personne**) | ✅ actées 2026-07-05/06 (§ Décisions actées, §C.1) |
| **Passe horizontale** (architecture de tous les systèmes) | ✅ FAITE 2026-07-06 — contrat : `docs/HORIZONTAL-PASS.md` (audit de compat inclus) |
| Renderer paysage (briques 27-31) | ✅ **TOUTES FAITES** — 27 (chantier 1), 28-29 (chantier 6, commitées + validées dev 2026-07-07), 30-31 (chantier 7 : cumulonimbus, pluie) ; spec : `docs/RENDERING.md` |
| **Chantier 1 — Socle 3D gameplay** | ✅ FAIT 2026-07-06 — journal : `docs/CHANTIER-1.md` (joueur FPS piloté par ses stats, PNJ 100 % Forms en patrouille, MeshCache/skinning/anim data, collision terrain Jolt, arbres brique 27) |
| **Chantier 2 — Monde habitable** (cellules 3D, éditeur de niveau+gizmos, lumières locales, kit+intérieur+portes, terrain auteuré+sculpt) | ✅ FAIT 2026-07-06 — journal : `docs/CHANTIER-2.md` ; **+ cellules extérieures implicites (briques 1-3, `docs/IMPLICIT-CELLS.md`) et volumes de triggers livrés nuit 2026-07-10** |
| **Chantier 3 — Vivant** (horloge, interaction E, schedules exécutés, IA hostile + combat mêlée 3D, repos) | ✅ FAIT 2026-07-06 (**validation visuelle dev en attente**) — journal : `docs/CHANTIER-3.md`. Reportés : cues/audio (pas d'assets son — dev doit les déposer), Recast (fallback TerrainNavigator), barter (chantier 4), ability GAS formelle pour l'attaque joueur |
| **Chantier 4 — Interfaces** (écrans RmlUi P0, barter, console en jeu, plugin stack unifié) | ✅ FAIT 2026-07-06 (**validation visuelle dev en attente**) — journal : `docs/CHANTIER-4.md`. 9 écrans (HUD/inventaire/conteneur/dialogue/barter/menus/attente/atelier), LoadoutEntryForm (§C.1), écart n°1 HORIZONTAL-PASS clos. Reportés : gamepad, localisation systématique, carte, éditeur d'UI (tous P1) |
| **Chantier 5 — Persistance** (save = couche de patches, mémoire des cellules, streaming lissé) | ✅ FAIT 2026-07-06 (**validation visuelle dev en attente**) — journal : `docs/CHANTIER-5.md`. Save = plugin TOML dans saves/ (F5/F9, menus), couche pending en RAM (cellules qui se souviennent sans disque), enfants de prefab persistés, spawn budgété. Reste : async IO réel (fichiers cuits par cellule) quand le monde dépassera la RAM |
| **Chantier 6 — P1 par valeur** (quête jouable + journal, stats avancées ex-Phase 9, économie/crime v1, passe lighting) | ✅ FAIT 2026-07-06 — journal : `docs/CHANTIER-6.md`. Axes A/C/D commités ; axe B (lighting : spots+falloff+ambiance intérieure, casters mesh/PNJ dans le CSM, grading brique 28, auto-expo brique 29) **validé dev + commité 2026-07-07**. B2b (ombre de lumière clé) → livrée au chantier 7. Les « restent du P1 » ont depuis été résorbés : éditeurs anim/FX + outillage de quêtes (chantier 8), briques 30-31 (chantier 7), localisation (chantier 9) ; reste : musique dynamique (chantier son), éditeur d'UI |
| **Chantier 7 — Graphisme restant** (shafts à poussière 34, contact shadows 33a, terrain shadows + skylighting 33b/c, eau plaçable 32, ombre de lumière clé B2b, cumulonimbus 30, pluie+wetness+occlusion 31) | ✅ FAIT 2026-07-07, **8/8 briques dont la refonte herbe** (réf. dev daniel-ilett/shaders-botw-grass, shaders seuls) — **validation visuelle dev en attente** — journal : `docs/CHANTIER-7.md` ; specs : `docs/RENDERING.md`. Chantiers suivants : 8 outillage, 9 confort & plateforme, 10 son & vie (ordre dev 2026-07-07) |
| Refonte herbe (renderer) | ✅ **VALIDÉE dev** — redo n°2 « Quick_Grass » (`620da69`), tout le tuning au panneau « Grass » (FrameUbo uGrass* + rescatter) ; post-passe perf 2026-07-07 : volume plein + LOD de densité métrique, scatter cell-major, fix crash quit (TerrainParams.patches → shared_ptr) |
| **Chantier perf GPU** (timers GPU RHI, knobs qualité en données, render scale, early-outs PostFx, réflexion, cache CSM…) | 🔨 **P0 FAITE** (timers GPU RHI + HUD F6, `4f4c1e6`) — plan : `docs/RENDERING.md` ; **baseline dev sur les 4 spots EN ATTENTE** — l'ordre des optimisations P1+ sera décidé par la table ; le structurel herbe est différé à la refonte visuelle |
| **Chantier GI — Radiance Cascades 3D** (GI dynamique world-space, technique SWITCHABLE en parallèle du lighting actuel) | ✅ **FAIT 2026-07-11, validé dev — 0,5 ms au F6** (G0-G7c : clipmap voxel + végétation, cascades, apply switchable, multi-bounce, RC-only lights, extension d'intervalle ; rampe finale = PAS FIXE en stops, leçon anti-adaptatif au journal) — journal : `docs/RENDERING.md`. G8 (index spatial) NON nécessaire perf ; à refaire surface quand perception/triggers voudront l'index partagé. Même session : CSM 4096 par défaut + knob, contact×soleil = MAX, fondu stylisé 0.45-0.55, grading OFF par défaut |
| **Chantier « Reliquats P0 » — le vivant 2** (combat mêlée complet : ability GAS + fenêtres de hit AnimEvents + shape casts + attach points + blocage ; perception dédiée + index spatial partagé + IA strafe/fuite/aide ; particules v2 + handlers de cues + footsteps ; mobilier GAS, kill-z/eau, journal de quêtes) | ✅ **FAIT 2026-07-12** — journal : `docs/CHANTIER-P0.md`. Axes A-D livrés : ability GAS + hit À LA LAME (shape casts sur AnimEvents), blocage directionnel, arc + projectiles (A7), épée visible aux sockets ; Perception dédiée + SpatialIndex partagé + strafe/fuite/appel à l'aide + distances par arme ; particules v2 + handlers de cues (shake inclus) + résolveur SoundForm + footsteps par matériau (premiers wavs dev câblés) ; mobilier GAS, kill-z mort franche, VRAIE nage. Lock-on ABANDONNÉ (décision dev). Rounds post-P0 : sneak (crouch + ×0.75 + bruit), tir chargé, économie de flèches, archer PNJ, parade parfaite, esquive, brain Lua. Suivi du **chantier propreté** (R1-R7, `aaa733a..03a2cd9` + fix « l'archer fuit » = maxHealthOverride réel `efdf8e7`) — archer validé dev, 418 tests |
| **Chantier 8 — Outillage** (GameDB duplication+used-by, éditeur de quêtes, éditeur de dialogues, timeline des schedules, outil de synthesis patch §5.1 ; **REPRIS 2026-07-09** : + interfaces visuelles & présentation nodale) | ✅ **TOUTES BRIQUES CONSTRUITES** — 8.1-8.5 le 2026-07-07, 8.6-8.11 la nuit du 2026-07-10 (345 tests) : fenêtre unique « True Adventurer DB » (Browser\|Editor\|Inspector, 8.7b), graphes nodaux anim/quêtes/dialogues (imgui-node-editor vendorisé), timeline des events de clip, builder de conditions, éditeur FX à preview live, éditeurs Effect/Ability — **validation dev EN COURS via `docs/chantier8-test.md`** (8.6 déjà validée) — journal : `docs/CHANTIER-8.md`. La sortie de chaque outil = plugin ordinaire (§5) ; positions de nœuds = side-store éditeur, pas des champs de Forms |
| **Chantier 9 — Confort & plateforme** (gamepad remappable + options, localisation systématique EN base + pack FR, carte raster CPU, save async + mesures, portabilité) | ✅ **FAIT 2026-07-12** (jalons manette + langue validés dev ; carte gardée, palette à retoucher) — journal : `docs/CHANTIER-9.md`. **Décision macOS actée : Vulkan + MoltenVK en chantier POST-DÉMO** (pas de down-port GL 4.1 du renderer 3D, pas de Metal natif) ; vérification Fedora = checklist au journal, à dérouler par le dev |
| **Chantier FOLLOWERS** (compagnons : suivre/combattre/à terre, affinité+recrutement, classes+niveaux, pouvoirs+perks, équipement, mort/tombe, multi+mercenaires ; montures découplées) | ✅ **É0→É10 LIVRÉES + É11 v1 (nuit du 2026-07-12→13, un commit/étape — validation dev en jeu EN ATTENTE)** — plan+statuts : `docs/CHANTIER-FOLLOWERS.md` ; protocole : `docs/FOLLOWERS-TEST.md` ; 3 placeholders (Aldric/Maela/Corvin) + poney tech-proof ; §2.11 « reuse before build » appliqué à chaque commit ; **correctifs post-retour dev 13/07** (`5456c6c`) : gates de dialogue PAR-PARTENAIRE (clauses FollowerActive/FollowerConvalescent) + maxima des followers en FORMULES (maxHealth = 0) ; restes : É8b dépouille portée, radial, suite montures |
| **Session Fable 2026-07-13 (matin) — 11 restes P0/P1 autonomes** (un commit/tâche, `9a9f0e2..a810ada`, 515 tests) | ✅ LIVRÉE : **préview d'anim 3D offscreen dans la DB** (fenêtre « Anim Preview », clip scrub + graphe live, `rhi::nativeTextureId`) ; **condition evaluator sur les transitions d'AnimGraph** (ConditionForm enfants d'une transition, seam callback plat) ; **skills-by-use v1** (SkillForm+seuils=effets, OnAbilityUsed, SavedSkillForm) ; **dégâts de chute** (fallDamage + kill-z létal, knobs StatsTuning) ; **interruption/reprise des schedules** (combat + dialogue OUVERT, re-éval immédiate) ; **console `setstage`** ; **`cooker validate`** (lint : orphelins/deps/dangling guids — la pile base passe) ; **index FormDatabase par type/parent** (forEach/childrenOf O(bucket)) ; **dégâts sournois** (×3 sneak+Calm, mêlée+flèches) ; **bounty PAR FACTION** (témoin, gate garde, SavedBountyForm) ; **duplication + snap éditeur** (Ctrl+D, ImGuizmo snap). Validation dev en jeu à prévoir : préview anim, chute, sournois, bounty, duplication/snap |
| **Chantier VULKAN — backend Vulkan + MoltenVK (renderer final)** | ✅ **FAIT 2026-07-18→19 (V0→V8g)** — la décision « macOS post-démo » du chantier 9 exécutée en avance (dev passé sur MacBook M1). LandscapeScene tourne sur Vulkan/M1, 0 erreur de validation, sync-validation opt-in (`MEADOWS_VK_SYNC_VALIDATION=1`), cache de pipelines persisté, sélection runtime Vulkan→GL + gating `MEADOWS_RHI_VULKAN`. Mesure Release V8g : frame **GPU-bound ~37 ms** (mur = vsync FIFO → ~21 fps ; palier suivant = mainPass sous 33,3 ms → 30 fps). Différés assumés (journal) : imposteurs végétation, queue de transfert dédiée, parité visuelle GL 4.6 PC, timeline semaphores, upload ImGui deux phases, volk, gating macOS Vulkan-seul — journal : `docs/RENDERING.md` |
| **Arbres par colonisation d'espace + cartes de feuillage** | ✅ FAIT 2026-07-19→20 : génération par colonisation d'espace, feuillage en cartes billboard (gradient de densité vers l'extérieur), échelle réaliste, LOD 3 niveaux (V8f) ; tuning live au panneau Tree builder, **moddable §5**. Les chiffres perf V8f/V8g sont à re-mesurer après cette refonte |
| **Passe commentaires (politique §8)** | ✅ FAITE 2026-07-22 : historique code → journaux `docs/`, purge des commentaires changelog, sommaire des headers piliers ; politique actée dans CLAUDE.md §8 |

> **CE FICHIER EST LA ROADMAP UNIQUE.** Les phases 0-8 du CLAUDE.md §9 sont
> de l'histoire (journaux `docs/PHASE-*.md`) ; les phases 8.5-14 ont été
> absorbées ici (mapping dans CLAUDE.md §9). Catalogue des chantiers
> nécessaires pour faire la **démo de gameplay dans Meadows** (décision
> 2026-07-05) plutôt que Godot/Unreal. Un passage sur un
> moteur externe reste possible plus tard — le seam sim/présentation (§2.10
> du CLAUDE.md) reste donc un invariant à protéger. Ce fichier liste QUOI ;
> chaque chantier fera l'objet de sa propre planification en briques le
> moment venu.
>
> Fil conducteur : **tout contenu est un Form moddable (§5), tout outil
> écrit des plugins**. L'éditeur de niveau interne n'est pas un système à
> part — c'est un auteur de plugins comme un autre (records `ReferenceForm`,
> résolution champ par champ, last-writer-wins). C'est l'avantage décisif
> du moteur maison sur Godot/Unreal pour CE jeu.

## Priorités

- **P0** — indispensable à une *vertical slice* Skyrim-like (un extérieur,
  un intérieur, un donjon ; combat 3D ; un PNJ à routine ; une quête).
- **P1** — nécessaire à la démo complète.
- **P2** — confort / plus tard / après la démo.

## Lecture du catalogue A-K

Chaque ligne porte son état d'avancement :
- **✅** = fait (livré et validé — le journal dit où : `HORIZONTAL-PASS`,
  `CHANTIER-1`…) ; un « v1 » signale que le cœur est là, des extensions
  listées dans la ligne restent.
- **🔨** = entamé : le seam/les Forms/le squelette sont posés (passe
  horizontale) ou une partie est livrée — le reste vient avec le chantier
  indiqué dans la ligne.
- *(vide)* = à faire.

## Ce qu'on a déjà (acquis, ne pas refaire)

Réflexion + Forms + résolution de plugins champ par champ + cooker (Phase 1),
ECS/monde/cellules 2D (Phase 2), GAS complet + stats/équipement/statuts
(Phases 3-7), Lua + quêtes/dialogues/conditions (Phase 4), seam
multithreading + snapshot de rendu (Phase 5), combat 2D (Phase 8 en cours),
renderer 3D paysage complet avec météo/culling/compute (`docs/RENDERING.md`),
loader glTF statique (cgltf), TomlWriter, RNG seedé, VFS d'assets par GUID
(design §5 — état d'implémentation à vérifier).

---

## A. Monde, niveaux & éditeur

> **Décision structurante (2026-07-05) : le monde est FAIT MAIN.** Les
> extérieurs sont auteurés à la main ; la génération procédurale n'existe
> que comme **outil d'assistance à l'authoring** (sortie = données
> retouchables), jamais comme système runtime du monde canonique.

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Streaming 3D worldspace→cells** (ex-Phase 10) | P0 | FAIT : CellStreamer (anneau + hystérésis, chantier 2 B1) + **persistance par cellule** (couche pending, chantier 5 B4) + **spawn budgété** (1 cellule/frame au franchissement, chantier 5 B8). Reste : vrai async IO (fichiers cuits par cellule + worker) quand le monde dépassera la RAM — le seam Phase 5 est prêt. |
| ✅ **Éditeur de niveau interne au jeu** | P0 | v1 FAITE (chantier 2 B3/B4/B9) : picking ray-AABB, gizmos ImGuizmo (1/2/3), palette, placement au sol, sculpt, undo/redo, **export = mod `data/mods/level-edits.toml`** rechargé au run suivant. **Duplication (Ctrl+D, un geste d'undo) + snap de grille (toggle, translate au pas + rotate 15°) FAITS (session 2026-07-13)**. Reste : multi-sélection rectangle, resync live après undo. |
| ✅ **Système de prefabs** | P0 | FAIT : expansion runtime (H8) + **« créer un prefab depuis la sélection »** dans l'éditeur (chantier 2 B4, doctesté). Reste : prefabs imbriqués éprouvés. |
| ✅ **Intérieurs** | P0 | v1 FAITE (chantier 2 B6) : worldspace interior, kit de modules (Quaternius village), mode renderer intérieur (pas de terrain/ciel/soleil, ambiant + lumières locales), occlusion v1 = cellule entière. Reste : ambiance audio/reverb (chantier « vivant »), portals P2. |
| ✅ **Terrain fait main** | P0 | v1 FAITE (chantier 2 B8/B9) : hauteur = bruit + **patches delta auteurés** (`.ter` assets + TerrainPatchForm — moddables §5), sculpt raise/lower/flatten/smooth dans l'éditeur, outil offline `cooker terrain-pad`, collision/scatter/rendu tous synchrones. Reste : peinture de splat (B10 non tirée), routes/splines P1. |
| ✅ **Génération de terrain réaliste** | P0 | **CHANTIER TERRAIN-GEN FAIT (2026-07-31) — `docs/TERRAIN-GEN.md`** : pipeline fastscape (uplift+stream power) + thermique + hydrologie (priority-flood → lacs d'altitude, rivières) + littoral + biomes, headless doctesté ; base bakée sous `height()` (bicubique, 3 couches jamais aplaties, sculpt préservé) ; **mode Sandbox infini** (super-tuiles streamées + cache disque, `sandboxTerrain` dans landscape.toml) ; eau locale (lacs/rivières rendus + nage) ; `TerrainGenTool` éditeur (bake → preview → Accept → records §5). Bake 4 km ≈ 4,4 s. Différés en fin de `docs/TERRAIN-GEN.md` (peinture des contrôles, ré-ancrage deltas, eau courante, climat→survie). |
| 🔨 **Sol réaliste — herbe & transition roche (GRASS-REDO)** | P0 | **CHANTIER CONSTRUIT (2026-08-03) — `docs/GRASS-REDO.md`** (étude sourcée CoD/GoT/HZD/Battlefront incluse ; le « visual redo » de l'herbe). Espèces par clump Voronoï (4, table GrassSpecies), zones de variantes du sol « 4 couleurs » (8 couches d'arrays, procédural + cooké), transition herbe/roche en rampe + brins nains + touffes de fissure, cailloux (boulders réutilisés, collision filtrée), ancrage couleur des props, raccord racine cooké (.mtex v2 à moyennes), self-shadow POM, ombres lisses par défaut. **En attente : validation visuelle + F6 dev.** Différés : touffes-meshes, decals splinés, cartes de type peintes. |
| ✅ **Texturing du terrain** | P0 | **CHANTIER CONSTRUIT (2026-08-02/03) — `docs/TERRAIN-TEXTURING.md`** (brief : `docs/AUDIT/TERRAIN-TEXTURING.md`, branche `feature/realistic-textures`). Pipeline offline (RHI BC7/BC5/R16 Vulkan-only + mips offline, `cooker cook-terrain-materials` avec recook auto au build, bibliothèque CC0 5 couches ~8,5 Mo) + toutes les phases visuelles : poids isolés (hybride C — override peint B10 possible), height blending, TerrainShadeMap (biome/wetness → GPU), teinte macro (herbe+GI raccordées), normal mapping + détail proche, POM, anti-répétition bi-fréquence. Sliders live panel « Terrain materials » + toggle A/B cooked/procédural. **En attente : verdict de DA du dev (photo vs stylisé) — il orientera les différés** (hex-tiling, sheen, retouche des sources CC0). |
| **Outils procéduraux d'assistance** | P1 | **Brosses de scatter** dans l'éditeur (une brosse ÉCRIT des références dans le plugin — outil d'authoring, pas système runtime ; l'herbe cosmétique peut rester rule-based, elle ne porte pas de gameplay). 🔨 **Générateur de bases de donjons par règles** : **CHANTIER CONSTRUIT (2026-08-07) — `docs/DUNGEON-GEN.md`** : cyclic dungeon generation (Dormans/Unexplored), pipeline headless D1→D7 (mission graph cyclique → embedding X×Z×étages → creusage SDF planchers plats → surface nets par cellule → nav multi-niveaux → records live+session), `DungeonGenTool` (bake → Accept → Export = mod), `InteriorNavigator` (le premier navigateur multi-sols), assets `.cmesh`/`.nvg`. 36 doctests verts. **En attente : validation in-game de la mine de démo.** Différés en fin de doc (thèmes moddables, modules/kits = 2ᵉ backend, patterns restants, populate gameplay). |
| ✅ **Volumes de gameplay** | P0 | Triggers ACTIFS (nuit 2026-07-10, `d05bcf0`) : `world/scene/TriggerSystem` — enter/leave → event sur l'EventBus + script Lua, latch `once` persistant, doctesté. Boîte orientée ANALYTIQUE, pas un sensor Jolt (CharacterVirtual n'est pas dans la broadphase — écart documenté dans le header). **Kill-z = mort franche (`5497a51`) et VRAIE nage (`64a7726`) FAITS (chantier P0 D2).** Reste : zones de son/reverb. |
| ✅ **Marqueurs** | P0 | FAIT : MarkerForm + spawner (H1/H8), utilisés en vrai par la patrouille du PNJ (chantier 1). Références invisibles = le modèle prévu. |

## B. Rendu — compléments gameplay (au-delà du paysage)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Matériaux & textures sur meshes** | P0 | v1 FAITE (H8 + chantier 1) : `MaterialForm` (albédo × teinte × rampe stylisée), MeshCache async, cube→props réels. Reste : normal maps P1, pipeline KTX2/Basis, atlas/array pour l'instancing par (model, material). |
| ✅ **Lumières locales + ombres intérieures** | P0 | v1 FAITE (chantier 2 B5) : LightsUbo des 16 plus proches (binding 5), falloff quadratique, flicker CPU, sur meshes+personnages ; le paysage reste sun-only. **⚠ Retour dev (2026-07-06) : la qualité d'éclairage (surtout intérieur) est « très simple et moche » — passe dédiée à planifier** : ombres 1-2 lumières clés, spots réels, AO/ambiance de pièce, rampe stylisée sur les lumières locales, éventuel ambient par sonde. Cible : début du chantier 6 (avec les briques renderer 28-29) ou brique tirée plus tôt si ça bloque la démo. **Clustered : CONSTRUIT 2026-07-24 — chantier CLUSTERED, plan et état dans `docs/RENDERING.md` §5** (Forward+ sur la grille de froxels, budget 24→64, sélection frustum+importance, terrain/herbe/arbres en direct + re-contrat du splat RC, atlas de 4 key shadows ; reste la validation visuelle dev au banc `torchbench`). |
| ✅ **Meshes skinnés (GPU skinning)** | P0 | FAIT (chantier 1) : import poids/squelette (remap parents-first), palettes SSBO, shaders skinnés. Reste : rendu INSTANCIÉ des personnages (chantier « vivant », quand ils seront nombreux). |
| **Transparence triée** | P1 | Alpha blend trié par distance (verre, fantômes, eau intérieure) ; le cutout reste l'exception (leçon fill-rate). |
| 🔨 **Émissifs & enchantements** | P1 | Champ `emissive` branché (mesh.frag + bloom). Reste : effets de matériau animés (dissolve, glow) pilotés par GameplayCues (voir F). |
| **Décals** | P1 | Sang, brûlures, impacts — projection simple boîte. |
| 🔨 **Lighting volumétrique + intérieurs « Helios »** | P1 | **Spec écrite : `docs/RENDERING.md`** — le fog devient un éclairage (in-scatter solaire × visibilité CSM ; pas de raytracing ; RC comme terme ambient de l'air), briques V1→V4 extérieur puis H1→H4 intérieurs (ambiance horaire+météo, raies héliotropes via `sunLinked`, règle « enterré » `buriedBelowY`). Acquis : `volumetric.frag` (raymarch CSM ½-res), lames `LightForm.shaft/sunLinked`. Briques à planifier au chantier. |
| 🔨 **LOD/impostors bâtiments & props placés** | P1 | LOD de canopée par chunk FAIT (chantier 1 B7 — le pattern à étendre). Reste : LOD des meshes auteurés, impostors P2. |
| ✅ **Première personne** | P0 | **Le jeu est à la 1re personne (décision 2026-07-06)** : caméra aux yeux sur capsule cinématique — FAIT (chantier 1). La 3e personne sert aux PNJ ; caméras de dialogue (cadrage) P1 ; vue 3e personne joueur = P2/option. |

## C. Personnages & animation

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Système d'animation squelettale** | P0 | v1 FAITE (H5 + chantier 1) : clips glTF, échantillonnage, cross-fades, graphe, anti-foot-sliding (referenceSpeed — **pas de root motion**, décidé 2026-07-05). Reste : couches haut/bas du corps, masques par os, additive P1 (chantier « vivant », le combat en a besoin). |
| ✅ **Contrôleur d'anim en données** | P0 | FAIT (H1/H5 + chantier 1) : `AnimGraphForm` + états/transitions enfants, params (vitesse) + gates par tags, moddable (prouvé : seuils retunables en TOML). **Condition evaluator complet branché sur les transitions (session 2026-07-13)** : `ConditionForm` enfants d'une `AnimTransitionForm`, seam callback plat (`setConditionCheck` — engine sans types gameplay), câblé sur l'acteur au spawn. |
| ✅ **Événements sur timeline** | P0 | `AnimEventForm` + tir au bon temps (H5, chantier 1) ; les CONSOMMATEURS sont livrés (chantier P0) : fenêtres de hit GAS (A3/A4), footsteps par matériau (C4), cues FX/son/shake (C2). Édition = timeline de clip du chantier 8 (8.8). |
| ✅ **Éditeur d'animation / timeline** | P1 | FAIT (chantier 8) : graphe nodal AnimGraph (8.6, validé dev) + timeline des events de clip (8.8), sortie = plugin. **+ Préview 3D offscreen (session 2026-07-13)** : fenêtre « Anim Preview » (dockée avec la Console) — clip scrub/play avec events surlignés, graphe LIVE (sliders de params, cases des tag-gates, sync entitySpeed, log d'events), mesh skinné orbitable, édits de session visibles avant export (`buildAnimGraph` overload EditSession). |
| 🔨 **Apparence modulaire des personnages** | P0 | `AppearanceForm` (slots+teintes) + `resolveActorVisual` FAITS (H1 + chantier 1) — mais v1 mono-mesh (premier slot rempli). Reste : compositing multi-slots sur un squelette partagé + équipement→visuel (échange du mesh du slot). |
| ✅ **Attach points** | P0 | FAIT (chantier P0 A2) : sockets sur os (main droite = arme, dos = fourreau), armes visibles rangées/dégainées. Selle P2. |
| **Regard & tête** (look-at IK) | P1 | Les PNJ regardent leur interlocuteur ; bouche qui bouge en dialogue P2 (subtitle-first). |
| **Anim LOD** | P1 | Fréquence d'update réduite avec la distance ; pas d'anim hors écran (le culling sait déjà qui est visible). |
| **Ragdoll** | P2 | Avec Jolt ; v1 = animations de mort. |
| **Character creator joueur** | P2 | L'apparence modulaire le rend possible ; UI dédiée plus tard. |

### C.1 Fiche de personnage — `ActorForm` comme hub (étude NarrativePro, 2026-07-06)

> Étude du modèle `UCharacterDefinition`/`UNPCDefinition` de Narrative Pro
> (Unreal) demandée par le dev : un data asset unique décrit TOUT le
> personnage (apparence, loadout, tags, factions, dialogue, planning,
> config d'abilities, vendeur). **Verdict : notre modèle couvre déjà le
> concept — `ActorForm` EST la character definition** (hooks H1 :
> `appearance`, `animGraph`, `schedule` + GAS §6), et notre convention
> « records enfants » exprime leurs TArray sans étendre la réflexion.
> Ce qui manque, planifié par verticale :

| Élément NarrativePro | Équivalent Meadows | Quand |
|---|---|---|
| DefaultAppearance | `ActorForm.appearance` → `AppearanceForm` (fait, H1) | B6 (branché) |
| AbilityConfiguration (abilities/attributs de départ) | **enfants `ActorGrantForm { parent, effect/ability guid }`** appliqués au spawn via applyEffect/grantAbility | chantier « vivant » |
| DefaultItemLoadout + currency (loot rolls) | **enfants `LoadoutEntryForm { parent, item, count, chance }`** roulés au spawn (RNG §8) + champ `currency` APPEND | chantier « vivant » |
| DefaultOwnedTags / DefaultFactions | **enfants `ActorTagForm { parent, tag }`** (factions = tags, §6.1 — déjà le modèle) | chantier « vivant » |
| Dialogue + TaggedDialogueSet | champ `dialogue` guid APPEND sur ActorForm → records Phase 4 ; tagged dialogues = condition evaluator | chantier « vivant » |
| ActivitySchedules / ActivityConfiguration | `ActorForm.schedule` → ScheduleForm (fait, H1) | chantier « vivant » |
| Vendeur (bIsVendor, buy/sell %, trading loadout) | **`VendorForm` enfant** (ou APPEND) + inventaire marchand | P1 économie |
| NPC unique vs multi-instances + GUID de save | **natif** : PNJ unique = ReferenceForm placée (GUID stable §2.5) ; génériques = spawns runtime | rien à faire |
| Min/MaxLevel | scaling par skills-by-use | P1 stats |
| Variations d'apparence aléatoires (meshes/teintes par slot, stream seedé) | **enfants `AppearanceVariationForm`** + RNG seedé par guid d'instance (déterministe §8) | P1 (foule variée) |

> Invariant : tout reste des Forms + records enfants — un mod ajoute une
> entrée de loadout ou une variation d'apparence sans toucher au parent.

## D. Physique & contrôleur (Jolt)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Intégration Jolt** | P0 | Monde + capsule + raycasts + height fields + **mesh colliders des statics/kits** (`addStaticMesh`, chantier 2 B2 — suivent le spawn/despawn des cellules) + **shape casts combat** (chantier P0 A1) FAITS. Les triggers sont ANALYTIQUES (TriggerSystem — CharacterVirtual hors broadphase), pas des sensors Jolt. |
| ✅ **Character controller** | P0 | FAIT (chantier 1) : capsule cinématique, pentes/marches, saut, sprint payé en énergie GAS, vitesses par stats dérivées. **Nage FAITE** (P0 D2b : flottaison, drain d'énergie, noyade), sneak accroupi (post-P0), kill-z mort franche (D2a). **Dégâts de chute FAITS (session 2026-07-13)** : `fallDamage` pur + knobs `fall*` StatsTuning, Blunt non mitigé (l'idiome noyade), hauteur létale → killOutright, l'eau annule. |
| **Objets dynamiques** | P1 | Props poussables, loot qui tombe, physique légère (le « clutter » Skyrim). Havok-cheese P2. |
| ✅ **Portes & mécanismes** | P0 | v1 FAITE (chantier 2 B7) : `DoorForm` (cible = référence de marker, résolution 100 % records), prompt [E], transition worldspace avec fondu. Reste : portes ANIMÉES (battant), verrous/crochetage P1, leviers/coffres, états persistés (chantier 5). |

## E. IA & navigation

| Fonctionnalité | Prio | Notes |
|---|---|---|
| 🔨 **Navmesh (Recast/Detour)** | P0 | v1 = `TerrainNavigator` (chantier 3 B2, doctesté) : A* grille 1 m projeté sur le terrain patché, obstacles = AABB des colliders statics. Reste : Recast/Detour réels (génération offline par cellule + liens, requêtes async, évitement local) — quand la grille pique. |
| ✅ **Emploi du temps des PNJ** | P0 | `evaluateSchedule` (H1/H7) + EXÉCUTION (chantier 3 B3) : ré-évaluation par slot 10 min-jeu, la journée du Villager se déroule seule. **Interruptions/reprise FAITES (session 2026-07-13)** : edges purs (`updateInterruption`), combat ET dialogue OUVERT (le PNJ parlé s'immobilise), reprise = ré-évaluation immédiate (un combat qui enjambe un slot reprend sur l'entrée COURANTE) + repath. Reste : simulation dégradée hors cellule P2. |
| ✅ **Packages IA (exécution)** | P0 | wander / travel / useFurniture (claim + anims par tag) / guard exécutés (chantier 3 B3). S'enrichit avec le contenu. |
| ✅ **Outil emploi du temps** | P0/P1 | FAIT : vue debug (drawSchedules + slider d'heure, H7) + éditeur visuel timeline des schedules (chantier 8.4), sortie = plugin. |
| ✅ **IA de combat 3D** | P0 | FAIT (chantier P0 B3 + A6) : strafe, fuite (courage/santé), appel à l'aide (factions), distances d'engagement par arme ; archer PNJ à carquois réel, brain Lua, trace d'intention (log). **Cible-entité généralisée (FOLLOWERS É2 : combat PNJ-contre-PNJ).** |
| ✅ **Perception 3D** | P0 | FAIT (chantier P0 B1/B2) : composant `world/ai/Perception` dédié (cône, ouïe, mémoire de dernière position) + **SpatialIndex partagé** (l'infra promise par le chantier RC). Le bruit du sneak joueur est branché sur l'ouïe. |
| **Foules/planification** | P2 | Budget d'update IA par frame, LOD IA (PNJ hors cellule simulés grossièrement — « offscreen simulation » Skyrim). |

## F. Gameplay (au-dessus du GAS existant)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Combat 3D** | P0 | FAIT (chantier P0 axe A) : ability GAS + hit À LA LAME (segment vs capsule sur fenêtres d'AnimEvents), blocage directionnel, arc + projectiles (tir chargé, économie de flèches), parade parfaite, esquive ; résolution de frappe UNIQUE partagée joueur/PNJ/flèches (`aaa733a`). Lock-on ABANDONNÉ (décision dev — le cône de mêlée suffit en vue subjective). |
| ✅ **GameplayCues (pont effets→présentation)** | P0 | FAIT (H7 + chantier P0 C2/C3) : registre + `CueTable` (fallback hiérarchique) + `CueForm` à trois canaux résolus — particule (FxDirector), shake caméra amorti, son (résolveur SoundForm). Émissions systématiques : hit par type de dégât, block, parry, mort, footsteps. Le look du combat est entièrement moddable (CueForms en data). Premiers wavs dev câblés (`77545b4`) ; la bibliothèque d'assets s'étoffe au fil du contenu. |
| ✅ **Interaction** | P0 | FAITE (chantier 3 B1/B7-lite) : E contextuel — porte (travel), prendre (inventaire), parler (placeholder dialogue), mobilier (lit 8 h / siège 1 h via `gameplay::sleep()` Phase 7). |
| ✅ **Mobilier & postes de travail** | P0 | `FurnitureForm` + occupancy + spawner FAITS ; PNJ : nav → claim → anim par tag (chantier 3 B3) ; joueur : repos/sommeil (B7-lite) ; **`screen` câblé** (chantier 4 B6) ; **effet GAS pendant l'usage + anims d'entrée/sortie** (chantier P0 D1) ; montable v1 (`mountSpeed`, FOLLOWERS É11). Reste : vrais écrans de craft. |
| 🔨 **Économie/marchands 3D** | P1 | Barter v1 FAIT (chantier 4 B5) : écran deux tables, prix = goldValue × multiplicateurs moddables, or = MiscItemForm, stock/bourse par LoadoutEntryForm, richesse du marchand finie. **Restock FAIT** (chantier 6 D1 : re-roll du loadout si > 24 h-jeu, horloge dans le composant réfléchi `VendorState` capturé par la save) + **buy/sellMult par marchand FAITS** (ActorForm APPEND). Reste : scaling charisme (P1 stats). |
| 🔨 **Crime & prime** | P1 | v1 FAITE (chantier 6 D2) : témoins (victime ou PNJ < 20 m avec LOS), composant `Bounty` mirroré en tag `Crime.Wanted`, gardes hostiles tant que Wanted, amende payable en dialogue. **Bounty PAR FACTION FAIT (session 2026-07-13)** : la prime va à la faction du TÉMOIN, un garde ne chasse que si SA faction détient une tranche (`bountyToward` — le total non attribué des vieilles saves compte pour tous), persistance `SavedBountyForm` par nom de tag, **amende réglée À LA FACTION du garde arrêteur** (`clearBountyToward` — les tranches des autres factions survivent, Wanted tient tant qu'il en reste). Reste : arrestation/prison. |
| 🔨 **Furtivité** | P1 | v1 FAITE (post-P0, design dev 2026-07-12) : mode sneak (corps accroupi ½ hauteur, vitesse ×0.75 — règle au STATS.md), bruit réduit branché sur l'ouïe de la Perception. **Dégâts sournois FAITS (session 2026-07-13)** : attaquant State.Sneaking × défenseur Calm (bool plat dans la résolution unique) → ×`sneakAttackMultiplier` (3, moddable), mêlée ET flèches. Reste : détection par lumière P2. |
| ✅ **Repos/attente 3D** | P0 | FAIT : lits/sièges via le mobilier (chantier 3 B7-lite, `gameplay::sleep()` Phase 7) + menu d'attente (chantier 4). |
| 🔨 **Progression** | P1 | **Skills-by-use v1 FAITE (session 2026-07-13, décision 2026-07-05)** : `SkillForm` (xpPerUse) + seuils enfants (`SkillThresholdForm` → EffectForm passif appliqué UNE fois), `AbilityForm.skill` APPEND, `OnAbilityUsed` dispatché par tryActivate (sites joueur : attaque/arc/esquive — vocabulaire ouvert pour les quêtes aussi), persistance `SavedSkillForm`. Reste : le CONTENU (skills + seuils en TOML), l'UI feuille de personnage, l'érudition (STATS.md). |

## G. UI (jeu) + éditeur d'UI

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Moteur UI : RmlUi (DÉCIDÉ 2026-07-05)** | P0 | v1 FAITE (chantier 4 B1/B2) : pile d'écrans (`ScreenStack` + `UiScreenForm`), data binding (façade DataModel — scalaires/rows/événements), clavier/texte, roots multi-plugins. Reste : gamepad P1. |
| ✅ **Rendu texte** | P0 | RmlUi+FreeType pour les écrans ; nameplates screen-space via le modèle HUD (chantier 4 B7). UTF-8 partout. |
| 🔨 **Écrans de jeu** | P0→P1 | P0 FAITS (chantier 4) : HUD (barres/crosshair/prompt/horloge), inventaire/équipement (table SkyUI), dialogue, conteneur/loot, barter, menu principal/pause/attente, atelier placeholder. **Journal de quêtes FAIT** (chantier 6 A3, écran J), **carte FAITE** (C9.6 — raster CPU stylisé, palette à retoucher), **options FAITES** (C9.4 — settings live + press-to-rebind). Reste P1 : feuille de personnage, level-up, boussole, écrans de chargement. |
| ✅ **Navigation gamepad** | P1 | FAITE (chantier 9, C9.1-C9.3) : canal manette dans `platform::Input`, couche ACTION remappable (settings.toml), focus/navigation directionnelle dans tous les écrans RmlUi — validée dev. |
| 🔨 **UI monde** | P0 | Nameplates/barres de vie FAITES (hostiles/blessés, chantier 4 B7) ; prompts flottants = le prompt HUD. Reste : textes de dégâts P2. |
| **Éditeur d'UI** | P1 | Édition de layout en jeu (ancres, conteneurs, styles), sauvegarde en données moddables. (P0 : layouts en TOML édités à la main + hot-reload.) |
| ✅ **Localisation** | P1 | FAITE (C9.5) : base EN + pack FR (172 chaînes), lookup runtime + bascule de langue live (validée dev), attributs data-loc dans les RML, `cooker import-csv` (+ `--patch` pour les packs de langue). |

## H. FX

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Système de particules** | P0 | v2 FAITE (chantier P0 C1) : émetteurs continus, formes sphere/cone/box, budget global 4096, flag additif, **rendu 3D en quads face caméra** (FxRenderer, SSBO d'instances, batches alpha triés + additif) — seam Phase-5 respecté (sim headless → snapshot POD). Banc : commande console `fx <editorId>`. Reste : soft particles/textures si besoin. |
| ✅ **Éditeur de FX** | P1 | FAIT (chantier 8.10) : édition des params d'émetteur avec preview live, save en Form/plugin. |
| **Trails & beams** | P1 | Traînées d'armes (sockets d'anim), rayons de sorts. |
| **FX de matériaux** | P1 | Dissolve, freeze, burn — anim de params de MaterialForm via cues. |
| **Screen FX** | P1 | Vignette de dégâts, overlays d'états (poison/ivresse — statuts Phase 7), hit flash — hooks dans le tonemap existant. |
| 🔨 **Bibliothèque de base** | P0 | Hit/block/parry/mort/footsteps FAITS en CueForms data (chantier P0 C2, `combat.toml` — fallback hiérarchique `Cue.Hit.Slash` → `Cue.Hit`). Reste : sang, soin, feu/torches, fumée, magie par école — s'étoffe au fil du contenu. |

## I. Audio (miniaudio)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Moteur son** | P0 | Seam FAIT (H6) : bus, play 2D/3D, crossfade musique, backend null pour les tests. **Résolveur `SoundForm` FAIT** (chantier P0 C3 : variantes pondérées, jitter pitch/volume, chemin AssetDatabase) ; premiers assets déposés (footsteps dev). |
| **Ambiances** | P0 | Beds par région/cellule/météo/heure (la météo pilote déjà tout le visuel — brancher l'audio dessus), transitions fondues (le crossfade météo existe). Chantier « vivant ». |
| **Musique dynamique** | P1 | Couches explore/combat/danger avec transitions (tags GAS `State.InCombat` déjà là). |
| ✅ **Footsteps par matériau** | P1 | FAITS (chantier P0 C4) : AnimEvent « Footstep » × matériau sous le pied (splat/intérieur) → cue `Cue.Footstep.<Material>` ; wavs dev câblés (`77545b4`) ; + bruit du joueur alimentant l'ouïe des PNJ. |
| **Reverb par volume** | P1 | Zones de reverb (intérieurs/grottes). |
| **Voix** | P2 | Hooks de playback par ligne de dialogue (subtitle-first pour la démo). |

## J. Données, plugins & outils transverses

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **GameDB browser (l'outil central)** | P0 | v1 FAITE (H2, scène « Game DB ») : navigation par type, recherche editorId, édition par PropertyGrid réflexion, création, **export = plugin**. Duplication + « utilisé par » FAITS (chantier 8.1) ; extension en éditeur de niveau FAITE (chantier 2) ; fenêtre unique « True Adventurer DB » Browser\|Editor\|Inspector (chantier 8.7b). |
| ✅ **Gestionnaire de plugins interne** | P0 | v1 FAITE (H2, PluginsPanel) : load order (plugins.toml — pilote désormais le JEU aussi, chantier 4 B1), enable/disable, **rapport de conflits par champ**, **dépendances affichées** (chantier 4 B7). Outil de synthesis §5.1 FAIT (chantier 8.5). |
| ✅ **Console développeur** | P0 | FAITE (H2) + **en jeu (F8, chantier 4 B7)** avec `spawn`/`tp`/`tgm`/`settime` (spawns transients). **`setstage <quête> <état>` FAIT (session 2026-07-13)** : `quest::setQuestState` (enrôle, saute, rouvre, termine par kind). |
| 🔨 **Pipeline d'assets** | P0 | Import glTF statique+skinné+anims FAIT (brique 23 + chantier 1) ; cooker binaire couvre tous les Forms (audit) ; hot-reload textures/shaders. Reste : KTX2/Basis, **asset browser** avec previews, hot-reload meshes/anims. |
| ✅ **Éditeur de quêtes/dialogues** | P1 | FAIT (chantier 8) : éditeurs arborescents (8.2/8.3) puis graphes nodaux (8.7, imgui-node-editor) + builder de conditions partagé (8.9) + remise d'items/récompenses en données (8.7e) ; positions de nœuds = side-store éditeur. |
| 🔨 **Profiler** | P1 | **Timers GPU par passe FAITS** (chantier GPU-PERF P0, HUD F6 + lignes de coût). Reste : zones CPU, graphe de frame, compteurs complets. |
| ✅ **Validation de mods** | P1 | **FAITE (session 2026-07-13)** : `cooker validate <plugins...>` sur la passe réutilisable `data::validatePlugins` — patches orphelins, violations de dépendances, **refs GUID pendantes** (balayage réflexif, guids procéduraux runtime whitelistés) = erreurs (exit 1, CI-able) ; les conflits de champ restent INFORMATIFS (§5 — le pack FR patchant l'EN est le design). La pile base passe. |
| **Docs moddeur générées** | P2 | Générer la référence des champs depuis la réflexion (le MODDING-EFFECTS.md à la main ne scalera pas). |
| ✅ **Doc utilisateur/moddeur `userdoc/`** | P0 (à maintenir) | FAITE 2026-07-05 : hub + pages thématiques liées. **Chaque vertical livré met à jour sa page** (chantier 1 → world-and-levels : props 3D + personnages). |
| ✅ **Index secondaires FormDatabase** | P1 | **FAITS (session 2026-07-13)** — décision 2026-07-05 tenue : PAS de SQL, la FormDatabase résolue EST la base. Index par typeId (toute la chaîne isA bucketée au add) et par `parent` réfléchi, ordre des handles préservé ; `forEach`/`childrenOf` basculés dessus, API inchangée (AnimBridge/quêtes/schedules en profitent gratuitement). Le resolver matérialise les patchs AVANT le add → le parent indexé est final (reparentage §5 couvert, testé). |
| **Crash handling** | P2 | Minidumps + log de la pile de plugins active. |
| **Sauvegardes de records ÉPARS** | P1 | Les sauvegardes outillées (render-tuning, tree-types) capturent les records EN ENTIER : tout champ ajouté plus tard reste figé à sa valeur du moment de la capture (piège vécu 2026-08-06 : le tuning sauvé re-forçait `tubeSides = 5` par-dessus la base, §5 dernier-écrivain). Fix : n'écrire que les champs qui DIFFÈRENT de la valeur résolue sans cette couche (diff par réflexion — le TomlWriter et la réflexion savent déjà tout faire). |

## K. Sauvegarde & streaming (ex-Phase 10) — ✅ FAIT (chantier 5, 2026-07-06)

Save = couche de patches runtime (§5, tenu) : une save est un **plugin
TOML** dans `saves/` (records SavedStats/SavedEffect/SavedItem/
SavedInjury enfants par référence + patches ReferenceForm + un
WorldStateForm), résolu en dernière couche au chargement. Couvert :
état/position/mort/inventaire/effets actifs des acteurs, items ramassés,
enfants de prefab, horloge/worldspace/caméra, mémoire des cellules
déchargées (sans disque), F5/F9 + menus + console. Reste pour plus tard :
stages de quêtes/journal (avec le chantier quêtes), météo fine, état PNJ
fin (schedule en cours — re-dérivé de l'horloge), async IO réel.
Journal : `docs/CHANTIER-5.md` ; doc moddeur : `userdoc/saving.md`.

---

## Décisions actées (2026-07-05, avec le dev)

1. **Phase 8.5 (validation Godot) : REPORTÉE après la démo.** La démo de
   gameplay se fait dans Meadows ; le seam sim/présentation (§2.10) reste
   un invariant protégé par les tests headless — Godot redevient une
   option post-démo, rien n'est brûlé.
2. **UI : RmlUi**, documents servis via le VFS de plugins (moddabilité au
   niveau document — le modèle SkyUI/Scaleform). Outils dev = ImGui.
3. **Root motion : NON** — contrôleur cinématique + anims in-place,
   playback rate calé sur la vélocité contre le foot-sliding.
4. **Progression : skills-by-use** (événements d'usage → XP de compétence
   → seuils = GameplayEffects passifs).
5. **Textures : OUI au chantier 1** — albédo plat stylisé + rampe pour
   meshes/personnages ; le paysage garde splat/vertex color.
6. **Le monde est FAIT MAIN, pas généré.** Le procédural n'existe que
   comme outils d'assistance à l'authoring (brosses de scatter, seed de
   terrain de départ, générateur de bases de donjons par règles — sortie
   toujours = records/prefabs retouchables), jamais comme système runtime
   du monde canonique.
7. **Le jeu est à la PREMIÈRE personne** (2026-07-06) : le joueur est un
   contrôleur FPS (capsule + caméra aux yeux, pas de mesh visible en v1) ;
   le rendu/anim 3e personne sert aux PNJ. Et le joueur EST un personnage
   au sens de `docs/STATS.md` : ses déplacements passent par les stats
   dérivées, toute mutation par GameplayEffect (fait, chantier 1 B5.5).

## Passe horizontale Fable (5 → 7 juillet 2026) — ✅ FAITE (2026-07-06)

> **Livrée intégralement : voir `docs/HORIZONTAL-PASS.md`** — l'état des
> 8 briques (H1-H8), les règles transverses, les notes « comment remplir »
> par module et les pièges connus. 229 tests verts. Les sessions post-7/07
> exécutent l'ordre macro ci-dessous en suivant ce document.

> **Stratégie de développement (directive dev)** : l'abonnement Fable
> expire le 7 juillet. D'ici là, **Fable pose les bases et l'architecture
> de tous les systèmes, horizontalement** — modules, interfaces, seams,
> types de Forms, squelettes qui compilent — sans détailler les
> implémentations. **Après le 7/07, un modèle moins puissant remplit les
> verticales une par une** en suivant l'architecture posée : suivre les
> seams, ne pas les re-décider, une verticale à la fois, cadence brique
> par brique inchangée. Chaque squelette laisse des commentaires
> d'intention et une note « comment remplir ».

Périmètre de la passe, priorisé par « coût d'une mauvaise décision » :

1. **Modèle de données complet (le cœur)** : déclarer + réfléchir + faire
   résoudre TOUS les nouveaux types de Forms — MaterialForm, LightForm,
   PrefabForm, marker/trigger Forms, CellForm 3D/intérieur,
   AnimClipForm/AnimGraphForm, AppearanceForm + slots, SoundForm,
   ParticleForm, AiPackageForm, ScheduleForm, FurnitureForm,
   LocalizationForm, refs de documents UI — avec doctests de résolution
   §5. (La demande explicite du dev : « travailler mieux mon modèle de
   données et de plugins ».)
2. **Seams de modules** (libs qui compilent, interfaces étroites, impls
   minimales) : `engine/anim/` (Skeleton/Clip/Sampler/Graph, évaluation
   headless-testable), `engine/physics/` (façade Jolt : monde, character
   controller, casts — dep CPM branchée), `engine/ui/` (adaptateur RmlUi
   sur RHI + documents via VFS plugins), `engine/audio/` (façade
   miniaudio : bus, play(SoundForm), 3D), `engine/fx/` (registre
   **GameplayCues** tag→handler — LE pont sim/présentation, headless
   no-op — + squelette particules), `engine/nav/` (interface navmesh,
   Recast branché ou stub assumé).
3. **Boucle éditeur = auteur de plugins** : session d'édition (dirty
   tracking de records → TomlWriter → plugin), squelette GameDB browser
   (table réflexion-driven), console dev (commandes par réflexion + REPL
   Lua). Prouve le concept central « l'outil écrit des plugins ».
4. **Extensions monde 3D** : worldspace/cell 3D + intérieurs dans les
   Forms, spawner 3D (catégories existantes + lumières/marqueurs),
   prefab = instanciation de groupe.
5. **Renderer gameplay (contrats seulement)** : MaterialForm → pipeline
   texturé stylisé (un mesh texturé à l'écran), chemin skinned mesh
   (structure des palettes de bones), design des lumières locales —
   l'implémentation complète reste post-7/07.

## Ordre macro POST-7/07 — les verticales (chaque chantier = sa propre planification en briques)

1. **Socle 3D gameplay — ✅ FAIT (2026-07-06, journal `docs/CHANTIER-1.md`)** :
   matériaux + meshes skinnés + animation data-driven (C) + Jolt &
   contrôleur première personne piloté par les stats (D, docs/STATS.md) +
   PNJ 100 % Forms en patrouille + brique renderer 27 (canopées pleines +
   LOD). Le joueur EST un acteur GAS (sprint = EffectForm, blessures =
   vitesse réelle).
2. **Monde habitable** : streaming/cellules 3D + intérieurs + lumières
   locales (A, B) + éditeur de niveau v1 + prefabs — « un village et une
   maison où entrer ». **Absorbe** : les lumières/ombres intérieures
   (ex-Phase 12 chemin custom). **Inclut le chantier « cellules extérieures
   implicites » (`docs/IMPLICIT-CELLS.md`, décidé 2026-07-08)** : éditer/poser
   n'importe où sur la grille infinie (cellules matérialisées à la pose, GUID
   déterministe — §2.5), au lieu des seules cellules autorisées à la main.
   Petit chantier (briques 1-3) — **LIVRÉ nuit 2026-07-10**, avec les
   volumes de triggers.
3. **Vivant** : navmesh + packages IA + perception (E) + combat 3D + cues +
   FX/audio P0 (F, H, I) — « la boucle Skyrim : explorer, combattre, parler ».
   **Absorbe le reliquat de la Phase 8** : IA ennemie (chase + attaque,
   portée en 3D), PNJ aubergiste (repos) et marchand (achat/vente) — la
   CombatArena 2D reste le banc d'essai GAS en attendant.
4. **Interfaces** : écrans RmlUi P0 (G) + console + GameDB browser + plugin
   manager (J) — squelettes déjà livrés par la passe horizontale.
5. **Persistance** : streaming & save = couche de patches (K — l'ex-Phase 10
   telle quelle, résolveur Phase 1 réutilisé).
6. Puis les **P1 par valeur** : **stats avancées (ex-Phase 9** — machine
   d'état de combat shaken/critical-weakness/démembrement/bleed-out, stats
   offensives/sociales/utilitaires dérivées, blessures avancées ; design
   complet : `docs/STATS.md`, nécessite la boucle de combat 3D du chantier
   3**)**, quêtes 3D outillées, économie/crime, éditeurs (anim/FX/UI),
   musique dynamique, localisation, et le **polish renderer** (briques
   28-31 : color grading LUT, auto-exposition, cumulonimbus, pluie — spec :
   `docs/RENDERING.md`).

> NB : `docs/RENDERING.md` reste le journal + la spec détaillée des
> briques renderer ; leur *planification* vit désormais ici (27→chantier 1,
> lumières intérieures→chantier 2, 28-31→chantier 6). La validation Godot
> (ex-Phase 8.5) reste une option post-démo
> (`docs/SIMULATION-AND-PRESENTATION.md`).