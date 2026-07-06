# MEADOWS-PLAN — Fonctionnalités moteur & outils pour le Skyrim-like

## OÙ ON EN EST (mis à jour 2026-07-06)

| Piste | État |
|---|---|
| Décisions de cadrage (7, dont **jeu 1re personne**) | ✅ actées 2026-07-05/06 (§ Décisions actées, §C.1) |
| **Passe horizontale** (architecture de tous les systèmes) | ✅ FAITE 2026-07-06 — contrat : `docs/HORIZONTAL-PASS.md` (audit de compat inclus) |
| Renderer paysage (briques 27-31) | 27 ✅ FAITE (canopées pleines + LOD, chantier 1) ; 28-31 → chantier 6 ; spec : `docs/3D-RENDERER.md` |
| **Chantier 1 — Socle 3D gameplay** | ✅ FAIT 2026-07-06 — journal : `docs/CHANTIER-1.md` (joueur FPS piloté par ses stats, PNJ 100 % Forms en patrouille, MeshCache/skinning/anim data, collision terrain Jolt, arbres brique 27) |
| **Chantier 2 — Monde habitable** (cellules 3D, éditeur de niveau+gizmos, lumières locales, kit+intérieur+portes, terrain auteuré+sculpt) | ✅ FAIT 2026-07-06 (**validation visuelle dev en attente**) — journal : `docs/CHANTIER-2.md` |
| **Chantier 3 — Vivant** (navmesh, IA/schedules exécutés, combat 3D, cues/FX/audio P0) | ⬅️ **PROCHAIN** — plan de briques proposé : `docs/CHANTIER-3.md` (à valider) |
| Chantier 4 — Interfaces (écrans RmlUi P0, GameDB/console déjà livrés en squelette) | à faire |
| Chantier 5 — Persistance (Phase 10 : streaming + save = couche de patches) | à faire |
| Chantier 6 — P1 par valeur (quêtes outillées, économie/crime, éditeurs anim/FX/UI…) | à faire |
| Refonte herbe (renderer) | ⏸️ en attente des recherches du dev |

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
renderer 3D paysage complet avec météo/culling/compute (`docs/3D-RENDERER.md`),
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
| 🔨 **Streaming 3D worldspace→cells** (ex-Phase 10) | P0 | v1 SYNCHRONE FAITE (chantier 2 B1) : CellStreamer (anneau + hystérésis), worldspaces intérieurs séparés, transitions portes. Reste (chantier 5) : chargement ASYNC + persistance par cellule. |
| ✅ **Éditeur de niveau interne au jeu** | P0 | v1 FAITE (chantier 2 B3/B4/B9) : picking ray-AABB, gizmos ImGuizmo (1/2/3), palette, placement au sol, sculpt, undo/redo, **export = mod `data/mods/level-edits.toml`** rechargé au run suivant. Reste : duplication, multi-sélection rectangle, resync live après undo, snap de grille. |
| ✅ **Système de prefabs** | P0 | FAIT : expansion runtime (H8) + **« créer un prefab depuis la sélection »** dans l'éditeur (chantier 2 B4, doctesté). Reste : prefabs imbriqués éprouvés. |
| ✅ **Intérieurs** | P0 | v1 FAITE (chantier 2 B6) : worldspace interior, kit de modules (Quaternius village), mode renderer intérieur (pas de terrain/ciel/soleil, ambiant + lumières locales), occlusion v1 = cellule entière. Reste : ambiance audio/reverb (chantier « vivant »), portals P2. |
| ✅ **Terrain fait main** | P0 | v1 FAITE (chantier 2 B8/B9) : hauteur = bruit + **patches delta auteurés** (`.ter` assets + TerrainPatchForm — moddables §5), sculpt raise/lower/flatten/smooth dans l'éditeur, outil offline `cooker terrain-pad`, collision/scatter/rendu tous synchrones. Reste : peinture de splat (B10 non tirée), routes/splines et rivières placées P1. `TerrainNoise` reste la base procédurale qu'on retouche. |
| **Outils procéduraux d'assistance** | P1 | **Brosses de scatter** dans l'éditeur (une brosse ÉCRIT des références dans le plugin — outil d'authoring, pas système runtime ; l'herbe cosmétique peut rester rule-based, elle ne porte pas de gameplay). **Générateur de bases de donjons par règles** (assemblage de kits de modules → sortie = records/prefabs retouchables à la main) P1/P2. |
| 🔨 **Volumes de gameplay** | P0 | TriggerForm + spawner + composant posés (H1/H8) ; reste le branchement Jolt sensors → EventBus (chantier 2/3), volumes d'eau, kill-z, zones de son/reverb. |
| ✅ **Marqueurs** | P0 | FAIT : MarkerForm + spawner (H1/H8), utilisés en vrai par la patrouille du PNJ (chantier 1). Références invisibles = le modèle prévu. |

## B. Rendu — compléments gameplay (au-delà du paysage)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Matériaux & textures sur meshes** | P0 | v1 FAITE (H8 + chantier 1) : `MaterialForm` (albédo × teinte × rampe stylisée), MeshCache async, cube→props réels. Reste : normal maps P1, pipeline KTX2/Basis, atlas/array pour l'instancing par (model, material). |
| ✅ **Lumières locales + ombres intérieures** | P0 | v1 FAITE (chantier 2 B5) : LightsUbo des 16 plus proches (binding 5), falloff quadratique, flicker CPU, sur meshes+personnages ; le paysage reste sun-only. Reste : ombres 1-2 lumières clés par intérieur, spots réels, clustered P1. |
| ✅ **Meshes skinnés (GPU skinning)** | P0 | FAIT (chantier 1) : import poids/squelette (remap parents-first), palettes SSBO, shaders skinnés. Reste : rendu INSTANCIÉ des personnages (chantier « vivant », quand ils seront nombreux). |
| **Transparence triée** | P1 | Alpha blend trié par distance (verre, fantômes, eau intérieure) ; le cutout reste l'exception (leçon fill-rate). |
| 🔨 **Émissifs & enchantements** | P1 | Champ `emissive` branché (mesh.frag + bloom). Reste : effets de matériau animés (dissolve, glow) pilotés par GameplayCues (voir F). |
| **Décals** | P1 | Sang, brûlures, impacts — projection simple boîte. |
| 🔨 **LOD/impostors bâtiments & props placés** | P1 | LOD de canopée par chunk FAIT (chantier 1 B7 — le pattern à étendre). Reste : LOD des meshes auteurés, impostors P2. |
| ✅ **Première personne** | P0 | **Le jeu est à la 1re personne (décision 2026-07-06)** : caméra aux yeux sur capsule cinématique — FAIT (chantier 1). La 3e personne sert aux PNJ ; caméras de dialogue (cadrage) P1 ; vue 3e personne joueur = P2/option. |

## C. Personnages & animation

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **Système d'animation squelettale** | P0 | v1 FAITE (H5 + chantier 1) : clips glTF, échantillonnage, cross-fades, graphe, anti-foot-sliding (referenceSpeed — **pas de root motion**, décidé 2026-07-05). Reste : couches haut/bas du corps, masques par os, additive P1 (chantier « vivant », le combat en a besoin). |
| ✅ **Contrôleur d'anim en données** | P0 | v1 FAITE (H1/H5 + chantier 1) : `AnimGraphForm` + états/transitions enfants, params (vitesse) + gates par tags, moddable (prouvé : seuils retunables en TOML). Reste : brancher le **condition evaluator** complet sur les transitions. |
| 🔨 **Événements sur timeline** | P0 | `AnimEventForm` + tir au bon temps FAITS (H5, doctesté) et posés sur le cycle de marche (chantier 1). Reste : les CONSOMMATEURS — fenêtres de dégâts GAS, footsteps audio par matériau, FX, shake (chantier « vivant »). |
| **Éditeur d'animation / timeline** | P1 | Outil ImGui : preview du personnage, scrub, pose des événements, réglage des transitions/blend times, sauvegarde en Form. (P0 minimal : édition TOML à la main + hot-reload.) |
| 🔨 **Apparence modulaire des personnages** | P0 | `AppearanceForm` (slots+teintes) + `resolveActorVisual` FAITS (H1 + chantier 1) — mais v1 mono-mesh (premier slot rempli). Reste : compositing multi-slots sur un squelette partagé + équipement→visuel (échange du mesh du slot). |
| **Attach points** | P0 | Sockets sur os (main droite = arme, dos = fourreau, selle P2). Armes visibles rangées/dégainées. Chantier « vivant » (combat). |
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
| 🔨 **Intégration Jolt** | P0 | Monde + capsule + raycasts + height fields + **mesh colliders des statics/kits** (`addStaticMesh`, chantier 2 B2 — suivent le spawn/despawn des cellules) FAITS. Reste : shape casts combat, triggers sensors (chantier « vivant »). |
| ✅ **Character controller** | P0 | v1 FAITE (chantier 1) : capsule cinématique, pentes/marches, saut, sprint payé en énergie GAS, vitesses par stats dérivées. Reste : dégâts de chute, nage P1. |
| **Objets dynamiques** | P1 | Props poussables, loot qui tombe, physique légère (le « clutter » Skyrim). Havok-cheese P2. |
| ✅ **Portes & mécanismes** | P0 | v1 FAITE (chantier 2 B7) : `DoorForm` (cible = référence de marker, résolution 100 % records), prompt [E], transition worldspace avec fondu. Reste : portes ANIMÉES (battant), verrous/crochetage P1, leviers/coffres, états persistés (chantier 5). |

## E. IA & navigation

| Fonctionnalité | Prio | Notes |
|---|---|---|
| 🔨 **Navmesh (Recast/Detour)** | P0 | Interface `nav::Navigator` + `GridNavigator` (A* 2D) posées (H7). Reste : Recast/Detour réels — génération offline par cellule (cooker) + liens, requêtes async, évitement local. Chantier « vivant ». |
| 🔨 **Emploi du temps des PNJ** | P0 | `ScheduleForm`/entrées enfants + `evaluateSchedule` FAITS (H1/H7, doctests : fenêtres, minuit, override par mod, conditions). Reste : l'EXÉCUTION sur le monde vivant (ScheduleAgent, interruptions/reprise) — chantier « vivant » ; simulation dégradée hors cellule P2. |
| 🔨 **Packages IA (exécution)** | P0 | `AiPackageForm` posé (H1) ; la patrouille du chantier 1 est l'embryon d'exécution. Reste : les comportements réels (dormir/manger/travailler/errer/voyager/mobilier) pilotés par le schedule — chantier « vivant ». |
| 🔨 **Outil emploi du temps** | P0/P1 | P0 (vue debug) : posée dans l'EditorScene (drawSchedules + slider d'heure, H7) ; à brancher sur le monde vivant. P1 = éditeur visuel timeline, sortie = plugin. |
| **IA de combat 3D** | P0 | Port du chase/attack 2D : distances d'engagement, strafe, usage d'abilities GAS (coûts/cooldowns existants), fuite (courage/santé), appel à l'aide (factions existantes). Chantier « vivant ». |
| **Perception 3D** | P0 | Vue (cône + raycast d'occlusion — la furtivité P1 en dépend), ouïe (événements sonores gameplay), mémoire de dernière position connue. Chantier « vivant ». |
| **Foules/planification** | P2 | Budget d'update IA par frame, LOD IA (PNJ hors cellule simulés grossièrement — « offscreen simulation » Skyrim). |

## F. Gameplay (au-dessus du GAS existant)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| **Combat 3D** | P0 | Traces d'armes (shape cast sur fenêtres de hit des anims), directions de blocage, projectiles (flèches/sorts — gravité simple), lock-on optionnel, staggers/posture déjà en données. Tout passe par les GameplayEffects existants. Chantier « vivant ». |
| 🔨 **GameplayCues (pont effets→présentation)** | P0 | Registre + `CueTable` (fallback hiérarchique) + `CueForm` + preuve (hit → étincelles CombatArena) FAITS (H7). Reste : les handlers standard (particule/son/shake résolus par CueForm) et les points d'émission systématiques — chantier « vivant ». |
| **Interaction** | P0 | Raycast d'interaction + prompt contextuel (Prendre/Parler/Ouvrir/Dormir), activation via le système existant. Chantier « vivant ». |
| 🔨 **Mobilier & postes de travail** | P0 | `FurnitureForm` + points d'usage enfants + `FurnitureOccupancy` (claim/release/file) + spawner FAITS (H1/H7/H8, doctests). Reste : le FLUX complet (nav vers le point → anims entrée/boucle/sortie → effet GAS pendant l'usage) — chantier « vivant ». |
| **Économie/marchands 3D** | P1 | Port du barter 2D ; inventaires de marchands régénérés (game clock), richesse limitée. |
| **Crime & prime** | P1 | Témoins (perception), bounty par faction (relations existantes), gardes qui arrêtent, prison/amende. |
| **Furtivité** | P1 | Détection (lumière P2, bruit, ligne de vue), état Sneak, dégâts sournois — la Phase 9 (stats) la prévoit. |
| **Repos/attente 3D** | P0 | Lits/chaises via furniture markers, menu d'attente (rest existant Phase 7). |
| **Progression** | P1 | **Décidé (2026-07-05) : skills-by-use** — chaque usage d'ability/effet émet un événement d'usage → XP de compétence → seuils = GameplayEffects passifs. Cohérent avec la courbe d'érudition (STATS.md) ; perks = effets passifs (déjà le modèle). |

## G. UI (jeu) + éditeur d'UI

| Fonctionnalité | Prio | Notes |
|---|---|---|
| 🔨 **Moteur UI : RmlUi (DÉCIDÉ 2026-07-05)** | P0 | Seam FAIT (H4) : rendu sur RHI, **overlay par chemin sur les `ui/` des plugins** (un mod override un écran — prouvé), scène démo. Reste : pile d'écrans (`showScreen`), data binding vers l'état de jeu, clavier/gamepad — chantier « interfaces ». |
| 🔨 **Rendu texte** | P0 | Couvert par RmlUi+FreeType (H4) pour les écrans. Reste : texte monde léger (nameplates) — overlay screen-space ou quads. UTF-8 partout (localisation-ready). |
| **Écrans de jeu** | P0→P1 | P0 : HUD (santé/energie/essence/posture — stats existantes, boussole, prompts, crosshair), inventaire/équipement, dialogue, conteneur/loot, menu principal/pause, journal de quêtes. P1 : carte (monde+locale+marqueurs), barter, feuille de personnage, level-up, options complètes, écrans de chargement avec lore. |
| **Navigation gamepad** | P1 | Focus/navigation directionnelle dans tous les écrans. |
| **UI monde** | P0 | Nameplates/barres de vie ennemis, prompts flottants, textes de dégâts P2. |
| **Éditeur d'UI** | P1 | Édition de layout en jeu (ancres, conteneurs, styles), sauvegarde en données moddables. (P0 : layouts en TOML édités à la main + hot-reload.) |
| 🔨 **Localisation** | P1 | `LocStringForm` posé (H1 — un pack de langue = un plugin qui patche `text`, doctesté). Reste : la discipline de clés dans dialogues/quêtes/UI et le lookup runtime. |

## H. FX

| Fonctionnalité | Prio | Notes |
|---|---|---|
| 🔨 **Système de particules** | P0 | `ParticleForm` + `fx::ParticleSim` (bursts déterministes, extraction sprites, preuve étincelles 2D) FAITS (H1/H7). Reste : émetteurs continus, formes, courbes, rendu 3D en quads caméra, soft particles, budget — chantier « vivant ». |
| **Éditeur de FX** | P1 | Panneau live : édition des params d'émetteur avec preview immédiate, save en Form. (P0 : TOML + hot-reload — le pattern tuning existant.) |
| **Trails & beams** | P1 | Traînées d'armes (sockets d'anim), rayons de sorts. |
| **FX de matériaux** | P1 | Dissolve, freeze, burn — anim de params de MaterialForm via cues. |
| **Screen FX** | P1 | Vignette de dégâts, overlays d'états (poison/ivresse — statuts Phase 7), hit flash — hooks dans le tonemap existant. |
| **Bibliothèque de base** | P0 | Le stock minimal : hit sparks, sang, soin, feu/torches, fumée, poussière de pas, magie par école. Chaque entrée = Form + cue. Chantier « vivant ». |

## I. Audio (miniaudio)

| Fonctionnalité | Prio | Notes |
|---|---|---|
| 🔨 **Moteur son** | P0 | Seam FAIT (H6) : bus, play 2D/3D, crossfade musique, backend null pour les tests. Reste : le résolveur `SoundForm` (variantes par poids, jitters, chemin AssetDatabase) — chantier « vivant ». |
| **Ambiances** | P0 | Beds par région/cellule/météo/heure (la météo pilote déjà tout le visuel — brancher l'audio dessus), transitions fondues (le crossfade météo existe). Chantier « vivant ». |
| **Musique dynamique** | P1 | Couches explore/combat/danger avec transitions (tags GAS `State.InCombat` déjà là). |
| **Footsteps par matériau** | P1 | Événements d'anim × matériau du sol (splat weights/type de sol connus). |
| **Reverb par volume** | P1 | Zones de reverb (intérieurs/grottes). |
| **Voix** | P2 | Hooks de playback par ligne de dialogue (subtitle-first pour la démo). |

## J. Données, plugins & outils transverses

| Fonctionnalité | Prio | Notes |
|---|---|---|
| ✅ **GameDB browser (l'outil central)** | P0 | v1 FAITE (H2, scène « Game DB ») : navigation par type, recherche editorId, édition par PropertyGrid réflexion, création, **export = plugin**. Reste : cross-références (« utilisé par »), duplication, et son extension en éditeur de niveau (chantier 2). |
| ✅ **Gestionnaire de plugins interne** | P0 | v1 FAITE (H2, PluginsPanel) : load order (plugins.toml), enable/disable, **rapport de conflits par champ**. Reste : dépendances affichées, l'outil de synthesis §5.1 (chantier « interfaces »). |
| ✅ **Console développeur** | P0 | v1 FAITE (H2) : get/set par réflexion (`EditorId.field`), find, undo/redo, REPL Lua. Reste : `spawn`, `tp`, `tgm`, `setstage` (au fil des chantiers qui apportent les systèmes visés). |
| 🔨 **Pipeline d'assets** | P0 | Import glTF statique+skinné+anims FAIT (brique 23 + chantier 1) ; cooker binaire couvre tous les Forms (audit) ; hot-reload textures/shaders. Reste : KTX2/Basis, **asset browser** avec previews, hot-reload meshes/anims. |
| **Éditeur de quêtes/dialogues** | P1 | Vue graphe des stages/branches de dialogue sur les records existants (Phase 4) ; P0 = TOML à la main (déjà le cas). |
| **Profiler** | P1 | Zones CPU + timers GPU par passe, compteurs (draws/instances/chunks — partiels), graphe de frame ; P0 minimal = timers par système dans le panneau. |
| **Validation de mods** | P1 | `tools/` : lint d'un plugin (GUIDs, champs inconnus, refs cassées), déjà prévu au CLAUDE.md. |
| **Docs moddeur générées** | P2 | Générer la référence des champs depuis la réflexion (le MODDING-EFFECTS.md à la main ne scalera pas). |
| ✅ **Doc utilisateur/moddeur `userdoc/`** | P0 (à maintenir) | FAITE 2026-07-05 : hub + pages thématiques liées. **Chaque vertical livré met à jour sa page** (chantier 1 → world-and-levels : props 3D + personnages). |
| **Index secondaires FormDatabase** | P1 | Décision 2026-07-05 : PAS de base SQL au runtime — la FormDatabase résolue EST la base (lookups GUID O(1)). La scalabilité vient de : (a) le format binaire cuit (Phase 1) pour le temps de chargement quand les fichiers se multiplient, (b) des index en mémoire construits après resolve (par type, par `parent`) pour remplacer les scans de `forEach`/`childrenOf` quand le volume le justifiera. SQL n'apporterait que de la friction (dep, mismatch avec la réflexion/le layering §5, lookups plus lents qu'une hashmap). |
| **Crash handling** | P2 | Minidumps + log de la pile de plugins active. |

## K. Sauvegarde & streaming (rappel ex-Phase 10) — à faire, chantier 5

Save = couche de patches runtime (§5, non négociable) : état des références
(positions, morts, inventaires), stages de quêtes, journal, temps/météo,
état du joueur. Le streaming 3D (A) et la save sont le MÊME chantier
(« persistance ») — inchangé sur le fond, re-priorisé P0.

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
   (ex-Phase 12 chemin custom).
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
   `docs/3D-RENDERER.md`).

> NB : `docs/3D-RENDERER.md` reste le journal + la spec détaillée des
> briques renderer ; leur *planification* vit désormais ici (27→chantier 1,
> lumières intérieures→chantier 2, 28-31→chantier 6). La validation Godot
> (ex-Phase 8.5) reste une option post-démo
> (`docs/SIMULATION-AND-PRESENTATION.md`).